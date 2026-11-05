// SPDX-License-Identifier: Apache-2.0
// Forward declarations of the CPython types Dream's headers mention, so a header outside the §2.5 rule 6 folders can
// name them without including Python.h. The tags match CPython 3.11 (object.h: struct _object; pystate.h: struct _ts).
// (slice A choice) Added so RunSupervisor.h and DreamPython.h stay Python.h-free.
#pragma once

struct _object;
struct _ts;

using FDreamPyObject = struct _object;       // == PyObject
using FDreamPyThreadState = struct _ts;      // == PyThreadState
