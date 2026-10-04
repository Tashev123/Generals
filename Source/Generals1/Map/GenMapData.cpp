#include "Map/GenMapData.h"
#include "Core/GenCompression.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "Map/GenDataChunk.h"
#include "Generals1.h"

namespace
{
	using FChunkReader = FGenChunkReader;

	// (Not "RGB": that is a Windows macro, and unity builds can put a Windows header in front of this file.)
	FLinearColor ReadColor3(FChunkReader& R) { const float X = R.F32(); const float Y = R.F32(); const float Z = R.F32(); return FLinearColor(X, Y, Z); }

	FGenMapLight ReadLight(FChunkReader& R)
	{
		FGenMapLight L;
		L.Ambient = ReadColor3(R);
		L.Diffuse = ReadColor3(R);
		L.Direction = R.Vec3();
		return L;
	}
}

FString FGenMapObject::GetString(const TCHAR* Key) const
{
	const FGenDictValue* V = Properties.Find(Key);
	return V ? V->String : FString();
}

int32 FGenMapObject::GetInt(const TCHAR* Key, int32 Default) const
{
	const FGenDictValue* V = Properties.Find(Key);
	return V ? V->Int : Default;
}

bool FGenMapData::LoadFromVfs(const FString& MapPath)
{
	TArray<uint8> Bytes;
	if (!FGenFileSystem::Get().ReadFile(MapPath, Bytes))
	{
		UE_LOG(LogGenerals, Error, TEXT("Map '%s' not found"), *MapPath);
		return false;
	}
	Path = MapPath;
	return LoadFromMemory(Bytes);
}

bool FGenMapData::LoadFromMemory(TConstArrayView<uint8> InData)
{
	TArray<uint8> Data;
	TMap<uint32, FString> Names;
	const int32 Pos = GenCompression::Decompress(InData, Data) ? FGenChunkReader::ReadNameTable(Data, Names) : 0;
	if (Pos == 0)
	{
		UE_LOG(LogGenerals, Error, TEXT("Map '%s': not a CkMp file"), *Path);
		return false;
	}

	FChunkReader R(Data, Names);
	R.Pos = Pos;
	FChunkReader::FChunk C;
	while (R.NextChunk(Data.Num(), C))
	{
		if (C.Name == TEXT("HeightMapData"))
		{
			Width = R.I32();
			Height = R.I32();
			Border = C.Version >= 3 ? R.I32() : 0;
			if (C.Version >= 4)
			{
				const int32 N = R.I32();
				for (int32 i = 0; i < N; ++i) { const int32 X = R.I32(); const int32 Y = R.I32(); Boundaries.Add(FIntPoint(X, Y)); }
			}
			else
			{
				Boundaries.Add(FIntPoint(Width - 2 * Border, Height - 2 * Border));
			}
			const int32 Size = R.I32();
			if (Size != Width * Height)
			{
				UE_LOG(LogGenerals, Error, TEXT("Map '%s': bad height map size"), *Path);
				return false;
			}
			R.Array(Heights, Size);
		}
		else if (C.Name == TEXT("BlendTileData"))
		{
			const int32 N = R.I32();
			R.Array(TileNdx, N);
			R.Array(BlendNdx, N);
			if (C.Version >= 6) { R.Array(ExtraBlendNdx, N); } else { ExtraBlendNdx.SetNumZeroed(N); }
			if (C.Version >= 5) { R.Array(CliffNdx, N); } else { CliffNdx.SetNumZeroed(N); }
			CliffStride = (Width + 7) / 8;
			CliffState.SetNumZeroed(CliffStride * Height);
			if (C.Version >= 7)
			{
				// Painted passability (cliff) flags. Version 7 was saved with a too-short row length.
				const int32 ByteW = C.Version == 7 ? (Width + 1) / 8 : CliffStride;
				for (int32 Y = 0; Y < Height; ++Y)
				{
					for (int32 X = 0; X < ByteW; ++X)
					{
						CliffState[Y * CliffStride + X] = R.U8();
					}
				}
			}
			else
			{
				InitCliffFlagsFromHeights();
			}
			/*NumBitmapTiles*/ R.I32();
			const int32 NumBlended = R.I32();
			const int32 NumCliff = C.Version >= 5 ? R.I32() : 1;
			const int32 NumClasses = R.I32();
			for (int32 i = 0; i < NumClasses; ++i)
			{
				FGenTextureClass& T = TextureClasses.AddDefaulted_GetRef();
				T.FirstTile = R.I32(); T.NumTiles = R.I32(); T.Width = R.I32(); /*legacy*/ R.I32();
				T.Name = R.AStr();
			}
			if (C.Version >= 4)
			{
				/*NumEdgeTiles*/ R.I32();
				const int32 NumEdge = R.I32();
				for (int32 i = 0; i < NumEdge; ++i)
				{
					FGenTextureClass& T = EdgeTextureClasses.AddDefaulted_GetRef();
					T.FirstTile = R.I32(); T.NumTiles = R.I32(); T.Width = R.I32();
					T.Name = R.AStr();
				}
			}
			BlendedTiles.SetNum(1);
			for (int32 i = 1; i < NumBlended; ++i)
			{
				FGenBlendedTile& B = BlendedTiles.AddDefaulted_GetRef();
				B.BlendNdx = R.I32();
				B.Horiz = R.U8(); B.Vert = R.U8(); B.RightDiagonal = R.U8(); B.LeftDiagonal = R.U8(); B.Inverted = R.U8();
				B.LongDiagonal = C.Version >= 3 ? R.U8() : 0;
				B.CustomBlendEdgeClass = C.Version >= 4 ? R.I32() : -1;
				if (uint32(R.I32()) != 0x7ADA0000u)
				{
					UE_LOG(LogGenerals, Error, TEXT("Map '%s': bad blend tile flag"), *Path);
					return false;
				}
			}
			// Cliff UV info (version 5+) follows; not needed until cliffs get custom UVs.
			(void)NumCliff;
		}
		else if (C.Name == TEXT("ObjectsList"))
		{
			FChunkReader::FChunk OC;
			while (R.NextChunk(C.End, OC))
			{
				if (OC.Name == TEXT("Object"))
				{
					FGenMapObject& O = Objects.AddDefaulted_GetRef();
					O.Location = R.Vec3();
					O.Angle = R.F32();
					O.Flags = R.I32();
					O.TemplateName = R.AStr();
					if (OC.Version >= 2) { O.Properties = R.Dict(); }
					if (OC.Version <= 2) { O.Location.Z = 0.f; }
				}
				R.Pos = OC.End;
			}
		}
		else if (C.Name == TEXT("WorldInfo"))
		{
			WorldInfo = R.Dict();
		}
		else if (C.Name == TEXT("PolygonTriggers"))
		{
			const int32 NumTriggers = R.I32();
			for (int32 i = 0; i < NumTriggers && R.Pos < C.End; ++i)
			{
				FGenPolygonTrigger T;
				T.Name = R.AStr();
				if (C.Version >= 4) { R.AStr(); } // layer name
				T.ID = R.I32();
				if (C.Version >= 2) { T.bWater = R.U8() != 0; }
				if (C.Version >= 3) { T.bRiver = R.U8() != 0; R.I32(); }
				const int32 NumPoints = R.I32();
				for (int32 p = 0; p < NumPoints; ++p)
				{
					const int32 X = R.I32(); const int32 Y = R.I32(); const int32 Z = R.I32();
					T.Points.Add(FIntVector(X, Y, Z));
				}
				if (NumPoints >= 2)
				{
					PolygonTriggers.Add(MoveTemp(T));
				}
			}
		}
		else if (C.Name == TEXT("WaypointsList"))
		{
			const int32 NumLinks = R.I32();
			for (int32 i = 0; i < NumLinks && R.Pos < C.End; ++i)
			{
				const int32 From = R.I32(); const int32 To = R.I32();
				WaypointLinks.Add(FIntPoint(From, To));
			}
		}
		else if (C.Name == TEXT("SidesList"))
		{
			const int32 NumSides = R.I32();
			for (int32 i = 0; i < NumSides; ++i)
			{
				FGenMapSide& S = Sides.AddDefaulted_GetRef();
				S.Properties = R.Dict();
				const int32 NumBuild = R.I32();
				for (int32 b = 0; b < NumBuild; ++b)
				{
					FGenMapSide::FBuild& B = S.BuildList.AddDefaulted_GetRef();
					B.BuildingName = R.AStr();
					B.TemplateName = R.AStr();
					B.Location = R.Vec3();
					B.Angle = R.F32();
					B.bInitiallyBuilt = R.U8() != 0;
					B.NumRebuilds = R.I32();
					if (C.Version >= 3)
					{
						R.AStr(); R.I32(); R.U8(); R.U8(); R.U8(); // script, health, whiner, unsellable, repairable
					}
				}
			}
			if (C.Version >= 2)
			{
				const int32 NumTeams = R.I32();
				for (int32 i = 0; i < NumTeams; ++i) { Teams.Add(R.Dict()); }
			}
		}
		else if (C.Name == TEXT("GlobalLighting"))
		{
			TimeOfDay = R.I32();
			Lighting.SetNum(4);
			for (int32 t = 0; t < 4; ++t)
			{
				FGenMapLighting& L = Lighting[t];
				L.Terrain.Add(ReadLight(R));
				L.Objects.Add(ReadLight(R));
				if (C.Version >= 2) { L.Objects.Add(ReadLight(R)); L.Objects.Add(ReadLight(R)); }
				if (C.Version >= 3) { L.Terrain.Add(ReadLight(R)); L.Terrain.Add(ReadLight(R)); }
			}
		}
		R.Pos = C.End;
	}

	if (Width <= 0 || Height <= 0)
	{
		UE_LOG(LogGenerals, Error, TEXT("Map '%s' has no height map"), *Path);
		return false;
	}
	return true;
}

float FGenMapData::GetHeightAt(int32 X, int32 Y) const
{
	X = FMath::Clamp(X, 0, Width - 1);
	Y = FMath::Clamp(Y, 0, Height - 1);
	return float(Heights[Y * Width + X]) * GenCoords::MapHeightScale;
}

float FGenMapData::GetGroundHeight(float WorldX, float WorldY) const
{
	const float FX = WorldX / GenCoords::MapXYFactor + float(Border);
	const float FY = WorldY / GenCoords::MapXYFactor + float(Border);
	const int32 X0 = FMath::FloorToInt32(FX);
	const int32 Y0 = FMath::FloorToInt32(FY);
	const float TX = FX - float(X0);
	const float TY = FY - float(Y0);
	const float H00 = GetHeightAt(X0, Y0), H10 = GetHeightAt(X0 + 1, Y0);
	const float H01 = GetHeightAt(X0, Y0 + 1), H11 = GetHeightAt(X0 + 1, Y0 + 1);
	return FMath::Lerp(FMath::Lerp(H00, H10, TX), FMath::Lerp(H01, H11, TX), TY);
}

FBox2f FGenMapData::GetPlayableBounds() const
{
	const FIntPoint B = Boundaries.Num() > 0 ? Boundaries[0] : FIntPoint(Width - 2 * Border, Height - 2 * Border);
	return FBox2f(FVector2f(0.f, 0.f), FVector2f(float(B.X), float(B.Y)) * GenCoords::MapXYFactor);
}

const FGenMapObject* FGenMapData::FindWaypoint(const FString& Name) const
{
	for (const FGenMapObject& O : Objects)
	{
		if (O.IsWaypoint() && O.GetString(TEXT("waypointName")).Equals(Name, ESearchCase::IgnoreCase))
		{
			return &O;
		}
	}
	return nullptr;
}

const FGenPolygonTrigger* FGenMapData::FindTrigger(const FString& Name) const
{
	for (const FGenPolygonTrigger& T : PolygonTriggers)
	{
		if (T.Name.Equals(Name, ESearchCase::CaseSensitive))
		{
			return &T;
		}
	}
	return nullptr;
}

const FGenMapObject* FGenMapData::FindWaypointByID(int32 Id) const
{
	for (const FGenMapObject& O : Objects)
	{
		if (O.IsWaypoint() && O.GetInt(TEXT("waypointID"), -1) == Id)
		{
			return &O;
		}
	}
	return nullptr;
}

const FGenMapObject* FGenMapData::GetClosestWaypointOnPath(const FVector2f& Pos, const FString& Label) const
{
	const FGenMapObject* Best = nullptr;
	float BestDistSq = 0.f;
	if (Label.IsEmpty())
	{
		return nullptr;
	}
	for (const FGenMapObject& O : Objects)
	{
		if (!O.IsWaypoint())
		{
			continue;
		}
		const bool bMatch = O.GetString(TEXT("waypointPathLabel1")).Equals(Label, ESearchCase::IgnoreCase)
			|| O.GetString(TEXT("waypointPathLabel2")).Equals(Label, ESearchCase::IgnoreCase)
			|| O.GetString(TEXT("waypointPathLabel3")).Equals(Label, ESearchCase::IgnoreCase);
		const float DistSq = FVector2f::DistSquared(Pos, FVector2f(O.Location.X, O.Location.Y));
		if (bMatch && (!Best || DistSq < BestDistSq))
		{
			Best = &O;
			BestDistSq = DistSq;
		}
	}
	return Best;
}

void FGenMapData::GetWaypointPath(const FGenMapObject& Start, TArray<FVector2f>& OutPath) const
{
	const FGenMapObject* Cur = &Start;
	TSet<int32> Seen;
	while (Cur)
	{
		const int32 Id = Cur->GetInt(TEXT("waypointID"), -1);
		if (Seen.Contains(Id))
		{
			break; // a loop
		}
		Seen.Add(Id);
		OutPath.Add(FVector2f(Cur->Location.X, Cur->Location.Y));
		const FGenMapObject* Next = nullptr;
		for (const FIntPoint& L : WaypointLinks)
		{
			if (L.X == Id)
			{
				Next = FindWaypointByID(L.Y);
				break;
			}
		}
		Cur = Next;
	}
}

const FGenMapLighting* FGenMapData::GetCurrentLighting() const
{
	const int32 Index = FMath::Clamp(TimeOfDay - 1, 0, 3);
	return Lighting.IsValidIndex(Index) ? &Lighting[Index] : nullptr;
}

bool FGenPolygonTrigger::Contains(float X, float Y) const
{
	// Even-odd rule (PolygonTrigger::pointInTrigger).
	bool bInside = false;
	const int32 N = Points.Num();
	for (int32 i = 0, j = N - 1; i < N; j = i++)
	{
		const float Xi = float(Points[i].X), Yi = float(Points[i].Y);
		const float Xj = float(Points[j].X), Yj = float(Points[j].Y);
		if (((Yi > Y) != (Yj > Y)) && (X < (Xj - Xi) * (Y - Yi) / (Yj - Yi) + Xi))
		{
			bInside = !bInside;
		}
	}
	return bInside;
}

void FGenMapData::InitCliffFlagsFromHeights()
{
	// WorldHeightMap::setCellCliffFlagFromHeights: height range within a cell > 9.8 is a cliff.
	for (int32 Y = 0; Y < Height - 1; ++Y)
	{
		for (int32 X = 0; X < Width - 1; ++X)
		{
			const float H[4] = { GetHeightAt(X, Y), GetHeightAt(X + 1, Y), GetHeightAt(X, Y + 1), GetHeightAt(X + 1, Y + 1) };
			const float MinZ = FMath::Min(FMath::Min(H[0], H[1]), FMath::Min(H[2], H[3]));
			const float MaxZ = FMath::Max(FMath::Max(H[0], H[1]), FMath::Max(H[2], H[3]));
			if (MaxZ - MinZ > 9.8f)
			{
				CliffState[Y * CliffStride + (X >> 3)] |= uint8(1 << (X & 7));
			}
		}
	}
}

bool FGenMapData::IsCliffCellIndex(int32 X, int32 Y) const
{
	if (X < 0 || Y < 0 || X >= Width || Y >= Height || CliffState.Num() == 0)
	{
		return false;
	}
	return (CliffState[Y * CliffStride + (X >> 3)] & (1 << (X & 7))) != 0;
}

bool FGenMapData::IsUnderwater(float WorldX, float WorldY, float* OutWaterZ) const
{
	float BestZ = -FLT_MAX;
	for (const FGenPolygonTrigger& T : PolygonTriggers)
	{
		if (T.bWater && T.Contains(WorldX, WorldY))
		{
			BestZ = FMath::Max(BestZ, T.GetWaterHeight());
		}
	}
	if (BestZ == -FLT_MAX)
	{
		return false;
	}
	if (OutWaterZ) { *OutWaterZ = BestZ; }
	return GetGroundHeight(WorldX, WorldY) < BestZ;
}
