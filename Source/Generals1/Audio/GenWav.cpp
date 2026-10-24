#include "Audio/GenWav.h"

namespace
{
	constexpr uint16 FormatPcm = 0x0001;
	constexpr uint16 FormatImaAdpcm = 0x0011;   // WAVE_FORMAT_DVI_ADPCM

	const int32 ImaIndexTable[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
	const int32 ImaStepTable[89] = {
		7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143,
		157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411,
		1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493,
		10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767 };

	uint16 ReadU16(const uint8* P) { return uint16(P[0] | (P[1] << 8)); }
	uint32 ReadU32(const uint8* P) { return uint32(P[0]) | (uint32(P[1]) << 8) | (uint32(P[2]) << 16) | (uint32(P[3]) << 24); }

	/** One 4-bit code of an IMA ADPCM stream. */
	FORCEINLINE int16 ImaStep(uint8 Nibble, int32& Predictor, int32& Index)
	{
		const int32 Step = ImaStepTable[Index];
		int32 Diff = Step >> 3;
		if (Nibble & 1) { Diff += Step >> 2; }
		if (Nibble & 2) { Diff += Step >> 1; }
		if (Nibble & 4) { Diff += Step; }
		Predictor = FMath::Clamp(Predictor + ((Nibble & 8) ? -Diff : Diff), -32768, 32767);
		Index = FMath::Clamp(Index + ImaIndexTable[Nibble], 0, 88);
		return int16(Predictor);
	}

	/**
	 * Microsoft's IMA ADPCM layout: blocks of BlockAlign bytes; each starts with a 4-byte header per
	 * channel (first sample, step index), then 4-byte groups of codes, one group per channel in turn,
	 * low nibble first.
	 */
	void DecodeIma(const uint8* Data, int32 DataSize, int32 Channels, int32 BlockAlign, int32 TotalFrames, TArray<int16>& Out)
	{
		const int32 FramesPerBlock = (BlockAlign - 4 * Channels) * 2 / Channels + 1;
		const int32 NumBlocks = DataSize / BlockAlign + ((DataSize % BlockAlign) > 4 * Channels ? 1 : 0);
		const int32 MaxFrames = NumBlocks * FramesPerBlock;
		const int32 Frames = TotalFrames > 0 ? FMath::Min(TotalFrames, MaxFrames) : MaxFrames;
		Out.SetNumZeroed(Frames * Channels);
		int32 Written = 0;
		for (int32 Offset = 0; Offset + 4 * Channels <= DataSize && Written < Frames; Offset += BlockAlign)
		{
			const uint8* Block = Data + Offset;
			const int32 BlockSize = FMath::Min(BlockAlign, DataSize - Offset);
			int32 Predictor[2] = { 0, 0 }, Index[2] = { 0, 0 };
			for (int32 c = 0; c < Channels; ++c)
			{
				Predictor[c] = int16(ReadU16(Block + c * 4));
				Index[c] = FMath::Clamp(int32(Block[c * 4 + 2]), 0, 88);
				Out[Written * Channels + c] = int16(Predictor[c]);
			}
			++Written;
			const uint8* Codes = Block + 4 * Channels;
			const int32 NumGroups = (BlockSize - 4 * Channels) / (4 * Channels);
			for (int32 g = 0; g < NumGroups && Written < Frames; ++g)
			{
				const int32 FramesHere = FMath::Min(8, Frames - Written);
				for (int32 c = 0; c < Channels; ++c)
				{
					const uint8* P = Codes + (g * Channels + c) * 4;
					for (int32 i = 0; i < FramesHere; ++i)
					{
						const uint8 Nibble = (i & 1) ? (P[i >> 1] >> 4) : (P[i >> 1] & 0x0f);
						Out[(Written + i) * Channels + c] = ImaStep(Nibble, Predictor[c], Index[c]);
					}
				}
				Written += FramesHere;
			}
		}
		Out.SetNum(Written * Channels, EAllowShrinking::No);
	}
}

bool GenWav::Decode(TConstArrayView<uint8> Bytes, FGenPcm& Out, FString* OutError)
{
	auto Fail = [OutError](const TCHAR* Why) { if (OutError) { *OutError = Why; } return false; };
	const uint8* D = Bytes.GetData();
	const int32 Num = Bytes.Num();
	if (Num < 12 || FMemory::Memcmp(D, "RIFF", 4) != 0 || FMemory::Memcmp(D + 8, "WAVE", 4) != 0)
	{
		return Fail(TEXT("not a RIFF WAVE file"));
	}
	uint16 Format = 0, Channels = 0, BlockAlign = 0, Bits = 0;
	uint32 Rate = 0, FactFrames = 0;
	const uint8* Data = nullptr;
	int32 DataSize = 0;
	for (int32 Pos = 12; Pos + 8 <= Num;)
	{
		const uint8* Chunk = D + Pos;
		const int32 Size = int32(FMath::Min<uint32>(ReadU32(Chunk + 4), uint32(Num - Pos - 8)));
		if (FMemory::Memcmp(Chunk, "fmt ", 4) == 0 && Size >= 16)
		{
			Format = ReadU16(Chunk + 8);
			Channels = ReadU16(Chunk + 10);
			Rate = ReadU32(Chunk + 12);
			BlockAlign = ReadU16(Chunk + 20);
			Bits = ReadU16(Chunk + 22);
		}
		else if (FMemory::Memcmp(Chunk, "fact", 4) == 0 && Size >= 4)
		{
			FactFrames = ReadU32(Chunk + 8);
		}
		else if (FMemory::Memcmp(Chunk, "data", 4) == 0)
		{
			Data = Chunk + 8;
			DataSize = Size;
			break;
		}
		Pos += 8 + Size + (Size & 1);
	}
	if (!Data || Channels < 1 || Channels > 2 || Rate == 0)
	{
		return Fail(TEXT("no usable fmt / data chunk"));
	}
	Out.SampleRate = int32(Rate);
	Out.NumChannels = Channels;
	if (Format == FormatPcm && Bits == 16)
	{
		const int32 NumSamples = DataSize / 2;
		Out.Samples.SetNumUninitialized(NumSamples);
		for (int32 i = 0; i < NumSamples; ++i)
		{
			Out.Samples[i] = int16(ReadU16(Data + i * 2));
		}
		return true;
	}
	if (Format == FormatPcm && Bits == 8)
	{
		Out.Samples.SetNumUninitialized(DataSize);
		for (int32 i = 0; i < DataSize; ++i)
		{
			Out.Samples[i] = int16((int32(Data[i]) - 128) << 8);
		}
		return true;
	}
	if (Format == FormatImaAdpcm && Bits == 4 && BlockAlign > 4 * Channels)
	{
		DecodeIma(Data, DataSize, Channels, BlockAlign, int32(FactFrames), Out.Samples);
		return true;
	}
	return Fail(TEXT("unsupported sample format"));
}

void GenWav::Convert(const FGenPcm& In, int32 SampleRate, int32 NumChannels, FGenPcm& Out)
{
	Out.SampleRate = SampleRate;
	Out.NumChannels = NumChannels;
	const int32 InFrames = In.NumFrames();
	if (InFrames == 0 || In.SampleRate <= 0 || SampleRate <= 0)
	{
		Out.Samples.Reset();
		return;
	}
	const int32 OutFrames = int32(int64(InFrames) * SampleRate / In.SampleRate);
	Out.Samples.SetNumUninitialized(OutFrames * NumChannels);
	const double Step = double(In.SampleRate) / double(SampleRate);
	auto Sample = [&In](int32 Frame, int32 Channel)
	{
		return float(In.Samples[Frame * In.NumChannels + FMath::Min(Channel, In.NumChannels - 1)]);
	};
	for (int32 f = 0; f < OutFrames; ++f)
	{
		const double Src = f * Step;
		const int32 F0 = FMath::Min(int32(Src), InFrames - 1), F1 = FMath::Min(F0 + 1, InFrames - 1);
		const float T = float(Src - double(F0));
		for (int32 c = 0; c < NumChannels; ++c)
		{
			float V;
			if (NumChannels == 1 && In.NumChannels == 2)
			{
				V = 0.5f * (FMath::Lerp(Sample(F0, 0), Sample(F1, 0), T) + FMath::Lerp(Sample(F0, 1), Sample(F1, 1), T));
			}
			else
			{
				V = FMath::Lerp(Sample(F0, c), Sample(F1, c), T);
			}
			Out.Samples[f * NumChannels + c] = int16(FMath::Clamp(FMath::RoundToInt32(V), -32768, 32767));
		}
	}
}
