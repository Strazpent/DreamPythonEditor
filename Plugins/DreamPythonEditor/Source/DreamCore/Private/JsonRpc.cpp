// SPDX-License-Identifier: Apache-2.0
// Dream core: JSON-RPC framing and message helpers (TDD §7.2.2). See dream/JsonRpc.h.
#include "dream/JsonRpc.h"

#include <algorithm>

namespace dream::JsonRpc
{
	namespace
	{
		char AsciiLower(char C)
		{
			return (C >= 'A' && C <= 'Z') ? static_cast<char>(C - 'A' + 'a') : C;
		}

		bool EqualsNoCase(std::string_view A, std::string_view B)
		{
			if (A.size() != B.size())
			{
				return false;
			}
			for (size_t Index = 0; Index < A.size(); ++Index)
			{
				if (AsciiLower(A[Index]) != AsciiLower(B[Index]))
				{
					return false;
				}
			}
			return true;
		}

		std::string_view Trim(std::string_view Text)
		{
			while (!Text.empty() && (Text.front() == ' ' || Text.front() == '\t'))
			{
				Text.remove_prefix(1);
			}
			while (!Text.empty() && (Text.back() == ' ' || Text.back() == '\t'))
			{
				Text.remove_suffix(1);
			}
			return Text;
		}

		int HexValue(char C)
		{
			if (C >= '0' && C <= '9')
			{
				return C - '0';
			}
			if (C >= 'a' && C <= 'f')
			{
				return C - 'a' + 10;
			}
			if (C >= 'A' && C <= 'F')
			{
				return C - 'A' + 10;
			}
			return -1;
		}

		bool IsUnreserved(unsigned char C)
		{
			return (C >= 'A' && C <= 'Z') || (C >= 'a' && C <= 'z') || (C >= '0' && C <= '9') || C == '-' || C == '.' ||
				C == '_' || C == '~';
		}
	}

	const char* FrameErrorName(FrameError Error)
	{
		switch (Error)
		{
		case FrameError::None: return "None";
		case FrameError::MissingLength: return "MissingLength";
		case FrameError::BadLength: return "BadLength";
		case FrameError::HeaderTooLong: return "HeaderTooLong";
		case FrameError::FrameTooLarge: return "FrameTooLarge";
		case FrameError::BadJson: return "BadJson";
		}
		return "Unknown";
	}

	FrameDecoder::FrameDecoder(const FrameLimits& InLimits)
		: Limits(InLimits)
	{
	}

	void FrameDecoder::Reset()
	{
		Error = FrameError::None;
		bInBody = false;
		BodyLength = 0;
		Header.clear();
		Body.clear();
	}

	FrameError FrameDecoder::ParseHeader()
	{
		// Header is "Name: value\r\n...\r\n\r\n". Only Content-Length matters (§7.2.2); the first one wins.
		std::string_view Rest(Header);
		Rest.remove_suffix(4);
		bool bFound = false;
		size_t Length = 0;
		while (!Rest.empty() && !bFound)
		{
			const size_t LineEnd = Rest.find("\r\n");
			const std::string_view Line = Rest.substr(0, LineEnd);
			Rest = LineEnd == std::string_view::npos ? std::string_view() : Rest.substr(LineEnd + 2);
			const size_t Colon = Line.find(':');
			if (Colon == std::string_view::npos || !EqualsNoCase(Trim(Line.substr(0, Colon)), "content-length"))
			{
				continue;
			}
			const std::string_view Digits = Trim(Line.substr(Colon + 1));
			if (Digits.empty())
			{
				return FrameError::BadLength;
			}
			for (const char C : Digits)
			{
				if (C < '0' || C > '9')
				{
					return FrameError::BadLength;
				}
				// Refused before it can overflow or allocate: a length above the cap is FrameTooLarge at once.
				Length = Length * 10 + static_cast<size_t>(C - '0');
				if (Length > Limits.MaxFrameBytes)
				{
					return FrameError::FrameTooLarge;
				}
			}
			bFound = true;
		}
		if (!bFound)
		{
			return FrameError::MissingLength;
		}
		BodyLength = Length;
		return FrameError::None;
	}

	FrameError FrameDecoder::Feed(std::span<const uint8_t> Bytes, std::vector<json::Value>& Out)
	{
		size_t At = 0;
		while (Error == FrameError::None && At < Bytes.size())
		{
			if (!bInBody)
			{
				// Byte by byte: the header is small and the terminator may straddle chunks.
				while (At < Bytes.size())
				{
					Header.push_back(static_cast<char>(Bytes[At++]));
					const size_t Size = Header.size();
					if (Size >= 4 && Header[Size - 4] == '\r' && Header[Size - 3] == '\n' && Header[Size - 2] == '\r' &&
						Header[Size - 1] == '\n')
					{
						Error = ParseHeader();
						Header.clear();
						if (Error != FrameError::None)
						{
							return Error;
						}
						bInBody = true;
						Body.clear();
						Body.reserve(BodyLength);
						break;
					}
					if (Size >= Limits.MaxHeaderBytes)
					{
						Error = FrameError::HeaderTooLong;
						Header.clear();
						return Error;
					}
				}
				if (!bInBody)
				{
					break;
				}
			}
			const size_t Take = std::min(BodyLength - Body.size(), Bytes.size() - At);
			Body.append(reinterpret_cast<const char*>(Bytes.data() + At), Take);
			At += Take;
			if (Body.size() == BodyLength)
			{
				json::ParseOptions Options;
				Options.MaxDepth = Limits.MaxJsonDepth;
				Options.MaxBytes = std::max(Limits.MaxFrameBytes, size_t(1));
				json::Value Message;
				if (!json::Parse(Body, Message, nullptr, Options))
				{
					Error = FrameError::BadJson;
					Body.clear();
					Body.shrink_to_fit();
					return Error;
				}
				Out.push_back(std::move(Message));
				bInBody = false;
				BodyLength = 0;
				Body.clear();
				if (Body.capacity() > (size_t(1) << 20))
				{
					Body.shrink_to_fit();       // one large frame does not pin its buffer for the server's lifetime
				}
			}
		}
		return Error;
	}

	std::vector<uint8_t> EncodeFrame(const json::Value& Message)
	{
		const std::string Body = json::Serialize(Message);
		const std::string Head = "Content-Length: " + std::to_string(Body.size()) + "\r\n\r\n";
		std::vector<uint8_t> Frame;
		Frame.reserve(Head.size() + Body.size());
		Frame.insert(Frame.end(), Head.begin(), Head.end());
		Frame.insert(Frame.end(), Body.begin(), Body.end());
		return Frame;
	}

	MessageKind Classify(const json::Value& Message)
	{
		if (!Message.IsObject())
		{
			return MessageKind::Invalid;
		}
		const json::Value* Method = Message.Find("method");
		const json::Value* Id = Message.Find("id");
		const bool bIdOk = Id != nullptr && (Id->IsInteger() || Id->IsString() || Id->GetKind() == json::Kind::Number);
		if (Method != nullptr)
		{
			if (!Method->IsString())
			{
				return MessageKind::Invalid;
			}
			if (Id == nullptr)
			{
				return MessageKind::Notification;
			}
			return bIdOk ? MessageKind::Request : MessageKind::Invalid;
		}
		if (Id != nullptr && (Message.Find("result") != nullptr || Message.Find("error") != nullptr))
		{
			return (bIdOk || Id->IsNull()) ? MessageKind::Response : MessageKind::Invalid;
		}
		return MessageKind::Invalid;
	}

	json::Value MakeRequest(int64_t Id, std::string_view Method, json::Value Params)
	{
		json::Value M = json::Value::MakeObject();
		M.Set("jsonrpc", json::Value::MakeString("2.0"));
		M.Set("id", json::Value::MakeInt(Id));
		M.Set("method", json::Value::MakeString(std::string(Method)));
		if (!Params.IsNull())
		{
			M.Set("params", std::move(Params));
		}
		return M;
	}

	json::Value MakeNotification(std::string_view Method, json::Value Params)
	{
		json::Value M = json::Value::MakeObject();
		M.Set("jsonrpc", json::Value::MakeString("2.0"));
		M.Set("method", json::Value::MakeString(std::string(Method)));
		if (!Params.IsNull())
		{
			M.Set("params", std::move(Params));
		}
		return M;
	}

	json::Value MakeResult(const json::Value& Id, json::Value Result)
	{
		json::Value M = json::Value::MakeObject();
		M.Set("jsonrpc", json::Value::MakeString("2.0"));
		M.Set("id", Id);
		M.Set("result", std::move(Result));
		return M;
	}

	json::Value MakeError(const json::Value& Id, int64_t Code, std::string_view Message)
	{
		json::Value Error = json::Value::MakeObject();
		Error.Set("code", json::Value::MakeInt(Code));
		Error.Set("message", json::Value::MakeString(std::string(Message)));
		json::Value M = json::Value::MakeObject();
		M.Set("jsonrpc", json::Value::MakeString("2.0"));
		M.Set("id", Id);
		M.Set("error", std::move(Error));
		return M;
	}

	std::string PathToFileUri(std::string_view PathUtf8)
	{
		static constexpr char Hex[] = "0123456789ABCDEF";
		std::string Path(PathUtf8);
		std::replace(Path.begin(), Path.end(), '\\', '/');
		std::string Uri = "file://";
		if (Path.empty() || Path.front() != '/')
		{
			Uri.push_back('/');
		}
		const bool bDrive = Path.size() >= 2 && Path[1] == ':' &&
			((Path[0] >= 'A' && Path[0] <= 'Z') || (Path[0] >= 'a' && Path[0] <= 'z'));
		for (size_t Index = 0; Index < Path.size(); ++Index)
		{
			const unsigned char C = static_cast<unsigned char>(Path[Index]);
			if (IsUnreserved(C) || C == '/' || (bDrive && Index == 1))
			{
				Uri.push_back(static_cast<char>(C));
			}
			else
			{
				Uri.push_back('%');
				Uri.push_back(Hex[C >> 4]);
				Uri.push_back(Hex[C & 0xF]);
			}
		}
		return Uri;
	}

	bool FileUriToPath(std::string_view Uri, std::string& OutPathUtf8)
	{
		OutPathUtf8.clear();
		if (Uri.size() < 7 || !EqualsNoCase(Uri.substr(0, 7), "file://"))
		{
			return false;
		}
		std::string_view Rest = Uri.substr(7);
		// An authority (host) is not supported; "file:///C:/x" and "file:///home/x" are.
		if (Rest.empty() || Rest.front() != '/')
		{
			return false;
		}
		std::string Decoded;
		Decoded.reserve(Rest.size());
		for (size_t Index = 0; Index < Rest.size(); ++Index)
		{
			const char C = Rest[Index];
			if (C == '%')
			{
				if (Index + 2 >= Rest.size())
				{
					return false;
				}
				const int High = HexValue(Rest[Index + 1]);
				const int Low = HexValue(Rest[Index + 2]);
				if (High < 0 || Low < 0)
				{
					return false;
				}
				Decoded.push_back(static_cast<char>((High << 4) | Low));
				Index += 2;
			}
			else
			{
				Decoded.push_back(C);
			}
		}
		// "/C:/x" -> "C:/x" (a Windows drive); "/home/x" stays.
		if (Decoded.size() >= 3 && Decoded[0] == '/' && Decoded[2] == ':' &&
			((Decoded[1] >= 'A' && Decoded[1] <= 'Z') || (Decoded[1] >= 'a' && Decoded[1] <= 'z')))
		{
			Decoded.erase(0, 1);
		}
		OutPathUtf8 = std::move(Decoded);
		return true;
	}

	bool SameFileUri(std::string_view A, std::string_view B)
	{
		if (A == B)
		{
			return true;
		}
		std::string PathA;
		std::string PathB;
		if (!FileUriToPath(A, PathA) || !FileUriToPath(B, PathB))
		{
			return false;
		}
		return EqualsNoCase(PathA, PathB);
	}
}
