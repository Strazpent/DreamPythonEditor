// SPDX-License-Identifier: Apache-2.0
// Tier 1: dream::JsonRpc framing and dream::LspClient protocol state (TDD §7.2, §7.12): T-LSP-001, T-LSP-002, T-LSP-003,
// T-LSP-011, T-LSP-012 (at a tier-1 count), T-LSP-013, T-LSP-014, T-LSP-015, T-LSP-017, T-LSP-019, T-LSP-037,
// T-DIAG-020, plus the §7.1.5 5 MB rule, stall detection, shutdown and code-action conversion.
#include "dream_test.h"
#include "test_util.h"

#include "dream/JsonRpc.h"
#include "dream/LspClient.h"
#include "dream/TextBuffer.h"
#include "dream/Utf.h"

#include <algorithm>
#include <string>
#include <vector>

using dream::json::Value;
using dream_test_util::N;
using dream_test_util::U;
namespace JsonRpc = dream::JsonRpc;

namespace
{
	std::vector<uint8_t> Bytes(std::string_view Text)
	{
		return std::vector<uint8_t>(Text.begin(), Text.end());
	}

	std::string Frame(std::string_view Body, std::string_view ExtraHeader = {})
	{
		return std::string(ExtraHeader) + "Content-Length: " + std::to_string(Body.size()) + "\r\n\r\n" + std::string(Body);
	}

	Value ParseJson(std::string_view Text)
	{
		Value V;
		dream::json::Parse(Text, V);
		return V;
	}

	/** The server's side of the pipe: every frame the client sends, decoded. */
	struct FakeIo final : dream::ILspIo
	{
		JsonRpc::FrameDecoder Decoder;
		std::vector<Value> Sent;

		void SendFrame(std::vector<uint8_t>&& Frame) override
		{
			Decoder.Feed(Frame, Sent);
		}

		std::vector<const Value*> WithMethod(std::string_view Method) const
		{
			std::vector<const Value*> Out;
			for (const Value& M : Sent)
			{
				if (M.Get("method").AsString() == Method)
				{
					Out.push_back(&M);
				}
			}
			return Out;
		}

		const Value* Last(std::string_view Method) const
		{
			const std::vector<const Value*> All = WithMethod(Method);
			return All.empty() ? nullptr : All.back();
		}
	};

	Value Response(int64_t Id, Value Result)
	{
		return JsonRpc::MakeResult(Value::MakeInt(Id), std::move(Result));
	}

	/** Starts a client and answers initialize with Capabilities (JSON text). */
	void Initialize(dream::LspClient& Client, FakeIo& Io, std::string_view Capabilities, int64_t Now = 0)
	{
		Client.Start(4242, "file:///C:/Proj", { { "file:///C:/Proj", "Proj" } }, Now);
		const Value* Init = Io.Last("initialize");
		if (Init == nullptr)
		{
			return;
		}
		Value Result = Value::MakeObject();
		Result.Set("capabilities", ParseJson(Capabilities));
		Client.OnFrame(Response(Init->Get("id").AsInt(), std::move(Result)), Now);
	}

	std::vector<dream::LspEvent> Take(dream::LspClient& Client)
	{
		std::vector<dream::LspEvent> Out;
		Client.TakeEvents(Out);
		return Out;
	}

	size_t CountKind(const std::vector<dream::LspEvent>& Events, dream::LspEventKind Kind)
	{
		size_t Count = 0;
		for (const dream::LspEvent& E : Events)
		{
			Count += E.Kind == Kind ? 1 : 0;
		}
		return Count;
	}

	constexpr std::string_view Utf8Caps =
		R"({"positionEncoding":"utf-8","textDocumentSync":{"openClose":true,"change":2},"codeActionProvider":true})";
	constexpr std::string_view Utf16Caps =
		R"({"textDocumentSync":2,"completionProvider":{},"documentFormattingProvider":true,"codeActionProvider":{}})";

	/** A mirror of one document as the server sees it, applying didOpen and didChange in the negotiated encoding. */
	struct Mirror
	{
		bool bUtf8 = true;
		std::u16string Text;
		int64_t Version = 0;
		bool bOk = true;

		int64_t OffsetOf(const Value& Position) const
		{
			const int64_t Line = Position.Get("line").AsInt();
			const int64_t Character = Position.Get("character").AsInt();
			int64_t Start = 0;
			for (int64_t L = 0; L < Line; ++L)
			{
				const size_t Next = Text.find(u'\n', static_cast<size_t>(Start));
				if (Next == std::u16string::npos)
				{
					return -1;
				}
				Start = static_cast<int64_t>(Next) + 1;
			}
			size_t End = Text.find(u'\n', static_cast<size_t>(Start));
			if (End == std::u16string::npos)
			{
				End = Text.size();
			}
			const std::u16string_view LineText(Text.data() + Start, End - static_cast<size_t>(Start));
			const int64_t Col = bUtf8 ? dream::utf::Utf8ColToUtf16(LineText, static_cast<int32_t>(Character)) : Character;
			return Start + Col;
		}

		void Apply(const Value& Message)
		{
			const std::string& Method = Message.Get("method").AsString();
			const Value& Params = Message.Get("params");
			if (Method == "textDocument/didOpen")
			{
				Text = U(Params.Get("textDocument").Get("text").AsString());
				Version = Params.Get("textDocument").Get("version").AsInt();
				return;
			}
			if (Method != "textDocument/didChange")
			{
				return;
			}
			Version = Params.Get("textDocument").Get("version").AsInt();
			const Value& Changes = Params.Get("contentChanges");
			for (size_t Index = 0; Index < Changes.Size(); ++Index)
			{
				const Value& Change = Changes.At(Index);
				const std::u16string Insert = U(Change.Get("text").AsString());
				if (Change.Find("range") == nullptr)
				{
					Text = Insert;
					continue;
				}
				const int64_t Start = OffsetOf(Change.Get("range").Get("start"));
				const int64_t End = OffsetOf(Change.Get("range").Get("end"));
				if (Start < 0 || End < Start || End > static_cast<int64_t>(Text.size()))
				{
					bOk = false;
					return;
				}
				Text.replace(static_cast<size_t>(Start), static_cast<size_t>(End - Start), Insert);
			}
		}
	};

	/** A random valid batch of 1-3 non-overlapping ranges with UTF-8 multi-byte and astral characters. */
	dream::EditBatch RandomBatch(const dream::TextBuffer& Buffer, dream_test_util::Rng& R)
	{
		static const char16_t* const Pieces[] = { u"a", u"bc", u"\n", u"\u00e9", u"\u20ac", u"\U0001F600", u"x\ny", u"  " };
		const dream::TextSnapshot Snap = Buffer.Snapshot();
		const std::u16string Text = Snap.Text();
		auto Fix = [&Text](int64_t At)
		{
			// Never split a surrogate pair.
			if (At > 0 && At < static_cast<int64_t>(Text.size()) && Text[static_cast<size_t>(At)] >= 0xDC00 &&
				Text[static_cast<size_t>(At)] <= 0xDFFF)
			{
				--At;
			}
			return At;
		};
		dream::EditBatch Batch;
		const int32_t Count = 1 + static_cast<int32_t>(R.Next() % 3);
		int64_t Cursor = 0;
		for (int32_t Index = 0; Index < Count; ++Index)
		{
			const int64_t Remaining = static_cast<int64_t>(Text.size()) - Cursor;
			if (Remaining < 0)
			{
				break;
			}
			const int64_t At = Fix(Cursor + static_cast<int64_t>(R.Next() % static_cast<uint64_t>(Remaining + 1)));
			if (At < Cursor)
			{
				break;
			}
			const int64_t MaxDelete = std::min<int64_t>(static_cast<int64_t>(Text.size()) - At, 4);
			int64_t End = Fix(At + static_cast<int64_t>(R.Next() % static_cast<uint64_t>(MaxDelete + 1)));
			if (End < At)
			{
				End = At;
			}
			dream::EditRange Range;
			Range.At = At;
			Range.DeleteLen = End - At;
			const int32_t Inserts = static_cast<int32_t>(R.Next() % 3);
			for (int32_t I = 0; I < Inserts; ++I)
			{
				Range.Insert += Pieces[R.Next() % (sizeof(Pieces) / sizeof(Pieces[0]))];
			}
			Batch.Ranges.push_back(std::move(Range));
			Cursor = End + 1;
		}
		return Batch;
	}
}

// ---- JsonRpc framing ----------------------------------------------------------------------------------------------------

TEST(JsonRpc_DecoderSplitAtEveryByte)
{
	// T-LSP-001: the same messages whatever the chunking, including a header split inside "\r\n\r\n" and a split inside a
	// multi-byte UTF-8 sequence of the body.
	const std::string Stream = Frame(R"({"jsonrpc":"2.0","id":1,"result":{"a":"\u00e9 caf)" "\xC3\xA9" R"("}})") +
		Frame(R"({"jsonrpc":"2.0","method":"x","params":[1,2,3]})", "Content-Type: application/vscode-jsonrpc; charset=utf-8\r\n") +
		Frame(R"({"jsonrpc":"2.0","id":"s","method":"m","params":{"t":")" "\xF0\x9F\x98\x80" R"("}})");
	const std::vector<uint8_t> All = Bytes(Stream);
	std::vector<Value> Expected;
	JsonRpc::FrameDecoder Whole;
	CHECK(Whole.Feed(All, Expected) == JsonRpc::FrameError::None);
	REQUIRE_EQ(Expected.size(), size_t(3));
	for (size_t Split = 0; Split <= All.size(); ++Split)
	{
		JsonRpc::FrameDecoder Decoder;
		std::vector<Value> Out;
		CHECK(Decoder.Feed(std::span<const uint8_t>(All.data(), Split), Out) == JsonRpc::FrameError::None);
		CHECK(Decoder.Feed(std::span<const uint8_t>(All.data() + Split, All.size() - Split), Out) == JsonRpc::FrameError::None);
		REQUIRE_EQ(Out.size(), Expected.size());
		for (size_t Index = 0; Index < Out.size(); ++Index)
		{
			CHECK_EQ(dream::json::Serialize(Out[Index]), dream::json::Serialize(Expected[Index]));
		}
		CHECK_EQ(Decoder.PendingBytes(), size_t(0));
	}
	JsonRpc::FrameDecoder OneByOne;
	std::vector<Value> Out;
	for (const uint8_t B : All)
	{
		OneByOne.Feed(std::span<const uint8_t>(&B, 1), Out);
	}
	CHECK_EQ(Out.size(), size_t(3));
}

TEST(JsonRpc_DecoderFuzz)
{
	// T-LSP-001 (tier-1 stand-in for the libFuzzer target): random chunking of valid streams decodes the same; random
	// bytes never crash, never exceed the caps, and an error is sticky.
	dream_test_util::Rng R(7);
	const std::string Valid = Frame(R"({"id":1,"result":null})") + Frame(R"({"method":"n"})") + Frame("[1,2,{\"k\":\"v\"}]");
	const std::vector<uint8_t> All = Bytes(Valid);
	for (int32_t Round = 0; Round < 2000; ++Round)
	{
		JsonRpc::FrameDecoder Decoder;
		std::vector<Value> Out;
		size_t At = 0;
		while (At < All.size())
		{
			const size_t Take = std::min<size_t>(All.size() - At, 1 + R.Next() % 17);
			CHECK(Decoder.Feed(std::span<const uint8_t>(All.data() + At, Take), Out) == JsonRpc::FrameError::None);
			At += Take;
		}
		CHECK_EQ(Out.size(), size_t(3));
	}
	JsonRpc::FrameLimits Small;
	Small.MaxHeaderBytes = 256;
	Small.MaxFrameBytes = 4096;
	for (int32_t Round = 0; Round < 3000; ++Round)
	{
		std::vector<uint8_t> Garbage(1 + R.Next() % 600);
		for (uint8_t& B : Garbage)
		{
			const uint64_t Pick = R.Next() % 10;
			B = Pick < 3 ? static_cast<uint8_t>("Content-Length: 12\r\n"[R.Next() % 20]) : static_cast<uint8_t>(R.Next());
		}
		JsonRpc::FrameDecoder Decoder(Small);
		std::vector<Value> Out;
		const JsonRpc::FrameError First = Decoder.Feed(Garbage, Out);
		CHECK(Decoder.PendingBytes() <= Small.MaxHeaderBytes + Small.MaxFrameBytes);
		if (First != JsonRpc::FrameError::None)
		{
			const size_t Before = Out.size();
			CHECK(Decoder.Feed(All, Out) == First);
			CHECK_EQ(Out.size(), Before);
		}
	}
}

TEST(JsonRpc_DecoderErrors)
{
	// T-LSP-002.
	auto ErrorOf = [](std::string_view Input, size_t* OutPending = nullptr)
	{
		JsonRpc::FrameDecoder Decoder;
		std::vector<Value> Out;
		const JsonRpc::FrameError Error = Decoder.Feed(Bytes(Input), Out);
		if (OutPending != nullptr)
		{
			*OutPending = Decoder.PendingBytes();
		}
		return Error;
	};
	CHECK(ErrorOf("Content-Type: x\r\n\r\n{}") == JsonRpc::FrameError::MissingLength);
	CHECK(ErrorOf("Content-Length: -5\r\n\r\n") == JsonRpc::FrameError::BadLength);
	CHECK(ErrorOf("Content-Length: +5\r\n\r\n") == JsonRpc::FrameError::BadLength);
	CHECK(ErrorOf("Content-Length: 1x\r\n\r\n") == JsonRpc::FrameError::BadLength);
	CHECK(ErrorOf("Content-Length: \r\n\r\n") == JsonRpc::FrameError::BadLength);
	size_t Pending = 0;
	CHECK(ErrorOf("Content-Length: 67108865\r\n\r\n", &Pending) == JsonRpc::FrameError::FrameTooLarge);
	CHECK_EQ(Pending, size_t(0));   // refused before any body allocation
	CHECK(ErrorOf("Content-Length: 99999999999999999999999999\r\n\r\n") == JsonRpc::FrameError::FrameTooLarge);
	CHECK(ErrorOf(std::string(9000, 'h')) == JsonRpc::FrameError::HeaderTooLong);
	CHECK(ErrorOf(Frame("{\"a\":")) == JsonRpc::FrameError::BadJson);
	CHECK(ErrorOf(Frame("{} {}")) == JsonRpc::FrameError::BadJson);
	// Exactly at the cap is accepted; the length header is case-insensitive.
	JsonRpc::FrameLimits Limits;
	Limits.MaxFrameBytes = 2;
	JsonRpc::FrameDecoder AtCap(Limits);
	std::vector<Value> Out;
	CHECK(AtCap.Feed(Bytes("content-LENGTH:2\r\n\r\n{}"), Out) == JsonRpc::FrameError::None);
	CHECK_EQ(Out.size(), size_t(1));
	CHECK(AtCap.Feed(Bytes("Content-Length: 3\r\n\r\n"), Out) == JsonRpc::FrameError::FrameTooLarge);
	CHECK(AtCap.Feed(Bytes("Content-Length: 2\r\n\r\n{}"), Out) == JsonRpc::FrameError::FrameTooLarge);   // sticky
	CHECK_EQ(std::string(JsonRpc::FrameErrorName(JsonRpc::FrameError::BadJson)), std::string("BadJson"));
}

TEST(JsonRpc_EncodeFrameUtf8)
{
	// T-LSP-003: the exact UTF-8 byte count, no trailing newline.
	Value M = Value::MakeObject();
	M.Set("t", Value::MakeString("\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80"));
	const std::vector<uint8_t> Frame = JsonRpc::EncodeFrame(M);
	const std::string Text(Frame.begin(), Frame.end());
	const std::string Body = dream::json::Serialize(M);
	CHECK_EQ(Text, "Content-Length: " + std::to_string(Body.size()) + "\r\n\r\n" + Body);
	CHECK_EQ(Body.size(), size_t(8 + 9));     // {"t":"..."}: 8 bytes of syntax and 9 bytes of text
	CHECK(Text.back() == '}');
	JsonRpc::FrameDecoder Decoder;
	std::vector<Value> Out;
	CHECK(Decoder.Feed(Frame, Out) == JsonRpc::FrameError::None);
	REQUIRE_EQ(Out.size(), size_t(1));
	CHECK_EQ(Out[0].Get("t").AsString(), M.Get("t").AsString());
}

TEST(JsonRpc_ClassifyAndUris)
{
	CHECK(JsonRpc::Classify(ParseJson(R"({"jsonrpc":"2.0","id":3,"method":"m"})")) == JsonRpc::MessageKind::Request);
	CHECK(JsonRpc::Classify(ParseJson(R"({"jsonrpc":"2.0","method":"m"})")) == JsonRpc::MessageKind::Notification);
	CHECK(JsonRpc::Classify(ParseJson(R"({"jsonrpc":"2.0","id":3,"result":null})")) == JsonRpc::MessageKind::Response);
	CHECK(JsonRpc::Classify(ParseJson(R"({"jsonrpc":"2.0","id":null,"error":{"code":-32700}})")) == JsonRpc::MessageKind::Response);
	CHECK(JsonRpc::Classify(ParseJson(R"({"jsonrpc":"2.0","id":3})")) == JsonRpc::MessageKind::Invalid);
	CHECK(JsonRpc::Classify(ParseJson("[1]")) == JsonRpc::MessageKind::Invalid);
	CHECK(JsonRpc::Classify(ParseJson(R"({"method":5})")) == JsonRpc::MessageKind::Invalid);

	CHECK_EQ(JsonRpc::PathToFileUri("C:\\Proj\\a b\\x#1.py"), std::string("file:///C:/Proj/a%20b/x%231.py"));
	CHECK_EQ(JsonRpc::PathToFileUri("/home/u/caf\xC3\xA9.py"), std::string("file:///home/u/caf%C3%A9.py"));
	std::string Path;
	CHECK(JsonRpc::FileUriToPath("file:///C:/Proj/a%20b/x%231.py", Path));
	CHECK_EQ(Path, std::string("C:/Proj/a b/x#1.py"));
	CHECK(JsonRpc::FileUriToPath("file:///home/u/caf%C3%A9.py", Path));
	CHECK_EQ(Path, std::string("/home/u/caf\xC3\xA9.py"));
	CHECK(!JsonRpc::FileUriToPath("untitled:x", Path));
	CHECK(!JsonRpc::FileUriToPath("file:///a%2", Path));
	CHECK(JsonRpc::SameFileUri("file:///C:/Proj/x.py", "file:///c%3A/proj/X.py"));
	CHECK(!JsonRpc::SameFileUri("file:///C:/Proj/x.py", "file:///C:/Proj/y.py"));
}

// ---- LspClient ------------------------------------------------------------------------------------------------------------

TEST(LspClient_InitializeAndNegotiate)
{
	// T-LSP-017: UTF-8 when offered; missing features are not routed; documents opened before Ready are opened at Ready.
	FakeIo Io;
	dream::LspClientOptions Options;
	Options.ServerId = "dream-check";
	dream::LspClient Client(Io, Options);
	dream::TextBuffer Buffer(u"print(1)\n");
	Client.Open(7, "file:///C:/Proj/a.py", "python", Buffer.Snapshot());
	CHECK(Io.Sent.empty());
	Initialize(Client, Io, Utf8Caps);
	const Value* Init = Io.Last("initialize");
	REQUIRE(Init != nullptr);
	CHECK_EQ(Init->Get("params").Get("processId").AsInt(), int64_t(4242));
	CHECK_EQ(Init->Get("params").Get("capabilities").Get("general").Get("positionEncodings").At(0).AsString(), std::string("utf-8"));
	CHECK(Init->Get("params").Get("capabilities").Get("workspace").Get("configuration").AsBool());
	CHECK(Client.State() == dream::LspState::Ready);
	CHECK(Client.Features().Encoding == dream::LspPositionEncoding::Utf8);
	CHECK(Client.Features().Has(dream::LspFeature::CodeAction));
	CHECK(!Client.Features().Has(dream::LspFeature::Hover));
	CHECK(Io.Last("initialized") != nullptr);
	const Value* Open = Io.Last("textDocument/didOpen");
	REQUIRE(Open != nullptr);
	CHECK_EQ(Open->Get("params").Get("textDocument").Get("text").AsString(), std::string("print(1)\n"));
	CHECK_EQ(Open->Get("params").Get("textDocument").Get("version").AsUInt(), Buffer.Version());
	CHECK_EQ(Client.Request("textDocument/hover", 7, Buffer.Version(), Value::MakeObject(), 1500, 0), int64_t(0));
	CHECK(Client.Request("textDocument/codeAction", 7, Buffer.Version(), Value::MakeObject(), 1500, 0) != 0);

	FakeIo Io16;
	dream::LspClient Client16(Io16, dream::LspClientOptions());
	Initialize(Client16, Io16, Utf16Caps);
	CHECK(Client16.Features().Encoding == dream::LspPositionEncoding::Utf16);
	CHECK(Client16.Features().Has(dream::LspFeature::Completion));
	CHECK(Client16.Features().Has(dream::LspFeature::Formatting));
	const std::vector<dream::LspEvent> Events = Take(Client16);
	CHECK_EQ(CountKind(Events, dream::LspEventKind::StateChanged), size_t(2));   // Initializing, Ready

	dream::LspClientOptions NoUtf8;
	NoUtf8.bOfferUtf8 = false;
	FakeIo IoNo;
	dream::LspClient ClientNo(IoNo, NoUtf8);
	Initialize(ClientNo, IoNo, Utf8Caps);   // the server claims UTF-8 although it was not offered
	CHECK(ClientNo.Features().Encoding == dream::LspPositionEncoding::Utf16);
}

TEST(LspClient_RandomEditsMirror)
{
	// T-LSP-012 at a tier-1 count (the 1M-edit run is the nightly scale): the server's mirror equals the client's text at
	// every version, in UTF-8 and in UTF-16, with astral characters, several ranges per batch and several batches per frame.
	for (const bool bUtf8 : { true, false })
	{
		FakeIo Io;
		dream::LspClient Client(Io, dream::LspClientOptions());
		Initialize(Client, Io, bUtf8 ? Utf8Caps : Utf16Caps);
		dream::TextBuffer Buffer(u"def f():\n    return 1\n\U0001F600 caf\u00e9\n");
		Client.Open(1, "file:///C:/Proj/m.py", "python", Buffer.Snapshot());
		Mirror M;
		M.bUtf8 = bUtf8;
		M.Apply(*Io.Last("textDocument/didOpen"));
		dream_test_util::Rng R(bUtf8 ? 11 : 13);
		size_t Seen = Io.Sent.size();
		int32_t Applied = 0;
		for (int32_t Frame = 0; Frame < 4000 && M.bOk; ++Frame)
		{
			std::vector<dream::LspDocChange> Changes;
			const int32_t Batches = 1 + static_cast<int32_t>(R.Next() % 3);
			for (int32_t B = 0; B < Batches; ++B)
			{
				const dream::EditBatch Batch = RandomBatch(Buffer, R);
				if (!Buffer.Validate(Batch))
				{
					continue;
				}
				dream::LspDocChange Change;
				Change.Changes = Buffer.Apply(Batch);
				Change.After = Buffer.Snapshot();
				Changes.push_back(std::move(Change));
				++Applied;
			}
			Client.Change(1, Changes);
			for (; Seen < Io.Sent.size(); ++Seen)
			{
				M.Apply(Io.Sent[Seen]);
			}
			CHECK(M.Text == Buffer.Snapshot().Text());
			CHECK_EQ(static_cast<uint64_t>(M.Version), Buffer.Version());
			if (M.Text != Buffer.Snapshot().Text())
			{
				break;
			}
		}
		CHECK(M.bOk);
		CHECK(Applied > 4000);
		CHECK(Io.Last("textDocument/didChange") != nullptr);
		CHECK(Io.Last("textDocument/didChange")->Get("params").Get("contentChanges").At(0).Find("range") != nullptr);
	}
}

TEST(LspClient_FullSync)
{
	// T-LSP-013: a server that wants full sync gets the whole text on each change; a version gap also sends the text.
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, R"({"textDocumentSync":1})");
	CHECK(Client.Features().Sync == dream::LspSyncKind::Full);
	dream::TextBuffer Buffer(u"a\n");
	Client.Open(1, "file:///C:/Proj/f.py", "python", Buffer.Snapshot());
	dream::LspDocChange Change;
	Change.Changes = Buffer.SetText(u"bcd\n", dream::EditOrigin::Command);
	Change.After = Buffer.Snapshot();
	Client.Change(1, { Change });
	const Value* Did = Io.Last("textDocument/didChange");
	REQUIRE(Did != nullptr);
	CHECK_EQ(Did->Get("params").Get("contentChanges").Size(), size_t(1));
	CHECK(Did->Get("params").Get("contentChanges").At(0).Find("range") == nullptr);
	CHECK_EQ(Did->Get("params").Get("contentChanges").At(0).Get("text").AsString(), std::string("bcd\n"));

	FakeIo IoInc;
	dream::LspClient Inc(IoInc, dream::LspClientOptions());
	Initialize(Inc, IoInc, Utf8Caps);
	dream::TextBuffer B2(u"x\n");
	Inc.Open(2, "file:///C:/Proj/g.py", "python", B2.Snapshot());
	B2.SetText(u"skipped\n", dream::EditOrigin::Command);   // a ChangeSet the client never saw
	dream::LspDocChange Gap;
	Gap.Changes = B2.SetText(u"y\n", dream::EditOrigin::Command);
	Gap.After = B2.Snapshot();
	Inc.Change(2, { Gap });
	const Value* GapDid = IoInc.Last("textDocument/didChange");
	REQUIRE(GapDid != nullptr);
	CHECK(GapDid->Get("params").Get("contentChanges").At(0).Find("range") == nullptr);
	CHECK_EQ(GapDid->Get("params").Get("contentChanges").At(0).Get("text").AsString(), std::string("y\n"));
}

TEST(LspClient_DeadlineCancels)
{
	// T-LSP-014: one $/cancelRequest at 1.5 s; Timeout reported; a late reply is dropped.
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf16Caps, 1000);
	Take(Client);
	const dream::LspRequestId Id = Client.Request("textDocument/completion", 0, 0, Value::MakeObject(),
		dream::DefaultRequestTimeoutMs("textDocument/completion"), 1000);
	REQUIRE(Id != 0);
	CHECK_EQ(dream::DefaultRequestTimeoutMs("textDocument/definition"), int64_t(5000));
	Client.Tick(2499);
	CHECK(Io.WithMethod("$/cancelRequest").empty());
	Client.Tick(2500);
	Client.Tick(2600);
	REQUIRE_EQ(Io.WithMethod("$/cancelRequest").size(), size_t(1));
	CHECK_EQ(Io.Last("$/cancelRequest")->Get("params").Get("id").AsInt(), Id);
	std::vector<dream::LspEvent> Events = Take(Client);
	REQUIRE_EQ(CountKind(Events, dream::LspEventKind::Timeout), size_t(1));
	Client.OnFrame(Response(Id, Value::MakeArray()), 2700);
	Events = Take(Client);
	CHECK_EQ(CountKind(Events, dream::LspEventKind::Response), size_t(0));
	CHECK_EQ(Client.PendingRequestCount(), size_t(0));
	// RequestCancelled and ContentModified answers are never shown, but each releases its owner with a Dropped event
	// (review r1: the transport's tag and a quick fix's callback waited forever).
	const dream::LspRequestId Second = Client.Request("textDocument/completion", 0, 0, Value::MakeObject(), 1500, 3000);
	Client.OnFrame(JsonRpc::MakeError(Value::MakeInt(Second), JsonRpc::ErrorCodes::ContentModified, "modified"), 3001);
	const dream::LspRequestId Third = Client.Request("textDocument/completion", 0, 0, Value::MakeObject(), 1500, 3002);
	Client.OnFrame(JsonRpc::MakeError(Value::MakeInt(Third), JsonRpc::ErrorCodes::RequestCancelled, "cancelled"), 3003);
	Events = Take(Client);
	REQUIRE_EQ(Events.size(), size_t(2));
	CHECK(Events[0].Kind == dream::LspEventKind::Dropped);
	CHECK_EQ(Events[0].Id, Second);
	CHECK_EQ(Events[0].ErrorCode, JsonRpc::ErrorCodes::ContentModified);
	CHECK(Events[1].Kind == dream::LspEventKind::Dropped);
	CHECK_EQ(Events[1].Id, Third);
	CHECK_EQ(Client.PendingRequestCount(), size_t(0));
}

TEST(LspClient_NothingButExitAfterShutdown)
{
	// Review r1: a late initialize answer after Shutdown() from Initializing, or after the initialize timeout, must not send
	// initialized or didOpen nor return to Ready; no $/cancelRequest goes out after shutdown.
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Client.Start(1, "file:///C:/Proj", {}, 0);
	dream::TextBuffer Buffer(u"x = 1\n");
	Client.Open(5, "file:///C:/Proj/a.py", "python", Buffer.Snapshot());
	const Value* Init = Io.Last("initialize");
	REQUIRE(Init != nullptr);
	const int64_t InitId = Init->Get("id").AsInt();
	CHECK(Client.Shutdown(10));
	CHECK(Client.State() == dream::LspState::ShuttingDown);
	Value Result = Value::MakeObject();
	Result.Set("capabilities", ParseJson(Utf16Caps));
	Client.OnFrame(Response(InitId, Result), 20);
	CHECK(Client.State() == dream::LspState::ShuttingDown);
	CHECK(Io.Last("initialized") == nullptr);
	CHECK(Io.Last("textDocument/didOpen") == nullptr);

	FakeIo Io2;
	dream::LspClient Timed(Io2, dream::LspClientOptions());
	Timed.Start(1, "file:///C:/Proj", {}, 0);
	const int64_t TimedId = Io2.Last("initialize")->Get("id").AsInt();
	Timed.Tick(30001);
	CHECK(Timed.State() == dream::LspState::Stalled);
	Timed.OnFrame(Response(TimedId, Result), 30100);
	CHECK(Timed.State() == dream::LspState::Stalled);
	CHECK(Io2.Last("initialized") == nullptr);

	FakeIo Io3;
	dream::LspClient Ready(Io3, dream::LspClientOptions());
	Initialize(Ready, Io3, Utf16Caps, 0);
	const dream::LspRequestId Timeout = Ready.Request("textDocument/completion", 0, 0, Value::MakeObject(), 1500, 0);
	const dream::LspRequestId Cancelled = Ready.Request("textDocument/completion", 0, 0, Value::MakeObject(), 0, 0);
	REQUIRE(Timeout != 0);
	REQUIRE(Cancelled != 0);
	CHECK(Ready.Shutdown(100));
	const size_t FramesAtShutdown = Io3.Sent.size();
	Ready.Tick(2000);                       // the deadline passes after shutdown
	Ready.Cancel(Cancelled);
	CHECK(Io3.WithMethod("$/cancelRequest").empty());
	CHECK_EQ(Io3.Sent.size(), FramesAtShutdown);
	CHECK_EQ(CountKind(Take(Ready), dream::LspEventKind::Timeout), size_t(1));   // still reported to its owner
}

TEST(LspClient_ApplyEditOneDocumentOnly)
{
	// T-DIAG-024 at tier 1 (M1 applies an edit to one open document): an applyEdit that names two documents, or an open and
	// a closed one, is answered applied:false with a reason, and nothing is applied (review r1).
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf8Caps);
	dream::TextBuffer A(u"x = 1\n");
	dream::TextBuffer B(u"y = 2\n");
	Client.Open(3, "file:///C:/Proj/a.py", "python", A.Snapshot());
	Client.Open(4, "file:///C:/Proj/b.py", "python", B.Snapshot());
	Take(Client);
	const char* TwoOpen = R"({"jsonrpc":"2.0","id":21,"method":"workspace/applyEdit","params":{"edit":{"changes":{
		"file:///C:/Proj/a.py":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"z"}],
		"file:///C:/Proj/b.py":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"w"}]}}}})";
	Client.NoteUserAction(1000);
	Client.OnFrame(ParseJson(TwoOpen), 1500);
	CHECK(!Io.Sent.back().Get("result").Get("applied").AsBool(true));
	CHECK(!Io.Sent.back().Get("result").Get("failureReason").AsString().empty());
	CHECK_EQ(CountKind(Take(Client), dream::LspEventKind::ApplyEdit), size_t(0));

	const char* OneClosed = R"({"jsonrpc":"2.0","id":22,"method":"workspace/applyEdit","params":{"edit":{"documentChanges":[
		{"textDocument":{"uri":"file:///C:/Proj/a.py","version":null},"edits":[{"range":{"start":{"line":0,"character":0},
		"end":{"line":0,"character":1}},"newText":"z"}]},
		{"textDocument":{"uri":"file:///C:/Proj/closed.py","version":null},"edits":[{"range":{"start":{"line":0,"character":0},
		"end":{"line":0,"character":1}},"newText":"w"}]}]}}})";
	Client.NoteUserAction(2000);
	Client.OnFrame(ParseJson(OneClosed), 2500);
	CHECK(!Io.Sent.back().Get("result").Get("applied").AsBool(true));
	CHECK_EQ(CountKind(Take(Client), dream::LspEventKind::ApplyEdit), size_t(0));

	const char* OneOpen = R"({"jsonrpc":"2.0","id":23,"method":"workspace/applyEdit","params":{"edit":{"changes":{
		"file:///C:/Proj/b.py":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"w"}]}}}})";
	Client.NoteUserAction(3000);
	Client.OnFrame(ParseJson(OneOpen), 3500);
	CHECK(Io.Sent.back().Get("result").Get("applied").AsBool());
	CHECK_EQ(CountKind(Take(Client), dream::LspEventKind::ApplyEdit), size_t(1));
}

TEST(LspClient_StaleRepliesDropped)
{
	// T-LSP-011: completion and formatting answers for an older version are dropped (Stale, the formatting notice).
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf16Caps);
	dream::TextBuffer Buffer(u"a\n");
	Client.Open(1, "file:///C:/Proj/s.py", "python", Buffer.Snapshot());
	const uint64_t V1 = Buffer.Version();
	const dream::LspRequestId Format = Client.Request("textDocument/formatting", 1, V1, Value::MakeObject(), 5000, 0);
	const dream::LspRequestId Complete = Client.Request("textDocument/completion", 1, V1, Value::MakeObject(), 1500, 0);
	const dream::LspRequestId Fresh = Client.Request("textDocument/completion", 1, V1, Value::MakeObject(), 1500, 0);
	Client.OnFrame(Response(Fresh, Value::MakeArray()), 10);
	dream::LspDocChange Change;
	Change.Changes = Buffer.SetText(u"ab\n", dream::EditOrigin::Typing);
	Change.After = Buffer.Snapshot();
	Client.Change(1, { Change });
	Take(Client);
	Client.OnFrame(Response(Format, Value::MakeArray()), 20);
	Client.OnFrame(Response(Complete, Value::MakeArray()), 30);
	const std::vector<dream::LspEvent> Events = Take(Client);
	REQUIRE_EQ(Events.size(), size_t(2));
	CHECK(Events[0].Kind == dream::LspEventKind::Stale);
	CHECK_EQ(Events[0].Method, std::string("textDocument/formatting"));
	CHECK(Events[1].Kind == dream::LspEventKind::Stale);
}

TEST(LspClient_ServerRequests)
{
	// T-LSP-019: configuration per scopeUri; unknown requests get MethodNotFound; an unsolicited applyEdit is refused and
	// one within 5 s of a user-picked action is accepted.
	FakeIo Io;
	dream::LspClientOptions Options;
	Options.Configuration = [](std::string_view ScopeUri, std::string_view Section)
	{
		Value V = Value::MakeObject();
		V.Set("oneUndoStepPerRun", Value::MakeBool(ScopeUri.find("scratch") == std::string_view::npos));
		V.Set("section", Value::MakeString(std::string(Section)));
		return V;
	};
	dream::LspClient Client(Io, Options);
	Initialize(Client, Io, Utf8Caps);
	dream::TextBuffer Buffer(u"x = 1\n");
	Client.Open(3, "file:///C:/Proj/a.py", "python", Buffer.Snapshot());
	Client.OnFrame(ParseJson(R"({"jsonrpc":"2.0","id":"c1","method":"workspace/configuration","params":{"items":[
		{"scopeUri":"file:///C:/Proj/a.py","section":"dream"},{"scopeUri":"file:///C:/Proj/scratch/b.py","section":"dream"}]}})"), 0);
	const Value& Config = Io.Sent.back();
	CHECK_EQ(Config.Get("id").AsString(), std::string("c1"));
	CHECK(Config.Get("result").At(0).Get("oneUndoStepPerRun").AsBool());
	CHECK(!Config.Get("result").At(1).Get("oneUndoStepPerRun").AsBool(true));
	CHECK_EQ(Config.Get("result").At(1).Get("section").AsString(), std::string("dream"));

	Client.OnFrame(ParseJson(R"({"jsonrpc":"2.0","id":9,"method":"custom/thing","params":{}})"), 0);
	CHECK_EQ(Io.Sent.back().Get("error").Get("code").AsInt(), JsonRpc::ErrorCodes::MethodNotFound);
	Client.OnFrame(ParseJson(R"({"jsonrpc":"2.0","id":10,"method":"client/registerCapability","params":{}})"), 0);
	CHECK(Io.Sent.back().Find("result") != nullptr);

	const char* ApplyEdit = R"({"jsonrpc":"2.0","id":11,"method":"workspace/applyEdit","params":{"edit":{"changes":{
		"file:///C:/Proj/a.py":[{"range":{"start":{"line":0,"character":0},"end":{"line":0,"character":1}},"newText":"y"}]}}}})";
	Client.OnFrame(ParseJson(ApplyEdit), 1000);
	CHECK(!Io.Sent.back().Get("result").Get("applied").AsBool(true));
	CHECK_EQ(CountKind(Take(Client), dream::LspEventKind::ApplyEdit), size_t(0));
	Client.NoteUserAction(2000);
	Client.OnFrame(ParseJson(ApplyEdit), 8000);   // 6 s later: refused
	CHECK(!Io.Sent.back().Get("result").Get("applied").AsBool(true));
	Client.NoteUserAction(9000);
	Client.OnFrame(ParseJson(ApplyEdit), 9500);
	CHECK(Io.Sent.back().Get("result").Get("applied").AsBool());
	const std::vector<dream::LspEvent> Events = Take(Client);
	REQUIRE_EQ(CountKind(Events, dream::LspEventKind::ApplyEdit), size_t(1));
	for (const dream::LspEvent& E : Events)
	{
		if (E.Kind == dream::LspEventKind::ApplyEdit)
		{
			CHECK_EQ(E.Actions[0].Doc, dream::LspDocId(3));
			CHECK(E.Actions[0].Edits[0].NewText == u"y");
		}
	}
}

TEST(LspClient_DiagnosticsCoalescedAndConverted)
{
	// T-LSP-037 and §7.2.3: only the newest set per document waits; UTF-8 columns become UTF-16; a set with no version
	// refers to the newest version sent; unknown URIs are dropped.
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf8Caps);
	dream::TextBuffer Buffer(U("s = '\xF0\x9F\x98\x80\xC3\xA9' ; unreal.Foo\n"));
	Client.Open(5, "file:///C:/Proj/d.py", "python", Buffer.Snapshot());
	Take(Client);
	auto Publish = [&](int64_t StartByte, const char* Code, bool bVersion)
	{
		std::string Text = R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///C:/Proj/d.py",)";
		if (bVersion)
		{
			Text += "\"version\":" + std::to_string(Buffer.Version()) + ",";
		}
		Text += R"("diagnostics":[{"range":{"start":{"line":0,"character":)" + std::to_string(StartByte) +
			R"(},"end":{"line":0,"character":)" + std::to_string(StartByte + 3) + R"(}},"severity":2,"code":")" + Code +
			R"(","source":"dream-check","message":"m","data":{"fixable":true}}]}})";
		Client.OnFrame(ParseJson(Text), 0);
	};
	Publish(0, "SUM101", true);
	Publish(15, "SUM102", false);     // byte 15 = after "s = '" (5) + emoji (4) + e-acute (2) + "' ; " (4)
	Client.OnFrame(ParseJson(R"({"jsonrpc":"2.0","method":"textDocument/publishDiagnostics","params":{"uri":"file:///C:/Other.py","diagnostics":[]}})"), 0);
	const std::vector<dream::LspEvent> Events = Take(Client);
	REQUIRE_EQ(Events.size(), size_t(1));
	const dream::LspEvent& E = Events[0];
	CHECK(E.Kind == dream::LspEventKind::Diagnostics);
	CHECK_EQ(E.Doc, dream::LspDocId(5));
	CHECK(E.bHasVersion);
	CHECK_EQ(E.Version, Buffer.Version());
	REQUIRE_EQ(E.Diagnostics.size(), size_t(1));
	CHECK_EQ(E.Diagnostics[0].Code, std::string("SUM102"));
	CHECK_EQ(E.Diagnostics[0].Range.StartCol, int32_t(5 + 2 + 1 + 4));   // UTF-16: emoji is 2 units, e-acute 1
	CHECK_EQ(E.Diagnostics[0].Severity, int32_t(2));
	CHECK(E.Diagnostics[0].bFixable);
	Client.Close(5);
	Publish(0, "SUM101", true);
	CHECK(Take(Client).empty());
}

TEST(LspClient_SkipsLargeDocuments)
{
	// §7.1.5 and T-LSP-045's client part: over 5 MB -> dream/skipped, never the text; below again -> didOpen at the current
	// version; crossing up -> didClose and dream/skipped.
	FakeIo Io;
	dream::LspClientOptions Options;
	Options.MaxDocumentBytes = 100;
	Options.bSendSkippedNotification = true;
	dream::LspClient Client(Io, Options);
	Initialize(Client, Io, Utf8Caps);
	dream::TextBuffer Buffer(std::u16string(60, u'\u00e9'));    // 120 UTF-8 bytes
	Client.Open(1, "file:///C:/Proj/big.py", "python", Buffer.Snapshot());
	CHECK(Io.Last("textDocument/didOpen") == nullptr);
	REQUIRE(Io.Last("dream/skipped") != nullptr);
	CHECK_EQ(Io.Last("dream/skipped")->Get("params").Get("bytes").AsInt(), int64_t(120));
	CHECK(Client.IsDocumentSkipped(1));
	dream::LspDocChange Shrink;
	Shrink.Changes = Buffer.SetText(u"small\n", dream::EditOrigin::Undo);
	Shrink.After = Buffer.Snapshot();
	Client.Change(1, { Shrink });
	REQUIRE(Io.Last("textDocument/didOpen") != nullptr);
	CHECK_EQ(Io.Last("textDocument/didOpen")->Get("params").Get("textDocument").Get("version").AsUInt(), Buffer.Version());
	dream::LspDocChange Grow;
	Grow.Changes = Buffer.SetText(std::u16string(101, u'x'), dream::EditOrigin::Command);
	Grow.After = Buffer.Snapshot();
	Client.Change(1, { Grow });
	CHECK(Io.Last("textDocument/didClose") != nullptr);
	CHECK_EQ(Io.WithMethod("dream/skipped").size(), size_t(2));
	CHECK(Io.Last("textDocument/didChange") == nullptr);
	CHECK_EQ(dream::LspClient::Utf8Length(dream::TextBuffer(U("a\xC3\xA9\xE2\x82\xAC\xF0\x9F\x98\x80")).Snapshot()), int64_t(10));
}

TEST(LspClient_StallAndShutdown)
{
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf16Caps, 0);
	for (int32_t Index = 0; Index < 3; ++Index)
	{
		Client.Request("textDocument/completion", 0, 0, Value::MakeObject(), 1500, 0);
	}
	Client.Tick(1600);
	CHECK(Client.State() == dream::LspState::Ready);     // 3 timeouts, but stdout spoke 1.6 s ago only
	Client.Tick(10001);
	CHECK(Client.State() == dream::LspState::Stalled);

	FakeIo Io2;
	dream::LspClient Second(Io2, dream::LspClientOptions());
	Second.Start(1, "", {}, 0);
	Second.Tick(30001);
	CHECK(Second.State() == dream::LspState::Stalled);    // initialize timeout (§7.3.5)
	std::vector<dream::LspEvent> Events = Take(Second);
	CHECK_EQ(CountKind(Events, dream::LspEventKind::ProtocolError), size_t(1));

	FakeIo Io3;
	dream::LspClient Third(Io3, dream::LspClientOptions());
	Initialize(Third, Io3, Utf16Caps);
	CHECK(Third.Shutdown(0));
	const Value* Shutdown = Io3.Last("shutdown");
	REQUIRE(Shutdown != nullptr);
	CHECK(Io3.Last("exit") == nullptr);
	Third.OnFrame(Response(Shutdown->Get("id").AsInt(), Value()), 1);
	CHECK(Io3.Last("exit") != nullptr);
	CHECK(Third.IsShutdownAnswered());
	Third.OnExited();
	CHECK(Third.State() == dream::LspState::Exited);
	CHECK(!Third.Shutdown(2));
}

TEST(LspClient_CodeActionsConverted)
{
	FakeIo Io;
	dream::LspClient Client(Io, dream::LspClientOptions());
	Initialize(Client, Io, Utf8Caps);
	dream::TextBuffer Buffer(U("caf\xC3\xA9.lod_group = 1\n"));
	Client.Open(4, "file:///C:/Proj/q.py", "python", Buffer.Snapshot());
	Take(Client);
	dream::LspDiagnostic Context;
	Context.Range = { 0, 0, 0, 14 };
	Context.Code = "SUM102";
	const dream::LspRequestId Id = Client.RequestCodeActions(4, { 0, 0, 0, 0 }, { Context }, 0);
	REQUIRE(Id != 0);
	const Value* Request = Io.Last("textDocument/codeAction");
	REQUIRE(Request != nullptr);
	CHECK_EQ(Request->Get("params").Get("context").Get("diagnostics").At(0).Get("range").Get("end").Get("character").AsInt(), int64_t(15));
	const std::string Reply = R"([{"title":"Use set_editor_property","kind":"quickfix","isPreferred":true,
		"diagnostics":[{"code":"SUM102"}],"edit":{"documentChanges":[{"textDocument":{"uri":"file:///C:/Proj/q.py","version":)" +
		std::to_string(Buffer.Version()) + R"J(},"edits":[{"range":{"start":{"line":0,"character":5},"end":{"line":0,"character":19}},
		"newText":"set_editor_property(\"lod_group\", 1)"}]}]}},{"title":"cmd","command":"x"}])J";
	Client.OnFrame(Response(Id, ParseJson(Reply)), 5);
	const std::vector<dream::LspEvent> Events = Take(Client);
	REQUIRE_EQ(Events.size(), size_t(1));
	REQUIRE_EQ(Events[0].Actions.size(), size_t(1));
	const dream::LspCodeAction& Action = Events[0].Actions[0];
	CHECK(Action.bPreferred);
	CHECK(Action.bHasVersion);
	CHECK_EQ(Action.Version, Buffer.Version());
	CHECK_EQ(Action.Codes.at(0), std::string("SUM102"));
	REQUIRE_EQ(Action.Edits.size(), size_t(1));
	CHECK_EQ(Action.Edits[0].Range.StartCol, int32_t(4));
	CHECK_EQ(Action.Edits[0].Range.EndCol, int32_t(18));
	CHECK(Action.Edits[0].NewText == u"set_editor_property(\"lod_group\", 1)");
}

TEST(LspBackoff_Sequence)
{
	// T-LSP-015: 1, 2, 4 ... 60 s; Paused after 5 crashes in 180 s; reset after 300 s healthy.
	dream::LspBackoff Backoff;
	const int64_t Expected[] = { 1000, 2000, 4000, 8000, 16000, 32000, 60000, 60000 };
	int64_t Now = 0;
	for (const int64_t Delay : Expected)
	{
		const dream::LspBackoff::Decision D = Backoff.OnCrash(Now);
		CHECK(!D.bPaused);
		CHECK_EQ(D.DelayMs, Delay);
		Now += 50000;   // 4 crashes at most inside any 180 s window
	}
	dream::LspBackoff Fast;
	for (int32_t Index = 0; Index < 4; ++Index)
	{
		CHECK(!Fast.OnCrash(Index * 10000).bPaused);
	}
	CHECK(Fast.OnCrash(179000).bPaused);
	CHECK(Fast.IsPaused());
	Fast.Reset();
	CHECK(!Fast.IsPaused());
	CHECK_EQ(Fast.OnCrash(200000).DelayMs, int64_t(1000));

	dream::LspBackoff Healthy;
	Healthy.OnCrash(0);
	Healthy.OnCrash(1000);
	Healthy.OnReady(5000);
	Healthy.OnHealthy(304999);
	CHECK_EQ(Healthy.RestartCount(), int32_t(2));
	Healthy.OnHealthy(305000);
	CHECK_EQ(Healthy.RestartCount(), int32_t(0));
	CHECK_EQ(Healthy.OnCrash(306000).DelayMs, int64_t(1000));
}

TEST(LspClient_MapRangeForward)
{
	// T-DIAG-020: old-version diagnostics are mapped through the edit log, dropped beyond it or on a deleted line.
	dream::TextBuffer Buffer(u"a = 1\nb = 2\nc = 3\n");
	const uint64_t V1 = Buffer.Version();
	dream::EditBatch Insert;
	Insert.Ranges.push_back({ 0, 0, u"# head\n# more\n" });
	Buffer.Apply(Insert);
	dream::LspRange Range{ 2, 0, 2, 5 };
	CHECK(dream::MapRangeForward(Buffer, V1, Range));
	CHECK_EQ(Range.StartLine, int32_t(4));
	CHECK_EQ(Range.EndCol, int32_t(5));
	const uint64_t V2 = Buffer.Version();
	dream::EditBatch Delete;
	const dream::TextSnapshot Snap = Buffer.Snapshot();
	Delete.Ranges.push_back({ Snap.LineStart(3), Snap.LineStart(4) - Snap.LineStart(3), u"" });     // line "b = 2"
	Buffer.Apply(Delete);
	dream::LspRange Deleted{ 3, 0, 3, 1 };
	CHECK(!dream::MapRangeForward(Buffer, V2, Deleted));
	Buffer.SetHistoryLimit(1);
	Buffer.Apply(Insert);
	Buffer.Apply(Insert);
	dream::LspRange Old{ 0, 0, 0, 1 };
	CHECK(!dream::MapRangeForward(Buffer, V1, Old));
	dream::LspRange Same{ 0, 0, 0, 1 };
	CHECK(dream::MapRangeForward(Buffer, Buffer.Version(), Same));
}
