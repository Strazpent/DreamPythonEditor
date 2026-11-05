// SPDX-License-Identifier: Apache-2.0
// Run File requests (TDD §5.2.1, §5.3.4, §4.13). See DreamRunFile.h.
#include "Run/DreamRunFile.h"

#include "Buffers/DreamBuffer.h"
#include "Buffers/DreamBufferManager.h"
#include "Misc/FileHelper.h"
#include "Misc/Paths.h"
#include "Module/DreamEditorModule.h"
#include "Module/DreamText.h"
#include "dream/Checksum.h"
#include "dream/TextBuffer.h"
#include "dream/TextCodec.h"

#define LOCTEXT_NAMESPACE "Dream"

namespace DreamRunFile
{
	bool ReadRunnableFile(const FString& Path, FString& OutText, dream::TextSnapshot& OutSnapshot, FText& OutError, FString* OutSha256)
	{
		if (Path.IsEmpty() || !FPaths::FileExists(Path))
		{
			OutError = FText::Format(LOCTEXT("RunFileMissing", "no such file: {0}"), FText::FromString(Path));
			return false;
		}
		TArray<uint8> Bytes;
		if (!FFileHelper::LoadFileToArray(Bytes, *Path, FILEREAD_Silent))
		{
			OutError = FText::Format(LOCTEXT("RunFileUnreadable", "cannot read {0}"), FText::FromString(Path));
			return false;
		}
		// The same decode as a file tab: compile() of a str ignores a coding cookie, so the text is decoded here (§4.13).
		const dream::codec::DecodeResult Decoded = dream::codec::Decode(
			std::string_view(reinterpret_cast<const char*>(Bytes.GetData()), static_cast<size_t>(Bytes.Num())));
		if (Decoded.Refused != dream::codec::Refusal::None)
		{
			OutError = FText::Format(LOCTEXT("RunFileNotText", "{0} is not a text file Dream Python Editor can run"), FText::FromString(Path));
			return false;
		}
		const dream::TextBuffer Text(Decoded.Text);
		OutText = DreamText::ToFString(Decoded.Text);
		OutSnapshot = Text.Snapshot();
		if (OutSha256 != nullptr)
		{
			dream::Sha256 Hasher;
			Hasher.Update(Bytes.GetData(), static_cast<size_t>(Bytes.Num()));
			*OutSha256 = DreamText::Utf8ToFString(dream::Sha256::ToHex(Hasher.Final()));
		}
		return true;
	}

	bool MakeFileRequest(const FString& InPath, EDreamRunRoute Route, FDreamRunRequest& OutRequest, FText& OutError)
	{
		FString Path = FPaths::ConvertRelativePathToFull(InPath);
		FPaths::NormalizeFilename(Path);

		FDreamRunRequest Request;
		Request.Route = Route;
		Request.bFileTarget = Route == EDreamRunRoute::Api;
		Request.Namespace = EDreamNamespaceMode::Isolated;      // §5.2.1: Isolated, one exec, transaction off by default
		Request.FirstLine = 1;
		Request.TabTitle = FPaths::GetCleanFilename(Path);

		FDreamBufferManager& Buffers = FDreamEditorModule::Get().GetBufferManager();
		if (const TSharedPtr<FDreamBuffer> Buffer = Buffers.FindByPath(Path))
		{
			// An open buffer runs its text; a dirty one is saved first (Starting step 4, §5.1.2), as Run File does.
			Request.BufferId = Buffer->GetId();
			Request.FilePath = Buffer->GetPath();
			Request.Filename = Buffer->GetPath();
			Request.bDirty = Buffer->IsDirty();
			Request.SourceSnapshot = Buffer->Snapshot();
			Request.Text = DreamText::ToFString(Request.SourceSnapshot.Text());
		}
		else
		{
			if (!ReadRunnableFile(Path, Request.Text, Request.SourceSnapshot, OutError, &Request.FileSha256))
			{
				return false;
			}
			Request.FilePath = Path;
			Request.Filename = Path;
		}
		OutRequest = MoveTemp(Request);
		return true;
	}
}

#undef LOCTEXT_NAMESPACE
