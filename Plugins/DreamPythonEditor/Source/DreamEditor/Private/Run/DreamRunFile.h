// SPDX-License-Identifier: Apache-2.0
// Run File requests (TDD §5.2.1 Run File and API rows, §5.3.4, §4.13): one place that turns a path into a request, used by
// Dream.RunFile, dream_editor.run(<path>) and a re-run whose recorded snapshot is empty. Game thread. Body: DreamRunFile.cpp.
#pragma once

#include "CoreMinimal.h"
#include "Run/DreamRunTypes.h"

namespace DreamRunFile
{
	/**
	 * Reads a file Dream can run: the bytes decoded with dream::codec::Decode (BOM, PEP 263 coding cookie, §4.13), as LF-only
	 * text and a snapshot numbered from 1. False for a missing, unreadable or non-text file; OutError says which.
	 * OutSha256, when given, receives the SHA-256 of the file's bytes (the audit's code.sha256 for Run File, §11.4).
	 */
	DREAMEDITOR_API bool ReadRunnableFile(const FString& Path, FString& OutText, dream::TextSnapshot& OutSnapshot, FText& OutError,
		FString* OutSha256 = nullptr);

	/**
	 * A request with Run File semantics for Path: route RunFile (Dream.RunFile, the toolbar), or route Api with bFileTarget
	 * (dream_editor.run(<path>)). Isolated, one exec, argv empty (the runner sets sys.argv = [path], RUN-14 arguments are
	 * Later). A buffer open on that path runs its text, and a dirty one is saved at Starting step 4 (§5.1.2); otherwise the
	 * file is read with ReadRunnableFile. The caller sets the namespace and transaction it wants on top.
	 */
	DREAMEDITOR_API bool MakeFileRequest(const FString& Path, EDreamRunRoute Route, FDreamRunRequest& OutRequest, FText& OutError);
}
