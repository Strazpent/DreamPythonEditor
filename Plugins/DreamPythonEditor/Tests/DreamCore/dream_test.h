// SPDX-License-Identifier: Apache-2.0
// Tier-1 test framework for DreamCore (TDD §14.1). Header-only, standard library only, no exceptions, no RTTI.
//
//   #include "dream_test.h"
//   TEST(TextBuffer_InsertAtEnd)
//   {
//       dream::TextBuffer B(u"ab");
//       CHECK_EQ(B.Length(), 2);
//       REQUIRE(B.LineCount() == 1);        // stops this test case on failure
//   }
//
// Exactly one translation unit defines DREAM_TEST_MAIN before including this header (Tests/DreamCore/test_main.cpp).
// The test executable takes an optional substring filter: dreamcore_tests.exe Lexer
// Exit code: 0 when every check passed, 1 otherwise.
// (slice A choice) This replaces GoogleTest (§2.3.4) until the CMake build lands; the macro names are GoogleTest-like so
// the suites port mechanically.
#pragma once

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

namespace dream_test
{
	using TestFn = void (*)();

	struct TestCase
	{
		const char* Name;
		TestFn Fn;
		const char* File;
		int Line;
	};

	inline std::vector<TestCase>& Registry()
	{
		static std::vector<TestCase> Cases;
		return Cases;
	}

	struct RunState
	{
		int Checks = 0;
		int Failures = 0;
		bool bCurrentFailed = false;
		const char* CurrentName = "";
	};

	inline RunState& State()
	{
		static RunState S;
		return S;
	}

	struct Registrar
	{
		Registrar(const char* Name, TestFn Fn, const char* File, int Line)
		{
			Registry().push_back(TestCase{Name, Fn, File, Line});
		}
	};

	// ---- Value printing for CHECK_EQ messages --------------------------------------------------------------

	inline void AppendEscaped(std::string& Out, char32_t C)
	{
		if (C >= 0x20 && C < 0x7F && C != '"' && C != '\\')
		{
			Out.push_back(static_cast<char>(C));
			return;
		}
		char Buffer[16];
		switch (C)
		{
		case '\n': Out += "\\n"; return;
		case '\r': Out += "\\r"; return;
		case '\t': Out += "\\t"; return;
		case '"': Out += "\\\""; return;
		case '\\': Out += "\\\\"; return;
		default: break;
		}
		std::snprintf(Buffer, sizeof(Buffer), C <= 0xFFFF ? "\\u%04X" : "\\U%08X", static_cast<unsigned>(C));
		Out += Buffer;
	}

	inline std::string Quote(std::string_view S)
	{
		std::string Out = "\"";
		for (char C : S)
		{
			AppendEscaped(Out, static_cast<unsigned char>(C));
		}
		Out += "\"";
		return Out;
	}

	inline std::string Quote(std::u16string_view S)
	{
		std::string Out = "u\"";
		for (char16_t C : S)
		{
			AppendEscaped(Out, C);
		}
		Out += "\"";
		return Out;
	}

	template <typename T>
	std::string Describe(const T& Value)
	{
		using U = std::remove_cv_t<std::remove_reference_t<T>>;
		char Buffer[64];
		if constexpr (std::is_same_v<U, bool>)
		{
			return Value ? "true" : "false";
		}
		else if constexpr (std::is_same_v<U, char16_t> || std::is_same_v<U, char32_t>)
		{
			std::string Out = "'";
			AppendEscaped(Out, static_cast<char32_t>(Value));
			return Out + "'";
		}
		else if constexpr (std::is_enum_v<U>)
		{
			std::snprintf(Buffer, sizeof(Buffer), "enum(%lld)", static_cast<long long>(static_cast<std::underlying_type_t<U>>(Value)));
			return Buffer;
		}
		else if constexpr (std::is_integral_v<U> && std::is_signed_v<U>)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%lld", static_cast<long long>(Value));
			return Buffer;
		}
		else if constexpr (std::is_integral_v<U>)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%llu", static_cast<unsigned long long>(Value));
			return Buffer;
		}
		else if constexpr (std::is_floating_point_v<U>)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%.17g", static_cast<double>(Value));
			return Buffer;
		}
		else if constexpr (std::is_convertible_v<const U&, std::string_view>)
		{
			return Quote(std::string_view(Value));
		}
		else if constexpr (std::is_convertible_v<const U&, std::u16string_view>)
		{
			return Quote(std::u16string_view(Value));
		}
		else if constexpr (std::is_pointer_v<U> || std::is_null_pointer_v<U>)
		{
			std::snprintf(Buffer, sizeof(Buffer), "%p", static_cast<const void*>(Value));
			return Buffer;
		}
		else
		{
			return "<value>";
		}
	}

	inline void Fail(const char* File, int Line, const std::string& Message)
	{
		RunState& S = State();
		++S.Failures;
		S.bCurrentFailed = true;
		std::fprintf(stderr, "%s(%d): FAILED in %s: %s\n", File, Line, S.CurrentName, Message.c_str());
	}

	inline bool Check(bool bOk, const char* File, int Line, const char* Expr)
	{
		++State().Checks;
		if (!bOk)
		{
			Fail(File, Line, std::string("CHECK(") + Expr + ")");
		}
		return bOk;
	}

	template <typename A, typename B>
	bool CheckEq(const A& Actual, const B& Expected, const char* File, int Line, const char* ActualExpr, const char* ExpectedExpr)
	{
		++State().Checks;
		if (Actual == Expected)
		{
			return true;
		}
		Fail(File, Line, std::string("CHECK_EQ(") + ActualExpr + ", " + ExpectedExpr + "): " + Describe(Actual) + " != " + Describe(Expected));
		return false;
	}

	template <typename A, typename B>
	bool CheckNe(const A& Actual, const B& Other, const char* File, int Line, const char* ActualExpr, const char* OtherExpr)
	{
		++State().Checks;
		if (!(Actual == Other))
		{
			return true;
		}
		Fail(File, Line, std::string("CHECK_NE(") + ActualExpr + ", " + OtherExpr + "): both " + Describe(Actual));
		return false;
	}

	inline int RunAll(int ArgC, char** ArgV)
	{
		const char* Filter = ArgC > 1 ? ArgV[1] : nullptr;
		RunState& S = State();
		int Ran = 0;
		int FailedCases = 0;
		for (const TestCase& Case : Registry())
		{
			if (Filter != nullptr && std::strstr(Case.Name, Filter) == nullptr)
			{
				continue;
			}
			S.CurrentName = Case.Name;
			S.bCurrentFailed = false;
			Case.Fn();
			++Ran;
			if (S.bCurrentFailed)
			{
				++FailedCases;
			}
			std::printf("%s %s\n", S.bCurrentFailed ? "[FAIL]" : "[ ok ]", Case.Name);
		}
		std::printf("\ndream_test: %d test cases, %d passed, %d failed; %d checks, %d failed\n",
			Ran, Ran - FailedCases, FailedCases, S.Checks, S.Failures);
		std::fflush(stdout);
		return S.Failures == 0 ? 0 : 1;
	}
}

#define DREAM_TEST_CONCAT_INNER(A, B) A##B
#define DREAM_TEST_CONCAT(A, B) DREAM_TEST_CONCAT_INNER(A, B)

/** Declares and registers a test case. Name must be a unique identifier in the executable. */
#define TEST(Name) \
	static void DREAM_TEST_CONCAT(DreamTest_, Name)(); \
	static ::dream_test::Registrar DREAM_TEST_CONCAT(DreamTestRegistrar_, Name)(#Name, &DREAM_TEST_CONCAT(DreamTest_, Name), __FILE__, __LINE__); \
	static void DREAM_TEST_CONCAT(DreamTest_, Name)()

/** Records a failure and continues. */
#define CHECK(Expr) ((void)::dream_test::Check(static_cast<bool>(Expr), __FILE__, __LINE__, #Expr))
#define CHECK_EQ(Actual, Expected) ((void)::dream_test::CheckEq((Actual), (Expected), __FILE__, __LINE__, #Actual, #Expected))
#define CHECK_NE(Actual, Other) ((void)::dream_test::CheckNe((Actual), (Other), __FILE__, __LINE__, #Actual, #Other))

/** Records a failure and returns from the current test case. */
#define REQUIRE(Expr) do { if (!::dream_test::Check(static_cast<bool>(Expr), __FILE__, __LINE__, #Expr)) { return; } } while (0)
#define REQUIRE_EQ(Actual, Expected) do { if (!::dream_test::CheckEq((Actual), (Expected), __FILE__, __LINE__, #Actual, #Expected)) { return; } } while (0)

#ifdef DREAM_TEST_MAIN
int main(int ArgC, char** ArgV)
{
	return ::dream_test::RunAll(ArgC, ArgV);
}
#endif
