// SPDX-License-Identifier: Apache-2.0
// Captures what reaches the Unreal log (the Output Log and Saved/Logs) while a spec runs: FDreamLogForwarder echoes run
// output under LogPython from the DreamOutput thread, with "# Result: " on result lines (§6.3), and the console executor
// echoes its input once (§6.6). Registered on GLog for the object's lifetime. Serialize runs on any thread.
#pragma once

#include "CoreMinimal.h"
#include "HAL/CriticalSection.h"
#include "Misc/OutputDevice.h"

namespace DreamTest
{
	class FDreamTestLogCapture : public FOutputDevice
	{
	public:
		FDreamTestLogCapture();
		virtual ~FDreamTestLogCapture() override;

		virtual void Serialize(const TCHAR* Text, ELogVerbosity::Type Verbosity, const FName& Category) override;
		virtual bool CanBeUsedOnAnyThread() const override { return true; }
		virtual bool CanBeUsedOnMultipleThreads() const override { return true; }

		/** Lines of Category that equal Text exactly. */
		int32 CountEqual(FName Category, const FString& Text) const;
		/** Lines of Category that contain Needle. */
		int32 CountContaining(FName Category, const FString& Needle) const;
		/** Every captured line of Category, for failure messages. */
		FString Describe(FName Category) const;

	private:
		struct FLine
		{
			FName Category;
			FString Text;
		};

		mutable FCriticalSection Lock;
		TArray<FLine> Lines;
	};
}
