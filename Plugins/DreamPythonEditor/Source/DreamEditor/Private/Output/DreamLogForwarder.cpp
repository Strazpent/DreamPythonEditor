// SPDX-License-Identifier: Apache-2.0
// FDreamLogForwarder (TDD §6.3 forwarding and fidelity rules, D19, RT-07, E3). See DreamLogForwarder.h.
// One UE_LOG per line, never GLog->Flush(), never through unreal.log; GDreamForwarding is set around every call so
// FDreamOutputDevice skips our own echo (T-OUT-021).
#include "Output/DreamLogForwarder.h"

#include "HAL/PlatformTime.h"
#include "Module/DreamLog.h"

thread_local bool GDreamForwarding = false;

// (slice A choice, A30) PythonScriptPlugin's LogPython is private (Private/PyUtil.h:16); this static object has the same
// name, so the log shows "LogPython:" as stock output does and LogPython verbosity settings apply to it too
// (FLogSuppressionInterface associates categories by name, LogSuppressionInterface.cpp:67).
DEFINE_LOG_CATEGORY_STATIC(LogPython, Log, All);

namespace DreamForwarderPrivate
{
	struct FForwardingScope
	{
		bool bPrevious;
		FForwardingScope() : bPrevious(GDreamForwarding) { GDreamForwarding = true; }
		~FForwardingScope() { GDreamForwarding = bPrevious; }
	};

	void EmitTo(bool bDreamCategory, ELogVerbosity::Type Verbosity, const FString& Text)
	{
		FForwardingScope Scope;
		const ELogVerbosity::Type Level = static_cast<ELogVerbosity::Type>(Verbosity & ELogVerbosity::VerbosityMask);
		if (bDreamCategory)
		{
			switch (Level)
			{
			case ELogVerbosity::Error:   UE_LOG(LogDream, Error, TEXT("%s"), *Text); break;
			case ELogVerbosity::Warning: UE_LOG(LogDream, Warning, TEXT("%s"), *Text); break;
			default:                     UE_LOG(LogDream, Log, TEXT("%s"), *Text); break;
			}
		}
		else
		{
			switch (Level)
			{
			case ELogVerbosity::Error:   UE_LOG(LogPython, Error, TEXT("%s"), *Text); break;
			case ELogVerbosity::Warning: UE_LOG(LogPython, Warning, TEXT("%s"), *Text); break;
			default:                     UE_LOG(LogPython, Log, TEXT("%s"), *Text); break;
			}
		}
	}

	/** The stock ShouldLog: empty or whitespace-only text is never logged (PyCore.cpp:1158-1173, §6.3 rule 2). */
	bool ShouldLog(const FString& Text)
	{
		for (const TCHAR Char : Text)
		{
			if (!FChar::IsWhitespace(Char))
			{
				return true;
			}
		}
		return false;
	}

	/** "# Result: " goes before the first line of a repr only; its later lines carry OLF_ResultCont (§6.4). */
	FString WithResultPrefix(const FDreamForwardLine& Line, const FString& Text)
	{
		return Line.Type == dream::LineType::Result && (Line.Flags & dream::OLF_ResultCont) == 0 ? TEXT("# Result: ") + Text : Text;
	}

	/** 48000 -> "48,000" without FText (the ingest thread formats no culture-dependent text). */
	FString Grouped(int64 Value)
	{
		FString Digits = FString::Printf(TEXT("%lld"), Value);
		FString Out;
		const int32 Len = Digits.Len();
		for (int32 Index = 0; Index < Len; ++Index)
		{
			if (Index > 0 && (Len - Index) % 3 == 0 && Digits[Index - 1] != TEXT('-'))
			{
				Out.AppendChar(TEXT(','));
			}
			Out.AppendChar(Digits[Index]);
		}
		return Out;
	}

	uint64 SplitKey(dream::LineType Type, uint32 RunNumber)
	{
		return (static_cast<uint64>(static_cast<uint8>(Type)) << 32) | RunNumber;
	}
}

FName FDreamLogForwarder::LogPythonName()
{
	static const FName Name(TEXT("LogPython"));
	return Name;
}

FName FDreamLogForwarder::LogDreamName()
{
	static const FName Name(TEXT("LogDream"));
	return Name;
}

bool FDreamLogForwarder::MapLine(dream::LineType Type, uint16 Flags, FName& OutCategory, ELogVerbosity::Type& OutVerbosity)
{
	// Lines FDreamOutputDevice captured are already in the log; input echo is never forwarded (OUT-12, T-OUT-044).
	if ((Flags & dream::OLF_FromLogPython) != 0)
	{
		return false;
	}
	switch (Type)
	{
	case dream::LineType::Stdout:
	case dream::LineType::Result:
	case dream::LineType::Info:
		OutCategory = LogPythonName();
		OutVerbosity = ELogVerbosity::Log;
		return true;
	case dream::LineType::Stderr:            // Error as in stock (unreal_core.py:59); the UI still says stderr
	case dream::LineType::Error:
		OutCategory = LogPythonName();
		OutVerbosity = ELogVerbosity::Error;
		return true;
	case dream::LineType::Warning:
		OutCategory = LogPythonName();
		OutVerbosity = ELogVerbosity::Warning;
		return true;
	case dream::LineType::Notice:
	case dream::LineType::Agent:
	case dream::LineType::Debugger:
	case dream::LineType::Logpoint:
	case dream::LineType::Evaluate:
	case dream::LineType::RunHeader:
	case dream::LineType::RunFooter:
		OutCategory = LogDreamName();          // §6.3 rule 4: Dream's own lines never reach LogPython parsers
		OutVerbosity = ELogVerbosity::Log;
		return true;
	case dream::LineType::Echo:
	default:
		return false;
	}
}

void FDreamLogForwarder::Forward(TConstArrayView<FDreamForwardLine> Lines)
{
	using namespace DreamForwarderPrivate;
	const double Now = FPlatformTime::Seconds();
	if (WindowStart == 0.0)
	{
		WindowStart = Now;
	}
	for (const FDreamForwardLine& Line : Lines)
	{
		const uint64 Key = SplitKey(Line.Type, Line.RunNumber);
		if ((Line.Flags & dream::OLF_EchoSplit) != 0)
		{
			// §6.3 rule 1: "Working..." waits for the rest of its line, so the log does not depend on statement starts.
			HeldSplits.FindOrAdd(Key).Append(Line.Text);
			continue;
		}
		FString Text = Line.Text;
		if (FString* Held = HeldSplits.Find(Key))
		{
			Text = *Held + Text;
			HeldSplits.Remove(Key);
		}
		if (!ShouldLog(Text))
		{
			continue;
		}
		if (Now - WindowStart >= 1.0)
		{
			CloseWindow();
			WindowStart = Now;
		}
		if (EchoCap > 0 && WindowLines >= EchoCap)
		{
			++WindowSuppressed;               // §6.3 rule 5: counted, summarized at the window's end
			++SuppressedTotal;
			WindowRunNumber = Line.RunNumber;
			continue;
		}
		++WindowLines;
		Emit(Line, WithResultPrefix(Line, Text));
	}
}

void FDreamLogForwarder::FlushRun(uint32 RunNumber)
{
	using namespace DreamForwarderPrivate;
	for (auto It = HeldSplits.CreateIterator(); It; ++It)
	{
		if (static_cast<uint32>(It->Key & 0xFFFFFFFFu) != RunNumber)
		{
			continue;
		}
		FDreamForwardLine Line;
		Line.Type = static_cast<dream::LineType>(static_cast<uint8>(It->Key >> 32));
		Line.RunNumber = RunNumber;
		Line.Text = MoveTemp(It->Value);
		It.RemoveCurrent();
		if (MapLine(Line.Type, 0, Line.Category, Line.Verbosity))
		{
			Forward(MakeArrayView(&Line, 1));
		}
	}
}

void FDreamLogForwarder::Emit(const FDreamForwardLine& Line, const FString& Text) const
{
	DreamForwarderPrivate::EmitTo(Line.Category == LogDreamName(), Line.Verbosity, Text);
}

void FDreamLogForwarder::EchoNow(const FDreamForwardLine& Line)
{
	DreamForwarderPrivate::EmitTo(Line.Category == LogDreamName(), Line.Verbosity, Line.Text);
}

void FDreamLogForwarder::CloseWindow()
{
	using namespace DreamForwarderPrivate;
	if (WindowSuppressed > 0)
	{
		FString Path = RunLogPathProvider ? RunLogPathProvider(WindowRunNumber) : FString();
		if (Path.IsEmpty())
		{
			Path = TEXT("the Dream output view");
		}
		const FString Summary = FString::Printf(TEXT("[Dream Python Editor] Run #%u: %s lines not echoed in the last second (echo cap %s/s). Full output: %s"),
			WindowRunNumber, *Grouped(WindowSuppressed), *Grouped(EchoCap), *Path);
		EmitTo(/*bDreamCategory*/ false, ELogVerbosity::Warning, Summary);
	}
	WindowLines = 0;
	WindowSuppressed = 0;
}

void FDreamLogForwarder::Tick(double Now)
{
	if (WindowStart != 0.0 && Now - WindowStart >= 1.0)
	{
		CloseWindow();
		WindowStart = Now;
	}
}

void FDreamLogForwarder::SetRunLogPathProvider(TFunction<FString(uint32)> Provider)
{
	RunLogPathProvider = MoveTemp(Provider);
}

void FDreamLogForwarder::SetEchoCap(int32 LinesPerSecond)
{
	const int32 Cap = FMath::Max(0, LinesPerSecond);
	if (Cap != EchoCap && WindowStart != 0.0)
	{
		// (output choice) A new cap starts a new window (§6.3 rule 5): the open window's summary names the cap that held
		// its lines back, and lines echoed under the old cap (0 = unlimited) never count against the new one.
		CloseWindow();
		WindowStart = FPlatformTime::Seconds();
	}
	EchoCap = Cap;
}

bool FDreamLogForwarder::IsSuppressing() const
{
	return WindowSuppressed > 0;
}

void FDreamLogForwarder::EmergencyFlush(TConstArrayView<FDreamForwardLine> Lines)
{
	using namespace DreamForwarderPrivate;
	CloseWindow();
	for (const FDreamForwardLine& Line : Lines)
	{
		const uint64 Key = SplitKey(Line.Type, Line.RunNumber);
		FString Text = Line.Text;
		if (FString* Held = HeldSplits.Find(Key))
		{
			Text = *Held + Text;
			HeldSplits.Remove(Key);
		}
		if ((Line.Flags & dream::OLF_EchoSplit) == 0 && ShouldLog(Text))
		{
			Emit(Line, WithResultPrefix(Line, Text));
		}
		else if ((Line.Flags & dream::OLF_EchoSplit) != 0)
		{
			HeldSplits.Add(Key, MoveTemp(Text));
		}
	}
	for (TPair<uint64, FString>& Held : HeldSplits)
	{
		FDreamForwardLine Line;
		Line.Type = static_cast<dream::LineType>(static_cast<uint8>(Held.Key >> 32));
		if (ShouldLog(Held.Value) && MapLine(Line.Type, 0, Line.Category, Line.Verbosity))
		{
			Emit(Line, Held.Value);
		}
	}
	HeldSplits.Reset();
}
