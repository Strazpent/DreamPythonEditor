// SPDX-License-Identifier: Apache-2.0
// Tier 1: the JSON codec (TDD §2.1, SEC-06): round trips, key order, duplicates, numbers, escapes, malformed input,
// limits, and a small fuzz pass (T-ARCH-016 style: no crash, failure leaves null).
#include "dream_test.h"
#include "test_util.h"

#include "dream/Json.h"

#include <cmath>
#include <string>

using dream::json::Kind;
using dream::json::Value;

namespace
{
	std::string RoundTrip(std::string_view Text, bool bPretty = false)
	{
		Value V;
		std::string Error;
		if (!dream::json::Parse(Text, V, &Error))
		{
			return "<error: " + Error + ">";
		}
		return dream::json::Serialize(V, bPretty);
	}
}

TEST(Json_RoundTripCompact)
{
	const char* Cases[] = {
		"null", "true", "false", "0", "-1", "9223372036854775807", "-9223372036854775808", "1.5", "-0.25", "1e+300",
		"\"\"", "\"a\\\"b\\\\c\\n\\t\"", "[]", "{}", "[1,[2,[3]],{\"k\":[]}]",
		"{\"b\":1,\"a\":2,\"c\":{\"z\":null,\"y\":[true,false]}}",
	};
	for (const char* C : Cases)
	{
		CHECK_EQ(RoundTrip(C), std::string(C));
	}
	CHECK_EQ(RoundTrip(" { \"a\" : [ 1 , 2 ] } "), std::string("{\"a\":[1,2]}"));
	CHECK_EQ(RoundTrip("2.0"), std::string("2.0"));          // a double stays a double
	CHECK_EQ(RoundTrip("1E2"), std::string("100.0"));
	CHECK_EQ(RoundTrip("{\"a\":[1,{\"b\":2}]}", true), std::string("{\n  \"a\": [\n    1,\n    {\n      \"b\": 2\n    }\n  ]\n}"));
}

TEST(Json_StringsAndUnicode)
{
	Value V;
	REQUIRE(dream::json::Parse("\"\\u00e9\\ud83d\\ude00\\u0001/\\/\"", V));
	CHECK_EQ(V.AsString(), std::string("\xC3\xA9\xF0\x9F\x98\x80\x01//"));
	CHECK_EQ(dream::json::Serialize(V), std::string("\"\xC3\xA9\xF0\x9F\x98\x80\\u0001//\""));
	REQUIRE(dream::json::Parse("\"\\ud800x\"", V));                   // a lone surrogate escape becomes U+FFFD
	CHECK_EQ(V.AsString(), std::string("\xEF\xBF\xBDx"));
	REQUIRE(dream::json::Parse("\"caf\xC3\xA9\"", V));
	CHECK_EQ(V.AsString(), std::string("caf\xC3\xA9"));
	// Invalid UTF-8 inside a string is an error on parse, and is written as U+FFFD by the serializer.
	CHECK(!dream::json::Parse("\"\xC3\x28\"", V));
	CHECK(V.IsNull());
	const Value Bad = Value::MakeString(std::string("a\xFF" "b"));
	CHECK_EQ(dream::json::Serialize(Bad), std::string("\"a\xEF\xBF\xBD" "b\""));
	CHECK(dream::json::Parse(dream::json::Serialize(Bad), V));
}

TEST(Json_NumbersExactAndLimits)
{
	Value V;
	REQUIRE(dream::json::Parse("18446744073709551615", V));          // beyond int64: a double
	CHECK(!V.IsInteger());
	CHECK_EQ(V.AsUInt(), 18446744073709551615ull);
	REQUIRE(dream::json::Parse("123456789012345678", V));
	CHECK(V.IsInteger());
	CHECK_EQ(V.AsInt(), 123456789012345678ll);
	REQUIRE(dream::json::Parse("-3.9", V));
	CHECK_EQ(V.AsInt(), -3);
	CHECK_EQ(V.AsUInt(7), 7u);
	REQUIRE(dream::json::Parse("1e999", V));                          // overflow is a number, written as null
	CHECK(std::isinf(V.AsDouble()));
	CHECK_EQ(dream::json::Serialize(V), std::string("null"));
	REQUIRE(dream::json::Parse("1e-999", V));
	CHECK_EQ(V.AsDouble(), 0.0);
	CHECK_EQ(dream::json::Serialize(Value::MakeUInt(5)), std::string("5"));
	CHECK_EQ(dream::json::Serialize(Value::MakeDouble(0.1)), std::string("0.1"));
	CHECK_EQ(dream::json::Serialize(Value::MakeDouble(std::nan(""))), std::string("null"));
}

TEST(Json_ObjectsKeepOrderAndLastDuplicateWins)
{
	Value V;
	REQUIRE(dream::json::Parse("{\"z\":1,\"a\":2,\"z\":3}", V));
	REQUIRE_EQ(V.Size(), 2u);
	CHECK_EQ(V.Members()[0].Key, std::string("z"));
	CHECK_EQ(V.Get("z").AsInt(), 3);
	CHECK_EQ(dream::json::Serialize(V), std::string("{\"z\":3,\"a\":2}"));
	// Writers keep unknown keys in place (§10.2.3).
	V.Set("a", Value::MakeString("x"));
	V.Set("new", Value::MakeBool(true));
	CHECK_EQ(dream::json::Serialize(V), std::string("{\"z\":3,\"a\":\"x\",\"new\":true}"));
	CHECK(V.Remove("z"));
	CHECK(!V.Remove("z"));
	CHECK(V.Find("z") == nullptr);
	CHECK(V.Get("missing").IsNull());
	CHECK(V.At(0).IsNull());
	Value A;
	A.Append(Value::MakeInt(1));
	CHECK(A.IsArray());
	CHECK_EQ(A.At(0).AsInt(), 1);
	CHECK_EQ(Value::MakeInt(4).AsString(), std::string());
}

TEST(Json_MalformedInputFails)
{
	const char* Bad[] = {
		"", " ", "nul", "tru", "[1,]", "[1 2]", "{\"a\" 1}", "{\"a\":}", "{a:1}", "{\"a\":1,}", "[", "]", "{", "\"abc",
		"\"a\nb\"", "01", "1.", ".5", "-", "+1", "1e", "1e+", "\"\\x41\"", "\"\\u12G4\"", "[1]x", "nan", "Infinity",
		"\"\\ud83d\\u12\"", "{\"a\":1}}", "[\"a\",]",
	};
	for (const char* B : Bad)
	{
		Value V = Value::MakeInt(1);
		std::string Error;
		CHECK(!dream::json::Parse(B, V, &Error));
		CHECK(V.IsNull());
		CHECK(!Error.empty());
	}
}

TEST(Json_DepthAndSizeLimits)
{
	std::string Deep(64, '[');
	Deep += std::string(64, ']');
	Value V;
	CHECK(dream::json::Parse(Deep, V));
	std::string TooDeep(65, '[');
	TooDeep += std::string(65, ']');
	CHECK(!dream::json::Parse(TooDeep, V));
	dream::json::ParseOptions Small;
	Small.MaxBytes = 4;
	CHECK(!dream::json::Parse("[1,2]", V, nullptr, Small));
	CHECK(dream::json::Parse("[1]", V, nullptr, Small));
	std::string Huge = "{";
	for (int I = 0; I < 20000; ++I)
	{
		Huge += (I ? ",\"k" : "\"k") + std::to_string(I) + "\":" + std::to_string(I);
	}
	Huge += "}";
	REQUIRE(dream::json::Parse(Huge, V));
	CHECK_EQ(V.Size(), 20000u);
	CHECK_EQ(V.Get("k19999").AsInt(), 19999);
}

TEST(Json_FuzzNeverCrashes)
{
	dream_test_util::Rng R(1234);
	const std::string Seeds[] = {"{\"a\":[1,2,{\"b\":\"c\\u00e9\"}],\"d\":-1.5e3,\"e\":null,\"f\":true}", "[[[[\"x\"]]]]", "\"\\ud83d\\ude00\""};
	int Parsed = 0;
	for (int I = 0; I < 20000; ++I)
	{
		std::string S = Seeds[R.Below(3)];
		const int Mutations = 1 + static_cast<int>(R.Below(4));
		for (int M = 0; M < Mutations; ++M)
		{
			const size_t At = R.Below(S.size() + 1);
			switch (R.Below(3))
			{
			case 0:
				S.insert(S.begin() + static_cast<std::ptrdiff_t>(At), static_cast<char>(R.Below(256)));
				break;
			case 1:
				if (At < S.size())
				{
					S.erase(At, 1);
				}
				break;
			default:
				if (At < S.size())
				{
					S[At] = static_cast<char>(R.Below(256));
				}
				break;
			}
		}
		Value V;
		if (dream::json::Parse(S, V))
		{
			++Parsed;
			Value Again;
			CHECK(dream::json::Parse(dream::json::Serialize(V), Again));   // whatever parses re-serializes and re-parses
			CHECK_EQ(dream::json::Serialize(Again), dream::json::Serialize(V));
		}
		else
		{
			CHECK(V.IsNull());
		}
	}
	CHECK(Parsed > 0);
}
