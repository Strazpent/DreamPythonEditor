// SPDX-License-Identifier: Apache-2.0
// Dream core: the JSON codec (RFC 8259) for Dream's own formats (TDD §2.1, SEC-06). Data only; bounded depth and size.
#include "dream/Json.h"

#include "dream/Utf.h"

#include <charconv>
#include <cmath>
#include <limits>
#include <system_error>
#include <unordered_map>
#include <utility>

namespace dream::json_detail
{
	using json::Kind;
	using json::Value;

	const Value& NullValue()
	{
		static const Value Null;
		return Null;
	}

	const std::string& EmptyString()
	{
		static const std::string Empty;
		return Empty;
	}

	/** Length of the valid UTF-8 sequence at I, or 0 when the bytes there are not valid UTF-8 (Unicode Table 3-7). */
	size_t ValidUtf8At(std::string_view S, size_t I)
	{
		const uint8_t B0 = static_cast<uint8_t>(S[I]);
		if (B0 < 0x80)
		{
			return 1;
		}
		size_t Need = 0;
		uint8_t Lo = 0x80;
		uint8_t Hi = 0xBF;
		if (B0 >= 0xC2 && B0 <= 0xDF)
		{
			Need = 1;
		}
		else if (B0 >= 0xE0 && B0 <= 0xEF)
		{
			Need = 2;
			Lo = B0 == 0xE0 ? 0xA0 : 0x80;
			Hi = B0 == 0xED ? 0x9F : 0xBF;
		}
		else if (B0 >= 0xF0 && B0 <= 0xF4)
		{
			Need = 3;
			Lo = B0 == 0xF0 ? 0x90 : 0x80;
			Hi = B0 == 0xF4 ? 0x8F : 0xBF;
		}
		else
		{
			return 0;
		}
		for (size_t K = 1; K <= Need; ++K)
		{
			if (I + K >= S.size())
			{
				return 0;
			}
			const uint8_t B = static_cast<uint8_t>(S[I + K]);
			const uint8_t MinB = K == 1 ? Lo : 0x80;
			const uint8_t MaxB = K == 1 ? Hi : 0xBF;
			if (B < MinB || B > MaxB)
			{
				return 0;
			}
		}
		return Need + 1;
	}

	class Parser
	{
	public:
		Parser(std::string_view InText, const json::ParseOptions& InOptions)
			: Text(InText), Options(InOptions)
		{
		}

		bool Run(Value& Out)
		{
			SkipWs();
			if (!ParseValue(Out, 0))
			{
				return false;
			}
			SkipWs();
			if (Pos != Text.size())
			{
				return Fail("trailing characters");
			}
			return true;
		}

		std::string Error;

	private:
		std::string_view Text;
		const json::ParseOptions& Options;
		size_t Pos = 0;

		bool Fail(const char* Message)
		{
			if (Error.empty())
			{
				Error = std::string(Message) + " at byte " + std::to_string(Pos);
			}
			return false;
		}

		void SkipWs()
		{
			while (Pos < Text.size())
			{
				const char C = Text[Pos];
				if (C != ' ' && C != '\t' && C != '\n' && C != '\r')
				{
					break;
				}
				++Pos;
			}
		}

		bool Literal(std::string_view Word)
		{
			if (Text.substr(Pos, Word.size()) != Word)
			{
				return Fail("invalid literal");
			}
			Pos += Word.size();
			return true;
		}

		bool ParseValue(Value& Out, size_t Depth)
		{
			if (Pos >= Text.size())
			{
				return Fail("unexpected end of input");
			}
			switch (Text[Pos])
			{
			case 'n':
				Out = Value::MakeNull();
				return Literal("null");
			case 't':
				Out = Value::MakeBool(true);
				return Literal("true");
			case 'f':
				Out = Value::MakeBool(false);
				return Literal("false");
			case '"':
			{
				std::string S;
				if (!ParseString(S))
				{
					return false;
				}
				Out = Value::MakeString(std::move(S));
				return true;
			}
			case '[':
				return ParseArray(Out, Depth + 1);
			case '{':
				return ParseObject(Out, Depth + 1);
			default:
				return ParseNumber(Out);
			}
		}

		bool ParseArray(Value& Out, size_t Depth)
		{
			if (Depth > Options.MaxDepth)
			{
				return Fail("nesting too deep");
			}
			++Pos;
			Out = Value::MakeArray();
			SkipWs();
			if (Pos < Text.size() && Text[Pos] == ']')
			{
				++Pos;
				return true;
			}
			for (;;)
			{
				SkipWs();
				Value Item;
				if (!ParseValue(Item, Depth))
				{
					return false;
				}
				Out.Append(std::move(Item));
				SkipWs();
				if (Pos >= Text.size())
				{
					return Fail("unterminated array");
				}
				if (Text[Pos] == ',')
				{
					++Pos;
					continue;
				}
				if (Text[Pos] == ']')
				{
					++Pos;
					return true;
				}
				return Fail("expected ',' or ']'");
			}
		}

		bool ParseObject(Value& Out, size_t Depth)
		{
			if (Depth > Options.MaxDepth)
			{
				return Fail("nesting too deep");
			}
			++Pos;
			Out = Value::MakeObject();
			SkipWs();
			if (Pos < Text.size() && Text[Pos] == '}')
			{
				++Pos;
				return true;
			}
			// Large objects index their keys so duplicate detection stays linear (fuzzed input, T-ARCH-016).
			std::unordered_map<std::string, size_t> Index;
			std::vector<std::pair<std::string, Value>> Items;
			for (;;)
			{
				SkipWs();
				if (Pos >= Text.size() || Text[Pos] != '"')
				{
					return Fail("expected a string key");
				}
				std::string Key;
				if (!ParseString(Key))
				{
					return false;
				}
				SkipWs();
				if (Pos >= Text.size() || Text[Pos] != ':')
				{
					return Fail("expected ':'");
				}
				++Pos;
				SkipWs();
				Value Item;
				if (!ParseValue(Item, Depth))
				{
					return false;
				}
				auto Found = Index.find(Key);
				if (Found != Index.end())
				{
					Items[Found->second].second = std::move(Item);   // the last duplicate wins, in place
				}
				else
				{
					Index.emplace(Key, Items.size());
					Items.emplace_back(std::move(Key), std::move(Item));
				}
				SkipWs();
				if (Pos >= Text.size())
				{
					return Fail("unterminated object");
				}
				if (Text[Pos] == ',')
				{
					++Pos;
					continue;
				}
				if (Text[Pos] == '}')
				{
					++Pos;
					break;
				}
				return Fail("expected ',' or '}'");
			}
			Out.ObjectMembers.reserve(Items.size());
			for (auto& Item : Items)
			{
				Out.ObjectMembers.push_back(Value::Member{std::move(Item.first), std::move(Item.second)});
			}
			return true;
		}

		bool Hex4(uint32_t& Out)
		{
			if (Pos + 4 > Text.size())
			{
				return Fail("truncated \\u escape");
			}
			uint32_t V = 0;
			for (int I = 0; I < 4; ++I)
			{
				const char C = Text[Pos + I];
				V <<= 4;
				if (C >= '0' && C <= '9')
				{
					V |= static_cast<uint32_t>(C - '0');
				}
				else if (C >= 'a' && C <= 'f')
				{
					V |= static_cast<uint32_t>(C - 'a' + 10);
				}
				else if (C >= 'A' && C <= 'F')
				{
					V |= static_cast<uint32_t>(C - 'A' + 10);
				}
				else
				{
					return Fail("invalid \\u escape");
				}
			}
			Pos += 4;
			Out = V;
			return true;
		}

		bool ParseString(std::string& Out)
		{
			++Pos;   // opening quote
			for (;;)
			{
				if (Pos >= Text.size())
				{
					return Fail("unterminated string");
				}
				const uint8_t C = static_cast<uint8_t>(Text[Pos]);
				if (C == '"')
				{
					++Pos;
					return true;
				}
				if (C < 0x20)
				{
					return Fail("control character in string");
				}
				if (C == '\\')
				{
					if (Pos + 1 >= Text.size())
					{
						return Fail("truncated escape");
					}
					const char E = Text[Pos + 1];
					Pos += 2;
					switch (E)
					{
					case '"': Out.push_back('"'); break;
					case '\\': Out.push_back('\\'); break;
					case '/': Out.push_back('/'); break;
					case 'b': Out.push_back('\b'); break;
					case 'f': Out.push_back('\f'); break;
					case 'n': Out.push_back('\n'); break;
					case 'r': Out.push_back('\r'); break;
					case 't': Out.push_back('\t'); break;
					case 'u':
					{
						uint32_t Unit = 0;
						if (!Hex4(Unit))
						{
							return false;
						}
						char32_t CodePoint = Unit;
						if (Unit >= 0xD800 && Unit <= 0xDBFF && Pos + 6 <= Text.size() && Text[Pos] == '\\' && Text[Pos + 1] == 'u')
						{
							const size_t Save = Pos;
							Pos += 2;
							uint32_t Low = 0;
							if (!Hex4(Low))
							{
								return false;
							}
							if (Low >= 0xDC00 && Low <= 0xDFFF)
							{
								CodePoint = 0x10000 + ((Unit - 0xD800) << 10) + (Low - 0xDC00);
							}
							else
							{
								Pos = Save;   // lone high surrogate; the next escape is parsed on its own
							}
						}
						// A lone surrogate cannot be held in UTF-8; AppendUtf8 writes U+FFFD (slice A choice: lenient).
						utf::AppendUtf8(CodePoint, Out);
						break;
					}
					default:
						return Fail("invalid escape");
					}
					continue;
				}
				if (C < 0x80)
				{
					Out.push_back(static_cast<char>(C));
					++Pos;
					continue;
				}
				const size_t Len = ValidUtf8At(Text, Pos);
				if (Len == 0)
				{
					return Fail("invalid UTF-8 in string");
				}
				Out.append(Text.substr(Pos, Len));
				Pos += Len;
			}
		}

		static bool IsDigit(char C) { return C >= '0' && C <= '9'; }

		bool ParseNumber(Value& Out)
		{
			const size_t Start = Pos;
			if (Pos < Text.size() && Text[Pos] == '-')
			{
				++Pos;
			}
			if (Pos >= Text.size() || !IsDigit(Text[Pos]))
			{
				return Fail("invalid value");
			}
			if (Text[Pos] == '0')
			{
				++Pos;
			}
			else
			{
				while (Pos < Text.size() && IsDigit(Text[Pos]))
				{
					++Pos;
				}
			}
			bool bInteger = true;
			if (Pos < Text.size() && Text[Pos] == '.')
			{
				bInteger = false;
				++Pos;
				if (Pos >= Text.size() || !IsDigit(Text[Pos]))
				{
					return Fail("invalid number");
				}
				while (Pos < Text.size() && IsDigit(Text[Pos]))
				{
					++Pos;
				}
			}
			if (Pos < Text.size() && (Text[Pos] == 'e' || Text[Pos] == 'E'))
			{
				bInteger = false;
				++Pos;
				if (Pos < Text.size() && (Text[Pos] == '+' || Text[Pos] == '-'))
				{
					++Pos;
				}
				if (Pos >= Text.size() || !IsDigit(Text[Pos]))
				{
					return Fail("invalid number");
				}
				while (Pos < Text.size() && IsDigit(Text[Pos]))
				{
					++Pos;
				}
			}
			const char* First = Text.data() + Start;
			const char* Last = Text.data() + Pos;
			if (bInteger)
			{
				int64_t I = 0;
				const auto R = std::from_chars(First, Last, I);
				if (R.ec == std::errc() && R.ptr == Last)
				{
					Out = Value::MakeInt(I);
					return true;
				}
			}
			double D = 0.0;
			const auto R = std::from_chars(First, Last, D);
			if (R.ptr != Last || (R.ec != std::errc() && R.ec != std::errc::result_out_of_range))
			{
				return Fail("invalid number");
			}
			if (R.ec == std::errc::result_out_of_range)
			{
				// A well-formed literal outside the double range: a negative exponent underflows to zero, anything
				// else overflows to infinity (which serializes as null).
				const std::string_view Lit = Text.substr(Start, Pos - Start);
				const size_t E = Lit.find_first_of("eE");
				const bool bUnderflow = E != std::string_view::npos && E + 1 < Lit.size() && Lit[E + 1] == '-';
				const double Sign = Lit.front() == '-' ? -1.0 : 1.0;
				D = bUnderflow ? Sign * 0.0 : Sign * std::numeric_limits<double>::infinity();
			}
			Out = Value::MakeDouble(D);
			return true;
		}
	};

	void AppendEscapedString(std::string_view S, std::string& Out)
	{
		static constexpr char Hex[] = "0123456789abcdef";
		Out.push_back('"');
		size_t I = 0;
		while (I < S.size())
		{
			const uint8_t C = static_cast<uint8_t>(S[I]);
			if (C >= 0x80)
			{
				const size_t Len = ValidUtf8At(S, I);
				if (Len == 0)
				{
					Out.append("\xEF\xBF\xBD");   // invalid UTF-8 in a value: write U+FFFD so the output always parses
					++I;
				}
				else
				{
					Out.append(S.substr(I, Len));
					I += Len;
				}
				continue;
			}
			switch (C)
			{
			case '"': Out.append("\\\""); break;
			case '\\': Out.append("\\\\"); break;
			case '\b': Out.append("\\b"); break;
			case '\f': Out.append("\\f"); break;
			case '\n': Out.append("\\n"); break;
			case '\r': Out.append("\\r"); break;
			case '\t': Out.append("\\t"); break;
			default:
				if (C < 0x20 || C == 0x7F)
				{
					Out.append("\\u00");
					Out.push_back(Hex[C >> 4]);
					Out.push_back(Hex[C & 0xF]);
				}
				else
				{
					Out.push_back(static_cast<char>(C));
				}
				break;
			}
			++I;
		}
		Out.push_back('"');
	}

	void AppendNumber(double D, bool bIsInt, int64_t I, std::string& Out)
	{
		char Buffer[64];
		if (bIsInt)
		{
			const auto R = std::to_chars(Buffer, Buffer + sizeof(Buffer), I);
			Out.append(Buffer, R.ptr);
			return;
		}
		if (!std::isfinite(D))
		{
			Out.append("null");
			return;
		}
		const auto R = std::to_chars(Buffer, Buffer + sizeof(Buffer), D);
		std::string_view Text(Buffer, static_cast<size_t>(R.ptr - Buffer));
		Out.append(Text);
		if (Text.find_first_of(".eE") == std::string_view::npos)
		{
			Out.append(".0");   // keep a double a double across a round trip
		}
	}

	void Indent(std::string& Out, size_t Level)
	{
		Out.push_back('\n');
		Out.append(Level * 2, ' ');
	}
}

namespace dream::json
{
	Value::Value() = default;
	Value::Value(const Value& Other) = default;
	Value::Value(Value&& Other) noexcept = default;
	Value& Value::operator=(const Value& Other) = default;
	Value& Value::operator=(Value&& Other) noexcept = default;
	Value::~Value() = default;

	Value Value::MakeNull()
	{
		return Value();
	}

	Value Value::MakeBool(bool B)
	{
		Value V;
		V.ValueKind = Kind::Bool;
		V.bBool = B;
		return V;
	}

	Value Value::MakeInt(int64_t I)
	{
		Value V;
		V.ValueKind = Kind::Number;
		V.bIsInt = true;
		V.Int = I;
		V.Num = static_cast<double>(I);
		return V;
	}

	Value Value::MakeUInt(uint64_t U)
	{
		if (U <= static_cast<uint64_t>(std::numeric_limits<int64_t>::max()))
		{
			return MakeInt(static_cast<int64_t>(U));
		}
		return MakeDouble(static_cast<double>(U));
	}

	Value Value::MakeDouble(double D)
	{
		Value V;
		V.ValueKind = Kind::Number;
		V.bIsInt = false;
		V.Num = D;
		return V;
	}

	Value Value::MakeString(std::string Utf8)
	{
		Value V;
		V.ValueKind = Kind::String;
		V.Str = std::move(Utf8);
		return V;
	}

	Value Value::MakeArray()
	{
		Value V;
		V.ValueKind = Kind::Array;
		return V;
	}

	Value Value::MakeObject()
	{
		Value V;
		V.ValueKind = Kind::Object;
		return V;
	}

	bool Value::AsBool(bool Default) const
	{
		return ValueKind == Kind::Bool ? bBool : Default;
	}

	int64_t Value::AsInt(int64_t Default) const
	{
		if (ValueKind != Kind::Number)
		{
			return Default;
		}
		if (bIsInt)
		{
			return Int;
		}
		if (!(Num == Num))
		{
			return Default;
		}
		if (Num >= 9223372036854775807.0)
		{
			return std::numeric_limits<int64_t>::max();
		}
		if (Num <= -9223372036854775808.0)
		{
			return std::numeric_limits<int64_t>::min();
		}
		return static_cast<int64_t>(Num);
	}

	uint64_t Value::AsUInt(uint64_t Default) const
	{
		if (ValueKind != Kind::Number)
		{
			return Default;
		}
		if (bIsInt)
		{
			return Int >= 0 ? static_cast<uint64_t>(Int) : Default;
		}
		if (!(Num >= 0.0))
		{
			return Default;
		}
		if (Num >= 18446744073709551615.0)
		{
			return std::numeric_limits<uint64_t>::max();
		}
		return static_cast<uint64_t>(Num);
	}

	double Value::AsDouble(double Default) const
	{
		if (ValueKind != Kind::Number)
		{
			return Default;
		}
		return bIsInt ? static_cast<double>(Int) : Num;
	}

	const std::string& Value::AsString() const
	{
		return ValueKind == Kind::String ? Str : json_detail::EmptyString();
	}

	size_t Value::Size() const
	{
		if (ValueKind == Kind::Array)
		{
			return Items.size();
		}
		if (ValueKind == Kind::Object)
		{
			return ObjectMembers.size();
		}
		return 0;
	}

	const Value& Value::At(size_t Index) const
	{
		if (ValueKind != Kind::Array || Index >= Items.size())
		{
			return json_detail::NullValue();
		}
		return Items[Index];
	}

	Value& Value::Append(Value V)
	{
		if (ValueKind != Kind::Array)
		{
			*this = MakeArray();
		}
		Items.push_back(std::move(V));
		return Items.back();
	}

	const Value* Value::Find(std::string_view Key) const
	{
		if (ValueKind != Kind::Object)
		{
			return nullptr;
		}
		for (const Member& M : ObjectMembers)
		{
			if (M.Key == Key)
			{
				return &M.Val;
			}
		}
		return nullptr;
	}

	Value* Value::Find(std::string_view Key)
	{
		return const_cast<Value*>(static_cast<const Value*>(this)->Find(Key));
	}

	const Value& Value::Get(std::string_view Key) const
	{
		const Value* V = Find(Key);
		return V != nullptr ? *V : json_detail::NullValue();
	}

	Value& Value::Set(std::string_view Key, Value V)
	{
		if (ValueKind != Kind::Object)
		{
			*this = MakeObject();
		}
		for (Member& M : ObjectMembers)
		{
			if (M.Key == Key)
			{
				M.Val = std::move(V);
				return M.Val;
			}
		}
		ObjectMembers.push_back(Member{std::string(Key), std::move(V)});
		return ObjectMembers.back().Val;
	}

	bool Value::Remove(std::string_view Key)
	{
		if (ValueKind != Kind::Object)
		{
			return false;
		}
		for (auto It = ObjectMembers.begin(); It != ObjectMembers.end(); ++It)
		{
			if (It->Key == Key)
			{
				ObjectMembers.erase(It);
				return true;
			}
		}
		return false;
	}

	const std::vector<Value::Member>& Value::Members() const
	{
		return ObjectMembers;
	}

	bool Parse(std::string_view Utf8, Value& Out, std::string* OutError, const ParseOptions& Options)
	{
		Out = Value();
		if (Utf8.size() > Options.MaxBytes)
		{
			if (OutError != nullptr)
			{
				*OutError = "input larger than the limit";
			}
			return false;
		}
		json_detail::Parser P(Utf8, Options);
		Value Result;
		if (!P.Run(Result))
		{
			if (OutError != nullptr)
			{
				*OutError = P.Error;
			}
			return false;
		}
		Out = std::move(Result);
		return true;
	}

	namespace
	{
		void SerializeInto(const Value& V, std::string& Out, bool bPretty, size_t Level)
		{
			switch (V.GetKind())
			{
			case Kind::Null:
				Out.append("null");
				return;
			case Kind::Bool:
				Out.append(V.AsBool() ? "true" : "false");
				return;
			case Kind::Number:
				json_detail::AppendNumber(V.AsDouble(), V.IsInteger(), V.AsInt(), Out);
				return;
			case Kind::String:
				json_detail::AppendEscapedString(V.AsString(), Out);
				return;
			case Kind::Array:
			{
				Out.push_back('[');
				for (size_t I = 0; I < V.Size(); ++I)
				{
					if (I > 0)
					{
						Out.push_back(',');
					}
					if (bPretty)
					{
						json_detail::Indent(Out, Level + 1);
					}
					SerializeInto(V.At(I), Out, bPretty, Level + 1);
				}
				if (bPretty && V.Size() > 0)
				{
					json_detail::Indent(Out, Level);
				}
				Out.push_back(']');
				return;
			}
			case Kind::Object:
			{
				Out.push_back('{');
				bool bFirst = true;
				for (const Value::Member& M : V.Members())
				{
					if (!bFirst)
					{
						Out.push_back(',');
					}
					bFirst = false;
					if (bPretty)
					{
						json_detail::Indent(Out, Level + 1);
					}
					json_detail::AppendEscapedString(M.Key, Out);
					Out.append(bPretty ? ": " : ":");
					SerializeInto(M.Val, Out, bPretty, Level + 1);
				}
				if (bPretty && V.Size() > 0)
				{
					json_detail::Indent(Out, Level);
				}
				Out.push_back('}');
				return;
			}
			}
		}
	}

	void Serialize(const Value& V, std::string& Out, bool bPretty)
	{
		SerializeInto(V, Out, bPretty, 0);
	}

	std::string Serialize(const Value& V, bool bPretty)
	{
		std::string Out;
		SerializeInto(V, Out, bPretty, 0);
		return Out;
	}
}
