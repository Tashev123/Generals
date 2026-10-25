#pragma once

#include "CoreMinimal.h"
#include "Subsystems/WorldSubsystem.h"
#include "Audio/GenAudioData.h"
#include "Audio/GenWav.h"
#include "Logic/GenLogicTypes.h"
#include "GenAudioManager.generated.h"

class AGenMapActor;
class FGenObject;
class UAudioComponent;
class UGenGameLogic;
class UGenMp3SoundWave;
class UGenPcmSoundWave;
class USoundEffectSubmixPreset;
struct FGenThingTemplate;

/**
 * Sound (GameAudio.cpp's AudioManager, GameSounds.cpp's SoundManager and the parts of
 * MilesAudioManager.cpp that decide what is heard): plays the original game's audio events from the
 * original files, by the original rules - who hears an event, how many of one kind may play at once,
 * what gives way when the channels run out, how loud a sound is at its distance from the microphone.
 * Plus the adviser (Eva.cpp) and the skirmish music.
 *
 * Everything here is client-side and runs in real time, whatever the simulation's speed.
 */
UCLASS()
class GENERALS1_API UGenAudioManager : public UWorldSubsystem
{
	GENERATED_BODY()

public:
	static UGenAudioManager* Get(const UObject* WorldContext);

	virtual bool ShouldCreateSubsystem(UObject* Outer) const override;
	virtual void Deinitialize() override;

	void Start(AGenMapActor* InMapActor, UGenGameLogic* InLogic);
	/** Once per rendered frame: the microphone, the volume of what is playing, what has finished, loops, the adviser, the music. */
	void Update(float DeltaSeconds);
	/** No new sounds while this is set (a network game catching up runs minutes of the match in a second: their
	 *  sounds would all come at once). The loops of what goes on are kept track of all the same. */
	void SetSuspended(bool bInSuspended) { bSuspended = bInSuspended; }
	bool bSuspended = false;

	// --- AudioManager::addAudioEvent ---
	/** A sound that belongs to an object: heard from where it is, by whoever the event is meant for. */
	uint32 PlayForObject(const FString& Event, const FGenObject& Obj);
	/** A sound at a place in the world. Player: whose sound it is (INDEX_NONE: nobody's). */
	uint32 PlayAt(const FString& Event, const FVector3f& Pos, int32 Player = INDEX_NONE);
	/** A sound that isn't in the world (interface, notifications). */
	uint32 PlayUI(const FString& Event, int32 Player = INDEX_NONE);
	void Stop(uint32 Handle);
	bool IsPlaying(uint32 Handle) const;

	/** One of an object template's own sounds ("VoiceSelect", "SoundMoveLoop", or an entry of its UnitSpecificSounds). */
	const FString& GetTemplateSound(const FGenThingTemplate& Template, const TCHAR* Key);

	/**
	 * The sound an object makes for as long as it is in some state (its engine while it moves, its hum):
	 * one per slot. A sound that loops for ever is retried every so often while it can't be heard; one
	 * that doesn't is played once. An empty event ends it (with its decay).
	 * With ExpireSeconds it ends by itself unless asked for again before then (a weapon firing in a stream),
	 * and each asking starts it again if it has run out.
	 */
	void SetObjectLoop(FGenObjectID Object, int32 Slot, const FString& Event, float ExpireSeconds = 0.f);
	void OnObjectDestroyed(FGenObjectID Object);

	/** Eva::setShouldPlay: the adviser says it, in the local side's voice, when its turn comes. */
	void Eva(const TCHAR* EventName);

	/** MusicManager: a track of Music.ini by name; an empty name stops the music. */
	void PlayMusic(const FString& Track);
	/** Before a game: the interface's sounds work and the menu's music plays, round and round. */
	void StartShell(AGenMapActor* InMapActor);
	/** One track, round and round, instead of the game's own (the score screen's). */
	void PlayTrackOnly(const FString& Track);

	/**
	 * The player's volumes (the options' audio page), 0..1 each, on top of the original's own: the music;
	 * sound effects (the world's and the interface's); the units' voices (Voice events: "an object says one
	 * thing at a time"); speech (the adviser, and dialogue streamed from Speech). Master scales them all.
	 * Kept for the program's whole run: every level's manager plays by them.
	 */
	static void SetUserVolumes(float Master, float Music, float Sound, float Voice, float Speech);
	/** Puts the volumes into effect on what plays now as well (the music and every voice), not only on what starts next. */
	void ApplyUserVolumes();

	// --- for tests ---
	int32 GetNumPlaying() const { return Voices.Num(); }
	/** Decodes every sound file an event refers to; logs what fails. Returns the number of files that decode. */
	int32 SelfTest(int32& OutFailed, double& OutSeconds);
	/** Decodes one sound file (a name from an event's Sounds list, or a path) and writes it out as plain 16-bit PCM. */
	bool DumpDecoded(const FString& SoundOrPath, const FString& OutWavPath);

private:
	struct FRequest
	{
		const FGenAudioEventInfo* Info = nullptr;
		FGenObjectID Object = GenInvalidObjectID;
		bool bHasPos = false;
		FVector3f Pos = FVector3f::ZeroVector;
		int32 Player = INDEX_NONE;
		bool bManagedLoop = false;      // an object's loop: plays until released
		double FireTime = 0.0;          // for delayed requests
	};

	struct FVoice
	{
		uint32 Handle = 0;
		const FGenAudioEventInfo* Info = nullptr;
		int32 Slot = INDEX_NONE;        // which audio component
		bool bPositional = false;
		bool bStream = false;           // speech
		bool bSpeech = false;           // the adviser's (played as an interface sound, heard at the speech volume)
		FVector3f Pos = FVector3f::ZeroVector;
		FGenObjectID Object = GenInvalidObjectID;
		float Volume = 1.f;             // the event's volume times this play's random shift
		float Pitch = 1.f;
		double StartTime = 0.0;
		double EndTime = 0.0;           // when to take it off (0: when the wave says it is finished)
		bool bLoop = false;
		bool bReleased = false;
		TWeakObjectPtr<UGenPcmSoundWave> Wave;
	};

	struct FObjectLoop
	{
		FString Event;
		uint32 Handle = 0;
		double NextTry = 0.0;
		double ExpireTime = 0.0;        // 0: lasts until it is changed
		bool bStarted = false;          // it has been asked for once (a sound that doesn't loop isn't asked for again)
		bool bWaited = false;           // a loop with a Delay: the wait before its next sound is over
	};

	struct FEvaPending
	{
		const FGenEvaEvent* Event = nullptr;
		double RequestTime = 0.0;
	};

	uint32 AddEvent(const FRequest& Request);
	uint32 StartVoice(const FRequest& Request);
	bool ShouldPlayLocally(const FGenAudioEventInfo& Info, int32 Player) const;
	bool IsPositional(const FRequest& Request) const;
	float GetEffectiveVolume(const FVoice& Voice) const;
	/** The player's volume for this voice's kind of sound (SetUserVolumes). */
	static float GetUserScale(const FVoice& Voice);
	/** How much of a voice goes to the left and to the right speaker. */
	void GetPan(const FVoice& Voice, float& OutLeft, float& OutRight) const;
	float GetRangeScale(const FGenAudioEventInfo& Info, const FVector3f& Pos) const;
	void StopVoiceAt(int32 Index);
	int32 FindVoice(uint32 Handle) const;
	int32 CountChannels(bool bPositional, bool bStream) const;
	/** The oldest playing voice of lower priority than the event, of the same kind of channel; INDEX_NONE if there is none. */
	int32 FindLowerPriority(const FGenAudioEventInfo& Info, bool bPositional, bool bStream) const;
	TSharedPtr<const FGenPcm> LoadSound(EGenAudioType Type, const FString& FileOrSound);
	int32 AcquireSlot();
	void UpdateMicrophone();
	void UpdateLoops();
	void UpdateEva();
	void UpdateMusic(float DeltaSeconds);

	UPROPERTY(Transient)
	TObjectPtr<AGenMapActor> MapActor;

	UPROPERTY(Transient)
	TObjectPtr<UGenGameLogic> Logic;

	/** One audio component per channel, reused; each play gets a sound wave of its own. */
	UPROPERTY(Transient)
	TArray<TObjectPtr<UAudioComponent>> Components;

	UPROPERTY(Transient)
	TArray<TObjectPtr<UGenPcmSoundWave>> SlotWaves;

	UPROPERTY(Transient)
	TObjectPtr<UAudioComponent> MusicComponent;

	UPROPERTY(Transient)
	TObjectPtr<UGenMp3SoundWave> MusicWave;

	/** Keeps a volley of cannons from running the output into full scale (the original's mixer simply clipped). */
	UPROPERTY(Transient)
	TObjectPtr<USoundEffectSubmixPreset> Limiter;

	/**
	 * Asked for from the simulation's thread (a shot, a loop begun or ended, an object gone, the adviser):
	 * kept until the next Update, which does it on the game thread - sound waves and audio components
	 * are the engine's, and its alone to touch.
	 */
	struct FSimAsk
	{
		enum EKind : uint8 { Event, Loop, Destroyed, Adviser };
		EKind Kind = Event;
		FRequest Request;
		FGenObjectID Object = GenInvalidObjectID;
		int32 Slot = 0;
		FString Name;
		float ExpireSeconds = 0.f;
	};
	TArray<FSimAsk> SimAsks;
	FCriticalSection SimAskLock;
	void DoSimAsks();

	TArray<int32> FreeSlots;
	TArray<FVoice> Voices;              // in the order they started
	TArray<FRequest> Delayed;
	TMap<uint64, FObjectLoop> Loops;    // key: object id and slot
	uint32 NextHandle = 1;
	double Now = 0.0;                   // seconds of real time since Start
	FRandomStream Random;               // GameAudioRandomValue: not the simulation's stream

	// The microphone (AudioManager::update)
	FVector3f MicPos = FVector3f::ZeroVector;
	FVector3f MicRight = FVector3f(1.f, 0.f, 0.f);  // towards the right-hand side of the screen, along the ground
	float ZoomVolume = 1.f;

	// Decoded sounds, most recently used last
	TMap<FString, TSharedPtr<const FGenPcm>> SoundCache;
	TArray<FString> SoundCacheOrder;
	int64 SoundCacheBytes = 0;
	TMap<FString, FString> TemplateSounds;

	// The adviser
	TArray<FEvaPending> EvaPending;
	TMap<FString, double> EvaNextAllowed;
	uint32 EvaHandle = 0;

	// Music
	FString MusicPrefix;                // "Game_USA_": the local side's tracks
	FString LoopTrack;                  // instead of those: this one, again and again
	int32 MusicTrackNumber = 0;
	double MusicNextStart = 0.0;
	FString MusicPlaying;
	float MusicBaseVolume = 1.f;        // the track's volume by the original's rules, before the player's

	bool bTrace = false;
	int32 NumPlayed = 0;
	/** Why sounds weren't played: too far, unseen, the object already speaking, too many of the kind, no channel, no slot. */
	enum ECull { CullDistance, CullShroud, CullVoice, CullLimit, CullChannels, CullOther, CullCount };
	int32 NumCulled[CullCount] = {};
};
