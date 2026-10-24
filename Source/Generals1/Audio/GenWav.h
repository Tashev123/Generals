#pragma once

#include "CoreMinimal.h"

/** Decoded sound: 16-bit samples, interleaved when stereo. */
struct GENERALS1_API FGenPcm
{
	int32 SampleRate = 22050;
	int32 NumChannels = 1;
	TArray<int16> Samples;

	int32 NumFrames() const { return NumChannels > 0 ? Samples.Num() / NumChannels : 0; }
	float GetDuration() const { return SampleRate > 0 ? float(NumFrames()) / float(SampleRate) : 0.f; }
};

namespace GenWav
{
	/**
	 * Decodes a RIFF WAVE file as the game ships them: 16-bit (or 8-bit) PCM and IMA ADPCM
	 * (WAVE_FORMAT_DVI_ADPCM), mono or stereo. False for anything else.
	 */
	GENERALS1_API bool Decode(TConstArrayView<uint8> Bytes, FGenPcm& Out, FString* OutError = nullptr);

	/** The same sound at another rate and channel count (linear interpolation; for joining the parts of a looping sound). */
	GENERALS1_API void Convert(const FGenPcm& In, int32 SampleRate, int32 NumChannels, FGenPcm& Out);
}
