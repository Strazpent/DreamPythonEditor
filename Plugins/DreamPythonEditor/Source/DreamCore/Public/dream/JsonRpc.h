// SPDX-License-Identifier: Apache-2.0
// Dream core: JSON-RPC 2.0 over LSP's Content-Length framing (TDD §7.2.1, §7.2.2, §7.2.6, D4, D7). Pure: no I/O, no
// threads, no engine. FrameDecoder is a state machine over byte chunks; its errors are fatal for the instance, because
// LSP has no resync marker (the supervisor restarts the server, §7.3.5). Any thread; one decoder per stream.
// Tier 1: Tests/DreamCore/test_jsonrpc.cpp (T-LSP-001, T-LSP-002, T-LSP-003).
#pragma once

#include "dream/Export.h"
#include "dream/Json.h"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace dream::JsonRpc
{
	/** §7.2.2: every decoder error is fatal for that server instance. */
	enum class FrameError : uint8_t
	{
		None,
		MissingLength,      // the header block has no Content-Length
		BadLength,          // empty, negative, signed or non-decimal Content-Length
		HeaderTooLong,      // no "\r\n\r\n" within MaxHeaderBytes
		FrameTooLarge,      // Content-Length above MaxFrameBytes (refused before any body allocation)
		BadJson,            // the body is not one JSON value
	};

	DREAMCORE_API const char* FrameErrorName(FrameError Error);

	/** §7.1.4 caps (proposed): frame <= 64 MB, header <= 8 KB. */
	struct FrameLimits
	{
		size_t MaxHeaderBytes = 8u * 1024u;
		size_t MaxFrameBytes = size_t(64) << 20;
		size_t MaxJsonDepth = 256;
	};

	class DREAMCORE_API FrameDecoder
	{
	public:
		explicit FrameDecoder(const FrameLimits& InLimits = FrameLimits());

		/**
		 * Consumes Bytes; every complete frame is parsed and appended to Out, in order. Returns None, or the first error,
		 * which is sticky: later calls return it at once and decode nothing (frames completed before the error stay in
		 * Out). Content-Type and unknown header fields are ignored; header names compare case-insensitively.
		 */
		FrameError Feed(std::span<const uint8_t> Bytes, std::vector<json::Value>& Out);

		FrameError GetError() const { return Error; }
		/** Bytes held for an incomplete header or body (bounded by the caps). */
		size_t PendingBytes() const { return Header.size() + Body.size(); }
		void Reset();

	private:
		FrameError ParseHeader();

		FrameLimits Limits;
		FrameError Error = FrameError::None;
		bool bInBody = false;
		size_t BodyLength = 0;
		std::string Header;
		std::string Body;
	};

	/** §7.2.2: "Content-Length: <UTF-8 bytes>\r\n\r\n<body>" as one buffer, with no trailing newline. */
	DREAMCORE_API std::vector<uint8_t> EncodeFrame(const json::Value& Message);

	enum class MessageKind : uint8_t
	{
		Request,            // "method" and "id"
		Notification,       // "method", no "id"
		Response,           // "id" with "result" or "error", no "method"
		Invalid,
	};

	DREAMCORE_API MessageKind Classify(const json::Value& Message);

	/** JSON-RPC and LSP error codes used by Dream. */
	namespace ErrorCodes
	{
		inline constexpr int64_t ParseError = -32700;
		inline constexpr int64_t InvalidRequest = -32600;
		inline constexpr int64_t MethodNotFound = -32601;
		inline constexpr int64_t InvalidParams = -32602;
		inline constexpr int64_t InternalError = -32603;
		inline constexpr int64_t ServerNotInitialized = -32002;
		inline constexpr int64_t RequestCancelled = -32800;
		inline constexpr int64_t ContentModified = -32801;
	}

	DREAMCORE_API json::Value MakeRequest(int64_t Id, std::string_view Method, json::Value Params);
	DREAMCORE_API json::Value MakeNotification(std::string_view Method, json::Value Params);
	DREAMCORE_API json::Value MakeResult(const json::Value& Id, json::Value Result);
	DREAMCORE_API json::Value MakeError(const json::Value& Id, int64_t Code, std::string_view Message);

	/**
	 * file: URIs (§7.2.3). PathToFileUri takes a UTF-8 absolute path with '/' or '\' separators ("C:/Proj/a b.py") and
	 * returns "file:///C:/Proj/a%20b.py" (RFC 3986 unreserved characters and '/', ':' after the drive kept; every other
	 * byte percent-encoded). FileUriToPath reverses it ('/' separators); false for a non-file URI or bad escapes.
	 */
	DREAMCORE_API std::string PathToFileUri(std::string_view PathUtf8);
	DREAMCORE_API bool FileUriToPath(std::string_view Uri, std::string& OutPathUtf8);
	/** True when two file URIs name the same path: decoded, separators unified, ASCII case folded (Windows drives and
	 *  paths compare case-insensitively; servers may re-encode or lower-case the drive letter). */
	DREAMCORE_API bool SameFileUri(std::string_view A, std::string_view B);
}
