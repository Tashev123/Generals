// The GLA's demo trap (DemoTrapUpdate): a hidden charge a worker puts down. Which weapon slot it has in
// hand says what it does - the proximity slot: it goes off when an enemy on the ground comes within
// TriggerDetonationRange (unless a friend is near too, where the data says so); the manual slot: it waits;
// the detonation slot (the "detonate" button): it goes off now. Its buttons are SWITCH_WEAPON buttons. It
// goes off when it is killed as well, and a dozer or worker that is disarming it doesn't set it off.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Core/GenPerf.h"
#include "Generals1.h"

void FGenObject::InitDemoTrap()
{
	// DemoTrapUpdate::onObjectCreated: its three "bogus weapons" are in the VETERAN set, and it starts in
	// proximity mode or manual mode.
	const FGenThingTemplate::FDemoTrap& D = Template->DemoTrap;
	SetFlags.AddUnique(TEXT("VETERAN"));
	RefreshWeaponSet();
	SwitchWeapon(D.bDefaultProximityMode ? D.ProximitySlot : D.ManualSlot);
}

void FGenObject::DetonateDemoTrap()
{
	// DemoTrapUpdate::detonate: "only shoot the weapon if not being built or sold".
	if (bDemoTrapDetonated)
	{
		return;
	}
	bDemoTrapDetonated = true;
	const FGenThingTemplate::FDemoTrap& D = Template->DemoTrap;
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [trap f%u] #%u %s goes off at (%.0f, %.0f)"), Logic->GetFrame(), ID, *Template->Name, Position.X, Position.Y);
	if (!bUnderConstruction && !IsBeingSold())
	{
		if (const FGenWeaponTemplate* W = FGenGameData::Get().FindWeapon(D.DetonationWeapon))
		{
			Logic->FireTempWeapon(*W, *this, Position);
		}
	}
	Kill();
}

void FGenObject::UpdateDemoTrap(uint32 Frame)
{
	// DemoTrapUpdate::update
	GEN_PERF_SCOPE("Obj.DemoTrap");
	const FGenThingTemplate::FDemoTrap& D = Template->DemoTrap;
	if (bDemoTrapDetonated || bUnderConstruction || IsBeingSold())
	{
		return;
	}
	const int32 Slot = GetCurrentWeaponSlot();
	if (Slot == D.DetonationSlot)
	{
		DetonateDemoTrap(); // "we've been externally triggered by the press of a command button"
		return;
	}
	if (DemoTrapScanWait > 0)
	{
		--DemoTrapScanWait; // ("don't scan every frame for performance reasons")
		return;
	}
	if (Slot != D.ProximitySlot)
	{
		return; // manual mode: "don't scan!"
	}
	DemoTrapScanWait = D.ScanFrames;
	bool bDetonate = false;
	bool bFriendNear = false;
	const float RangeSq = D.TriggerRange * D.TriggerRange;
	Logic->GetPartition().ForEachInRange(FVector2f(Position.X, Position.Y), D.TriggerRange, [&](FGenObject& Other)
	{
		if (&Other == this || Other.bDead || Other.bPendingDestroy || Other.IsContained() || bFriendNear)
		{
			return;
		}
		for (const FString& K : D.IgnoreKindOf) { if (Other.IsKindOf(*K)) { return; } }
		if (FVector2f::DistSquared(FVector2f(Other.Position.X, Other.Position.Y), FVector2f(Position.X, Position.Y)) > RangeSq)
		{
			return;
		}
		// "If we're dealing with a dozer... check if it's trying to disarm me. If so, don't blow up!"
		if (Other.IsKindOf(TEXT("DOZER")) && Other.IsClearingMines())
		{
			return;
		}
		if (Logic->GetRelationship(Owner, Other.Owner) != EGenRelationship::Enemies)
		{
			bFriendNear |= !D.bFriendlyDetonation; // "not allowed to proximity detonate with friends nearby"
			return;
		}
		if (Other.Position.Z - Other.GetSurfaceHeight() > 1.f)
		{
			return; // "don't detonate on anything airborne"
		}
		bDetonate = true;
	});
	if (bDetonate && !bFriendNear)
	{
		DetonateDemoTrap(); // "Yeehaw!"
	}
}
