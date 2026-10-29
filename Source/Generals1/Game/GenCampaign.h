#pragma once

#include "CoreMinimal.h"

/** A mission of a campaign (CampaignManager.cpp's Mission). */
struct GENERALS1_API FGenCampaignMission
{
	FString Name;               // "Mission01"
	FString Map;                // the map's folder: "MD_USA01" (Campaign.ini: Maps\MD_USA01\MD_USA01.map)
	FString NextMission;
	FString IntroMovie;
	FString GeneralName;        // label (Generals Challenge)
	TArray<FString> ObjectiveLines;
	FString LocationName;       // label
};

/** A campaign (Campaign.ini). */
struct GENERALS1_API FGenCampaign
{
	FString Name;               // "USA"
	FString NameLabel;          // "CAMPAIGN:USA"
	FString FirstMission;
	FString FinalVictoryMovie;
	FString PlayerFaction;      // (Generals Challenge)
	bool bChallenge = false;
	TArray<FGenCampaignMission> Missions;

	const FGenCampaignMission* FindMission(const FString& MissionName) const;
};

/** Data\INI\Campaign.ini, read once. */
class GENERALS1_API FGenCampaignData
{
public:
	static FGenCampaignData& Get();

	const TArray<FGenCampaign>& GetCampaigns();
	const FGenCampaign* FindCampaign(const FString& Name);
	/** The missions of a campaign that this game can play so far (the rest are listed, but locked). */
	static bool IsMissionPorted(const FString& Campaign, const FString& Mission);

private:
	void Load();

	bool bLoaded = false;
	TArray<FGenCampaign> Campaigns;
};
