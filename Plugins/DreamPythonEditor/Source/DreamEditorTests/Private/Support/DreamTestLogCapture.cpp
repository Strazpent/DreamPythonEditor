// SPDX-License-Identifier: Apache-2.0
#include "Support/DreamTestLogCapture.h"

#include "Misc/OutputDeviceRedirector.h"
#include "Misc/ScopeLock.h"

namespace DreamTest
{
	FDreamTestLogCapture::FDreamTestLogCapture()
	{
		GLog->AddOutputDevice(this);
	}

	FDreamTestLogCapture::~FDreamTestLogCapture()
	{
		// RemoveOutputDevice waits for a Serialize in flight on another thread.
		GLog->RemoveOutputDevice(this);
	}

	void FDreamTestLogCapture::Serialize(const TCHAR* Text, ELogVerbosity::Type /*Verbosity*/, const FName& Category)
	{
		FScopeLock ScopeLock(&Lock);
		Lines.Add(FLine{ Category, FString(Text) });
	}

	int32 FDreamTestLogCapture::CountEqual(FName Category, const FString& Text) const
	{
		FScopeLock ScopeLock(&Lock);
		int32 Count = 0;
		for (const FLine& Line : Lines)
		{
			Count += (Line.Category == Category && Line.Text.TrimEnd() == Text) ? 1 : 0;
		}
		return Count;
	}

	int32 FDreamTestLogCapture::CountContaining(FName Category, const FString& Needle) const
	{
		FScopeLock ScopeLock(&Lock);
		int32 Count = 0;
		for (const FLine& Line : Lines)
		{
			Count += (Line.Category == Category && Line.Text.Contains(Needle)) ? 1 : 0;
		}
		return Count;
	}

	FString FDreamTestLogCapture::Describe(FName Category) const
	{
		FScopeLock ScopeLock(&Lock);
		FString Out;
		for (const FLine& Line : Lines)
		{
			if (Line.Category == Category)
			{
				Out += FString::Printf(TEXT(" | %s"), *Line.Text);
			}
		}
		return Out.IsEmpty() ? FString(TEXT(" (none)")) : Out;
	}
}
