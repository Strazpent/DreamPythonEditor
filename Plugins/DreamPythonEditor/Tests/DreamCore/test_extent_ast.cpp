// SPDX-License-Identifier: Apache-2.0
// Tier 1: T-CORE-039, StatementExtent against the engine interpreter's ast on its own Lib/**/*.py. build.bat writes the
// golden rows with extent_golden.py and names them in DREAM_EXTENT_GOLDEN; without it the test reports a skip.
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"

#include "dream/StatementExtent.h"
#include "dream/TextCodec.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <string>
#include <vector>

namespace
{
	struct GoldenRow
	{
		int32_t Caret = 0;
		int32_t First = 0;
		int32_t Last = 0;
	};

	bool ParseInt(std::string_view S, int32_t& Out)
	{
		const auto R = std::from_chars(S.data(), S.data() + S.size(), Out);
		return R.ec == std::errc() && R.ptr == S.data() + S.size();
	}

	bool ParseRow(std::string_view Line, std::string& OutPath, GoldenRow& Out)
	{
		std::string_view Fields[4];
		for (int I = 0; I < 3; ++I)
		{
			const size_t Tab = Line.find('\t');
			if (Tab == std::string_view::npos)
			{
				return false;
			}
			Fields[I] = Line.substr(0, Tab);
			Line.remove_prefix(Tab + 1);
		}
		Fields[3] = Line;
		OutPath.assign(Fields[0]);
		return ParseInt(Fields[1], Out.Caret) && ParseInt(Fields[2], Out.First) && ParseInt(Fields[3], Out.Last);
	}
}

TEST(Extent_AgainstAstGolden)
{
	const char* Golden = std::getenv("DREAM_EXTENT_GOLDEN");
	if (Golden == nullptr || *Golden == '\0')
	{
		std::printf("  (skipped: DREAM_EXTENT_GOLDEN is not set; build.bat sets it when the engine's python.exe exists)\n");
		return;
	}
	std::ifstream In(Golden, std::ios::binary);
	REQUIRE(In.good());
	std::map<std::string, std::vector<GoldenRow>> ByFile;
	std::string Line;
	while (std::getline(In, Line))
	{
		if (!Line.empty() && Line.back() == '\r')
		{
			Line.pop_back();
		}
		std::string Path;
		GoldenRow Row;
		if (ParseRow(Line, Path, Row))
		{
			ByFile[Path].push_back(Row);
		}
	}
	REQUIRE(ByFile.size() > 100u);
	size_t Rows = 0;
	size_t Mismatches = 0;
	for (const auto& [Path, FileRows] : ByFile)
	{
		std::ifstream F(Path, std::ios::binary);
		const std::string Bytes((std::istreambuf_iterator<char>(F)), std::istreambuf_iterator<char>());
		const dream::codec::DecodeResult D = dream::codec::Decode(Bytes);
		dream::TextBuffer B(D.Text);
		dream::PyLexer Lexer;   // frontier at line 0: every query relexes first
		for (const GoldenRow& R : FileRows)
		{
			++Rows;
			const std::optional<dream::LineRange> E = dream::StatementExtent(B.Snapshot(), Lexer, R.Caret);
			if (!E || E->First != R.First || E->Last != R.Last)
			{
				if (++Mismatches <= 20)
				{
					std::fprintf(stderr, "  %s caret %d: got %d..%d, want %d..%d\n", Path.c_str(), R.Caret + 1, E ? E->First + 1 : 0,
						E ? E->Last + 1 : 0, R.First + 1, R.Last + 1);
				}
			}
		}
	}
	std::printf("  %zu files, %zu carets, %zu mismatches\n", ByFile.size(), Rows, Mismatches);
	CHECK_EQ(Mismatches, 0u);
}
