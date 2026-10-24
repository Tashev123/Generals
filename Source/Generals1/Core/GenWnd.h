#pragma once

#include "CoreMinimal.h"

/** One of the nine images a window state draws with (WinDrawData: image, colour, border colour). */
struct GENERALS1_API FGenWndDrawData
{
	FString Image;                     // a mapped image; "NoImage" read as empty
	FColor Color = FColor(255, 255, 255, 255);
	FColor BorderColor = FColor(0, 0, 0, 255);
};

/**
 * A window of a .wnd layout (GameWindowManagerScript.cpp): its place in the layout's creation resolution,
 * its status bits, the draw data of its three states and its text, with its children.
 */
struct GENERALS1_API FGenWndWindow
{
	FString Name;                      // "ControlBar.wnd:ButtonCommand01"
	FString ShortName;                 // "ButtonCommand01"
	FString Type;                      // USER, PUSHBUTTON, STATICTEXT, PROGRESSBAR, ...
	FBox2D Rect = FBox2D(ForceInit);   // UPPERLEFT .. BOTTOMRIGHT (inclusive pixels), in CreationRes
	FIntPoint CreationRes = FIntPoint(800, 600);
	TArray<FString> Status;            // ENABLED, HIDDEN, IMAGE, SEE_THRU, NOINPUT, ...
	TArray<FGenWndDrawData> Enabled, Disabled, Hilite;
	FString Text;                      // a string table label ("GUI:...") or plain text
	FString Tooltip;
	FString FontName;
	int32 FontSize = 0;
	bool bBold = false;
	FColor TextColor = FColor::White;  // ENABLED
	FColor DisabledTextColor = FColor::White;
	FColor HiliteTextColor = FColor::White;
	TArray<TSharedPtr<FGenWndWindow>> Children;
	const FGenWndWindow* Parent = nullptr;

	bool HasStatus(const TCHAR* Bit) const;
	/** The first enabled image, if there is one. */
	const FString& GetImage() const;
	/** This window or one below it by its short name. */
	const FGenWndWindow* Find(const FString& InShortName) const;
};

/** A parsed .wnd file. */
struct GENERALS1_API FGenWndLayout
{
	FString FileName;
	TArray<TSharedPtr<FGenWndWindow>> Roots;

	const FGenWndWindow* Find(const FString& ShortName) const;
	/** Every window that would take the mouse (shown, neither NOINPUT nor SEE_THRU, under shown parents). */
	void GetInputRects(TArray<FBox2D>& Out) const;
};

namespace GenWnd
{
	/** "ControlBar.wnd" -> the parsed Window\ControlBar.wnd of the game's files (cached; nullptr if missing). */
	GENERALS1_API const FGenWndLayout* Load(const FString& FileName);
	/** Parses .wnd text. */
	GENERALS1_API TSharedPtr<FGenWndLayout> Parse(const FString& Text, const FString& FileName);
}
