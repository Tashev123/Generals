#include "Audio/GenAudioManager.h"
#include "Core/GenPerf.h"
#include "Audio/GenSoundWave.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "Core/GenIni.h"
#include "Game/GenCameraPawn.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Map/GenMapActor.h"
#include "Generals1.h"
#include "Camera/CameraComponent.h"
#include "AudioMixerBlueprintLibrary.h"
#include "Components/AudioComponent.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Misc/CommandLine.h"
#include "Misc/FileHelper.h"
#include "Misc/Parse.h"
#include "SubmixEffects/AudioMixerSubmixEffectDynamicsProcessor.h"

namespace
{
	/** Decoded sounds kept in memory (the original's AudioFootprintInBytes was 4 MB; times have changed). */
	constexpr int64 SoundCacheLimitBytes = 192ll * 1024 * 1024;
	/** How long a channel stays taken after its sound's last sample went to the mixer (its buffers still have to play). */
	constexpr double VoiceTailSeconds = 0.15;
	constexpr int32 MaxSlots = 48;
	/**
	 * A sound in the middle goes to both speakers at equal power, 3 dB down in each (UGenPcmSoundWave);
	 * this brings it back to its own level there. (Unreal then leaves 3 dB of headroom at its output.)
	 */
	constexpr float OutputGain = 1.41f;

	/** The player's volumes (SetUserVolumes): the options' audio page. */
	struct FUserVolumes
	{
		float Master = 1.f, Music = 1.f, Sound = 1.f, Voice = 1.f, Speech = 1.f;
	};
	FUserVolumes UserVolumes;

	uint64 LoopKey(FGenObjectID Object, int32 Slot)
	{
		return (uint64(Object) << 8) | uint64(Slot & 0xff);
	}
}

UGenAudioManager* UGenAudioManager::Get(const UObject* WorldContext)
{
	const UWorld* World = WorldContext ? WorldContext->GetWorld() : nullptr;
	return World ? World->GetSubsystem<UGenAudioManager>() : nullptr;
}

bool UGenAudioManager::ShouldCreateSubsystem(UObject* Outer) const
{
	const UWorld* World = Cast<UWorld>(Outer);
	return World && (World->WorldType == EWorldType::Game || World->WorldType == EWorldType::PIE);
}

void UGenAudioManager::Deinitialize()
{
	if (Limiter)
	{
		UAudioMixerBlueprintLibrary::RemoveMasterSubmixEffect(this, Limiter);
		Limiter = nullptr;
	}
	Voices.Reset();
	Delayed.Reset();
	Loops.Reset();
	SoundCache.Reset();
	SoundCacheOrder.Reset();
	EvaPending.Reset();
	MapActor = nullptr;
	Logic = nullptr;
	Super::Deinitialize();
}

void UGenAudioManager::Start(AGenMapActor* InMapActor, UGenGameLogic* InLogic)
{
	MapActor = InMapActor;
	Logic = InLogic;
	FGenAudioData::Get().Initialize();
	for (int32 i = Voices.Num() - 1; i >= 0; --i)
	{
		StopVoiceAt(i);
	}
	Delayed.Reset();
	Loops.Reset();
	EvaPending.Reset();
	EvaNextAllowed.Reset();
	EvaHandle = 0;
	Now = 0.0;
	NumPlayed = 0;
	FMemory::Memzero(NumCulled);
	Random.Initialize(int32(FPlatformTime::Cycles64() & 0x7fffffff));
	bTrace = FParse::Param(FCommandLine::Get(), TEXT("GenTrace"));

	if (!Limiter && !FParse::Param(FCommandLine::Get(), TEXT("GenNoLimiter")))
	{
		USubmixEffectDynamicsProcessorPreset* Preset = NewObject<USubmixEffectDynamicsProcessorPreset>(this, NAME_None, RF_Transient);
		FSubmixEffectDynamicsProcessorSettings Settings;
		Settings.DynamicsProcessorType = ESubmixEffectDynamicsProcessorType::Limiter;
		Settings.PeakMode = ESubmixEffectDynamicsPeakMode::Peak;
		Settings.LinkMode = ESubmixEffectDynamicsChannelLinkMode::Peak;
		// Measured on a volley of tank guns: with these the output stays within about 1.5 dB above the
		// threshold (a 1 ms attack lets the first crack of a shot through 4 dB over).
		Settings.ThresholdDb = -3.f;
		Settings.Ratio = 20.f;
		Settings.KneeBandwidthDb = 2.f;
		Settings.LookAheadMsec = 3.f;
		Settings.AttackTimeMsec = 0.1f;
		Settings.ReleaseTimeMsec = 120.f;
		Settings.bAnalogMode = false;
		// (The preset's own copy is what a new instance of the effect starts from; SetSettings alone only reaches existing ones.)
		Preset->Settings = Settings;
		Preset->SetSettings(Settings);
		UAudioMixerBlueprintLibrary::AddMasterSubmixEffect(this, Preset);
		Limiter = Preset;
	}

	// The skirmish scripts play the local side's own tracks in turn, the first after ten seconds.
	MusicPrefix.Reset();
	LoopTrack.Reset();
	MusicTrackNumber = 0;
	MusicNextStart = 10.0;
	PlayMusic(FString());
	if (Logic && !FParse::Param(FCommandLine::Get(), TEXT("GenNoMusic")))
	{
		const TArray<FGenPlayer>& Players = Logic->GetPlayers();
		const int32 Local = Logic->GetLocalPlayer();
		const FString BaseSide = Players.IsValidIndex(Local) && Players[Local].Template ? Players[Local].Template->BaseSide : FString();
		MusicPrefix = BaseSide.Equals(TEXT("China"), ESearchCase::IgnoreCase) ? TEXT("Game_CHI_")
			: (BaseSide.Equals(TEXT("GLA"), ESearchCase::IgnoreCase) ? TEXT("Game_GLA_") : TEXT("Game_USA_"));
	}
}

// ---- who hears what -----------------------------------------------------------------------------

bool UGenAudioManager::ShouldPlayLocally(const FGenAudioEventInfo& Info, int32 Player) const
{
	// AudioManager::shouldPlayLocally
	using namespace GenSoundType;
	if (Info.SoundType == EGenAudioType::Music || !(Info.Type & (GenSoundType::Player | Allies | Enemies | Everyone)) || (Info.Type & Everyone))
	{
		return true;
	}
	if (Player == INDEX_NONE)
	{
		// "Player-based UI sound without specifying a player" plays; anything else needs an owner.
		return (Info.Type & GenSoundType::Player) && (Info.Type & GenSoundType::UI);
	}
	if (!Logic)
	{
		return false;
	}
	const int32 Local = Logic->GetLocalPlayer();
	if (Info.Type & GenSoundType::Player)
	{
		return Player == Local;
	}
	if (Info.Type & Allies)
	{
		return Player != Local && Logic->GetRelationship(Player, Local) == EGenRelationship::Allies;
	}
	if (Info.Type & Enemies)
	{
		return Logic->GetRelationship(Player, Local) == EGenRelationship::Enemies;
	}
	return false;
}

bool UGenAudioManager::IsPositional(const FRequest& Request) const
{
	// AudioEventRTS::isPositionalAudio
	return (Request.Info->Type & GenSoundType::World) && (Request.Object != GenInvalidObjectID || Request.bHasPos);
}

float UGenAudioManager::GetRangeScale(const FGenAudioEventInfo& Info, const FVector3f& Pos) const
{
	// MilesAudioManager::getEffectiveVolume: full volume within the minimum range, then falling off with
	// the distance, and nothing beyond the maximum.
	const FGenAudioSettings& S = FGenAudioData::Get().GetSettings();
	const bool bGlobal = (Info.Type & GenSoundType::Global) != 0;
	const float MinDist = bGlobal ? S.GlobalMinRange : Info.MinRange;
	const float MaxDist = bGlobal ? S.GlobalMaxRange : Info.MaxRange;
	const float Dist = FVector3f::Dist(MicPos, Pos);
	if (Dist >= MaxDist)
	{
		return 0.f;
	}
	return (Dist > MinDist && MinDist > 0.f) ? MinDist / Dist : 1.f;
}

float UGenAudioManager::GetEffectiveVolume(const FVoice& Voice) const
{
	const FGenAudioSettings& S = FGenAudioData::Get().GetSettings();
	if (Voice.bStream)
	{
		return OutputGain * Voice.Volume * S.DefaultSpeechVolume;
	}
	if (Voice.bPositional)
	{
		return OutputGain * Voice.Volume * S.Default3DSoundVolume * ZoomVolume * GetRangeScale(*Voice.Info, Voice.Pos);
	}
	// Relative2DVolume: "if less than 0, 2D volumes will be 100% - x%, while 3D sounds will be 100%".
	return OutputGain * Voice.Volume * S.DefaultSoundVolume * (S.Relative2DVolume < 0.f ? 1.f + S.Relative2DVolume : 1.f);
}

float UGenAudioManager::GetUserScale(const FVoice& Voice)
{
	const float Kind = (Voice.bStream || Voice.bSpeech) ? UserVolumes.Speech
		: ((Voice.Info && (Voice.Info->Type & GenSoundType::Voice)) ? UserVolumes.Voice : UserVolumes.Sound);
	return UserVolumes.Master * Kind;
}

void UGenAudioManager::SetUserVolumes(float Master, float Music, float Sound, float Voice, float Speech)
{
	UserVolumes.Master = FMath::Clamp(Master, 0.f, 1.f);
	UserVolumes.Music = FMath::Clamp(Music, 0.f, 1.f);
	UserVolumes.Sound = FMath::Clamp(Sound, 0.f, 1.f);
	UserVolumes.Voice = FMath::Clamp(Voice, 0.f, 1.f);
	UserVolumes.Speech = FMath::Clamp(Speech, 0.f, 1.f);
}

void UGenAudioManager::ApplyUserVolumes()
{
	if (MusicComponent && MusicWave)
	{
		MusicComponent->SetVolumeMultiplier(FMath::Max(MusicBaseVolume * UserVolumes.Master * UserVolumes.Music, 0.0001f));
	}
	for (const FVoice& V : Voices)
	{
		if (Components.IsValidIndex(V.Slot) && Components[V.Slot])
		{
			Components[V.Slot]->SetVolumeMultiplier(FMath::Max(GetEffectiveVolume(V) * GetUserScale(V), 0.0001f));
		}
	}
}

void UGenAudioManager::GetPan(const FVoice& Voice, float& OutLeft, float& OutRight) const
{
	// -1: all the way left, 1: all the way right; the two speakers share the sound at equal power.
	float Side = 0.f;
	if (Voice.bPositional)
	{
		const FVector3f ToSound = Voice.Pos - MicPos;
		const float Dist = ToSound.Size();
		Side = Dist > 1.f ? FMath::Clamp(FVector3f::DotProduct(ToSound, MicRight) / Dist, -1.f, 1.f) : 0.f;
	}
	FMath::SinCos(&OutRight, &OutLeft, (Side + 1.f) * 0.25f * UE_PI);
}

// ---- playing ------------------------------------------------------------------------------------

uint32 UGenAudioManager::PlayForObject(const FString& Event, const FGenObject& Obj)
{
	FRequest Request;
	Request.Info = FGenAudioData::Get().Find(Event);
	Request.Object = Obj.GetID();
	Request.bHasPos = true;
	Request.Pos = Logic ? Logic->GetRenderPosition(Obj) : Obj.GetPosition();
	Request.Player = Obj.GetOwner();
	return AddEvent(Request);
}

uint32 UGenAudioManager::PlayAt(const FString& Event, const FVector3f& Pos, int32 Player)
{
	FRequest Request;
	Request.Info = FGenAudioData::Get().Find(Event);
	Request.bHasPos = true;
	Request.Pos = Pos;
	Request.Player = Player;
	return AddEvent(Request);
}

uint32 UGenAudioManager::PlayUI(const FString& Event, int32 Player)
{
	FRequest Request;
	Request.Info = FGenAudioData::Get().Find(Event);
	Request.Player = Player;
	return AddEvent(Request);
}

uint32 UGenAudioManager::AddEvent(const FRequest& Request)
{
	// AudioManager::addAudioEvent
	if (!Request.Info || !MapActor || bSuspended)
	{
		return 0;
	}
	if (!IsInGameThread())
	{
		FScopeLock Lock(&SimAskLock);
		SimAsks.AddDefaulted_GetRef().Request = Request;
		return 0;
	}
	if (!ShouldPlayLocally(*Request.Info, Request.Player))
	{
		return 0; // AHSV_NotForLocal
	}
	if (Request.Info->DelayMax > 0.f && Request.FireTime == 0.0)
	{
		FRequest& Later = Delayed.Add_GetRef(Request);
		Later.FireTime = Now + double(Random.FRandRange(Request.Info->DelayMin, Request.Info->DelayMax)) / 1000.0;
		return 0;
	}
	return StartVoice(Request);
}

int32 UGenAudioManager::FindVoice(uint32 Handle) const
{
	return Handle != 0 ? Voices.IndexOfByPredicate([Handle](const FVoice& V) { return V.Handle == Handle; }) : INDEX_NONE;
}

bool UGenAudioManager::IsPlaying(uint32 Handle) const
{
	return FindVoice(Handle) != INDEX_NONE;
}

void UGenAudioManager::Stop(uint32 Handle)
{
	const int32 Index = FindVoice(Handle);
	if (Index != INDEX_NONE)
	{
		StopVoiceAt(Index);
	}
}

void UGenAudioManager::StopVoiceAt(int32 Index)
{
	const FVoice& V = Voices[Index];
	if (Components.IsValidIndex(V.Slot) && Components[V.Slot])
	{
		Components[V.Slot]->Stop();
	}
	FreeSlots.Add(V.Slot);
	Voices.RemoveAt(Index);
}

int32 UGenAudioManager::CountChannels(bool bPositional, bool bStream) const
{
	int32 Count = 0;
	for (const FVoice& V : Voices)
	{
		Count += (V.bStream == bStream && (bStream || V.bPositional == bPositional)) ? 1 : 0;
	}
	return Count;
}

int32 UGenAudioManager::FindLowerPriority(const FGenAudioEventInfo& Info, bool bPositional, bool bStream) const
{
	// MilesAudioManager::findLowestPrioritySound: the least important thing playing, if it matters less than this.
	int32 Best = INDEX_NONE;
	for (int32 i = 0; i < Voices.Num(); ++i)
	{
		const FVoice& V = Voices[i];
		if (V.bStream != bStream || (!bStream && V.bPositional != bPositional) || V.Info->Priority >= Info.Priority)
		{
			continue;
		}
		if (Best == INDEX_NONE || V.Info->Priority < Voices[Best].Info->Priority)
		{
			Best = i;
		}
	}
	return Best;
}

int32 UGenAudioManager::AcquireSlot()
{
	if (FreeSlots.Num() > 0)
	{
		return FreeSlots.Pop(EAllowShrinking::No);
	}
	if (Components.Num() >= MaxSlots || !MapActor)
	{
		return INDEX_NONE;
	}
	UAudioComponent* C = NewObject<UAudioComponent>(MapActor, NAME_None, RF_Transient);
	C->bAutoActivate = false;
	C->bAutoDestroy = false;
	C->SetupAttachment(MapActor->GetRootComponent());
	// To Unreal every sound is a plain stereo one: how loud it is at its distance and where it sits between
	// the speakers are the game's own rules (the component's volume, the sound wave's pan).
	C->bAllowSpatialization = false;
	C->bIsUISound = true;
	C->RegisterComponent();
	SlotWaves.Add(nullptr);
	return Components.Add(C);
}

TSharedPtr<const FGenPcm> UGenAudioManager::LoadSound(EGenAudioType Type, const FString& FileOrSound)
{
	const FString Path = FGenAudioData::Get().ResolvePath(Type, FileOrSound);
	const FString Key = Path.ToLower();
	if (const TSharedPtr<const FGenPcm>* Found = SoundCache.Find(Key))
	{
		SoundCacheOrder.RemoveSingle(Key);
		SoundCacheOrder.Add(Key);
		return *Found;
	}
	TArray<uint8> Bytes;
	TSharedPtr<FGenPcm> Pcm = MakeShared<FGenPcm>();
	FString Error = TEXT("file not found");
	if (!FGenFileSystem::Get().ReadFile(Path, Bytes) || !GenWav::Decode(Bytes, *Pcm, &Error))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Audio: %s: %s"), *Path, *Error);
		Pcm.Reset(); // remembered as missing, so it isn't looked for again
	}
	SoundCache.Add(Key, Pcm);
	SoundCacheOrder.Add(Key);
	SoundCacheBytes += Pcm ? int64(Pcm->Samples.Num()) * 2 : 0;
	// Out with what hasn't been heard for longest (unless it is playing right now).
	for (int32 i = 0; SoundCacheBytes > SoundCacheLimitBytes && i < SoundCacheOrder.Num() - 1;)
	{
		const TSharedPtr<const FGenPcm> Old = SoundCache.FindRef(SoundCacheOrder[i]);
		if (Old && Old.GetSharedReferenceCount() > 2)
		{
			++i;
			continue;
		}
		SoundCacheBytes -= Old ? int64(Old->Samples.Num()) * 2 : 0;
		SoundCache.Remove(SoundCacheOrder[i]);
		SoundCacheOrder.RemoveAt(i);
	}
	return Pcm;
}

uint32 UGenAudioManager::StartVoice(const FRequest& Request)
{
	const FGenAudioEventInfo& Info = *Request.Info;
	const FGenAudioSettings& S = FGenAudioData::Get().GetSettings();
	const bool bPositional = IsPositional(Request);
	const bool bStream = Info.SoundType == EGenAudioType::Streaming;
	const bool bInterrupt = (Info.Control & GenAudioControl::Interrupt) != 0;
	FVector3f Pos = Request.Pos;
	const FGenObject* Obj = Logic && Request.Object != GenInvalidObjectID ? Logic->FindObject(Request.Object) : nullptr;
	if (Obj)
	{
		Pos = Logic->GetRenderPosition(*Obj);
	}

	// SoundManager::canPlayNow
	if (bPositional && !(Info.Type & GenSoundType::Global) && Info.Priority != EGenAudioPriority::Critical)
	{
		if (FVector3f::Dist(MicPos, Pos) >= Info.MaxRange)
		{
			++NumCulled[CullDistance];
			return 0;
		}
		if ((Info.Type & GenSoundType::Shrouded) && Logic && !Logic->IsPositionClear(Pos))
		{
			++NumCulled[CullShroud];
			return 0;
		}
	}
	// violatesVoice: an object says one thing at a time; only an interrupting line cuts it short.
	if ((Info.Type & GenSoundType::Voice) && Request.Object != GenInvalidObjectID)
	{
		const int32 Speaking = Voices.IndexOfByPredicate([&Request](const FVoice& V) { return V.Object == Request.Object && (V.Info->Type & GenSoundType::Voice); });
		if (Speaking != INDEX_NONE)
		{
			if (!bInterrupt)
			{
				++NumCulled[CullVoice];
				return 0;
			}
			StopVoiceAt(Speaking);
		}
	}
	// doesViolateLimit: so many of one event at once; an interrupting one takes the place of the oldest.
	auto FindOldestSame = [this, &Info, bPositional](int32& OutCount)
	{
		int32 Oldest = INDEX_NONE;
		OutCount = 0;
		for (int32 i = 0; i < Voices.Num(); ++i)
		{
			if (Voices[i].Info == &Info && Voices[i].bPositional == bPositional)
			{
				Oldest = Oldest == INDEX_NONE ? i : Oldest;
				++OutCount;
			}
		}
		return Oldest;
	};
	int32 SameCount = 0;
	int32 OldestSame = FindOldestSame(SameCount);
	if (Info.Limit > 0 && SameCount >= Info.Limit)
	{
		if (!bInterrupt)
		{
			++NumCulled[CullLimit];
			return 0;
		}
		StopVoiceAt(OldestSame);
		OldestSame = FindOldestSame(SameCount);
	}
	// The channels: a free one, else that of something less important, else (interrupting) of its own kind.
	const int32 Cap = bStream ? S.StreamCount : (bPositional ? S.SampleCount3D : S.SampleCount2D);
	if (CountChannels(bPositional, bStream) >= Cap)
	{
		const int32 Lower = FindLowerPriority(Info, bPositional, bStream);
		if (Lower != INDEX_NONE)
		{
			StopVoiceAt(Lower);
		}
		else if (bInterrupt && OldestSame != INDEX_NONE)
		{
			StopVoiceAt(OldestSame);
		}
		else
		{
			++NumCulled[CullChannels];
			return 0;
		}
	}

	// AudioEventRTS::generateFilename / generatePlayInfo: which file, and its attack and decay.
	TArray<TSharedPtr<const FGenPcm>> Parts;
	int32 LoopStart = INDEX_NONE, LoopEnd = INDEX_NONE, LoopCount = 1;
	FString Played;
	const bool bLoop = Info.SoundType == EGenAudioType::SoundEffect && (Info.Control & GenAudioControl::Loop) != 0;
	if (Info.SoundType == EGenAudioType::SoundEffect)
	{
		const int32 N = Info.Sounds.Num();
		if (N == 0)
		{
			return 0;
		}
		int32 Which;
		if (Info.Control & GenAudioControl::Random)
		{
			Which = Random.RandRange(0, N - 1);
			if (Which == Info.LastIndex && N > 2)
			{
				Which = (Which + 1) % N;
			}
		}
		else
		{
			Which = (Info.LastIndex + 1) % N;
		}
		Info.LastIndex = Which;
		Played = Info.Sounds[Which];
		if (bLoop)
		{
			if (Info.Attack.Num() > 0)
			{
				Parts.Add(LoadSound(Info.SoundType, Info.Attack[Random.RandRange(0, Info.Attack.Num() - 1)]));
			}
			LoopStart = Parts.Num();
			if (Info.Control & GenAudioControl::All)
			{
				// The loop is all of the sounds, one after the other.
				for (const FString& Sound : Info.Sounds) { Parts.Add(LoadSound(Info.SoundType, Sound)); }
			}
			else
			{
				Parts.Add(LoadSound(Info.SoundType, Played));
			}
			LoopEnd = Parts.Num() - 1;
			if (Info.Decay.Num() > 0)
			{
				Parts.Add(LoadSound(Info.SoundType, Info.Decay[Random.RandRange(0, Info.Decay.Num() - 1)]));
			}
			// An object's own loop runs its count, or (count 0) until the object lets go of it; a loop that
			// nobody holds on to plays once through rather than for ever.
			LoopCount = Request.bManagedLoop ? FMath::Max(Info.LoopCount, 0) : FMath::Max(Info.LoopCount, 1);
			if (Request.bManagedLoop && Info.DelayMax > 0.f)
			{
				LoopCount = 1; // (a loop with a Delay: one sound, then UpdateLoops waits and asks for the next)
			}
		}
		else
		{
			Parts.Add(LoadSound(Info.SoundType, Played));
		}
	}
	else
	{
		Played = Info.Filename;
		Parts.Add(LoadSound(Info.SoundType, Info.Filename));
	}
	// A missing file leaves a hole; without the sound itself there is nothing to play.
	const TSharedPtr<const FGenPcm> Main = Parts.IsValidIndex(FMath::Max(LoopStart, 0)) ? Parts[FMath::Max(LoopStart, 0)] : nullptr;
	if (!Main || Main->Samples.Num() == 0)
	{
		return 0;
	}
	double Seconds = 0.0;
	for (TSharedPtr<const FGenPcm>& Part : Parts)
	{
		if (Part && (Part->SampleRate != Main->SampleRate || Part->NumChannels != Main->NumChannels))
		{
			// The parts go to the mixer as one stream: one format for all of them.
			TSharedPtr<FGenPcm> Converted = MakeShared<FGenPcm>();
			GenWav::Convert(*Part, Main->SampleRate, Main->NumChannels, *Converted);
			Part = Converted;
		}
		Seconds += Part ? double(Part->GetDuration()) : 0.0;
	}

	const int32 Slot = AcquireSlot();
	if (Slot == INDEX_NONE)
	{
		++NumCulled[CullOther];
		return 0;
	}
	FVoice& V = Voices.AddDefaulted_GetRef();
	V.Handle = NextHandle++;
	V.Info = &Info;
	V.Slot = Slot;
	V.bPositional = bPositional;
	V.bStream = bStream;
	V.Pos = Pos;
	V.Object = Request.Object;
	V.Volume = Info.Volume * Random.FRandRange(1.f + FMath::Min(Info.VolumeShift, 0.f), 1.f); // "volume shifts are between 0 and 1"
	V.Pitch = FMath::Clamp(Random.FRandRange(Info.PitchShiftMin, Info.PitchShiftMax), 0.1f, 4.f);
	V.StartTime = Now;
	V.bLoop = bLoop && LoopCount == 0;
	V.EndTime = V.bLoop ? 0.0 : Now + Seconds * (bLoop ? double(LoopCount) : 1.0) / double(V.Pitch) + VoiceTailSeconds;

	UGenPcmSoundWave* Wave = NewObject<UGenPcmSoundWave>(this, NAME_None, RF_Transient);
	Wave->Setup(Parts, LoopStart, LoopEnd, LoopCount, bPositional);
	float PanLeft = 0.f, PanRight = 0.f;
	GetPan(V, PanLeft, PanRight);
	Wave->SetPan(PanLeft, PanRight);
	V.Wave = Wave;
	SlotWaves[Slot] = Wave;
	UAudioComponent* C = Components[Slot];
	C->Stop();
	C->SetSound(Wave);
	const float Volume = GetEffectiveVolume(V);
	C->SetVolumeMultiplier(FMath::Max(Volume * GetUserScale(V), 0.0001f));
	C->SetPitchMultiplier(V.Pitch);
	C->Play();
	++NumPlayed;
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [audio f%u] %s (%s) %s vol=%.2f pitch=%.2f %.2fs%s playing=%d"), Logic ? Logic->GetFrame() : 0, *Info.Name, *Played,
		bStream ? TEXT("speech") : (bPositional ? *FString::Printf(TEXT("at (%.0f, %.0f) dist=%.0f L=%.2f R=%.2f"), Pos.X, Pos.Y, FVector3f::Dist(MicPos, Pos), PanLeft, PanRight) : TEXT("2D")),
		Volume, V.Pitch, Seconds, V.bLoop ? TEXT(" loop") : TEXT(""), Voices.Num());
	return V.Handle;
}

// ---- per frame ----------------------------------------------------------------------------------

void UGenAudioManager::UpdateMicrophone()
{
	// AudioManager::update: the microphone sits where the camera looks, a little way up towards it.
	UWorld* World = GetWorld();
	APlayerController* PC = World ? World->GetFirstPlayerController() : nullptr;
	const AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	if (!Cam || !Cam->GetCamera() || !MapActor || !Logic)
	{
		return;
	}
	const FGenAudioSettings& S = FGenAudioData::Get().GetSettings();
	const FTransform MapXf = MapActor->GetActorTransform();
	const FVector GroundUE = MapXf.InverseTransformPosition(Cam->GetTarget());
	const FVector CameraUE = MapXf.InverseTransformPosition(Cam->GetCamera()->GetComponentLocation());
	FVector3f Ground = FVector3f(GenCoords::FromUE(GroundUE));
	const FVector3f Camera = FVector3f(GenCoords::FromUE(CameraUE));
	FVector3f GroundToCamera = Camera - Ground;
	float Scale = S.MicrophoneMaxPercentageBetweenGroundAndCamera;
	if (Camera.Z > S.MicrophoneDesiredHeightAboveTerrain && GroundToCamera.Z > 0.f)
	{
		Scale = FMath::Min(Scale, S.MicrophoneDesiredHeightAboveTerrain / GroundToCamera.Z);
	}
	Ground.Z = Logic->GetGroundHeight(Ground.X, Ground.Y);
	MicPos = Ground + GroundToCamera * Scale;

	// "We can't boost a sound above 100%, instead reduce the normal sound level": zoomed in, the world is louder.
	const float Boost = S.ZoomSoundVolumePercentageAmount;
	const float Dist = FVector3f::Dist(Camera, MicPos);
	ZoomVolume = Dist < S.ZoomMinDistance ? 1.f
		: (Dist < S.ZoomMaxDistance ? 1.f - (Dist - S.ZoomMinDistance) / FMath::Max(S.ZoomMaxDistance - S.ZoomMinDistance, 1.f) * Boost : 1.f - Boost);

	// It faces the way the camera faces: what is on the right of the screen is heard on the right.
	const FVector RightUE = MapXf.InverseTransformVector(FRotationMatrix(FRotator(0.0, Cam->GetCamera()->GetComponentRotation().Yaw, 0.0)).GetUnitAxis(EAxis::Y));
	MicRight = FVector3f(float(RightUE.X), float(-RightUE.Y), 0.f).GetSafeNormal(UE_SMALL_NUMBER, FVector3f(1.f, 0.f, 0.f));
}

void UGenAudioManager::Update(float DeltaSeconds)
{
	GEN_PERF_SCOPE("R.Audio");
	if (!MapActor)
	{
		return;
	}
	Now += double(FMath::Clamp(DeltaSeconds, 0.f, 0.5f));
	UpdateMicrophone();
	DoSimAsks();

	// Sounds that were told to wait (Delay).
	for (int32 i = Delayed.Num() - 1; i >= 0; --i)
	{
		if (Now >= Delayed[i].FireTime)
		{
			const FRequest Request = Delayed[i];
			Delayed.RemoveAtSwap(i);
			StartVoice(Request);
		}
	}

	for (int32 i = Voices.Num() - 1; i >= 0; --i)
	{
		FVoice& V = Voices[i];
		UAudioComponent* C = Components.IsValidIndex(V.Slot) ? Components[V.Slot].Get() : nullptr;
		UGenPcmSoundWave* Wave = V.Wave.Get();
		// A looping sound that was let go: once its decay is with the mixer, a moment more and it is over.
		if (V.EndTime == 0.0 && (!Wave || Wave->IsFinished()))
		{
			V.EndTime = Now + VoiceTailSeconds;
		}
		if (!C || (V.EndTime > 0.0 && Now >= V.EndTime))
		{
			StopVoiceAt(i);
			continue;
		}
		if (V.bPositional)
		{
			// The sound stays with its object.
			if (const FGenObject* Obj = Logic && V.Object != GenInvalidObjectID ? Logic->FindObject(V.Object) : nullptr)
			{
				V.Pos = Logic->GetRenderPosition(*Obj);
			}
			if (Wave)
			{
				float PanLeft = 0.f, PanRight = 0.f;
				GetPan(V, PanLeft, PanRight);
				Wave->SetPan(PanLeft, PanRight);
			}
		}
		const float Volume = GetEffectiveVolume(V);
		if (V.bPositional && !(V.Info->Type & GenSoundType::Global) && V.Info->Priority != EGenAudioPriority::Critical)
		{
			// MilesAudioManager::processPlayingList: a sound in the world that has become too faint to hear
			// (the camera moved away) is taken off, and its channel is free for something that can be heard.
			const FGenAudioSettings& S = FGenAudioData::Get().GetSettings();
			const float Considered = Volume / (OutputGain * (S.Default3DSoundVolume > 0.f && S.DefaultSoundVolume > 0.f ? S.DefaultSoundVolume : 1.f));
			if (Considered < S.MinSampleVolume)
			{
				StopVoiceAt(i);
				continue;
			}
		}
		C->SetVolumeMultiplier(FMath::Max(Volume * GetUserScale(V), 0.0001f));
	}

	UpdateLoops();
	UpdateEva();
	UpdateMusic(DeltaSeconds);

	if (bTrace)
	{
		static double LastReport = 0.0;
		if (Now < LastReport) { LastReport = 0.0; }
		if (Now - LastReport >= 5.0)
		{
			LastReport = Now;
			FString LoopNames;
			for (const TPair<uint64, FObjectLoop>& It : Loops)
			{
				if (LoopNames.Len() < 120)
				{
					LoopNames += FString::Printf(TEXT(" #%u:%s%s"), uint32(It.Key >> 8), *It.Value.Event, FindVoice(It.Value.Handle) != INDEX_NONE ? TEXT("") : TEXT("(unheard)"));
				}
			}
			UE_LOG(LogGenerals, Log, TEXT("  [audio f%u] %d played so far, not played: %d too far, %d unseen, %d already speaking, %d over their limit, %d for want of a channel, %d other; %d playing (%d in the world), %d loops wanted%s, music '%s', cache %.1f MB"),
				Logic ? Logic->GetFrame() : 0, NumPlayed, NumCulled[CullDistance], NumCulled[CullShroud], NumCulled[CullVoice], NumCulled[CullLimit], NumCulled[CullChannels], NumCulled[CullOther],
				Voices.Num(), CountChannels(true, false), Loops.Num(), *LoopNames, *MusicPlaying, double(SoundCacheBytes) / 1048576.0);
		}
	}
}

// ---- the sounds objects keep making -------------------------------------------------------------

const FString& UGenAudioManager::GetTemplateSound(const FGenThingTemplate& Template, const TCHAR* Key)
{
	const FString CacheKey = Template.Name + TEXT("|") + Key;
	if (const FString* Found = TemplateSounds.Find(CacheKey))
	{
		return *Found;
	}
	FString Sound;
	if (Template.Block)
	{
		Sound = Template.Block->GetString(Key);
		for (const FGenIniEntry& E : Template.Block->Entries)
		{
			if (Sound.IsEmpty() && E.Block && E.Key.Equals(TEXT("UnitSpecificSounds"), ESearchCase::IgnoreCase))
			{
				Sound = E.Block->GetString(Key);
			}
		}
	}
	if (Sound.Equals(TEXT("NoSound"), ESearchCase::IgnoreCase))
	{
		Sound.Reset();
	}
	return TemplateSounds.Add(CacheKey, Sound);
}

void UGenAudioManager::DoSimAsks()
{
	TArray<FSimAsk> Asks;
	{
		FScopeLock Lock(&SimAskLock);
		Asks = MoveTemp(SimAsks);
		SimAsks.Reset();
	}
	for (const FSimAsk& A : Asks)
	{
		switch (A.Kind)
		{
		case FSimAsk::Event: AddEvent(A.Request); break;
		case FSimAsk::Loop: SetObjectLoop(A.Object, A.Slot, A.Name, A.ExpireSeconds); break;
		case FSimAsk::Destroyed: OnObjectDestroyed(A.Object); break;
		case FSimAsk::Adviser: Eva(*A.Name); break;
		}
	}
}

void UGenAudioManager::SetObjectLoop(FGenObjectID Object, int32 Slot, const FString& Event, float ExpireSeconds)
{
	if (!IsInGameThread())
	{
		FScopeLock Lock(&SimAskLock);
		FSimAsk& A = SimAsks.AddDefaulted_GetRef();
		A.Kind = FSimAsk::Loop;
		A.Object = Object;
		A.Slot = Slot;
		A.Name = Event;
		A.ExpireSeconds = ExpireSeconds;
		return;
	}
	const uint64 Key = LoopKey(Object, Slot);
	FObjectLoop* Loop = Loops.Find(Key);
	if (!Loop)
	{
		if (Event.IsEmpty())
		{
			return;
		}
		Loop = &Loops.Add(Key);
	}
	Loop->ExpireTime = ExpireSeconds > 0.f ? Now + double(ExpireSeconds) : 0.0;
	if (Loop->Event == Event)
	{
		if (ExpireSeconds > 0.f && FindVoice(Loop->Handle) == INDEX_NONE)
		{
			// FiringTracker::shotFired: "if the sound has stopped playing, then we need to re-add it".
			Loop->bStarted = false;
			Loop->NextTry = 0.0;
		}
		return;
	}
	// Something else from now on: the old sound gets its decay and goes.
	const int32 Index = FindVoice(Loop->Handle);
	if (Index != INDEX_NONE)
	{
		Voices[Index].bReleased = true;
		if (UGenPcmSoundWave* Wave = Voices[Index].Wave.Get()) { Wave->Release(); }
	}
	Loop->Event = Event;
	Loop->Handle = 0;
	Loop->NextTry = 0.0;
	Loop->bStarted = false;
	if (Event.IsEmpty())
	{
		Loops.Remove(Key);
	}
}

void UGenAudioManager::OnObjectDestroyed(FGenObjectID Object)
{
	if (!IsInGameThread())
	{
		FScopeLock Lock(&SimAskLock);
		FSimAsk& A = SimAsks.AddDefaulted_GetRef();
		A.Kind = FSimAsk::Destroyed;
		A.Object = Object;
		return;
	}
	for (int32 Slot = 0; Slot < 12; ++Slot)
	{
		SetObjectLoop(Object, Slot, FString());
	}
}

void UGenAudioManager::UpdateLoops()
{
	const double Retry = double(FGenAudioData::Get().GetSettings().TimeBetweenDrawableSoundsMS) / 1000.0;
	// FiringTracker::update: a firing loop nobody has asked for lately stops.
	TArray<uint64, TInlineAllocator<8>> Expired;
	for (const TPair<uint64, FObjectLoop>& It : Loops)
	{
		if (It.Value.ExpireTime > 0.0 && Now >= It.Value.ExpireTime) { Expired.Add(It.Key); }
	}
	for (uint64 Key : Expired)
	{
		SetObjectLoop(FGenObjectID(Key >> 8), int32(Key & 0xff), FString());
	}
	for (TPair<uint64, FObjectLoop>& It : Loops)
	{
		FObjectLoop& Loop = It.Value;
		if (FindVoice(Loop.Handle) != INDEX_NONE || Now < Loop.NextTry)
		{
			continue;
		}
		// Not playing (never started, or it went out of earshot): try (again), but not every frame - and
		// only what loops for ever (Drawable::update: a one-shot sound is started once, or it would keep repeating).
		const FGenAudioEventInfo* Info = FGenAudioData::Get().Find(Loop.Event);
		const bool bPermanent = Info && (Info->Control & GenAudioControl::Loop) != 0 && Info->LoopCount == 0;
		if (Loop.bStarted && !bPermanent)
		{
			continue;
		}
		const FGenObjectID Object = FGenObjectID(It.Key >> 8);
		const FGenObject* Obj = Logic ? Logic->FindObject(Object) : nullptr;
		Loop.Handle = 0;
		if (Info && Info->DelayMax > 0.f && !Loop.bWaited)
		{
			// AudioEventRTS::generateFilename: "we want delay to apply between every loop of a sound" - a loop
			// with a Delay plays one of its sounds, waits a new random while, then another (birds now and then).
			Loop.bWaited = true;
			Loop.NextTry = Now + double(Random.FRandRange(Info->DelayMin, Info->DelayMax)) / 1000.0;
			continue;
		}
		Loop.bWaited = false;
		Loop.NextTry = Now + Retry * double(Random.FRandRange(0.8f, 1.2f));
		if (!Obj)
		{
			continue;
		}
		Loop.bStarted = true;
		FRequest Request;
		Request.Info = Info;
		Request.Object = Object;
		Request.bHasPos = true;
		Request.Pos = Logic->GetRenderPosition(*Obj);
		Request.Player = Obj->GetOwner();
		Request.bManagedLoop = true;
		Request.FireTime = Now; // (the wait was done above: not the delayed list, whose request has no handle to watch)
		Loop.Handle = AddEvent(Request);
	}
}

// ---- the adviser --------------------------------------------------------------------------------

void UGenAudioManager::Eva(const TCHAR* EventName)
{
	// Eva::setShouldPlay
	if (!IsInGameThread())
	{
		FScopeLock Lock(&SimAskLock);
		FSimAsk& A = SimAsks.AddDefaulted_GetRef();
		A.Kind = FSimAsk::Adviser;
		A.Name = EventName;
		return;
	}
	const FGenEvaEvent* Event = FGenAudioData::Get().FindEva(EventName);
	if (!Event || Event->Priority <= 0 || !MapActor)
	{
		return;
	}
	if (const double* Next = EvaNextAllowed.Find(Event->Name))
	{
		if (Now < *Next)
		{
			return; // said not long ago
		}
	}
	if (!EvaPending.ContainsByPredicate([Event](const FEvaPending& P) { return P.Event == Event; }))
	{
		EvaPending.Add({ Event, Now });
	}
}

void UGenAudioManager::UpdateEva()
{
	if (!Logic)
	{
		return;
	}
	// Eva::update polls a few things itself; a power shortage is one of them.
	static double NextPoll = 0.0;
	if (Now < NextPoll - 5.0) { NextPoll = 0.0; }
	if (Now >= NextPoll)
	{
		NextPoll = Now + 2.0;
		int32 Produced = 0, Consumed = 0;
		Logic->GetEnergy(Logic->GetLocalPlayer(), Produced, Consumed);
		if (Consumed > Produced && Logic->GetFrame() > 60)
		{
			Eva(TEXT("LowPower"));
		}
	}

	// What has waited too long is dropped; then, if the adviser isn't talking, the most important goes next.
	EvaPending.RemoveAll([this](const FEvaPending& P) { return Now > P.RequestTime + double(P.Event->ExpirationTimeMS) / 1000.0; });
	if (EvaPending.Num() == 0 || IsPlaying(EvaHandle))
	{
		return;
	}
	int32 Best = 0;
	for (int32 i = 1; i < EvaPending.Num(); ++i)
	{
		if (EvaPending[i].Event->Priority > EvaPending[Best].Event->Priority) { Best = i; }
	}
	const FGenEvaEvent* Event = EvaPending[Best].Event;
	EvaPending.RemoveAt(Best);
	EvaNextAllowed.Add(Event->Name, Now + double(Event->TimeBetweenChecksMS) / 1000.0);

	const TArray<FGenPlayer>& Players = Logic->GetPlayers();
	const int32 Local = Logic->GetLocalPlayer();
	const FString Side = Players.IsValidIndex(Local) && Players[Local].Template ? Players[Local].Template->Side.ToLower() : FString();
	const TArray<FString>* Sounds = Event->SideSounds.Find(Side);
	if (!Sounds || Sounds->Num() == 0)
	{
		return;
	}
	EvaHandle = PlayUI((*Sounds)[Random.RandRange(0, Sounds->Num() - 1)], Local);
	// (The adviser speaks through an interface sound: heard at the speech volume, not the sound effects'.)
	if (const int32 Index = FindVoice(EvaHandle); Index != INDEX_NONE)
	{
		FVoice& V = Voices[Index];
		V.bSpeech = true;
		if (Components.IsValidIndex(V.Slot) && Components[V.Slot])
		{
			Components[V.Slot]->SetVolumeMultiplier(FMath::Max(GetEffectiveVolume(V) * GetUserScale(V), 0.0001f));
		}
	}
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [audio f%u] the adviser: %s (%s)"), Logic->GetFrame(), *Event->Name, EvaHandle ? TEXT("playing") : TEXT("could not play"));
}

// ---- music --------------------------------------------------------------------------------------

void UGenAudioManager::PlayMusic(const FString& Track)
{
	if (MusicComponent)
	{
		MusicComponent->Stop();
	}
	MusicWave = nullptr;
	MusicPlaying.Reset();
	const FGenAudioEventInfo* Info = FGenAudioData::Get().Find(Track);
	if (!Info || Info->SoundType != EGenAudioType::Music || !MapActor)
	{
		return;
	}
	TArray<uint8> Bytes;
	const FString Path = FGenAudioData::Get().ResolvePath(EGenAudioType::Music, Info->Filename);
	if (!FGenFileSystem::Get().ReadFile(Path, Bytes))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Music: %s not found"), *Path);
		return;
	}
	UGenMp3SoundWave* Wave = NewObject<UGenMp3SoundWave>(this, NAME_None, RF_Transient);
	if (!Wave->SetupFromFile(MoveTemp(Bytes)))
	{
		UE_LOG(LogGenerals, Warning, TEXT("Music: %s can't be decoded"), *Path);
		return;
	}
	if (!MusicComponent)
	{
		MusicComponent = NewObject<UAudioComponent>(MapActor, NAME_None, RF_Transient);
		MusicComponent->bAutoActivate = false;
		MusicComponent->bAutoDestroy = false;
		MusicComponent->bAllowSpatialization = false;
		MusicComponent->bIsUISound = true;
		MusicComponent->SetupAttachment(MapActor->GetRootComponent());
		MusicComponent->RegisterComponent();
	}
	MusicWave = Wave;
	MusicPlaying = Info->Name;
	MusicComponent->SetSound(Wave);
	// (A music track goes to the speakers as it is, each side at its own level: no gain to make up.)
	MusicBaseVolume = Info->Volume * FGenAudioData::Get().GetSettings().DefaultMusicVolume;
	MusicComponent->SetVolumeMultiplier(FMath::Max(MusicBaseVolume * UserVolumes.Master * UserVolumes.Music, 0.0001f));
	MusicComponent->Play();
	UE_CLOG(bTrace, LogGenerals, Log, TEXT("  [audio f%u] music: %s (%s, %d Hz, %d channels)"), Logic ? Logic->GetFrame() : 0, *Info->Name, *Info->Filename,
		int32(Wave->GetSampleRateForCurrentPlatform()), Wave->NumChannels);
}

void UGenAudioManager::StartShell(AGenMapActor* InMapActor)
{
	MapActor = InMapActor;
	Logic = nullptr;
	FGenAudioData::Get().Initialize();
	Now = 0.0;
	bTrace = FParse::Param(FCommandLine::Get(), TEXT("GenTrace"));
	PlayTrackOnly(TEXT("Shell"));
}

void UGenAudioManager::PlayTrackOnly(const FString& Track)
{
	MusicPrefix.Reset();
	LoopTrack.Reset();
	if (FParse::Param(FCommandLine::Get(), TEXT("GenNoMusic")))
	{
		return;
	}
	LoopTrack = Track;
	PlayMusic(Track);
}

void UGenAudioManager::UpdateMusic(float DeltaSeconds)
{
	if (MusicPrefix.IsEmpty())
	{
		if (!LoopTrack.IsEmpty() && MusicWave && MusicWave->IsFinished())
		{
			PlayMusic(LoopTrack);
		}
		return;
	}
	if (MusicWave)
	{
		if (!MusicWave->IsFinished())
		{
			return;
		}
		// 'MUSIC_TRACK_HAS_COMPLETED': on to the next, after a breath (and a longer one once all ten have played).
		PlayMusic(FString());
		MusicNextStart = Now + (MusicTrackNumber >= 10 ? 30.0 : 2.0);
		return;
	}
	if (Now >= MusicNextStart)
	{
		MusicTrackNumber = MusicTrackNumber >= 10 ? 1 : MusicTrackNumber + 1;
		PlayMusic(FString::Printf(TEXT("%s%02d"), *MusicPrefix, MusicTrackNumber));
		if (!MusicWave)
		{
			MusicNextStart = Now + 5.0; // not there: try the next one in a while
		}
	}
}

// ---- tests --------------------------------------------------------------------------------------

int32 UGenAudioManager::SelfTest(int32& OutFailed, double& OutSeconds)
{
	FGenAudioData& Data = FGenAudioData::Get();
	Data.Initialize();
	TSet<FString> Paths;
	// Every event of every kind is reached through the names the game data uses for them.
	TArray<TSharedPtr<FGenIniBlock>> Blocks;
	for (const TCHAR* File : { TEXT("SoundEffects.ini"), TEXT("Voice.ini"), TEXT("Speech.ini") })
	{
		GenIni::ParseFile(FString(TEXT("Data\\INI\\")) + File, Blocks);
	}
	for (const TSharedPtr<FGenIniBlock>& B : Blocks)
	{
		const FGenAudioEventInfo* Info = Data.Find(B->Name());
		if (!Info || Info->SoundType == EGenAudioType::Music)
		{
			continue;
		}
		if (Info->SoundType == EGenAudioType::Streaming)
		{
			Paths.Add(Data.ResolvePath(Info->SoundType, Info->Filename));
		}
		for (const TArray<FString>* List : { &Info->Sounds, &Info->Attack, &Info->Decay })
		{
			for (const FString& Sound : *List) { Paths.Add(Data.ResolvePath(Info->SoundType, Sound)); }
		}
	}
	int32 Ok = 0, Missing = 0;
	OutFailed = 0;
	OutSeconds = 0.0;
	for (const FString& Path : Paths)
	{
		TArray<uint8> Bytes;
		FGenPcm Pcm;
		FString Error;
		if (!FGenFileSystem::Get().ReadFile(Path, Bytes))
		{
			++Missing;
			UE_CLOG(Missing <= 15, LogGenerals, Log, TEXT("Audio self test: missing %s"), *Path);
		}
		else if (!GenWav::Decode(Bytes, Pcm, &Error) || Pcm.Samples.Num() == 0)
		{
			++OutFailed;
			UE_CLOG(OutFailed <= 15, LogGenerals, Warning, TEXT("Audio self test: %s: %s"), *Path, *Error);
		}
		else
		{
			++Ok;
			OutSeconds += double(Pcm.GetDuration());
		}
	}
	UE_LOG(LogGenerals, Log, TEXT("Audio self test: %d events name %d files: %d decode (%.0f minutes of sound), %d fail, %d are not in the archives"),
		Data.NumEvents(), Paths.Num(), Ok, OutSeconds / 60.0, OutFailed, Missing);
	return Ok;
}

bool UGenAudioManager::DumpDecoded(const FString& SoundOrPath, const FString& OutWavPath)
{
	FGenAudioData& Data = FGenAudioData::Get();
	Data.Initialize();
	FString Path = SoundOrPath;
	if (!Path.Contains(TEXT("\\")) && !Path.Contains(TEXT("/")))
	{
		Path = Data.ResolvePath(EGenAudioType::SoundEffect, SoundOrPath);
		if (!FGenFileSystem::Get().Exists(Path))
		{
			Path = Data.ResolvePath(EGenAudioType::Streaming, SoundOrPath + TEXT(".wav"));
		}
	}
	TArray<uint8> Bytes;
	FGenPcm Pcm;
	if (!FGenFileSystem::Get().ReadFile(Path, Bytes) || !GenWav::Decode(Bytes, Pcm))
	{
		return false;
	}
	// A plain 16-bit PCM WAVE file.
	TArray<uint8> Out;
	auto U32 = [&Out](uint32 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 4); };
	auto U16 = [&Out](uint16 V) { Out.Append(reinterpret_cast<const uint8*>(&V), 2); };
	const uint32 DataBytes = uint32(Pcm.Samples.Num()) * 2;
	Out.Append(reinterpret_cast<const uint8*>("RIFF"), 4); U32(36 + DataBytes);
	Out.Append(reinterpret_cast<const uint8*>("WAVEfmt "), 8); U32(16); U16(1); U16(uint16(Pcm.NumChannels));
	U32(uint32(Pcm.SampleRate)); U32(uint32(Pcm.SampleRate * Pcm.NumChannels * 2)); U16(uint16(Pcm.NumChannels * 2)); U16(16);
	Out.Append(reinterpret_cast<const uint8*>("data"), 4); U32(DataBytes);
	Out.Append(reinterpret_cast<const uint8*>(Pcm.Samples.GetData()), int32(DataBytes));
	return FFileHelper::SaveArrayToFile(Out, *OutWavPath);
}
