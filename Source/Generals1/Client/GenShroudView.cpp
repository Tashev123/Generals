// The fog of war on screen (W3DShroud.cpp and the ghost objects of W3DGhostObject.cpp).
//
// The original multiplies the terrain and everything on it by a texture with one texel per fog-of-war
// cell: 255 where the player is looking, FogAlpha (127) where they have been, ShroudAlpha (0) where they
// never were and beyond the edge of the map. Here the same texture goes into a post-process material
// (M_Shroud) that finds the cell under each pixel from its world position, so the terrain, the models,
// the particles and the water are all dimmed alike without their materials knowing about it.

#include "Logic/GenGameLogic.h"
#include "Core/GenPerf.h"
#include "Core/GenCoords.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapActor.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Components/PostProcessComponent.h"
#include "Engine/Texture2D.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "RenderingThread.h"
#include "TextureResource.h"

namespace
{
	/** "Take one second to go from black to fully lit" (FOG_INTERPOLATION_RATE). */
	constexpr float ShroudFadePerSecond = 255.f;

	uint8 ShroudLevel(EGenCellShroud Status)
	{
		const FGenGameData& GD = FGenGameData::Get();
		const uint8 Level = Status == EGenCellShroud::Clear ? GD.ClearAlpha : (Status == EGenCellShroud::Fogged ? GD.FogAlpha : GD.ShroudAlpha);
		return FMath::Max(Level, GD.ShroudAlpha);
	}
}

void UGenGameLogic::InitShroudView()
{
	UPostProcessComponent* PostProcess = MapActor ? MapActor->GetPostProcess() : nullptr;
	if (PostProcess && ShroudMaterial)
	{
		PostProcess->Settings.RemoveBlendable(ShroudMaterial);
	}
	ShroudTexture = nullptr;
	ShroudMaterial = nullptr;
	ShroudCurrent.Reset();
	ShroudFinal.Reset();
	ShroudFadeCarry = 0.f;
	ClearGhosts();
	if (!bShroudOn || !Shroud.IsValid() || !MapActor)
	{
		return; // -GenNoShroud: nothing is dimmed, nothing is hidden
	}

	// One texel per cell, and a border of unexplored cells all round: with clamped addressing everything
	// beyond the playable area comes out black, as it does in the original.
	const int32 W = Shroud.GetCellsX() + 2, H = Shroud.GetCellsY() + 2;
	const uint8 Border = FGenGameData::Get().ShroudAlpha;
	ShroudCurrent.Init(Border, W * H);   // the map fades in from black
	ShroudFinal.Init(Border, W * H);
	ShroudViewSerial = 0;

	ShroudTexture = UTexture2D::CreateTransient(W, H, PF_B8G8R8A8, TEXT("GenShroud"));
	if (!ShroudTexture)
	{
		return;
	}
	ShroudTexture->SRGB = false;
	ShroudTexture->Filter = TF_Bilinear;
	ShroudTexture->AddressX = TA_Clamp;
	ShroudTexture->AddressY = TA_Clamp;
	ShroudTexture->NeverStream = true;
	ShroudTexture->CompressionSettings = TC_VectorDisplacementmap; // plain 8-bit channels, no compression
	if (FTexturePlatformData* Platform = ShroudTexture->GetPlatformData())
	{
		uint8* Dst = static_cast<uint8*>(Platform->Mips[0].BulkData.Lock(LOCK_READ_WRITE));
		for (int32 i = 0; i < W * H; ++i)
		{
			Dst[i * 4 + 0] = Dst[i * 4 + 1] = Dst[i * 4 + 2] = Border;
			Dst[i * 4 + 3] = uint8(255 - Border);
		}
		Platform->Mips[0].BulkData.Unlock();
	}
	ShroudTexture->UpdateResource();

	UMaterialInterface* Base = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/Generals/Materials/M_Shroud.M_Shroud"));
	if (!Base || !PostProcess)
	{
		UE_LOG(LogGenerals, Warning, TEXT("Fog of war: material M_Shroud not found (run Tools/ue_setup.py); the fog will not be drawn"));
		return;
	}
	ShroudMaterial = UMaterialInstanceDynamic::Create(Base, this);
	ShroudMaterial->SetTextureParameterValue(TEXT("Shroud"), ShroudTexture);
	// Unreal world position -> texture coordinate. Generals x = UE x / 10, Generals y = -UE y / 10 (relative
	// to the map actor), and cell (i, j) is texel (i + 1, j + 1).
	const FVector Loc = MapActor->GetActorLocation();
	const double CellCm = double(Shroud.GetCellSize()) * GenCoords::UnitsToCm;
	const FLinearColor Scale(float(1.0 / (CellCm * W)), float(-1.0 / (CellCm * H)), 0.f, 0.f);
	const FLinearColor Offset(float((1.0 - Loc.X / CellCm) / W), float((1.0 + Loc.Y / CellCm) / H), 0.f, 0.f);
	ShroudMaterial->SetVectorParameterValue(TEXT("ShroudScale"), Scale);
	ShroudMaterial->SetVectorParameterValue(TEXT("ShroudOffset"), Offset);
	PostProcess->AddOrUpdateBlendable(ShroudMaterial, 1.f);
}

void UGenGameLogic::UpdateShroudView(float DeltaSeconds)
{
	GEN_PERF_SCOPE("R.ShroudView");
	if (!ShroudTexture || ShroudFinal.Num() == 0)
	{
		return;
	}
	const int32 NX = Shroud.GetCellsX(), NY = Shroud.GetCellsY();
	const int32 W = NX + 2, H = NY + 2;

	// PartitionCell::addLooker / removeLooker -> W3DDisplay::setShroudLevel: where the fog is heading.
	const uint32 Serial = Shroud.GetChangeSerial(LocalPlayer);
	if (Serial != ShroudViewSerial)
	{
		ShroudViewSerial = Serial;
		for (int32 Y = 0; Y < NY; ++Y)
		{
			uint8* Row = &ShroudFinal[(Y + 1) * W + 1];
			for (int32 X = 0; X < NX; ++X)
			{
				Row[X] = ShroudLevel(Shroud.GetStatus(LocalPlayer, X, Y));
			}
		}
	}

	// W3DShroud::interpolateFogLevels: what is shown eases towards it, in real time.
	ShroudFadeCarry += FMath::Clamp(DeltaSeconds, 0.f, 0.25f) * ShroudFadePerSecond;
	const int32 Step = FMath::Min(FMath::FloorToInt32(ShroudFadeCarry), 255);
	if (Step <= 0)
	{
		return;
	}
	ShroudFadeCarry -= float(Step);
	bool bChanged = false;
	for (int32 i = 0, Num = ShroudCurrent.Num(); i < Num; ++i)
	{
		const int32 Cur = ShroudCurrent[i], Fin = ShroudFinal[i];
		if (Cur != Fin)
		{
			ShroudCurrent[i] = uint8(Cur < Fin ? FMath::Min(Cur + Step, Fin) : FMath::Max(Cur - Step, Fin));
			bChanged = true;
		}
	}
	if (!bChanged)
	{
		return;
	}

	// W3DShroud::setShroudLevel: the level times ShroudColor ("unshrouded pixels should be fully lit");
	// the alpha carries the darkness, for the radar's overlay.
	const FColor Tint = FGenGameData::Get().ShroudColor;
	uint8* Data = static_cast<uint8*>(FMemory::Malloc(W * H * 4));
	for (int32 i = 0; i < W * H; ++i)
	{
		const int32 Level = ShroudCurrent[i];
		Data[i * 4 + 0] = Level == 255 ? 255 : uint8(Level * Tint.B / 255);
		Data[i * 4 + 1] = Level == 255 ? 255 : uint8(Level * Tint.G / 255);
		Data[i * 4 + 2] = Level == 255 ? 255 : uint8(Level * Tint.R / 255);
		Data[i * 4 + 3] = uint8(255 - Level);
	}
	FUpdateTextureRegion2D* Region = new FUpdateTextureRegion2D(0, 0, 0, 0, W, H);
	ShroudTexture->UpdateTextureRegions(0, 1, Region, W * 4, 4, Data,
		[](uint8* SrcData, const FUpdateTextureRegion2D* Regions)
		{
			FMemory::Free(SrcData);
			delete Regions;
		});
}

// ---- ghosts -------------------------------------------------------------------------------------

void UGenGameLogic::UpdateGhosts()
{
	GEN_PERF_SCOPE("R.Ghosts");
	// W3DGhostObjectManager::updateOrphanedObjects: once the place is looked at again (or forgotten
	// altogether), what was remembered of a building that is no longer there goes away.
	for (int32 i = Ghosts.Num() - 1; i >= 0; --i)
	{
		const FGhost& G = Ghosts[i];
		int32 NumShrouded = 0;
		bool bSeen = !bShroudOn;
		const int32 NX = FMath::Max(Shroud.GetCellsX(), 1);
		for (int32 Index : G.Cells)
		{
			const EGenCellShroud S = Shroud.GetStatus(LocalPlayer, Index % NX, Index / NX);
			bSeen |= S == EGenCellShroud::Clear;
			NumShrouded += S == EGenCellShroud::Shrouded ? 1 : 0;
		}
		if (bSeen || NumShrouded == G.Cells.Num() || !G.Model.IsValid())
		{
			for (const TWeakObjectPtr<UGenModelComponent>& Extra : G.Extras)
			{
				if (UGenModelComponent* E = Extra.Get()) { E->DestroyComponent(); }
			}
			if (UGenModelComponent* M = G.Model.Get()) { M->DestroyComponent(); }
			Ghosts.RemoveAtSwap(i);
		}
	}
}

void UGenGameLogic::ClearGhosts()
{
	for (const FGhost& G : Ghosts)
	{
		for (const TWeakObjectPtr<UGenModelComponent>& Extra : G.Extras)
		{
			if (UGenModelComponent* E = Extra.Get()) { E->DestroyComponent(); }
		}
		if (UGenModelComponent* M = G.Model.Get()) { M->DestroyComponent(); }
	}
	Ghosts.Reset();
}
