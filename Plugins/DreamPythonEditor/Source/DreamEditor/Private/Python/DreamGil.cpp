// SPDX-License-Identifier: Apache-2.0
// FDreamPyRef: the generation-stamped strong reference C++ holds (TDD §3.8, GIL-7). The registry counts live refs of
// the current generation in FDreamPython; HandleShutdown asserts it is empty.
#include "Python/DreamGil.h"

#include "Python/DreamPython.h"

#if WITH_PYTHON

namespace DreamGilPrivate
{
	/** Drops a reference: decref with the GIL, queue it without, leak it when its interpreter is gone (§3.8 step 5). */
	void DropReference(PyObject* Object, uint32 Generation)
	{
		if (Object == nullptr)
		{
			return;
		}
		if (Generation == 0 || Generation != FDreamPython::GetAliveGeneration())
		{
			ensureMsgf(false, TEXT("Dream: a Python reference of generation %u outlived its interpreter; it is leaked, not released."), Generation);
			return;
		}
		if (PyGILState_Check())
		{
			Py_DECREF(Object);
		}
		else
		{
			FDreamPython::QueueDecref(Object, Generation);
		}
	}
}

FDreamPyRef FDreamPyRef::Steal(PyObject* NewRef)
{
	FDreamPyRef Ref;
	if (NewRef != nullptr)
	{
		Ref.Object = NewRef;
		Ref.Generation = FDreamPython::GetAliveGeneration();
		FDreamPython::RegisterRef(Ref.Generation);
	}
	return Ref;
}

FDreamPyRef FDreamPyRef::Borrow(PyObject* BorrowedRef)
{
	Py_XINCREF(BorrowedRef);
	return Steal(BorrowedRef);
}

FDreamPyRef::FDreamPyRef(const FDreamPyRef& Other)
	: Object(Other.Object)
	, Generation(Other.Generation)
{
	if (Object != nullptr)
	{
		Py_INCREF(Object);
		FDreamPython::RegisterRef(Generation);
	}
}

FDreamPyRef::FDreamPyRef(FDreamPyRef&& Other)
	: Object(Other.Object)
	, Generation(Other.Generation)
{
	Other.Object = nullptr;
	Other.Generation = 0;
}

FDreamPyRef& FDreamPyRef::operator=(const FDreamPyRef& Other)
{
	if (this != &Other)
	{
		FDreamPyRef Copy(Other);
		*this = MoveTemp(Copy);
	}
	return *this;
}

FDreamPyRef& FDreamPyRef::operator=(FDreamPyRef&& Other)
{
	if (this != &Other)
	{
		Reset();
		Object = Other.Object;
		Generation = Other.Generation;
		Other.Object = nullptr;
		Other.Generation = 0;
	}
	return *this;
}

FDreamPyRef::~FDreamPyRef()
{
	Reset();
}

PyObject* FDreamPyRef::Release()
{
	PyObject* Released = Object;
	if (Released != nullptr)
	{
		FDreamPython::UnregisterRef(Generation);
	}
	Object = nullptr;
	Generation = 0;
	return Released;
}

void FDreamPyRef::Reset()
{
	if (Object == nullptr)
	{
		return;
	}
	const uint32 OldGeneration = Generation;
	PyObject* Old = Release();
	DreamGilPrivate::DropReference(Old, OldGeneration);
}

#endif // WITH_PYTHON
