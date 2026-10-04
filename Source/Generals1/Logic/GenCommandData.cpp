#include "Logic/GenCommandData.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Generals1.h"

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
		Btn.ButtonImage = B->GetString(TEXT("ButtonImage"));
		Btn.Upgrade = B->GetString(TEXT("Upgrade"));
		Btn.SpecialPower = B->GetString(TEXT("SpecialPower"));
		Btn.Sciences.Reset();
		if (const FGenIniEntry* S = B->Find(TEXT("Science")))
		{
			Btn.Sciences = S->Values;
		}
		Btn.bNeedTargetPos = Btn.bNeedTargetObject = Btn.bNeedSpecialPowerScience = Btn.bNeedUpgrade = false;
		if (const FGenIniEntry* O = B->Find(TEXT("Options")))
		{
			for (const FString& V : O->Values)
			{
				if (V.Equals(TEXT("NEED_TARGET_POS"), ESearchCase::IgnoreCase)) { Btn.bNeedTargetPos = true; }
				else if (V.StartsWith(TEXT("NEED_TARGET_"), ESearchCase::IgnoreCase)) { Btn.bNeedTargetObject = true; }
				else if (V.Equals(TEXT("NEED_SPECIAL_POWER_SCIENCE"), ESearchCase::IgnoreCase)) { Btn.bNeedSpecialPowerScience = true; }
				else if (V.Equals(TEXT("NEED_UPGRADE"), ESearchCase::IgnoreCase)) { Btn.bNeedUpgrade = true; }
			}
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
			: (Cmd.Equals(TEXT("SPECIAL_POWER"), ESearchCase::IgnoreCase) || Cmd.Equals(TEXT("SPECIAL_POWER_FROM_SHORTCUT"), ESearchCase::IgnoreCase))
				? EGenCommand::SpecialPower : EGenCommand::Other;
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

	// Mapped images: every INI below Data\INI\MappedImages.
	for (const FString& Path : FS.FindFiles(TEXT("Data\\INI\\MappedImages\\"), TEXT(".ini")))
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
			Images.Add(B->Name().ToLower(), MoveTemp(Img));
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
