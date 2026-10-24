#include "Audio/GenSoundWave.h"

#if WITH_DR_LIBS_MP3
#include "dr_libs_mp3decoder.h"
#endif

// ---- decoded sounds -----------------------------------------------------------------------------

UGenPcmSoundWave::UGenPcmSoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 2;
	SetSampleRate(22050);
	Duration = INDEFINITELY_LOOPING_DURATION;
	SoundGroup = SOUNDGROUP_Default;
	bLooping = false;
	// The volume comes from the game's own range rules and may be nothing at all for a moment (an engine
	// loop leaving earshot): the mixer is not to drop the voice for that.
	VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
}

void UGenPcmSoundWave::Setup(const TArray<TSharedPtr<const FGenPcm>>& InParts, int32 InLoopStart, int32 InLoopEnd, int32 InLoopCount, bool bInMixToMono)
{
	ResetAudio();
	Parts = InParts;
	LoopStart = InLoopStart;
	LoopEnd = InLoopEnd;
	LoopsLeft = FMath::Max(InLoopCount, 0);
	bMixToMono = bInMixToMono;
	PartIndex = 0;
	Position = 0;
	bGainsStarted = false;
	bReleased = false;
	bFinished = Parts.Num() == 0;
	NumChannels = 2;
	if (Parts.Num() > 0 && Parts[0].IsValid())
	{
		SetSampleRate(Parts[0]->SampleRate);
	}
}

void UGenPcmSoundWave::SetPan(float Left, float Right)
{
	PanLeft = FMath::Clamp(Left, 0.f, 1.f);
	PanRight = FMath::Clamp(Right, 0.f, 1.f);
}

void UGenPcmSoundWave::Release()
{
	bReleased = true;
}

int32 UGenPcmSoundWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	// (Audio render thread.) Hand over up to NumSamples samples - two to a frame, left and right - moving
	// on through the parts.
	const int32 NumFrames = NumSamples / 2;
	OutAudio.Reset();
	OutAudio.AddUninitialized(NumFrames * 2 * int32(sizeof(int16)));
	int16* Dst = reinterpret_cast<int16*>(OutAudio.GetData());

	// The pan moves to where the game last put it over the length of this block (no clicks).
	const float TargetLeft = PanLeft.load();
	const float TargetRight = PanRight.load();
	if (!bGainsStarted)
	{
		GainLeft = TargetLeft;
		GainRight = TargetRight;
		bGainsStarted = true;
	}
	const float StepLeft = (TargetLeft - GainLeft) / float(FMath::Max(NumFrames, 1));
	const float StepRight = (TargetRight - GainRight) / float(FMath::Max(NumFrames, 1));

	int32 Written = 0; // frames
	while (Written < NumFrames && PartIndex < Parts.Num())
	{
		const bool bInLoop = LoopStart != INDEX_NONE && PartIndex >= LoopStart && PartIndex <= LoopEnd;
		if (bInLoop && bReleased.load())
		{
			// The loop is over: straight on to the decay.
			PartIndex = LoopEnd + 1;
			Position = 0;
			continue;
		}
		const FGenPcm* Part = Parts[PartIndex].Get();
		const int32 Channels = Part ? FMath::Max(Part->NumChannels, 1) : 1;
		const int32 Available = Part ? (Part->Samples.Num() - Position) / Channels : 0;
		if (Available <= 0)
		{
			Position = 0;
			if (bInLoop && PartIndex == LoopEnd && (LoopsLeft == 0 || --LoopsLeft > 0))
			{
				// Once more round - unless there is nothing in the loop to play (never spin on empty parts).
				bool bAnything = false;
				for (int32 i = LoopStart; i <= LoopEnd; ++i) { bAnything |= Parts[i].IsValid() && Parts[i]->Samples.Num() > 0; }
				if (bAnything)
				{
					PartIndex = LoopStart;
					continue;
				}
			}
			++PartIndex;
			continue;
		}
		const int32 Count = FMath::Min(Available, NumFrames - Written);
		const int16* Src = Part->Samples.GetData() + Position;
		for (int32 i = 0; i < Count; ++i, Src += Channels)
		{
			float Left = float(Src[0]);
			float Right = Channels > 1 ? float(Src[1]) : Left;
			if (bMixToMono && Channels > 1)
			{
				Left = Right = 0.5f * (Left + Right);
			}
			GainLeft += StepLeft;
			GainRight += StepRight;
			*Dst++ = int16(FMath::Clamp(FMath::RoundToInt32(Left * GainLeft), -32768, 32767));
			*Dst++ = int16(FMath::Clamp(FMath::RoundToInt32(Right * GainRight), -32768, 32767));
		}
		Position += Count * Channels;
		Written += Count;
	}
	GainLeft = TargetLeft;
	GainRight = TargetRight;
	if (PartIndex >= Parts.Num())
	{
		bFinished = true;
	}
	// Only what was written stays in the buffer. (USoundWaveProcedural::GeneratePCMData trims the buffer to what
	// we report only when that is more than nothing; with nothing written it played the whole buffer as it
	// stood - the last block over again, a stutter at the end of every sound.)
	OutAudio.SetNum(Written * 2 * int32(sizeof(int16)), EAllowShrinking::No);
	return Written * 2;
}

// ---- music --------------------------------------------------------------------------------------

UGenMp3SoundWave::UGenMp3SoundWave(const FObjectInitializer& ObjectInitializer)
	: Super(ObjectInitializer)
{
	NumChannels = 2;
	SetSampleRate(44100);
	Duration = INDEFINITELY_LOOPING_DURATION;
	SoundGroup = SOUNDGROUP_Music;
	bLooping = false;
	VirtualizationMode = EVirtualizationMode::PlayWhenSilent;
}

void UGenMp3SoundWave::BeginDestroy()
{
#if WITH_DR_LIBS_MP3
	delete Decoder;
#endif
	Decoder = nullptr;
	Super::BeginDestroy();
}

bool UGenMp3SoundWave::SetupFromFile(TArray<uint8>&& InBytes)
{
#if WITH_DR_LIBS_MP3
	Bytes = MoveTemp(InBytes);
	ReadPos = 0;
	Pending.Reset();
	PendingPos = 0;
	SamplesOut = 0;
	bFinished = false;
	delete Decoder;
	Decoder = new FMP3Decoder();

	// The first frame tells the format (it also gets past an ID3 tag at the head of the file).
	TArray<float> Frame;
	Frame.SetNumUninitialized(1152 * 2);
	while (ReadPos < Bytes.Num())
	{
		FMP3Decoder::FFrameInfo Info;
		const int32 Frames = Decoder->Decode(&Info, Frame.GetData(), Frame.Num() * int32(sizeof(float)), Bytes.GetData() + ReadPos, FMath::Min(Bytes.Num() - ReadPos, 64 * 1024));
		if (Frames > 0 && Info.NumChannels > 0 && Info.SampleRate > 0)
		{
			NumChannels = Info.NumChannels;
			SetSampleRate(Info.SampleRate);
			// Start over from this frame, so that nothing of it is lost.
			Decoder->Reset();
			return true;
		}
		if (Info.NumFrameBytes <= 0)
		{
			break;
		}
		ReadPos += Info.NumFrameBytes;
	}
#endif
	bFinished = true;
	return false;
}

int32 UGenMp3SoundWave::OnGeneratePCMAudio(TArray<uint8>& OutAudio, int32 NumSamples)
{
	OutAudio.Reset();
	OutAudio.AddUninitialized(NumSamples * int32(sizeof(int16)));
	int16* Dst = reinterpret_cast<int16*>(OutAudio.GetData());
	int32 Written = 0;
#if WITH_DR_LIBS_MP3
	float Frame[1152 * 2];
	while (Written < NumSamples)
	{
		if (PendingPos < Pending.Num())
		{
			const int32 Count = FMath::Min(Pending.Num() - PendingPos, NumSamples - Written);
			FMemory::Memcpy(Dst + Written, Pending.GetData() + PendingPos, Count * sizeof(int16));
			PendingPos += Count;
			Written += Count;
			continue;
		}
		if (!Decoder || ReadPos >= Bytes.Num())
		{
			break;
		}
		FMP3Decoder::FFrameInfo Info;
		const int32 Frames = Decoder->Decode(&Info, Frame, int32(sizeof(Frame)), Bytes.GetData() + ReadPos, FMath::Min(Bytes.Num() - ReadPos, 64 * 1024));
		if (Info.NumFrameBytes <= 0)
		{
			ReadPos = Bytes.Num(); // nothing more to be found
			break;
		}
		ReadPos += Info.NumFrameBytes;
		if (Frames > 0 && Info.NumChannels == NumChannels)
		{
			const int32 Count = Frames * NumChannels;
			Pending.SetNumUninitialized(Count, EAllowShrinking::No);
			for (int32 i = 0; i < Count; ++i)
			{
				Pending[i] = int16(FMath::Clamp(FMath::RoundToInt32(Frame[i] * 32767.f), -32768, 32767));
			}
			PendingPos = 0;
		}
	}
#endif
	SamplesOut += Written;
	if (Written == 0 && ReadPos >= Bytes.Num())
	{
		bFinished = true;
	}
	OutAudio.SetNum(Written * int32(sizeof(int16)), EAllowShrinking::No); // (as above: nothing stale)
	return Written;
}
