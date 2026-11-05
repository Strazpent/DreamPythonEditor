// SPDX-License-Identifier: Apache-2.0
// Dream's Unreal Insights trace channels (TDD §13.1 "Unreal Insights trace channel", §5.4.6, TEL-03). Body: DreamTrace.cpp
// (run group). The Dream.Pump and Dream.Run channels of TEL-03 are DreamPumpChannel and DreamRunChannel: a trace channel's
// name is its C identifier without the "Channel" suffix (Trace/Detail/Trace.h TRACE_PRIVATE_CHANNEL_IMPL, Channel.cpp
// Setup), so Insights and -trace= call them "DreamPump" and "DreamRun" (run group choice: no dot is possible there).
//   DreamPump: the CPU scope Dream.Pump around each pump and the Dream.Pump event per pump (cost, rate, lines moved).
//   DreamRun:  the CPU scope Dream.Run.PendingCall, and the Dream.Run events RunBegin, RunEnd and Deferral.
// The existing counters (Dream.Pump/CostMs, Dream.Pump/RateHz, Dream.Run/Deferrals) stay on the engine's counters channel.
// Example capture: UnrealEditor.exe DreamHost.uproject -trace=cpu,counters,DreamPump,DreamRun
#pragma once

#include "CoreMinimal.h"
#include "Trace/Trace.h"

#if UE_TRACE_ENABLED
UE_TRACE_CHANNEL_EXTERN(DreamPumpChannel);
UE_TRACE_CHANNEL_EXTERN(DreamRunChannel);
#endif

namespace DreamTrace
{
	/** One pump (§5.4.4): its cost, the rate in effect (§5.4.6) and whether the pending call drained output first. */
	DREAMEDITOR_API void TracePump(uint64 RunId, double CostMs, double RateHz, bool bOutputArrived);
	/** The entry into _run and the finish (§5.2.4). Outcome is the EDreamRunOutcome value. */
	DREAMEDITOR_API void TraceRunBegin(uint64 RunId, int32 RunNumber);
	DREAMEDITOR_API void TraceRunEnd(uint64 RunId, int32 RunNumber, uint8 Outcome, int32 Pumps, int32 Deferrals);
	/** A pending call that deferred (§5.4.3); Reason is the EDreamDeferReason value. */
	DREAMEDITOR_API void TraceDeferral(uint64 RunId, uint8 Reason);

	/** True when both channels are registered in this build (UE::Trace::IsChannel; Dream.Diagnostics.Trace). */
	DREAMEDITOR_API bool AreChannelsRegistered();
}
