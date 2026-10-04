#pragma once

#include "CoreMinimal.h"
#include "Map/GenMapData.h"

/**
 * Reader for the "CkMp" data chunk format (DataChunkInput): maps (.map) and script files (.scb).
 * A file starts with a table of names (chunk labels and dict keys), followed by nested chunks of
 * <name id, version, size>.
 */
class FGenChunkReader
{
public:
	FGenChunkReader(TConstArrayView<uint8> InData, const TMap<uint32, FString>& InNames) : Data(InData), Names(InNames) {}

	/** Checks the "CkMp" tag and reads the name table; returns the offset of the first chunk (0 on failure). */
	static int32 ReadNameTable(TConstArrayView<uint8> InData, TMap<uint32, FString>& OutNames)
	{
		if (InData.Num() < 8 || FMemory::Memcmp(InData.GetData(), "CkMp", 4) != 0)
		{
			return 0;
		}
		int32 At = 4;
		int32 Count = 0;
		FMemory::Memcpy(&Count, &InData[At], 4);
		At += 4;
		for (int32 i = 0; i < Count && At < InData.Num(); ++i)
		{
			const int32 Len = InData[At++];
			FString Name;
			for (int32 c = 0; c < Len && At < InData.Num(); ++c) { Name.AppendChar(TCHAR(InData[At++])); }
			uint32 Id = 0;
			if (At + 4 <= InData.Num()) { FMemory::Memcpy(&Id, &InData[At], 4); }
			At += 4;
			OutNames.Add(Id, Name);
		}
		return At;
	}

	bool Has(int32 N) const { return Pos >= 0 && Pos + N <= Data.Num(); }
	int32 I32() { int32 V = 0; if (Has(4)) { FMemory::Memcpy(&V, &Data[Pos], 4); } Pos += 4; return V; }
	uint32 U32() { return uint32(I32()); }
	uint16 U16() { uint16 V = 0; if (Has(2)) { FMemory::Memcpy(&V, &Data[Pos], 2); } Pos += 2; return V; }
	uint8 U8() { const uint8 V = Has(1) ? Data[Pos] : 0; Pos += 1; return V; }
	float F32() { float V = 0; if (Has(4)) { FMemory::Memcpy(&V, &Data[Pos], 4); } Pos += 4; return V; }
	FVector3f Vec3() { const float X = F32(); const float Y = F32(); const float Z = F32(); return FVector3f(X, Y, Z); }

	FString AStr()
	{
		const int32 N = U16();
		FString S;
		S.Reserve(N);
		for (int32 i = 0; i < N && Has(1); ++i) { S.AppendChar(TCHAR(Data[Pos])); ++Pos; }
		return S;
	}

	FString UStr()
	{
		const int32 N = U16();
		FString S;
		S.Reserve(N);
		for (int32 i = 0; i < N && Has(2); ++i) { S.AppendChar(TCHAR(Data[Pos] | (Data[Pos + 1] << 8))); Pos += 2; }
		return S;
	}

	/** DataChunkInput::readNameKey: an id into the name table, stored like a dict key (id << 8 | type). */
	FString NameKey()
	{
		const FString* N = Names.Find(uint32(I32()) >> 8);
		return N ? *N : FString();
	}

	FGenDict Dict()
	{
		FGenDict D;
		const int32 N = U16();
		for (int32 i = 0; i < N && Has(4); ++i)
		{
			const int32 KeyAndType = I32();
			const int32 Type = KeyAndType & 0xFF;
			const FString* KeyName = Names.Find(uint32(KeyAndType) >> 8);
			FGenDictValue V;
			switch (Type)
			{
			case 0: V.Type = FGenDictValue::EType::Bool; V.Bool = U8() != 0; break;
			case 1: V.Type = FGenDictValue::EType::Int; V.Int = I32(); break;
			case 2: V.Type = FGenDictValue::EType::Real; V.Real = F32(); break;
			case 3: V.Type = FGenDictValue::EType::AsciiString; V.String = AStr(); break;
			case 4: V.Type = FGenDictValue::EType::UnicodeString; V.String = UStr(); break;
			default: return D; // corrupt
			}
			D.Add(KeyName ? *KeyName : FString::Printf(TEXT("#%d"), uint32(KeyAndType) >> 8), MoveTemp(V));
		}
		return D;
	}

	template<typename T>
	void Array(TArray<T>& Out, int32 Count)
	{
		Out.SetNumZeroed(Count);
		const int32 Bytes = Count * int32(sizeof(T));
		if (Has(Bytes)) { FMemory::Memcpy(Out.GetData(), &Data[Pos], Bytes); }
		Pos += Bytes;
	}

	struct FChunk { FString Name; uint16 Version; int32 Start; int32 End; };

	bool NextChunk(int32 End, FChunk& Out)
	{
		if (Pos + 10 > End) { return false; }
		const uint32 Id = U32();
		Out.Version = U16();
		const int32 Size = I32();
		Out.Start = Pos;
		Out.End = Pos + Size;
		const FString* N = Names.Find(Id);
		Out.Name = N ? *N : FString();
		return Size >= 0 && Out.End <= End;
	}

	TConstArrayView<uint8> Data;
	const TMap<uint32, FString>& Names;
	int32 Pos = 0;
};
