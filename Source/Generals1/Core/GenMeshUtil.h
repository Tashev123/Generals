#pragma once

#include "CoreMinimal.h"
#include "Engine/StaticMesh.h"

/**
 * A material slot of a mesh made while the game runs (models, terrain, roads, water). The texture streamer
 * asks every slot how densely its UVs cover the mesh; the editor works that out when it builds a mesh, so
 * a mesh made here says it plainly - 1, the textures as they are (a packaged game checks it is there).
 */
inline FStaticMaterial GenRuntimeMaterialSlot(UMaterialInterface* Material, FName SlotName)
{
	FStaticMaterial Slot(Material, SlotName);
	Slot.UVChannelData = FMeshUVChannelInfo(1.f);
	return Slot;
}
