#include "Game/GenCursors.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Framework/Application/SlateApplication.h"
#include "GameFramework/PlayerController.h"
#include "GenericPlatform/ICursor.h"
#include "Generals1.h"

static TAutoConsoleVariable<int32> CVarGenCursorScale(
	TEXT("gen.CursorScale"), 0,
	TEXT("How many screen pixels a pixel of the game's 32 x 32 cursors takes. 0: by the height of the screen (1 up to 1200 lines, 2 up to 2000, 3 above)."));

static TAutoConsoleVariable<int32> CVarGenCursors(
	TEXT("gen.Cursors"), 1,
	TEXT("1: the game's own cursors (Data\\Cursors). 0: the system's arrow."));

FGenCursors& FGenCursors::Get()
{
	static FGenCursors Instance;
	return Instance;
}

int32 FGenCursorImage::StepAt(double Seconds) const
{
	if (Steps.Num() <= 1 || Duration <= 0.f)
	{
		return 0;
	}
	float T = float(FMath::Fmod(Seconds, double(Duration)));
	for (int32 i = 0; i < Steps.Num(); ++i)
	{
		if (T < Steps[i].Seconds)
		{
			return i;
		}
		T -= Steps[i].Seconds;
	}
	return Steps.Num() - 1;
}

int32 FGenCursors::GetScale(int32 ViewportHeight)
{
	const int32 Asked = CVarGenCursorScale.GetValueOnGameThread();
	if (Asked > 0)
	{
		return FMath::Min(Asked, 8);
	}
	return ViewportHeight > 2000 ? 3 : (ViewportHeight > 1200 ? 2 : 1);
}

void FGenCursors::Initialize()
{
	if (bInitialized)
	{
		return;
	}
	bInitialized = true;
	// Mouse.ini: "MouseCursor Select / Image = SCCSelect / Directions = 8".
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\Mouse.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\Mouse.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("MouseCursor"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FKind& Kind = Kinds.FindOrAdd(B->Name().ToLower());
		// (Win32Mouse takes the name the texture has; the two are the same in every entry.)
		Kind.Image = B->GetString(TEXT("Texture"));
		if (Kind.Image.IsEmpty())
		{
			Kind.Image = B->GetString(TEXT("Image"));
		}
		Kind.Directions = FMath::Clamp(int32(B->GetFloat(TEXT("Directions"), 1.f)), 1, 16);
	}
	UE_LOG(LogGenerals, Log, TEXT("Cursors: %d kinds in Mouse.ini"), Kinds.Num());
}

int32 FGenCursors::GetNumDirections(const FString& Name)
{
	Initialize();
	const FKind* Kind = Kinds.Find(Name.ToLower());
	return Kind ? Kind->Directions : 1;
}

const FGenCursorImage* FGenCursors::Find(const FString& Name, int32 Direction)
{
	Initialize();
	const FKind* Kind = Kinds.Find(Name.ToLower());
	if (!Kind || Kind->Image.IsEmpty())
	{
		return nullptr;
	}
	// Win32Mouse::initCursorResources: "data\\cursors\\%s%d.ANI" for a cursor with directions.
	const FGenCursorImage* Image = Load(Kind->Directions > 1 ? FString::Printf(TEXT("%s%d"), *Kind->Image, FMath::Clamp(Direction, 0, Kind->Directions - 1)) : Kind->Image);
	return Image && Image->Frames.Num() > 0 ? Image : nullptr;
}

FGenCursorImage* FGenCursors::Load(const FString& File)
{
	const FString Key = File.ToLower();
	if (const TUniquePtr<FGenCursorImage>* Known = Images.Find(Key))
	{
		return Known->Get();
	}
	TUniquePtr<FGenCursorImage>& Slot = Images.Add(Key, MakeUnique<FGenCursorImage>());
	Slot->File = File;
	TArray<uint8> Data;
	if (!FGenFileSystem::Get().ReadFile(TEXT("Data\\Cursors\\") + File + TEXT(".ani"), Data) || !Decode(Data, *Slot))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Cursors: %s.ani is missing or can't be read"), *File);
		Slot->Frames.Reset();
	}
	return Slot.Get();
}

namespace
{
	uint32 ReadU32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }
	uint16 ReadU16(const uint8* P) { return uint16(uint32(P[0]) | (uint32(P[1]) << 8)); }

	/** One frame: a .cur file - a directory entry with the hot spot, then a bitmap that is the colours with the mask under them. */
	bool DecodeCur(const uint8* P, int32 Size, FGenCursorImage& Out)
	{
		if (Size < 6 + 16 + 40)
		{
			return false;
		}
		const int32 HotX = ReadU16(P + 10), HotY = ReadU16(P + 12);
		const int32 Offset = int32(ReadU32(P + 18));
		if (Offset < 22 || Offset + 40 > Size)
		{
			return false;
		}
		const uint8* H = P + Offset;
		const int32 HeaderSize = int32(ReadU32(H));
		const int32 W = int32(ReadU32(H + 4));
		const int32 H2 = int32(ReadU32(H + 8)) / 2; // "height" counts the mask too
		const int32 Bits = ReadU16(H + 14);
		if (W <= 0 || H2 <= 0 || W > 256 || H2 > 256 || ReadU32(H + 16) != 0 || (Bits != 1 && Bits != 4 && Bits != 8 && Bits != 24 && Bits != 32))
		{
			return false;
		}
		int32 NumColors = Bits <= 8 ? int32(ReadU32(H + 32)) : 0;
		if (Bits <= 8 && NumColors == 0)
		{
			NumColors = 1 << Bits;
		}
		const uint8* Palette = H + HeaderSize;
		const uint8* Colors = Palette + NumColors * 4;
		const int32 ColorStride = ((W * Bits + 31) / 32) * 4;
		const int32 MaskStride = ((W + 31) / 32) * 4;
		const uint8* Mask = Colors + ColorStride * H2;
		if (Mask + MaskStride * H2 > P + Size)
		{
			return false;
		}
		if (Out.Frames.Num() == 0)
		{
			Out.Width = W;
			Out.Height = H2;
			Out.HotSpot = FIntPoint(FMath::Clamp(HotX, 0, W - 1), FMath::Clamp(HotY, 0, H2 - 1));
		}
		else if (W != Out.Width || H2 != Out.Height)
		{
			return false;
		}
		TArray<FColor>& Pixels = Out.Frames.AddDefaulted_GetRef();
		Pixels.SetNumUninitialized(W * H2);
		for (int32 y = 0; y < H2; ++y)
		{
			// (Rows are stored from the bottom up.)
			const uint8* Row = Colors + ColorStride * (H2 - 1 - y);
			const uint8* MaskRow = Mask + MaskStride * (H2 - 1 - y);
			for (int32 x = 0; x < W; ++x)
			{
				FColor C(0, 0, 0, 255);
				if (Bits <= 8)
				{
					const int32 Index = Bits == 8 ? Row[x] : Bits == 4 ? ((Row[x >> 1] >> ((x & 1) ? 0 : 4)) & 15) : ((Row[x >> 3] >> (7 - (x & 7))) & 1);
					if (Index < NumColors)
					{
						C = FColor(Palette[Index * 4 + 2], Palette[Index * 4 + 1], Palette[Index * 4 + 0], 255);
					}
				}
				else
				{
					const uint8* Px = Row + x * (Bits / 8);
					C = FColor(Px[2], Px[1], Px[0], Bits == 32 ? Px[3] : 255);
				}
				// A set bit in the mask: the screen shows through.
				if ((MaskRow[x >> 3] >> (7 - (x & 7))) & 1)
				{
					C = FColor(0, 0, 0, 0);
				}
				Pixels[y * W + x] = C;
			}
		}
		return true;
	}
}

bool FGenCursors::Decode(const TArray<uint8>& Data, FGenCursorImage& Out)
{
	// A RIFF file of type ACON: "anih" (frames, steps, the pace in sixtieths of a second), maybe "rate"
	// (a pace for each step) and "seq " (which frame each step shows), and the list "fram" of "icon"s.
	const uint8* P = Data.GetData();
	const int32 Num = Data.Num();
	if (Num < 12 || FMemory::Memcmp(P, "RIFF", 4) != 0 || FMemory::Memcmp(P + 8, "ACON", 4) != 0)
	{
		return false;
	}
	uint32 Pace = 10, NumSteps = 0;
	TArray<uint32> Rates, Sequence;
	for (int32 At = 12; At + 8 <= Num;)
	{
		const uint8* Chunk = P + At;
		const int32 Size = int32(ReadU32(Chunk + 4));
		if (Size < 0 || At + 8 + Size > Num)
		{
			break;
		}
		const uint8* Body = Chunk + 8;
		if (FMemory::Memcmp(Chunk, "anih", 4) == 0 && Size >= 36)
		{
			NumSteps = ReadU32(Body + 8);
			Pace = ReadU32(Body + 28);
		}
		else if (FMemory::Memcmp(Chunk, "rate", 4) == 0)
		{
			for (int32 i = 0; i + 4 <= Size; i += 4) { Rates.Add(ReadU32(Body + i)); }
		}
		else if (FMemory::Memcmp(Chunk, "seq ", 4) == 0)
		{
			for (int32 i = 0; i + 4 <= Size; i += 4) { Sequence.Add(ReadU32(Body + i)); }
		}
		else if (FMemory::Memcmp(Chunk, "LIST", 4) == 0 && Size >= 4 && FMemory::Memcmp(Body, "fram", 4) == 0)
		{
			for (int32 In = 4; In + 8 <= Size;)
			{
				const int32 IconSize = int32(ReadU32(Body + In + 4));
				if (IconSize < 0 || In + 8 + IconSize > Size)
				{
					break;
				}
				if (FMemory::Memcmp(Body + In, "icon", 4) == 0 && !DecodeCur(Body + In + 8, IconSize, Out))
				{
					return false;
				}
				In += 8 + IconSize + (IconSize & 1);
			}
		}
		At += 8 + Size + (Size & 1);
	}
	if (Out.Frames.Num() == 0)
	{
		return false;
	}
	const int32 Steps = Sequence.Num() > 0 ? Sequence.Num() : (NumSteps > 0 ? int32(NumSteps) : Out.Frames.Num());
	Out.Duration = 0.f;
	for (int32 i = 0; i < Steps; ++i)
	{
		FGenCursorImage::FStep Step;
		Step.Frame = FMath::Clamp(Sequence.IsValidIndex(i) ? int32(Sequence[i]) : i, 0, Out.Frames.Num() - 1);
		Step.Seconds = float(FMath::Max(1u, Rates.IsValidIndex(i) ? Rates[i] : Pace)) / 60.f;
		Out.Duration += Step.Seconds;
		Out.Steps.Add(Step);
	}
	return true;
}

const FGenCursorImage* FGenCursors::GetCurrent(int32& OutFrame) const
{
	OutFrame = CurrentFrame;
	return Current && Current->Frames.IsValidIndex(CurrentFrame) ? Current : nullptr;
}

void FGenCursors::Show(APlayerController* PC, const FString& Name, int32 Direction, int32 ViewportHeight)
{
	if (!PC)
	{
		return;
	}
	if (CVarGenCursors.GetValueOnGameThread() == 0)
	{
		PC->CurrentMouseCursor = EMouseCursor::Default;
		CurrentName.Reset();
		Current = nullptr;
		return;
	}
	const double Now = FPlatformTime::Seconds();
	FGenCursorImage* Image = const_cast<FGenCursorImage*>(Find(Name, Direction));
	if (Image != Current || !Name.Equals(CurrentName, ESearchCase::IgnoreCase))
	{
		// (Another kind with the same picture - "AttackObj" after "Target" - carries on where the picture was.)
		if (Image != Current)
		{
			CurrentSince = Now;
			CurrentFrame = INDEX_NONE;
		}
		Current = Image;
		CurrentName = Name;
	}
	if (!Image)
	{
		// No picture: no cursor ("Build" - the building being placed is under the mouse instead).
		PC->CurrentMouseCursor = EMouseCursor::None;
		return;
	}
	const int32 Frame = Image->Steps.IsValidIndex(0) ? Image->Steps[Image->StepAt(Now - CurrentSince)].Frame : 0;
	const int32 Scale = GetScale(ViewportHeight);
	TSharedPtr<ICursor> Platform = FSlateApplication::IsInitialized() ? FSlateApplication::Get().GetPlatformCursor() : nullptr;
	if (Platform.IsValid() && Platform->IsCreateCursorFromRGBABufferSupported())
	{
		if (Image->HandleScale != Scale)
		{
			// The frames as cursors of the platform's, each pixel Scale by Scale (nothing smoothed: they are drawn pixel by pixel).
			Image->HandleScale = Scale;
			Image->Handles.Reset();
			const int32 W = Image->Width * Scale, H = Image->Height * Scale;
			TArray<FColor> Big;
			Big.SetNumUninitialized(W * H);
			const FVector2D Hot((double(Image->HotSpot.X) + (Scale > 1 ? 0.5 : 0.0)) / double(Image->Width), (double(Image->HotSpot.Y) + (Scale > 1 ? 0.5 : 0.0)) / double(Image->Height));
			for (const TArray<FColor>& Pixels : Image->Frames)
			{
				for (int32 y = 0; y < H; ++y)
				{
					for (int32 x = 0; x < W; ++x)
					{
						// (The platform wants the bytes red first: an FColor has them blue first.)
						const FColor C = Pixels[(y / Scale) * Image->Width + (x / Scale)];
						Big[y * W + x] = FColor(C.B, C.G, C.R, C.A);
					}
				}
				Image->Handles.Add(Platform->CreateCursorFromRGBABuffer(Big.GetData(), W, H, Hot));
			}
			UE_LOG(LogGenerals, Log, TEXT("Cursors: %s.ani - %d frame(s), %d step(s) in %.2f s, hot spot (%d, %d) - made into the platform's at x%d (the view is %d high)%s"), *Image->File,
				Image->Frames.Num(), Image->Steps.Num(), Image->Duration, Image->HotSpot.X, Image->HotSpot.Y, Scale, ViewportHeight, Image->Handles.Contains(nullptr) ? TEXT(": FAILED") : TEXT(""));
		}
		void* Handle = Image->Handles.IsValidIndex(Frame) ? Image->Handles[Frame] : nullptr;
		if (Handle && Handle != CurrentHandle)
		{
			CurrentHandle = Handle;
			Platform->SetTypeShape(EMouseCursor::Custom, Handle);
		}
		PC->CurrentMouseCursor = Handle ? EMouseCursor::Custom : EMouseCursor::Default;
	}
	else
	{
		static bool bSaid = false;
		UE_CLOG(!bSaid, LogGenerals, Log, TEXT("Cursors: no platform cursor here (the pictures are still worked out; the view is %d high)"), ViewportHeight);
		bSaid = true;
	}
	CurrentFrame = Frame;
}
