#include "Core/GenCompression.h"
#include "Misc/Compression.h"

bool GenCompression::IsCompressed(TConstArrayView<uint8> Data)
{
	if (Data.Num() < 8)
	{
		return false;
	}
	const uint8* D = Data.GetData();
	if (D[0] == 'E' && D[1] == 'A' && D[2] == 'R' && D[3] == 0)
	{
		return true;
	}
	return D[0] == 'Z' && D[1] == 'L' && D[2] >= '1' && D[2] <= '9' && D[3] == 0;
}

bool GenCompression::Decompress(TConstArrayView<uint8> Data, TArray<uint8>& Out)
{
	if (!IsCompressed(Data))
	{
		Out = TArray<uint8>(Data.GetData(), Data.Num());
		return true;
	}
	const uint8* D = Data.GetData();
	const int32 UncompressedSize = int32(uint32(D[4]) | (uint32(D[5]) << 8) | (uint32(D[6]) << 16) | (uint32(D[7]) << 24));
	const TConstArrayView<uint8> Payload(D + 8, Data.Num() - 8);

	if (D[0] == 'E')
	{
		return RefPackDecode(Payload, Out);
	}

	Out.SetNumUninitialized(UncompressedSize);
	return FCompression::UncompressMemory(NAME_Zlib, Out.GetData(), UncompressedSize, Payload.GetData(), Payload.Num());
}

bool GenCompression::RefPackDecode(TConstArrayView<uint8> Src, TArray<uint8>& Out)
{
	const uint8* S = Src.GetData();
	const uint8* End = S + Src.Num();
	if (Src.Num() < 5)
	{
		return false;
	}

	uint32 Type = (uint32(S[0]) << 8) | S[1];
	S += 2;
	uint32 ULen = 0;
	if (Type & 0x8000)
	{
		if (Type & 0x100) { S += 4; }
		ULen = (uint32(S[0]) << 24) | (uint32(S[1]) << 16) | (uint32(S[2]) << 8) | S[3];
		S += 4;
	}
	else
	{
		if (Type & 0x100) { S += 3; }
		ULen = (uint32(S[0]) << 16) | (uint32(S[1]) << 8) | S[2];
		S += 3;
	}

	Out.Reset();
	Out.SetNumUninitialized(ULen);
	uint8* D = Out.GetData();
	uint8* DEnd = D + ULen;

	auto Literal = [&](uint32 Run) -> bool
	{
		if (S + Run > End || D + Run > DEnd) { return false; }
		FMemory::Memcpy(D, S, Run);
		D += Run; S += Run;
		return true;
	};
	auto Copy = [&](int64 Distance, uint32 Run) -> bool
	{
		const uint8* Ref = D - 1 - Distance;
		if (Ref < Out.GetData() || D + Run > DEnd) { return false; }
		while (Run--) { *D++ = *Ref++; } // may overlap: byte by byte
		return true;
	};

	while (S < End)
	{
		const uint8 First = *S++;
		if (!(First & 0x80))
		{
			const uint8 Second = *S++;
			if (!Literal(First & 3)) { return false; }
			if (!Copy(((First & 0x60) << 3) + Second, ((First & 0x1C) >> 2) + 3)) { return false; }
			continue;
		}
		if (!(First & 0x40))
		{
			const uint8 Second = S[0], Third = S[1];
			S += 2;
			if (!Literal(Second >> 6)) { return false; }
			if (!Copy(((Second & 0x3F) << 8) + Third, (First & 0x3F) + 4)) { return false; }
			continue;
		}
		if (!(First & 0x20))
		{
			const uint8 Second = S[0], Third = S[1], Forth = S[2];
			S += 3;
			if (!Literal(First & 3)) { return false; }
			if (!Copy((((First & 0x10) >> 4) << 16) + (Second << 8) + Third, (((First & 0x0C) >> 2) << 8) + Forth + 5)) { return false; }
			continue;
		}
		const uint32 Run = ((First & 0x1F) << 2) + 4;
		if (Run <= 112)
		{
			if (!Literal(Run)) { return false; }
			continue;
		}
		Literal(First & 3); // end of stream (+0..3 literals)
		break;
	}
	return D == DEnd;
}
