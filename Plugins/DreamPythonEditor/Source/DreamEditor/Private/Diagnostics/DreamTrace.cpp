// SPDX-License-Identifier: Apache-2.0
// Dream's Insights trace channels and events (TDD §13.1, §5.4.6, TEL-03). See DreamTrace.h.
#include "Diagnostics/DreamTrace.h"

#include "HAL/PlatformTime.h"
#include "Trace/Trace.inl"

#if UE_TRACE_ENABLED

UE_TRACE_CHANNEL_DEFINE(DreamPumpChannel)
UE_TRACE_CHANNEL_DEFINE(DreamRunChannel)

UE_TRACE_EVENT_BEGIN(Dream, Pump)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, RunId)
	UE_TRACE_EVENT_FIELD(float, CostMs)
	UE_TRACE_EVENT_FIELD(float, RateHz)
	UE_TRACE_EVENT_FIELD(uint8, OutputArrived)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Dream, RunBegin)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, RunId)
	UE_TRACE_EVENT_FIELD(int32, RunNumber)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Dream, RunEnd)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, RunId)
	UE_TRACE_EVENT_FIELD(int32, RunNumber)
	UE_TRACE_EVENT_FIELD(uint8, Outcome)
	UE_TRACE_EVENT_FIELD(int32, Pumps)
	UE_TRACE_EVENT_FIELD(int32, Deferrals)
UE_TRACE_EVENT_END()

UE_TRACE_EVENT_BEGIN(Dream, Deferral)
	UE_TRACE_EVENT_FIELD(uint64, Cycle)
	UE_TRACE_EVENT_FIELD(uint64, RunId)
	UE_TRACE_EVENT_FIELD(uint8, Reason)
UE_TRACE_EVENT_END()

#endif // UE_TRACE_ENABLED

namespace DreamTrace
{
	void TracePump(uint64 RunId, double CostMs, double RateHz, bool bOutputArrived)
	{
#if UE_TRACE_ENABLED
		UE_TRACE_LOG(Dream, Pump, DreamPumpChannel)
			<< Pump.Cycle(FPlatformTime::Cycles64())
			<< Pump.RunId(RunId)
			<< Pump.CostMs(static_cast<float>(CostMs))
			<< Pump.RateHz(static_cast<float>(RateHz))
			<< Pump.OutputArrived(bOutputArrived ? uint8(1) : uint8(0));
#else
		(void)RunId; (void)CostMs; (void)RateHz; (void)bOutputArrived;
#endif
	}

	void TraceRunBegin(uint64 RunId, int32 RunNumber)
	{
#if UE_TRACE_ENABLED
		UE_TRACE_LOG(Dream, RunBegin, DreamRunChannel)
			<< RunBegin.Cycle(FPlatformTime::Cycles64())
			<< RunBegin.RunId(RunId)
			<< RunBegin.RunNumber(RunNumber);
#else
		(void)RunId; (void)RunNumber;
#endif
	}

	void TraceRunEnd(uint64 RunId, int32 RunNumber, uint8 Outcome, int32 Pumps, int32 Deferrals)
	{
#if UE_TRACE_ENABLED
		UE_TRACE_LOG(Dream, RunEnd, DreamRunChannel)
			<< RunEnd.Cycle(FPlatformTime::Cycles64())
			<< RunEnd.RunId(RunId)
			<< RunEnd.RunNumber(RunNumber)
			<< RunEnd.Outcome(Outcome)
			<< RunEnd.Pumps(Pumps)
			<< RunEnd.Deferrals(Deferrals);
#else
		(void)RunId; (void)RunNumber; (void)Outcome; (void)Pumps; (void)Deferrals;
#endif
	}

	void TraceDeferral(uint64 RunId, uint8 Reason)
	{
#if UE_TRACE_ENABLED
		UE_TRACE_LOG(Dream, Deferral, DreamRunChannel)
			<< Deferral.Cycle(FPlatformTime::Cycles64())
			<< Deferral.RunId(RunId)
			<< Deferral.Reason(Reason);
#else
		(void)RunId; (void)Reason;
#endif
	}

	bool AreChannelsRegistered()
	{
#if UE_TRACE_ENABLED
		return UE::Trace::IsChannel(TEXT("DreamPump")) && UE::Trace::IsChannel(TEXT("DreamRun"));
#else
		return false;
#endif
	}
}
