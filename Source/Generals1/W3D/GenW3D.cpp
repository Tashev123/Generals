#include "W3D/GenW3D.h"
#include "Generals1.h"

namespace
{
	// Chunk ids (w3d_file.h)
	enum : uint32
	{
		W3D_MESH = 0x000,
		W3D_VERTICES = 0x002,
		W3D_VERTEX_NORMALS = 0x003,
		W3D_VERTEX_INFLUENCES = 0x00E,
		W3D_MESH_HEADER3 = 0x01F,
		W3D_TRIANGLES = 0x020,
		W3D_SHADERS = 0x029,
		W3D_VERTEX_MATERIALS = 0x02A,
		W3D_VERTEX_MATERIAL = 0x02B,
		W3D_VERTEX_MATERIAL_NAME = 0x02C,
		W3D_VERTEX_MATERIAL_INFO = 0x02D,
		W3D_VERTEX_MAPPER_ARGS0 = 0x02E,
		W3D_TEXTURES = 0x030,
		W3D_TEXTURE = 0x031,
		W3D_TEXTURE_NAME = 0x032,
		W3D_TEXTURE_INFO = 0x033,
		W3D_MATERIAL_PASS = 0x038,
		W3D_VERTEX_MATERIAL_IDS = 0x039,
		W3D_SHADER_IDS = 0x03A,
		W3D_DCG = 0x03B,
		W3D_TEXTURE_STAGE = 0x048,
		W3D_TEXTURE_IDS = 0x049,
		W3D_STAGE_TEXCOORDS = 0x04A,

		W3D_HIERARCHY = 0x100,
		W3D_HIERARCHY_HEADER = 0x101,
		W3D_PIVOTS = 0x102,

		W3D_ANIMATION = 0x200,
		W3D_ANIMATION_HEADER = 0x201,
		W3D_ANIMATION_CHANNEL = 0x202,
		W3D_BIT_CHANNEL = 0x203,

		W3D_COMPRESSED_ANIMATION = 0x280,
		W3D_COMPRESSED_ANIMATION_HEADER = 0x281,
		W3D_COMPRESSED_ANIMATION_CHANNEL = 0x282,
		W3D_COMPRESSED_BIT_CHANNEL = 0x283,

		W3D_HLOD = 0x700,
		W3D_HLOD_HEADER = 0x701,
		W3D_HLOD_LOD_ARRAY = 0x702,
		W3D_HLOD_SUB_OBJECT_ARRAY_HEADER = 0x703,
		W3D_HLOD_SUB_OBJECT = 0x704,
		W3D_HLOD_AGGREGATE_ARRAY = 0x705,

		W3D_BOX = 0x740,
	};

	constexpr uint32 TimecodedBinaryFlag = 0x80000000u;

	struct FChunk
	{
		uint32 Type;
		int32 Start; // payload start
		int32 End;   // payload end
	};

	class FReader
	{
	public:
		explicit FReader(TConstArrayView<uint8> InData) : Data(InData) {}

		bool Valid(int32 Pos, int32 Size) const { return Pos >= 0 && Size >= 0 && Pos + Size <= Data.Num(); }

		uint32 U32(int32 Pos) const { uint32 V = 0; if (Valid(Pos, 4)) { FMemory::Memcpy(&V, &Data[Pos], 4); } return V; }
		int32 I32(int32 Pos) const { return int32(U32(Pos)); }
		uint16 U16(int32 Pos) const { uint16 V = 0; if (Valid(Pos, 2)) { FMemory::Memcpy(&V, &Data[Pos], 2); } return V; }
		uint8 U8(int32 Pos) const { return Valid(Pos, 1) ? Data[Pos] : 0; }
		float F32(int32 Pos) const { float V = 0; if (Valid(Pos, 4)) { FMemory::Memcpy(&V, &Data[Pos], 4); } return V; }
		FVector3f Vec3(int32 Pos) const { return FVector3f(F32(Pos), F32(Pos + 4), F32(Pos + 8)); }

		FString Str(int32 Pos, int32 MaxLen) const
		{
			FString S;
			for (int32 i = 0; i < MaxLen && Valid(Pos + i, 1) && Data[Pos + i] != 0; ++i)
			{
				S.AppendChar(TCHAR(Data[Pos + i]));
			}
			return S;
		}

		TArray<FChunk> Chunks(int32 Start, int32 End) const
		{
			TArray<FChunk> Out;
			int32 Pos = Start;
			while (Pos + 8 <= End)
			{
				const uint32 Type = U32(Pos);
				const int32 Size = int32(U32(Pos + 4) & 0x7FFFFFFFu);
				const int32 PStart = Pos + 8;
				if (Size < 0 || PStart + Size > End)
				{
					break;
				}
				Out.Add({ Type, PStart, PStart + Size });
				Pos = PStart + Size;
			}
			return Out;
		}

		TConstArrayView<uint8> Data;
	};

	FColor RGB(const FReader& R, int32 Pos) { return FColor(R.U8(Pos), R.U8(Pos + 1), R.U8(Pos + 2), 255); }

	void ParseVertexMaterial(const FReader& R, const FChunk& C, FGenW3DVertexMaterial& Out)
	{
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			switch (S.Type)
			{
			case W3D_VERTEX_MATERIAL_NAME: Out.Name = R.Str(S.Start, S.End - S.Start); break;
			case W3D_VERTEX_MATERIAL_INFO:
				Out.Attributes = R.U32(S.Start);
				Out.Ambient = RGB(R, S.Start + 4);
				Out.Diffuse = RGB(R, S.Start + 8);
				Out.Specular = RGB(R, S.Start + 12);
				Out.Emissive = RGB(R, S.Start + 16);
				Out.Shininess = R.F32(S.Start + 20);
				Out.Opacity = R.F32(S.Start + 24);
				Out.Translucency = R.F32(S.Start + 28);
				break;
			case W3D_VERTEX_MAPPER_ARGS0: Out.MapperArgs0 = R.Str(S.Start, S.End - S.Start); break;
			default: break;
			}
		}
	}

	void ReadU32Array(const FReader& R, const FChunk& C, TArray<uint32>& Out)
	{
		const int32 N = (C.End - C.Start) / 4;
		Out.SetNumUninitialized(N);
		for (int32 i = 0; i < N; ++i) { Out[i] = R.U32(C.Start + i * 4); }
	}

	void ParseMaterialPass(const FReader& R, const FChunk& C, FGenW3DMaterialPass& Out)
	{
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			switch (S.Type)
			{
			case W3D_VERTEX_MATERIAL_IDS: ReadU32Array(R, S, Out.VertexMaterialIds); break;
			case W3D_SHADER_IDS: ReadU32Array(R, S, Out.ShaderIds); break;
			case W3D_DCG:
			{
				const int32 N = (S.End - S.Start) / 4;
				Out.DiffuseColors.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i)
				{
					const int32 P = S.Start + i * 4;
					Out.DiffuseColors[i] = FColor(R.U8(P), R.U8(P + 1), R.U8(P + 2), R.U8(P + 3));
				}
				break;
			}
			case W3D_TEXTURE_STAGE:
			{
				FGenW3DTextureStage& Stage = Out.Stages.AddDefaulted_GetRef();
				for (const FChunk& T : R.Chunks(S.Start, S.End))
				{
					if (T.Type == W3D_TEXTURE_IDS)
					{
						ReadU32Array(R, T, Stage.TextureIds);
					}
					else if (T.Type == W3D_STAGE_TEXCOORDS)
					{
						const int32 N = (T.End - T.Start) / 8;
						Stage.TexCoords.SetNumUninitialized(N);
						for (int32 i = 0; i < N; ++i)
						{
							Stage.TexCoords[i] = FVector2f(R.F32(T.Start + i * 8), R.F32(T.Start + i * 8 + 4));
						}
					}
				}
				break;
			}
			default: break;
			}
		}
	}

	void ParseMesh(const FReader& R, const FChunk& C, FGenW3DMesh& M)
	{
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			switch (S.Type)
			{
			case W3D_MESH_HEADER3:
				M.Attributes = R.U32(S.Start + 4);
				M.Name = R.Str(S.Start + 8, 16);
				M.Container = R.Str(S.Start + 24, 16);
				M.SortLevel = R.I32(S.Start + 56);
				M.BoundsMin = R.Vec3(S.Start + 76);
				M.BoundsMax = R.Vec3(S.Start + 88);
				break;
			case W3D_VERTICES:
			case W3D_VERTEX_NORMALS:
			{
				TArray<FVector3f>& Arr = S.Type == W3D_VERTICES ? M.Vertices : M.Normals;
				const int32 N = (S.End - S.Start) / 12;
				Arr.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i) { Arr[i] = R.Vec3(S.Start + i * 12); }
				break;
			}
			case W3D_VERTEX_INFLUENCES:
			{
				const int32 N = (S.End - S.Start) / 8;
				M.Influences.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i) { M.Influences[i] = R.U16(S.Start + i * 8); }
				break;
			}
			case W3D_TRIANGLES:
			{
				const int32 N = (S.End - S.Start) / 32;
				M.Triangles.SetNumUninitialized(N);
				for (int32 i = 0; i < N; ++i)
				{
					const int32 P = S.Start + i * 32;
					M.Triangles[i] = FIntVector3(R.I32(P), R.I32(P + 4), R.I32(P + 8));
				}
				break;
			}
			case W3D_SHADERS:
			{
				const int32 N = (S.End - S.Start) / 16;
				for (int32 i = 0; i < N; ++i)
				{
					const int32 P = S.Start + i * 16;
					FGenW3DShader& Sh = M.Shaders.AddDefaulted_GetRef();
					Sh.DepthCompare = R.U8(P + 0);
					Sh.DepthMask = R.U8(P + 1);
					Sh.DestBlend = R.U8(P + 3);
					Sh.PriGradient = R.U8(P + 5);
					Sh.SecGradient = R.U8(P + 6);
					Sh.SrcBlend = R.U8(P + 7);
					Sh.Texturing = R.U8(P + 8);
					Sh.DetailColorFunc = R.U8(P + 9);
					Sh.DetailAlphaFunc = R.U8(P + 10);
					Sh.AlphaTest = R.U8(P + 12);
				}
				break;
			}
			case W3D_VERTEX_MATERIALS:
				for (const FChunk& V : R.Chunks(S.Start, S.End))
				{
					if (V.Type == W3D_VERTEX_MATERIAL)
					{
						ParseVertexMaterial(R, V, M.VertexMaterials.AddDefaulted_GetRef());
					}
				}
				break;
			case W3D_TEXTURES:
				for (const FChunk& T : R.Chunks(S.Start, S.End))
				{
					if (T.Type != W3D_TEXTURE) { continue; }
					FGenW3DTexture& Tex = M.Textures.AddDefaulted_GetRef();
					for (const FChunk& TT : R.Chunks(T.Start, T.End))
					{
						if (TT.Type == W3D_TEXTURE_NAME)
						{
							Tex.Name = R.Str(TT.Start, TT.End - TT.Start);
						}
						else if (TT.Type == W3D_TEXTURE_INFO)
						{
							Tex.Attributes = R.U16(TT.Start);
							Tex.AnimType = R.U16(TT.Start + 2);
							Tex.FrameCount = R.U32(TT.Start + 4);
							Tex.FrameRate = R.F32(TT.Start + 8);
						}
					}
				}
				break;
			case W3D_MATERIAL_PASS:
				ParseMaterialPass(R, S, M.Passes.AddDefaulted_GetRef());
				break;
			default:
				break;
			}
		}
	}

	TSharedPtr<FGenW3DHierarchy> ParseHierarchy(const FReader& R, const FChunk& C)
	{
		TSharedPtr<FGenW3DHierarchy> H = MakeShared<FGenW3DHierarchy>();
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			if (S.Type == W3D_HIERARCHY_HEADER)
			{
				H->Name = R.Str(S.Start + 4, 16);
				H->Center = R.Vec3(S.Start + 24);
			}
			else if (S.Type == W3D_PIVOTS)
			{
				const int32 N = (S.End - S.Start) / 60;
				H->Pivots.SetNum(N);
				for (int32 i = 0; i < N; ++i)
				{
					const int32 P = S.Start + i * 60;
					FGenW3DPivot& Pv = H->Pivots[i];
					Pv.Name = R.Str(P, 16);
					const uint32 Parent = R.U32(P + 16);
					Pv.Parent = Parent == 0xFFFFFFFFu ? -1 : int32(Parent);
					Pv.Translation = R.Vec3(P + 20);
					// P + 32: euler angles (unused by the runtime)
					Pv.Rotation = FQuat4f(R.F32(P + 44), R.F32(P + 48), R.F32(P + 52), R.F32(P + 56));
				}
			}
		}
		return H;
	}

	TSharedPtr<FGenW3DHLod> ParseHLod(const FReader& R, const FChunk& C)
	{
		TSharedPtr<FGenW3DHLod> H = MakeShared<FGenW3DHLod>();
		float BestScreenSize = -1.f;
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			if (S.Type == W3D_HLOD_HEADER)
			{
				H->Name = R.Str(S.Start + 8, 16);
				H->HierarchyName = R.Str(S.Start + 24, 16);
			}
			else if (S.Type == W3D_HLOD_LOD_ARRAY || S.Type == W3D_HLOD_AGGREGATE_ARRAY)
			{
				float ScreenSize = 0.f;
				TArray<FGenW3DSubObject> Objs;
				for (const FChunk& A : R.Chunks(S.Start, S.End))
				{
					if (A.Type == W3D_HLOD_SUB_OBJECT_ARRAY_HEADER)
					{
						ScreenSize = R.F32(A.Start + 4);
					}
					else if (A.Type == W3D_HLOD_SUB_OBJECT)
					{
						Objs.Add({ int32(R.U32(A.Start)), R.Str(A.Start + 4, 32) });
					}
				}
				if (S.Type == W3D_HLOD_AGGREGATE_ARRAY)
				{
					H->Aggregates.Append(Objs);
				}
				else if (ScreenSize > BestScreenSize)
				{
					// Highest detail = largest MaxScreenSize.
					BestScreenSize = ScreenSize;
					H->SubObjects = MoveTemp(Objs);
				}
			}
		}
		return H;
	}

	FGenW3DChannel::EKind KindFromFlags(uint32 Flags, bool& bValid)
	{
		bValid = true;
		switch (Flags)
		{
		case 0: case 7: case 11: return FGenW3DChannel::EKind::X;
		case 1: case 8: case 12: return FGenW3DChannel::EKind::Y;
		case 2: case 9: case 13: return FGenW3DChannel::EKind::Z;
		case 6: case 10: case 14: return FGenW3DChannel::EKind::Q;
		default: bValid = false; return FGenW3DChannel::EKind::X; // euler channels: unused by the runtime
		}
	}

	FQuat4f SlerpQ(const FQuat4f& A, const FQuat4f& B, float T)
	{
		return FQuat4f::Slerp(A, B, T).GetNormalized();
	}

	// Raw (uncompressed) motion channel: frames [First, Last]; identity outside.
	void ParseRawChannel(const FReader& R, const FChunk& C, FGenW3DAnimation& Anim)
	{
		const int32 First = R.U16(C.Start);
		const int32 Last = R.U16(C.Start + 2);
		const int32 VecLen = R.U16(C.Start + 4);
		const uint32 Flags = R.U16(C.Start + 6);
		const int32 Pivot = R.U16(C.Start + 8);
		bool bValid;
		const FGenW3DChannel::EKind Kind = KindFromFlags(Flags, bValid);
		if (!bValid || Last < First)
		{
			return;
		}
		FGenW3DChannel& Ch = Anim.Channels.AddDefaulted_GetRef();
		Ch.Pivot = Pivot;
		Ch.Kind = Kind;
		Ch.FirstFrame = First;
		const int32 Num = Last - First + 1;
		const int32 DataPos = C.Start + 12;
		if (Kind == FGenW3DChannel::EKind::Q && VecLen == 4)
		{
			Ch.Quats.SetNum(Num);
			for (int32 i = 0; i < Num; ++i)
			{
				const int32 P = DataPos + i * 16;
				Ch.Quats[i] = FQuat4f(R.F32(P), R.F32(P + 4), R.F32(P + 8), R.F32(P + 12));
			}
		}
		else
		{
			Ch.Scalars.SetNum(Num);
			for (int32 i = 0; i < Num; ++i) { Ch.Scalars[i] = R.F32(DataPos + i * 4 * VecLen); }
		}
	}

	void ParseBitChannel(const FReader& R, const FChunk& C, FGenW3DAnimation& Anim)
	{
		const int32 First = R.U16(C.Start);
		const int32 Last = R.U16(C.Start + 2);
		const int32 Pivot = R.U16(C.Start + 6);
		const uint8 Default = R.U8(C.Start + 8);
		FGenW3DChannel& Ch = Anim.Channels.AddDefaulted_GetRef();
		Ch.Pivot = Pivot;
		Ch.Kind = FGenW3DChannel::EKind::Vis;
		Ch.DefaultVis = Default != 0;
		Ch.FirstFrame = First;
		const int32 Num = FMath::Max(0, Last - First + 1);
		Ch.Bits.SetNum(Num);
		for (int32 b = 0; b < Num; ++b)
		{
			Ch.Bits[b] = (R.U8(C.Start + 9 + b / 8) >> (b % 8)) & 1;
		}
	}

	// Time-coded channel: keys {u32 time (MSB = binary step), VecLen floats}; sampled at integer frames.
	void ParseTimecodedChannel(const FReader& R, const FChunk& C, FGenW3DAnimation& Anim)
	{
		const int32 NumKeys = int32(R.U32(C.Start));
		const int32 Pivot = R.U16(C.Start + 4);
		const int32 VecLen = R.U8(C.Start + 6);
		const uint32 Flags = R.U8(C.Start + 7);
		bool bValid;
		const FGenW3DChannel::EKind Kind = KindFromFlags(Flags, bValid);
		if (!bValid || NumKeys <= 0)
		{
			return;
		}
		const int32 Stride = (VecLen + 1) * 4;
		auto KeyTime = [&](int32 k) { return int32(R.U32(C.Start + 8 + k * Stride) & ~TimecodedBinaryFlag); };
		auto KeyBinary = [&](int32 k) { return (R.U32(C.Start + 8 + k * Stride) & TimecodedBinaryFlag) != 0; };
		auto KeyVal = [&](int32 k, int32 i) { return R.F32(C.Start + 8 + k * Stride + 4 + i * 4); };

		FGenW3DChannel& Ch = Anim.Channels.AddDefaulted_GetRef();
		Ch.Pivot = Pivot;
		Ch.Kind = Kind;
		Ch.FirstFrame = 0;
		const int32 Num = FMath::Max(Anim.NumFrames, KeyTime(NumKeys - 1) + 1);
		if (Kind == FGenW3DChannel::EKind::Q) { Ch.Quats.SetNum(Num); } else { Ch.Scalars.SetNum(Num); }

		int32 Key = 0;
		for (int32 Frame = 0; Frame < Num; ++Frame)
		{
			while (Key + 1 < NumKeys && KeyTime(Key + 1) <= Frame) { ++Key; }
			const bool bLast = Key == NumKeys - 1 || Frame <= KeyTime(Key);
			const int32 Next = FMath::Min(Key + 1, NumKeys - 1);
			const float T = bLast || KeyBinary(Next) ? 0.f
				: float(Frame - KeyTime(Key)) / float(FMath::Max(1, KeyTime(Next) - KeyTime(Key)));
			if (Kind == FGenW3DChannel::EKind::Q)
			{
				const FQuat4f A(KeyVal(Key, 0), KeyVal(Key, 1), KeyVal(Key, 2), KeyVal(Key, 3));
				const FQuat4f B(KeyVal(Next, 0), KeyVal(Next, 1), KeyVal(Next, 2), KeyVal(Next, 3));
				Ch.Quats[Frame] = T > 0.f ? SlerpQ(A, B, T) : A;
			}
			else
			{
				Ch.Scalars[Frame] = FMath::Lerp(KeyVal(Key, 0), KeyVal(Next, 0), T);
			}
		}
	}

	// AdaptiveDeltaMotionChannelClass::decompress, fully expanded.
	void ParseAdaptiveDeltaChannel(const FReader& R, const FChunk& C, FGenW3DAnimation& Anim)
	{
		static float FilterTable[256];
		static bool bTableInit = false;
		if (!bTableInit)
		{
			const float Fixed[16] = { 1e-8f, 1e-7f, 1e-6f, 1e-5f, 1e-4f, 1e-3f, 1e-2f, 1e-1f, 1.f, 10.f, 100.f, 1e3f, 1e4f, 1e5f, 1e6f, 1e7f };
			for (int32 i = 0; i < 16; ++i) { FilterTable[i] = Fixed[i]; }
			for (int32 i = 0; i < 240; ++i)
			{
				FilterTable[16 + i] = 1.f - FMath::Sin(FMath::DegreesToRadians(90.f * (float(i) / 240.f)));
			}
			bTableInit = true;
		}

		const int32 NumFrames = int32(R.U32(C.Start));
		const int32 Pivot = R.U16(C.Start + 4);
		const int32 VecLen = R.U8(C.Start + 6);
		const uint32 Flags = R.U8(C.Start + 7);
		const float Scale = R.F32(C.Start + 8);
		bool bValid;
		const FGenW3DChannel::EKind Kind = KindFromFlags(Flags, bValid);
		if (!bValid || NumFrames <= 0 || VecLen <= 0 || VecLen > 4)
		{
			return;
		}
		const int32 Base = C.Start + 12;
		TArray<float> Values; // [frame * VecLen + vi]
		Values.SetNumZeroed(NumFrames * VecLen);
		for (int32 vi = 0; vi < VecLen; ++vi)
		{
			float Last = R.F32(Base + vi * 4);
			Values[vi] = Last;
			int32 P = Base + VecLen * 4 + 9 * vi;
			int32 Frame = 1;
			while (Frame < NumFrames && R.Valid(P, 9))
			{
				const float Filter = FilterTable[R.U8(P)] * Scale;
				++P;
				for (int32 fi = 0; fi < 16 && Frame < NumFrames; ++fi)
				{
					const uint8 Byte = R.U8(P + (fi >> 1));
					int32 Nyb = (fi & 1) ? (Byte >> 4) : (Byte & 0xF);
					if (Nyb & 0x8) { Nyb -= 16; }
					Last += float(Nyb) * Filter;
					Values[Frame * VecLen + vi] = Last;
					++Frame;
				}
				P += 9 * VecLen - 1;
			}
		}

		FGenW3DChannel& Ch = Anim.Channels.AddDefaulted_GetRef();
		Ch.Pivot = Pivot;
		Ch.Kind = Kind;
		Ch.FirstFrame = 0;
		if (Kind == FGenW3DChannel::EKind::Q && VecLen == 4)
		{
			Ch.Quats.SetNum(NumFrames);
			for (int32 f = 0; f < NumFrames; ++f)
			{
				Ch.Quats[f] = FQuat4f(Values[f * 4], Values[f * 4 + 1], Values[f * 4 + 2], Values[f * 4 + 3]).GetNormalized();
			}
		}
		else
		{
			Ch.Scalars.SetNum(NumFrames);
			for (int32 f = 0; f < NumFrames; ++f) { Ch.Scalars[f] = Values[f * VecLen]; }
		}
	}

	void ParseTimecodedBitChannel(const FReader& R, const FChunk& C, FGenW3DAnimation& Anim)
	{
		const int32 NumKeys = int32(R.U32(C.Start));
		const int32 Pivot = R.U16(C.Start + 4);
		const uint8 Default = R.U8(C.Start + 7);
		FGenW3DChannel& Ch = Anim.Channels.AddDefaulted_GetRef();
		Ch.Pivot = Pivot;
		Ch.Kind = FGenW3DChannel::EKind::Vis;
		Ch.DefaultVis = Default != 0;
		Ch.FirstFrame = 0;
		Ch.Bits.SetNum(FMath::Max(1, Anim.NumFrames));
		int32 Key = -1;
		for (int32 Frame = 0; Frame < Ch.Bits.Num(); ++Frame)
		{
			while (Key + 1 < NumKeys && int32(R.U32(C.Start + 8 + (Key + 1) * 4) & ~TimecodedBinaryFlag) <= Frame) { ++Key; }
			Ch.Bits[Frame] = Key < 0 ? Default : uint8((R.U32(C.Start + 8 + Key * 4) & TimecodedBinaryFlag) != 0);
		}
	}

	TSharedPtr<FGenW3DAnimation> ParseAnimation(const FReader& R, const FChunk& C, bool bCompressed)
	{
		TSharedPtr<FGenW3DAnimation> A = MakeShared<FGenW3DAnimation>();
		uint16 Flavor = 0;
		for (const FChunk& S : R.Chunks(C.Start, C.End))
		{
			if (S.Type == W3D_ANIMATION_HEADER || S.Type == W3D_COMPRESSED_ANIMATION_HEADER)
			{
				A->Name = R.Str(S.Start + 4, 16);
				A->HierarchyName = R.Str(S.Start + 20, 16);
				A->NumFrames = int32(R.U32(S.Start + 36));
				if (bCompressed)
				{
					A->FrameRate = float(R.U16(S.Start + 40));
					Flavor = R.U16(S.Start + 42);
				}
				else
				{
					A->FrameRate = float(R.U32(S.Start + 40));
				}
			}
			else if (S.Type == W3D_ANIMATION_CHANNEL)
			{
				ParseRawChannel(R, S, *A);
			}
			else if (S.Type == W3D_BIT_CHANNEL)
			{
				ParseBitChannel(R, S, *A);
			}
			else if (S.Type == W3D_COMPRESSED_ANIMATION_CHANNEL)
			{
				if (Flavor == 0) { ParseTimecodedChannel(R, S, *A); }
				else { ParseAdaptiveDeltaChannel(R, S, *A); }
			}
			else if (S.Type == W3D_COMPRESSED_BIT_CHANNEL)
			{
				ParseTimecodedBitChannel(R, S, *A);
			}
		}
		if (A->FrameRate <= 0.f)
		{
			A->FrameRate = 30.f;
		}
		A->BuildLookup();
		return A;
	}
}

// ---- types ----------------------------------------------------------------------------------

EGenW3DBlend FGenW3DShader::GetBlend() const
{
	// Collapse D3D blend factors into the modes Generals actually uses.
	if (SrcBlend == 1 && DestBlend == 0) { return AlphaTest ? EGenW3DBlend::AlphaTest : EGenW3DBlend::Opaque; }
	if ((SrcBlend == 2 || SrcBlend == 1) && DestBlend == 5) { return EGenW3DBlend::Alpha; }
	if ((SrcBlend == 1 || SrcBlend == 2) && DestBlend == 1) { return EGenW3DBlend::Additive; }
	if (SrcBlend == 0 && DestBlend == 2) { return EGenW3DBlend::Multiply; }
	if (DestBlend == 3) { return EGenW3DBlend::Multiply; } // ONE, ONE_MINUS_SRC_COLOR (rare)
	return AlphaTest ? EGenW3DBlend::AlphaTest : EGenW3DBlend::Opaque;
}

int32 FGenW3DHierarchy::FindPivot(const FString& PivotName) const
{
	for (int32 i = 0; i < Pivots.Num(); ++i)
	{
		if (Pivots[i].Name.Equals(PivotName, ESearchCase::IgnoreCase))
		{
			return i;
		}
	}
	return INDEX_NONE;
}

void FGenW3DAnimation::BuildLookup()
{
	int32 MaxPivot = -1;
	for (const FGenW3DChannel& Ch : Channels) { MaxPivot = FMath::Max(MaxPivot, Ch.Pivot); }
	PivotLookup.Init(INDEX_NONE, (MaxPivot + 1) * 5);
	for (int32 i = 0; i < Channels.Num(); ++i)
	{
		PivotLookup[Channels[i].Pivot * 5 + int32(Channels[i].Kind)] = i;
	}
}

void FGenW3DAnimation::SamplePivot(int32 PivotIndex, int32 Frame, FVector3f& OutTranslation, FQuat4f& OutRotation, bool& OutVisible) const
{
	OutTranslation = FVector3f::ZeroVector;
	OutRotation = FQuat4f::Identity;
	OutVisible = true;
	if (PivotIndex < 0 || PivotIndex * 5 + 4 >= PivotLookup.Num())
	{
		return;
	}
	auto Scalar = [&](int32 Kind) -> float
	{
		const int32 Idx = PivotLookup[PivotIndex * 5 + Kind];
		if (Idx == INDEX_NONE) { return 0.f; }
		const FGenW3DChannel& Ch = Channels[Idx];
		const int32 Local = Frame - Ch.FirstFrame;
		return Ch.Scalars.IsValidIndex(Local) ? Ch.Scalars[Local] : 0.f;
	};
	OutTranslation = FVector3f(Scalar(0), Scalar(1), Scalar(2));

	const int32 QIdx = PivotLookup[PivotIndex * 5 + 3];
	if (QIdx != INDEX_NONE)
	{
		const FGenW3DChannel& Ch = Channels[QIdx];
		const int32 Local = Frame - Ch.FirstFrame;
		if (Ch.Quats.IsValidIndex(Local))
		{
			OutRotation = Ch.Quats[Local];
		}
	}
	const int32 VIdx = PivotLookup[PivotIndex * 5 + 4];
	if (VIdx != INDEX_NONE)
	{
		const FGenW3DChannel& Ch = Channels[VIdx];
		const int32 Local = Frame - Ch.FirstFrame;
		OutVisible = Ch.Bits.IsValidIndex(Local) ? Ch.Bits[Local] != 0 : Ch.DefaultVis;
	}
}

const FGenW3DMesh* FGenW3DFile::FindMesh(const FString& FullName) const
{
	for (const FGenW3DMesh& M : Meshes)
	{
		if (M.FullName().Equals(FullName, ESearchCase::IgnoreCase))
		{
			return &M;
		}
	}
	return nullptr;
}

const FGenW3DBox* FGenW3DFile::FindBox(const FString& FullName) const
{
	for (const FGenW3DBox& B : Boxes)
	{
		if (B.Name.Equals(FullName, ESearchCase::IgnoreCase))
		{
			return &B;
		}
	}
	return nullptr;
}

bool GenW3D::Parse(TConstArrayView<uint8> Data, FGenW3DFile& Out)
{
	const FReader R(Data);
	for (const FChunk& C : R.Chunks(0, Data.Num()))
	{
		switch (C.Type)
		{
		case W3D_MESH: ParseMesh(R, C, Out.Meshes.AddDefaulted_GetRef()); break;
		case W3D_HIERARCHY: Out.Hierarchy = ParseHierarchy(R, C); break;
		case W3D_ANIMATION: Out.Animations.Add(ParseAnimation(R, C, false)); break;
		case W3D_COMPRESSED_ANIMATION: Out.Animations.Add(ParseAnimation(R, C, true)); break;
		case W3D_HLOD: Out.HLod = ParseHLod(R, C); break;
		case W3D_BOX:
		{
			FGenW3DBox& B = Out.Boxes.AddDefaulted_GetRef();
			B.Attributes = R.U32(C.Start + 4);
			B.Name = R.Str(C.Start + 8, 32);
			B.Center = R.Vec3(C.Start + 44);
			B.Extent = R.Vec3(C.Start + 56);
			break;
		}
		default: break;
		}
	}
	return Out.Meshes.Num() > 0 || Out.Hierarchy.IsValid() || Out.Animations.Num() > 0 || Out.HLod.IsValid();
}
