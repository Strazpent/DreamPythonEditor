// SPDX-License-Identifier: Apache-2.0
// Bridge-internal CPython helpers (see DreamPyUtil.h). GIL held throughout.
#include "Python/DreamPyUtil.h"

#if WITH_PYTHON

static_assert(sizeof(TCHAR) == 2, "DreamPy converts through UTF-16; TCHAR is UTF-16 on every UE 5.8 platform (TDD §4.1.2).");

namespace DreamPy
{
	PyObject* FromString(FStringView Text)
	{
		int ByteOrder = -1;     // little endian, no BOM
		return PyUnicode_DecodeUTF16(reinterpret_cast<const char*>(Text.GetData()),
			static_cast<Py_ssize_t>(Text.Len()) * 2, "replace", &ByteOrder);
	}

	bool ToString(PyObject* Object, FString& Out)
	{
		if (Object == nullptr || !PyUnicode_Check(Object))
		{
			PyErr_Format(PyExc_TypeError, "expected str, not %.200s", Object ? Py_TYPE(Object)->tp_name : "NULL");
			return false;
		}
		PyObject* Bytes = PyUnicode_AsEncodedString(Object, "utf-16-le", "surrogatepass");
		if (Bytes == nullptr)
		{
			return false;
		}
		const Py_ssize_t Size = PyBytes_GET_SIZE(Bytes);
		const TCHAR* Data = reinterpret_cast<const TCHAR*>(PyBytes_AS_STRING(Bytes));
		Out = FString::ConstructFromPtrSize(Data, static_cast<int32>(Size / 2));
		Py_DECREF(Bytes);
		return true;
	}

	FString ToStringLossy(PyObject* Object)
	{
		if (Object == nullptr)
		{
			return FString();
		}
		PyObject* Text = PyObject_Str(Object);
		FString Result;
		if (Text == nullptr || !ToString(Text, Result))
		{
			PyErr_Clear();
			Result = FString::Printf(TEXT("<%hs>"), Py_TYPE(Object)->tp_name);
		}
		Py_XDECREF(Text);
		return Result;
	}

	namespace Utf8Private
	{
		void AppendUtf8(TArray<UTF8CHAR>& Out, uint32 CodePoint)
		{
			if (CodePoint < 0x80)
			{
				Out.Add(static_cast<UTF8CHAR>(CodePoint));
			}
			else if (CodePoint < 0x800)
			{
				Out.Add(static_cast<UTF8CHAR>(0xC0 | (CodePoint >> 6)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | (CodePoint & 0x3F)));
			}
			else if (CodePoint < 0x10000)
			{
				Out.Add(static_cast<UTF8CHAR>(0xE0 | (CodePoint >> 12)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | ((CodePoint >> 6) & 0x3F)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | (CodePoint & 0x3F)));
			}
			else
			{
				Out.Add(static_cast<UTF8CHAR>(0xF0 | (CodePoint >> 18)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | ((CodePoint >> 12) & 0x3F)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | ((CodePoint >> 6) & 0x3F)));
				Out.Add(static_cast<UTF8CHAR>(0x80 | (CodePoint & 0x3F)));
			}
		}
	}

	bool EncodeUtf8Replacing(PyObject* Text, TArray<UTF8CHAR>& Scratch, const UTF8CHAR*& OutData, int32& OutBytes)
	{
		OutData = nullptr;
		OutBytes = 0;
		if (Text == nullptr || !PyUnicode_Check(Text))
		{
			return false;
		}
		Py_ssize_t Size = 0;
		const char* Utf8 = PyUnicode_AsUTF8AndSize(Text, &Size);
		if (Utf8 != nullptr)
		{
			OutData = reinterpret_cast<const UTF8CHAR*>(Utf8);
			OutBytes = static_cast<int32>(Size);
			return true;
		}
		// A lone surrogate makes the strict encoder fail; rebuild with U+FFFD in its place (§6.1).
		PyErr_Clear();
		const Py_ssize_t Length = PyUnicode_GET_LENGTH(Text);
		const int Kind = PyUnicode_KIND(Text);
		const void* Data = PyUnicode_DATA(Text);
		Scratch.Reset(static_cast<int32>(Length) * 3);
		for (Py_ssize_t Index = 0; Index < Length; ++Index)
		{
			uint32 CodePoint = PyUnicode_READ(Kind, Data, Index);
			if (CodePoint >= 0xD800 && CodePoint <= 0xDFFF)
			{
				CodePoint = 0xFFFD;
			}
			Utf8Private::AppendUtf8(Scratch, CodePoint);
		}
		OutData = Scratch.GetData();
		OutBytes = Scratch.Num();
		return true;
	}

	FString FetchExceptionText()
	{
		if (!PyErr_Occurred())
		{
			return FString();
		}
		PyObject* Type = nullptr;
		PyObject* Value = nullptr;
		PyObject* Traceback = nullptr;
		PyErr_Fetch(&Type, &Value, &Traceback);
		PyErr_NormalizeException(&Type, &Value, &Traceback);
		if (Value != nullptr && Traceback != nullptr)
		{
			PyException_SetTraceback(Value, Traceback);
		}

		FString Result;
		if (PyObject* TracebackModule = PyImport_ImportModule("traceback"))
		{
			PyObject* Lines = PyObject_CallMethod(TracebackModule, "format_exception", "O", Value ? Value : Py_None);
			if (Lines != nullptr && PyList_Check(Lines))
			{
				for (Py_ssize_t Index = 0; Index < PyList_GET_SIZE(Lines); ++Index)
				{
					FString Line;
					if (ToString(PyList_GET_ITEM(Lines, Index), Line))
					{
						Result += Line;
					}
					else
					{
						PyErr_Clear();
					}
				}
			}
			Py_XDECREF(Lines);
			Py_DECREF(TracebackModule);
		}
		PyErr_Clear();
		if (Result.IsEmpty())
		{
			const char* TypeName = Type && PyType_Check(Type) ? reinterpret_cast<PyTypeObject*>(Type)->tp_name : "Exception";
			Result = FString::Printf(TEXT("%hs: %s"), TypeName, *ToStringLossy(Value));
		}
		Result.TrimEndInline();

		Py_XDECREF(Type);
		Py_XDECREF(Value);
		Py_XDECREF(Traceback);
		return Result;
	}

	bool CheckArgCount(const char* Function, Py_ssize_t NArgs, Py_ssize_t Expected)
	{
		if (NArgs == Expected)
		{
			return true;
		}
		PyErr_Format(PyExc_TypeError, "%.100s() takes exactly %zd argument%s (%zd given)", Function, Expected,
			Expected == 1 ? "" : "s", NArgs);
		return false;
	}
}

#endif // WITH_PYTHON
