#include "Audio/GenAudioData.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Generals1.h"

namespace
{
	/** INI::parsePercentToReal */
	float Percent(const FGenIniEntry& E)
	{
		return E.AsFloat() / 100.f;
	}

	uint32 ParseBits(const FGenIniEntry& E, const TCHAR* const* Names, int32 NumNames)
	{
		uint32 Bits = 0;
		for (const FString& V : E.Values)
		{
			for (int32 i = 0; i < NumNames; ++i)
			{
				if (V.Equals(Names[i], ESearchCase::IgnoreCase)) { Bits |= 1u << i; }
			}
		}
		return Bits;
	}

	/** AudioEventInfo::m_audioEventInfo field table. */
	void ParseEvent(const FGenIniBlock& B, FGenAudioEventInfo& Out)
	{
		static const TCHAR* TypeNames[] = { TEXT("UI"), TEXT("WORLD"), TEXT("SHROUDED"), TEXT("GLOBAL"), TEXT("VOICE"), TEXT("PLAYER"), TEXT("ALLIES"), TEXT("ENEMIES"), TEXT("EVERYONE") };
		static const TCHAR* ControlNames[] = { TEXT("LOOP"), TEXT("RANDOM"), TEXT("ALL"), TEXT("POSTDELAY"), TEXT("INTERRUPT") };
		static const TCHAR* PriorityNames[] = { TEXT("LOWEST"), TEXT("LOW"), TEXT("NORMAL"), TEXT("HIGH"), TEXT("CRITICAL") };
		for (const FGenIniEntry& E : B.Entries)
		{
			auto Is = [&E](const TCHAR* Key) { return E.Key.Equals(Key, ESearchCase::IgnoreCase); };
			if (Is(TEXT("Filename"))) { Out.Filename = E.Value(); }
			else if (Is(TEXT("Volume"))) { Out.Volume = Percent(E); }
			else if (Is(TEXT("VolumeShift"))) { Out.VolumeShift = Percent(E); }
			else if (Is(TEXT("PitchShift")))
			{
				// parsePitchShift: "-10 10" -> 0.9 .. 1.1
				Out.PitchShiftMin = 1.f + E.AsFloat(0) / 100.f;
				Out.PitchShiftMax = FMath::Max(Out.PitchShiftMin, 1.f + E.AsFloat(1, E.AsFloat(0)) / 100.f);
			}
			else if (Is(TEXT("Delay")))
			{
				Out.DelayMin = E.AsFloat(0);
				Out.DelayMax = FMath::Max(Out.DelayMin, E.AsFloat(1, Out.DelayMin));
			}
			else if (Is(TEXT("Limit"))) { Out.Limit = E.AsInt(); }
			else if (Is(TEXT("LoopCount"))) { Out.LoopCount = E.AsInt(); }
			else if (Is(TEXT("Priority")))
			{
				for (int32 i = 0; i < UE_ARRAY_COUNT(PriorityNames); ++i)
				{
					if (E.Value().Equals(PriorityNames[i], ESearchCase::IgnoreCase)) { Out.Priority = EGenAudioPriority(i); }
				}
			}
			else if (Is(TEXT("Type"))) { Out.Type = ParseBits(E, TypeNames, UE_ARRAY_COUNT(TypeNames)); }
			else if (Is(TEXT("Control"))) { Out.Control = ParseBits(E, ControlNames, UE_ARRAY_COUNT(ControlNames)); }
			else if (Is(TEXT("Sounds"))) { Out.Sounds = E.Values; }
			else if (Is(TEXT("Attack"))) { Out.Attack = E.Values; }
			else if (Is(TEXT("Decay"))) { Out.Decay = E.Values; }
			else if (Is(TEXT("MinRange"))) { Out.MinRange = E.AsFloat(); }
			else if (Is(TEXT("MaxRange"))) { Out.MaxRange = E.AsFloat(); }
		}
	}
}

FGenAudioData& FGenAudioData::Get()
{
	static FGenAudioData Instance;
	return Instance;
}

bool FGenAudioData::Initialize()
{
	if (bInitialized)
	{
		return true;
	}
	bInitialized = true;
	const double Start = FPlatformTime::Seconds();

	// AudioSettings.ini
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\AudioSettings.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("AudioSettings"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			auto Pct = [&B](const TCHAR* Key, float& Out) { if (const FGenIniEntry* E = B->Find(Key)) { Out = Percent(*E); } };
			Settings.AudioRoot = B->GetString(TEXT("AudioRoot"), Settings.AudioRoot);
			Settings.SoundsFolder = B->GetString(TEXT("SoundsFolder"), Settings.SoundsFolder);
			Settings.MusicFolder = B->GetString(TEXT("MusicFolder"), Settings.MusicFolder);
			Settings.StreamingFolder = B->GetString(TEXT("StreamingFolder"), Settings.StreamingFolder);
			Settings.SoundsExtension = B->GetString(TEXT("SoundsExtension"), Settings.SoundsExtension);
			Settings.SampleCount2D = B->GetInt(TEXT("SampleCount2D"), Settings.SampleCount2D);
			Settings.SampleCount3D = B->GetInt(TEXT("SampleCount3D"), Settings.SampleCount3D);
			Settings.StreamCount = B->GetInt(TEXT("StreamCount"), Settings.StreamCount);
			Settings.GlobalMinRange = B->GetFloat(TEXT("GlobalMinRange"), Settings.GlobalMinRange);
			Settings.GlobalMaxRange = B->GetFloat(TEXT("GlobalMaxRange"), Settings.GlobalMaxRange);
			Settings.TimeBetweenDrawableSoundsMS = B->GetInt(TEXT("TimeBetweenDrawableSounds"), Settings.TimeBetweenDrawableSoundsMS);
			Settings.MicrophoneDesiredHeightAboveTerrain = B->GetFloat(TEXT("MicrophoneDesiredHeightAboveTerrain"), Settings.MicrophoneDesiredHeightAboveTerrain);
			Settings.ZoomMinDistance = B->GetFloat(TEXT("ZoomMinDistance"), Settings.ZoomMinDistance);
			Settings.ZoomMaxDistance = B->GetFloat(TEXT("ZoomMaxDistance"), Settings.ZoomMaxDistance);
			Pct(TEXT("MinSampleVolume"), Settings.MinSampleVolume);
			Pct(TEXT("Relative2DVolume"), Settings.Relative2DVolume);
			Pct(TEXT("DefaultSoundVolume"), Settings.DefaultSoundVolume);
			Pct(TEXT("Default3DSoundVolume"), Settings.Default3DSoundVolume);
			Pct(TEXT("DefaultSpeechVolume"), Settings.DefaultSpeechVolume);
			Pct(TEXT("DefaultMusicVolume"), Settings.DefaultMusicVolume);
			Pct(TEXT("MicrophoneMaxPercentageBetweenGroundAndCamera"), Settings.MicrophoneMaxPercentageBetweenGroundAndCamera);
			Pct(TEXT("ZoomSoundVolumePercentageAmount"), Settings.ZoomSoundVolumePercentageAmount);
		}
	}

	// Sound effects and voices (AudioEvent), speech (DialogEvent) and music (MusicTrack). Each kind starts
	// from its default entry (INI::parseAudioEventDefinition and its siblings).
	struct FKind
	{
		const TCHAR* File;
		const TCHAR* Keyword;
		const TCHAR* DefaultName;
		EGenAudioType Type;
	};
	static const FKind Kinds[] = {
		{ TEXT("SoundEffects.ini"), TEXT("AudioEvent"), TEXT("DefaultSoundEffect"), EGenAudioType::SoundEffect },
		{ TEXT("Voice.ini"), TEXT("AudioEvent"), TEXT("DefaultSoundEffect"), EGenAudioType::SoundEffect },
		{ TEXT("Speech.ini"), TEXT("DialogEvent"), TEXT("DefaultDialog"), EGenAudioType::Streaming },
		{ TEXT("Music.ini"), TEXT("MusicTrack"), TEXT("DefaultMusicTrack"), EGenAudioType::Music },
	};
	for (const FKind& Kind : Kinds)
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(FString(TEXT("Data\\INI\\Default\\")) + Kind.File, Blocks);
		GenIni::ParseFile(FString(TEXT("Data\\INI\\")) + Kind.File, Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(Kind.Keyword, ESearchCase::IgnoreCase) || B->Name().IsEmpty())
			{
				continue;
			}
			FGenAudioEventInfo Info;
			if (const FGenAudioEventInfo* Default = Events.Find(FString(Kind.DefaultName).ToLower()))
			{
				Info = *Default;
			}
			Info.Name = B->Name();
			Info.SoundType = Kind.Type;
			Info.LastIndex = -1;
			ParseEvent(*B, Info);
			Events.Add(Info.Name.ToLower(), MoveTemp(Info));
		}
	}

	// MiscAudio.ini: the events behind the game's own notifications.
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\MiscAudio.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			for (const FGenIniEntry& E : B->Entries)
			{
				Misc.Add(E.Key.ToLower(), E.Value());
			}
		}
	}

	// Eva.ini
	{
		TArray<TSharedPtr<FGenIniBlock>> Blocks;
		GenIni::ParseFile(TEXT("Data\\INI\\Eva.ini"), Blocks);
		for (const TSharedPtr<FGenIniBlock>& B : Blocks)
		{
			if (!B->Keyword.Equals(TEXT("EvaEvent"), ESearchCase::IgnoreCase))
			{
				continue;
			}
			FGenEvaEvent& Event = Eva.FindOrAdd(B->Name().ToLower());
			Event.Name = B->Name();
			Event.Priority = B->GetInt(TEXT("Priority"), Event.Priority);
			Event.TimeBetweenChecksMS = B->GetInt(TEXT("TimeBetweenChecksMS"), 0);
			Event.ExpirationTimeMS = B->GetInt(TEXT("ExpirationTimeMS"), 0);
			for (const FGenIniEntry& E : B->Entries)
			{
				if (E.Block && E.Key.Equals(TEXT("SideSounds"), ESearchCase::IgnoreCase))
				{
					if (const FGenIniEntry* Sounds = E.Block->Find(TEXT("Sounds")))
					{
						Event.SideSounds.Add(E.Block->GetString(TEXT("Side")).ToLower(), Sounds->Values);
					}
				}
			}
		}
	}

	UE_LOG(LogGenerals, Log, TEXT("Audio data: %d events, %d misc sounds, %d adviser events (%.2fs)"), Events.Num(), Misc.Num(), Eva.Num(),
		FPlatformTime::Seconds() - Start);
	return true;
}

namespace
{
	uint32 ReadLE32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }
	uint16 ReadLE16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }

	/** RIFF WAVE: the "fact" chunk's sample count if there is one (compressed formats), else the data size over the block size. */
	float WavLength(TConstArrayView<uint8> B)
	{
		if (B.Num() < 12 || FMemory::Memcmp(B.GetData(), "RIFF", 4) != 0 || FMemory::Memcmp(B.GetData() + 8, "WAVE", 4) != 0)
		{
			return 0.f;
		}
		uint32 Rate = 0, BytesPerSec = 0, BlockAlign = 0, SamplesPerBlock = 0, FactSamples = 0, DataSize = 0;
		uint16 Format = 0;
		for (int32 At = 12; At + 8 <= B.Num();)
		{
			const uint32 Size = ReadLE32(&B[At + 4]);
			const uint8* Body = &B[At + 8];
			const int32 Avail = B.Num() - (At + 8);
			if (FMemory::Memcmp(&B[At], "fmt ", 4) == 0 && Avail >= 16)
			{
				Format = ReadLE16(Body);
				Rate = ReadLE32(Body + 4);
				BytesPerSec = ReadLE32(Body + 8);
				BlockAlign = ReadLE16(Body + 12);
				if (Size >= 20 && Avail >= 20)
				{
					SamplesPerBlock = ReadLE16(Body + 18); // (IMA ADPCM: after cbSize)
				}
			}
			else if (FMemory::Memcmp(&B[At], "fact", 4) == 0 && Avail >= 4)
			{
				FactSamples = ReadLE32(Body);
			}
			else if (FMemory::Memcmp(&B[At], "data", 4) == 0)
			{
				DataSize = FMath::Min<uint32>(Size, uint32(Avail));
			}
			At += 8 + int32(Size + (Size & 1));
		}
		if (Rate == 0)
		{
			return 0.f;
		}
		if (FactSamples > 0)
		{
			return float(FactSamples) / float(Rate);
		}
		if (Format == 0x11 && BlockAlign > 0 && SamplesPerBlock > 0)
		{
			return float((DataSize / BlockAlign) * SamplesPerBlock) / float(Rate);
		}
		return BytesPerSec > 0 ? float(DataSize) / float(BytesPerSec) : 0.f;
	}

	/** MPEG audio: the frames' headers, one after another (a Xing header's frame count if there is one). */
	float Mp3Length(TConstArrayView<uint8> B)
	{
		static const int32 Bitrates[2][3][16] = {
			{ { 0, 32, 64, 96, 128, 160, 192, 224, 256, 288, 320, 352, 384, 416, 448, 0 },   // MPEG-1 layer I
			  { 0, 32, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 384, 0 },      // layer II
			  { 0, 32, 40, 48, 56, 64, 80, 96, 112, 128, 160, 192, 224, 256, 320, 0 } },     // layer III
			{ { 0, 32, 48, 56, 64, 80, 96, 112, 128, 144, 160, 176, 192, 224, 256, 0 },      // MPEG-2/2.5 layer I
			  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 },
			  { 0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0 } } };
		static const int32 Rates[3][3] = { { 44100, 48000, 32000 }, { 22050, 24000, 16000 }, { 11025, 12000, 8000 } };
		int32 At = 0;
		if (B.Num() >= 10 && FMemory::Memcmp(B.GetData(), "ID3", 3) == 0)
		{
			At = 10 + ((B[6] & 0x7f) << 21 | (B[7] & 0x7f) << 14 | (B[8] & 0x7f) << 7 | (B[9] & 0x7f));
		}
		double Seconds = 0.0;
		while (At + 4 <= B.Num())
		{
			if (B[At] != 0xff || (B[At + 1] & 0xe0) != 0xe0)
			{
				++At;
				continue;
			}
			const int32 Version = (B[At + 1] >> 3) & 3;   // 3: MPEG-1, 2: MPEG-2, 0: MPEG-2.5
			const int32 Layer = (B[At + 1] >> 1) & 3;     // 3: I, 2: II, 1: III
			const int32 BitIndex = B[At + 2] >> 4;
			const int32 RateIndex = (B[At + 2] >> 2) & 3;
			if (Version == 1 || Layer == 0 || BitIndex == 0 || BitIndex == 15 || RateIndex == 3)
			{
				++At;
				continue;
			}
			const int32 V = Version == 3 ? 0 : 1;
			const int32 L = 3 - Layer;
			const int32 Rate = Rates[Version == 3 ? 0 : (Version == 2 ? 1 : 2)][RateIndex];
			const int32 Bitrate = Bitrates[V][L][BitIndex] * 1000;
			const int32 Padding = (B[At + 2] >> 1) & 1;
			const int32 Samples = L == 0 ? 384 : (L == 2 && V == 1 ? 576 : 1152);
			const int32 Size = L == 0 ? (12 * Bitrate / Rate + Padding) * 4 : (Samples / 8) * Bitrate / Rate + Padding;
			if (Size <= 0)
			{
				++At;
				continue;
			}
			Seconds += double(Samples) / double(Rate);
			At += Size;
		}
		return float(Seconds);
	}
}

float FGenAudioData::GetFileLengthSeconds(const FString& VfsPath)
{
	TArray<uint8> Bytes;
	if (!FGenFileSystem::Get().ReadFile(VfsPath, Bytes))
	{
		return 0.f;
	}
	return VfsPath.EndsWith(TEXT(".mp3"), ESearchCase::IgnoreCase) ? Mp3Length(Bytes) : WavLength(Bytes);
}

float FGenAudioData::GetEventLengthSeconds(const FString& Name) const
{
	const FString Key = Name.ToLower();
	{
		FScopeLock Lock(&LengthLock);
		if (const float* Found = LengthCache.Find(Key))
		{
			return *Found;
		}
	}
	float Seconds = 0.f;
	if (const FGenAudioEventInfo* Info = Find(Name))
	{
		const FString File = Info->SoundType == EGenAudioType::SoundEffect ? (Info->Sounds.Num() > 0 ? Info->Sounds[0] : FString()) : Info->Filename;
		if (!File.IsEmpty())
		{
			Seconds = GetFileLengthSeconds(ResolvePath(Info->SoundType, File));
		}
	}
	FScopeLock Lock(&LengthLock);
	LengthCache.Add(Key, Seconds);
	return Seconds;
}

const FGenAudioEventInfo* FGenAudioData::Find(const FString& Name) const
{
	if (Name.IsEmpty() || Name.Equals(TEXT("NoSound"), ESearchCase::IgnoreCase))
	{
		return nullptr;
	}
	return Events.Find(Name.ToLower());
}

const FString& FGenAudioData::GetMisc(const TCHAR* Key) const
{
	static const FString Empty;
	const FString* Found = Misc.Find(FString(Key).ToLower());
	return Found ? *Found : Empty;
}

const FGenEvaEvent* FGenAudioData::FindEva(const FString& Name) const
{
	return Eva.Find(Name.ToLower());
}

FString FGenAudioData::ResolvePath(EGenAudioType Type, const FString& FileOrSound) const
{
	const FString& Folder = Type == EGenAudioType::Music ? Settings.MusicFolder : (Type == EGenAudioType::Streaming ? Settings.StreamingFolder : Settings.SoundsFolder);
	FString File = FileOrSound;
	if (Type == EGenAudioType::SoundEffect)
	{
		File += TEXT(".") + Settings.SoundsExtension;
	}
	// "Try the localized version first so that we're guaranteed to get it even if the generic data
	// directory holds a version of the file."
	const FString Base = Settings.AudioRoot + TEXT("\\") + Folder + TEXT("\\");
	const FString Localized = Base + Settings.Language + TEXT("\\") + File;
	return FGenFileSystem::Get().Exists(Localized) ? Localized : Base + File;
}
