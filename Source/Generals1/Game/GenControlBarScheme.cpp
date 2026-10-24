#include "Game/GenControlBarScheme.h"
#include "Core/GenIni.h"

namespace
{
	/** "X:0 Y:408" */
	FVector2D ReadCoord(const FGenIniEntry* E)
	{
		FVector2D P = FVector2D::ZeroVector;
		if (!E)
		{
			return P;
		}
		TArray<FString> T;
		GenIni::Tokenize(E->Raw, T, TEXT(" \t=:"));
		for (int32 i = 0; i + 1 < T.Num(); ++i)
		{
			if (T[i].Equals(TEXT("X"), ESearchCase::IgnoreCase)) { P.X = FCString::Atof(*T[i + 1]); }
			else if (T[i].Equals(TEXT("Y"), ESearchCase::IgnoreCase)) { P.Y = FCString::Atof(*T[i + 1]); }
		}
		return P;
	}

	/** "R:0 G:21 B:126 A:255" */
	FColor ReadColor(const FGenIniEntry* E, const FColor& Default)
	{
		if (!E)
		{
			return Default;
		}
		FColor C(0, 0, 0, 255);
		TArray<FString> T;
		GenIni::Tokenize(E->Raw, T, TEXT(" \t=:"));
		for (int32 i = 0; i + 1 < T.Num(); ++i)
		{
			const uint8 V = uint8(FMath::Clamp(FCString::Atoi(*T[i + 1]), 0, 255));
			if (T[i].Equals(TEXT("R"), ESearchCase::IgnoreCase)) { C.R = V; }
			else if (T[i].Equals(TEXT("G"), ESearchCase::IgnoreCase)) { C.G = V; }
			else if (T[i].Equals(TEXT("B"), ESearchCase::IgnoreCase)) { C.B = V; }
			else if (T[i].Equals(TEXT("A"), ESearchCase::IgnoreCase)) { C.A = V; }
		}
		return C;
	}

	FBox2D ReadRect(const FGenIniBlock& B, const TCHAR* Prefix)
	{
		const FGenIniEntry* UL = B.Find(*(FString(Prefix) + TEXT("UL")));
		const FGenIniEntry* LR = B.Find(*(FString(Prefix) + TEXT("LR")));
		return (UL && LR) ? FBox2D(ReadCoord(UL), ReadCoord(LR)) : FBox2D(ForceInit);
	}

	FGenControlBarButtonImages ReadButton(const FGenIniBlock& B, const TCHAR* Prefix)
	{
		// (The data spells it "Hightlited".)
		FGenControlBarButtonImages I;
		I.Enable = B.GetString(*(FString(Prefix) + TEXT("ButtonEnable")));
		I.Highlighted = B.GetString(*(FString(Prefix) + TEXT("ButtonHightlited")));
		I.Pushed = B.GetString(*(FString(Prefix) + TEXT("ButtonPushed")));
		I.Disabled = B.GetString(*(FString(Prefix) + TEXT("ButtonDisabled")));
		return I;
	}

	TArray<FGenControlBarScheme>& Schemes()
	{
		static TArray<FGenControlBarScheme> List;
		static bool bLoaded = false;
		if (bLoaded)
		{
			return List;
		}
		bLoaded = true;
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\ControlBarScheme.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\ControlBarScheme.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& Block : Blocks)
		{
			if (!Block->Keyword.Equals(TEXT("ControlBarScheme"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			const FGenIniBlock& B = *Block;
			FGenControlBarScheme* S = List.FindByPredicate([&B](const FGenControlBarScheme& X) { return X.Name.Equals(B.Name(), ESearchCase::IgnoreCase); });
			if (!S)
			{
				S = &List.AddDefaulted_GetRef();
			}
			*S = FGenControlBarScheme();
			S->Name = B.Name();
			S->Side = B.GetString(TEXT("Side"));
			const FVector2D Res = ReadCoord(B.Find(TEXT("ScreenCreationRes")));
			S->CreationRes = FIntPoint(Res.X > 0.0 ? int32(Res.X) : 800, Res.Y > 0.0 ? int32(Res.Y) : 600);
			S->QueueButtonImage = B.GetString(TEXT("QueueButtonImage"));
			S->RightHUDImage = B.GetString(TEXT("RightHUDImage"));
			S->ExpBarForegroundImage = B.GetString(TEXT("ExpBarForegroundImage"));
			S->GenArrow = B.GetString(TEXT("GenArrow"));
			S->CommandMarkerImage = B.GetString(TEXT("CommandMarkerImage"));
			S->PowerPurchaseImage = B.GetString(TEXT("PowerPurchaseImage"));
			S->CommandBarBorderColor = ReadColor(B.Find(TEXT("CommandBarBorderColor")), FColor::Black);
			S->BuildUpClockColor = ReadColor(B.Find(TEXT("BuildUpClockColor")), FColor(0, 0, 0, 160));
			S->BorderBuild = ReadColor(B.Find(TEXT("ButtonBorderBuildColor")), FColor::White);
			S->BorderAction = ReadColor(B.Find(TEXT("ButtonBorderActionColor")), FColor::White);
			S->BorderUpgrade = ReadColor(B.Find(TEXT("ButtonBorderUpgradeColor")), FColor::White);
			S->BorderSystem = ReadColor(B.Find(TEXT("ButtonBorderSystemColor")), FColor::White);
			S->GenBarButtonIn = B.GetString(TEXT("GenBarButtonIn"));
			S->GenBarButtonOn = B.GetString(TEXT("GenBarButtonOn"));
			S->ToggleUp.Enable = B.GetString(TEXT("ToggleButtonUpIn"));
			S->ToggleUp.Highlighted = B.GetString(TEXT("ToggleButtonUpOn"));
			S->ToggleUp.Pushed = B.GetString(TEXT("ToggleButtonUpPushed"));
			S->ToggleDown.Enable = B.GetString(TEXT("ToggleButtonDownIn"));
			S->ToggleDown.Highlighted = B.GetString(TEXT("ToggleButtonDownOn"));
			S->ToggleDown.Pushed = B.GetString(TEXT("ToggleButtonDownPushed"));
			S->Options = ReadButton(B, TEXT("Options"));
			S->IdleWorker = ReadButton(B, TEXT("IdleWorker"));
			S->Buddy = ReadButton(B, TEXT("Buddy"));
			S->Beacon = ReadButton(B, TEXT("Beacon"));
			S->General = ReadButton(B, TEXT("General"));
			S->UAttack = ReadButton(B, TEXT("UAttack"));
			S->MinMax = ReadButton(B, TEXT("MinMax"));
			S->MinMaxRect = ReadRect(B, TEXT("MinMax"));
			S->GeneralRect = ReadRect(B, TEXT("General"));
			S->UAttackRect = ReadRect(B, TEXT("UAttack"));
			S->OptionsRect = ReadRect(B, TEXT("Options"));
			S->WorkerRect = ReadRect(B, TEXT("Worker"));
			S->ChatRect = ReadRect(B, TEXT("Chat"));
			S->BeaconRect = ReadRect(B, TEXT("Beacon"));
			S->PowerBarRect = ReadRect(B, TEXT("PowerBar"));
			S->MoneyRect = ReadRect(B, TEXT("Money"));
			for (const FGenIniEntry* E : B.FindAll(TEXT("ImagePart")))
			{
				if (!E->Block.IsValid())
				{
					continue;
				}
				FGenControlBarImagePart& P = S->ImageParts.AddDefaulted_GetRef();
				P.Position = ReadCoord(E->Block->Find(TEXT("Position")));
				P.Size = ReadCoord(E->Block->Find(TEXT("Size")));
				P.Image = E->Block->GetString(TEXT("ImageName"));
				P.Layer = FMath::Clamp(E->Block->GetInt(TEXT("Layer"), 0), 0, 5);
			}
		}
		return List;
	}
}

const FGenControlBarScheme* GenControlBarSchemes::FindBySide(const FString& Side)
{
	// "if((!tempScheme || tempScheme->m_ScreenCreationRes.x < CBScheme->m_ScreenCreationRes.x))": of that
	// side's, the one made for the widest screen.
	const FGenControlBarScheme* Best = nullptr;
	const FGenControlBarScheme* Default = nullptr;
	const FString Want = Side.IsEmpty() ? FString(TEXT("Observer")) : Side;
	for (const FGenControlBarScheme& S : Schemes())
	{
		if (S.Side.Equals(Want, ESearchCase::IgnoreCase) && (!Best || Best->CreationRes.X < S.CreationRes.X))
		{
			Best = &S;
		}
		if (S.Name.Equals(TEXT("Default"), ESearchCase::IgnoreCase))
		{
			Default = &S;
		}
	}
	return Best ? Best : Default;
}
