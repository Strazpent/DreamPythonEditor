// SPDX-License-Identifier: Apache-2.0
// Bodies of the §14.2 fixture UObjects: the getter counter and CallPythonOverride.
#include "Fixtures/DreamTestGetterFixture.h"
#include "Fixtures/DreamTestOverrideFixture.h"

#include <atomic>

namespace DreamTestFixtureObjectsPrivate
{
	std::atomic<int64> GetterCalls{ 0 };
}

int64 UDreamTestGetterFixture::GetGetterCalls()
{
	return DreamTestFixtureObjectsPrivate::GetterCalls.load();
}

void UDreamTestGetterFixture::ResetGetterCalls()
{
	DreamTestFixtureObjectsPrivate::GetterCalls.store(0);
}

void UDreamTestGetterFixture::CountGetterCall()
{
	DreamTestFixtureObjectsPrivate::GetterCalls.fetch_add(1);
}

int32 UDreamTestOverrideFixture::CallPythonOverride(int32 Times)
{
	const int32 Count = FMath::Clamp(Times, 0, 1000);
	for (int32 Index = 0; Index < Count; ++Index)
	{
		++NativeCalls;
		OnPythonOverride(Index);
	}
	return Count;
}
