// SPDX-License-Identifier: Apache-2.0
#include "AnimTKBrushes.h"

#include "Brushes/SlateDynamicImageBrush.h"
#include "Brushes/SlateImageBrush.h"
#include "HAL/FileManager.h"
#include "HAL/PlatformTime.h"
#include "Misc/Paths.h"
#include "Styling/AppStyle.h"
#include "Styling/CoreStyle.h"

namespace AnimTK
{
	static TMap<FString, TSharedPtr<FSlateBrush>>& Cache()
	{
		static TMap<FString, TSharedPtr<FSlateBrush>> Brushes;
		return Brushes;
	}

	const FSlateBrush* ResolveBrush(const FString& NameOrPath, const FVector2D& Size)
	{
		if (NameOrPath.IsEmpty())
		{
			return nullptr;
		}

		const bool bLooksLikeFile = NameOrPath.EndsWith(TEXT(".png"), ESearchCase::IgnoreCase) || NameOrPath.EndsWith(TEXT(".svg"), ESearchCase::IgnoreCase);
		if (bLooksLikeFile)
		{
			// Image widgets ask every frame; look at the file at most every two seconds.
			struct FRecent { double Time = 0.0; const FSlateBrush* Brush = nullptr; };
			static TMap<FString, FRecent> Recent;
			const FString RecentKey = FString::Printf(TEXT("%s|%.0fx%.0f"), *NameOrPath, Size.X, Size.Y);
			const double Now = FPlatformTime::Seconds();
			if (const FRecent* Hit = Recent.Find(RecentKey))
			{
				if (Now - Hit->Time < 2.0)
				{
					return Hit->Brush;
				}
			}

			const FString Path = FPaths::ConvertRelativePathToFull(NameOrPath);
			const FDateTime Stamp = IFileManager::Get().GetTimeStamp(*Path);
			if (Stamp == FDateTime::MinValue())
			{
				Recent.Add(RecentKey, { Now, nullptr });
				return nullptr;
			}
			const FString Key = FString::Printf(TEXT("%s|%.0fx%.0f|%lld"), *Path, Size.X, Size.Y, Stamp.GetTicks());
			if (const TSharedPtr<FSlateBrush>* Found = Cache().Find(Key))
			{
				Recent.Add(RecentKey, { Now, Found->Get() });
				return Found->Get();
			}
			TSharedPtr<FSlateBrush> Brush;
			if (Path.EndsWith(TEXT(".svg"), ESearchCase::IgnoreCase))
			{
				Brush = MakeShared<FSlateVectorImageBrush>(Path, Size);
			}
			else
			{
				// A dynamic brush loads the file on demand; a plain FSlateImageBrush only draws images a style set
				// registered at start-up (anything else draws white). Slate caches by file name, so a tool that
				// re-captures an image writes it under a new name (the pose library does).
				Brush = MakeShared<FSlateDynamicImageBrush>(FName(*Path), Size);
			}
			Cache().Add(Key, Brush);
			Recent.Add(RecentKey, { Now, Brush.Get() });
			return Brush.Get();
		}

		if (const FSlateBrush* StyleBrush = FAppStyle::Get().GetOptionalBrush(FName(*NameOrPath), nullptr, nullptr))
		{
			return StyleBrush;
		}
		return FCoreStyle::Get().GetOptionalBrush(FName(*NameOrPath), nullptr, nullptr);
	}

	void ClearBrushCache()
	{
		Cache().Empty();
	}
}
