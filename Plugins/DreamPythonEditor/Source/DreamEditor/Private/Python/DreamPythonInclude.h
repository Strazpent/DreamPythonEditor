// SPDX-License-Identifier: Apache-2.0
// The only way Dream includes CPython (TDD §2.5 rule 6, D7, D15). Include it only from Private/Python,
// Private/Run/RunSupervisor*.cpp and Private/Compat. Every other header uses the forward declarations below.
//
// Written from the behaviour of the engine's own wrapper (PythonScriptPlugin Private/IncludePython.h, 5.8): Python.h
// inside THIRD_PARTY_INCLUDES_START/END with the register-keyword warnings disabled, all under WITH_PYTHON (defined by
// the Python3 module, Engine/Source/ThirdParty/Python3/Python3.Build.cs:82). The engine's wrapper undefines no macros,
// and neither does this one: Python 3.11's pyport.h redefines the ctype functions only for C (not C++), and pyconfig.h's
// _DEBUG branch (Py_DEBUG, python311_d.lib) never applies because UE builds never define _DEBUG.
// (slice A choice) structmember.h is not included: _dream declares its members with PyGetSetDef / PyMethodDef only.
#pragma once

#include "CoreTypes.h"

#if WITH_PYTHON

#if defined(Py_LIMITED_API)
#error "Dream Python Editor links python311.lib only; the limited API (python3.lib) is not shipped (TDD §1.3 C7)."
#endif

THIRD_PARTY_INCLUDES_START
PRAGMA_DISABLE_REGISTER_WARNINGS
#include "Python.h"
PRAGMA_ENABLE_REGISTER_WARNINGS
THIRD_PARTY_INCLUDES_END

#endif // WITH_PYTHON
