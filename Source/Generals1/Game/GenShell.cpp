// The shell: the skirmish menu (SkirmishGameOptionsMenu.cpp, with the map selection of
// SkirmishMapSelectMenu.cpp), the in-game menu (QuitMenu.cpp) and the score screen (ScoreScreen.cpp).
// Drawn by the HUD in immediate mode: each element is drawn and asked whether it was clicked in one go.
//
// Ids for test automation (-GenTestMenu="seconds,click,Id;..."):
//   SelectMap, Map=<name>, MapAccept, MapBack           the map selection
//   Slot<N>.State / .Color / .Side / .Team              opens that list of slot N (0 = the player's);
//   Slot<N>.State=<item> ...                            ... or chooses the item with that label at once
//   Cash, Cash=<item>, TestUnits                        starting cash, the battle group switch
//   Start<N>                                            a left click on start position N (1-based)
//   Play, Exit                                          the menu's buttons
//   Menu.Open, Menu.Resume, Menu.Restart, Menu.Exit     the in-game menu
//   Score.Continue, Score.Watch                         the score screen

#include "Game/GenHUD.h"
#include "Audio/GenAudioData.h"
#include "Audio/GenAudioManager.h"
#include "Core/GenFileSystem.h"
#include "Game/GenGameMode.h"
#include "Game/GenPlayerController.h"
#include "Game/GenShellData.h"
#include "Game/GenSkirmishSetup.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Net/GenNetSubsystem.h"
#include "Misc/ConfigCacheIni.h"
#include "Serialization/MemoryWriter.h"
#include "W3D/GenAssets.h"
#include "W3D/GenTextureLoader.h"
#include "Generals1.h"
#include "CanvasItem.h"
#include "Engine/Canvas.h"
#include "Engine/Font.h"
#include "Engine/Texture2D.h"
#include "Fonts/FontMeasure.h"
#include "Framework/Application/SlateApplication.h"
#include "HAL/PlatformApplicationMisc.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Rendering/SlateRenderer.h"
#include "Styling/CoreStyle.h"

namespace
{
	const FLinearColor ShellGold(1.f, 0.8f, 0.22f);
	const FLinearColor ShellTextColor(0.86f, 0.9f, 1.f);
	const FLinearColor ShellDim(0.45f, 0.5f, 0.6f);
	const FLinearColor ShellFill(0.03f, 0.06f, 0.13f, 0.92f);
	const FLinearColor ShellHot(0.1f, 0.24f, 0.5f, 0.95f);
	const FLinearColor ShellEdge(0.22f, 0.42f, 0.78f);
	const FLinearColor ShellRed(1.f, 0.3f, 0.25f);

	constexpr float DesignWidth = 1600.f;
	constexpr float DesignHeight = 900.f;

	/**
	 * A crisp font at any size: the engine's Roboto, rendered as it is asked for (its other canvas fonts
	 * are bitmaps made for one size). A canvas text item wants the font as an asset, which this one is.
	 */
	FSlateFontInfo ShellFont(float Pixels, bool bBold)
	{
		static TWeakObjectPtr<UFont> Roboto;
		if (!Roboto.IsValid())
		{
			Roboto = LoadObject<UFont>(nullptr, TEXT("/Engine/EngineFonts/Roboto.Roboto"));
		}
		return FSlateFontInfo(Roboto.Get(), float(FMath::Max(6, FMath::RoundToInt32(Pixels * 0.75f))), FName(bBold ? TEXT("Bold") : TEXT("Regular")));
	}

	FVector2D MeasureShellText(const FString& Text, const FSlateFontInfo& Font)
	{
		if (!FSlateApplication::IsInitialized())
		{
			return FVector2D(Text.Len() * Font.Size * 0.75, Font.Size * 1.6);
		}
		return FVector2D(FSlateApplication::Get().GetRenderer()->GetFontMeasureService()->Measure(Text, Font));
	}

	/** "Units \n Created" -> "Created" (the score screen's column titles are two lines in the string file). */
	FString SecondLine(const FString& Text)
	{
		FString Left, Right;
		if (Text.Split(TEXT("\\n"), &Left, &Right) || Text.Split(TEXT("\n"), &Left, &Right))
		{
			return Right.TrimStartAndEnd();
		}
		return Text;
	}
}

// ---- the elements ---------------------------------------------------------------------------------

void AGenHUD::ShellTestClick(const FString& Id)
{
	Shell.TestClick = Id;
	Shell.TestClickFrames = 0;
}

FBox2D AGenHUD::ShellRect(float X, float Y, float W, float H) const
{
	return FBox2D(Shell.Origin + FVector2D(X, Y) * Shell.Scale, Shell.Origin + FVector2D(X + W, Y + H) * Shell.Scale);
}

bool AGenHUD::ShellHover(const FBox2D& R) const
{
	return !Shell.bBlocked && Shell.OpenId.IsEmpty() && R.IsInside(Shell.Mouse);
}

bool AGenHUD::ShellTestHit(const FString& Id)
{
	if (Shell.bBlocked || Shell.TestClick.IsEmpty() || !Shell.TestClick.Equals(Id, ESearchCase::IgnoreCase))
	{
		return false;
	}
	Shell.TestClick.Reset();
	return true;
}

void AGenHUD::ShellClickSound()
{
	if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
	{
		Audio->PlayUI(FGenAudioData::Get().GetMisc(TEXT("GUIClickSound")));
	}
}

bool AGenHUD::ShellImage(const FString& Image, const FBox2D& R, const FLinearColor& Tint)
{
	const FGenMappedImage* Img = Image.IsEmpty() ? nullptr : FGenCommandData::Get().FindImage(Image);
	UTexture2D* Tex = Img ? UGenAssetSubsystem::Get()->GetTexture(Img->Texture) : nullptr;
	if (!Tex)
	{
		return false;
	}
	DrawTexture(Tex, R.Min.X, R.Min.Y, R.GetSize().X, R.GetSize().Y, Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y, Tint);
	return true;
}

void AGenHUD::ShellBackdrop(const TCHAR* Image, float Darken)
{
	// The picture fills the screen (cut at the sides rather than squeezed); without one, plain dark.
	const FVector2D View(Canvas->SizeX, Canvas->SizeY);
	const FGenMappedImage* Img = FGenCommandData::Get().FindImage(Image);
	UTexture2D* Tex = Img ? UGenAssetSubsystem::Get()->GetTexture(Img->Texture) : nullptr;
	static bool bReported = false;
	if (!bReported && (!Img || !Tex))
	{
		bReported = true;
		UE_LOG(LogGenerals, Warning, TEXT("Shell: no backdrop '%s' (image %s, texture '%s' %s)"), Image, Img ? TEXT("found") : TEXT("unknown"),
			Img ? *Img->Texture : TEXT(""), Tex ? TEXT("loaded") : TEXT("not loaded"));
	}
	if (Tex && Img->Size.X > 0.f && Img->Size.Y > 0.f)
	{
		const float Fit = FMath::Max(View.X / Img->Size.X, View.Y / Img->Size.Y);
		const FVector2D Size(Img->Size.X * Fit, Img->Size.Y * Fit);
		DrawRect(FLinearColor::Black, 0.f, 0.f, View.X, View.Y);
		DrawTexture(Tex, (View.X - Size.X) * 0.5f, (View.Y - Size.Y) * 0.5f, Size.X, Size.Y, Img->UV0.X, Img->UV0.Y, Img->UV1.X - Img->UV0.X, Img->UV1.Y - Img->UV0.Y,
			FLinearColor(1.f - Darken, 1.f - Darken, 1.f - Darken));
	}
	else
	{
		DrawRect(FLinearColor(0.01f, 0.02f, 0.04f), 0.f, 0.f, View.X, View.Y);
	}
}

void AGenHUD::ShellPanel(const FBox2D& R, float Alpha)
{
	const FVector2D Size = R.GetSize();
	DrawRect(FLinearColor(0.015f, 0.03f, 0.07f, Alpha), R.Min.X, R.Min.Y, Size.X, Size.Y);
	const float T = FMath::Max(1.f, 1.5f * Shell.Scale);
	DrawRect(ShellEdge, R.Min.X, R.Min.Y, Size.X, T);
	DrawRect(ShellEdge, R.Min.X, R.Max.Y - T, Size.X, T);
	DrawRect(ShellEdge, R.Min.X, R.Min.Y, T, Size.Y);
	DrawRect(ShellEdge, R.Max.X - T, R.Min.Y, T, Size.Y);
}

void AGenHUD::ShellText(const FString& Text, float X, float Y, const FLinearColor& Color, float Size, int32 Align, bool bBold)
{
	if (Text.IsEmpty())
	{
		return;
	}
	const FSlateFontInfo Font = ShellFont(Size * Shell.Scale, bBold);
	if (!Font.FontObject)
	{
		// (No such font: the plain canvas one, whatever it looks like at this size.)
		DrawText(Text, Color, X, Y, nullptr, Size * Shell.Scale / 14.f);
		return;
	}
	if (Align != 0)
	{
		const float Width = float(MeasureShellText(Text, Font).X);
		X -= Align == 1 ? Width * 0.5f : Width;
	}
	FCanvasTextItem Item(FVector2D(FMath::RoundToFloat(X), FMath::RoundToFloat(Y)), FText::FromString(Text), Font, Color);
	Item.EnableShadow(FLinearColor(0.f, 0.f, 0.f, Color.A), FVector2D(1.5, 1.5));
	Canvas->DrawItem(Item);
}

bool AGenHUD::ShellButton(const FString& Id, const FBox2D& R, const FString& Label, bool bEnabled)
{
	const bool bHover = bEnabled && ShellHover(R);
	const FVector2D Size = R.GetSize();
	DrawRect(bHover ? ShellHot : ShellFill, R.Min.X, R.Min.Y, Size.X, Size.Y);
	const float T = FMath::Max(1.f, 1.5f * Shell.Scale);
	const FLinearColor Edge = bEnabled ? (bHover ? ShellGold : ShellEdge) : ShellDim;
	DrawRect(Edge, R.Min.X, R.Min.Y, Size.X, T);
	DrawRect(Edge, R.Min.X, R.Max.Y - T, Size.X, T);
	DrawRect(Edge, R.Min.X, R.Min.Y, T, Size.Y);
	DrawRect(Edge, R.Max.X - T, R.Min.Y, T, Size.Y);
	const float TextSize = FMath::Clamp(float(Size.Y) / Shell.Scale * 0.42f, 13.f, 26.f);
	const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * Shell.Scale, true)).Y);
	ShellText(Label, R.Min.X + Size.X * 0.5f, R.Min.Y + (Size.Y - TextHeight) * 0.5f, bEnabled ? (bHover ? FLinearColor::White : ShellTextColor) : ShellDim, TextSize, 1, true);
	const bool bClicked = bEnabled && ((bHover && Shell.bClick) || ShellTestHit(Id));
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellCheckbox(const FString& Id, const FBox2D& R, const FString& Label, bool bValue)
{
	const bool bHover = ShellHover(R);
	const float Box = R.GetSize().Y;
	DrawRect(bHover ? ShellHot : ShellFill, R.Min.X, R.Min.Y, Box, Box);
	const float T = FMath::Max(1.f, 1.5f * Shell.Scale);
	DrawRect(ShellEdge, R.Min.X, R.Min.Y, Box, T);
	DrawRect(ShellEdge, R.Min.X, R.Max.Y - T, Box, T);
	DrawRect(ShellEdge, R.Min.X, R.Min.Y, T, Box);
	DrawRect(ShellEdge, R.Min.X + Box - T, R.Min.Y, T, Box);
	if (bValue)
	{
		DrawRect(ShellGold, R.Min.X + Box * 0.25f, R.Min.Y + Box * 0.25f, Box * 0.5f, Box * 0.5f);
	}
	const float TextSize = 18.f;
	const float TextHeight = float(MeasureShellText(Label, ShellFont(TextSize * Shell.Scale, false)).Y);
	ShellText(Label, R.Min.X + Box * 1.4f, R.Min.Y + (Box - TextHeight) * 0.5f, ShellTextColor, TextSize);
	const bool bClicked = (bHover && Shell.bClick) || ShellTestHit(Id);
	if (bClicked)
	{
		Shell.bClick = false;
		ShellClickSound();
	}
	return bClicked;
}

bool AGenHUD::ShellDropdown(const FString& Id, const FBox2D& R, const TArray<FString>& Items, const TArray<FLinearColor>& Colors, int32 Current, int32& OutPicked)
{
	// What was chosen in the open list (BeginShell saw the click) is collected here.
	if (!Shell.PickedId.IsEmpty() && Shell.PickedId == Id)
	{
		const int32 Picked = Shell.PickedIndex;
		Shell.PickedId.Reset();
		if (Items.IsValidIndex(Picked) && Picked != Current)
		{
			OutPicked = Picked;
			return true;
		}
	}
	// Test automation: "Id=Item label" chooses without opening the list.
	if (!Shell.bBlocked && Shell.TestClick.StartsWith(Id + TEXT("="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(Id.Len() + 1);
		Shell.TestClick.Reset();
		Shell.OpenId.Reset();
		const int32 Picked = Items.IndexOfByPredicate([&Wanted](const FString& Item) { return Item.Equals(Wanted, ESearchCase::IgnoreCase); });
		if (Picked == INDEX_NONE)
		{
			UE_LOG(LogGenerals, Warning, TEXT("Test menu: '%s' has no item '%s'"), *Id, *Wanted);
		}
		else if (Picked != Current)
		{
			OutPicked = Picked;
			return true;
		}
	}

	const bool bHover = ShellHover(R);
	const bool bOpen = Shell.OpenId == Id;
	const FVector2D Size = R.GetSize();
	DrawRect((bHover || bOpen) ? ShellHot : ShellFill, R.Min.X, R.Min.Y, Size.X, Size.Y);
	const float T = FMath::Max(1.f, 1.f * Shell.Scale);
	const FLinearColor Edge = (bHover || bOpen) ? ShellGold : ShellEdge;
	DrawRect(Edge, R.Min.X, R.Min.Y, Size.X, T);
	DrawRect(Edge, R.Min.X, R.Max.Y - T, Size.X, T);
	DrawRect(Edge, R.Min.X, R.Min.Y, T, Size.Y);
	DrawRect(Edge, R.Max.X - T, R.Min.Y, T, Size.Y);
	const float TextSize = 18.f;
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(TextSize * Shell.Scale, false)).Y);
	if (Items.IsValidIndex(Current))
	{
		ShellText(Items[Current], R.Min.X + 10.f * Shell.Scale, R.Min.Y + (Size.Y - TextHeight) * 0.5f, Colors.IsValidIndex(Current) ? Colors[Current] : ShellTextColor, TextSize);
	}
	// The mark that says "there is a list behind this": a small triangle of lines.
	const float AX = R.Max.X - 18.f * Shell.Scale, AY = R.Min.Y + Size.Y * 0.5f, AS = 5.f * Shell.Scale;
	DrawLine(AX - AS, AY - AS * 0.5f, AX, AY + AS * 0.6f, Edge, 1.5f * Shell.Scale);
	DrawLine(AX, AY + AS * 0.6f, AX + AS, AY - AS * 0.5f, Edge, 1.5f * Shell.Scale);

	if ((bHover && Shell.bClick) || ShellTestHit(Id))
	{
		Shell.bClick = false;
		Shell.OpenId = Id;
		Shell.OpenRect = R;
		Shell.OpenItems = Items;
		Shell.OpenColors = Colors;
		Shell.OpenCurrent = Current;
		ShellClickSound();
	}
	return false;
}

namespace
{
	/** Where the open list goes: under its button, or over it when there is no room below. */
	FBox2D OpenListRect(const FBox2D& Button, int32 NumItems, float ViewHeight)
	{
		const float ItemHeight = float(Button.GetSize().Y) * 0.86f;
		const float Height = ItemHeight * NumItems;
		const bool bBelow = Button.Max.Y + Height <= ViewHeight - 4.f || Button.Min.Y - Height < 4.f;
		const float Top = bBelow ? float(Button.Max.Y) : float(Button.Min.Y) - Height;
		return FBox2D(FVector2D(Button.Min.X, Top), FVector2D(Button.Max.X, Top + Height));
	}
}

void AGenHUD::BeginShell(AGenPlayerController* PC)
{
	Shell.Scale = FMath::Max(0.25f, FMath::Min(Canvas->SizeX / DesignWidth, Canvas->SizeY / DesignHeight));
	Shell.Origin = FVector2D((Canvas->SizeX - DesignWidth * Shell.Scale) * 0.5f, (Canvas->SizeY - DesignHeight * Shell.Scale) * 0.5f);
	Shell.bBlocked = false;
	Shell.Mouse = FVector2D(-10000.0, -10000.0);
	Shell.bClick = Shell.bRightClick = false;
	Shell.Wheel = 0.f;
	float MX = 0.f, MY = 0.f;
	if (PC && PC->IsGenInputEnabled())
	{
		if (PC->GetMousePosition(MX, MY))
		{
			Shell.Mouse = FVector2D(MX, MY);
		}
		Shell.bClick = PC->WasInputKeyJustPressed(EKeys::LeftMouseButton);
		Shell.bRightClick = PC->WasInputKeyJustPressed(EKeys::RightMouseButton);
		Shell.Wheel = PC->GetInputAnalogKeyState(EKeys::MouseWheelAxis);
	}
	// A click while a list is open is the list's: on an item it chooses that, anywhere else it closes the list.
	if (!Shell.OpenId.IsEmpty() && (Shell.bClick || Shell.bRightClick))
	{
		const FBox2D List = OpenListRect(Shell.OpenRect, Shell.OpenItems.Num(), Canvas->SizeY);
		if (Shell.bClick && List.IsInside(Shell.Mouse) && Shell.OpenItems.Num() > 0)
		{
			Shell.PickedId = Shell.OpenId;
			Shell.PickedIndex = FMath::Clamp(int32((Shell.Mouse.Y - List.Min.Y) / (List.GetSize().Y / Shell.OpenItems.Num())), 0, Shell.OpenItems.Num() - 1);
			ShellClickSound();
		}
		Shell.OpenId.Reset();
		Shell.bClick = Shell.bRightClick = false;
	}
}

void AGenHUD::EndShell()
{
	if (!Shell.OpenId.IsEmpty() && Shell.OpenItems.Num() > 0)
	{
		const FBox2D List = OpenListRect(Shell.OpenRect, Shell.OpenItems.Num(), Canvas->SizeY);
		const float ItemHeight = float(List.GetSize().Y) / Shell.OpenItems.Num();
		const float Width = float(List.GetSize().X);
		DrawRect(FLinearColor(0.01f, 0.02f, 0.05f, 0.98f), List.Min.X, List.Min.Y, Width, List.GetSize().Y);
		const float TextSize = 17.f;
		const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(TextSize * Shell.Scale, false)).Y);
		for (int32 i = 0; i < Shell.OpenItems.Num(); ++i)
		{
			const float Y = float(List.Min.Y) + ItemHeight * i;
			const bool bHover = Shell.Mouse.X >= List.Min.X && Shell.Mouse.X < List.Max.X && Shell.Mouse.Y >= Y && Shell.Mouse.Y < Y + ItemHeight;
			if (bHover || i == Shell.OpenCurrent)
			{
				DrawRect(bHover ? ShellHot : FLinearColor(0.06f, 0.1f, 0.2f, 1.f), List.Min.X, Y, Width, ItemHeight);
			}
			ShellText(Shell.OpenItems[i], List.Min.X + 10.f * Shell.Scale, Y + (ItemHeight - TextHeight) * 0.5f,
				Shell.OpenColors.IsValidIndex(i) ? Shell.OpenColors[i] : ShellTextColor, TextSize);
		}
		const float T = FMath::Max(1.f, 1.f * Shell.Scale);
		DrawRect(ShellGold, List.Min.X, List.Min.Y, Width, T);
		DrawRect(ShellGold, List.Min.X, List.Max.Y - T, Width, T);
		DrawRect(ShellGold, List.Min.X, List.Min.Y, T, List.GetSize().Y);
		DrawRect(ShellGold, List.Max.X - T, List.Min.Y, T, List.GetSize().Y);
	}
	// A scripted click that nothing on the screen answered to.
	if (!Shell.TestClick.IsEmpty() && ++Shell.TestClickFrames > 20)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Test menu: nothing on the screen has the id '%s'"), *Shell.TestClick);
		Shell.TestClick.Reset();
	}
}

UTexture2D* AGenHUD::GetMapPreview(const FString& MapName)
{
	// MapUtil.cpp getMapPreviewImage: the map's own picture lies next to it, as a TGA of the same name.
	const FString Key = MapName.ToLower();
	if (const TObjectPtr<UTexture2D>* Found = MapPreviews.Find(Key))
	{
		return *Found;
	}
	UTexture2D* Texture = nullptr;
	TArray<uint8> Bytes;
	FGenImage Image;
	if (FGenFileSystem::Get().ReadFile(FString::Printf(TEXT("Maps\\%s\\%s.tga"), *MapName, *MapName), Bytes) && GenTextureLoader::DecodeTGA(Bytes, Image))
	{
		Texture = GenTextureLoader::CreateFromImage(Image, TEXT("MapPreview_") + Key, true, false);
	}
	MapPreviews.Add(Key, Texture);
	return Texture;
}

// ---- the skirmish menu ----------------------------------------------------------------------------

void AGenHUD::DrawMapPreview(const FGenMapInfo& Map, const FBox2D& Area, bool bInteractive)
{
	FGenSetupData* Setup = ShellSetup ? ShellSetup : static_cast<FGenSetupData*>(UGenSkirmishSetup::Get(this));
	const FGenShellData& Data = FGenShellData::Get();
	// The picture, in the map's own proportions.
	const FVector2f Extent = Map.ExtentMax - Map.ExtentMin;
	const float Aspect = (Extent.X > 1.f && Extent.Y > 1.f) ? Extent.X / Extent.Y : 1.f;
	FVector2D Size = Area.GetSize();
	if (Size.X / Size.Y > Aspect) { Size.X = Size.Y * Aspect; }
	else { Size.Y = Size.X / Aspect; }
	const FVector2D Min = Area.GetCenter() - Size * 0.5;
	DrawRect(FLinearColor::Black, Area.Min.X, Area.Min.Y, Area.GetSize().X, Area.GetSize().Y);
	if (UTexture2D* Preview = GetMapPreview(Map.Name))
	{
		DrawTexture(Preview, Min.X, Min.Y, Size.X, Size.Y, 0.f, 0.f, 1.f, 1.f);
	}
	else
	{
		ShellText(Map.DisplayName, Area.GetCenter().X, Area.GetCenter().Y - 10.f * Shell.Scale, ShellDim, 18.f, 1);
	}
	auto ToScreen = [&](const FVector2f& P)
	{
		const float U = Extent.X > 1.f ? (P.X - Map.ExtentMin.X) / Extent.X : 0.5f;
		const float V = Extent.Y > 1.f ? 1.f - (P.Y - Map.ExtentMin.Y) / Extent.Y : 0.5f;
		return Min + FVector2D(U * Size.X, V * Size.Y);
	};
	// Supply docks and tech buildings (the original marks them with small icons).
	const float Dot = 7.f * Shell.Scale;
	for (const FVector2f& P : Map.SupplyPositions)
	{
		const FVector2D S = ToScreen(P);
		DrawRect(FLinearColor::Black, S.X - Dot * 0.5f - 1.f, S.Y - Dot * 0.5f - 1.f, Dot + 2.f, Dot + 2.f);
		DrawRect(FLinearColor(0.35f, 1.f, 0.35f), S.X - Dot * 0.5f, S.Y - Dot * 0.5f, Dot, Dot);
	}
	for (const FVector2f& P : Map.TechPositions)
	{
		const FVector2D S = ToScreen(P);
		DrawRect(FLinearColor::Black, S.X - Dot * 0.5f - 1.f, S.Y - Dot * 0.5f - 1.f, Dot + 2.f, Dot + 2.f);
		DrawRect(FLinearColor(1.f, 0.6f, 0.15f), S.X - Dot * 0.5f, S.Y - Dot * 0.5f, Dot, Dot);
	}
	// Start positions: whose it is shows by number and colour; a click hands it on (see below).
	const float Mark = 30.f * Shell.Scale;
	for (int32 i = 0; i < Map.NumPlayers && i < Map.Starts.Num(); ++i)
	{
		const FVector2D S = ToScreen(Map.Starts[i]);
		const FBox2D Box(S - FVector2D(Mark, Mark) * 0.5, S + FVector2D(Mark, Mark) * 0.5);
		const int32 Holder = (bInteractive && Setup) ? Setup->FindSlotAtStart(i) : INDEX_NONE;
		const bool bHover = bInteractive && ShellHover(Box);
		FLinearColor Tint = bHover ? FLinearColor::White : FLinearColor(0.8f, 0.8f, 0.8f);
		if (Holder != INDEX_NONE && Data.GetColors().IsValidIndex(Setup->Slots[Holder].Color))
		{
			Tint = Data.GetColors()[Setup->Slots[Holder].Color].Color;
		}
		if (!ShellImage(bHover ? TEXT("PlayerStartHilite") : TEXT("PlayerStart"), Box, Tint))
		{
			DrawRect(FLinearColor::Black, Box.Min.X, Box.Min.Y, Mark, Mark);
			DrawRect(Tint, Box.Min.X + 2.f, Box.Min.Y + 2.f, Mark - 4.f, Mark - 4.f);
		}
		if (Holder != INDEX_NONE)
		{
			ShellText(FString::FromInt(Holder + 1), S.X, S.Y - 11.f * Shell.Scale, FLinearColor::White, 17.f, 1, true);
		}
		if (!bInteractive || !Setup)
		{
			continue;
		}
		// SkirmishGameOptionsMenu: a left click on a taken spot passes it to the next player in the list
		// who has none (and frees it if there is nobody); on a free one, the first such player gets it,
		// or the human player moves there. A right click frees it.
		const bool bLeft = (bHover && Shell.bClick) || ShellTestHit(FString::Printf(TEXT("Start%d"), i + 1));
		if (bLeft)
		{
			Shell.bClick = false;
			ShellClickSound();
			auto NextWithoutSpot = [Setup](int32 From)
			{
				for (int32 j = From; j < UGenSkirmishSetup::MaxSlots; ++j)
				{
					if (Setup->Slots[j].IsOccupied() && Setup->Slots[j].StartPos < 0) { return j; }
				}
				return int32(INDEX_NONE);
			};
			if (Holder != INDEX_NONE)
			{
				const int32 Next = NextWithoutSpot(Holder + 1);
				Setup->SetSlotStartPos(Holder, -1);
				if (Next != INDEX_NONE) { Setup->SetSlotStartPos(Next, i); }
			}
			else
			{
				const int32 Next = NextWithoutSpot(0);
				Setup->SetSlotStartPos(Next != INDEX_NONE ? Next : 0, i);
			}
		}
		else if (bHover && Shell.bRightClick && Holder != INDEX_NONE)
		{
			Shell.bRightClick = false;
			Setup->SetSlotStartPos(Holder, -1);
		}
	}
}

void AGenHUD::ShellTypeInto(FString& Text, int32 MaxLen, bool& bOutDone)
{
	bOutDone = false;
	AGenPlayerController* KeyPC = Cast<AGenPlayerController>(GetOwningPlayerController());
	if (!KeyPC || !KeyPC->IsGenInputEnabled())
	{
		return;
	}
	const bool bShift = KeyPC->IsInputKeyDown(EKeys::LeftShift) || KeyPC->IsInputKeyDown(EKeys::RightShift);
	const bool bCtrl = KeyPC->IsInputKeyDown(EKeys::LeftControl) || KeyPC->IsInputKeyDown(EKeys::RightControl);
	static const FKey Letters[] = { EKeys::A, EKeys::B, EKeys::C, EKeys::D, EKeys::E, EKeys::F, EKeys::G, EKeys::H, EKeys::I, EKeys::J, EKeys::K, EKeys::L, EKeys::M,
		EKeys::N, EKeys::O, EKeys::P, EKeys::Q, EKeys::R, EKeys::S, EKeys::T, EKeys::U, EKeys::V, EKeys::W, EKeys::X, EKeys::Y, EKeys::Z };
	static const FKey Digits[] = { EKeys::Zero, EKeys::One, EKeys::Two, EKeys::Three, EKeys::Four, EKeys::Five, EKeys::Six, EKeys::Seven, EKeys::Eight, EKeys::Nine };
	static const FKey PadDigits[] = { EKeys::NumPadZero, EKeys::NumPadOne, EKeys::NumPadTwo, EKeys::NumPadThree, EKeys::NumPadFour, EKeys::NumPadFive,
		EKeys::NumPadSix, EKeys::NumPadSeven, EKeys::NumPadEight, EKeys::NumPadNine };
	FString Typed;
	if (bCtrl)
	{
		// Ctrl+V: whatever is on the clipboard, with the line breaks and control characters taken out.
		if (KeyPC->WasInputKeyJustPressed(EKeys::V))
		{
			FString Clip;
			FPlatformApplicationMisc::ClipboardPaste(Clip);
			for (int32 i = 0; i < Clip.Len(); ++i)
			{
				const TCHAR C = Clip[i];
				if (C >= 32 && C != 127) { Typed.AppendChar(C); }
			}
		}
	}
	else
	{
		for (int32 k = 0; k < UE_ARRAY_COUNT(Letters); ++k)
		{
			if (KeyPC->WasInputKeyJustPressed(Letters[k])) { Typed.AppendChar(TCHAR((bShift ? TEXT('A') : TEXT('a')) + k)); }
		}
		for (int32 k = 0; k < UE_ARRAY_COUNT(Digits); ++k)
		{
			if (KeyPC->WasInputKeyJustPressed(Digits[k]) || KeyPC->WasInputKeyJustPressed(PadDigits[k])) { Typed.AppendChar(TCHAR(TEXT('0') + k)); }
		}
		if (KeyPC->WasInputKeyJustPressed(EKeys::SpaceBar)) { Typed.AppendChar(TEXT(' ')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Hyphen) || KeyPC->WasInputKeyJustPressed(EKeys::Subtract)) { Typed.AppendChar(bShift ? TEXT('_') : TEXT('-')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Period) || KeyPC->WasInputKeyJustPressed(EKeys::Decimal)) { Typed.AppendChar(TEXT('.')); }
		if (KeyPC->WasInputKeyJustPressed(EKeys::Semicolon) && bShift) { Typed.AppendChar(TEXT(':')); }
	}
	if (KeyPC->WasInputKeyJustPressed(EKeys::BackSpace) && Text.Len() > 0) { Text.LeftChopInline(1); }
	Text = (Text + Typed).Left(MaxLen);
	bOutDone = KeyPC->WasInputKeyJustPressed(EKeys::Enter) || KeyPC->WasInputKeyJustPressed(EKeys::Escape);
}

void AGenHUD::ShellStatic(const FBox2D& R, const FString& Text, const FLinearColor& Color)
{
	DrawRect(FLinearColor(0.02f, 0.04f, 0.09f, 0.8f), R.Min.X, R.Min.Y, R.GetSize().X, R.GetSize().Y);
	const float TextHeight = float(MeasureShellText(TEXT("Ag"), ShellFont(18.f * Shell.Scale, false)).Y);
	ShellText(Text, R.Min.X + 10.f * Shell.Scale, R.Min.Y + (R.GetSize().Y - TextHeight) * 0.5f, Color, 18.f);
}

void AGenHUD::DrawSkirmishMenu(AGenGameMode* GM)
{
	UGenSkirmishSetup* LocalSetup = UGenSkirmishSetup::Get(this);
	if (!LocalSetup)
	{
		return;
	}
	LocalSetup->EnsureLoaded();
	// A network game's lobby is this screen as well: it shows the host's setup, and what this player changes in
	// it (its own place; the host also the rest that isn't another player's) goes to the host, which sends
	// everybody the result.
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	FGenNetClient* Client = (GM->GetPhase() == EGenGamePhase::NetLobby && Net) ? Net->GetClient() : nullptr;
	if (GM->GetPhase() == EGenGamePhase::NetLobby && (!Client || !Client->HasLobby() || Client->GetLocalSlot() == INDEX_NONE))
	{
		DrawNetWaiting(GM, Client);
		return;
	}
	FGenSetupData NetSetup;
	if (Client)
	{
		NetSetup = Client->GetLobby();
	}
	FGenSetupData* Setup = Client ? &NetSetup : static_cast<FGenSetupData*>(LocalSetup);
	ShellSetup = Setup;
	const int32 LocalSlot = Client ? Client->GetLocalSlot() : 0;
	const bool bHost = !Client || Client->IsHost();
	auto MayEdit = [Client, LocalSlot, bHost](int32 Slot) { return !Client || Slot == LocalSlot || (bHost && !Client->IsSlotHuman(Slot)); };
	const FGenShellData& Data = FGenShellData::Get();
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.6f);
	Shell.bBlocked = Shell.bMapSelect; // under the map selection nothing reacts

	const float S = Shell.Scale;
	ShellText(Client ? FString(TEXT("NETWORK GAME")) : FGenShellData::Text(TEXT("GUI:Skirmish"), TEXT("SKIRMISH")), ShellRect(60, 0, 0, 0).Min.X,
		ShellRect(0, 34, 0, 0).Min.Y, ShellGold, 44.f, 0, true);

	// --- who plays ---
	ShellPanel(ShellRect(40, 110, 1010, 650));
	const float HeadY = ShellRect(0, 128, 0, 0).Min.Y;
	ShellText(FGenShellData::Text(TEXT("GUI:Players"), TEXT("Players")), ShellRect(60, 0, 0, 0).Min.X, HeadY, ShellGold, 19.f, 0, true);
	ShellText(FGenShellData::Text(TEXT("GUI:Color"), TEXT("Color")), ShellRect(400, 0, 0, 0).Min.X, HeadY, ShellGold, 19.f, 0, true);
	ShellText(FGenShellData::Text(TEXT("GUI:Faction"), TEXT("Army")), ShellRect(570, 0, 0, 0).Min.X, HeadY, ShellGold, 19.f, 0, true);
	ShellText(FGenShellData::Text(TEXT("GUI:Team"), TEXT("Team")), ShellRect(900, 0, 0, 0).Min.X, HeadY, ShellGold, 19.f, 0, true);

	const TArray<FString> StateItems = {
		FGenShellData::Text(TEXT("GUI:Open"), TEXT("Open")), FGenShellData::Text(TEXT("GUI:Closed"), TEXT("Closed")),
		FGenShellData::Text(TEXT("GUI:EasyAI"), TEXT("Easy Army")), FGenShellData::Text(TEXT("GUI:MediumAI"), TEXT("Medium Army")),
		FGenShellData::Text(TEXT("GUI:HardAI"), TEXT("Hard Army")) };
	TArray<FString> SideItems = { FGenShellData::Text(TEXT("GUI:Random"), TEXT("Random")) };
	for (const FGenSideInfo& Side : Data.GetSides())
	{
		SideItems.Add(Side.Label);
	}
	TArray<FString> TeamItems = { FGenShellData::Text(TEXT("Team:0"), TEXT("None")) };
	for (int32 t = 1; t <= UGenSkirmishSetup::MaxSlots / 2; ++t)
	{
		TeamItems.Add(FGenShellData::Text(*FString::Printf(TEXT("Team:%d"), t), *FString::FromInt(t)));
	}
	const TArray<FLinearColor> NoColors;

	for (int32 i = 0; i < UGenSkirmishSetup::MaxSlots; ++i)
	{
		const float Y = 170.f + i * 58.f;
		const FGenSetupSlot& Slot = Setup->Slots[i];
		int32 Picked = INDEX_NONE;
		if (i == LocalSlot)
		{
			// The player's own place: a click on the name lets it be typed (Enter or a click elsewhere ends that).
			const FBox2D NameBox = ShellRect(60, Y, 320, 44);
			const bool bNameHover = ShellHover(NameBox);
			const bool bWasEditing = Shell.bEditingName;
			if (Shell.bClick)
			{
				Shell.bEditingName = bNameHover;
				Shell.bClick = Shell.bClick && !bNameHover;
			}
			if (Shell.bEditingName && !Shell.bMapSelect)
			{
				bool bDone = false;
				ShellTypeInto(LocalSetup->PlayerName, 16, bDone);
				Shell.bEditingName = !bDone;
			}
			if (bWasEditing && !Shell.bEditingName)
			{
				if (LocalSetup->PlayerName.TrimStartAndEnd().IsEmpty()) { LocalSetup->PlayerName = FGenShellData::Text(TEXT("GUI:Player"), TEXT("Player")); }
				if (Client) { Client->SendName(LocalSetup->PlayerName); }
			}
			DrawRect((bNameHover || Shell.bEditingName) ? ShellHot : ShellFill, NameBox.Min.X, NameBox.Min.Y, NameBox.GetSize().X, NameBox.GetSize().Y);
			if (Shell.bEditingName)
			{
				const float T = FMath::Max(1.f, S);
				DrawRect(ShellGold, NameBox.Min.X, NameBox.Max.Y - T, NameBox.GetSize().X, T);
			}
			// (A blinking bar where the next letter goes.)
			const bool bCaret = Shell.bEditingName && FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 1.f) < 0.5f;
			ShellText(LocalSetup->PlayerName + (bCaret ? TEXT("|") : TEXT("")), NameBox.Min.X + 10.f * S, NameBox.Min.Y + 9.f * S, FLinearColor::White, 19.f, 0, true);
		}
		else if (Client && Client->IsSlotHuman(i))
		{
			ShellStatic(ShellRect(60, Y, 320, 44), Slot.Name, FLinearColor::White);
		}
		else if (!MayEdit(i))
		{
			ShellStatic(ShellRect(60, Y, 320, 44), StateItems.IsValidIndex(int32(Slot.State)) ? StateItems[int32(Slot.State)] : FString(), ShellTextColor);
		}
		else if (ShellDropdown(FString::Printf(TEXT("Slot%d.State"), i), ShellRect(60, Y, 320, 44), StateItems, NoColors, int32(Slot.State), Picked))
		{
			Setup->SetSlotState(i, EGenSlotState(Picked));
		}
		if (!Slot.IsOccupied())
		{
			continue;
		}
		if (!MayEdit(i))
		{
			// (Another player's choices: shown, not changed here.)
			const bool bHasColor = Data.GetColors().IsValidIndex(Slot.Color);
			ShellStatic(ShellRect(400, Y, 150, 44), bHasColor ? Data.GetColors()[Slot.Color].Label : FGenShellData::Text(TEXT("GUI:???"), TEXT("???")),
				bHasColor ? Data.GetColors()[Slot.Color].Color : ShellTextColor);
			ShellStatic(ShellRect(570, Y, 310, 44), SideItems.IsValidIndex(Slot.Side + 1) ? SideItems[Slot.Side + 1] : FString(), ShellTextColor);
			ShellStatic(ShellRect(900, Y, 130, 44), TeamItems.IsValidIndex(Slot.Team + 1) ? TeamItems[Slot.Team + 1] : FString(), ShellTextColor);
			continue;
		}
		// The colours nobody else wears, after "???" for "any".
		TArray<FString> ColorItems = { FGenShellData::Text(TEXT("GUI:???"), TEXT("???")) };
		TArray<FLinearColor> ColorColors = { ShellTextColor };
		TArray<int32> ColorIndex = { -1 };
		for (int32 c = 0; c < Data.GetColors().Num(); ++c)
		{
			if (!Setup->IsColorTaken(c, i))
			{
				ColorItems.Add(Data.GetColors()[c].Label);
				ColorColors.Add(Data.GetColors()[c].Color);
				ColorIndex.Add(c);
			}
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Color"), i), ShellRect(400, Y, 150, 44), ColorItems, ColorColors, FMath::Max(0, ColorIndex.Find(Slot.Color)), Picked))
		{
			Setup->SetSlotColor(i, ColorIndex[Picked]);
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Side"), i), ShellRect(570, Y, 310, 44), SideItems, NoColors, Slot.Side + 1, Picked))
		{
			Setup->SetSlotSide(i, Picked - 1);
		}
		if (ShellDropdown(FString::Printf(TEXT("Slot%d.Team"), i), ShellRect(900, Y, 130, 44), TeamItems, NoColors, Slot.Team + 1, Picked))
		{
			Setup->SetSlotTeam(i, Picked - 1);
		}
	}

	// --- the rules ---
	ShellText(FGenShellData::Text(TEXT("GUI:StartingMoney"), TEXT("Starting Cash")), ShellRect(60, 0, 0, 0).Min.X, ShellRect(0, 660, 0, 0).Min.Y, ShellGold, 19.f, 0, true);
	TArray<FString> CashItems;
	for (int32 Cash : Data.GetMoneyChoices())
	{
		CashItems.Add(FString::Printf(TEXT("$%d"), Cash));
	}
	int32 PickedCash = INDEX_NONE;
	if (!bHost)
	{
		ShellStatic(ShellRect(60, 696, 200, 44), FString::Printf(TEXT("$%d"), Setup->StartingCash), ShellTextColor);
	}
	else if (ShellDropdown(TEXT("Cash"), ShellRect(60, 696, 200, 44), CashItems, NoColors, Data.GetMoneyChoices().Find(Setup->StartingCash), PickedCash))
	{
		Setup->StartingCash = Data.GetMoneyChoices()[PickedCash];
	}
	// (The battle group is the skirmish's: a network game's players start as the original's do.)
	if (!Client && ShellCheckbox(TEXT("TestUnits"), ShellRect(400, 702, 32, 32), TEXT("Start with a battle group (for trying things out)"), Setup->bTestUnits))
	{
		Setup->bTestUnits = !Setup->bTestUnits;
	}

	// --- where ---
	ShellPanel(ShellRect(1070, 110, 490, 650));
	if (const FGenMapInfo* Map = Setup->GetMapInfo())
	{
		DrawMapPreview(*Map, ShellRect(1090, 130, 450, 450), true);
		ShellText(Map->DisplayName, ShellRect(1090, 0, 0, 0).Min.X, ShellRect(0, 592, 0, 0).Min.Y, FLinearColor::White, 24.f, 0, true);
		const bool bTooMany = Setup->GetNumPlayers() > Map->NumPlayers;
		ShellText(FString::Printf(TEXT("%s %d / %d"), *FGenShellData::Text(TEXT("GUI:NumPlayers"), TEXT("Players:")), Setup->GetNumPlayers(), Map->NumPlayers),
			ShellRect(1090, 0, 0, 0).Min.X, ShellRect(0, 630, 0, 0).Min.Y, bTooMany ? ShellRed : ShellTextColor, 18.f);
	}
	if (bHost && ShellButton(TEXT("SelectMap"), ShellRect(1090, 686, 450, 54), FGenShellData::Text(TEXT("GUI:SelectMap"), TEXT("SELECT MAP"))))
	{
		Shell.bMapSelect = true;
		Shell.MapChoice = Setup->Map;
		const int32 Index = Data.GetMaps().IndexOfByPredicate([Setup](const FGenMapInfo& M) { return M.Name.Equals(Setup->Map, ESearchCase::IgnoreCase); });
		Shell.MapScroll = float(FMath::Max(0, Index - 6));
		GM->ClearShellError();
	}

	// --- go ---
	const APlayerController* PC = GetOwningPlayerController();
	const AGenPlayerController* GenPC = Cast<AGenPlayerController>(PC);
	const bool bEnter = !Shell.bMapSelect && !Shell.bEditingName && !Shell.bEditingAddress && GenPC && GenPC->IsGenInputEnabled() && PC->WasInputKeyJustPressed(EKeys::Enter);
	if (Client)
	{
		// The network game: what is going on, then leave or (the host) start.
		float LineY = 750.f;
		for (int32 n = FMath::Max(0, GM->NetNotices.Num() - 2); n < GM->NetNotices.Num(); ++n)
		{
			ShellText(GM->NetNotices[n].Value, ShellRect(340, 0, 0, 0).Min.X, ShellRect(0, LineY, 0, 0).Min.Y, ShellTextColor, 16.f);
			LineY += 22.f;
		}
		ShellText(FString::Printf(TEXT("%s - ping %.0f ms"), *Client->GetStatus(), Client->GetPingMs()), ShellRect(340, 0, 0, 0).Min.X, ShellRect(0, 830, 0, 0).Min.Y,
			ShellDim, 16.f);
		if (ShellButton(TEXT("Leave"), ShellRect(40, 810, 280, 60), TEXT("LEAVE GAME")))
		{
			GM->LeaveNetworkGame();
		}
		if (ShellButton(TEXT("Play"), ShellRect(1280, 810, 280, 60), bHost ? FString(TEXT("START GAME")) : FString(TEXT("WAITING FOR HOST")), bHost) || (bEnter && bHost))
		{
			Client->RequestStart();
		}
	}
	else
	{
		if (!GM->GetShellError().IsEmpty())
		{
			ShellText(GM->GetShellError(), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 775, 0, 0).Min.Y, ShellRed, 20.f, 1, true);
		}
		if (ShellButton(TEXT("Exit"), ShellRect(40, 810, 280, 60), FGenShellData::Text(TEXT("GUI:Exit"), TEXT("EXIT GAME"))))
		{
			LocalSetup->Save();
			UKismetSystemLibrary::QuitGame(this, GetOwningPlayerController(), EQuitPreference::Quit, false);
		}
		// Network games: host one here, or join one by its address - an IP, or a name like yourname.duckdns.org
		// for a home connection whose address changes; ":port" after it if the host's isn't 8088.
		if (Net)
		{
			if (!Shell.bJoinAddressLoaded)
			{
				Shell.bJoinAddressLoaded = true;
				if (GConfig) { GConfig->GetString(TEXT("Generals.Network"), TEXT("LastJoin"), Shell.JoinAddress, GGameUserSettingsIni); }
			}
			if (ShellButton(TEXT("NetHost"), ShellRect(340, 810, 240, 60), TEXT("HOST ONLINE")))
			{
				LocalSetup->Save();
				GM->HostNetworkGame();
			}
			const FBox2D AddrBox = ShellRect(590, 818, 300, 44);
			const bool bAddrHover = ShellHover(AddrBox);
			if (Shell.bClick && !Shell.bMapSelect)
			{
				Shell.bEditingAddress = bAddrHover;
				Shell.bClick = Shell.bClick && !bAddrHover;
			}
			if (Shell.bEditingAddress)
			{
				bool bDone = false;
				ShellTypeInto(Shell.JoinAddress, 64, bDone);
				Shell.bEditingAddress = !bDone;
			}
			DrawRect((bAddrHover || Shell.bEditingAddress) ? ShellHot : ShellFill, AddrBox.Min.X, AddrBox.Min.Y, AddrBox.GetSize().X, AddrBox.GetSize().Y);
			const bool bCaret = Shell.bEditingAddress && FMath::Fmod(GetWorld()->GetRealTimeSeconds(), 1.f) < 0.5f;
			ShellText(Shell.JoinAddress.IsEmpty() && !Shell.bEditingAddress ? FString(TEXT("host address")) : Shell.JoinAddress + (bCaret ? TEXT("|") : TEXT("")),
				AddrBox.Min.X + 10.f * S, AddrBox.Min.Y + 10.f * S, Shell.JoinAddress.IsEmpty() ? ShellDim : FLinearColor::White, 18.f);
			if (ShellButton(TEXT("NetJoin"), ShellRect(900, 810, 170, 60), TEXT("JOIN"), !Shell.JoinAddress.TrimStartAndEnd().IsEmpty()))
			{
				if (GConfig)
				{
					GConfig->SetString(TEXT("Generals.Network"), TEXT("LastJoin"), *Shell.JoinAddress, GGameUserSettingsIni);
					GConfig->Flush(false, GGameUserSettingsIni);
				}
				LocalSetup->Save();
				GM->JoinNetworkGame(Shell.JoinAddress);
			}
			FString SeatAddress, SeatName;
			uint64 Seat = 0;
			if (Net->RecallSeat(SeatAddress, SeatName, Seat) && ShellButton(TEXT("NetRejoin"), ShellRect(1080, 810, 190, 60), TEXT("REJOIN")))
			{
				GM->RejoinNetworkGame();
			}
		}
		if (ShellButton(TEXT("Play"), ShellRect(1280, 810, 280, 60), FGenShellData::Text(TEXT("GUI:Start"), TEXT("PLAY GAME"))) || bEnter)
		{
			GM->RequestStart();
		}
	}

	if (Shell.bMapSelect)
	{
		Shell.bBlocked = false;
		DrawMapSelect();
	}

	// What this player changed goes to the host; the screen shows the host's lobby until it answers.
	if (Client)
	{
		TArray<uint8> Before, After;
		FMemoryWriter WB(Before), WA(After);
		FGenSetupData Old = Client->GetLobby();
		Old.Serialize(WB);
		NetSetup.Serialize(WA);
		if (Before != After)
		{
			GM->NetLobbyEdit(NetSetup);
		}
	}
	ShellSetup = nullptr;
}

void AGenHUD::DrawNetWaiting(AGenGameMode* GM, FGenNetClient* Client)
{
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.6f);
	ShellText(TEXT("NETWORK GAME"), ShellRect(60, 0, 0, 0).Min.X, ShellRect(0, 34, 0, 0).Min.Y, ShellGold, 44.f, 0, true);
	ShellPanel(ShellRect(300, 260, 1000, 300));
	const FString Status = Client ? Client->GetStatus() : FString(TEXT("Not connected"));
	const bool bFailed = Client && (Client->GetState() == FGenNetClient::EState::Refused || Client->GetState() == FGenNetClient::EState::Failed);
	ShellText(Status, ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 320, 0, 0).Min.Y, bFailed ? ShellRed : FLinearColor::White, 22.f, 1, true);
	if (Client && Client->GetState() == FGenNetClient::EState::Match)
	{
		ShellText(TEXT("Loading the match..."), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 370, 0, 0).Min.Y, ShellTextColor, 18.f, 1);
	}
	float LineY = 420.f;
	for (const TPair<double, FString>& Line : GM->NetNotices)
	{
		ShellText(Line.Value, ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, LineY, 0, 0).Min.Y, ShellTextColor, 17.f, 1);
		LineY += 24.f;
	}
	if (ShellButton(TEXT("Leave"), ShellRect(40, 810, 280, 60), bFailed ? FString(TEXT("BACK")) : FString(TEXT("CANCEL"))))
	{
		GM->LeaveNetworkGame();
	}
}

void AGenHUD::DrawNetOverlay(AGenGameMode* GM, UGenGameLogic* Logic)
{
	// Top middle, over the game: what the network game has to say (somebody dropped out, came back; the
	// connection is being made again; this game is catching up with the match; two games came apart).
	UGenNetSubsystem* Net = UGenNetSubsystem::Get(this);
	FGenNetClient* Client = Net ? Net->GetClient() : nullptr;
	TArray<TPair<FString, FLinearColor>> Lines;
	if (Client && Client->GetStatus().StartsWith(TEXT("Lost")))
	{
		Lines.Emplace(Client->GetStatus() + TEXT(" - the game waits"), ShellRed);
	}
	if (Logic->IsCatchingUp() && Client)
	{
		Lines.Emplace(FString::Printf(TEXT("Catching up with the match: %d%%"), int32(100.0 * double(Logic->GetFrame()) / double(FMath::Max(1u, Client->GetReadyFrames())))), ShellGold);
	}
	if (GM)
	{
		for (const TPair<double, FString>& Notice : GM->NetNotices)
		{
			Lines.Emplace(Notice.Value, ShellTextColor);
		}
	}
	const float S = FMath::Max(0.6f, Canvas->SizeY / 1080.f);
	float Y = Canvas->SizeY * 0.12f;
	for (const TPair<FString, FLinearColor>& Line : Lines)
	{
		const float SavedScale = Shell.Scale;
		Shell.Scale = S;
		ShellText(Line.Key, Canvas->SizeX * 0.5f, Y, Line.Value, 20.f, 1, true);
		Shell.Scale = SavedScale;
		Y += 28.f * S;
	}
}

void AGenHUD::DrawMapSelect()
{
	FGenSetupData* Setup = ShellSetup ? ShellSetup : static_cast<FGenSetupData*>(UGenSkirmishSetup::Get(this));
	const TArray<FGenMapInfo>& Maps = FGenShellData::Get().GetMaps();
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.6f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(230, 60, 1140, 780), 0.97f);
	const float S = Shell.Scale;
	ShellText(FGenShellData::Text(TEXT("GUI:SelectMap"), TEXT("SELECT MAP")), ShellRect(256, 0, 0, 0).Min.X, ShellRect(0, 76, 0, 0).Min.Y, ShellGold, 30.f, 0, true);
	ShellText(FGenShellData::Text(TEXT("GUI:Players"), TEXT("Players")), ShellRect(800, 0, 0, 0).Min.X, ShellRect(0, 96, 0, 0).Min.Y, ShellDim, 15.f, 2);

	// Test automation: "Map=<name>" (the name the list shows, or the map's folder).
	if (Shell.TestClick.StartsWith(TEXT("Map="), ESearchCase::IgnoreCase))
	{
		const FString Wanted = Shell.TestClick.Mid(4);
		Shell.TestClick.Reset();
		const int32 Index = Maps.IndexOfByPredicate([&Wanted](const FGenMapInfo& M)
		{
			return M.DisplayName.Equals(Wanted, ESearchCase::IgnoreCase) || M.Name.Equals(Wanted, ESearchCase::IgnoreCase);
		});
		if (Index != INDEX_NONE)
		{
			Shell.MapChoice = Maps[Index].Name;
			Shell.MapScroll = float(FMath::Max(0, Index - 6));
		}
		else
		{
			UE_LOG(LogGenerals, Warning, TEXT("Test menu: no map '%s'"), *Wanted);
		}
	}

	// The list: the wheel moves it.
	constexpr int32 Rows = 15;
	constexpr float RowHeight = 40.f;
	const FBox2D ListArea = ShellRect(250, 130, 560, Rows * RowHeight);
	const float MaxScroll = float(FMath::Max(0, Maps.Num() - Rows));
	if (ListArea.IsInside(Shell.Mouse))
	{
		Shell.MapScroll -= Shell.Wheel * 2.f;
	}
	Shell.MapScroll = FMath::Clamp(Shell.MapScroll, 0.f, MaxScroll);
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.5f), ListArea.Min.X, ListArea.Min.Y, ListArea.GetSize().X, ListArea.GetSize().Y);
	const int32 First = FMath::RoundToInt32(Shell.MapScroll);
	for (int32 r = 0; r < Rows && First + r < Maps.Num(); ++r)
	{
		const FGenMapInfo& Map = Maps[First + r];
		const FBox2D Row = ShellRect(250, 130 + r * RowHeight, 548, RowHeight - 2);
		const bool bChosen = Map.Name.Equals(Shell.MapChoice, ESearchCase::IgnoreCase);
		const bool bHover = ShellHover(Row);
		if (bChosen || bHover)
		{
			DrawRect(bChosen ? ShellHot : FLinearColor(0.06f, 0.1f, 0.2f, 0.9f), Row.Min.X, Row.Min.Y, Row.GetSize().X, Row.GetSize().Y);
		}
		ShellText(Map.DisplayName, Row.Min.X + 12.f * S, Row.Min.Y + 8.f * S, bChosen ? FLinearColor::White : ShellTextColor, 18.f, 0, bChosen);
		ShellText(FString::FromInt(Map.NumPlayers), Row.Max.X - 14.f * S, Row.Min.Y + 8.f * S, bChosen ? FLinearColor::White : ShellDim, 18.f, 2);
		if (bHover && Shell.bClick)
		{
			Shell.bClick = false;
			Shell.MapChoice = Map.Name;
			ShellClickSound();
		}
	}
	if (MaxScroll > 0.f)
	{
		// Where in the list the window is.
		const float Track = float(ListArea.GetSize().Y);
		const float Thumb = Track * Rows / Maps.Num();
		DrawRect(ShellEdge, ListArea.Max.X - 6.f * S, ListArea.Min.Y + (Track - Thumb) * Shell.MapScroll / MaxScroll, 5.f * S, Thumb);
	}

	if (const FGenMapInfo* Map = FGenShellData::Get().FindMap(Shell.MapChoice))
	{
		DrawMapPreview(*Map, ShellRect(840, 130, 500, 500), false);
		ShellText(Map->DisplayName, ShellRect(840, 0, 0, 0).Min.X, ShellRect(0, 644, 0, 0).Min.Y, FLinearColor::White, 24.f, 0, true);
		ShellText(FString::Printf(TEXT("%s %d"), *FGenShellData::Text(TEXT("GUI:NumPlayers"), TEXT("Players:")), Map->NumPlayers),
			ShellRect(840, 0, 0, 0).Min.X, ShellRect(0, 682, 0, 0).Min.Y, ShellTextColor, 18.f);
		ShellText(FString::Printf(TEXT("%d x %d"), FMath::RoundToInt32(Map->ExtentMax.X - Map->ExtentMin.X), FMath::RoundToInt32(Map->ExtentMax.Y - Map->ExtentMin.Y)),
			ShellRect(1340, 0, 0, 0).Min.X, ShellRect(0, 682, 0, 0).Min.Y, ShellDim, 16.f, 2);
	}
	if (ShellButton(TEXT("MapBack"), ShellRect(250, 762, 260, 54), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		Shell.bMapSelect = false;
	}
	if (ShellButton(TEXT("MapAccept"), ShellRect(1080, 762, 260, 54), FGenShellData::Text(TEXT("GUI:Accept"), TEXT("ACCEPT"))))
	{
		if (Setup)
		{
			Setup->SetMap(Shell.MapChoice);
		}
		Shell.bMapSelect = false;
	}
}

void AGenHUD::DrawLoading()
{
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.75f);
	const UGenSkirmishSetup* Setup = UGenSkirmishSetup::Get(this);
	const FGenMapInfo* Map = Setup ? Setup->GetMapInfo() : nullptr;
	ShellText(FGenShellData::Text(TEXT("GUI:Loading"), TEXT("LOADING")), Canvas->SizeX * 0.5f, ShellRect(0, 390, 0, 0).Min.Y, ShellGold, 46.f, 1, true);
	if (Map)
	{
		ShellText(Map->DisplayName, Canvas->SizeX * 0.5f, ShellRect(0, 460, 0, 0).Min.Y, ShellTextColor, 24.f, 1);
	}
}

// ---- during and after a game ------------------------------------------------------------------------

void AGenHUD::DrawQuitMenu(AGenGameMode* GM)
{
	// Test automation opens it the way Esc does.
	if (ShellTestHit(TEXT("Menu.Open")) && !GM->IsQuitMenuOpen())
	{
		GM->ToggleQuitMenu();
	}
	if (!GM->IsQuitMenuOpen())
	{
		return;
	}
	DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.55f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(590, 250, 420, 400), 0.95f);
	if (!ShellImage(TEXT("GeneralsLogo"), ShellRect(700, 266, 200, 100)))
	{
		ShellText(TEXT("GENERALS"), Canvas->SizeX * 0.5f, ShellRect(0, 296, 0, 0).Min.Y, ShellGold, 30.f, 1, true);
	}
	if (ShellButton(TEXT("Menu.Resume"), ShellRect(630, 390, 340, 60), FGenShellData::Text(TEXT("GUI:ReturnToGame"), TEXT("RETURN TO GAME"))))
	{
		GM->ToggleQuitMenu();
	}
	// (A network game goes on whatever one player does: no restart there, and the game doesn't stand still.)
	else if (ShellButton(TEXT("Menu.Restart"), ShellRect(630, 470, 340, 60), FGenShellData::Text(TEXT("GUI:RestartGame"), TEXT("RESTART GAME")),
		!(UGenGameLogic::Get(this) && UGenGameLogic::Get(this)->IsNetworkGame())))
	{
		GM->RestartGame();
	}
	else if (ShellButton(TEXT("Menu.Exit"), ShellRect(630, 550, 340, 60), FGenShellData::Text(TEXT("GUI:Exit"), TEXT("EXIT GAME"))))
	{
		GM->EndGame();
	}
}

void AGenHUD::DrawScoreScreen(AGenGameMode* GM, UGenGameLogic* Logic)
{
	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FGenPlayerTemplate* LocalSide = Players.IsValidIndex(Local) ? Players[Local].Template : nullptr;
	if (!LocalSide || !ShellImage(LocalSide->ScoreScreenImage, FBox2D(FVector2D::ZeroVector, FVector2D(Canvas->SizeX, Canvas->SizeY)), FLinearColor(0.3f, 0.3f, 0.3f)))
	{
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.85f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	}

	const bool bWon = Logic->HasLocalPlayerWon();
	const bool bLost = Logic->IsPlayerDefeated(Local);
	const FString Title = bWon ? FGenShellData::Text(TEXT("GUI:Victory"), TEXT("Victory"))
		: (bLost ? FGenShellData::Text(TEXT("GUI:Defeat"), TEXT("Defeat")) : FGenShellData::Text(TEXT("GUI:Score"), TEXT("Score")));
	ShellText(Title.ToUpper(), Canvas->SizeX * 0.5f, ShellRect(0, 50, 0, 0).Min.Y, bWon ? ShellGold : (bLost ? ShellRed : ShellTextColor), 54.f, 1, true);

	// grabMultiPlayerInfo: everybody who played, the best score first.
	TArray<const FGenPlayer*> Rows;
	for (const FGenPlayer& P : Players)
	{
		if (!P.bNeutral && P.Template && P.StartPosition >= 0)
		{
			Rows.Add(&P);
		}
	}
	Rows.StableSort([](const FGenPlayer& A, const FGenPlayer& B) { return A.Score.Calculate(A.Index) > B.Score.Calculate(B.Index); });

	ShellPanel(ShellRect(90, 170, 1420, 110 + Rows.Num() * 56), 0.82f);
	// Three columns each for units and buildings, one for the supplies (the string file has their titles in two lines).
	const float Columns[] = { 660.f, 780.f, 900.f, 1050.f, 1170.f, 1290.f, 1470.f };
	const float GroupY = ShellRect(0, 186, 0, 0).Min.Y, HeadY = ShellRect(0, 222, 0, 0).Min.Y;
	const FString Supplies = FGenShellData::Text(TEXT("GUI:ResourcesCollected"), TEXT("Supplies \n Collected"));
	FString SuppliesTop, SuppliesRest;
	if (!Supplies.Split(TEXT("\\n"), &SuppliesTop, &SuppliesRest) && !Supplies.Split(TEXT("\n"), &SuppliesTop, &SuppliesRest))
	{
		SuppliesTop = Supplies;
	}
	ShellText(FGenShellData::Text(TEXT("GUI:Units"), TEXT("Units")), ShellRect(780, 0, 0, 0).Min.X, GroupY, ShellGold, 20.f, 2, true);
	ShellText(FGenShellData::Text(TEXT("GUI:Buildings"), TEXT("Buildings")), ShellRect(1170, 0, 0, 0).Min.X, GroupY, ShellGold, 20.f, 2, true);
	ShellText(SuppliesTop.TrimStartAndEnd(), ShellRect(1470, 0, 0, 0).Min.X, GroupY, ShellGold, 20.f, 2, true);
	const FString Heads[] = {
		SecondLine(FGenShellData::Text(TEXT("GUI:UnitsBuilt"), TEXT("Created"))), SecondLine(FGenShellData::Text(TEXT("GUI:UnitsLost"), TEXT("Lost"))),
		SecondLine(FGenShellData::Text(TEXT("GUI:UnitsKilled"), TEXT("Destroyed"))), SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsBuilt"), TEXT("Constructed"))),
		SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsLost"), TEXT("Lost"))), SecondLine(FGenShellData::Text(TEXT("GUI:BuildingsKilled"), TEXT("Destroyed"))),
		SecondLine(Supplies) };
	for (int32 c = 0; c < UE_ARRAY_COUNT(Columns); ++c)
	{
		ShellText(Heads[c], ShellRect(Columns[c], 0, 0, 0).Min.X, HeadY, ShellDim, 15.f, 2);
	}
	for (int32 r = 0; r < Rows.Num(); ++r)
	{
		const FGenPlayer& P = *Rows[r];
		const float Y = ShellRect(0, 262 + r * 56, 0, 0).Min.Y;
		FLinearColor Color = P.Color;
		Color.A = 1.f;
		const int32 SideIndex = FGenShellData::Get().FindSide(P.Template->Name);
		const FString Side = FGenShellData::Get().GetSides().IsValidIndex(SideIndex) ? FGenShellData::Get().GetSides()[SideIndex].Label : P.Template->Side;
		ShellImage(P.Template->SideIcon, ShellRect(110, 256 + r * 56, 40, 40));
		ShellText(P.Name, ShellRect(164, 0, 0, 0).Min.X, Y, Color, 22.f, 0, true);
		ShellText(Side, ShellRect(400, 0, 0, 0).Min.X, Y + 3.f * Shell.Scale, ShellDim, 17.f);
		const int32 Values[] = { P.Score.UnitsBuilt, P.Score.UnitsLost, P.Score.GetUnitsDestroyed(), P.Score.BuildingsBuilt, P.Score.BuildingsLost,
			P.Score.GetBuildingsDestroyed(), P.Score.MoneyEarned };
		for (int32 c = 0; c < UE_ARRAY_COUNT(Columns); ++c)
		{
			ShellText(FString::FromInt(Values[c]), ShellRect(Columns[c], 0, 0, 0).Min.X, Y, Color, 22.f, 2);
		}
	}

	if (GM->CanKeepWatching() && ShellButton(TEXT("Score.Watch"), ShellRect(940, 800, 300, 60), FGenShellData::Text(TEXT("GUI:ReturnToGame"), TEXT("RETURN TO GAME"))))
	{
		GM->KeepWatching();
	}
	else if (ShellButton(TEXT("Score.Continue"), ShellRect(1260, 800, 300, 60), FGenShellData::Text(TEXT("GUI:Continue"), TEXT("CONTINUE"))))
	{
		GM->ExitToMenu();
	}
}
