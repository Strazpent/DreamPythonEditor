// SPDX-License-Identifier: Apache-2.0
// Dream core: the per-server LSP protocol state (TDD §7.2.1, §7.2.3, §7.2.4, §7.2.6, D4). One LspClient per server,
// single-threaded, on that server's transport read thread (FLspTransport, DreamEditor): capability negotiation, document
// sync (incremental, UTF-8 or UTF-16 positions), the pending table with deadlines and $/cancelRequest, stale-result
// dropping, diagnostics coalesced per document, server-to-client requests, and the 5 MB rule of §7.1.5. Pure: no I/O,
// no threads, no clock (callers pass monotonic milliseconds), no engine. Frames go out through ILspIo; results come
// back as LspEvents with every position converted to 0-based lines and UTF-16 columns of the snapshot they refer to.
// (lang choice) The §7.2.5 profile files (Config/Dream/LspProfiles/<id>.json) and NeutralConfig are M2 with the type
// server and ruff; M1's one server (dream-check) is described by LspClientOptions.
// Tier 1: Tests/DreamCore/test_jsonrpc.cpp.
#pragma once

#include "dream/Export.h"
#include "dream/Json.h"
#include "dream/TextBuffer.h"

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace dream
{
	class TextBuffer;

	using LspDocId = uint64_t;
	using LspRequestId = int64_t;

	/** §7.2.6. */
	enum class LspState : uint8_t
	{
		Starting,           // constructed; Start not called
		Initializing,       // initialize sent
		Ready,              // initialize answered, initialized sent
		Stalled,            // 3 consecutive timeouts and 10 s without stdout bytes (§7.2.3): counts as a crash
		ShuttingDown,       // shutdown sent
		Exited,
	};

	DREAMCORE_API const char* LspStateName(LspState State);

	enum class LspPositionEncoding : uint8_t
	{
		Utf16,
		Utf8,
	};

	enum class LspSyncKind : uint8_t
	{
		None,
		Full,
		Incremental,
	};

	/** §7.2.4: server capabilities as a bitset, which drives routing (§7.3.6). */
	namespace LspFeature
	{
		inline constexpr uint32_t Completion = 1u << 0;
		inline constexpr uint32_t Hover = 1u << 1;
		inline constexpr uint32_t SignatureHelp = 1u << 2;
		inline constexpr uint32_t Definition = 1u << 3;
		inline constexpr uint32_t DocumentSymbol = 1u << 4;
		inline constexpr uint32_t CodeAction = 1u << 5;
		inline constexpr uint32_t Formatting = 1u << 6;
		inline constexpr uint32_t RangeFormatting = 1u << 7;
		inline constexpr uint32_t WorkspaceSymbol = 1u << 8;
	}

	struct LspServerFeatures
	{
		uint32_t Bits = 0;
		LspPositionEncoding Encoding = LspPositionEncoding::Utf16;
		LspSyncKind Sync = LspSyncKind::Incremental;
		bool bOpenClose = true;

		bool Has(uint32_t Feature) const { return (Bits & Feature) != 0; }
	};

	/** 0-based lines, UTF-16 columns (the editor's coordinates). */
	struct LspRange
	{
		int32_t StartLine = 0;
		int32_t StartCol = 0;
		int32_t EndLine = 0;
		int32_t EndCol = 0;
	};

	struct LspDiagnostic
	{
		LspRange Range;
		int32_t Severity = 1;               // LSP 1 Error, 2 Warning, 3 Information, 4 Hint (§7.8.2)
		std::string Code;
		std::string Source;
		std::string Message;
		bool bFixable = false;              // data.fixable: a quick fix other than "Suppress" exists (dream-check)
	};

	struct LspTextEdit
	{
		LspRange Range;
		std::u16string NewText;
	};

	/** A CodeAction literal, its edit converted for one document (§7.8.3). */
	struct LspCodeAction
	{
		std::string Title;
		std::string Kind;
		bool bPreferred = false;
		std::vector<std::string> Codes;     // codes of the diagnostics it fixes
		LspDocId Doc = 0;
		bool bHasVersion = false;
		uint64_t Version = 0;               // the document version the edit was computed for
		std::vector<LspTextEdit> Edits;
		bool bTouchesOtherDocuments = false;    // part of the edit names a document that is not open here (dropped)
	};

	enum class LspEventKind : uint8_t
	{
		StateChanged,       // State
		Diagnostics,        // Doc, Version (bHasVersion always true: a set without one refers to the newest version sent)
		Response,           // Id, Method, Doc, Version, Result (raw), Actions (textDocument/codeAction, converted)
		Error,              // Id, Method, ErrorCode, Text: the server answered with an error (not -32800/-32801)
		Timeout,            // Id, Method: the deadline passed, one $/cancelRequest was sent, a late answer is dropped
		Dropped,            // Id, Method, ErrorCode: answered RequestCancelled or ContentModified; never shown (§7.2.3),
		                    // but the request's owner is released
		Stale,              // Id, Method, Doc, Version: an answer for an older version was dropped (formatting notice)
		Log,                // Text: window/logMessage, window/showMessage(Request), unknown traffic worth a log line
		ProtocolError,      // Text: the client cannot continue with this instance (initialize timeout, bad message)
		ApplyEdit,          // Actions[0]: workspace/applyEdit accepted within 5 s of a user-picked code action
	};

	struct LspEvent
	{
		LspEventKind Kind = LspEventKind::Log;
		LspState State = LspState::Starting;
		LspDocId Doc = 0;
		uint64_t Version = 0;
		bool bHasVersion = false;
		std::vector<LspDiagnostic> Diagnostics;
		LspRequestId Id = 0;
		std::string Method;
		json::Value Result;
		std::vector<LspCodeAction> Actions;
		int64_t ErrorCode = 0;
		std::string Text;
	};

	/** The client's only output: whole frames for the server's stdin (the transport's write thread sends them). */
	class ILspIo
	{
	public:
		virtual ~ILspIo() = default;
		virtual void SendFrame(std::vector<uint8_t>&& Frame) = 0;
	};

	struct LspWorkspaceFolder
	{
		std::string Uri;
		std::string Name;
	};

	/** One applied ChangeSet and the snapshot right after it; its After.Version() is the new LSP document version. */
	struct LspDocChange
	{
		ChangeSet Changes;
		TextSnapshot After;
	};

	struct LspClientOptions
	{
		std::string ServerId;
		std::string ClientName = "Dream";
		std::string ClientVersion;
		json::Value InitializationOptions;  // null sends {}
		bool bOfferUtf8 = true;             // positionEncodings ["utf-8", "utf-16"] (UTF-8 preferred, §7.2.3)
		int64_t InitializeTimeoutMs = 30000;    // §7.3.5 (proposed)
		/** §7.1.5: buffers above this many UTF-8 bytes are never sent (didClose instead of the change). */
		int64_t MaxDocumentBytes = int64_t(5) << 20;
		/** dream-check: a skipped buffer gets "dream/skipped {uri, version, bytes}" instead (§7.1.5, SUM902). */
		bool bSendSkippedNotification = false;
		/** workspace/configuration (§7.2.4): the value for one item; null when unknown. Called on the client's thread. */
		std::function<json::Value(std::string_view ScopeUri, std::string_view Section)> Configuration;
	};

	/** §7.2.3 deadlines: 1.5 s for interactive requests, 5 s for definition, documentSymbol and formatting. */
	DREAMCORE_API int64_t DefaultRequestTimeoutMs(std::string_view Method);

	class DREAMCORE_API LspClient
	{
	public:
		LspClient(ILspIo& InIo, LspClientOptions InOptions);
		~LspClient();
		LspClient(const LspClient&) = delete;
		LspClient& operator=(const LspClient&) = delete;

		/** Sends initialize (processId = ParentPid, the editor; §7.1.3 "All platforms"). Starting -> Initializing. */
		void Start(int64_t ParentPid, const std::string& RootUri, const std::vector<LspWorkspaceFolder>& Roots, int64_t NowMs);

		/** One decoded frame from the server's stdout. */
		void OnFrame(json::Value&& Message, int64_t NowMs);
		/** Any stdout byte arrived (stall detection, §7.2.3). */
		void NoteInput(int64_t NowMs);

		/** Documents. Before Ready they are remembered and opened once the server is Ready (§7.3.5 re-open). */
		void Open(LspDocId Doc, const std::string& Uri, const std::string& LanguageId, const TextSnapshot& Text);
		/** One frame's edits become one didChange (§7.2.3); a gap in versions sends the whole text. */
		void Change(LspDocId Doc, const std::vector<LspDocChange>& Changes);
		void Close(LspDocId Doc);

		void Notify(std::string_view Method, json::Value Params);
		/**
		 * Sends a request unless the server lacks the feature (T-LSP-017; returns 0 then). Doc and DocVersion tie the
		 * answer to a document version for stale dropping; TimeoutMs <= 0 means no deadline.
		 */
		LspRequestId Request(std::string_view Method, LspDocId Doc, uint64_t DocVersion, json::Value Params, int64_t TimeoutMs,
			int64_t NowMs);
		/** textDocument/codeAction for Range (UTF-16) of Doc's newest version, with the given context diagnostics. */
		LspRequestId RequestCodeActions(LspDocId Doc, const LspRange& Range, const std::vector<LspDiagnostic>& Context, int64_t NowMs);
		/** Sends one $/cancelRequest; the answer is dropped. */
		void Cancel(LspRequestId Id);

		/** The user picked a code action from this server: workspace/applyEdit is accepted for 5 s (§7.2.4). */
		void NoteUserAction(int64_t NowMs);

		/** Deadlines, cancel frames, the initialize timeout and stall detection. */
		void Tick(int64_t NowMs);
		/** Sends shutdown (Ready or Initializing); exit follows its answer. Returns false when nothing was sent. */
		bool Shutdown(int64_t NowMs);
		/** The process is gone: Exited. */
		void OnExited();

		/** Moves out every event since the last call; diagnostics last, coalesced to the newest set per document. */
		void TakeEvents(std::vector<LspEvent>& Out);

		LspState State() const { return CurrentState; }
		const LspServerFeatures& Features() const { return ServerFeatures; }
		bool IsShutdownAnswered() const { return bShutdownAnswered; }
		bool IsDocumentOpen(LspDocId Doc) const;
		bool IsDocumentSkipped(LspDocId Doc) const;
		/** The newest version sent (or remembered before Ready) for Doc; 0 when unknown. */
		uint64_t SentVersion(LspDocId Doc) const;
		size_t PendingRequestCount() const { return Pending.size(); }

		/** UTF-8 byte length of a snapshot's text (the §7.1.5 size check). */
		static int64_t Utf8Length(const TextSnapshot& Text);

	private:
		struct DocState
		{
			std::string Uri;
			std::string LanguageId;
			TextSnapshot Current;
			bool bOpenOnServer = false;
			bool bSkipped = false;
			std::vector<TextSnapshot> Recent;   // recent versions for position conversion, newest last
		};
		struct PendingRequest
		{
			std::string Method;
			LspDocId Doc = 0;
			uint64_t DocVersion = 0;
			int64_t Deadline = 0;           // 0: none
		};

		void SetState(LspState NewState);
		void Send(const json::Value& Message);
		void SendOpen(LspDocId Doc, DocState& State);
		void SendSkipped(const DocState& State, int64_t Bytes);
		void RememberSnapshot(DocState& State, const TextSnapshot& Snapshot);
		const TextSnapshot* FindSnapshot(const DocState& State, uint64_t Version) const;
		json::Value MakePosition(const TextSnapshot& Snapshot, int32_t Line, int32_t Col16) const;
		bool ReadPosition(const json::Value& Position, const TextSnapshot* Snapshot, int32_t& OutLine, int32_t& OutCol16) const;
		bool ReadRange(const json::Value& Range, const TextSnapshot* Snapshot, LspRange& Out) const;
		LspDocId FindDocByUri(std::string_view Uri) const;
		void HandleResponse(const json::Value& Message);
		void HandleServerRequest(const json::Value& Message);
		void HandleNotification(const json::Value& Message);
		void HandleInitializeResult(const json::Value& Result);
		bool ConvertWorkspaceEdit(const json::Value& Edit, LspCodeAction& Action) const;
		void ConvertCodeActions(const json::Value& Result, std::vector<LspCodeAction>& Out) const;
		void PostLog(std::string Text);
		bool CanSendCancel() const;

		ILspIo& Io;
		LspClientOptions Options;
		LspState CurrentState = LspState::Starting;
		LspServerFeatures ServerFeatures;
		int64_t NextId = 1;
		LspRequestId InitializeId = 0;
		LspRequestId ShutdownId = 0;
		bool bShutdownAnswered = false;
		int64_t StartMs = 0;
		int64_t LastInputMs = 0;
		int64_t UserActionMs = -1;
		int32_t ConsecutiveTimeouts = 0;
		std::map<LspDocId, DocState> Docs;
		std::unordered_map<LspRequestId, PendingRequest> Pending;
		std::set<LspRequestId> Abandoned;   // timed out or cancelled: a late answer is dropped
		std::vector<LspEvent> Events;
		std::map<LspDocId, LspEvent> PendingDiagnostics;
	};

	/**
	 * T-DIAG-020: maps a range from an older version of Buffer to its current version with TextBuffer::MapLine (§7.2.3
	 * "Diagnostic versions"). Columns are kept and clamped to the mapped lines. False when the edit log no longer covers
	 * FromVersion or a line of the range was deleted.
	 */
	DREAMCORE_API bool MapRangeForward(const TextBuffer& Buffer, uint64_t FromVersion, LspRange& InOutRange);

	/**
	 * §7.3.5 supervision backoff (T-LSP-015): 1, 2, 4 s doubling to a 60 s cap; 5 crashes within 180 s pause the server
	 * ("Language features paused"); 300 s of healthy uptime after Ready reset the doubling (proposed values).
	 */
	class DREAMCORE_API LspBackoff
	{
	public:
		struct Decision
		{
			bool bPaused = false;
			int64_t DelayMs = 0;
		};

		static constexpr int64_t FirstDelayMs = 1000;
		static constexpr int64_t MaxDelayMs = 60000;
		static constexpr int32_t PauseCrashCount = 5;
		static constexpr int64_t PauseWindowMs = 180000;
		static constexpr int64_t HealthyResetMs = 300000;

		/** A crash (exit, protocol error, stall, spawn failure) at NowMs: the delay before the next spawn, or Paused. */
		Decision OnCrash(int64_t NowMs);
		/** The server reached Ready at NowMs. */
		void OnReady(int64_t NowMs);
		/** Call while Ready: after HealthyResetMs of uptime the doubling restarts at 1 s. */
		void OnHealthy(int64_t NowMs);
		/** The user's Restart from Paused. */
		void Reset();

		bool IsPaused() const { return bPaused; }
		int32_t RestartCount() const { return Consecutive; }

	private:
		std::vector<int64_t> Crashes;
		int32_t Consecutive = 0;
		int64_t ReadySinceMs = -1;
		bool bPaused = false;
	};
}
