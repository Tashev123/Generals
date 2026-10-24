#include "Logic/GenCommandData.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Generals1.h"

const FGenRadiusDecalTemplate* FGenCommandData::FindRadiusCursor(const FString& Type) const
{
	const FGenRadiusDecalTemplate* T = Type.IsEmpty() ? nullptr : RadiusCursors.Find(Type.ToUpper());
	return T && T->IsValid() ? T : nullptr;
}

FGenCommandData& FGenCommandData::Get()
{
	static FGenCommandData Instance;
	return Instance;
}

bool FGenCommandData::Initialize()
{
	if (bInitialized)
	{
		return true;
	}
	bInitialized = true;
	const double Start = FPlatformTime::Seconds();
	FGenFileSystem& FS = FGenFileSystem::Get();

	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\CommandButton.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\CommandButton.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("CommandButton"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenCommandButton& Btn = Buttons.FindOrAdd(B->Name().ToLower());
		Btn.Name = B->Name();
		Btn.CommandName = B->GetString(TEXT("Command"));
		Btn.Object = B->GetString(TEXT("Object"));
		Btn.TextLabel = B->GetString(TEXT("TextLabel"));
		Btn.DescriptLabel = B->GetString(TEXT("DescriptLabel"));
		Btn.ConflictingLabel = B->GetString(TEXT("ConflictingLabel"));
		Btn.ButtonImage = B->GetString(TEXT("ButtonImage"));
		Btn.Upgrade = B->GetString(TEXT("Upgrade"));
		{
			const FString Border = B->GetString(TEXT("ButtonBorderType"));
			Btn.BorderType = Border.Equals(TEXT("BUILD"), ESearchCase::IgnoreCase) ? 1 : Border.Equals(TEXT("UPGRADE"), ESearchCase::IgnoreCase) ? 2
				: Border.Equals(TEXT("ACTION"), ESearchCase::IgnoreCase) ? 3 : Border.Equals(TEXT("SYSTEM"), ESearchCase::IgnoreCase) ? 4 : 0;
		}
		Btn.SpecialPower = B->GetString(TEXT("SpecialPower"));
		Btn.CursorName = B->GetString(TEXT("CursorName"));
		Btn.InvalidCursorName = B->GetString(TEXT("InvalidCursorName"));
		Btn.Sciences.Reset();
		if (const FGenIniEntry* S = B->Find(TEXT("Science")))
		{
			Btn.Sciences = S->Values;
		}
		Btn.bNeedTargetPos = Btn.bNeedTargetObject = Btn.bNeedSpecialPowerScience = Btn.bNeedUpgrade = false;
		Btn.CommandOption = 0;
		Btn.bScriptOnly = false;
		if (const FGenIniEntry* O = B->Find(TEXT("Options")))
		{
			for (const FString& V : O->Values)
			{
				if (V.Equals(TEXT("NEED_TARGET_POS"), ESearchCase::IgnoreCase)) { Btn.bNeedTargetPos = true; }
				else if (V.Equals(TEXT("USES_MINE_CLEARING_WEAPONSET"), ESearchCase::IgnoreCase)) { Btn.bUsesMineClearingWeaponSet = true; }
				else if (V.StartsWith(TEXT("NEED_TARGET_"), ESearchCase::IgnoreCase)) { Btn.bNeedTargetObject = true; }
				else if (V.Equals(TEXT("NEED_SPECIAL_POWER_SCIENCE"), ESearchCase::IgnoreCase)) { Btn.bNeedSpecialPowerScience = true; }
				else if (V.Equals(TEXT("NEED_UPGRADE"), ESearchCase::IgnoreCase)) { Btn.bNeedUpgrade = true; }
				else if (V.Equals(TEXT("CONTEXTMODE_COMMAND"), ESearchCase::IgnoreCase)) { Btn.bContextMode = true; }
				else if (V.Equals(TEXT("SCRIPT_ONLY"), ESearchCase::IgnoreCase)) { Btn.bScriptOnly = true; }
				else if (V.Equals(TEXT("OPTION_ONE"), ESearchCase::IgnoreCase)) { Btn.CommandOption = 1; }
				else if (V.Equals(TEXT("OPTION_TWO"), ESearchCase::IgnoreCase)) { Btn.CommandOption = 2; }
				else if (V.Equals(TEXT("OPTION_THREE"), ESearchCase::IgnoreCase)) { Btn.CommandOption = 3; }
			}
		}
		{
			const FString Slot = B->GetString(TEXT("WeaponSlot"));
			Btn.WeaponSlot = Slot.Equals(TEXT("PRIMARY"), ESearchCase::IgnoreCase) ? 0 : Slot.Equals(TEXT("SECONDARY"), ESearchCase::IgnoreCase) ? 1
				: Slot.Equals(TEXT("TERTIARY"), ESearchCase::IgnoreCase) ? 2 : INDEX_NONE;
			Btn.MaxShotsToFire = B->GetInt(TEXT("MaxShotsToFire"), MAX_int32);
			Btn.RadiusCursorType = B->GetString(TEXT("RadiusCursorType")).ToUpper();
		}
		const FString& Cmd = Btn.CommandName;
		Btn.bFromShortcut = Cmd.EndsWith(TEXT("_FROM_SHORTCUT"), ESearchCase::IgnoreCase);
		Btn.Command = Cmd.Equals(TEXT("UNIT_BUILD"), ESearchCase::IgnoreCase) ? EGenCommand::UnitBuild
			: Cmd.Equals(TEXT("DOZER_CONSTRUCT"), ESearchCase::IgnoreCase) ? EGenCommand::DozerConstruct
			: Cmd.Equals(TEXT("SELL"), ESearchCase::IgnoreCase) ? EGenCommand::Sell
			: Cmd.Equals(TEXT("STOP"), ESearchCase::IgnoreCase) ? EGenCommand::Stop
			: Cmd.Equals(TEXT("PLAYER_UPGRADE"), ESearchCase::IgnoreCase) ? EGenCommand::PlayerUpgrade
			: Cmd.Equals(TEXT("OBJECT_UPGRADE"), ESearchCase::IgnoreCase) ? EGenCommand::ObjectUpgrade
			: Cmd.Equals(TEXT("PURCHASE_SCIENCE"), ESearchCase::IgnoreCase) ? EGenCommand::PurchaseScience
			: Cmd.Equals(TEXT("EXIT_CONTAINER"), ESearchCase::IgnoreCase) ? EGenCommand::ExitContainer
			: Cmd.Equals(TEXT("EVACUATE"), ESearchCase::IgnoreCase) ? EGenCommand::Evacuate
			: Cmd.Equals(TEXT("ATTACK_MOVE"), ESearchCase::IgnoreCase) ? EGenCommand::AttackMove
			: Cmd.Equals(TEXT("GUARD"), ESearchCase::IgnoreCase) ? EGenCommand::Guard
			: Cmd.Equals(TEXT("GUARD_WITHOUT_PURSUIT"), ESearchCase::IgnoreCase) ? EGenCommand::GuardWithoutPursuit
			: Cmd.Equals(TEXT("GUARD_FLYING_UNITS_ONLY"), ESearchCase::IgnoreCase) ? EGenCommand::GuardFlyingOnly
			: Cmd.Equals(TEXT("HACK_INTERNET"), ESearchCase::IgnoreCase) ? EGenCommand::HackInternet
			: Cmd.Equals(TEXT("FIRE_WEAPON"), ESearchCase::IgnoreCase) ? EGenCommand::FireWeapon
			: Cmd.Equals(TEXT("COMBATDROP"), ESearchCase::IgnoreCase) ? EGenCommand::CombatDrop
			: Cmd.Equals(TEXT("SWITCH_WEAPON"), ESearchCase::IgnoreCase) ? EGenCommand::SwitchWeapon
			: Cmd.Equals(TEXT("SET_RALLY_POINT"), ESearchCase::IgnoreCase) ? EGenCommand::SetRallyPoint
			: Cmd.Equals(TEXT("DOZER_CONSTRUCT_CANCEL"), ESearchCase::IgnoreCase) ? EGenCommand::DozerConstructCancel
			: Cmd.Equals(TEXT("TOGGLE_OVERCHARGE"), ESearchCase::IgnoreCase) ? EGenCommand::ToggleOvercharge
			: Cmd.Equals(TEXT("HIJACK_VEHICLE"), ESearchCase::IgnoreCase) ? EGenCommand::HijackVehicle
			: Cmd.Equals(TEXT("CONVERT_TO_CARBOMB"), ESearchCase::IgnoreCase) ? EGenCommand::ConvertToCarBomb
			: Cmd.Equals(TEXT("SABOTAGE_BUILDING"), ESearchCase::IgnoreCase) ? EGenCommand::SabotageBuilding
			: (Cmd.Equals(TEXT("SPECIAL_POWER"), ESearchCase::IgnoreCase) || Cmd.Equals(TEXT("SPECIAL_POWER_FROM_SHORTCUT"), ESearchCase::IgnoreCase)
				|| Cmd.Equals(TEXT("SPECIAL_POWER_CONSTRUCT"), ESearchCase::IgnoreCase) || Cmd.Equals(TEXT("SPECIAL_POWER_CONSTRUCT_FROM_SHORTCUT"), ESearchCase::IgnoreCase))
				? EGenCommand::SpecialPower : EGenCommand::Other;
		Btn.bConstruct = Cmd.StartsWith(TEXT("SPECIAL_POWER_CONSTRUCT"), ESearchCase::IgnoreCase);
	}

	Blocks.Reset();
	GenIni::ParseFile(TEXT("Data\\INI\\Default\\CommandSet.ini"), Blocks);
	GenIni::ParseFile(TEXT("Data\\INI\\CommandSet.ini"), Blocks);
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		if (!B->Keyword.Equals(TEXT("CommandSet"), ESearchCase::IgnoreCase))
		{
			continue;
		}
		FGenCommandSet& Set = Sets.FindOrAdd(B->Name().ToLower());
		Set = FGenCommandSet();
		Set.Name = B->Name();
		for (const FGenIniEntry& E : B->Entries)
		{
			const int32 Slot = FCString::Atoi(*E.Key);
			if (Slot >= 1 && Slot <= FGenCommandSet::NumSlots)
			{
				Set.Slots[Slot - 1] = E.Value();
			}
		}
	}

	// InGameUI.ini: the radius cursors (InGameUI::m_radiusCursors, by RadiusCursorType).
	{
		static const TCHAR* const Fields[][2] = {
			{ TEXT("ATTACK_DAMAGE_AREA"), TEXT("AttackDamageAreaRadiusCursor") }, { TEXT("ATTACK_SCATTER_AREA"), TEXT("AttackScatterAreaRadiusCursor") },
			{ TEXT("ATTACK_CONTINUE_AREA"), TEXT("AttackContinueAreaRadiusCursor") }, { TEXT("FRIENDLY_SPECIALPOWER"), TEXT("FriendlySpecialPowerRadiusCursor") },
			{ TEXT("OFFENSIVE_SPECIALPOWER"), TEXT("OffensiveSpecialPowerRadiusCursor") }, { TEXT("SUPERWEAPON_SCATTER_AREA"), TEXT("SuperweaponScatterAreaRadiusCursor") },
			{ TEXT("GUARD_AREA"), TEXT("GuardAreaRadiusCursor") }, { TEXT("EMERGENCY_REPAIR"), TEXT("EmergencyRepairRadiusCursor") },
			{ TEXT("PARTICLECANNON"), TEXT("ParticleCannonRadiusCursor") }, { TEXT("A10STRIKE"), TEXT("A10StrikeRadiusCursor") },
			{ TEXT("CARPETBOMB"), TEXT("CarpetBombRadiusCursor") }, { TEXT("DAISYCUTTER"), TEXT("DaisyCutterRadiusCursor") },
			{ TEXT("PARADROP"), TEXT("ParadropRadiusCursor") }, { TEXT("SPYSATELLITE"), TEXT("SpySatelliteRadiusCursor") },
			{ TEXT("SPECTREGUNSHIP"), TEXT("SpectreGunshipRadiusCursor") }, { TEXT("HELIX_NAPALM_BOMB"), TEXT("HelixNapalmBombRadiusCursor") },
			{ TEXT("NUCLEARMISSILE"), TEXT("NuclearMissileRadiusCursor") }, { TEXT("EMPPULSE"), TEXT("EMPPulseRadiusCursor") },
			{ TEXT("ARTILLERYBARRAGE"), TEXT("ArtilleryRadiusCursor") }, { TEXT("FRENZY"), TEXT("FrenzyRadiusCursor") },
			{ TEXT("NAPALMSTRIKE"), TEXT("NapalmStrikeRadiusCursor") }, { TEXT("CLUSTERMINES"), TEXT("ClusterMinesRadiusCursor") },
			{ TEXT("SCUDSTORM"), TEXT("ScudStormRadiusCursor") }, { TEXT("ANTHRAXBOMB"), TEXT("AnthraxBombRadiusCursor") },
			{ TEXT("AMBUSH"), TEXT("AmbushRadiusCursor") }, { TEXT("RADAR"), TEXT("RadarRadiusCursor") },
			{ TEXT("SPYDRONE"), TEXT("SpyDroneRadiusCursor") }, { TEXT("CLEARMINES"), TEXT("ClearMinesRadiusCursor") },
			{ TEXT("AMBULANCE"), TEXT("AmbulanceRadiusCursor") },
		};
		Blocks.Reset();
		GenIni::ParseFile(TEXT("Data\\INI\\Default\\InGameUI.ini"), Blocks);
		GenIni::ParseFile(TEXT("Data\\INI\\InGameUI.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("InGameUI"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			for (const auto& F : Fields)
			{
				const FGenIniEntry* E = B->Find(F[1]);
				if (E && E->Block.IsValid())
				{
					RadiusCursors.Add(F[0], FGenRadiusDecalTemplate::Parse(E->Block.Get()));
				}
			}
		}
	}

	// Mapped images: every INI below Data\INI\MappedImages - (Image::load) "TextureSize_512" first, then
	// "HandCreated" over it (INI_LOAD_OVERWRITE: the control bar's frame is defined in both).
	TArray<FString> ImageFiles = FS.FindFiles(TEXT("Data\\INI\\MappedImages\\"), TEXT(".ini"));
	ImageFiles.StableSort([](const FString& A, const FString& B)
	{
		const bool bHandA = A.Contains(TEXT("\\HandCreated\\")), bHandB = B.Contains(TEXT("\\HandCreated\\"));
		return bHandA != bHandB ? !bHandA : A < B;
	});
	for (const FString& Path : ImageFiles)
	{
		Blocks.Reset();
		GenIni::ParseFile(Path, Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("MappedImage"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FGenMappedImage Img;
			Img.Texture = B->GetString(TEXT("Texture"));
			const float TW = FMath::Max(1.f, B->GetFloat(TEXT("TextureWidth"), 512.f));
			const float TH = FMath::Max(1.f, B->GetFloat(TEXT("TextureHeight"), 512.f));
			float L = 0.f, T = 0.f, R = TW, Bt = TH;
			if (const FGenIniEntry* C = B->Find(TEXT("Coords")))
			{
				// "Left:187 Top:51 Right:247 Bottom:99"
				TArray<FString> Tok;
				GenIni::Tokenize(C->Raw, Tok, TEXT(" \t=:"));
				for (int32 i = 0; i + 1 < Tok.Num(); i += 2)
				{
					const float V = FCString::Atof(*Tok[i + 1]);
					if (Tok[i].Equals(TEXT("Left"), ESearchCase::IgnoreCase)) { L = V; }
					else if (Tok[i].Equals(TEXT("Top"), ESearchCase::IgnoreCase)) { T = V; }
					else if (Tok[i].Equals(TEXT("Right"), ESearchCase::IgnoreCase)) { R = V; }
					else if (Tok[i].Equals(TEXT("Bottom"), ESearchCase::IgnoreCase)) { Bt = V; }
				}
			}
			Img.UV0 = FVector2f(L / TW, T / TH);
			Img.UV1 = FVector2f(R / TW, Bt / TH);
			Img.Size = FVector2f(R - L, Bt - T);
			// A few images are defined twice (the hand-made full-screen pictures); the definition whose
			// texture the game doesn't actually have must not replace the one that works.
			const FString Key = B->Name().ToLower();
			if (Images.Contains(Key))
			{
				const FString Base = FPaths::GetBaseFilename(Img.Texture);
				if (!FS.Exists(TEXT("Art\\Textures\\") + Base + TEXT(".tga")) && !FS.Exists(TEXT("Art\\Textures\\") + Base + TEXT(".dds"))
					&& !FS.Exists(TEXT("Data\\English\\Art\\Textures\\") + Base + TEXT(".tga")) && !FS.Exists(TEXT("Data\\English\\Art\\Textures\\") + Base + TEXT(".dds")))
				{
					continue;
				}
			}
			Images.Add(Key, MoveTemp(Img));
		}
	}

	LoadStrings();
	UE_LOG(LogGenerals, Log, TEXT("Command data: %d buttons, %d command sets, %d mapped images, %d strings (%.2fs)"),
		Buttons.Num(), Sets.Num(), Images.Num(), Strings.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

void FGenCommandData::LoadStrings()
{
	// generals.csf: " FSC" header, then per label " LBL" + its strings (" RTS" / "WRTS"), whose UTF-16
	// characters are stored bit-inverted.
	FGenFileSystem& FS = FGenFileSystem::Get();
	TArray<uint8> Data;
	bool bRead = FS.ReadFile(TEXT("Data\\English\\generals.csf"), Data);
	if (!bRead)
	{
		for (const FString& Path : FS.FindFiles(TEXT("Data\\"), TEXT(".csf")))
		{
			if (Path.EndsWith(TEXT("generals.csf"), ESearchCase::IgnoreCase) && FS.ReadFile(Path, Data))
			{
				bRead = true;
				break;
			}
		}
	}
	if (!bRead || Data.Num() < 24)
	{
		UE_LOG(LogGenerals, Warning, TEXT("generals.csf not found: button names will show their labels"));
		return;
	}
	auto U32 = [&Data](int32 Pos) -> uint32
	{
		return Pos + 4 <= Data.Num() ? (uint32(Data[Pos]) | (uint32(Data[Pos + 1]) << 8) | (uint32(Data[Pos + 2]) << 16) | (uint32(Data[Pos + 3]) << 24)) : 0u;
	};
	const uint32 TagLabel = 0x4C424C20; // "LBL " read as little-endian " LBL"
	const uint32 TagString = 0x53545220, TagStringW = 0x53545257;
	const int32 NumLabels = int32(U32(8));
	int32 Pos = 24;
	for (int32 n = 0; n < NumLabels && Pos + 12 <= Data.Num(); ++n)
	{
		if (U32(Pos) != TagLabel)
		{
			break;
		}
		const int32 NumStrings = int32(U32(Pos + 4));
		const int32 LabelLen = int32(U32(Pos + 8));
		Pos += 12;
		if (LabelLen < 0 || Pos + LabelLen > Data.Num())
		{
			break;
		}
		const FString Label = FString::ConstructFromPtrSize(reinterpret_cast<const ANSICHAR*>(Data.GetData() + Pos), LabelLen);
		Pos += LabelLen;
		for (int32 s = 0; s < NumStrings && Pos + 8 <= Data.Num(); ++s)
		{
			const uint32 Tag = U32(Pos);
			const int32 Len = int32(U32(Pos + 4));
			Pos += 8;
			if (Len < 0 || Pos + Len * 2 > Data.Num())
			{
				return;
			}
			if (s == 0)
			{
				FString Text;
				Text.Reserve(Len);
				for (int32 i = 0; i < Len; ++i)
				{
					const uint16 Ch = uint16(~(uint16(Data[Pos + i * 2]) | (uint16(Data[Pos + i * 2 + 1]) << 8)));
					if (Ch != TEXT('&')) // hotkey marker
					{
						Text.AppendChar(TCHAR(Ch));
					}
				}
				Strings.Add(Label.ToLower(), MoveTemp(Text));
			}
			Pos += Len * 2;
			if (Tag == TagStringW && Pos + 4 <= Data.Num())
			{
				Pos += 4 + int32(U32(Pos)); // name of the speech file
			}
			else if (Tag != TagString && Tag != TagStringW)
			{
				return;
			}
		}
	}
}

const FGenCommandButton* FGenCommandData::FindButton(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Buttons.Find(Name.ToLower());
}

const FGenCommandSet* FGenCommandData::FindSet(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Sets.Find(Name.ToLower());
}

const FGenMappedImage* FGenCommandData::FindImage(const FString& Name) const
{
	return Name.IsEmpty() ? nullptr : Images.Find(Name.ToLower());
}

FString FGenCommandData::GetText(const FString& Label) const
{
	const FString* Found = Label.IsEmpty() ? nullptr : Strings.Find(Label.ToLower());
	return Found ? *Found : Label;
}
