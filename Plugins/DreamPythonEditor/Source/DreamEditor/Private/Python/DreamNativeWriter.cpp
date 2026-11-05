// SPDX-License-Identifier: Apache-2.0
// _dream.Writer and _dream.BinaryWriter (TDD §6.1 "Design: _dream.Writer", §12.1.1): the text streams that replace
// sys.stdout and sys.stderr for a run. Any thread, GIL held, never blocks: each write is one MPSC push (GIL-2).
#include "Python/DreamNativeInternal.h"

#include "Output/DreamOutputTypes.h"
#include "Python/DreamPyUtil.h"

#if WITH_PYTHON

namespace DreamNativeWriterPrivate
{
	struct FWriterObject
	{
		PyObject_HEAD
		int32 Stream;
		FDreamRunId RunId;
		PyObject* Buffer;           // the BinaryWriter, strong
	};

	struct FBinaryWriterObject
	{
		PyObject_HEAD
		int32 Stream;
		FDreamRunId RunId;
	};

	thread_local TArray<UTF8CHAR> GEncodeScratch;

	PyObject* RaiseUnsupported(const char* What)
	{
		PyObject* Type = DreamNativeInternal::State().UnsupportedOperation.Get();
		PyErr_SetString(Type ? Type : PyExc_OSError, What);
		return nullptr;
	}

	bool ParseStreamAndRunId(PyObject* Args, PyObject* Kwds, const char* TypeName, int32& OutStream, FDreamRunId& OutId)
	{
		if (Kwds != nullptr && PyDict_GET_SIZE(Kwds) != 0)
		{
			PyErr_Format(PyExc_TypeError, "%s() takes no keyword arguments", TypeName);
			return false;
		}
		if (!PyTuple_Check(Args) || PyTuple_GET_SIZE(Args) != 2)
		{
			PyErr_Format(PyExc_TypeError, "%s(stream: int, run_id: int) takes exactly 2 arguments", TypeName);
			return false;
		}
		return DreamNativeInternal::ParseStream(PyTuple_GET_ITEM(Args, 0), OutStream, /*bWriterStreamsOnly*/ true)
			&& DreamNativeInternal::ParseWriteRunId(PyTuple_GET_ITEM(Args, 1), OutId);
	}

	// ---- BinaryWriter ------------------------------------------------------------------------------------------

	PyObject* BinaryWriter_New(PyTypeObject* Type, PyObject* Args, PyObject* Kwds)
	{
		int32 Stream = 0;
		FDreamRunId RunId = 0;
		if (!ParseStreamAndRunId(Args, Kwds, "BinaryWriter", Stream, RunId))
		{
			return nullptr;
		}
		FBinaryWriterObject* Self = reinterpret_cast<FBinaryWriterObject*>(Type->tp_alloc(Type, 0));
		if (Self == nullptr)
		{
			return nullptr;
		}
		Self->Stream = Stream;
		Self->RunId = RunId;
		return reinterpret_cast<PyObject*>(Self);
	}

	void BinaryWriter_Dealloc(PyObject* Self)
	{
		PyTypeObject* Type = Py_TYPE(Self);
		Type->tp_free(Self);
		Py_DECREF(Type);            // heap type instances own a reference to their type
	}

	PyObject* BinaryWriter_Write(PyObject* SelfObject, PyObject* Data)
	{
		FBinaryWriterObject* Self = reinterpret_cast<FBinaryWriterObject*>(SelfObject);
		Py_buffer View;
		if (PyObject_GetBuffer(Data, &View, PyBUF_SIMPLE) != 0)
		{
			return nullptr;
		}
		const Py_ssize_t Length = View.len;
		// §6.1: decode UTF-8 with replacement, then the Writer.write path.
		PyObject* Text = PyUnicode_DecodeUTF8(static_cast<const char*>(View.buf), View.len, "replace");
		PyBuffer_Release(&View);
		if (Text == nullptr)
		{
			return nullptr;
		}
		const UTF8CHAR* Utf8 = nullptr;
		int32 Bytes = 0;
		if (DreamPy::EncodeUtf8Replacing(Text, GEncodeScratch, Utf8, Bytes) && Bytes > 0)
		{
			DreamNativeInternal::PushText(Self->Stream, Self->RunId, Utf8, Bytes);
		}
		Py_DECREF(Text);
		return PyLong_FromSsize_t(Length);
	}

	PyObject* Common_ReturnNone(PyObject*, PyObject*)
	{
		return DreamPy::NewNone();
	}
	PyObject* Common_ReturnFalse(PyObject*, PyObject*)
	{
		return DreamPy::NewBool(false);
	}
	PyObject* Common_ReturnTrue(PyObject*, PyObject*)
	{
		return DreamPy::NewBool(true);
	}
	PyObject* Common_Fileno(PyObject*, PyObject*)
	{
		return RaiseUnsupported("fileno");
	}
	PyObject* Common_Close(PyObject*, PyObject*)
	{
		return RaiseUnsupported("close");
	}
	PyObject* Common_Detach(PyObject*, PyObject*)
	{
		return RaiseUnsupported("detach");
	}

	PyMethodDef BinaryWriterMethods[] = {
		{ "write", BinaryWriter_Write, METH_O, "write(b) -> int: decode UTF-8 with replacement and write it as text." },
		{ "flush", Common_ReturnNone, METH_NOARGS, "flush() -> None: no I/O." },
		{ "writable", Common_ReturnTrue, METH_NOARGS, nullptr },
		{ "readable", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "seekable", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "isatty", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "fileno", Common_Fileno, METH_NOARGS, nullptr },
		{ "close", Common_Close, METH_NOARGS, nullptr },
		{ nullptr, nullptr, 0, nullptr },
	};

	PyType_Slot BinaryWriterSlots[] = {
		{ Py_tp_new, reinterpret_cast<void*>(&BinaryWriter_New) },
		{ Py_tp_dealloc, reinterpret_cast<void*>(&BinaryWriter_Dealloc) },
		{ Py_tp_methods, BinaryWriterMethods },
		{ Py_tp_doc, const_cast<char*>("BinaryWriter(stream, run_id): the buffer of a Dream Python Editor Writer (TDD 6.1).") },
		{ 0, nullptr },
	};

	PyType_Spec BinaryWriterSpec = {
		"_dream.BinaryWriter",
		static_cast<int>(sizeof(FBinaryWriterObject)),
		0,
		Py_TPFLAGS_DEFAULT,
		BinaryWriterSlots,
	};

	// ---- Writer ------------------------------------------------------------------------------------------------

	PyObject* Writer_New(PyTypeObject* Type, PyObject* Args, PyObject* Kwds)
	{
		int32 Stream = 0;
		FDreamRunId RunId = 0;
		if (!ParseStreamAndRunId(Args, Kwds, "Writer", Stream, RunId))
		{
			return nullptr;
		}
		PyObject* BinaryType = DreamNativeInternal::State().BinaryWriterType.Get();
		if (BinaryType == nullptr)
		{
			PyErr_SetString(PyExc_RuntimeError, "_dream.Writer: the native module is shut down");
			return nullptr;
		}
		PyObject* Buffer = PyObject_Call(BinaryType, Args, nullptr);
		if (Buffer == nullptr)
		{
			return nullptr;
		}
		FWriterObject* Self = reinterpret_cast<FWriterObject*>(Type->tp_alloc(Type, 0));
		if (Self == nullptr)
		{
			Py_DECREF(Buffer);
			return nullptr;
		}
		Self->Stream = Stream;
		Self->RunId = RunId;
		Self->Buffer = Buffer;
		return reinterpret_cast<PyObject*>(Self);
	}

	void Writer_Dealloc(PyObject* SelfObject)
	{
		FWriterObject* Self = reinterpret_cast<FWriterObject*>(SelfObject);
		Py_CLEAR(Self->Buffer);
		PyTypeObject* Type = Py_TYPE(SelfObject);
		Type->tp_free(SelfObject);
		Py_DECREF(Type);
	}

	PyObject* Writer_Write(PyObject* SelfObject, PyObject* Text)
	{
		FWriterObject* Self = reinterpret_cast<FWriterObject*>(SelfObject);
		if (!PyUnicode_Check(Text))
		{
			// The same text as TextIOWrapper.write (§6.1).
			PyErr_Format(PyExc_TypeError, "write() argument must be str, not %.100s", Py_TYPE(Text)->tp_name);
			return nullptr;
		}
		const UTF8CHAR* Utf8 = nullptr;
		int32 Bytes = 0;
		if (DreamPy::EncodeUtf8Replacing(Text, GEncodeScratch, Utf8, Bytes) && Bytes > 0)
		{
			DreamNativeInternal::PushText(Self->Stream, Self->RunId, Utf8, Bytes);
		}
		return PyLong_FromSsize_t(PyUnicode_GET_LENGTH(Text));
	}

	PyObject* Writer_GetBuffer(PyObject* SelfObject, void*)
	{
		FWriterObject* Self = reinterpret_cast<FWriterObject*>(SelfObject);
		Py_INCREF(Self->Buffer);
		return Self->Buffer;
	}
	PyObject* Writer_GetEncoding(PyObject*, void*)
	{
		return PyUnicode_FromString("utf-8");
	}
	PyObject* Writer_GetErrors(PyObject*, void*)
	{
		return PyUnicode_FromString("replace");
	}
	PyObject* Writer_GetName(PyObject* SelfObject, void*)
	{
		const FWriterObject* Self = reinterpret_cast<const FWriterObject*>(SelfObject);
		return PyUnicode_FromString(Self->Stream == static_cast<int32>(EDreamStream::Stdout) ? "<dream stdout>" : "<dream stderr>");
	}
	PyObject* Writer_GetTrue(PyObject*, void*)
	{
		return DreamPy::NewBool(true);
	}
	PyObject* Writer_GetFalse(PyObject*, void*)
	{
		return DreamPy::NewBool(false);
	}

	PyMethodDef WriterMethods[] = {
		{ "write", Writer_Write, METH_O, "write(s: str) -> int: one output record for the run (no I/O, never blocks)." },
		{ "flush", Common_ReturnNone, METH_NOARGS, "flush() -> None: no I/O; partial lines stay open." },
		{ "isatty", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "readable", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "seekable", Common_ReturnFalse, METH_NOARGS, nullptr },
		{ "writable", Common_ReturnTrue, METH_NOARGS, nullptr },
		{ "fileno", Common_Fileno, METH_NOARGS, nullptr },
		{ "close", Common_Close, METH_NOARGS, nullptr },
		{ "detach", Common_Detach, METH_NOARGS, nullptr },
		{ nullptr, nullptr, 0, nullptr },
	};

	PyGetSetDef WriterGetSet[] = {
		{ "buffer", Writer_GetBuffer, nullptr, nullptr, nullptr },
		{ "encoding", Writer_GetEncoding, nullptr, nullptr, nullptr },
		{ "errors", Writer_GetErrors, nullptr, nullptr, nullptr },
		{ "name", Writer_GetName, nullptr, nullptr, nullptr },
		{ "line_buffering", Writer_GetTrue, nullptr, nullptr, nullptr },
		{ "closed", Writer_GetFalse, nullptr, nullptr, nullptr },
		{ nullptr, nullptr, nullptr, nullptr, nullptr },
	};

	PyType_Slot WriterSlots[] = {
		{ Py_tp_new, reinterpret_cast<void*>(&Writer_New) },
		{ Py_tp_dealloc, reinterpret_cast<void*>(&Writer_Dealloc) },
		{ Py_tp_methods, WriterMethods },
		{ Py_tp_getset, WriterGetSet },
		{ Py_tp_doc, const_cast<char*>("Writer(stream, run_id): the text stream that replaces sys.stdout or sys.stderr for a Dream run (TDD 6.1).") },
		{ 0, nullptr },
	};

	PyType_Spec WriterSpec = {
		"_dream.Writer",
		static_cast<int>(sizeof(FWriterObject)),
		0,
		Py_TPFLAGS_DEFAULT,
		WriterSlots,
	};
}

namespace DreamNativeInternal
{
	bool AddWriterTypes(PyObject* Module)
	{
		using namespace DreamNativeWriterPrivate;
		FState& S = State();

		PyObject* BinaryType = PyType_FromSpec(&BinaryWriterSpec);
		if (BinaryType == nullptr)
		{
			return false;
		}
		S.BinaryWriterType = FDreamPyRef::Borrow(BinaryType);
		if (PyModule_AddObject(Module, "BinaryWriter", BinaryType) != 0)
		{
			Py_DECREF(BinaryType);
			return false;
		}

		PyObject* WriterType = PyType_FromSpec(&WriterSpec);
		if (WriterType == nullptr)
		{
			return false;
		}
		S.WriterType = FDreamPyRef::Borrow(WriterType);
		if (PyModule_AddObject(Module, "Writer", WriterType) != 0)
		{
			Py_DECREF(WriterType);
			return false;
		}
		return true;
	}
}

#endif // WITH_PYTHON
