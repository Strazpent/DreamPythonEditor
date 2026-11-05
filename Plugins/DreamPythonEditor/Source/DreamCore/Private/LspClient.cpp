// SPDX-License-Identifier: Apache-2.0
// Dream core: the per-server LSP protocol state (TDD §7.2.3, §7.2.4, §7.3.5). See dream/LspClient.h.
#include "dream/LspClient.h"

#include "dream/JsonRpc.h"
#include "dream/Utf.h"

#include <algorithm>
#include <initializer_list>
#include <utility>

namespace dream
{
	namespace
	{
		using json::Value;

		constexpr size_t RecentSnapshots = 16;          // versions kept per document for position conversion
		constexpr size_t MaxAbandoned = 1024;
		constexpr int64_t ApplyEditWindowMs = 5000;     // §7.2.4
		constexpr int64_t StallSilenceMs = 10000;       // §7.2.3 (proposed)
		constexpr int32_t StallTimeouts = 3;

		Value Str(std::string_view Text)
		{
			return Value::MakeString(std::string(Text));
		}

		Value StrArray(std::initializer_list<const char*> Items)
		{
			Value A = Value::MakeArray();
			for (const char* Item : Items)
			{
				A.Append(Str(Item));
			}
			return A;
		}

		Value Obj()
		{
			return Value::MakeObject();
		}

		bool IsProvided(const Value* Capability)
		{
			if (Capability == nullptr || Capability->IsNull())
			{
				return false;
			}
			if (Capability->GetKind() == json::Kind::Bool)
			{
				return Capability->AsBool();
			}
			return true;
		}

		uint32_t FeatureForMethod(std::string_view Method)
		{
			if (Method == "textDocument/completion" || Method == "completionItem/resolve")
			{
				return LspFeature::Completion;
			}
			if (Method == "textDocument/hover")
			{
				return LspFeature::Hover;
			}
			if (Method == "textDocument/signatureHelp")
			{
				return LspFeature::SignatureHelp;
			}
			if (Method == "textDocument/definition")
			{
				return LspFeature::Definition;
			}
			if (Method == "textDocument/documentSymbol")
			{
				return LspFeature::DocumentSymbol;
			}
			if (Method == "textDocument/codeAction")
			{
				return LspFeature::CodeAction;
			}
			if (Method == "textDocument/formatting")
			{
				return LspFeature::Formatting;
			}
			if (Method == "textDocument/rangeFormatting")
			{
				return LspFeature::RangeFormatting;
			}
			if (Method == "workspace/symbol")
			{
				return LspFeature::WorkspaceSymbol;
			}
			return 0;
		}

		/** §7.2.3: answers for an older document version are dropped for these. */
		bool IsVersionBound(std::string_view Method)
		{
			return Method == "textDocument/completion" || Method == "completionItem/resolve" || Method == "textDocument/hover" ||
				Method == "textDocument/signatureHelp" || Method == "textDocument/formatting" ||
				Method == "textDocument/rangeFormatting" || Method == "textDocument/codeAction";
		}

		std::string CodeText(const Value& Code)
		{
			if (Code.IsString())
			{
				return Code.AsString();
			}
			if (Code.IsInteger())
			{
				return std::to_string(Code.AsInt());
			}
			return std::string();
		}

		std::u16string ToUtf16NoCr(const std::string& Utf8)
		{
			std::u16string Out;
			utf::Utf8ToUtf16(Utf8, Out);
			Out.erase(std::remove(Out.begin(), Out.end(), u'\r'), Out.end());
			return Out;
		}

		std::string ToUtf8(std::u16string_view Text)
		{
			std::string Out;
			utf::Utf16ToUtf8(Text, Out);
			return Out;
		}

		Value BuildClientCapabilities(bool bOfferUtf8)
		{
			Value Caps = Obj();

			Value General = Obj();
			General.Set("positionEncodings", bOfferUtf8 ? StrArray({ "utf-8", "utf-16" }) : StrArray({ "utf-16" }));
			Caps.Set("general", std::move(General));

			Value Text = Obj();
			{
				Value Sync = Obj();
				Sync.Set("dynamicRegistration", Value::MakeBool(false));
				Sync.Set("didSave", Value::MakeBool(true));
				Sync.Set("willSave", Value::MakeBool(false));
				Sync.Set("willSaveWaitUntil", Value::MakeBool(false));
				Text.Set("synchronization", std::move(Sync));
			}
			{
				Value Item = Obj();
				Item.Set("snippetSupport", Value::MakeBool(true));
				Item.Set("labelDetailsSupport", Value::MakeBool(true));
				Item.Set("insertReplaceSupport", Value::MakeBool(true));
				Item.Set("documentationFormat", StrArray({ "markdown", "plaintext" }));
				Value Resolve = Obj();
				Resolve.Set("properties", StrArray({ "documentation", "detail" }));
				Item.Set("resolveSupport", std::move(Resolve));
				Value Completion = Obj();
				Completion.Set("completionItem", std::move(Item));
				Completion.Set("contextSupport", Value::MakeBool(true));
				Text.Set("completion", std::move(Completion));
			}
			{
				Value Hover = Obj();
				Hover.Set("contentFormat", StrArray({ "markdown", "plaintext" }));
				Text.Set("hover", std::move(Hover));
			}
			{
				Value Param = Obj();
				Param.Set("labelOffsetSupport", Value::MakeBool(true));
				Value Info = Obj();
				Info.Set("documentationFormat", StrArray({ "markdown", "plaintext" }));
				Info.Set("parameterInformation", std::move(Param));
				Info.Set("activeParameterSupport", Value::MakeBool(true));
				Value Signature = Obj();
				Signature.Set("signatureInformation", std::move(Info));
				Text.Set("signatureHelp", std::move(Signature));
			}
			{
				Value Diag = Obj();
				Diag.Set("relatedInformation", Value::MakeBool(true));
				Diag.Set("versionSupport", Value::MakeBool(true));
				Diag.Set("codeDescriptionSupport", Value::MakeBool(true));
				Diag.Set("dataSupport", Value::MakeBool(true));
				Text.Set("publishDiagnostics", std::move(Diag));
			}
			{
				Value Kind = Obj();
				Kind.Set("valueSet", StrArray({ "quickfix", "source.fixAll", "source.organizeImports" }));
				Value Literal = Obj();
				Literal.Set("codeActionKind", std::move(Kind));
				Value Resolve = Obj();
				Resolve.Set("properties", StrArray({ "edit" }));
				Value Action = Obj();
				Action.Set("codeActionLiteralSupport", std::move(Literal));
				Action.Set("isPreferredSupport", Value::MakeBool(true));
				Action.Set("dataSupport", Value::MakeBool(true));
				Action.Set("resolveSupport", std::move(Resolve));
				Text.Set("codeAction", std::move(Action));
			}
			{
				Value Definition = Obj();
				Definition.Set("linkSupport", Value::MakeBool(true));
				Text.Set("definition", std::move(Definition));
				Value Symbols = Obj();
				Symbols.Set("hierarchicalDocumentSymbolSupport", Value::MakeBool(true));
				Text.Set("documentSymbol", std::move(Symbols));
				Text.Set("formatting", Obj());
				Text.Set("rangeFormatting", Obj());
			}
			Caps.Set("textDocument", std::move(Text));

			Value Workspace = Obj();
			Workspace.Set("configuration", Value::MakeBool(true));
			Workspace.Set("workspaceFolders", Value::MakeBool(true));
			Workspace.Set("applyEdit", Value::MakeBool(true));
			{
				Value Edit = Obj();
				Edit.Set("documentChanges", Value::MakeBool(true));
				Workspace.Set("workspaceEdit", std::move(Edit));
				Workspace.Set("symbol", Obj());
				Value Watched = Obj();
				Watched.Set("dynamicRegistration", Value::MakeBool(false));
				Workspace.Set("didChangeWatchedFiles", std::move(Watched));
				Value Config = Obj();
				Config.Set("dynamicRegistration", Value::MakeBool(false));
				Workspace.Set("didChangeConfiguration", std::move(Config));
			}
			Caps.Set("workspace", std::move(Workspace));

			Value Window = Obj();
			Window.Set("workDoneProgress", Value::MakeBool(true));
			Caps.Set("window", std::move(Window));
			return Caps;
		}
	}

	const char* LspStateName(LspState State)
	{
		switch (State)
		{
		case LspState::Starting: return "Starting";
		case LspState::Initializing: return "Initializing";
		case LspState::Ready: return "Ready";
		case LspState::Stalled: return "Stalled";
		case LspState::ShuttingDown: return "ShuttingDown";
		case LspState::Exited: return "Exited";
		}
		return "Unknown";
	}

	int64_t DefaultRequestTimeoutMs(std::string_view Method)
	{
		if (Method == "textDocument/completion" || Method == "completionItem/resolve" || Method == "textDocument/hover" ||
			Method == "textDocument/signatureHelp" || Method == "textDocument/codeAction" || Method == "workspace/symbol")
		{
			return 1500;
		}
		if (Method == "textDocument/definition" || Method == "textDocument/documentSymbol" || Method == "textDocument/formatting" ||
			Method == "textDocument/rangeFormatting")
		{
			return 5000;
		}
		return 0;
	}

	LspClient::LspClient(ILspIo& InIo, LspClientOptions InOptions)
		: Io(InIo)
		, Options(std::move(InOptions))
	{
	}

	LspClient::~LspClient() = default;

	void LspClient::SetState(LspState NewState)
	{
		if (CurrentState == NewState)
		{
			return;
		}
		CurrentState = NewState;
		LspEvent E;
		E.Kind = LspEventKind::StateChanged;
		E.State = NewState;
		Events.push_back(std::move(E));
	}

	void LspClient::Send(const json::Value& Message)
	{
		Io.SendFrame(JsonRpc::EncodeFrame(Message));
	}

	void LspClient::PostLog(std::string Text)
	{
		LspEvent E;
		E.Kind = LspEventKind::Log;
		E.Text = std::move(Text);
		Events.push_back(std::move(E));
	}

	void LspClient::Start(int64_t ParentPid, const std::string& RootUri, const std::vector<LspWorkspaceFolder>& Roots, int64_t NowMs)
	{
		if (CurrentState != LspState::Starting)
		{
			return;
		}
		Value Params = Obj();
		Params.Set("processId", Value::MakeInt(ParentPid));
		Value ClientInfo = Obj();
		ClientInfo.Set("name", Str(Options.ClientName));
		if (!Options.ClientVersion.empty())
		{
			ClientInfo.Set("version", Str(Options.ClientVersion));
		}
		Params.Set("clientInfo", std::move(ClientInfo));
		Params.Set("rootUri", RootUri.empty() ? Value::MakeNull() : Str(RootUri));
		Value Folders = Value::MakeArray();
		for (const LspWorkspaceFolder& Folder : Roots)
		{
			Value F = Obj();
			F.Set("uri", Str(Folder.Uri));
			F.Set("name", Str(Folder.Name));
			Folders.Append(std::move(F));
		}
		Params.Set("workspaceFolders", std::move(Folders));
		Params.Set("capabilities", BuildClientCapabilities(Options.bOfferUtf8));
		Params.Set("initializationOptions", Options.InitializationOptions.IsNull() ? Obj() : Options.InitializationOptions);
		InitializeId = NextId++;
		StartMs = NowMs;
		LastInputMs = NowMs;
		Send(JsonRpc::MakeRequest(InitializeId, "initialize", std::move(Params)));
		SetState(LspState::Initializing);
	}

	void LspClient::HandleInitializeResult(const json::Value& Result)
	{
		const Value& Caps = Result.Get("capabilities");
		LspServerFeatures F;
		const Value& Encoding = Caps.Get("positionEncoding");
		F.Encoding = (Options.bOfferUtf8 && Encoding.IsString() && Encoding.AsString() == "utf-8") ? LspPositionEncoding::Utf8
			: LspPositionEncoding::Utf16;
		const Value& Sync = Caps.Get("textDocumentSync");
		int64_t SyncKind = 2;
		if (Sync.IsInteger())
		{
			SyncKind = Sync.AsInt();
		}
		else if (Sync.IsObject())
		{
			F.bOpenClose = Sync.Get("openClose").AsBool(false);
			SyncKind = Sync.Get("change").AsInt(0);
		}
		else if (Sync.IsNull())
		{
			SyncKind = 0;
			F.bOpenClose = false;
		}
		F.Sync = SyncKind == 1 ? LspSyncKind::Full : SyncKind == 2 ? LspSyncKind::Incremental : LspSyncKind::None;
		if (Sync.IsInteger())
		{
			F.bOpenClose = SyncKind != 0;
		}
		struct FMap
		{
			const char* Key;
			uint32_t Bit;
		};
		static constexpr FMap Providers[] = {
			{ "completionProvider", LspFeature::Completion },
			{ "hoverProvider", LspFeature::Hover },
			{ "signatureHelpProvider", LspFeature::SignatureHelp },
			{ "definitionProvider", LspFeature::Definition },
			{ "documentSymbolProvider", LspFeature::DocumentSymbol },
			{ "codeActionProvider", LspFeature::CodeAction },
			{ "documentFormattingProvider", LspFeature::Formatting },
			{ "documentRangeFormattingProvider", LspFeature::RangeFormatting },
			{ "workspaceSymbolProvider", LspFeature::WorkspaceSymbol },
		};
		for (const FMap& Provider : Providers)
		{
			if (IsProvided(Caps.Find(Provider.Key)))
			{
				F.Bits |= Provider.Bit;
			}
		}
		ServerFeatures = F;
		Send(JsonRpc::MakeNotification("initialized", Obj()));
		SetState(LspState::Ready);
		// §7.3.5: every open document is (re)sent with didOpen at its current version.
		for (auto& [Doc, State] : Docs)
		{
			SendOpen(Doc, State);
		}
	}

	void LspClient::OnFrame(json::Value&& Message, int64_t NowMs)
	{
		LastInputMs = std::max(LastInputMs, NowMs);
		switch (JsonRpc::Classify(Message))
		{
		case JsonRpc::MessageKind::Response:
			HandleResponse(Message);
			break;
		case JsonRpc::MessageKind::Request:
			if (Message.Get("method").AsString() == "workspace/applyEdit")
			{
				const bool bAllowed = UserActionMs >= 0 && NowMs - UserActionMs <= ApplyEditWindowMs;
				Value Reply = Obj();
				if (bAllowed)
				{
					LspCodeAction Action;
					Action.Title = Message.Get("params").Get("label").AsString();
					const bool bConverted = ConvertWorkspaceEdit(Message.Get("params").Get("edit"), Action);
					if (bConverted && Action.Doc != 0 && Action.bTouchesOtherDocuments)
					{
						// Part of the edit could not be applied (a second document, a document that is not open, a file
						// operation): applying the rest would report a partial edit as applied.
						Reply.Set("applied", Value::MakeBool(false));
						Reply.Set("failureReason", Str("Dream Python Editor applies an edit to one open document only."));
					}
					else if (bConverted && Action.Doc != 0)
					{
						LspEvent E;
						E.Kind = LspEventKind::ApplyEdit;
						E.Actions.push_back(std::move(Action));
						Events.push_back(std::move(E));
						Reply.Set("applied", Value::MakeBool(true));
					}
					else
					{
						Reply.Set("applied", Value::MakeBool(false));
						Reply.Set("failureReason", Str("The edit names no open document."));
					}
					UserActionMs = -1;
				}
				else
				{
					Reply.Set("applied", Value::MakeBool(false));
					Reply.Set("failureReason", Str("Dream Python Editor accepts workspace/applyEdit only right after a code action the user picked."));
				}
				Send(JsonRpc::MakeResult(Message.Get("id"), std::move(Reply)));
			}
			else
			{
				HandleServerRequest(Message);
			}
			break;
		case JsonRpc::MessageKind::Notification:
			HandleNotification(Message);
			break;
		case JsonRpc::MessageKind::Invalid:
			PostLog("ignored a message that is not JSON-RPC 2.0");
			break;
		}
	}

	void LspClient::NoteInput(int64_t NowMs)
	{
		LastInputMs = std::max(LastInputMs, NowMs);
	}

	void LspClient::HandleResponse(const json::Value& Message)
	{
		const Value& IdValue = Message.Get("id");
		if (!IdValue.IsInteger())
		{
			PostLog("ignored a response with a non-integer id");
			return;
		}
		const LspRequestId Id = IdValue.AsInt();
		const Value* Error = Message.Find("error");
		if (InitializeId != 0 && Id == InitializeId)
		{
			InitializeId = 0;
			if (CurrentState != LspState::Initializing)
			{
				// After Shutdown() from Initializing or the initialize timeout (Stalled), a late answer must not bring the client
				// back to Ready: no initialized or didOpen may follow a shutdown.
				PostLog("ignored a late initialize answer");
				return;
			}
			if (Error != nullptr)
			{
				LspEvent E;
				E.Kind = LspEventKind::ProtocolError;
				E.Text = "initialize failed: " + Error->Get("message").AsString();
				Events.push_back(std::move(E));
				return;
			}
			HandleInitializeResult(Message.Get("result"));
			return;
		}
		if (ShutdownId != 0 && Id == ShutdownId)
		{
			ShutdownId = 0;
			bShutdownAnswered = true;
			Send(JsonRpc::MakeNotification("exit", Value()));
			return;
		}
		if (Abandoned.erase(Id) > 0)
		{
			return;     // a late answer after a timeout or a cancel (§7.2.3)
		}
		const auto It = Pending.find(Id);
		if (It == Pending.end())
		{
			return;
		}
		const PendingRequest Request = std::move(It->second);
		Pending.erase(It);
		ConsecutiveTimeouts = 0;
		if (Error != nullptr)
		{
			const int64_t Code = Error->Get("code").AsInt();
			if (Code == JsonRpc::ErrorCodes::RequestCancelled || Code == JsonRpc::ErrorCodes::ContentModified)
			{
				// Dropped silently for the user (§7.2.3), but its owner (a transport tag, a quick fix's callback) is released.
				LspEvent Dropped;
				Dropped.Kind = LspEventKind::Dropped;
				Dropped.Id = Id;
				Dropped.Method = Request.Method;
				Dropped.Doc = Request.Doc;
				Dropped.ErrorCode = Code;
				Events.push_back(std::move(Dropped));
				return;
			}
			LspEvent E;
			E.Kind = LspEventKind::Error;
			E.Id = Id;
			E.Method = Request.Method;
			E.Doc = Request.Doc;
			E.ErrorCode = Code;
			E.Text = Error->Get("message").AsString();
			Events.push_back(std::move(E));
			return;
		}
		LspEvent E;
		E.Id = Id;
		E.Method = Request.Method;
		E.Doc = Request.Doc;
		E.Version = Request.DocVersion;
		E.bHasVersion = Request.Doc != 0;
		if (Request.Doc != 0 && IsVersionBound(Request.Method) && Request.DocVersion < SentVersion(Request.Doc))
		{
			E.Kind = LspEventKind::Stale;
			Events.push_back(std::move(E));
			return;
		}
		E.Kind = LspEventKind::Response;
		E.Result = Message.Get("result");
		if (Request.Method == "textDocument/codeAction")
		{
			ConvertCodeActions(E.Result, E.Actions);
		}
		Events.push_back(std::move(E));
	}

	void LspClient::HandleServerRequest(const json::Value& Message)
	{
		const std::string& Method = Message.Get("method").AsString();
		const Value& Id = Message.Get("id");
		const Value& Params = Message.Get("params");
		if (Method == "workspace/configuration")
		{
			Value Result = Value::MakeArray();
			const Value& Items = Params.Get("items");
			for (size_t Index = 0; Index < Items.Size(); ++Index)
			{
				const Value& Item = Items.At(Index);
				Result.Append(Options.Configuration ? Options.Configuration(Item.Get("scopeUri").AsString(), Item.Get("section").AsString())
					: Value());
			}
			Send(JsonRpc::MakeResult(Id, std::move(Result)));
			return;
		}
		if (Method == "client/registerCapability" || Method == "client/unregisterCapability" || Method == "window/workDoneProgress/create")
		{
			Send(JsonRpc::MakeResult(Id, Value()));
			return;
		}
		if (Method == "window/showMessageRequest")
		{
			PostLog("showMessageRequest: " + Params.Get("message").AsString());
			Send(JsonRpc::MakeResult(Id, Value()));     // no popups (§7.2.4)
			return;
		}
		Send(JsonRpc::MakeError(Id, JsonRpc::ErrorCodes::MethodNotFound, "Method not found: " + Method));
	}

	void LspClient::HandleNotification(const json::Value& Message)
	{
		const std::string& Method = Message.Get("method").AsString();
		const Value& Params = Message.Get("params");
		if (Method == "textDocument/publishDiagnostics")
		{
			const LspDocId Doc = FindDocByUri(Params.Get("uri").AsString());
			if (Doc == 0)
			{
				return;     // a closed or unknown document
			}
			const DocState& State = Docs.at(Doc);
			LspEvent E;
			E.Kind = LspEventKind::Diagnostics;
			E.Doc = Doc;
			E.bHasVersion = true;
			const Value& VersionValue = Params.Get("version");
			// §7.2.3: a set with no version refers to the newest version sent before it arrived.
			E.Version = VersionValue.IsInteger() ? VersionValue.AsUInt() : State.Current.Version();
			const TextSnapshot* Snapshot = FindSnapshot(State, E.Version);
			if (Snapshot == nullptr)
			{
				Snapshot = &State.Current;
			}
			const Value& List = Params.Get("diagnostics");
			E.Diagnostics.reserve(List.Size());
			for (size_t Index = 0; Index < List.Size(); ++Index)
			{
				const Value& Item = List.At(Index);
				LspDiagnostic D;
				if (!ReadRange(Item.Get("range"), Snapshot, D.Range))
				{
					continue;
				}
				D.Severity = static_cast<int32_t>(std::clamp<int64_t>(Item.Get("severity").AsInt(1), 1, 4));
				D.Code = CodeText(Item.Get("code"));
				D.Source = Item.Get("source").AsString();
				D.Message = Item.Get("message").AsString();
				D.bFixable = Item.Get("data").Get("fixable").AsBool(false);
				E.Diagnostics.push_back(std::move(D));
			}
			PendingDiagnostics[Doc] = std::move(E);     // coalesced: only the newest set per document waits (T-LSP-037)
			return;
		}
		if (Method == "window/logMessage" || Method == "window/showMessage")
		{
			PostLog(Params.Get("message").AsString());
			return;
		}
		// $/progress, telemetry/event and anything else: nothing to do.
	}

	void LspClient::SendOpen(LspDocId Doc, DocState& State)
	{
		(void)Doc;
		State.bOpenOnServer = false;
		if (CurrentState != LspState::Ready || !ServerFeatures.bOpenClose || ServerFeatures.Sync == LspSyncKind::None)
		{
			return;
		}
		const int64_t Bytes = Utf8Length(State.Current);
		if (Bytes > Options.MaxDocumentBytes)
		{
			State.bSkipped = true;
			SendSkipped(State, Bytes);
			return;
		}
		State.bSkipped = false;
		Value Item = Obj();
		Item.Set("uri", Str(State.Uri));
		Item.Set("languageId", Str(State.LanguageId));
		Item.Set("version", Value::MakeUInt(State.Current.Version()));
		Item.Set("text", Str(State.Current.TextUtf8()));
		Value Params = Obj();
		Params.Set("textDocument", std::move(Item));
		Send(JsonRpc::MakeNotification("textDocument/didOpen", std::move(Params)));
		State.bOpenOnServer = true;
	}

	void LspClient::SendSkipped(const DocState& State, int64_t Bytes)
	{
		if (!Options.bSendSkippedNotification)
		{
			return;
		}
		Value Params = Obj();
		Params.Set("uri", Str(State.Uri));
		Params.Set("version", Value::MakeUInt(State.Current.Version()));
		Params.Set("bytes", Value::MakeInt(Bytes));
		Send(JsonRpc::MakeNotification("dream/skipped", std::move(Params)));
	}

	void LspClient::RememberSnapshot(DocState& State, const TextSnapshot& Snapshot)
	{
		if (!State.Recent.empty() && State.Recent.back().Version() == Snapshot.Version())
		{
			return;
		}
		State.Recent.push_back(Snapshot);
		if (State.Recent.size() > RecentSnapshots)
		{
			State.Recent.erase(State.Recent.begin());
		}
	}

	const TextSnapshot* LspClient::FindSnapshot(const DocState& State, uint64_t Version) const
	{
		for (auto It = State.Recent.rbegin(); It != State.Recent.rend(); ++It)
		{
			if (It->Version() == Version)
			{
				return &*It;
			}
		}
		return nullptr;
	}

	json::Value LspClient::MakePosition(const TextSnapshot& Snapshot, int32_t Line, int32_t Col16) const
	{
		int32_t Character = Col16;
		if (ServerFeatures.Encoding == LspPositionEncoding::Utf8)
		{
			std::u16string Scratch;
			Character = utf::Utf16ColToUtf8(Snapshot.LineView(Line, Scratch), Col16);
		}
		Value P = Obj();
		P.Set("line", Value::MakeInt(Line));
		P.Set("character", Value::MakeInt(Character));
		return P;
	}

	bool LspClient::ReadPosition(const json::Value& Position, const TextSnapshot* Snapshot, int32_t& OutLine, int32_t& OutCol16) const
	{
		const Value* Line = Position.Find("line");
		const Value* Character = Position.Find("character");
		if (Line == nullptr || Character == nullptr || Line->GetKind() != json::Kind::Number || Character->GetKind() != json::Kind::Number)
		{
			return false;
		}
		int64_t L = std::max<int64_t>(Line->AsInt(), 0);
		int64_t C = std::max<int64_t>(Character->AsInt(), 0);
		if (Snapshot != nullptr)
		{
			const int64_t LastLine = Snapshot->LineCount() - 1;
			if (L > LastLine)
			{
				// A position past the last line (the end of a text that ends in '\n' as some servers write it).
				L = LastLine;
				C = Snapshot->LineLength(static_cast<LineNo>(L));
				OutLine = static_cast<int32_t>(L);
				OutCol16 = static_cast<int32_t>(C);
				return true;
			}
			std::u16string Scratch;
			const std::u16string_view Text = Snapshot->LineView(static_cast<LineNo>(L), Scratch);
			if (ServerFeatures.Encoding == LspPositionEncoding::Utf8)
			{
				C = utf::Utf8ColToUtf16(Text, static_cast<int32_t>(std::min<int64_t>(C, INT32_MAX)));
			}
			C = std::min<int64_t>(C, static_cast<int64_t>(Text.size()));
		}
		OutLine = static_cast<int32_t>(std::min<int64_t>(L, INT32_MAX));
		OutCol16 = static_cast<int32_t>(std::min<int64_t>(C, INT32_MAX));
		return true;
	}

	bool LspClient::ReadRange(const json::Value& Range, const TextSnapshot* Snapshot, LspRange& Out) const
	{
		if (!ReadPosition(Range.Get("start"), Snapshot, Out.StartLine, Out.StartCol) ||
			!ReadPosition(Range.Get("end"), Snapshot, Out.EndLine, Out.EndCol))
		{
			return false;
		}
		if (Out.EndLine < Out.StartLine || (Out.EndLine == Out.StartLine && Out.EndCol < Out.StartCol))
		{
			Out.EndLine = Out.StartLine;
			Out.EndCol = Out.StartCol;
		}
		return true;
	}

	LspDocId LspClient::FindDocByUri(std::string_view Uri) const
	{
		for (const auto& [Doc, State] : Docs)
		{
			if (State.Uri == Uri)
			{
				return Doc;
			}
		}
		for (const auto& [Doc, State] : Docs)
		{
			if (JsonRpc::SameFileUri(State.Uri, Uri))
			{
				return Doc;
			}
		}
		return 0;
	}

	bool LspClient::ConvertWorkspaceEdit(const json::Value& Edit, LspCodeAction& Action) const
	{
		auto AddEdits = [&](std::string_view Uri, const Value& Edits, const Value* Version) -> bool
		{
			const LspDocId Doc = FindDocByUri(Uri);
			if (Doc == 0 || (Action.Doc != 0 && Action.Doc != Doc))
			{
				// (lang choice) M1 applies a code action to one open document; other documents are reported, not opened.
				Action.bTouchesOtherDocuments = true;
				return false;
			}
			const DocState& State = Docs.at(Doc);
			Action.Doc = Doc;
			const TextSnapshot* Snapshot = &State.Current;
			if (Version != nullptr && Version->IsInteger())
			{
				Action.bHasVersion = true;
				Action.Version = Version->AsUInt();
				if (const TextSnapshot* Found = FindSnapshot(State, Action.Version))
				{
					Snapshot = Found;
				}
			}
			for (size_t Index = 0; Index < Edits.Size(); ++Index)
			{
				const Value& Item = Edits.At(Index);
				LspTextEdit TextEdit;
				if (!ReadRange(Item.Get("range"), Snapshot, TextEdit.Range))
				{
					return false;
				}
				TextEdit.NewText = ToUtf16NoCr(Item.Get("newText").AsString());
				Action.Edits.push_back(std::move(TextEdit));
			}
			return true;
		};

		if (!Edit.IsObject())
		{
			return false;
		}
		bool bAny = false;
		const Value& DocumentChanges = Edit.Get("documentChanges");
		if (DocumentChanges.IsArray())
		{
			for (size_t Index = 0; Index < DocumentChanges.Size(); ++Index)
			{
				const Value& Change = DocumentChanges.At(Index);
				if (Change.Find("kind") != nullptr)
				{
					Action.bTouchesOtherDocuments = true;    // create / rename / delete file operations
					continue;
				}
				const Value& Document = Change.Get("textDocument");
				bAny |= AddEdits(Document.Get("uri").AsString(), Change.Get("edits"), Document.Find("version"));
			}
			return bAny;
		}
		const Value& Changes = Edit.Get("changes");
		for (const Value::Member& Member : Changes.Members())
		{
			bAny |= AddEdits(Member.Key, Member.Val, nullptr);
		}
		return bAny;
	}

	void LspClient::ConvertCodeActions(const json::Value& Result, std::vector<LspCodeAction>& Out) const
	{
		for (size_t Index = 0; Index < Result.Size(); ++Index)
		{
			const Value& Item = Result.At(Index);
			if (!Item.IsObject() || Item.Get("command").IsString())
			{
				continue;   // a bare Command: Dream executes no server commands
			}
			LspCodeAction Action;
			Action.Title = Item.Get("title").AsString();
			Action.Kind = Item.Get("kind").AsString();
			Action.bPreferred = Item.Get("isPreferred").AsBool(false);
			const Value& Diagnostics = Item.Get("diagnostics");
			for (size_t D = 0; D < Diagnostics.Size(); ++D)
			{
				Action.Codes.push_back(CodeText(Diagnostics.At(D).Get("code")));
			}
			if (!ConvertWorkspaceEdit(Item.Get("edit"), Action) || Action.Edits.empty())
			{
				continue;
			}
			Out.push_back(std::move(Action));
		}
	}

	void LspClient::Open(LspDocId Doc, const std::string& Uri, const std::string& LanguageId, const TextSnapshot& Text)
	{
		if (Doc == 0)
		{
			return;
		}
		if (Docs.count(Doc) != 0)
		{
			Close(Doc);
		}
		DocState& State = Docs[Doc];
		State.Uri = Uri;
		State.LanguageId = LanguageId;
		State.Current = Text;
		RememberSnapshot(State, Text);
		SendOpen(Doc, State);
	}

	void LspClient::Change(LspDocId Doc, const std::vector<LspDocChange>& Changes)
	{
		const auto It = Docs.find(Doc);
		if (It == Docs.end() || Changes.empty())
		{
			return;
		}
		DocState& State = It->second;
		const TextSnapshot Before = State.Current;
		const TextSnapshot& Newest = Changes.back().After;
		for (const LspDocChange& Change : Changes)
		{
			RememberSnapshot(State, Change.After);
		}
		State.Current = Newest;
		if (CurrentState != LspState::Ready)
		{
			return;     // remembered; opened at its current version once Ready
		}
		// §7.1.5: the size check runs before the didChange is built. UTF-8 is at most 3 bytes per UTF-16 unit.
		const bool bMayBeLarge = Newest.Length() * 3 > Options.MaxDocumentBytes || State.bSkipped;
		const int64_t Bytes = bMayBeLarge ? Utf8Length(Newest) : 0;
		if (State.bSkipped)
		{
			if (Bytes <= Options.MaxDocumentBytes)
			{
				SendOpen(Doc, State);   // dropped below 5 MB: reopened at its current version
			}
			return;
		}
		if (Bytes > Options.MaxDocumentBytes)
		{
			if (State.bOpenOnServer)
			{
				Value Item = Obj();
				Item.Set("uri", Str(State.Uri));
				Value Params = Obj();
				Params.Set("textDocument", std::move(Item));
				Send(JsonRpc::MakeNotification("textDocument/didClose", std::move(Params)));
			}
			State.bOpenOnServer = false;
			State.bSkipped = true;
			SendSkipped(State, Bytes);
			return;
		}
		if (!State.bOpenOnServer)
		{
			return;
		}

		Value ContentChanges = Value::MakeArray();
		bool bFull = ServerFeatures.Sync == LspSyncKind::Full;
		if (!bFull)
		{
			TextSnapshot Prev = Before;
			for (const LspDocChange& Change : Changes)
			{
				if (Change.Changes.IsEmpty())
				{
					Prev = Change.After;
					continue;
				}
				if (Change.Changes.VersionBefore != Prev.Version() || Change.Changes.VersionAfter != Change.After.Version())
				{
					bFull = true;   // a gap in the versions: the whole text is the only safe change
					break;
				}
				const std::vector<ChangeRange>& Ranges = Change.Changes.Ranges;
				std::vector<int64_t> NewAt(Ranges.size());
				int64_t Delta = 0;
				for (size_t Index = 0; Index < Ranges.size(); ++Index)
				{
					NewAt[Index] = Ranges[Index].At + Delta;
					Delta += Ranges[Index].NewLen - Ranges[Index].OldLen;
				}
				// Highest range first: each one's pre-edit coordinates stay valid after the ones above it are applied.
				for (size_t Index = Ranges.size(); Index-- > 0;)
				{
					const ChangeRange& R = Ranges[Index];
					const Offset EndAt = R.At + R.OldLen;
					const LineNo EndLine = Prev.LineOf(EndAt);
					const int32_t EndCol = static_cast<int32_t>(EndAt - Prev.LineStart(EndLine));
					Value Range = Obj();
					Range.Set("start", MakePosition(Prev, static_cast<int32_t>(R.StartLine), R.StartCol16));
					Range.Set("end", MakePosition(Prev, static_cast<int32_t>(EndLine), EndCol));
					std::u16string Inserted;
					Change.After.CopyRange(NewAt[Index], R.NewLen, Inserted);
					Value Item = Obj();
					Item.Set("range", std::move(Range));
					Item.Set("text", Str(ToUtf8(Inserted)));
					ContentChanges.Append(std::move(Item));
				}
				Prev = Change.After;
			}
		}
		if (bFull)
		{
			ContentChanges = Value::MakeArray();
			Value Item = Obj();
			Item.Set("text", Str(Newest.TextUtf8()));
			ContentChanges.Append(std::move(Item));
		}
		if (ContentChanges.Size() == 0)
		{
			return;
		}
		Value Document = Obj();
		Document.Set("uri", Str(State.Uri));
		Document.Set("version", Value::MakeUInt(Newest.Version()));
		Value Params = Obj();
		Params.Set("textDocument", std::move(Document));
		Params.Set("contentChanges", std::move(ContentChanges));
		Send(JsonRpc::MakeNotification("textDocument/didChange", std::move(Params)));
	}

	void LspClient::Close(LspDocId Doc)
	{
		const auto It = Docs.find(Doc);
		if (It == Docs.end())
		{
			return;
		}
		if (It->second.bOpenOnServer && CurrentState == LspState::Ready)
		{
			Value Item = Obj();
			Item.Set("uri", Str(It->second.Uri));
			Value Params = Obj();
			Params.Set("textDocument", std::move(Item));
			Send(JsonRpc::MakeNotification("textDocument/didClose", std::move(Params)));
		}
		Docs.erase(It);
		PendingDiagnostics.erase(Doc);
	}

	void LspClient::Notify(std::string_view Method, json::Value Params)
	{
		if (CurrentState != LspState::Ready)
		{
			return;
		}
		Send(JsonRpc::MakeNotification(Method, std::move(Params)));
	}

	LspRequestId LspClient::Request(std::string_view Method, LspDocId Doc, uint64_t DocVersion, json::Value Params, int64_t TimeoutMs,
		int64_t NowMs)
	{
		if (CurrentState != LspState::Ready)
		{
			return 0;
		}
		const uint32_t Feature = FeatureForMethod(Method);
		if (Feature != 0 && !ServerFeatures.Has(Feature))
		{
			return 0;   // missing features are not routed (T-LSP-017)
		}
		const LspRequestId Id = NextId++;
		PendingRequest Request;
		Request.Method = std::string(Method);
		Request.Doc = Doc;
		Request.DocVersion = DocVersion;
		Request.Deadline = TimeoutMs > 0 ? NowMs + TimeoutMs : 0;
		Pending.emplace(Id, std::move(Request));
		Send(JsonRpc::MakeRequest(Id, Method, std::move(Params)));
		return Id;
	}

	LspRequestId LspClient::RequestCodeActions(LspDocId Doc, const LspRange& Range, const std::vector<LspDiagnostic>& Context, int64_t NowMs)
	{
		const auto It = Docs.find(Doc);
		if (It == Docs.end() || !It->second.bOpenOnServer)
		{
			return 0;
		}
		const DocState& State = It->second;
		auto MakeRange = [&](const LspRange& R)
		{
			Value Out = Obj();
			Out.Set("start", MakePosition(State.Current, R.StartLine, R.StartCol));
			Out.Set("end", MakePosition(State.Current, R.EndLine, R.EndCol));
			return Out;
		};
		Value Diagnostics = Value::MakeArray();
		for (const LspDiagnostic& D : Context)
		{
			Value Item = Obj();
			Item.Set("range", MakeRange(D.Range));
			Item.Set("severity", Value::MakeInt(D.Severity));
			Item.Set("code", Str(D.Code));
			Item.Set("source", Str(D.Source));
			Item.Set("message", Str(D.Message));
			Diagnostics.Append(std::move(Item));
		}
		Value ContextValue = Obj();
		ContextValue.Set("diagnostics", std::move(Diagnostics));
		Value Document = Obj();
		Document.Set("uri", Str(State.Uri));
		Value Params = Obj();
		Params.Set("textDocument", std::move(Document));
		Params.Set("range", MakeRange(Range));
		Params.Set("context", std::move(ContextValue));
		return Request("textDocument/codeAction", Doc, State.Current.Version(), std::move(Params),
			DefaultRequestTimeoutMs("textDocument/codeAction"), NowMs);
	}

	void LspClient::Cancel(LspRequestId Id)
	{
		const auto It = Pending.find(Id);
		if (It == Pending.end())
		{
			return;
		}
		Pending.erase(It);
		Abandoned.insert(Id);
		if (CanSendCancel())
		{
			Value Params = Obj();
			Params.Set("id", Value::MakeInt(Id));
			Send(JsonRpc::MakeNotification("$/cancelRequest", std::move(Params)));
		}
	}

	bool LspClient::CanSendCancel() const
	{
		// Nothing but exit may follow a shutdown request.
		return CurrentState == LspState::Ready || CurrentState == LspState::Stalled;
	}

	void LspClient::NoteUserAction(int64_t NowMs)
	{
		UserActionMs = NowMs;
	}

	void LspClient::Tick(int64_t NowMs)
	{
		if (CurrentState == LspState::Initializing && NowMs - StartMs > Options.InitializeTimeoutMs)
		{
			LspEvent E;
			E.Kind = LspEventKind::ProtocolError;
			E.Text = "initialize timed out after " + std::to_string(Options.InitializeTimeoutMs / 1000) + " s";
			Events.push_back(std::move(E));
			SetState(LspState::Stalled);
			return;
		}
		std::vector<LspRequestId> Expired;
		for (const auto& [Id, Request] : Pending)
		{
			if (Request.Deadline != 0 && Request.Deadline <= NowMs)
			{
				Expired.push_back(Id);
			}
		}
		std::sort(Expired.begin(), Expired.end());
		for (const LspRequestId Id : Expired)
		{
			const auto It = Pending.find(Id);
			LspEvent E;
			E.Kind = LspEventKind::Timeout;
			E.Id = Id;
			E.Method = It->second.Method;
			E.Doc = It->second.Doc;
			Pending.erase(It);
			Abandoned.insert(Id);
			if (CanSendCancel())
			{
				Value Params = Obj();
				Params.Set("id", Value::MakeInt(Id));
				Send(JsonRpc::MakeNotification("$/cancelRequest", std::move(Params)));
			}
			Events.push_back(std::move(E));
			++ConsecutiveTimeouts;
		}
		while (Abandoned.size() > MaxAbandoned)
		{
			Abandoned.erase(Abandoned.begin());
		}
		if (CurrentState == LspState::Ready && ConsecutiveTimeouts >= StallTimeouts && NowMs - LastInputMs >= StallSilenceMs)
		{
			SetState(LspState::Stalled);
		}
	}

	bool LspClient::Shutdown(int64_t NowMs)
	{
		(void)NowMs;
		if (CurrentState != LspState::Ready && CurrentState != LspState::Initializing && CurrentState != LspState::Stalled)
		{
			return false;
		}
		ShutdownId = NextId++;
		Send(JsonRpc::MakeRequest(ShutdownId, "shutdown", Value()));
		SetState(LspState::ShuttingDown);
		return true;
	}

	void LspClient::OnExited()
	{
		Pending.clear();
		SetState(LspState::Exited);
	}

	void LspClient::TakeEvents(std::vector<LspEvent>& Out)
	{
		for (LspEvent& E : Events)
		{
			Out.push_back(std::move(E));
		}
		Events.clear();
		for (auto& [Doc, E] : PendingDiagnostics)
		{
			Out.push_back(std::move(E));
		}
		PendingDiagnostics.clear();
	}

	bool LspClient::IsDocumentOpen(LspDocId Doc) const
	{
		const auto It = Docs.find(Doc);
		return It != Docs.end() && It->second.bOpenOnServer;
	}

	bool LspClient::IsDocumentSkipped(LspDocId Doc) const
	{
		const auto It = Docs.find(Doc);
		return It != Docs.end() && It->second.bSkipped;
	}

	uint64_t LspClient::SentVersion(LspDocId Doc) const
	{
		const auto It = Docs.find(Doc);
		return It == Docs.end() ? 0 : It->second.Current.Version();
	}

	int64_t LspClient::Utf8Length(const TextSnapshot& Text)
	{
		int64_t Bytes = 0;
		const int64_t Length = Text.Length();
		Offset At = 0;
		while (At < Length)
		{
			const std::u16string_view Chunk = Text.Chunk(At, Length - At);
			if (Chunk.empty())
			{
				break;
			}
			for (const char16_t C : Chunk)
			{
				Bytes += C < 0x80 ? 1 : C < 0x800 ? 2 : (C >= 0xD800 && C <= 0xDFFF) ? 2 : 3;
			}
			At += static_cast<Offset>(Chunk.size());
		}
		return Bytes;
	}

	bool MapRangeForward(const TextBuffer& Buffer, uint64_t FromVersion, LspRange& InOutRange)
	{
		if (FromVersion == Buffer.Version())
		{
			return true;
		}
		if (FromVersion > Buffer.Version() || FromVersion < Buffer.OldestMappableVersion())
		{
			return false;
		}
		const LineNo Start = Buffer.MapLine(FromVersion, InOutRange.StartLine);
		const LineNo End = Buffer.MapLine(FromVersion, InOutRange.EndLine);
		if (Start == InvalidLine || End == InvalidLine)
		{
			return false;
		}
		const TextSnapshot Current = Buffer.Snapshot();
		InOutRange.StartLine = static_cast<int32_t>(Start);
		InOutRange.EndLine = static_cast<int32_t>(std::max(Start, End));
		InOutRange.StartCol = std::min(InOutRange.StartCol, Current.LineLength(Start));
		InOutRange.EndCol = std::min(InOutRange.EndCol, Current.LineLength(InOutRange.EndLine));
		if (InOutRange.EndLine == InOutRange.StartLine && InOutRange.EndCol < InOutRange.StartCol)
		{
			InOutRange.EndCol = InOutRange.StartCol;
		}
		return true;
	}

	LspBackoff::Decision LspBackoff::OnCrash(int64_t NowMs)
	{
		ReadySinceMs = -1;
		Crashes.push_back(NowMs);
		Crashes.erase(std::remove_if(Crashes.begin(), Crashes.end(), [NowMs](int64_t At) { return At < NowMs - PauseWindowMs; }),
			Crashes.end());
		Decision Result;
		if (static_cast<int32_t>(Crashes.size()) >= PauseCrashCount)
		{
			bPaused = true;
			Result.bPaused = true;
			return Result;
		}
		Result.DelayMs = Consecutive >= 6 ? MaxDelayMs : std::min(FirstDelayMs << Consecutive, MaxDelayMs);
		++Consecutive;
		return Result;
	}

	void LspBackoff::OnReady(int64_t NowMs)
	{
		ReadySinceMs = NowMs;
	}

	void LspBackoff::OnHealthy(int64_t NowMs)
	{
		if (ReadySinceMs >= 0 && NowMs - ReadySinceMs >= HealthyResetMs)
		{
			Consecutive = 0;
			Crashes.clear();
		}
	}

	void LspBackoff::Reset()
	{
		Crashes.clear();
		Consecutive = 0;
		ReadySinceMs = -1;
		bPaused = false;
	}
}
