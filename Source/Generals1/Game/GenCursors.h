#pragma once

#include "CoreMinimal.h"

class APlayerController;

/** One of the game's cursor files (Data\Cursors\*.ani), decoded: its pictures, and the order and pace they are shown in. */
struct GENERALS1_API FGenCursorImage
{
	struct FStep
	{
		int32 Frame = 0;
		float Seconds = 0.f;
	};
	FString File;                      // "SCCSelect"
	int32 Width = 0, Height = 0;
	FIntPoint HotSpot = FIntPoint::ZeroValue; // the pixel that is "where the cursor is"
	TArray<TArray<FColor>> Frames;
	TArray<FStep> Steps;
	float Duration = 0.f;              // of one run through the steps

	/** The step showing that long after the cursor came up. */
	int32 StepAt(double Seconds) const;

	// The platform's cursors made of the frames (when first shown, at the scale wanted then).
	TArray<void*> Handles;
	int32 HandleScale = 0;
};

/**
 * The mouse cursors: Mouse.ini says which picture each kind of cursor has ("Select" -> SCCSelect, "Scroll"
 * in eight directions), the pictures are the animated cursor files the original hands to Windows
 * (Win32Mouse: LoadCursorFromFile("data\\cursors\\%s.ANI")). Here they are read, made into the
 * platform's cursors frame by frame - so they can be shown at twice or three times their 32 pixels on
 * a screen with many more than the original's - and animated by the file's own timing.
 */
class FGenCursors
{
public:
	static FGenCursors& Get();
	FGenCursors() = default;
	FGenCursors(const FGenCursors&) = delete;
	FGenCursors& operator=(const FGenCursors&) = delete;

	/** The picture of a kind of cursor by its Mouse.ini name (Mouse::getCursorIndex's names); none: it has no picture. */
	const FGenCursorImage* Find(const FString& Name, int32 Direction = 0);
	/** How many directions that kind has pictures for (1, or the scroll cursor's 8). */
	int32 GetNumDirections(const FString& Name);

	/**
	 * Shows that cursor from now on (called every frame: the animation goes on from where it was, and
	 * starts over when the kind changes). A kind without a picture hides the cursor, as it does in the
	 * original ("Build": the building itself is under the mouse).
	 */
	void Show(APlayerController* PC, const FString& Name, int32 Direction, int32 ViewportHeight);

	/** What is showing: for the log, and for drawing it into a picture where there is no mouse. */
	const FString& GetCurrentName() const { return CurrentName; }
	const FGenCursorImage* GetCurrent(int32& OutFrame) const;
	/** How many screen pixels a pixel of the cursor takes, on a screen that high (gen.CursorScale: 0 = by the screen). */
	static int32 GetScale(int32 ViewportHeight);

private:
	void Initialize();
	FGenCursorImage* Load(const FString& File);
	static bool Decode(const TArray<uint8>& Data, FGenCursorImage& Out);

	struct FKind
	{
		FString Image;
		int32 Directions = 1;
	};
	bool bInitialized = false;
	TMap<FString, FKind> Kinds;                         // key: lower case Mouse.ini name
	TMap<FString, TUniquePtr<FGenCursorImage>> Images;  // key: lower case file name (an empty one: no such file)

	FString CurrentName;
	FGenCursorImage* Current = nullptr;
	int32 CurrentFrame = INDEX_NONE;
	void* CurrentHandle = nullptr;
	double CurrentSince = 0.0;
};
