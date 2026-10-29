#pragma once

#include "CoreMinimal.h"

/** SoundType bits (theSoundTypeNames): where a sound lives and who gets to hear it. */
namespace GenSoundType
{
	enum : uint32
	{
		UI = 0x0001,
		World = 0x0002,         // has a place in the world
		Shrouded = 0x0004,      // not heard where the fog of war covers it
		Global = 0x0008,        // heard from anywhere
		Voice = 0x0010,         // an object says one thing at a time
		Player = 0x0020,        // only its owner hears it
		Allies = 0x0040,
		Enemies = 0x0080,
		Everyone = 0x0100,
	};
}

/** AudioControl bits (theAudioControlNames). */
namespace GenAudioControl
{
	enum : uint32
	{
		Loop = 0x0001,
		Random = 0x0002,        // one of the sounds at random (otherwise they take turns)
		All = 0x0004,
		PostDelay = 0x0008,
		Interrupt = 0x0010,     // at its limit, a new one replaces the oldest instead of being dropped
	};
}

enum class EGenAudioPriority : uint8 { Lowest, Low, Normal, High, Critical };

enum class EGenAudioType : uint8
{
	Music,          // MusicTrack: a file under Tracks
	Streaming,      // DialogEvent: speech, a file under Speech
	SoundEffect,    // AudioEvent: one of a list of files under Sounds
};

/** AudioEventInfo: an AudioEvent of SoundEffects.ini / Voice.ini, a DialogEvent of Speech.ini or a MusicTrack. */
struct GENERALS1_API FGenAudioEventInfo
{
	FString Name;
	EGenAudioType SoundType = EGenAudioType::SoundEffect;
	FString Filename;                   // music and speech
	TArray<FString> Sounds;             // sound effects: file names without folder or extension
	TArray<FString> Attack;             // played once before a looping sound
	TArray<FString> Decay;              // ... and once after it
	float Volume = 1.f;
	float VolumeShift = 0.f;            // each play is quieter by a random share of up to this (negative)
	float PitchShiftMin = 1.f;
	float PitchShiftMax = 1.f;
	float DelayMin = 0.f;               // milliseconds before it starts
	float DelayMax = 0.f;
	int32 Limit = 0;                    // how many of this event may play at once (0: any number)
	int32 LoopCount = 0;                // for a looping sound: how many times round (0: until it is stopped)
	EGenAudioPriority Priority = EGenAudioPriority::Normal;
	uint32 Type = 0;                    // GenSoundType bits
	uint32 Control = 0;                 // GenAudioControl bits
	float MinRange = 0.f;               // full volume within this distance of the microphone
	float MaxRange = 0.f;               // silent beyond this
	mutable int32 LastIndex = -1;       // AudioEventRTS::m_playingAudioIndex: which of the sounds played last
};

/** AudioSettings.ini */
struct GENERALS1_API FGenAudioSettings
{
	FString AudioRoot = TEXT("Data\\Audio");
	FString SoundsFolder = TEXT("Sounds");
	FString MusicFolder = TEXT("Tracks");
	FString StreamingFolder = TEXT("Speech");
	FString SoundsExtension = TEXT("wav");
	FString Language = TEXT("English");     // the localized sub-folder (GetRegistryLanguage)
	int32 SampleCount2D = 4;
	int32 SampleCount3D = 25;
	int32 StreamCount = 3;
	float GlobalMinRange = 5000.f;
	float GlobalMaxRange = 500000.f;
	int32 TimeBetweenDrawableSoundsMS = 1000;
	float MinSampleVolume = 0.02f;
	float Relative2DVolume = -0.1f;
	float DefaultSoundVolume = 0.8f;
	float Default3DSoundVolume = 0.8f;
	float DefaultSpeechVolume = 0.7f;
	float DefaultMusicVolume = 0.55f;
	float MicrophoneDesiredHeightAboveTerrain = 50.f;
	float MicrophoneMaxPercentageBetweenGroundAndCamera = 0.333f;
	float ZoomMinDistance = 130.f;
	float ZoomMaxDistance = 425.f;
	float ZoomSoundVolumePercentageAmount = 0.2f;
};

/** EvaEvent: something the adviser says, in each side's own voice. */
struct GENERALS1_API FGenEvaEvent
{
	FString Name;
	int32 Priority = 1;                         // 0 never plays; higher goes first
	int32 TimeBetweenChecksMS = 0;              // not said again for this long
	int32 ExpirationTimeMS = 0;                 // if it can't be said within this time, it is dropped
	TMap<FString, TArray<FString>> SideSounds;  // side (lower case) -> dialog events
};

/** Registry of the audio INI files (GameAudio.cpp's AudioManager tables). */
class GENERALS1_API FGenAudioData
{
public:
	static FGenAudioData& Get();

	bool Initialize();

	/** Null for unknown names and for "NoSound". */
	const FGenAudioEventInfo* Find(const FString& Name) const;
	/** The event behind a MiscAudio.ini entry ("MoneyDepositSound"), or an empty string. */
	const FString& GetMisc(const TCHAR* Key) const;
	const FGenEvaEvent* FindEva(const FString& Name) const;
	const FGenAudioSettings& GetSettings() const { return Settings; }

	/** AudioEventRTS::generateFilenamePrefix + adjustForLocalization: the VFS path of a sound, the localized one if it exists. */
	FString ResolvePath(EGenAudioType Type, const FString& FileOrSound) const;

	int32 NumEvents() const { return Events.Num(); }

	/**
	 * AudioManager::getAudioLengthMS: how long an event's sound lasts, in seconds, from its file's header (the first
	 * of its sounds; 0 if there is none). Asked by the scripts from the simulation's thread too.
	 */
	float GetEventLengthSeconds(const FString& Name) const;
	/** ... of a file in the archives (WAV or MP3). */
	static float GetFileLengthSeconds(const FString& VfsPath);

private:
	mutable FCriticalSection LengthLock;
	mutable TMap<FString, float> LengthCache;  // key: lower-case event name
	bool bInitialized = false;
	FGenAudioSettings Settings;
	TMap<FString, FGenAudioEventInfo> Events;   // key: lower-case name
	TMap<FString, FString> Misc;                // key: lower-case field name
	TMap<FString, FGenEvaEvent> Eva;
};
