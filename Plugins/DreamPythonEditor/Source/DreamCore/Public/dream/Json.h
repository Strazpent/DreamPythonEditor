// SPDX-License-Identifier: Apache-2.0
// Dream core: the one JSON codec for Dream's own formats (TDD §2.1 "JsonRpc ... also the one JSON codec", SEC-06):
// journal record heads (§10.2.2), REPL history lines (§6.5), state files (§10.2.3).
// Data only, never code. Fuzzed (T-ARCH-016). Any thread; values are plain data.
// (slice A choice) The codec lives in its own header now; dream::JsonRpc (slice B) reuses it.
#pragma once

#include "dream/Export.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace dream::json_detail
{
	class Parser;
}

namespace dream::json
{
	enum class Kind : uint8_t
	{
		Null,
		Bool,
		Number,
		String,
		Array,
		Object,
	};

	/**
	 * A JSON value. Objects keep member order and duplicate-free keys (the last duplicate wins on parse), so a
	 * writer that round-trips a file keeps unknown keys in place (TDD §10.2.3 "writers keep unknown keys").
	 * Numbers keep an exact int64 when the literal is an integer that fits, else a double.
	 */
	class DREAMCORE_API Value
	{
	public:
		struct Member;

		Value();                                         // null
		// Declared, and defined in Json.cpp where Member is complete: a dllexport class (UBT's DREAMCORE_API) would
		// otherwise instantiate the implicit copy of std::vector<Member> while Member is still incomplete.
		Value(const Value& Other);
		Value(Value&& Other) noexcept;
		Value& operator=(const Value& Other);
		Value& operator=(Value&& Other) noexcept;
		~Value();

		static Value MakeNull();
		static Value MakeBool(bool B);
		static Value MakeInt(int64_t I);
		static Value MakeUInt(uint64_t U);               // stored as int64 when it fits, else as double
		static Value MakeDouble(double D);
		static Value MakeString(std::string Utf8);
		static Value MakeArray();
		static Value MakeObject();

		Kind GetKind() const { return ValueKind; }
		bool IsNull() const { return ValueKind == Kind::Null; }
		bool IsObject() const { return ValueKind == Kind::Object; }
		bool IsArray() const { return ValueKind == Kind::Array; }
		bool IsString() const { return ValueKind == Kind::String; }
		/** A number that holds an exact int64 (an integer literal that fit, MakeInt, MakeUInt that fit). */
		bool IsInteger() const { return ValueKind == Kind::Number && bIsInt; }

		bool AsBool(bool Default = false) const;
		int64_t AsInt(int64_t Default = 0) const;        // a double is truncated toward zero
		uint64_t AsUInt(uint64_t Default = 0) const;
		double AsDouble(double Default = 0.0) const;
		const std::string& AsString() const;             // empty string for non-strings

		// Arrays
		size_t Size() const;                             // array length or object member count
		const Value& At(size_t Index) const;             // null value when out of range or not an array
		Value& Append(Value V);                          // turns a null into an array first

		// Objects
		const Value* Find(std::string_view Key) const;   // nullptr when absent or not an object
		Value* Find(std::string_view Key);
		const Value& Get(std::string_view Key) const;    // null value when absent
		Value& Set(std::string_view Key, Value V);       // turns a null into an object first; replaces in place
		bool Remove(std::string_view Key);
		const std::vector<Member>& Members() const;

	private:
		friend class dream::json_detail::Parser;      // fills ObjectMembers directly once keys are deduplicated

		Kind ValueKind = Kind::Null;
		bool bBool = false;
		bool bIsInt = false;
		int64_t Int = 0;
		double Num = 0.0;
		std::string Str;
		std::vector<Value> Items;
		std::vector<Member> ObjectMembers;
	};

	struct Value::Member
	{
		std::string Key;
		Value Val;
	};

	struct ParseOptions
	{
		size_t MaxDepth = 64;
		size_t MaxBytes = 16u << 20;                     // larger input is refused (policy files cap lower, §11.2.2)
	};

	/** RFC 8259 parse of UTF-8 text. Invalid UTF-8 inside strings is an error. On failure Out is null. */
	DREAMCORE_API bool Parse(std::string_view Utf8, Value& Out, std::string* OutError = nullptr, const ParseOptions& Options = {});

	/** Compact serialization (no spaces), or two-space indentation when bPretty. Non-finite doubles become null. */
	DREAMCORE_API void Serialize(const Value& V, std::string& Out, bool bPretty = false);
	DREAMCORE_API std::string Serialize(const Value& V, bool bPretty = false);
}
