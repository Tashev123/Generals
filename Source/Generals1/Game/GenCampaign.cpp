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

bool FGenCampaignData::IsMissionPorted(const FString& Campaign, const FString& Mission)
{
	// One mission at a time: each needs the script actions, conditions and objects its map uses.
	return Campaign.Equals(TEXT("USA"), ESearchCase::IgnoreCase) && Mission.Equals(TEXT("Mission01"), ESearchCase::IgnoreCase);
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
	UE_LOG(LogGenerals, Log, TEXT("Campaign.ini: %d campaigns"), Campaigns.Num());
}
