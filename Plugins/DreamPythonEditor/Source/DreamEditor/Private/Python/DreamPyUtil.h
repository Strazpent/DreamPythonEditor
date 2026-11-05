// SPDX-License-Identifier: Apache-2.0
// Bridge-internal CPython helpers: FString <-> str conversions, exception text and argument checks shared by the
// _dream module, the Writer types and FDreamPython's helper calls. GIL held for every function. Include only from
// Private/Python (§2.5 rule 6). Bodies: DreamPyUtil.cpp.
#pragma once

#include "CoreMinimal.h"
#include "Python/DreamPythonInclude.h"

#if WITH_PYTHON

namespace DreamPy
{
	/** A new str from UTF-16 text; a lone surrogate becomes U+FFFD so compile and print never fail on it. */
	PyObject* FromString(FStringView Text);
	inline PyObject* FromString(const FString& Text) { return FromString(FStringView(Text)); }
	inline PyObject* FromString(const TCHAR* Text) { return FromString(FStringView(Text)); }

	/** str to FString (lone surrogates pass through as UTF-16 units). False with a Python error set for a non-str. */
	bool ToString(PyObject* Object, FString& Out);
	/** str(object) to FString; on failure clears the error and returns the type name in angle brackets. */
	FString ToStringLossy(PyObject* Object);

	/**
	 * UTF-8 of a str with every lone surrogate replaced by U+FFFD (§6.1 Writer.write). Fast path: CPython's cached UTF-8.
	 * Returns false only for a non-str (no error set).
	 */
	bool EncodeUtf8Replacing(PyObject* Text, TArray<UTF8CHAR>& Scratch, const UTF8CHAR*& OutData, int32& OutBytes);

	/** The pending exception as "Type: message" plus the formatted traceback when traceback is importable; clears it. */
	FString FetchExceptionText();

	/** An int that is not a bool (the _dream argument rule: bool -> TypeError, as native.py). */
	inline bool IsStrictInt(PyObject* Object)
	{
		return Object != nullptr && PyLong_Check(Object) && !PyBool_Check(Object);
	}

	/** Raises TypeError "<Function>() takes exactly N arguments (M given)" when NArgs != Expected. */
	bool CheckArgCount(const char* Function, Py_ssize_t NArgs, Py_ssize_t Expected);

	/** Py_RETURN_NONE as an expression. */
	inline PyObject* NewNone()
	{
		Py_INCREF(Py_None);
		return Py_None;
	}
	inline PyObject* NewBool(bool bValue)
	{
		return PyBool_FromLong(bValue ? 1 : 0);
	}
}

/** The PyCFunction cast of a METH_FASTCALL or METH_NOARGS function, as CPython's own headers require. */
#define DREAM_PY_CFUNCTION(Function) reinterpret_cast<PyCFunction>(reinterpret_cast<void*>(Function))

#endif // WITH_PYTHON
