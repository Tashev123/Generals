#pragma once

#include "CoreMinimal.h"
#include "Audio/GenWav.h"
#include "Sound/SoundWaveProcedural.h"
#include <atomic>
#include "GenSoundWave.generated.h"

class FMP3Decoder;

/**
 * A sound decoded from the original game's files, handed to the mixer as it is asked for.
 *
 * It plays its parts one after the other: an optional attack, the sound itself (once, a number of
 * times, or over and over until Release), and an optional decay - the way the original's looping
 * sounds (engines, ambience) are put together from three files. The parts share one format.
 *
 * What goes to the mixer is always stereo: where the sound sits between the speakers is the game's own
 * doing (SetPan), as with the original's default "fast 2D positional" sound - not the engine's 3D
 * panner, whose result depends on the speakers it believes it has.
 */
UCLASS()
class GENERALS1_API UGenPcmSoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	UGenPcmSoundWave(const FObjectInitializer& ObjectInitializer);

	/**
	 * Parts in playing order. LoopStart..LoopEnd are the parts that repeat as a group (INDEX_NONE: none);
	 * LoopCount is how often they play (0: until Release). Call before playing.
	 */
	void Setup(const TArray<TSharedPtr<const FGenPcm>>& InParts, int32 InLoopStart, int32 InLoopEnd, int32 InLoopCount, bool bInMixToMono);

	/** How much of the sound goes to each speaker (0..1). Changes are eased in over the next block of sound. */
	void SetPan(float Left, float Right);

	/** A looping sound ends: on to the decay part (or silence) at once. */
	void Release();

	/** Everything has been handed to the mixer. */
	bool IsFinished() const { return bFinished.load(); }

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

private:
	TArray<TSharedPtr<const FGenPcm>> Parts;
	int32 LoopStart = INDEX_NONE;
	int32 LoopEnd = INDEX_NONE;
	// Read and written on the audio render thread once playing; the game thread only sets the flags.
	int32 PartIndex = 0;
	int32 Position = 0;             // in samples (not frames) within the current part
	int32 LoopsLeft = 0;            // 0: forever
	bool bMixToMono = false;        // a stereo file played from a place in the world: both sides, then panned
	float GainLeft = 0.f;
	float GainRight = 0.f;
	bool bGainsStarted = false;
	std::atomic<float> PanLeft{ 0.70710678f };
	std::atomic<float> PanRight{ 0.70710678f };
	std::atomic<bool> bReleased{ false };
	std::atomic<bool> bFinished{ false };
};

/** A music track: an MP3 file out of the game's archives, decoded frame by frame as it plays. */
UCLASS()
class GENERALS1_API UGenMp3SoundWave : public USoundWaveProcedural
{
	GENERATED_BODY()

public:
	UGenMp3SoundWave(const FObjectInitializer& ObjectInitializer);
	virtual void BeginDestroy() override;

	/** Takes the file's bytes. False if no MP3 frame is found in them. */
	bool SetupFromFile(TArray<uint8>&& InBytes);

	bool IsFinished() const { return bFinished.load(); }
	/** Seconds of sound handed to the mixer so far. */
	float GetPlayedSeconds() const { return SampleRate > 0 && NumChannels > 0 ? float(SamplesOut.load()) / float(SampleRate * NumChannels) : 0.f; }

	virtual int32 OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples) override;

private:
	TArray<uint8> Bytes;
	int32 ReadPos = 0;
	FMP3Decoder* Decoder = nullptr;
	TArray<int16> Pending;          // decoded but not yet handed over
	int32 PendingPos = 0;
	std::atomic<int64> SamplesOut{ 0 };
	std::atomic<bool> bFinished{ false };
};
