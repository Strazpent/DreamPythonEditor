// SPDX-License-Identifier: Apache-2.0
// The plain-mode Python marshaller (TDD §4.15 row 3, §4.3, EDT-01): dream::PyLexer tokens become FSlateTextRuns with the
// DreamEditorStyle token colours. It never asks for live updates (re-marshalling on every keystroke is O(n)); SDreamCodeView
// marks it dirty a moment after typing stops, and the stock layout keeps cursor and selection across the refresh.
// GetText joins lines with '\n' only, so the widget's text is exactly the model's LF-only text (§4.1.2).
// Game thread. Slice-internal (shell).
#pragma once

#include "CoreMinimal.h"
#include "Framework/Text/BaseTextLayoutMarshaller.h"
#include "Styling/SlateTypes.h"
#include "dream/PyLexer.h"

class FDreamBuffer;
class FTextLayout;

class DREAMEDITOR_API FDreamPythonMarshaller final : public FBaseTextLayoutMarshaller
{
public:
	static TSharedRef<FDreamPythonMarshaller> Create(const TSharedPtr<FDreamBuffer>& InBuffer, int32 InFontSize);

	virtual ~FDreamPythonMarshaller() override = default;

	// ITextLayoutMarshaller
	virtual void SetText(const FString& SourceString, FTextLayout& TargetTextLayout) override;
	virtual void GetText(FString& TargetString, const FTextLayout& SourceTextLayout) override;

	void SetFontSize(int32 InFontSize);
	/** (theme) Rebuilds the token styles from the current theme and marks the layout dirty (a live theme switch, PO-04). */
	void RefreshStyles();
	/** The widget's text layout, captured at the first SetText (the gutter and current-line highlight read its line views). */
	const FTextLayout* GetLayout() const { return Layout; }
	/** The same layout, for a layout update right after a whole-text set (SDreamPlainText::SetWholeText). */
	FTextLayout* GetMutableLayout() const { return Layout; }
	const FTextBlockStyle& GetDefaultStyle() const { return Styles[0]; }

private:
	FDreamPythonMarshaller(const TSharedPtr<FDreamBuffer>& InBuffer, int32 InFontSize);
	void RebuildStyles();

	TWeakPtr<FDreamBuffer> Buffer;
	int32 FontSize = 10;
	FTextLayout* Layout = nullptr;
	TArray<FTextBlockStyle> Styles;     // indexed by dream::TokenKind
};
