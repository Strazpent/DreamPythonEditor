// SPDX-License-Identifier: Apache-2.0
// Dream core: the output store's bounded, typed, indexed line history (TDD §6.2 "dream::OutputRing", D19, OUT-04).
// Threads: Append, ClearBefore and GetLine on the game thread (FDreamOutputStore::Drain); an OutputSnapshot may be
// read on any thread without locks (filter and search workers, §6.2).
#pragma once

#include "dream/Export.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

namespace dream
{
	/** Line types (D19, §6.2). The first eleven equal the _dream STREAM_* constants 1..11 (§6.1, §12.1.1). */
	enum class LineType : uint8_t
	{
		Stdout = 1,
		Stderr,
		Result,
		Notice,
		Info,
		Warning,
		Error,
		Agent,
		Debugger,
		Logpoint,
		Evaluate,
		RunHeader,                     // C++ only
		RunFooter,                     // C++ only
		Echo,                          // C++ only: input echo (OUT-12)
	};

	inline constexpr uint8_t FirstStreamLineType = 1;
	inline constexpr uint8_t LastStreamLineType = 11;

	/** Line flags (§6.2). */
	enum OutputLineFlag : uint16_t
	{
		OLF_Background    = 1 << 0,
		OLF_Partial       = 1 << 1,    // committed without a terminator (run end, 16 KB, before an echo)
		OLF_Continued     = 1 << 2,    // continuation of a line over 64 KB
		OLF_Truncated     = 1 << 3,    // a result cut at 10,000 characters; the rest is a blob in the store
		OLF_HasLinks      = 1 << 4,
		OLF_FromUnrealLog = 1 << 5,    // the unreal.log* shims
		OLF_FromLogPython = 1 << 6,    // captured by FDreamOutputDevice
		OLF_EchoFirst     = 1 << 7,    // first echo line of a statement
		OLF_ReplPrompt    = 1 << 8,    // (slice A choice) echo of a REPL or console run: the run log writes '>>>' / '...', not a
		                               // line number (§6.2); the history view draws no prompt (A226)
		OLF_EchoSplit     = 1 << 9,    // a partial line committed before the next statement's echo (§6.1); with OLF_Partial.
		                               // The forwarder joins it with the next line of its stream and run (§6.3 rule 1)
		OLF_PromptFirst   = 1 << 10,   // a REPL or console echo line that is its statement's first line: '>>>' in the run log
		OLF_ResultCont    = 1 << 11,   // a result line after the first line of its repr: no '# Result: ' decoration (§6.4)
	};

	enum class LinkKind : uint8_t
	{
		PythonFrame,                   // File "<path>", line N
		PseudoFile,                    // File "<dream:...>", line N
		FileLine,                      // <path>(N) or <path>(N,C)
	};

	/** A clickable span in a line; byte offsets into the line's UTF-8 text (§6.2 link spans). */
	struct LinkSpan
	{
		uint32_t Start = 0;
		uint32_t Len = 0;
		LinkKind Kind = LinkKind::PythonFrame;
		int32_t Line = 0;              // 1-based
		int32_t Col = 0;               // 1-based; 0 when the link has no column
		uint32_t PathStart = 0;        // the path's bytes inside the line
		uint32_t PathLen = 0;
	};

	/** A run-length style span from SGR parsing (OUT-03, M2). Slice A stores none. */
	struct StyleSpan
	{
		uint32_t Start = 0;
		uint32_t Len = 0;
		uint32_t Style = 0;            // packed SGR attributes
	};

	/** Per-line metadata given to Append and returned by GetLine. */
	struct LineInfo
	{
		uint32_t RunNumber = 0;        // the run's #N resolved at ingest; 0 = outside runs
		LineType Type = LineType::Stdout;
		uint16_t Flags = 0;            // OutputLineFlag bits
		uint32_t ThreadTag = 0;        // index into the store's thread-name table; 0 = game thread
		uint32_t TimeMs = 0;           // (slice A choice) milliseconds since the editor session started
		int32_t EchoLine = 0;          // Echo lines: the 1-based buffer line (stored in the link-span field, §6.2)
	};

	/** One stored line. The views stay valid while the line is not evicted or cleared (they reference its chunk). */
	struct LineView
	{
		uint64_t Seq = 0;
		LineInfo Info;
		std::string_view Text;         // UTF-8, no terminator
		std::span<const StyleSpan> Styles;
		std::span<const LinkSpan> Links;
	};

	struct OutputRingConfig
	{
		uint32_t MaxLines = 200000;    // D19
		uint64_t MaxBytes = 64ull << 20; // OUT-04
		uint32_t ChunkBytes = 64u << 10; // R05 §2.3
	};

	/** Reported when the oldest chunk is dropped (§6.2 caps), so the view can draw the eviction marker. */
	struct EvictionInfo
	{
		uint64_t FirstEvicted = 0;
		uint64_t LastEvicted = 0;      // inclusive
		std::vector<uint32_t> RunNumbers; // distinct run numbers that lost lines, ascending
	};

	class OutputRing;

	/** An immutable view of a ring range, readable on any thread (holds chunk references). */
	class DREAMCORE_API OutputSnapshot
	{
	public:
		OutputSnapshot();
		uint64_t FirstSeq() const;
		uint64_t EndSeq() const;       // one past the last line
		bool GetLine(uint64_t Seq, LineView& Out) const;

	private:
		friend class OutputRing;
		struct Data;
		std::shared_ptr<const Data> State;
	};

	class DREAMCORE_API OutputRing
	{
	public:
		explicit OutputRing(const OutputRingConfig& Config = OutputRingConfig());
		~OutputRing();
		OutputRing(const OutputRing&) = delete;
		OutputRing& operator=(const OutputRing&) = delete;

		/**
		 * Appends one line and returns its sequence number (monotonic, never reused). Text over ChunkBytes is the
		 * caller's bug (ingest splits at 64 KB, §6.1) and is cut. May evict the oldest sealed chunk first.
		 */
		uint64_t Append(const LineInfo& Info, std::string_view Utf8, std::span<const StyleSpan> Styles, std::span<const LinkSpan> Links);

		/** O(1) lookup (§6.2 line index). False when Seq is evicted, cleared or not yet appended. */
		bool GetLine(uint64_t Seq, LineView& Out) const;

		uint64_t FirstSeq() const;
		uint64_t EndSeq() const;
		uint32_t LineCount() const;
		uint64_t TextBytes() const;

		OutputSnapshot TakeSnapshot() const;

		/** Drops every line before Seq (Clear keeps the active run's header by passing its Seq, §6.4). */
		void ClearBefore(uint64_t Seq);

		void SetEvictionListener(std::function<void(const EvictionInfo&)> Listener);

	private:
		struct Impl;
		std::unique_ptr<Impl> State;
	};
}
