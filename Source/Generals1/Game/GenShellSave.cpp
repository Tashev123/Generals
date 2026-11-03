// Saving and loading in the menus: the list of saved games (the in-game menu's "save game" and "load game", the main
// menu's "load game") and the screen while a saved game is played up to where it was left (see FGenSavedGame).
//
// Ids for test automation: Menu.Save, Menu.Load, Main.Load, Save.New, Save.Row=<n>, Save.Del=<n>, Save.Prev, Save.Next, Save.Back.

#include "Game/GenHUD.h"
#include "Game/GenGameMode.h"
#include "Game/GenShellData.h"
#include "Game/GenShellStyle.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenMatch.h"
#include "Generals1.h"
#include "Engine/Canvas.h"

using namespace GenShellStyle;

namespace
{
	constexpr int32 RowsPerPage = 7;

	FString DescribeSave(const FGenSaveInfo& Info)
	{
		const int32 Seconds = int32(Info.Frame) / FMath::Max(1, GenLogic::GetLogicFPS());
		const FString Played = Seconds >= 3600
			? FString::Printf(TEXT("%d:%02d:%02d"), Seconds / 3600, (Seconds / 60) % 60, Seconds % 60)
			: FString::Printf(TEXT("%d:%02d"), Seconds / 60, Seconds % 60);
		return FString::Printf(TEXT("%s   -   %s into the game"), *Info.Date, *Played);
	}
}

void AGenHUD::OpenSaveLoad(bool bSaving)
{
	Shell.SaveMode = bSaving ? 1 : 2;
	Shell.SavePage = 0;
	Shell.SaveConfirm.Reset();
	FGenSavedGame::List(Shell.SaveList);
}

void AGenHUD::DrawSaveLoadList(AGenGameMode* GM, float Top)
{
	const bool bSaving = Shell.SaveMode == 1;
	const float RowStep = 72.f;
	float Y = Top;
	if (bSaving)
	{
		if (ShellPrimaryButton(TEXT("Save.New"), ShellRect(370, Y, 860, 54), TEXT("NEW SAVED GAME")))
		{
			if (GM->SaveGame(FString()))
			{
				Shell.SaveMode = 0;
				return;
			}
		}
		Y += 66.f;
	}
	const int32 NumPages = FMath::Max(1, (Shell.SaveList.Num() + RowsPerPage - 1) / RowsPerPage);
	Shell.SavePage = FMath::Clamp(Shell.SavePage, 0, NumPages - 1);
	if (Shell.SaveList.Num() == 0)
	{
		ShellText(TEXT("No saved games yet."), Canvas->SizeX * 0.5f, ShellRect(0, Y + 40.f, 0, 0).Min.Y, ShellDim * 1.4f, 18.f, 1);
	}
	const int32 RowsHere = bSaving ? RowsPerPage - 1 : RowsPerPage;
	for (int32 i = Shell.SavePage * RowsPerPage; i < Shell.SaveList.Num() && i < Shell.SavePage * RowsPerPage + RowsHere; ++i)
	{
		const FGenSaveInfo& Info = Shell.SaveList[i];
		const int32 Row = i - Shell.SavePage * RowsPerPage;
		const bool bAsked = Shell.SaveConfirm == Info.Path;
		const FString Sub = bSaving && bAsked ? FString(TEXT("Click again to save over this one")) : DescribeSave(Info);
		if (ShellMenuItem(FString::Printf(TEXT("Save.Row=%d"), Row), ShellRect(370, Y, 700, 64), Info.Title, Sub))
		{
			if (!bSaving)
			{
				GM->RequestLoadGame(Info.Path);
				return;
			}
			// (The player's work: saving over it takes a second click.)
			if (bAsked)
			{
				if (GM->SaveGame(Info.Path))
				{
					Shell.SaveMode = 0;
					return;
				}
			}
			Shell.SaveConfirm = Info.Path;
		}
		const FString DeleteKey = TEXT("del:") + Info.Path;
		const bool bDeleting = Shell.SaveConfirm == DeleteKey;
		if (ShellButton(FString::Printf(TEXT("Save.Del=%d"), Row), ShellRect(1085, Y + 8.f, 145, 48), bDeleting ? TEXT("SURE?") : TEXT("DELETE")))
		{
			if (bDeleting)
			{
				FGenSavedGame::Delete(Info.Path);
				FGenSavedGame::List(Shell.SaveList);
				Shell.SaveConfirm.Reset();
				return;
			}
			Shell.SaveConfirm = DeleteKey;
		}
		Y += RowStep;
	}
	const float FootY = Top + (bSaving ? 66.f : 0.f) + float(RowsHere) * RowStep + 8.f;
	if (NumPages > 1)
	{
		if (ShellButton(TEXT("Save.Prev"), ShellRect(370, FootY, 150, 48), TEXT("PREVIOUS"), Shell.SavePage > 0))
		{
			--Shell.SavePage;
		}
		if (ShellButton(TEXT("Save.Next"), ShellRect(530, FootY, 150, 48), TEXT("NEXT"), Shell.SavePage < NumPages - 1))
		{
			++Shell.SavePage;
		}
		ShellText(FString::Printf(TEXT("%d / %d"), Shell.SavePage + 1, NumPages), ShellRect(700, 0, 0, 0).Min.X, ShellRect(0, FootY + 12.f, 0, 0).Min.Y, ShellDim * 1.4f, 16.f, 0);
	}
}

void AGenHUD::DrawLoadGameMenu(AGenGameMode* GM)
{
	// The main menu's "load game".
	if (Shell.SaveMode != 2)
	{
		OpenSaveLoad(false);
	}
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	ShellHeader(TEXT("Load Game"), TEXT("Main menu"));
	ShellBottomBar();
	DrawSaveLoadList(GM, 150.f);
	if (ShellButton(TEXT("Save.Back"), ShellRect(40, 812, 240, 56), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		Shell.SaveMode = 0;
		GM->SetMenuScreen(EGenMenuScreen::Main);
	}
}

void AGenHUD::DrawSaveLoadPanel(AGenGameMode* GM)
{
	// Over the game, standing still behind the in-game menu.
	DrawRect(FLinearColor(0.f, 0.005f, 0.02f, 0.62f), 0.f, 0.f, Canvas->SizeX, Canvas->SizeY);
	ShellPanel(ShellRect(340, 70, 920, 770), 1.f);
	ShellTextEx(Shell.SaveMode == 1 ? TEXT("SAVE GAME") : TEXT("LOAD GAME"), Canvas->SizeX * 0.5f, ShellRect(0, 92, 0, 0).Min.Y, ShellGold, 28.f, 1, true, 120);
	DrawSaveLoadList(GM, 150.f);
	if (Shell.SaveMode != 0 && ShellButton(TEXT("Save.Back"), ShellRect(1050, 778, 180, 48), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		Shell.SaveMode = 0;
	}
}

void AGenHUD::DrawResumeScreen(AGenGameMode* GM)
{
	// The game is being played again up to where it was saved: the loading screen, with how far that has come.
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.15f);
	const float S = Shell.Scale;
	const float Top = ShellRect(0, 740, 0, 0).Min.Y;
	DrawRect(FLinearColor(0.f, 0.01f, 0.03f, 0.8f), 0.f, Top, Canvas->SizeX, Canvas->SizeY - Top);
	ShellTextEx(TEXT("LOADING SAVED GAME"), ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 790, 0, 0).Min.Y, ShellGold, 30.f, 0, true, 200);
	ShellText(GM->GetResumeTitle(), ShellRect(100, 0, 0, 0).Min.X, ShellRect(0, 834, 0, 0).Min.Y, FLinearColor::White, 22.f, 0, true);
	const UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const float Done = Logic ? FMath::Clamp(Logic->GetResumeProgress(), 0.f, 1.f) : 0.f;
	ShellText(FString::Printf(TEXT("%d%%"), FMath::FloorToInt32(Done * 100.f)), ShellRect(1500, 0, 0, 0).Min.X, ShellRect(0, 838, 0, 0).Min.Y, ShellDim * 1.3f, 17.f, 2);
	const FBox2D Bar = ShellRect(100, 876, 1400, 0);
	DrawRect(FLinearColor(0.02f, 0.04f, 0.09f, 0.9f), Bar.Min.X, Bar.Min.Y, Bar.GetSize().X, 3.f * S);
	DrawRect(ShellGold, Bar.Min.X, Bar.Min.Y, Bar.GetSize().X * Done, 3.f * S);
}
