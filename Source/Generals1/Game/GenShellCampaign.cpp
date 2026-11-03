// The campaign menu (the original's SinglePlayerMenu and DifficultySelect): a side's campaign, its missions in order -
// those this game can play so far open, the rest listed but locked - and the difficulty, then "start".
//
// Ids for test automation: Campaign.Challenge, General=<index>, Opponent=<mission>, Challenge.Back, Challenge.Start.
// Ids for test automation: Main.Campaign (the main menu), Campaign=<name>, Mission=<name>, Difficulty=<0..2>,
// Campaign.Start, Campaign.Back.

#include "Game/GenHUD.h"
#include "Game/GenCampaign.h"
#include "Game/GenGameMode.h"
#include "Game/GenShellData.h"
#include "Game/GenShellStyle.h"
#include "Logic/GenCommandData.h"
#include "Logic/GenGameLogic.h"
#include "Audio/GenAudioManager.h"
#include "Engine/Canvas.h"

using namespace GenShellStyle;

namespace
{
	/** A label of generals.csf, or the fallback when the game has no such text. */
	FString CampaignText(const FString& Label, const FString& Fallback)
	{
		if (Label.IsEmpty())
		{
			return Fallback;
		}
		const FString Text = FGenCommandData::Get().GetText(Label);
		return Text.IsEmpty() || Text.Equals(Label, ESearchCase::IgnoreCase) ? Fallback : Text;
	}
}

void AGenHUD::DrawCampaignMenu(AGenGameMode* GM)
{
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	ShellHeader(TEXT("Campaign"), TEXT("Main menu"));
	ShellBottomBar();

	// The sides' campaigns (Campaign.ini; the Generals Challenge is another game), USA, China and GLA first.
	TArray<const FGenCampaign*> List;
	for (const FGenCampaign& C : FGenCampaignData::Get().GetCampaigns())
	{
		if (!C.bChallenge && C.Missions.Num() > 0)
		{
			List.Add(&C);
		}
	}
	auto Order = [](const FGenCampaign* C)
	{
		static const TCHAR* Sides[] = { TEXT("USA"), TEXT("China"), TEXT("GLA") };
		for (int32 i = 0; i < UE_ARRAY_COUNT(Sides); ++i)
		{
			if (C->Name.Equals(Sides[i], ESearchCase::IgnoreCase)) { return i; }
		}
		return 99;
	};
	List.StableSort([&Order](const FGenCampaign& A, const FGenCampaign& B) { return Order(&A) < Order(&B); });
	if (List.Num() == 0)
	{
		ShellText(TEXT("No campaigns found (Data\\INI\\Campaign.ini)"), ShellRect(330, 200, 0, 0).Min.X, ShellRect(0, 200, 0, 0).Min.Y, ShellDim, 18.f);
	}
	const FGenCampaign* Chosen = List.Num() > 0 ? List[0] : nullptr;
	for (const FGenCampaign* C : List)
	{
		if (C->Name.Equals(CampaignMenu.Campaign, ESearchCase::IgnoreCase))
		{
			Chosen = C;
		}
	}

	// --- the campaigns ---
	ShellPanel(ShellRect(60, 140, 440, 620), 0.9f);
	ShellSection(TEXT("Side"), 90, 158, 380);
	float Y = 210.f;
	for (const FGenCampaign* C : List)
	{
		int32 Open = 0;
		for (const FGenCampaignMission& M : C->Missions)
		{
			Open += FGenCampaignData::IsMissionPorted(C->Name, M.Name) ? 1 : 0;
		}
		const FBox2D R = ShellRect(90, Y, 380, 76);
		if (C == Chosen)
		{
			DrawRect(FLinearColor(ShellGold.R, ShellGold.G, ShellGold.B, 0.9f), R.Min.X - 8.f * Shell.Scale, R.Min.Y, 4.f * Shell.Scale, R.GetSize().Y);
		}
		const FString Sub = FString::Printf(TEXT("%d missions, %d playable"), C->Missions.Num(), Open);
		if (ShellMenuItem(TEXT("Campaign=") + C->Name, R, CampaignText(C->NameLabel, C->Name), Sub))
		{
			CampaignMenu.Campaign = C->Name;
			CampaignMenu.Mission.Reset();
			Chosen = C;
		}
		Y += 88.f;
	}

	if (ShellMenuItem(TEXT("Campaign.Challenge"), ShellRect(90, Y, 380, 76), TEXT("Generals' Challenge"), TEXT("A general against all the others")))
	{
		GM->ClearShellError();
		GM->SetMenuScreen(EGenMenuScreen::Challenge);
	}

	// --- its missions ---
	ShellPanel(ShellRect(530, 140, 560, 620), 0.9f);
	ShellSection(TEXT("Missions"), 560, 158, 500);
	const FGenCampaignMission* Mission = nullptr;
	if (Chosen)
	{
		for (const FGenCampaignMission& M : Chosen->Missions)
		{
			if (M.Name.Equals(CampaignMenu.Mission, ESearchCase::IgnoreCase) && FGenCampaignData::IsMissionPorted(Chosen->Name, M.Name))
			{
				Mission = &M;
			}
		}
		if (!Mission)
		{
			// (The first one that can be played.)
			Mission = Chosen->Missions.FindByPredicate([Chosen](const FGenCampaignMission& M) { return FGenCampaignData::IsMissionPorted(Chosen->Name, M.Name); });
		}
		Y = 206.f;
		const float Step = FMath::Min(70.f, 540.f / FMath::Max(1, Chosen->Missions.Num()));
		for (int32 i = 0; i < Chosen->Missions.Num(); ++i)
		{
			const FGenCampaignMission& M = Chosen->Missions[i];
			const bool bOpen = FGenCampaignData::IsMissionPorted(Chosen->Name, M.Name);
			const FBox2D R = ShellRect(560, Y, 500, Step - 8.f);
			if (&M == Mission)
			{
				DrawRect(FLinearColor(ShellGold.R, ShellGold.G, ShellGold.B, 0.9f), R.Min.X - 8.f * Shell.Scale, R.Min.Y, 4.f * Shell.Scale, R.GetSize().Y);
			}
			const FString Title = CampaignText(FString::Printf(TEXT("MAP:%sTitle"), *M.Map), M.Map);
			const FString Label = FString::Printf(TEXT("%d.  %s"), i + 1, *Title);
			const FString Sub = bOpen ? CampaignText(M.LocationName, M.Map) : FString(TEXT("Not ported yet"));
			if (ShellMenuItem(TEXT("Mission=") + M.Name, R, Label, Sub, bOpen))
			{
				CampaignMenu.Mission = M.Name;
				Mission = &M;
			}
			Y += Step;
		}
	}

	// --- the difficulty, and what the mission is about ---
	ShellPanel(ShellRect(1120, 140, 420, 620), 0.9f);
	ShellSection(TEXT("Difficulty"), 1150, 158, 360);
	static const TCHAR* Levels[] = { TEXT("GUI:Easy"), TEXT("GUI:Medium"), TEXT("GUI:Hard") };
	static const TCHAR* LevelNames[] = { TEXT("EASY"), TEXT("MEDIUM"), TEXT("HARD") };
	for (int32 d = 0; d < 3; ++d)
	{
		const FBox2D R = ShellRect(1150, 206.f + d * 66.f, 360, 54);
		const FString Id = FString::Printf(TEXT("Difficulty=%d"), d);
		const FString Name = FGenShellData::Text(Levels[d], LevelNames[d]).ToUpper();
		if (d == CampaignMenu.Difficulty ? ShellPrimaryButton(Id, R, Name) : ShellButton(Id, R, Name))
		{
			CampaignMenu.Difficulty = d;
		}
	}
	if (Mission)
	{
		ShellSection(TEXT("Objectives"), 1150, 430, 360);
		float TY = 476.f;
		for (const FString& Line : Mission->ObjectiveLines)
		{
			const FString Text = CampaignText(Line, FString());
			if (!Text.IsEmpty())
			{
				ShellText(Text, ShellRect(1150, 0, 0, 0).Min.X, ShellRect(0, TY, 0, 0).Min.Y, ShellTextColor, 15.f);
				TY += 26.f;
			}
		}
	}

	if (ShellButton(TEXT("Campaign.Back"), ShellRect(40, 812, 240, 56), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		GM->SetMenuScreen(EGenMenuScreen::Main);
	}
	if (ShellPrimaryButton(TEXT("Campaign.Start"), ShellRect(1260, 812, 300, 56), FGenShellData::Text(TEXT("GUI:Start"), TEXT("START")).ToUpper(), Chosen && Mission))
	{
		GM->RequestCampaignStart(Chosen->Name, Mission->Name, CampaignMenu.Difficulty);
	}
	const FString& Error = GM->GetShellError();
	if (!Error.IsEmpty())
	{
		ShellText(Error, ShellRect(300, 0, 0, 0).Min.X, ShellRect(0, 826, 0, 0).Min.Y, FLinearColor(1.f, 0.4f, 0.3f), 16.f);
	}
}

// ---- the Generals' Challenge (ChallengeMenu.cpp, ChallengeLoadScreen, displayChallengeWinLoss) -------------------

void AGenHUD::DrawChallengeMenu(AGenGameMode* GM)
{
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.72f);
	ShellHeader(TEXT("Generals' Challenge"), TEXT("Campaign"));
	ShellBottomBar();

	// "Prevent from selecting the disabled Generals."
	FGenCampaignData& Data = FGenCampaignData::Get();
	const TArray<FGenChallengeGeneral>& Generals = Data.GetChallengeGenerals();
	TArray<int32> Open;
	for (int32 i = 0; i < Generals.Num(); ++i)
	{
		if (Generals[i].bStartsEnabled && Data.FindCampaign(Generals[i].Campaign))
		{
			Open.Add(i);
		}
	}
	if (Open.Num() > 0 && !Open.Contains(CampaignMenu.General))
	{
		CampaignMenu.General = Open[0];
	}
	const FGenChallengeGeneral* General = Generals.IsValidIndex(CampaignMenu.General) ? &Generals[CampaignMenu.General] : nullptr;
	const FGenCampaign* Campaign = General ? Data.FindCampaign(General->Campaign) : nullptr;

	// --- the generals ---
	ShellPanel(ShellRect(60, 140, 440, 620), 0.9f);
	ShellSection(TEXT("Your general"), 90, 158, 380);
	const float GeneralStep = FMath::Min(76.f, 546.f / FMath::Max(1, Open.Num()));
	float Y = 206.f;
	for (const int32 i : Open)
	{
		const FGenChallengeGeneral& G = Generals[i];
		const FBox2D R = ShellRect(90, Y, 380, GeneralStep - 6.f);
		if (i == CampaignMenu.General)
		{
			DrawRect(FLinearColor(ShellGold.R, ShellGold.G, ShellGold.B, 0.9f), R.Min.X - 8.f * Shell.Scale, R.Min.Y, 4.f * Shell.Scale, R.GetSize().Y);
		}
		if (ShellMenuItem(FString::Printf(TEXT("General=%d"), i), R, CampaignText(G.BioName, G.PlayerTemplate), CampaignText(G.BioBranch, FString())))
		{
			CampaignMenu.General = i;
			CampaignMenu.Opponent.Reset();
			if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
			{
				Audio->PlayUI(G.SelectionSound);
			}
		}
		Y += GeneralStep;
	}

	// --- who it fights, in order ---
	ShellPanel(ShellRect(530, 140, 560, 620), 0.9f);
	ShellSection(TEXT("Opponents"), 560, 158, 500);
	const FGenCampaignMission* Mission = nullptr;
	if (Campaign)
	{
		Mission = Campaign->FindMission(CampaignMenu.Opponent);
		if (!Mission)
		{
			Mission = Campaign->FindMission(Campaign->FirstMission);
		}
		Y = 206.f;
		const float Step = FMath::Min(70.f, 540.f / FMath::Max(1, Campaign->Missions.Num()));
		for (int32 i = 0; i < Campaign->Missions.Num(); ++i)
		{
			const FGenCampaignMission& M = Campaign->Missions[i];
			const FBox2D R = ShellRect(560, Y, 500, Step - 8.f);
			if (&M == Mission)
			{
				DrawRect(FLinearColor(ShellGold.R, ShellGold.G, ShellGold.B, 0.9f), R.Min.X - 8.f * Shell.Scale, R.Min.Y, 4.f * Shell.Scale, R.GetSize().Y);
			}
			const FString Label = FString::Printf(TEXT("%d.  %s"), i + 1, *CampaignText(M.GeneralName, M.Map));
			if (ShellMenuItem(TEXT("Opponent=") + M.Name, R, Label, CampaignText(FString::Printf(TEXT("MAP:%sTitle"), *M.Map), M.Map)))
			{
				CampaignMenu.Opponent = M.Name;
				Mission = &M;
			}
			Y += Step;
		}
	}

	// --- the difficulty, and the general's file ---
	ShellPanel(ShellRect(1120, 140, 420, 620), 0.9f);
	ShellSection(TEXT("Difficulty"), 1150, 158, 360);
	static const TCHAR* Levels[] = { TEXT("GUI:Easy"), TEXT("GUI:Medium"), TEXT("GUI:Hard") };
	static const TCHAR* LevelNames[] = { TEXT("EASY"), TEXT("MEDIUM"), TEXT("HARD") };
	for (int32 d = 0; d < 3; ++d)
	{
		const FBox2D R = ShellRect(1150, 206.f + d * 66.f, 360, 54);
		const FString Id = FString::Printf(TEXT("Difficulty=%d"), d);
		const FString Name = FGenShellData::Text(Levels[d], LevelNames[d]).ToUpper();
		if (d == CampaignMenu.Difficulty ? ShellPrimaryButton(Id, R, Name) : ShellButton(Id, R, Name))
		{
			CampaignMenu.Difficulty = d;
		}
	}
	if (General)
	{
		ShellSection(CampaignText(General->BioName, TEXT("General")), 1150, 420, 360);
		DrawMappedImage(General->PortraitLarge, ShellRect(1150, 466, 170, 214));
		float TY = 470.f;
		for (const FString* Line : { &General->BioRank, &General->BioBranch, &General->BioStrategy })
		{
			const FString Text = CampaignText(*Line, FString());
			if (!Text.IsEmpty())
			{
				ShellText(Text, ShellRect(1334, 0, 0, 0).Min.X, ShellRect(0, TY, 0, 0).Min.Y, ShellTextColor, 14.f);
				TY += 30.f;
			}
		}
	}

	if (ShellButton(TEXT("Challenge.Back"), ShellRect(40, 812, 240, 56), FGenShellData::Text(TEXT("GUI:Back"), TEXT("BACK"))))
	{
		GM->SetMenuScreen(EGenMenuScreen::Campaign);
	}
	if (Mission && ShellPrimaryButton(TEXT("Challenge.Start"), ShellRect(1260, 812, 300, 56), FGenShellData::Text(TEXT("GUI:Start"), TEXT("START")).ToUpper()))
	{
		GM->RequestCampaignStart(Campaign->Name, Mission->Name, CampaignMenu.Difficulty);
	}
	const FString& Error = GM->GetShellError();
	if (!Error.IsEmpty())
	{
		ShellText(Error, ShellRect(300, 0, 0, 0).Min.X, ShellRect(0, 826, 0, 0).Min.Y, FLinearColor(1.f, 0.4f, 0.3f), 16.f);
	}
}

void AGenHUD::DrawChallengeLoading(const FGenCampaign& Campaign, const FString& MissionName)
{
	// (The original's portraits are movies, which this game doesn't play: the bio pictures stand in.)
	FGenCampaignData& Data = FGenCampaignData::Get();
	const FGenCampaignMission* Mission = Campaign.FindMission(MissionName.IsEmpty() ? Campaign.FirstMission : MissionName);
	const FGenChallengeGeneral* Player = Data.FindGeneralByCampaign(Campaign.Name);
	const FGenChallengeGeneral* Opponent = Mission ? Data.FindGeneralByName(Mission->GeneralName) : nullptr;
	ShellBackdrop(TEXT("MainMenuBackdrop"), 0.8f);
	auto Side = [this](const FGenChallengeGeneral* G, float X)
	{
		if (G)
		{
			DrawMappedImage(G->PortraitLarge, ShellRect(X, 220, 340, 428));
			ShellTextEx(CampaignText(G->BioName, FString()).ToUpper(), ShellRect(X + 170.f, 0, 0, 0).Min.X, ShellRect(0, 670, 0, 0).Min.Y, FLinearColor::White, 24.f, 1, true, 60);
		}
	};
	Side(Player, 200.f);
	Side(Opponent, 1060.f);
	ShellTextEx(TEXT("VS"), Canvas->SizeX * 0.5f, ShellRect(0, 400, 0, 0).Min.Y, ShellGold, 72.f, 1, true, 100);
	if (Mission)
	{
		ShellText(CampaignText(FString::Printf(TEXT("MAP:%sTitle"), *Mission->Map), Mission->Map), Canvas->SizeX * 0.5f, ShellRect(0, 760, 0, 0).Min.Y, ShellDim * 1.4f, 20.f, 1);
	}
	ShellTextEx(FGenShellData::Text(TEXT("GUI:Loading"), TEXT("LOADING")).ToUpper(), Canvas->SizeX * 0.5f, ShellRect(0, 810, 0, 0).Min.Y, ShellGold, 20.f, 1, true, 200);
}

void AGenHUD::DrawChallengeResult(const FGenMatchSettings& Match, bool bWon, float Top)
{
	FGenCampaignData& Data = FGenCampaignData::Get();
	const FGenCampaign* Campaign = Data.FindCampaign(Match.Campaign);
	const FGenCampaignMission* Mission = Campaign ? Campaign->FindMission(Match.Mission) : nullptr;
	const FGenChallengeGeneral* Opponent = Mission ? Data.FindGeneralByName(Mission->GeneralName) : nullptr;
	if (!Opponent)
	{
		return;
	}
	if (!bChallengeSoundPlayed)
	{
		bChallengeSoundPlayed = true;
		if (UGenAudioManager* Audio = UGenAudioManager::Get(this))
		{
			Audio->PlayUI(bWon ? Opponent->WinSound : Opponent->LossSound);
		}
	}
	ShellPanel(ShellRect(90, Top, 1420, 790.f - Top), 0.9f);
	const float Height = 790.f - Top - 28.f;
	DrawMappedImage(bWon ? Opponent->DefeatedImage : Opponent->VictoriousImage, ShellRect(114, Top + 14.f, Height * 0.8f, Height));
	const float X = ShellRect(114.f + Height * 0.8f + 28.f, 0, 0, 0).Min.X;
	FString Header = CampaignText(bWon ? TEXT("GUI:ChallengeWinText") : TEXT("GUI:ChallengeLossText"), FString());
	Header.ReplaceInline(TEXT("%s"), *CampaignText(Mission->GeneralName, FString()));
	Header.ReplaceInline(TEXT("%ls"), *CampaignText(Mission->GeneralName, FString()));
	ShellTextEx(Header.ToUpper(), X, ShellRect(0, Top + 26.f, 0, 0).Min.Y, ShellGold, 22.f, 0, true, 40);

	// (The general's last word: a paragraph, in lines of about seventy letters.)
	TArray<FString> Words;
	CampaignText(bWon ? Opponent->DefeatedString : Opponent->VictoriousString, FString()).ParseIntoArrayWS(Words);
	FString Line;
	float LineY = Top + 76.f;
	for (int32 i = 0; i <= Words.Num(); ++i)
	{
		if (i == Words.Num() || Line.Len() + Words[i].Len() > 70)
		{
			ShellText(Line, X, ShellRect(0, LineY, 0, 0).Min.Y, ShellTextColor, 17.f);
			LineY += 28.f;
			Line.Reset();
		}
		if (i < Words.Num())
		{
			Line += (Line.IsEmpty() ? TEXT("") : TEXT(" ")) + Words[i];
		}
	}
}
