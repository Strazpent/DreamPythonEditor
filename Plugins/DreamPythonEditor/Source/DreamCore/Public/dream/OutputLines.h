// SPDX-License-Identifier: Apache-2.0
// Dream core: output ingest helpers (TDD §6.1 "line splitting", §6.2 helpers). Pure, tier 1 (T-OUT-002, T-OUT-004).
// Used on the DreamOutput ingest thread; each LineSplitter belongs to one (native thread, stream, run) key.
#pragma once

#include "dream/OutputRing.h"

#include <string>
#include <string_view>
#include <vector>

namespace dream
{
	/** A committed line from a LineSplitter. */
	struct SplitLine
	{
		std::string Text;              // UTF-8, terminator removed
		uint16_t Flags = 0;            // OLF_Partial, OLF_Continued, or OLF_Partial | OLF_EchoSplit
	};

	/**
	 * The §6.1 line-splitting state machine for one key:
	 *  - "\n" and "\r\n" end a line; empty lines are kept;
	 *  - a lone "\r" discards the open partial text (progress-bar redraw);
	 *  - a "\r" at the end of a write is held: a following write that starts with "\n" makes the pair a line end,
	 *    anything else makes it a carriage return; at Flush a held "\r" ends the line;
	 *  - a partial line is committed at PartialCommitBytes (flag Partial);
	 *  - lines over ContinuationBytes are split into continuation records (flag Continued).
	 * (slice A choice) The 16 KB rule applies to text still open when a write ends; a line that is longer inside one
	 * write is split at 64 KB. Records after the first piece of a line carry Continued; a terminator that arrives after
	 * a committed piece ends that line without an empty record. Splits never cut a UTF-8 sequence.
	 */
	class DREAMCORE_API LineSplitter
	{
	public:
		static constexpr size_t PartialCommitBytes = 16u << 10;
		static constexpr size_t ContinuationBytes = 64u << 10;

		/** Feeds one write; appends every completed line to Out. */
		void Feed(std::string_view Utf8, std::vector<SplitLine>& Out);

		/**
		 * Commits the open partial text (and a held "\r") as a line flagged Partial; nothing when there is none. Run end
		 * (bBeforeEcho false), and before an Echo record of the run (bBeforeEcho true: flagged Partial | EchoSplit) so
		 * statement k's unterminated print stays above statement k+1's echo while the Output Log still gets it joined with
		 * the rest of its line (§6.1 "Partial lines and input echo", §6.3 rule 1).
		 */
		void Flush(std::vector<SplitLine>& Out, bool bBeforeEcho = false);

		bool HasPartial() const;
		/** The open partial text, shown as a provisional last line of its block (§6.1). */
		std::string_view PartialText() const;

	private:
		void Commit(std::vector<SplitLine>& Out, uint16_t Flags);
		void Append(std::string_view Text, std::vector<SplitLine>& Out);

		std::string Partial;
		bool bHeldCr = false;
		bool bContinuing = false;      // the open line's head was already committed (16 KB partial, 64 KB split)
	};

	/**
	 * Finds link spans in one UTF-8 line (§6.2 ScanLinks): `File "<path>", line N` (real paths and <dream:...> pseudo files)
	 * and `<path>(N)` / `<path>(N,C)`. Hand-written scanner, no std::regex. Appends to Out; returns true when any found.
	 */
	DREAMCORE_API bool ScanLinks(std::string_view Utf8Line, std::vector<LinkSpan>& Out);

	/** Parses a Dream pseudo filename "<dream:Tab Name#id>" (RUN-06); OutName is "Tab Name", OutId the hex id or "N". */
	DREAMCORE_API bool ParsePseudoFilename(std::string_view Filename, std::string_view& OutName, std::string_view& OutId);
}
