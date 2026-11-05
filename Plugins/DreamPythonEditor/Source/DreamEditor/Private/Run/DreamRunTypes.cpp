// SPDX-License-Identifier: Apache-2.0
// U2 and SPEC wording for run states, outcomes, routes and stop sources (TDD §5.1.1, §5.2.1, §5.7.1, §12.2).
#include "Run/DreamRunTypes.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRunText
{
	FText StateName(EDreamRunState State)
	{
		switch (State)
		{
		case EDreamRunState::Idle:
			return LOCTEXT("StateIdle", "Idle");
		case EDreamRunState::Starting:
			return LOCTEXT("StateStarting", "Starting");
		case EDreamRunState::Running:
			return LOCTEXT("StateRunning", "Running");
		case EDreamRunState::Stopping:
			return LOCTEXT("StateStopping", "Stopping…");
		case EDreamRunState::Paused:
			return LOCTEXT("StatePaused", "Paused");
		case EDreamRunState::TaskRunning:
			return LOCTEXT("StateTaskRunning", "Task running");
		}
		return FText::GetEmpty();
	}

	FText OutcomeName(EDreamRunOutcome Outcome)
	{
		switch (Outcome)
		{
		case EDreamRunOutcome::None:
			return FText::GetEmpty();
		case EDreamRunOutcome::Ok:
			return LOCTEXT("OutcomeOk", "ok");
		case EDreamRunOutcome::Error:
			return LOCTEXT("OutcomeError", "error");
		case EDreamRunOutcome::Cancelled:
			return LOCTEXT("OutcomeCancelled", "cancelled");
		case EDreamRunOutcome::NotExecuted:
			return LOCTEXT("OutcomeNotExecuted", "not executed");
		case EDreamRunOutcome::Lost:
			return LOCTEXT("OutcomeLost", "lost");
		}
		return FText::GetEmpty();
	}

	bool IsWaitingForIdle(const FDreamRunRecord& Record, double NowSeconds)
	{
		if (Record.State != EDreamRunState::Starting || NowSeconds - Record.StartSeconds < StartingWaitNoticeSeconds)
		{
			return false;
		}
		// The snapshot step has its own 2 s bound and the Run File save its own progress; neither waits for idle.
		switch (Record.StartingStep)
		{
		case EDreamStartingStep::Queued:
		case EDreamStartingStep::Prepare:
		case EDreamStartingStep::Enter:
			return true;
		default:
			return false;
		}
	}

	FText StartingStateText(const FDreamRunRecord& Record, double NowSeconds)
	{
		return IsWaitingForIdle(Record, NowSeconds)
			? LOCTEXT("StateStartingWaiting", "Starting… waiting for the editor to become idle")
			: StateName(EDreamRunState::Starting);
	}

	const TCHAR* RouteKey(EDreamRunRoute Route)
	{
		switch (Route)
		{
		case EDreamRunRoute::Selection:
			return TEXT("selection");
		case EDreamRunRoute::Line:
			return TEXT("line");
		case EDreamRunRoute::Cell:
			return TEXT("cell");
		case EDreamRunRoute::Buffer:
			return TEXT("buffer");
		case EDreamRunRoute::Repl:
			return TEXT("repl");
		case EDreamRunRoute::ConsoleExecutor:
			return TEXT("console");
		case EDreamRunRoute::RunFile:
			return TEXT("run_file");
		case EDreamRunRoute::Shelf:
			return TEXT("shelf");
		case EDreamRunRoute::Snippet:
			return TEXT("snippet");
		case EDreamRunRoute::Reload:
			return TEXT("reload");
		case EDreamRunRoute::ReloadOnSave:
			return TEXT("reload_on_save");
		case EDreamRunRoute::Revert:
			return TEXT("revert");
		case EDreamRunRoute::EntryPoint:
			return TEXT("entry_point");
		case EDreamRunRoute::TaskStep:
			return TEXT("task_step");
		case EDreamRunRoute::Api:
			return TEXT("api");
		case EDreamRunRoute::Bridge:
			return TEXT("bridge");
		case EDreamRunRoute::Mcp:
			return TEXT("mcp");
		}
		return TEXT("unknown");
	}

	const TCHAR* StopSourceText(EDreamStopSource Source)
	{
		switch (Source)
		{
		case EDreamStopSource::RunWindowButton:
		case EDreamStopSource::BannerButton:
			return TEXT("Stop button");
		case EDreamStopSource::StatusBarButton:
			return TEXT("status bar");
		case EDreamStopSource::Esc:
			return TEXT("Esc");
		case EDreamStopSource::ShiftF5:
			return TEXT("Shift+F5");
		case EDreamStopSource::PausedWindow:
			return TEXT("Paused window");
		case EDreamStopSource::InterruptCleanup:
			return TEXT("Interrupt cleanup");
		case EDreamStopSource::Api:
			return TEXT("API");
		}
		return TEXT("async");
	}

	bool RouteTransactionDefault(EDreamRunRoute Route)
	{
		// The RT-08 column of TDD §5.2.1. An API run takes its target's default; FDreamRunService sets the override
		// for it, so the value here only matters for a request built without one.
		switch (Route)
		{
		case EDreamRunRoute::RunFile:
		case EDreamRunRoute::Reload:
		case EDreamRunRoute::ReloadOnSave:
		case EDreamRunRoute::Revert:
			return false;
		case EDreamRunRoute::Selection:
		case EDreamRunRoute::Line:
		case EDreamRunRoute::Cell:
		case EDreamRunRoute::Buffer:
		case EDreamRunRoute::Repl:
		case EDreamRunRoute::ConsoleExecutor:
		case EDreamRunRoute::Shelf:
		case EDreamRunRoute::Snippet:
		case EDreamRunRoute::EntryPoint:
		case EDreamRunRoute::TaskStep:
		case EDreamRunRoute::Api:
		case EDreamRunRoute::Bridge:
		case EDreamRunRoute::Mcp:
			return true;
		}
		return true;
	}

	bool HasRunFileSemantics(EDreamRunRoute Route, bool bFileTarget)
	{
		return Route == EDreamRunRoute::RunFile || (Route == EDreamRunRoute::Api && bFileTarget);
	}

	bool RouteUsesStatementMode(EDreamRunRoute Route, EDreamNamespaceMode Namespace, bool bFileTarget)
	{
		// Statement mode is On exactly where auto-print is On (RUN-27, §5.2.1); an Isolated run turns both off, and an API
		// run of a file follows Run File ("As the target"), which never uses it.
		if (Namespace != EDreamNamespaceMode::Session || HasRunFileSemantics(Route, bFileTarget))
		{
			return false;
		}
		switch (Route)
		{
		case EDreamRunRoute::Selection:
		case EDreamRunRoute::Line:
		case EDreamRunRoute::Cell:
		case EDreamRunRoute::Buffer:
		case EDreamRunRoute::Repl:
		case EDreamRunRoute::ConsoleExecutor:
		case EDreamRunRoute::Api:          // a tab target runs in the Session (§5.2.1 "As the target")
		case EDreamRunRoute::Bridge:
			return true;
		default:
			return false;
		}
	}
}

#undef LOCTEXT_NAMESPACE
