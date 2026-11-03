// Campaign.ini (CampaignManager.cpp): the campaigns, their missions and the maps they are played on.

#include "Game/GenCampaign.h"
#include "Core/GenIni.h"
#include "Misc/Paths.h"
#include "Generals1.h"

const FGenCampaignMission* FGenCampaign::FindMission(const FString& MissionName) const
{
	return Missions.FindByPredicate([&MissionName](const FGenCampaignMission& M) { return M.Name.Equals(MissionName, ESearchCase::IgnoreCase); });
}

FGenCampaignData& FGenCampaignData::Get()
{
	static FGenCampaignData Instance;
	return Instance;
}

const TArray<FGenCampaign>& FGenCampaignData::GetCampaigns()
{
	Load();
	return Campaigns;
}

const FGenCampaign* FGenCampaignData::FindCampaign(const FString& Name)
{
	Load();
	return Campaigns.FindByPredicate([&Name](const FGenCampaign& C) { return C.Name.Equals(Name, ESearchCase::IgnoreCase); });
}

const TArray<FGenChallengeGeneral>& FGenCampaignData::GetChallengeGenerals()
{
	Load();
	return ChallengeGenerals;
}

const FGenChallengeGeneral* FGenCampaignData::FindGeneralByName(const FString& BioName)
{
	Load();
	return ChallengeGenerals.FindByPredicate([&BioName](const FGenChallengeGeneral& G) { return G.BioName.Equals(BioName, ESearchCase::IgnoreCase); });
}

const FGenChallengeGeneral* FGenCampaignData::FindGeneralByCampaign(const FString& CampaignName)
{
	Load();
	return ChallengeGenerals.FindByPredicate([&CampaignName](const FGenChallengeGeneral& G) { return G.Campaign.Equals(CampaignName, ESearchCase::IgnoreCase); });
}

bool FGenCampaignData::IsMissionPorted(const FString& Campaign, const FString& Mission)
{
	if (Campaign.StartsWith(TEXT("CHALLENGE_"), ESearchCase::IgnoreCase))
	{
		return true; // (the Generals' Challenge: every general's seven or eight opponents)
	}
	// One mission at a time: each needs the script actions, conditions and objects its map uses.
	// The menu plays whatever is on this list straight from the mission list: a mission is not
	// unlocked by finishing the one before it.
	static const TCHAR* Ported[][2] = {
		{ TEXT("USA"), TEXT("Mission01") },
		{ TEXT("USA"), TEXT("Mission02") },
		{ TEXT("USA"), TEXT("Mission03") },
		{ TEXT("USA"), TEXT("Mission04") },
		{ TEXT("USA"), TEXT("Mission05") },
		{ TEXT("GLA"), TEXT("Mission01") },
		{ TEXT("GLA"), TEXT("Mission02") },
		{ TEXT("GLA"), TEXT("Mission03") },
		{ TEXT("GLA"), TEXT("Mission04") },
		{ TEXT("GLA"), TEXT("Mission05") },
		{ TEXT("China"), TEXT("Mission01") },
		{ TEXT("China"), TEXT("Mission02") },
		{ TEXT("China"), TEXT("Mission03") },
		{ TEXT("China"), TEXT("Mission04") },
		{ TEXT("China"), TEXT("Mission05") },
	};
	for (const TCHAR* const* Row : Ported)
	{
		if (Campaign.Equals(Row[0], ESearchCase::IgnoreCase) && Mission.Equals(Row[1], ESearchCase::IgnoreCase))
		{
			return true;
		}
	}
	return false;
}

void FGenCampaignData::Load()
{
	if (bLoaded)
	{
		return;
	}
	bLoaded = true;
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Campaign.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("Campaign"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenCampaign& C = Campaigns.AddDefaulted_GetRef();
		C.Name = B->Name();
		C.NameLabel = B->GetString(TEXT("CampaignNameLabel"));
		C.FirstMission = B->GetString(TEXT("FirstMission"));
		C.FinalVictoryMovie = B->GetString(TEXT("FinalVictoryMovie"));
		C.PlayerFaction = B->GetString(TEXT("PlayerFaction"));
		C.bChallenge = B->GetBool(TEXT("IsChallengeCampaign"));
		for (const FGenIniEntry& E : B->Entries)
		{
			if (!E.Key.Equals(TEXT("Mission"), ESearchCase::IgnoreCase) || !E.Block)
			{
				continue;
			}
			const FGenIniBlock& MB = *E.Block;
			FGenCampaignMission& M = C.Missions.AddDefaulted_GetRef();
			M.Name = E.Value();
			// "Maps\MD_USA01\MD_USA01.map": the map actor loads maps by their folder's name.
			FString MapPath = MB.GetString(TEXT("Map"));
			MapPath.ReplaceInline(TEXT("/"), TEXT("\\"));
			M.Map = FPaths::GetBaseFilename(MapPath);
			M.NextMission = MB.GetString(TEXT("NextMission"));
			M.IntroMovie = MB.GetString(TEXT("IntroMovie"));
			M.GeneralName = MB.GetString(TEXT("GeneralName"));
			M.LocationName = MB.GetString(TEXT("LocationNameLabel"));
			for (int32 i = 0; i < 5; ++i)
			{
				const FString Line = MB.GetString(*FString::Printf(TEXT("ObjectiveLine%d"), i));
				if (!Line.IsEmpty())
				{
					M.ObjectiveLines.Add(Line);
				}
			}
		}
	}
	TArray<TSharedPtr<FGenIniBlock>> ChallengeBlocks;
	GenIni::ParseFile(TEXT("Data\\INI\\ChallengeMode.ini"), ChallengeBlocks);
	for (const TSharedPtr<FGenIniBlock>& B : ChallengeBlocks)
	{
		for (const FGenIniEntry& E : B->Entries)
		{
			if (!E.Block)
			{
				continue;
			}
			const FGenIniBlock& GB = *E.Block;
			FGenChallengeGeneral& G = ChallengeGenerals.AddDefaulted_GetRef();
			G.PlayerTemplate = GB.GetString(TEXT("PlayerTemplate"));
			G.bStartsEnabled = GB.GetBool(TEXT("StartsEnabled"));
			G.BioName = GB.GetString(TEXT("BioNameString"));
			G.BioRank = GB.GetString(TEXT("BioRankString"));
			G.BioBranch = GB.GetString(TEXT("BioBranchString"));
			G.BioStrategy = GB.GetString(TEXT("BioStrategyString"));
			G.PortraitSmall = GB.GetString(TEXT("BioPortraitSmall"));
			G.PortraitLarge = GB.GetString(TEXT("BioPortraitLarge"));
			G.DefeatedImage = GB.GetString(TEXT("DefeatedImage"));
			G.VictoriousImage = GB.GetString(TEXT("VictoriousImage"));
			G.DefeatedString = GB.GetString(TEXT("DefeatedString"));
			G.VictoriousString = GB.GetString(TEXT("VictoriousString"));
			G.SelectionSound = GB.GetString(TEXT("SelectionSound"));
			G.WinSound = GB.GetString(TEXT("WinSound"));
			G.LossSound = GB.GetString(TEXT("LossSound"));
			G.Campaign = GB.GetString(TEXT("Campaign"));
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Campaign.ini: %d campaigns, ChallengeMode.ini: %d generals"), Campaigns.Num(), ChallengeGenerals.Num());
}
