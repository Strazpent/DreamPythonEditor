// SPDX-License-Identifier: Apache-2.0
// Tier 1: T-LEX-001, PyLexer token boundaries against the engine interpreter's 3.11 tokenize on its own Lib/**/*.py
// (f-string and escape sub-tokens merged into their STRING token). build.bat writes the golden rows with lex_golden.py
// and names them in DREAM_LEX_GOLDEN; without it the test reports a skip.
#define _CRT_SECURE_NO_WARNINGS   // std::getenv
#include "dream_test.h"

#include "dream/PyLexer.h"
#include "dream/TextCodec.h"
#include "dream/Utf.h"

#include <charconv>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace
{
	struct GoldenLine
	{
		int32_t Line = 0;
		std::vector<int32_t> Bounds;
		std::vector<std::pair<int32_t, int32_t>> Merged;
	};

	std::vector<int32_t> ParseInts(std::string_view S, char Sep)
	{
		std::vector<int32_t> Out;
		size_t Start = 0;
		while (Start < S.size())
		{
			size_t End = S.find(Sep, Start);
			if (End == std::string_view::npos)
			{
				End = S.size();
			}
			int32_t V = 0;
			std::from_chars(S.data() + Start, S.data() + End, V);
			Out.push_back(V);
			Start = End + 1;
		}
		return Out;
	}

	bool ParseLine(std::string_view Row, GoldenLine& Out)
	{
		const size_t T1 = Row.find('\t');
		const size_t T2 = T1 == std::string_view::npos ? T1 : Row.find('\t', T1 + 1);
		if (T2 == std::string_view::npos)
		{
			return false;
		}
		std::from_chars(Row.data(), Row.data() + T1, Out.Line);
		Out.Bounds = ParseInts(Row.substr(T1 + 1, T2 - T1 - 1), ',');
		Out.Merged.clear();
		const std::string_view M = Row.substr(T2 + 1);
		if (M != ".")
		{
			size_t Start = 0;
			while (Start < M.size())
			{
				size_t End = M.find(';', Start);
				if (End == std::string_view::npos)
				{
					End = M.size();
				}
				const std::vector<int32_t> SE = ParseInts(M.substr(Start, End - Start), '-');
				if (SE.size() == 2)
				{
					Out.Merged.emplace_back(SE[0], SE[1]);
				}
				Start = End + 1;
			}
		}
		return true;
	}

	struct FileCheck
	{
		std::string Path;
		std::vector<GoldenLine> Lines;
	};

	void CheckFile(const FileCheck& F, size_t& Lines, size_t& Mismatches, size_t& BadFiles)
	{
		std::ifstream In(F.Path, std::ios::binary);
		const std::string Bytes((std::istreambuf_iterator<char>(In)), std::istreambuf_iterator<char>());
		const dream::codec::DecodeResult D = dream::codec::Decode(Bytes);
		const dream::TextBuffer B(D.Text);
		std::vector<std::vector<uint32_t>> Toks;
		dream::PyLexer::LexFull(B.Snapshot(), dream::LexGrammar::Py311, Toks);
		bool bFileBad = false;
		for (const GoldenLine& G : F.Lines)
		{
			++Lines;
			if (G.Line < 0 || static_cast<size_t>(G.Line) >= Toks.size())
			{
				bFileBad = true;
				++Mismatches;
				continue;
			}
			std::vector<int32_t> Ours;
			for (uint32_t T : Toks[static_cast<size_t>(G.Line)])
			{
				const int32_t S = dream::TokenStart(T);
				bool bInner = false;
				for (const auto& [MS, ME] : G.Merged)
				{
					bInner = bInner || (S > MS && S < ME);
				}
				if (!bInner)
				{
					Ours.push_back(S);
				}
			}
			std::vector<int32_t> Want;
			for (int32_t W : G.Bounds)
			{
				bool bInner = false;
				for (const auto& [MS, ME] : G.Merged)
				{
					bInner = bInner || (W > MS && W < ME);
				}
				if (!bInner)
				{
					Want.push_back(W);
				}
			}
			if (Ours != Want)
			{
				if (++Mismatches <= 25)
				{
					std::string O;
					std::string Wt;
					for (int32_t X : Ours)
					{
						O += std::to_string(X) + ",";
					}
					for (int32_t X : Want)
					{
						Wt += std::to_string(X) + ",";
					}
					std::string Text;
					dream::utf::Utf16ToUtf8(B.Snapshot().LineText(G.Line), Text);
					std::fprintf(stderr, "  %s:%d: ours %s tokenize %s | %.120s\n", F.Path.c_str(), G.Line + 1, O.c_str(), Wt.c_str(), Text.c_str());
				}
				bFileBad = true;
			}
		}
		if (bFileBad)
		{
			++BadFiles;
		}
	}
}

TEST(Lex_AgainstTokenizeGolden)   // T-LEX-001
{
	const char* Golden = std::getenv("DREAM_LEX_GOLDEN");
	if (Golden == nullptr || *Golden == '\0')
	{
		std::printf("  (skipped: DREAM_LEX_GOLDEN is not set; build.bat sets it when the engine's python.exe exists)\n");
		return;
	}
	std::ifstream In(Golden, std::ios::binary);
	REQUIRE(In.good());
	std::string Row;
	FileCheck Current;
	size_t Files = 0;
	size_t Lines = 0;
	size_t Mismatches = 0;
	size_t BadFiles = 0;
	while (std::getline(In, Row))
	{
		if (!Row.empty() && Row.back() == '\r')
		{
			Row.pop_back();
		}
		if (Row.empty() || Row[0] == '#')
		{
			continue;   // the generator's stamp line
		}
		if (Row.size() > 2 && Row[0] == 'F' && Row[1] == '\t')
		{
			if (!Current.Path.empty())
			{
				CheckFile(Current, Lines, Mismatches, BadFiles);
				++Files;
			}
			Current.Path = Row.substr(2);
			Current.Lines.clear();
			continue;
		}
		GoldenLine G;
		if (ParseLine(Row, G))
		{
			Current.Lines.push_back(std::move(G));
		}
	}
	if (!Current.Path.empty())
	{
		CheckFile(Current, Lines, Mismatches, BadFiles);
		++Files;
	}
	std::printf("  %zu files, %zu lines, %zu mismatching lines in %zu files\n", Files, Lines, Mismatches, BadFiles);
	CHECK(Files > 1000u);
	CHECK_EQ(Mismatches, 0u);
}
