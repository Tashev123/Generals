// Aircraft that live at an airfield, and the airfield itself.
// Follows JetAIUpdate.cpp (the state machines for planes and for helicopters built on a helipad),
// ParkingPlaceBehavior.cpp and JetSlowDeathBehavior.cpp. The WINGS locomotor is in GenFlight.cpp.

#include "Logic/GenGameLogic.h"
#include "Logic/GenGameData.h"
#include "Logic/GenObject.h"
#include "Core/GenCoords.h"
#include "Audio/GenAudioManager.h"
#include "Audio/GenAudioData.h"
#include "W3D/GenAssets.h"
#include "Generals1.h"

namespace
{
	constexpr float BigLift = 99999.f;              // BIGNUM
	constexpr float JetAirborneThreshold = 9.f;     // SIGNIFICANTLY_ABOVE_TERRAIN_THRESHOLD
	constexpr float ApproachDist = 0.75f;           // how far beyond the runway's end landings begin, in runway lengths

	int32 JetExhaustBit() { static const int32 I = GenConditions::Index(TEXT("JETEXHAUST")); return I; }
	int32 JetAfterburnerBit() { static const int32 I = GenConditions::Index(TEXT("JETAFTERBURNER")); return I; }

	const TCHAR* JetStateName(EGenJetState State)
	{
		static const TCHAR* Names[] = { TEXT("none"), TEXT("taxi from hangar"), TEXT("await takeoff clearance"), TEXT("taxi to takeoff"),
			TEXT("pause before takeoff"), TEXT("taking off"), TEXT("await landing clearance"), TEXT("landing"), TEXT("taxi from landing"),
			TEXT("orient for parking"), TEXT("reload"), TEXT("returning for landing"), TEXT("return to dead airfield"),
			TEXT("circling dead airfield") };
		return Names[int32(State)];
	}

	/** JetAIStateMachine / HeliAIStateMachine: where each state leads on success and on failure. */
	EGenJetState NextJetState(EGenJetState State, bool bSuccess, bool bNeedsRunway)
	{
		using S = EGenJetState;
		switch (State)
		{
		case S::ReturningForLanding:   return bSuccess ? S::LandingAwaitClearance : S::ReturnToDeadAirfield;
		case S::ReturnToDeadAirfield:  return bSuccess ? S::CirclingDeadAirfield : S::ReturnToDeadAirfield;
		case S::CirclingDeadAirfield:  return S::None;
		case S::ReloadAmmo:            return S::None;
		case S::TakingOff:             return S::None;
		default: break;
		}
		if (bNeedsRunway)
		{
			switch (State)
			{
			case S::TakeoffAwaitClearance: return bSuccess ? S::TaxiToTakeoff : S::None;
			case S::TaxiToTakeoff:         return bSuccess ? S::PauseBeforeTakeoff : S::None;
			case S::PauseBeforeTakeoff:    return bSuccess ? S::TakingOff : S::None;
			case S::LandingAwaitClearance: return bSuccess ? S::Landing : S::None;
			case S::Landing:               return bSuccess ? S::TaxiFromLanding : S::None;
			case S::TaxiFromLanding:       return bSuccess ? S::OrientForParking : S::None;
			case S::TaxiFromHangar:        return bSuccess ? S::OrientForParking : S::None;
			case S::OrientForParking:      return bSuccess ? S::ReloadAmmo : S::None;
			default: break;
			}
		}
		else
		{
			switch (State)
			{
			case S::TakeoffAwaitClearance: return bSuccess ? S::TakingOff : S::None;
			case S::LandingAwaitClearance: return bSuccess ? S::OrientForParking : S::None;
			case S::OrientForParking:      return bSuccess ? S::Landing : S::None;
			case S::Landing:               return bSuccess ? S::ReloadAmmo : S::None;
			default: break;
			}
		}
		return S::None;
	}

	/** intersectInfiniteLine2D: where the line through A heading AngleA meets the line through C heading AngleC. */
	bool IntersectLines(const FVector3f& A, float AngleA, const FVector3f& C, float AngleC, FVector3f& Out)
	{
		const float BX = A.X + FMath::Cos(AngleA), BY = A.Y + FMath::Sin(AngleA);
		const float DX = C.X + FMath::Cos(AngleC), DY = C.Y + FMath::Sin(AngleC);
		const float Denom = (BX - A.X) * (DY - C.Y) - (BY - A.Y) * (DX - C.X);
		if (FMath::Abs(Denom) < 1e-6f)
		{
			return false; // parallel
		}
		const float R = ((A.Y - C.Y) * (DX - C.X) - (A.X - C.X) * (DY - C.Y)) / Denom;
		Out.X = A.X + R * (BX - A.X);
		Out.Y = A.Y + R * (BY - A.Y);
		return true;
	}

	bool IsHelipadUnit(const FGenThingTemplate& T) { return T.IsKindOf(TEXT("PRODUCED_AT_HELIPAD")); }
}

// ---- the airfield (ParkingPlaceBehavior) --------------------------------------------------------

bool FGenParkingPlace::IsGone(FGenObjectID Id) const
{
	const FGenObject* O = Logic->FindObject(Id);
	return !O || O->IsDead() || O->IsDestroyed();
}

float FGenParkingPlace::GetLandingDeckHeightOffset() const
{
	return Airfield.GetTemplate()->ParkingDeckHeightOffset;
}

void FGenParkingPlace::BuildInfo()
{
	// The spaces and runways are bones of the airfield's model; found once the building stands.
	if (bGotInfo || Airfield.IsUnderConstruction() || Airfield.IsBeingSold())
	{
		return;
	}
	const FGenThingTemplate* T = Airfield.GetTemplate();
	UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
	const FGenDrawModule* Draw = T->PrimaryDraw();
	if (!Assets || !Draw)
	{
		return;
	}
	const FString Model = Draw->DefaultModel();
	const FTransform3f Xf = Airfield.GetTransform();
	const float Heading = Airfield.GetOrientation();
	auto Bone = [&](const FString& Name, FVector3f& OutPos, float* OutAngle)
	{
		TArray<FTransform3f> Bones;
		OutPos = Airfield.GetPosition();
		if (OutAngle) { *OutAngle = Heading; }
		if (Assets->GetPristineBones(Model, Name, 0, 1, Bones) == 0)
		{
			return false;
		}
		OutPos = Xf.TransformPositionNoScale(Bones[0].GetTranslation());
		if (OutAngle)
		{
			const FVector3f X = Bones[0].GetRotation().GetAxisX();
			*OutAngle = GenLogic::NormalizeAngle(Heading + FMath::Atan2(X.Y, X.X));
		}
		return true;
	};

	Spaces.Reset(T->ParkingRows * T->ParkingCols);
	if (T->bFlightDeck)
	{
		// A flight deck takes its spaces from the bones the INI names, walked in the order it lists them
		// (runway 1 space 1, runway 2 space 1, runway 1 space 2...). The game puts an aircraft on every
		// space as the deck is built: those are objects, not part of the ship's model, which is why a
		// carrier in the game has fighters standing on it. Parking here means standing on the deck, so the
		// deck's height is what counts as the ground under them.
		const FGenThingTemplate* Payload = T->FlightPayloadTemplate.IsEmpty() ? nullptr
			: FGenGameData::Get().FindTemplate(T->FlightPayloadTemplate);
		for (int32 Row = 0; Row < T->ParkingRows; ++Row)
		{
			for (int32 Col = 0; Col < T->ParkingCols; ++Col)
			{
				FSpace& S = Spaces.AddDefaulted_GetRef();
				S.Runway = Col;
				S.Row = Row;
				if (T->FlightRunways.IsValidIndex(Col) && T->FlightRunways[Col].SpaceBones.IsValidIndex(Row))
				{
					Bone(T->FlightRunways[Col].SpaceBones[Row], S.Location, &S.Orient);
				}
				// On a deck the space is the prep: the game puts the aircraft at the bone and measures
				// whether it stands at the front of its runway from that same point.
				S.Prep = S.Location;
				if (!Payload)
				{
					continue;
				}
				FGenObject* Jet = Logic->CreateObject(Payload, Airfield.GetOwner(), S.Location, S.Orient);
				if (!Jet)
				{
					continue;
				}
				Jet->Producer = Airfield.GetID();             // FlightDeckBehavior::buildInfo: setProducer(the ship)
				Jet->bStatusDeckHeightOffset = true;          // ... and the deck height offset
				S.Occupant = Jet->GetID();                   // ... and the plane is assigned to its space
			}
		}
	}
	else
	{
		for (int32 Row = 0; Row < T->ParkingRows; ++Row)
		{
			for (int32 Col = 0; Col < T->ParkingCols; ++Col)
			{
				FSpace& S = Spaces.AddDefaulted_GetRef();
				Bone(FString::Printf(TEXT("Runway%dPark%dHan"), Col + 1, Row + 1), S.HangarStart, &S.HangarOrient);
				Bone(FString::Printf(TEXT("Runway%dParking%d"), Col + 1, Row + 1), S.Location, &S.Orient);
				Bone(FString::Printf(TEXT("Runway%dPrep%d"), Col + 1, Row + 1), S.Prep, nullptr);
				S.Runway = Col;
			}
		}
	}
	if (T->bFlightDeck || T->bParkingHasRunways)
	{
		for (int32 Col = 0; Col < T->ParkingCols; ++Col)
		{
			const FGenThingTemplate::FFlightDeckRunway* RW = T->bFlightDeck ? &T->FlightRunways[Col] : nullptr;
			FRunway& R = Runways.AddDefaulted_GetRef();
			Bone(RW && !RW->TakeoffStart.IsEmpty() ? RW->TakeoffStart : FString::Printf(TEXT("RunwayStart%d"), Col + 1), R.Start, nullptr);
			Bone(RW && !RW->TakeoffEnd.IsEmpty() ? RW->TakeoffEnd : FString::Printf(TEXT("RunwayEnd%d"), Col + 1), R.End, nullptr);
			if (RW)
			{
				Bone(RW->LandingStart, R.LandingStart, nullptr);
				Bone(RW->LandingEnd, R.LandingEnd, nullptr);
				for (const FString& Name : RW->TaxiBones)
				{
					Bone(Name, R.Taxi.AddDefaulted_GetRef(), nullptr);
				}
				for (int32 i = 0; i < RW->CreationBones.Num(); ++i)
				{
					Bone(RW->CreationBones[i], R.Creation.AddDefaulted_GetRef(), i == 0 ? &R.StartOrient : nullptr);
				}
			}
		}
	}
	bHasHeliPark = Bone(TEXT("HeliPark01"), HeliPark, &HeliParkOrient);
	bGotInfo = true;
}

void FGenParkingPlace::PurgeDead()
{
	BuildInfo();
	for (FSpace& S : Spaces)
	{
		if (S.Occupant != GenInvalidObjectID && IsGone(S.Occupant))
		{
			S.Occupant = GenInvalidObjectID;
			S.bReservedForExit = false;
		}
	}
	for (FRunway& R : Runways)
	{
		if (R.InUseBy != GenInvalidObjectID && IsGone(R.InUseBy))
		{
			R.InUseBy = GenInvalidObjectID;
			R.bWasInLine = false;
		}
		if (R.InUseForLanding != GenInvalidObjectID && IsGone(R.InUseForLanding))
		{
			R.InUseForLanding = GenInvalidObjectID;
		}
		if (R.NextInLineForTakeoff != GenInvalidObjectID && IsGone(R.NextInLineForTakeoff))
		{
			R.NextInLineForTakeoff = GenInvalidObjectID;
		}
	}
	Healing.RemoveAll([this](FGenObjectID Id) { return IsGone(Id); });
}

void FGenParkingPlace::UpdateDoors(uint32 Frame)
{
	// A ship's deck has one door: the one its production goes through (the game's DOOR_1, the dock
	// animation). DOOR_2 and DOOR_3 are its ramps, and an airfield's per-space hangar doors do not exist
	// on a ship, so nothing here is a hangar door.
	if (IsFlightDeck())
	{
		return;
	}
	// ProductionUpdate::setHoldDoorOpen: the hangar door of a space stays open for as long as an aircraft
	// holds that space (parked there or out flying), and closes when the space is given up.
	const FGenThingTemplate* T = Airfield.GetTemplate();
	for (int32 i = 0; i < Spaces.Num(); ++i)
	{
		FSpace& S = Spaces[i];
		const bool bWantOpen = S.Occupant != GenInvalidObjectID;
		const uint8 Old = S.DoorState;
		switch (S.DoorState)
		{
		case 0: if (bWantOpen) { S.DoorState = 1; S.DoorTimer = Frame + uint32(T->DoorOpeningTime); } break;
		case 1: if (Frame >= S.DoorTimer) { S.DoorState = 2; } break;
		case 2: if (!bWantOpen) { S.DoorState = 3; S.DoorTimer = Frame + uint32(T->DoorCloseTime); } break;
		default: if (Frame >= S.DoorTimer) { S.DoorState = 0; } break;
		}
		if (S.DoorState != Old)
		{
			static const TCHAR* Suffix[4] = { nullptr, TEXT("OPENING"), TEXT("WAITING_OPEN"), TEXT("CLOSING") };
			for (int32 State = 1; State <= 3; ++State)
			{
				Airfield.SetModelCondition(GenConditions::Index(FString::Printf(TEXT("DOOR_%d_%s"), i + 1, Suffix[State])), S.DoorState == State);
			}
		}
	}
}

// ---- a ship's flight deck: the order it keeps, and the aircraft it sends -------------------------

bool FGenParkingPlace::IsFlightDeck() const
{
	return Airfield.GetTemplate()->bFlightDeck;
}

FGenObjectID FGenParkingPlace::GetAirfieldID() const
{
	return Airfield.GetID();
}

bool FGenParkingPlace::OrderDeck(EGenCommandType Type, const FVector3f& Pos, FGenObjectID Target, int32 Slot)
{
	if (!IsFlightDeck())
	{
		return false;
	}
	// FlightDeckBehavior::aiDoCommand keeps only the orders its aircraft can act on: hold a spot, attack a
	// thing, attack a place, attack-move to a place, or do nothing. Anything else takes the order off the
	// deck. The player's own way of pointing the ship at something is its bar button
	// Command_AircraftCarrierFire, whose weapon (AircraftCarrierOrderLaunch) has no projectile and 0.00001
	// damage - it is there to pick a target - so what it picks is taken as the deck's attack order, which is
	// what that button is for. The port has no separate order to do nothing, so the player's stop stands for
	// the game's AICMD_IDLE: the aircraft that were out are sent back.
	switch (Type)
	{
	case EGenCommandType::Guard:
	case EGenCommandType::AttackMove:
	case EGenCommandType::Attack:
	case EGenCommandType::ForceAttack:
	case EGenCommandType::FireWeapon:
	case EGenCommandType::FireWeaponAt:
	case EGenCommandType::Stop:
		DeckOrder = Type;
		DeckPos = Pos;
		DeckTarget = Target;
		DeckSlot = Slot;
		DeckOrderIsPosition = Target == GenInvalidObjectID;
		break;
	default:
		DeckOrder = EGenCommandType::None;
		DeckPos = FVector3f::ZeroVector;
		DeckTarget = GenInvalidObjectID;
		DeckOrderIsPosition = false;
		break;
	}
	PropagateOrdersToPlanes();
	return true;
}

bool FGenParkingPlace::HasTakeoffOrders()
{
	switch (DeckOrder)
	{
	case EGenCommandType::Guard:
	case EGenCommandType::AttackMove:
		return true;
	case EGenCommandType::Attack:
	case EGenCommandType::ForceAttack:
	case EGenCommandType::FireWeapon:
	case EGenCommandType::FireWeaponAt:
		// A place is always there; a thing may not be, and then the order is dropped.
		if (DeckOrderIsPosition || Logic->FindObject(DeckTarget))
		{
			return true;
		}
		DeckOrder = EGenCommandType::None;
		DeckTarget = GenInvalidObjectID;
		return false;
	default:
		return false; // ordered to do nothing: the deck keeps its aircraft where they are
	}
}

// Whether an aircraft is standing in its own space, or on the way into it - that is the aircraft the deck
// launches or moves up. One that is out flying something is not. (JetAIUpdate's isIdle and
// isTaxiingToParking; the game's isIdle is false while an order is held for after takeoff.)
bool FGenParkingPlace::IsPlaneStandingInItsSpace(const FGenObject& Jet) const
{
	switch (Jet.GetJetState())
	{
	case EGenJetState::TaxiFromHangar:
	case EGenJetState::TaxiFromLanding:
	case EGenJetState::OrientForParking:
	case EGenJetState::ReloadAmmo:
	case EGenJetState::TakeoffAwaitClearance:
	case EGenJetState::TaxiToTakeoff:
	case EGenJetState::PauseBeforeTakeoff:
	case EGenJetState::TakingOff:
		return true;
	default:
		break;
	}
	return !Jet.bJetPendingCommand && Jet.GetJetState() == EGenJetState::None && Jet.IsAIIdle();
}

// isAbleToGiveUpParkingSpace: an aircraft that is out, or gone, can hand its space to another one.
bool FGenParkingPlace::CanPlaneGiveUpSpace(FGenObject* Jet)
{
	if (!Jet || Jet->IsAirborneTarget())
	{
		return true;
	}
	if (IsPlaneStandingInItsSpace(*Jet))
	{
		return false; // this is the aircraft the deck works with
	}
	// It is here for something else. If the order it is holding is to come back to the ship, or the deck's
	// order is to do nothing, that order is dropped and the plane stays where it stands.
	if ((Jet->bJetPendingCommand && Jet->JetLastCommand.Type == FGenObject::FJetCommand::Land) || DeckOrder == EGenCommandType::Stop)
	{
		Jet->bJetPendingCommand = false; // friend_purgePendingCommand
		return false;
	}
	// It must not be waiting for its runway either.
	for (const FRunway& R : Runways)
	{
		if (R.InUseBy == Jet->GetID())
		{
			return false;
		}
	}
	return true;
}

// isAbleToMoveForward: an aircraft that stands still, or rearms where it stands, can be moved up.
bool FGenParkingPlace::CanPlaneMoveForward(const FGenObject& Jet) const
{
	if (Jet.IsAirborneTarget())
	{
		return false;
	}
	return Jet.GetJetState() == EGenJetState::ReloadAmmo
		|| (Jet.GetJetState() == EGenJetState::None && !Jet.bJetPendingCommand && Jet.IsAIIdle());
}

// isInPositionToTakeoff: only the aircraft at the start of a runway can be sent, and only when it is
// standing there ("distanceSqr < 10.0f", on the ground plane).
bool FGenParkingPlace::IsPlaneAtTakeoffPosition(const FGenObject& Jet) const
{
	const FSpace* S = Spaces.FindByPredicate([&Jet](const FSpace& Other) { return Other.Occupant == Jet.GetID(); });
	if (!S || S->Row != 0)
	{
		return false;
	}
	const FVector2f A(Jet.GetPosition().X, Jet.GetPosition().Y);
	const FVector2f B(S->Prep.X, S->Prep.Y);
	return (A - B).SizeSquared() < 10.f;
}

// propagateOrderToSpecificPlane: the deck's order, given to one aircraft as an AI order.
void FGenParkingPlace::PropagateOrderToPlane(FGenObject* Jet)
{
	if (!Jet)
	{
		return;
	}
	switch (DeckOrder)
	{
	case EGenCommandType::Guard:
		Jet->CommandGuardPosition(FVector2f(DeckPos.X, DeckPos.Y), EGenGuardMode::Normal);
		break;
	case EGenCommandType::AttackMove:
		Jet->CommandAttackMove(DeckPos);
		break;
	case EGenCommandType::Attack:
	case EGenCommandType::ForceAttack:
	case EGenCommandType::FireWeapon:
	case EGenCommandType::FireWeaponAt:
		if (DeckOrderIsPosition)
		{
			Jet->CommandAttackPosition(DeckPos, -1, DeckSlot); // NO_MAX_SHOTS_LIMIT
		}
		else if (Logic->FindObject(DeckTarget))
		{
			Jet->CommandAttack(DeckTarget, true); // aiForceAttackObject
		}
		break;
	case EGenCommandType::Stop:
		Jet->CommandLandAt(Airfield.GetID()); // aiEnter(the ship): back on the deck
		break;
	default:
		break;
	}
}

// The order goes to the aircraft that are already out; the ones standing on the deck are sent one at a time,
// when that deck's ramp is up (LaunchPlanes).
void FGenParkingPlace::PropagateOrdersToPlanes()
{
	for (const FSpace& S : Spaces)
	{
		if (S.Occupant == GenInvalidObjectID)
		{
			continue;
		}
		if (FGenObject* Jet = Logic->FindObject(S.Occupant))
		{
			if (CanPlaneGiveUpSpace(Jet))
			{
				PropagateOrderToPlane(Jet);
			}
		}
	}
}

// The first space of a runway is the ramp end of it: the only one the deck launches from.
FGenParkingPlace::FSpace* FGenParkingPlace::FrontSpace(int32 Runway)
{
	if (!bGotInfo)
	{
		return nullptr;
	}
	return Spaces.FindByPredicate([Runway](const FSpace& S) { return S.Runway == Runway && S.Row == 0; });
}

void FGenParkingPlace::SetRampState(int32 Runway, bool bUp)
{
	// The deck's ramps are the game's DOOR_2 / DOOR_3 model states, one pair per runway (the game's
	// clearAndSetModelConditionState: the one it sets is the opening or the closing state).
	const int32 Opening = GenConditions::Index(FString::Printf(TEXT("DOOR_%d_OPENING"), Runway + 2));
	const int32 Closing = GenConditions::Index(FString::Printf(TEXT("DOOR_%d_CLOSING"), Runway + 2));
	if (Opening < 0 || Closing < 0)
	{
		return;
	}
	Airfield.SetModelCondition(Opening, bUp);
	Airfield.SetModelCondition(Closing, !bUp);
}

void FGenParkingPlace::LaunchPlanes(uint32 Frame)
{
	const FGenThingTemplate* T = Airfield.GetTemplate();
	for (int32 R = 0; R < Runways.Num(); ++R)
	{
		FRunway& RW = Runways[R];
		const FSpace* Front = FrontSpace(R);
		FGenObject* Jet = Front ? Logic->FindObject(Front->Occupant) : nullptr;
		if (Jet && !CanPlaneGiveUpSpace(Jet) && IsPlaneAtTakeoffPosition(*Jet) && HasTakeoffOrders())
		{
			// One aircraft per runway per wave: the ramp goes up first, and the plane at the front of the
			// runway goes once it is up; the next one has to wait for the wave delay.
			if (RW.NextWaveFrame <= Frame)
			{
				if (!RW.bRampUp)
				{
					RW.bRampUp = true;
					RW.RampUpFrame = Frame + uint32(T->DeckLaunchRampFrames);
					RW.LowerRampFrame = MAX_uint32;
					SetRampState(R, true);
				}
				if (RW.bRampUp && RW.RampUpFrame <= Frame)
				{
					PropagateOrderToPlane(Jet);
					RW.NextWaveFrame = Frame + uint32(T->DeckLaunchWaveFrames);
					RW.CatapultFrame = Frame + uint32(T->DeckCatapultFireFrames);
					RW.LowerRampFrame = Frame + uint32(T->DeckLowerRampFrames);
				}
			}
		}
		// "Handle firing of the catapult steam effect upon launching planes."
		if (RW.CatapultFrame != 0 && RW.CatapultFrame <= Frame)
		{
			RW.CatapultFrame = 0;
			if (T->FlightRunways.IsValidIndex(R) && !T->FlightRunways[R].CatapultSystem.IsEmpty())
			{
				Logic->PlayParticleSystemAt(T->FlightRunways[R].CatapultSystem, RW.Start);
			}
		}
		if (RW.bRampUp && RW.LowerRampFrame <= Frame)
		{
			RW.bRampUp = false;
			SetRampState(R, false);
		}
	}
}

// ParkingCleanupPeriod: keep the aircraft near the front of the deck. A space whose aircraft is out gets the
// nearest one behind it that is standing still; one plane per runway per pass, and the ones behind that wait
// for the follow period instead of all moving at once.
void FGenParkingPlace::MovePlanesForward(uint32 Frame)
{
	if (Frame < NextCleanupFrame)
	{
		return;
	}
	const int32 Cols = Airfield.GetTemplate()->ParkingCols;
	NextCleanupFrame = Frame + uint32(Airfield.GetTemplate()->DeckCleanupFrames);
	if (!bGotInfo || Cols <= 0 || Spaces.Num() == 0)
	{
		return;
	}
	TArray<bool> Done;
	Done.Init(false, Cols);
	for (int32 i = 0; i < Spaces.Num(); ++i)
	{
		FSpace& S = Spaces[i];
		FGenObject* Out = Logic->FindObject(S.Occupant);
		if (Out && !CanPlaneGiveUpSpace(Out))
		{
			continue; // an aircraft stands in this space: nothing to fill
		}
		// The spaces are ordered runway by runway, so only look along this one.
		for (int32 J = i + Cols; J < Spaces.Num(); J += Cols)
		{
			if (Done[S.Runway])
			{
				continue;
			}
			FSpace& Back = Spaces[J];
			FGenObject* Standing = Logic->FindObject(Back.Occupant);
			if (Standing && CanPlaneMoveForward(*Standing))
			{
				// "Now handle assignment swap": the one that is out comes home to the space behind. The taxi state
				// works the path out from the assignment (and the status: it isn't coming from the hangar).
				Back.Occupant = Out ? Out->GetID() : GenInvalidObjectID;
				S.Occupant = Standing->GetID();
				Standing->bStatusReassignParking = true;
				Standing->SetJetState(EGenJetState::TaxiFromHangar, Frame);
				Done[S.Runway] = true;
				NextCleanupFrame = Frame + uint32(Airfield.GetTemplate()->DeckHumanFollowFrames);
			}
			break; // one look per space, then on to the next
		}
	}
}

void FGenParkingPlace::ReplaceLostPlanes(uint32 Frame)
{
	// A space with nobody assigned to it: its plane is lost. One at a time, from the hangar, whatever it costs (nothing).
	const FGenThingTemplate* T = Airfield.GetTemplate();
	const FGenThingTemplate* Payload = T->FlightPayloadTemplate.IsEmpty() ? nullptr : FGenGameData::Get().FindTemplate(T->FlightPayloadTemplate);
	if (!Payload || Airfield.Production.Num() > 0 || Frame < NextReplacementFrame || !FindEmptySpace())
	{
		return;
	}
	FGenObject::FProduction& P = Airfield.Production.AddDefaulted_GetRef();
	P.Unit = Payload;
	P.ExitDoor = 0; // ("Uses the same door for all production.")
	NextReplacementFrame = Frame + uint32(T->DeckReplacementFrames + T->DeckDockAnimationFrames);
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [jet f%u] #%u %s builds a replacement %s"), Frame, Airfield.GetID(), *T->Name, *Payload->Name);
}

bool FGenParkingPlace::CalcBestParkingAssignment(FGenObjectID Id, FVector3f& OutPos)
{
	// "Search for the front-most available space that doesn't have any planes blocking it. So start from the back and
	// keep looking at empty spaces until we find one with a plane blocking."
	const int32 Mine = Spaces.IndexOfByPredicate([Id](const FSpace& S) { return S.Occupant == Id; });
	if (Mine == INDEX_NONE)
	{
		return false;
	}
	OutPos = Spaces[Mine].Prep;
	bool bCheckForPlaneInWay = false;
	int32 Best = INDEX_NONE;
	FGenObject* BestJet = nullptr;
	for (int32 i = 0; i < Mine; ++i)
	{
		if (Spaces[i].Runway != Spaces[Mine].Runway)
		{
			continue;
		}
		FGenObject* Other = Logic->FindObject(Spaces[i].Occupant);
		if (!Other || CanPlaneGiveUpSpace(Other))
		{
			if (!bCheckForPlaneInWay)
			{
				Best = i;
				BestJet = Other;
				bCheckForPlaneInWay = true;
				OutPos = Spaces[i].Prep;
			}
		}
		else if (bCheckForPlaneInWay)
		{
			// "Ugh, there's a plane parked between us and the best spot!"
			bCheckForPlaneInWay = false;
			OutPos = Spaces[Mine].Prep;
			Best = INDEX_NONE;
		}
	}
	if (Best == INDEX_NONE)
	{
		return false;
	}
	Spaces[Mine].Occupant = BestJet ? BestJet->GetID() : GenInvalidObjectID;
	Spaces[Best].Occupant = Id;
	return true;
}

const TArray<FVector3f>* FGenParkingPlace::GetTaxiLocations(FGenObjectID Id) const
{
	const FSpace* S = Spaces.FindByPredicate([Id](const FSpace& Other) { return Other.Occupant == Id; });
	return S && Runways.IsValidIndex(S->Runway) ? &Runways[S->Runway].Taxi : nullptr;
}

const TArray<FVector3f>* FGenParkingPlace::GetCreationLocations(FGenObjectID Id) const
{
	const FSpace* S = Spaces.FindByPredicate([Id](const FSpace& Other) { return Other.Occupant == Id; });
	return S && Runways.IsValidIndex(S->Runway) ? &Runways[S->Runway].Creation : nullptr;
}

void FGenParkingPlace::ParkAllPlanes()
{
	if (!IsFlightDeck() || !bGotInfo)
	{
		return;
	}
	DeckOrder = EGenCommandType::None;
	DeckTarget = GenInvalidObjectID;
	for (FRunway& R : Runways)
	{
		R.InUseBy = R.InUseForLanding = GenInvalidObjectID;
	}
	// The aircraft are put forward first: the spaces in their order, the empty ones behind.
	TArray<FGenObjectID> Planes;
	for (FSpace& S : Spaces)
	{
		if (S.Occupant != GenInvalidObjectID && !IsGone(S.Occupant))
		{
			Planes.Add(S.Occupant);
		}
		S.Occupant = GenInvalidObjectID;
	}
	const uint32 Frame = Logic->GetFrame();
	for (int32 i = 0; i < Planes.Num(); ++i)
	{
		FSpace& S = Spaces[i];
		FGenObject* Jet = Logic->FindObject(Planes[i]);
		S.Occupant = Planes[i];
		Jet->SetJetState(EGenJetState::None, Frame);
		Jet->ClearAIMode();
		Jet->ClearAttack();
		Jet->CommandStop();
		Jet->bJetPendingCommand = false;
		Jet->bJetUseReturnLoco = false;
		Jet->bJetInterruptForReload = false;
		Jet->JetReturnToBaseFrame = 0;
		Jet->bJetAllowAirLoco = false;
		Jet->bStatusDeckHeightOffset = true;
		Jet->ChooseLocomotorSet(TEXT("SET_TAXIING"));
		Jet->Velocity = FVector3f::ZeroVector;
		Jet->SetPosition(S.Prep);
		Jet->SetOrientation(S.Orient);
		Jet->SnapshotPrevious();
		for (FGenWeaponInstance& WI : Jet->Weapons)
		{
			if (WI.Template)
			{
				WI.AmmoInClip = WI.Template->ClipSize;
				WI.bReloading = false;
			}
		}
	}
}

bool FGenParkingPlace::TakeFirstFreeSpace(FGenObjectID Id, FVector3f& OutPos, float& OutAngle)
{
	PurgeDead();
	FSpace* S = FindEmptySpace();
	if (!S || !Runways.IsValidIndex(S->Runway) || Runways[S->Runway].Creation.Num() == 0)
	{
		return false;
	}
	S->Occupant = Id;
	OutPos = Runways[S->Runway].Creation[0];
	OutAngle = Runways[S->Runway].StartOrient;
	return true;
}

void FGenParkingPlace::Update(uint32 Frame)
{
	PurgeDead();
	UpdateDoors(Frame);
	if (IsFlightDeck())
	{
		MovePlanesForward(Frame);
		ReplaceLostPlanes(Frame);
		LaunchPlanes(Frame);
	}
	// Aircraft on the ground here are repaired (five times a second is plenty).
	const int32 HealRateFrames = FMath::Max(1, GenLogic::GetLogicFPS() / 5);
	if (Healing.Num() == 0)
	{
		NextHealFrame = MAX_uint32;
		return;
	}
	if (NextHealFrame == MAX_uint32)
	{
		NextHealFrame = Frame + uint32(HealRateFrames);
	}
	if (Frame >= NextHealFrame)
	{
		NextHealFrame = Frame + uint32(HealRateFrames);
		const float Amount = float(HealRateFrames) * Airfield.GetTemplate()->ParkingHealPerSecond * GenLogic::SecondsPerFrame();
		for (FGenObjectID Id : Healing)
		{
			if (FGenObject* O = Logic->FindObject(Id))
			{
				O->AttemptHealing(Amount);
			}
		}
	}
}

bool FGenParkingPlace::HasReservedSpace(FGenObjectID Id) const
{
	return bGotInfo && Id != GenInvalidObjectID && Spaces.ContainsByPredicate([Id](const FSpace& S) { return S.Occupant == Id; });
}

FGenParkingPlace::FSpace* FGenParkingPlace::FindSpace(FGenObjectID Id)
{
	return (bGotInfo && Id != GenInvalidObjectID) ? Spaces.FindByPredicate([Id](const FSpace& S) { return S.Occupant == Id; }) : nullptr;
}

FGenParkingPlace::FSpace* FGenParkingPlace::FindEmptySpace()
{
	return bGotInfo ? Spaces.FindByPredicate([](const FSpace& S) { return S.Occupant == GenInvalidObjectID && !S.bReservedForExit; }) : nullptr;
}

bool FGenParkingPlace::HasAvailableSpaceFor(const FGenThingTemplate& Thing) const
{
	if (!bGotInfo)
	{
		return false;
	}
	if (IsHelipadUnit(Thing))
	{
		return true;
	}
	for (const FSpace& S : Spaces)
	{
		if ((S.Occupant == GenInvalidObjectID || IsGone(S.Occupant)) && !S.bReservedForExit)
		{
			return true;
		}
	}
	return false;
}

bool FGenParkingPlace::ShouldReserveDoorWhenQueued(const FGenThingTemplate& Thing) const
{
	return !IsHelipadUnit(Thing);
}

bool FGenParkingPlace::ReserveSpace(FGenObjectID Id, float ParkingOffset, FGenParkingInfo* OutInfo)
{
	PurgeDead();
	FSpace* S = FindSpace(Id);
	if (!S)
	{
		S = FindEmptySpace();
		if (!S)
		{
			return false; // full
		}
	}
	S->Occupant = Id;
	S->bReservedForExit = false;
	if (Airfield.GetTemplate()->ParkingDeckHeightOffset)
	{
		// ParkingPlaceBehavior: parked on a deck, the deck is what the ground under it is.
		if (FGenObject* O = Logic->FindObject(Id))
		{
			O->bStatusDeckHeightOffset = true;
		}
	}
	if (OutInfo)
	{
		CalcInfo(Id, *OutInfo);
		if (ParkingOffset != 0.f)
		{
			OutInfo->ParkingSpace.X += ParkingOffset * FMath::Cos(S->Orient);
			OutInfo->ParkingSpace.Y += ParkingOffset * FMath::Sin(S->Orient);
		}
	}
	return true;
}

bool FGenParkingPlace::CalcInfo(FGenObjectID Id, FGenParkingInfo& Info)
{
	const FSpace* S = FindSpace(Id);
	if (!S || !Runways.IsValidIndex(S->Runway))
	{
		return false;
	}
	const FGenThingTemplate* T = Airfield.GetTemplate();
	const FRunway& R = Runways[S->Runway];
	if (T->bFlightDeck)
	{
		// FlightDeckBehavior::calcPPInfo: the space is all there is to it; beyond the strips' ends, 3/4 of their length on
		// and the approach height over the deck, is where a plane is up, or starts down.
		const float Above = T->ParkingApproachHeight + T->ParkingDeckHeightOffset;
		Info.ParkingSpace = Info.RunwayPrep = S->Prep;
		Info.ParkingOrientation = S->Orient;
		Info.RunwayStart = R.Start;
		Info.RunwayEnd = R.End;
		Info.RunwayExit = R.End + (R.End - R.Start) * ApproachDist;
		Info.RunwayExit.Z = R.End.Z + Above;
		Info.RunwayLandingStart = R.LandingStart;
		Info.RunwayLandingEnd = R.LandingEnd;
		Info.RunwayApproach = R.LandingStart + (R.LandingStart - R.LandingEnd) * ApproachDist;
		Info.RunwayApproach.Z = R.LandingStart.Z + Above;
		Info.RunwayTakeoffDist = (R.Start - R.End).Size();
		// (The one with the clearance goes from where it stands: there is no walking back to the bone.)
		if (R.InUseBy == Id)
		{
			Info.RunwayStart = Info.RunwayPrep;
		}
		return true;
	}
	Info.ParkingSpace = T->bParkInHangars ? S->HangarStart : S->Location;
	Info.ParkingOrientation = T->bParkInHangars ? S->HangarOrient : S->Orient;
	Info.RunwayPrep = S->Prep;
	Info.RunwayStart = R.Start;
	Info.RunwayEnd = R.End;
	Info.RunwayApproach = R.End;
	Info.RunwayApproach.X += (R.End.X - R.Start.X) * ApproachDist;
	Info.RunwayApproach.Y += (R.End.Y - R.Start.Y) * ApproachDist;
	Info.RunwayApproach.Z = R.End.Z + T->ParkingApproachHeight;
	Info.RunwayExit = Info.RunwayApproach;
	Info.HangarInternal = S->HangarStart;
	Info.HangarInternalOrient = S->HangarOrient;
	Info.RunwayTakeoffDist = (Info.RunwayStart - Info.RunwayEnd).Size();
	// The second in line starts its roll from where it waited.
	if (R.InUseBy == Id && R.bWasInLine)
	{
		Info.RunwayStart = Info.RunwayPrep;
	}
	return true;
}

void FGenParkingPlace::ReleaseSpace(FGenObjectID Id)
{
	PurgeDead();
	for (FSpace& S : Spaces)
	{
		if (S.Occupant == Id)
		{
			S.Occupant = GenInvalidObjectID;
			S.bReservedForExit = false;
			if (Airfield.GetTemplate()->ParkingDeckHeightOffset)
			{
				if (FGenObject* O = Logic->FindObject(Id))
				{
					O->bStatusDeckHeightOffset = false;
				}
			}
		}
	}
}

FGenObjectID FGenParkingPlace::GetRunwayReservation(int32 Runway)
{
	PurgeDead();
	return Runways.IsValidIndex(Runway) ? Runways[Runway].InUseBy : GenInvalidObjectID;
}

void FGenParkingPlace::TransferRunwayReservationToNextInLineForTakeoff(FGenObjectID Id)
{
	if (IsFlightDeck())
	{
		return; // "Aircraft carrier controls this functionality with an iron fist."
	}
	PurgeDead();
	for (FRunway& R : Runways)
	{
		if (R.InUseBy == Id && R.NextInLineForTakeoff != GenInvalidObjectID)
		{
			R.InUseBy = R.NextInLineForTakeoff;
			R.bWasInLine = true;
			R.NextInLineForTakeoff = GenInvalidObjectID;
		}
	}
}

bool FGenParkingPlace::ReserveRunway(FGenObjectID Id, bool bForLanding)
{
	PurgeDead();
	const FSpace* S = FindSpace(Id);
	if (!S || !Runways.IsValidIndex(S->Runway))
	{
		return false; // only aircraft with a space get a runway
	}
	FRunway& R = Runways[S->Runway];
	if (IsFlightDeck())
	{
		// FlightDeckBehavior::reserveRunway: a runway's taking off and its landing are two things, one plane each; and
		// "Only look at the front spaces for takeoff. You can't take off from the back!"
		if (!bForLanding && S->Row != 0)
		{
			return false;
		}
		FGenObjectID& Holder = bForLanding ? R.InUseForLanding : R.InUseBy;
		if (Holder == GenInvalidObjectID)
		{
			Holder = Id;
		}
		return Holder == Id;
	}
	if (R.InUseBy == Id)
	{
		return true;
	}
	if (R.InUseBy == GenInvalidObjectID)
	{
		R.InUseBy = Id;
		R.bWasInLine = R.NextInLineForTakeoff == Id;
		if (R.bWasInLine)
		{
			R.NextInLineForTakeoff = GenInvalidObjectID;
		}
		return true;
	}
	if (!bForLanding && R.NextInLineForTakeoff == GenInvalidObjectID)
	{
		R.NextInLineForTakeoff = Id; // next in line, but not cleared yet
	}
	return false;
}

void FGenParkingPlace::ReleaseRunway(FGenObjectID Id)
{
	PurgeDead();
	for (FRunway& R : Runways)
	{
		if (R.InUseBy == Id)
		{
			R.InUseBy = GenInvalidObjectID;
			R.bWasInLine = false;
		}
		if (R.InUseForLanding == Id)
		{
			R.InUseForLanding = GenInvalidObjectID;
		}
		if (R.NextInLineForTakeoff == Id)
		{
			R.NextInLineForTakeoff = GenInvalidObjectID;
		}
	}
}

void FGenParkingPlace::SetHealee(FGenObjectID Id, bool bAdd)
{
	if (bAdd)
	{
		Healing.AddUnique(Id);
	}
	else
	{
		Healing.Remove(Id);
	}
}

void FGenParkingPlace::KillAllParkedUnits()
{
	PurgeDead();
	for (const FSpace& S : Spaces)
	{
		FGenObject* O = Logic->FindObject(S.Occupant);
		if (!O || O->IsDead())
		{
			continue;
		}
		// Those in the air and not in the middle of a takeoff or landing get away.
		const bool bAboveTerrain = O->GetPosition().Z > Logic->GetGroundHeight(O->GetPosition().X, O->GetPosition().Y);
		if (bAboveTerrain && !O->bJetTakeoffInProgress && !O->bJetLandingInProgress)
		{
			continue;
		}
		O->Kill();
	}
	PurgeDead();
}

int32 FGenParkingPlace::ReserveDoorForExit(const FGenThingTemplate& Thing)
{
	PurgeDead();
	if (IsHelipadUnit(Thing))
	{
		return DoorNoneNeeded;
	}
	if (FSpace* S = FindEmptySpace())
	{
		S->bReservedForExit = true;
		return int32(S - Spaces.GetData());
	}
	return DoorNoneAvailable;
}

void FGenParkingPlace::UnreserveDoorForExit(int32 Door)
{
	if (Spaces.IsValidIndex(Door) && Spaces[Door].bReservedForExit)
	{
		Spaces[Door].Occupant = GenInvalidObjectID;
		Spaces[Door].bReservedForExit = false;
	}
}

bool FGenParkingPlace::ClaimDoor(FGenObjectID Id, int32 Door)
{
	if (!Spaces.IsValidIndex(Door) || !Spaces[Door].bReservedForExit || Spaces[Door].Occupant != GenInvalidObjectID)
	{
		return false;
	}
	Spaces[Door].Occupant = Id;
	Spaces[Door].bReservedForExit = false;
	return true;
}

bool FGenParkingPlace::GetHeliPark(FVector3f& OutPos, float& OutAngle)
{
	BuildInfo();
	if (!bHasHeliPark)
	{
		return false;
	}
	OutPos = HeliPark;
	OutAngle = HeliParkOrient;
	return true;
}

// ---- aircraft: helpers --------------------------------------------------------------------------

void FGenObject::InitJet()
{
	if (Template->bParkingPlace)
	{
		Parking = MakeUnique<FGenParkingPlace>(*this, Logic);
	}
	bJetAI = Template->bJetAI;
	if (bJetAI)
	{
		// JetAIUpdate::onObjectCreated: on the ground until we know better (see JetGetProducerLocation).
		bJetAllowAirLoco = false;
		ChooseLocomotorSet(TEXT("SET_TAXIING"));
	}
}

void FGenObject::ChooseLocomotorSet(const TCHAR* SetName)
{
	FString Name = SetName;
	if (bJetAI)
	{
		// JetAIUpdate::chooseLocomotorSet
		if (!bJetAllowAirLoco)
		{
			Name = TEXT("SET_TAXIING");
		}
		else if (JetAttackLocoExpireFrame != 0 && !Template->JetAttackLocoSet.IsEmpty())
		{
			Name = Template->JetAttackLocoSet;
		}
		else if (bJetUseReturnLoco && !Template->JetReturnLocoSet.IsEmpty())
		{
			Name = Template->JetReturnLocoSet;
		}
	}
	if (bLocoUpgraded && Name == TEXT("SET_NORMAL") && Template->LocomotorSets.Contains(TEXT("SET_NORMAL_UPGRADED")))
	{
		Name = TEXT("SET_NORMAL_UPGRADED"); // LocomotorSetUpgrade
	}
	const FGenLocomotorTemplate* New = nullptr;
	for (const FString& Try : { Name, FString(TEXT("SET_NORMAL")) })
	{
		if (const TArray<FString>* Set = Template->LocomotorSets.Find(Try))
		{
			SetLocomotorSet(*Set);
			// (chooseLocomotorSet -> chooseGoodLocomotorFromCurrentSet: the one for where the unit is.)
			New = LocoSet.Num() > 1 ? FindLocomotorForPosition() : nullptr;
			if (!New && LocoSet.Num() > 0)
			{
				New = LocoSet[0];
			}
		}
		if (New)
		{
			bLocoSupersonic = Try == TEXT("SET_SUPERSONIC");
			break;
		}
	}
	if (!New || New == Locomotor)
	{
		return;
	}
	const bool bWasAir = IsUsingAirLoco();
	Locomotor = New;
	// Special handling is switched off whenever the locomotor changes; states that want it ask again.
	bLocoPreciseZ = false;
	bLocoUltraAccurate = false;
	bLocoBraking = false;
	BrakingFactor = 1.f;
	bMaintainPosValid = false;
	if (bWasAir && !IsUsingAirLoco())
	{
		// Wheels down: level out.
		BodyPitch = BodyPitchRate = BodyRoll = BodyRollRate = 0.f;
		SetOrientation(Orientation);
		bFreeRotation = false;
		Velocity.Z = 0.f;
	}
}

FGenParkingPlace* FGenObject::GetProducerParking(FGenObject** OutAirfield) const
{
	// getPP: the airfield we belong to, as long as it stands.
	if (OutAirfield) { *OutAirfield = nullptr; }
	FGenObject* A = Logic->FindObject(Producer);
	if (!A || A->bDead || A->bPendingDestroy || A->SellFrame != 0 || !A->Parking || !A->IsKindOf(TEXT("FS_AIRFIELD")))
	{
		return nullptr;
	}
	if (OutAirfield) { *OutAirfield = A; }
	return A->Parking.Get();
}

FGenObject* FGenObject::FindSuitableAirfield() const
{
	// The closest airfield of ours (or an ally's) with a space to spare.
	FGenObject* Best = nullptr;
	float BestDistSq = FLT_MAX;
	for (FGenObjectID Id : Logic->GetObjectIDs())
	{
		FGenObject* A = Logic->FindObject(Id);
		if (!A || !A->Parking || A->bDead || A->bPendingDestroy || A->bUnderConstruction || A->SellFrame != 0 || !A->IsKindOf(TEXT("FS_AIRFIELD"))
			|| Logic->GetRelationship(Owner, A->Owner) != EGenRelationship::Allies)
		{
			continue;
		}
		if (!A->Parking->HasReservedSpace(ID) && !A->Parking->HasAvailableSpaceFor(*Template))
		{
			continue;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(Position.X, Position.Y), FVector2f(A->Position.X, A->Position.Y));
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Best = A;
		}
	}
	return Best;
}

bool FGenObject::IsParkedAt(FGenObjectID AirfieldId) const
{
	if (bJetAllowAirLoco || IsHelipadUnit(*Template) || AirfieldId == GenInvalidObjectID)
	{
		return false;
	}
	FGenObject* A = nullptr;
	return GetProducerParking(&A) && A && A->ID == AirfieldId;
}

bool FGenObject::IsWeaponOutOfAmmo(int32 Slot) const
{
	const FGenWeaponInstance& WI = Weapons[Slot];
	return WI.Template && WI.Template->ClipSize > 0 && WI.AmmoInClip <= 0 && WI.Template->ReloadType != EGenReloadType::Auto;
}

bool FGenObject::IsOutOfSpecialReloadAmmo() const
{
	// At least one weapon that reloads only at base, and all of those are empty.
	int32 Specials = 0, Out = 0;
	for (int32 s = 0; s < 3; ++s)
	{
		const FGenWeaponInstance& WI = Weapons[s];
		if (!WI.Template || WI.Template->ReloadType != EGenReloadType::ReturnToBase)
		{
			continue;
		}
		++Specials;
		if (WI.Template->ClipSize > 0 && WI.AmmoInClip <= 0)
		{
			++Out;
		}
	}
	return Specials > 0 && Out == Specials;
}

float FGenObject::GetDrawLift(const FVector3f& RenderPos) const
{
	// JetAIUpdate::update: near the ground the model is raised to MinHeight, so the wheels stand on the tarmac.
	if (!bJetAI || bDead || Template->JetMinHeight <= 0.f)
	{
		return 0.f;
	}
	// ("minHeight += pp->getLandingDeckHeightOffset()": on a deck they stand that much over the bones, which are in it.)
	const float MinHeight = Template->JetMinHeight + GetCarrierDeckHeight();
	const float Height = FMath::Max(0.f, RenderPos.Z - Logic->GetGroundHeight(RenderPos.X, RenderPos.Y));
	return Height < MinHeight ? MinHeight - Height : 0.f;
}

void FGenObject::JetEnableAfterburners(bool bOn)
{
	bJetAfterburners = bOn;
	SetModelCondition(JetAfterburnerBit(), bOn);
}

void FGenObject::JetGetProducerLocation()
{
	if (bJetHasProducerLocation)
	{
		return;
	}
	bJetHasProducerLocation = true;
	const FGenObject* A = Logic->FindObject(Producer);
	JetProducerLocation = A ? A->Position : Position;
	// An aircraft without a parking space was placed on the map directly (or built on a helipad): it may fly at once.
	FGenParkingPlace* PP = GetProducerParking();
	bJetAllowAirLoco = !PP || !PP->HasReservedSpace(ID);
	ChooseLocomotorSet(bJetAllowAirLoco ? TEXT("SET_NORMAL") : TEXT("SET_TAXIING"));
}

// ---- aircraft: orders ---------------------------------------------------------------------------

bool FGenObject::JetInterceptCommand(const FJetCommand& Cmd)
{
	// JetAIUpdate::aiDoCommand
	const uint32 Frame = Logic->GetFrame();
	JetGetProducerLocation();
	JetLastCommand = Cmd; // always remembered, even when nothing ends up pending
	const bool bHelipad = IsHelipadUnit(*Template);

	if (bJetTakeoffInProgress || bJetLandingInProgress)
	{
		bJetPendingCommand = true; // has to wait until we are up (or down)
		return true;
	}
	if (Cmd.Type == FJetCommand::Stop && JetState == EGenJetState::ReloadAmmo)
	{
		bJetPendingCommand = true; // already doing nothing; obeying would stop the reload
		return true;
	}
	if (Cmd.Type == FJetCommand::Stop && IsAirborneTarget() && !bHelipad)
	{
		SetJetState(EGenJetState::ReturningForLanding, Frame); // a plane told to stop goes home
		return true;
	}
	if (!bJetAllowAirLoco)
	{
		if (Cmd.Type == FJetCommand::Land && IsParkedAt(Cmd.Target))
		{
			return true; // already there
		}
		if (Cmd.Type != FJetCommand::Stop)
		{
			bJetPendingCommand = true;
			SetJetState(EGenJetState::TakeoffAwaitClearance, Frame);
			return true;
		}
	}
	// Hunting and guarding carry on after a trip home for ammunition.
	bJetInterruptForReload = Cmd.Type == FJetCommand::Hunt || Cmd.Type == FJetCommand::Guard;
	bJetPendingCommand = false;
	if (JetState != EGenJetState::None)
	{
		SetJetState(EGenJetState::None, Frame); // the order replaces the trip home
	}
	return false;
}

void FGenObject::JetDoCommand(const FJetCommand& InCmd)
{
	const FJetCommand Cmd = InCmd; // the calls below store their own copy
	switch (Cmd.Type)
	{
	case FJetCommand::Move:       CommandMoveTo(Cmd.Pos); break;
	case FJetCommand::Attack:     CommandAttack(Cmd.Target); break;
	case FJetCommand::Hunt:       CommandHunt(); break;
	case FJetCommand::Guard:      CommandGuard(FVector2f(Cmd.Pos.X, Cmd.Pos.Y), Cmd.Radius, Cmd.Area); break;
	case FJetCommand::FollowPath: CommandFollowPath(Cmd.Points); break;
	case FJetCommand::Land:       CommandLandAt(Cmd.Target); break;
	default: break;
	}
}

void FGenObject::CommandIdle()
{
	if (bDead)
	{
		return;
	}
	if (bJetAI)
	{
		FJetCommand Cmd;
		Cmd.Type = FJetCommand::Stop;
		if (JetInterceptCommand(Cmd))
		{
			return;
		}
	}
	AIMode = EGenAIMode::None;
	ClearAttack();
	CommandStop();
}

void FGenObject::CommandLandAt(FGenObjectID AirfieldId)
{
	if (!bJetAI || bDead)
	{
		return;
	}
	FJetCommand Cmd;
	Cmd.Type = FJetCommand::Land;
	Cmd.Target = AirfieldId;
	if (JetInterceptCommand(Cmd))
	{
		return;
	}
	// JetAIUpdate::privateEnter -> doLandingCommand
	FGenObject* A = Logic->FindObject(AirfieldId);
	if (!A || !A->Parking || A->bDead || A->bUnderConstruction || A->SellFrame != 0
		|| Logic->GetRelationship(Owner, A->Owner) != EGenRelationship::Allies)
	{
		return;
	}
	const bool bHelipad = IsHelipadUnit(*Template);
	if (bHelipad)
	{
		// A helicopter comes down on open ground beside the airfield (not all on the one helipad).
		JetHelipadLanding = Logic->FindFreeGroundAround(A->Position, A->Template->GetBoundingCircleRadius() * 10.f);
	}
	if (bHelipad || A->Parking->ReserveSpace(ID, Template->JetParkingOffset, nullptr))
	{
		// A space held at another airfield is given up.
		FGenParkingPlace* Old = GetProducerParking();
		if (Old && Old != A->Parking.Get())
		{
			Old->ReleaseSpace(ID);
		}
		Producer = AirfieldId;
		bJetUseReturnLoco = false;
		bJetInterruptForReload = false;
		SetJetState(EGenJetState::ReturningForLanding, Logic->GetFrame());
	}
}

// ---- aircraft: per-frame (JetAIUpdate::update) --------------------------------------------------

void FGenObject::UpdateJet(uint32 Frame)
{
	JetGetProducerLocation();
	FGenParkingPlace* PP = GetProducerParking();
	const bool bHelipad = IsHelipadUnit(*Template);

	// AIUpdateInterface::isIdle (ours also says "not idle" while an order is pending)
	const bool bIdle = JetState == EGenJetState::None && !bMoving && DockTarget == GenInvalidObjectID && BuildTarget == GenInvalidObjectID
		&& TargetID == GenInvalidObjectID && AIMode == EGenAIMode::None;
	if (bIdle || JetState == EGenJetState::ReloadAmmo)
	{
		if (PP)
		{
			if (!bJetAllowAirLoco && !bJetPendingCommand && bHelipad && Health >= MaxHealth)
			{
				// A helicopter that landed for repairs is whole again: up it goes.
				PP->SetHealee(ID, false);
				bJetAllowAirLoco = true;
				SetJetState(EGenJetState::TakeoffAwaitClearance, Frame);
			}
			else
			{
				PP->SetHealee(ID, !bJetAllowAirLoco);
			}
		}

		// (There may be other weapons with ammunition left; the special ones decide.)
		if (IsOutOfSpecialReloadAmmo() && bJetAllowAirLoco)
		{
			JetReturnToBaseFrame = 0;
			bJetUseReturnLoco = true;
			SetJetState(EGenJetState::ReturningForLanding, Frame);
		}
		else if (bJetPendingCommand && JetState != EGenJetState::ReloadAmmo)
		{
			JetReturnToBaseFrame = 0;
			bJetPendingCommand = false;
			JetDoCommand(JetLastCommand);
		}
		else if (JetReturnToBaseFrame != 0 && Frame >= JetReturnToBaseFrame && bJetAllowAirLoco)
		{
			JetReturnToBaseFrame = 0;
			bJetUseReturnLoco = false;
			SetJetState(EGenJetState::ReturningForLanding, Frame);
		}
		else if (JetReturnToBaseFrame == 0 && Template->JetReturnToBaseIdleTime > 0 && bJetAllowAirLoco)
		{
			JetReturnToBaseFrame = Frame + uint32(Template->JetReturnToBaseIdleTime);
		}
	}
	else
	{
		if (PP)
		{
			PP->SetHealee(ID, false);
		}
		JetReturnToBaseFrame = 0;
		if (bJetInterruptForReload && IsOutOfSpecialReloadAmmo() && bJetAllowAirLoco)
		{
			// Out of ammunition in the middle of a hunt or a guard: home to reload, then back to it.
			bJetUseReturnLoco = true;
			bJetPendingCommand = true;
			bJetInterruptForReload = false;
			SetJetState(EGenJetState::ReturningForLanding, Frame);
		}
	}

	SetModelCondition(JetExhaustBit(), bJetAllowAirLoco && !Velocity.IsNearlyZero(0.0001f));

	// AttackLocomotorType (the Aurora goes supersonic for its attack run) and ReturnForAmmoLocomotorType.
	if (TargetID != GenInvalidObjectID)
	{
		JetAttackLocoExpireFrame = Frame + uint32(FMath::Max(1, Template->JetAttackLocoPersistTime));
	}
	else if (JetAttackLocoExpireFrame != 0 && Frame >= JetAttackLocoExpireFrame)
	{
		JetAttackLocoExpireFrame = 0;
	}
	// AttackersMissPersistTime: "how long after our attack we continue immunity"
	if (TargetID != GenInvalidObjectID)
	{
		JetAttackersMissExpireFrame = Frame + uint32(Template->JetAttackersMissPersistTime);
	}
	else if (JetAttackersMissExpireFrame != 0 && Frame >= JetAttackersMissExpireFrame)
	{
		JetAttackersMissExpireFrame = 0;
	}
	if (bJetAllowAirLoco && (!Template->JetAttackLocoSet.IsEmpty() || !Template->JetReturnLocoSet.IsEmpty()))
	{
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
	}

	if (JetState != EGenJetState::None)
	{
		UE_CLOG(Logic->IsTracing() && Frame % 6 == 0 && JetState != EGenJetState::ReloadAmmo, LogGenerals, Log,
			TEXT("  [jetpos f%u] #%u %s leg %d pos=(%.1f, %.1f) alt=%.1f heading=%.0f speed=%.2f vz=%.2f lift=%.3f"), Frame, ID, JetStateName(JetState),
			JetPathIndex, Position.X, Position.Y, Position.Z - Logic->GetGroundHeight(Position.X, Position.Y), FMath::RadiansToDegrees(Orientation),
			FVector2f(Velocity.X, Velocity.Y).Size(), Velocity.Z, FMath::Min(LocoMaxLift, Locomotor->Lift));
		const EJetResult Result = JetUpdateState(Frame);
		if (Result != EJetResult::Continue)
		{
			SetJetState(NextJetState(JetState, Result == EJetResult::Success, Template->bJetNeedsRunway), Frame);
		}
	}
}

void FGenObject::SetJetState(EGenJetState NewState, uint32 Frame)
{
	// A state whose entry already succeeds or fails leads straight on to the next one.
	for (int32 Guard = 0; Guard < 16; ++Guard)
	{
		const EGenJetState Old = JetState;
		if (Old != EGenJetState::None)
		{
			JetExitState();
		}
		JetState = NewState;
		UE_CLOG(Logic->IsTracing() && Old != NewState, LogGenerals, Log, TEXT("  [jet f%u] #%u %s: %s -> %s"), Frame, ID, *Template->Name,
			JetStateName(Old), JetStateName(NewState));
		if (NewState == EGenJetState::None)
		{
			return;
		}
		// StateMachine::clear: whatever the aircraft was doing is dropped (what matters is kept in JetLastCommand).
		AIMode = EGenAIMode::None;
		ClearAttack();
		StopDocking();
		bMoving = false;
		Path.Reset();
		bJetBreakingOff = false;
		const EJetResult Result = JetEnterState(Frame);
		if (Result == EJetResult::Continue)
		{
			return;
		}
		NewState = NextJetState(JetState, Result == EJetResult::Success, Template->bJetNeedsRunway);
	}
}

void FGenObject::JetStartPath(bool bGround)
{
	JetPathIndex = 0;
	JetPathFrom = Position;
	JetLegFrom = JetPathFrom;
	bJetPathOnGround = bGround;
	bLocoBraking = false;
	BrakingFactor = 1.f;
}

bool FGenObject::JetFollowPath(uint32 Frame, float DesiredSpeed)
{
	if (JetPath.Num() == 0)
	{
		return true;
	}
	bJetDriving = true;
	const FVector2f Pos(Position.X, Position.Y);
	if (bJetPathOnGround)
	{
		// Taxiing is exact: every corner of the route is driven to, and the last point is hit precisely.
		const float Step = FMath::Max(Locomotor->Speed, 0.5f);
		while (JetPathIndex < JetPath.Num() - 1 && FVector2f::Distance(Pos, FVector2f(JetPath[JetPathIndex].X, JetPath[JetPathIndex].Y)) <= Step * 1.25f)
		{
			++JetPathIndex;
		}
		const FVector3f Target = JetPath[JetPathIndex];
		float Dist = FVector2f::Distance(Pos, FVector2f(Target.X, Target.Y));
		if (JetPathIndex == JetPath.Num() - 1 && Dist <= Step)
		{
			Position.X = Target.X;
			Position.Y = Target.Y;
			Velocity = FVector3f::ZeroVector;
			bStatusBraking = false;
			bLocoBraking = false;
			return true;
		}
		for (int32 i = JetPathIndex + 1; i < JetPath.Num(); ++i)
		{
			Dist += FVector2f::Distance(FVector2f(JetPath[i - 1].X, JetPath[i - 1].Y), FVector2f(JetPath[i].X, JetPath[i].Y));
		}
		LocoMoveTowardsPosition(Target, Dist, DesiredSpeed, Frame);
		return false;
	}

	// In the air a leg is done once the aircraft is abeam of its end (Path::computeFlightDistToGoal);
	// a helicopter, which can stop, has to get there: over the spot on a flight home (AIInternalMoveToState,
	// at its own height), within 3 units in all three axes on its way straight up or down.
	const bool bPlane = HasMinSpeed();
	const bool bHeliUpOrDown = JetState == EGenJetState::TakingOff || JetState == EGenJetState::Landing;
	float Ahead = 0.f;
	bool bAdvanced = false;
	while (JetPathIndex < JetPath.Num())
	{
		const FVector3f& To = JetPath[JetPathIndex];
		const FVector2f To2(To.X, To.Y);
		const float Direct = FVector2f::Distance(Pos, To2);
		if (bPlane)
		{
			// (AIFollowPathState: a next point less than a cell away is not worth turning for.)
			const FVector2f Dir = (To2 - FVector2f(JetLegFrom.X, JetLegFrom.Y)).GetSafeNormal();
			Ahead = FMath::Min(FVector2f::DotProduct(To2 - Pos, Dir), Direct);
			if (Ahead >= FMath::Max(Locomotor->CloseEnoughDist, 1.f) && !(bAdvanced && Direct < GenLogic::PathfindCellSize))
			{
				break;
			}
		}
		else if (bHeliUpOrDown)
		{
			// (setLocomotorGoalPositionExplicit: the locomotor is told no distance along a path, so it brakes
			// and slides onto the point instead of circling it.)
			Ahead = 0.f;
			if ((To - Position).SizeSquared() > 9.f) // HeliTakeoffOrLandingState: within 3 units, in all three axes
			{
				break;
			}
		}
		else
		{
			Ahead = Direct;
			if (Direct > FMath::Max(Locomotor->CloseEnoughDist, 1.f))
			{
				break;
			}
		}
		++JetPathIndex;
		JetLegFrom = Position; // the next leg is measured from here
		bAdvanced = true;
	}
	if (JetPathIndex >= JetPath.Num())
	{
		return true;
	}
	PreciseGoalZ = JetPath[JetPathIndex].Z;
	LocoMoveTowardsPosition(JetPath[JetPathIndex], Ahead, DesiredSpeed, Frame);
	return false;
}

bool FGenObject::JetFaceAngle(float Angle, float Tolerance)
{
	const float Rate = Locomotor->TurnRate;
	SetOrientation(Orientation + FMath::Clamp(GenLogic::AngleDiff(Angle, Orientation), -Rate, Rate));
	return FMath::Abs(GenLogic::AngleDiff(Angle, Orientation)) <= Tolerance;
}

// ---- aircraft: the states -----------------------------------------------------------------------

FGenObject::EJetResult FGenObject::JetEnterState(uint32 Frame)
{
	using S = EGenJetState;
	const bool bHelipad = IsHelipadUnit(*Template);
	const bool bPlane = Template->bJetNeedsRunway;
	FGenObject* Airfield = nullptr;
	FGenParkingPlace* PP = GetProducerParking(&Airfield);
	FGenParkingInfo Info;

	switch (JetState)
	{
	case S::ReturningForLanding:
	{
		// JetOrHeliReturnForLandingState
		if (!PP)
		{
			// Home is gone: any airfield with room will do.
			Producer = GenInvalidObjectID;
			Airfield = FindSuitableAirfield();
			PP = Airfield ? Airfield->Parking.Get() : nullptr;
			if (!PP)
			{
				return EJetResult::Failure;
			}
			Producer = Airfield->ID;
		}
		FVector3f GoalPos = JetHelipadLanding;
		if (!bHelipad)
		{
			if (!PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
			{
				return EJetResult::Failure;
			}
			GoalPos = bPlane ? Info.RunwayApproach : Info.ParkingSpace;
		}
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
		JetPath.Reset();
		JetPath.Add(GoalPos);
		JetStartPath(false);
		return EJetResult::Continue;
	}

	case S::ReturnToDeadAirfield:
		// JetOrHeliReturningToDeadAirfieldState
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
		JetPath.Reset();
		JetPath.Add(JetProducerLocation);
		JetStartPath(false);
		return EJetResult::Continue;

	case S::CirclingDeadAirfield:
		// An aircraft that was simply placed on the map, and came "home" only because it was idle, just idles.
		if (!IsOutOfSpecialReloadAmmo() && Producer == GenInvalidObjectID)
		{
			return EJetResult::Failure;
		}
		JetCheckAirfield = GenLogic::GetLogicFPS();
		// "Play the "low fuel" voice whenever the craft is circling above the airfield."
		Logic->PlayObjectSound(*this, { TEXT("VoiceLowFuel") });
		return EJetResult::Continue;

	case S::TakeoffAwaitClearance:
	case S::LandingAwaitClearance:
		if (!bPlane)
		{
			return EJetResult::Success; // helicopters need no runway
		}
		// JetAwaitingRunwayState
		bJetTakeoffInProgress = JetState == S::TakeoffAwaitClearance;
		bJetLandingInProgress = JetState == S::LandingAwaitClearance;
		bJetAllowCircling = true;
		return EJetResult::Continue;

	case S::TaxiFromHangar:
	case S::TaxiToTakeoff:
	case S::TaxiFromLanding:
	{
		// JetOrHeliTaxiState
		const bool bToParking = JetState == S::TaxiFromLanding;
		bJetTakeoffInProgress = !bToParking;
		bJetLandingInProgress = bToParking;
		bJetTaxiInProgress = true;
		if (bToParking)
		{
			ReloadCountermeasures(); // "Instantly reload flares."
		}
		bJetAllowAirLoco = false;
		ChooseLocomotorSet(TEXT("SET_TAXIING"));
		if (!PP)
		{
			return EJetResult::Success; // no airfield: nothing to taxi on
		}
		if (!PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
		{
			return EJetResult::Failure;
		}
		// A space that doesn't face its runway gets a corner in the route.
		FVector3f Intermed((Info.ParkingSpace.Z + Info.RunwayPrep.Z) * 0.5f);
		bool bIntermed = false;
		const float Orient = FMath::Atan2(Info.RunwayPrep.Y - Info.ParkingSpace.Y, Info.RunwayPrep.X - Info.ParkingSpace.X);
		if (FMath::Abs(GenLogic::AngleDiff(Orient, Info.ParkingOrientation)) > PI / 128.f)
		{
			bIntermed = IntersectLines(Info.ParkingSpace, Info.ParkingOrientation, Info.RunwayPrep, Info.ParkingOrientation + PI / 2.f, Intermed);
		}
		JetPath.Reset();
		if (bStatusDeckHeightOffset)
		{
			// JetOrHeliTaxiState on a carrier: the bones the deck lists, and the space the deck says.
			if (JetState == S::TaxiFromLanding)
			{
				if (const TArray<FVector3f>* Taxi = PP->GetTaxiLocations(ID))
				{
					JetPath.Append(*Taxi);
				}
				// "We just landed... see if we can get a better space forward so we don't stop and pause at our initially
				// assigned spot."
				FVector3f Best;
				PP->CalcBestParkingAssignment(ID, Best);
				JetPath.Add(Best);
			}
			else if (JetState == S::TaxiToTakeoff)
			{
				if (Info.RunwayStart != Info.RunwayPrep)
				{
					JetPath.Add(Info.RunwayStart);
				}
			}
			else if (bStatusReassignParking)
			{
				// "This status means we are being reassigned a parking space. We're not actually moving from the hangar."
				bStatusReassignParking = false;
				JetPath.Add(Info.RunwayPrep);
			}
			else
			{
				if (const TArray<FVector3f>* Creation = PP->GetCreationLocations(ID))
				{
					// ("Skip the first node because it's the creation location.")
					for (int32 i = 1; i < Creation->Num(); ++i)
					{
						JetPath.Add((*Creation)[i]);
					}
				}
				JetPath.Add(Info.RunwayPrep);
			}
		}
		else if (JetState == S::TaxiFromLanding)
		{
			JetPath.Add(Info.RunwayPrep);
			if (bIntermed) { JetPath.Add(Intermed); }
			JetPath.Add(Info.ParkingSpace);
		}
		else if (JetState == S::TaxiToTakeoff)
		{
			if (bIntermed) { JetPath.Add(Intermed); }
			JetPath.Add(Info.RunwayPrep);
			JetPath.Add(Info.RunwayStart);
		}
		else
		{
			JetPath.Add(Info.ParkingSpace);
		}
		JetStartPath(true);
		bLocoUltraAccurate = true;
		return EJetResult::Continue;
	}

	case S::PauseBeforeTakeoff:
		// JetPauseBeforeTakeoffState: face down the runway, wait for the neighbour, light the afterburners.
		bJetTakeoffInProgress = true;
		bJetLandingInProgress = false;
		JetWhen = JetWhenTransfer = 0;
		JetWaitedForTaxi = GenInvalidObjectID;
		bJetResetTimer = false;
		if (!PP || !PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
		{
			return EJetResult::Success;
		}
		JetPath.Reset();
		JetPath.Add(Info.RunwayEnd);
		return EJetResult::Continue;

	case S::TakingOff:
	case S::Landing:
	{
		const bool bLanding = JetState == S::Landing;
		bJetLandingSoundPlayed = false;
		bJetTakeoffInProgress = !bLanding;
		bJetLandingInProgress = bLanding;
		bJetAllowAirLoco = true;
		ChooseLocomotorSet(TEXT("SET_NORMAL"));
		LocoMaxLift = BigLift;
		bLocoPreciseZ = true;
		bLocoUltraAccurate = true;
		PreciseGoalZ = Position.Z;
		if (!bPlane)
		{
			// HeliTakeoffOrLandingState: straight up from the pad, or down onto it.
			if (!PP)
			{
				return EJetResult::Success;
			}
			FVector3f ParkingLoc, Approach;
			if (bHelipad)
			{
				ParkingLoc = bLanding ? JetHelipadLanding : Position;
				Approach = ParkingLoc + FVector3f(0.f, 0.f, Airfield->Template->ParkingApproachHeight);
			}
			else
			{
				if (!PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
				{
					return EJetResult::Failure;
				}
				ParkingLoc = Info.ParkingSpace;
				Approach = ParkingLoc + FVector3f(0.f, 0.f, Info.RunwayApproach.Z - Info.RunwayEnd.Z);
			}
			JetPath.Reset();
			JetPath.Add(bLanding ? Approach : ParkingLoc);
			JetPath.Add(bLanding ? ParkingLoc : Approach);
			JetStartPath(false);
			return EJetResult::Continue;
		}
		// JetTakeoffOrLandingState
		JetTakeoffLift = Locomotor->Lift;
		if (!bLanding)
		{
			LocoMaxLift = 0.f; // lift comes with the roll down the runway
		}
		if (!PP)
		{
			return EJetResult::Success;
		}
		if (!PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
		{
			return EJetResult::Failure;
		}
		// (Checked for landing only: on takeoff the runway may already have been handed to the next in line.)
		if (bLanding && !PP->ReserveRunway(ID, true))
		{
			return EJetResult::Failure;
		}
		JetPath.Reset();
		if (bLanding)
		{
			JetPath.Add(Info.RunwayApproach);
			if (bStatusDeckHeightOffset)
			{
				// "Assigned to an aircraft carrier which has separate landing strips."
				JetPath.Add(Info.RunwayLandingStart);
				JetPath.Add(Info.RunwayLandingEnd);
			}
			else
			{
				// Down the same runway we took off from, the other way.
				JetPath.Add(Info.RunwayEnd);
				JetPath.Add(Info.RunwayStart);
			}
		}
		else
		{
			FVector3f End = Info.RunwayEnd;
			End.Z = Info.RunwayApproach.Z;
			JetPath.Add(End);
			JetPath.Add(Info.RunwayExit);
		}
		JetStartPath(false);
		return EJetResult::Continue;
	}

	case S::OrientForParking:
		// JetOrHeliParkOrientState
		if (bHelipad)
		{
			return EJetResult::Success;
		}
		bJetTakeoffInProgress = false;
		bJetLandingInProgress = true;
		return EJetResult::Continue;

	case S::ReloadAmmo:
	{
		// JetOrHeliReloadAmmoState: as long as the emptiest clip takes, in proportion to how empty it is.
		bJetTakeoffInProgress = bJetLandingInProgress = false;
		bJetUseReturnLoco = false;
		JetReloadTime = 0;
		for (const FGenWeaponInstance& WI : Weapons)
		{
			if (!WI.Template || WI.Template->ClipSize <= 0)
			{
				continue;
			}
			const int32 Needed = WI.Template->ClipSize - WI.AmmoInClip;
			JetReloadTime = FMath::Max(JetReloadTime, uint32(FMath::Max(0, WI.Template->ClipReloadTime * Needed / WI.Template->ClipSize)));
		}
		JetReloadTime = FMath::Max(JetReloadTime, 1u);
		JetReloadDoneFrame = Frame + JetReloadTime;
		return EJetResult::Continue;
	}

	default:
		return EJetResult::Success;
	}
}

FGenObject::EJetResult FGenObject::JetUpdateState(uint32 Frame)
{
	using S = EGenJetState;
	FGenParkingPlace* PP = GetProducerParking();
	FGenParkingInfo Info;

	switch (JetState)
	{
	case S::ReturningForLanding:
	case S::ReturnToDeadAirfield:
		return JetFollowPath(Frame, Locomotor->Speed) ? EJetResult::Success : EJetResult::Continue;

	case S::CirclingDeadAirfield:
	{
		// Nowhere to land: what fuel is left burns away.
		const float DamageRate = Template->JetOutOfAmmoDamagePerSecond * GenLogic::SecondsPerFrame() * MaxHealth;
		if (DamageRate > 0.f)
		{
			AttemptDamage(DamageRate, TEXT("UNRESISTABLE"), TEXT("NORMAL"), GenInvalidObjectID);
			if (bDead)
			{
				return EJetResult::Continue;
			}
		}
		if (--JetCheckAirfield <= 0)
		{
			JetCheckAirfield = GenLogic::GetLogicFPS();
			if (const FGenObject* A = FindSuitableAirfield())
			{
				Producer = A->ID;
				return EJetResult::Success;
			}
		}
		return EJetResult::Continue; // (circles: nothing drives it this frame)
	}

	case S::TakeoffAwaitClearance:
	case S::LandingAwaitClearance:
		if (!PP)
		{
			return EJetResult::Success; // no airfield: skip the formalities
		}
		// A runway goes with a parking space.
		if (!PP->ReserveSpace(ID, Template->JetParkingOffset, nullptr))
		{
			return EJetResult::Failure;
		}
		return PP->ReserveRunway(ID, JetState == S::LandingAwaitClearance) ? EJetResult::Success : EJetResult::Continue;

	case S::TaxiFromHangar:
	case S::TaxiFromLanding:
		// "Keep checking to see if there is a better spot as it moves forward. If we find a better spot, then append the
		// position to our move."
		if (PP && bStatusDeckHeightOffset)
		{
			FVector3f Best;
			if (PP->CalcBestParkingAssignment(ID, Best))
			{
				JetPath.Add(Best);
			}
		}
		// fall through
	case S::TaxiToTakeoff:
		return JetFollowPath(Frame, Locomotor->Speed) ? EJetResult::Success : EJetResult::Continue;

	case S::PauseBeforeTakeoff:
	{
		const FVector3f& End = JetPath.Num() > 0 ? JetPath[0] : Position;
		const bool bFacing = JetFaceAngle(FMath::Atan2(End.Y - Position.Y, End.X - Position.X), 0.02f);
		// If the plane on the other runway is still taxiing, wait for it: pairs take off together.
		bool bWaiting = false;
		for (int32 i = 0; PP && i < PP->GetRunwayCount(); ++i)
		{
			const FGenObject* Other = Logic->FindObject(PP->GetRunwayReservation(i));
			if (Other && Other != this && Other->JetState == S::TaxiToTakeoff)
			{
				if (JetWaitedForTaxi == GenInvalidObjectID)
				{
					JetWaitedForTaxi = Other->ID;
				}
				bWaiting = true;
				break;
			}
		}
		if (bWaiting)
		{
			return EJetResult::Continue;
		}
		if (!bJetResetTimer)
		{
			JetWhen = Frame + uint32(Template->JetTakeoffPause);
			if (JetWaitedForTaxi == GenInvalidObjectID)
			{
				JetWaitedForTaxi = ID; // so nobody else is picked up
				JetWhenTransfer = Frame + 1;
			}
			else
			{
				JetWhenTransfer = Frame + 2;
			}
			bJetResetTimer = true;
		}
		if (!bJetAfterburners)
		{
			JetEnableAfterburners(true);
		}
		// Once the final wait begins the runway goes to whoever is behind, so they can start taxiing.
		if (PP && Frame >= JetWhenTransfer)
		{
			PP->TransferRunwayReservationToNextInLineForTakeoff(ID);
		}
		return (Frame >= JetWhen && bFacing) ? EJetResult::Success : EJetResult::Continue;
	}

	case S::TakingOff:
	case S::Landing:
		if (!Template->bJetNeedsRunway)
		{
			return JetFollowPath(Frame, Locomotor->Speed) ? EJetResult::Success : EJetResult::Continue;
		}
		if (JetState == S::Landing)
		{
			LocoMaxLift = BigLift;
			if (!bJetLandingSoundPlayed && Position.Z - 0.25f <= Logic->GetGroundHeight(Position.X, Position.Y) + (PP ? PP->GetLandingDeckHeightOffset() : 0.f))
			{
				// "zPos - zSlop <= groundZ": the wheels touch the runway (MiscAudio's AircraftWheelScreech).
				bJetLandingSoundPlayed = true;
				Logic->PlaySoundAt(FGenAudioData::Get().GetMisc(TEXT("AircraftWheelScreech")), *this);
			}
			// The approach and the roll-out are flown as slowly as the plane can.
			return JetFollowPath(Frame, Locomotor->MinSpeed) ? EJetResult::Success : EJetResult::Continue;
		}
		if (PP)
		{
			PP->TransferRunwayReservationToNextInLineForTakeoff(ID);
		}
		if (PP && PP->CalcInfo(ID, Info) && Info.RunwayTakeoffDist > 0.f)
		{
			// Lift grows with the distance covered down the runway rather than with speed.
			float Ratio = 1.f - (Info.RunwayEnd - Position).Size() / Info.RunwayTakeoffDist;
			Ratio = FMath::Clamp(Ratio * Ratio, 0.f, 1.f);
			LocoMaxLift = JetTakeoffLift * Ratio;
		}
		else
		{
			LocoMaxLift = JetTakeoffLift;
		}
		return JetFollowPath(Frame, Locomotor->Speed) ? EJetResult::Success : EJetResult::Continue;

	case S::OrientForParking:
		if (!PP || !PP->ReserveSpace(ID, Template->JetParkingOffset, &Info))
		{
			return EJetResult::Failure;
		}
		if (FMath::Abs(GenLogic::AngleDiff(Orientation, Info.ParkingOrientation)) <= 0.001f)
		{
			return EJetResult::Success;
		}
		// Exactly on the spot, turning to face the way the space does.
		Velocity.X = Velocity.Y = 0.f;
		Position.X = bStatusDeckHeightOffset ? Info.RunwayPrep.X : Info.ParkingSpace.X;
		Position.Y = bStatusDeckHeightOffset ? Info.RunwayPrep.Y : Info.ParkingSpace.Y;
		JetFaceAngle(Info.ParkingOrientation, 0.f);
		return EJetResult::Continue;

	case S::ReloadAmmo:
	{
		bool bAllDone = true;
		for (FGenWeaponInstance& WI : Weapons)
		{
			if (!WI.Template || WI.Template->ClipSize <= 0)
			{
				continue;
			}
			// Weapon::setClipPercentFull
			const float Fraction = Frame >= JetReloadDoneFrame ? 1.f : float(JetReloadTime - (JetReloadDoneFrame - Frame)) / float(JetReloadTime);
			const int32 Ammo = FMath::FloorToInt32(float(WI.Template->ClipSize) * Fraction);
			if (Ammo > WI.AmmoInClip)
			{
				WI.AmmoInClip = Ammo;
				WI.bReloading = false;
			}
			bAllDone &= WI.AmmoInClip >= WI.Template->ClipSize;
		}
		return bAllDone ? EJetResult::Success : EJetResult::Continue;
	}

	default:
		return EJetResult::Success;
	}
}

void FGenObject::JetExitState()
{
	using S = EGenJetState;
	switch (JetState)
	{
	case S::TakeoffAwaitClearance:
	case S::LandingAwaitClearance:
		bJetTakeoffInProgress = bJetLandingInProgress = false;
		bJetAllowCircling = false;
		break;

	case S::TaxiFromHangar:
	case S::TaxiToTakeoff:
	case S::TaxiFromLanding:
		bJetTakeoffInProgress = bJetLandingInProgress = false;
		bJetTaxiInProgress = false;
		bStatusBraking = false;
		bLocoBraking = false;
		bLocoUltraAccurate = false;
		break;

	case S::PauseBeforeTakeoff:
	case S::OrientForParking:
	case S::ReloadAmmo:
		bJetTakeoffInProgress = bJetLandingInProgress = false;
		break;

	case S::TakingOff:
	case S::Landing:
	{
		const bool bLanding = JetState == S::Landing;
		bJetTakeoffInProgress = bJetLandingInProgress = false;
		JetEnableAfterburners(false);
		bLocoPreciseZ = false;
		bLocoUltraAccurate = false;
		if (!bDead)
		{
			LocoMaxLift = BigLift; // (not for the dead: the crash wants no lift)
		}
		FGenParkingPlace* PP = GetProducerParking();
		if (!Template->bJetNeedsRunway)
		{
			if (bLanding)
			{
				// Down: a helicopter on the ground uses its taxi locomotor, like a parked plane.
				bJetAllowAirLoco = false;
				ChooseLocomotorSet(TEXT("SET_TAXIING"));
			}
			else if (PP && !Template->bJetKeepsParkingSpace)
			{
				PP->ReleaseSpace(ID);
			}
			break;
		}
		if (!bLanding && PP && !Template->bJetKeepsParkingSpace)
		{
			PP->ReleaseSpace(ID);
		}
		if (PP)
		{
			PP->ReleaseRunway(ID);
		}
		break;
	}

	default:
		break;
	}
}

// ---- aircraft: attack passes --------------------------------------------------------------------

void FGenObject::UpdateJetAttack(FGenObject& Target, int32 Slot, uint32 Frame)
{
	// AIAttackAimAtTargetState for something that cannot turn on the spot: the aim is the flight path. The
	// plane flies at its target and fires when it has it ahead and in range; a weapon with a minimum range
	// means flying on past and coming round for another pass.
	const FGenWeaponInstance& WI = Weapons[Slot];
	const FGenWeaponTemplate& W = *WI.Template;
	const float Dist = AttackDistance(Target);
	if (Dist < W.MinimumAttackRange)
	{
		bJetBreakingOff = true;
	}
	else if (bJetBreakingOff && Dist >= W.MinimumAttackRange + 0.5f * FMath::Max(0.f, W.AttackRange - W.MinimumAttackRange))
	{
		bJetBreakingOff = false;
	}

	FVector3f GoalPos = Target.Position;
	if (bJetBreakingOff)
	{
		const FVector2f Dir = GetDirection2D();
		GoalPos = FVector3f(Position.X + Dir.X * 1000.f, Position.Y + Dir.Y * 1000.f, Position.Z);
	}
	bJetDriving = true;
	bMoving = false;
	Path.Reset();
	LocoMoveTowardsPosition(GoalPos, 99999.f, Locomotor->Speed, Frame);

	if (bJetBreakingOff || Dist > W.AttackRange || Frame < WI.ReadyFrame || IsWeaponOutOfAmmo(Slot))
	{
		return;
	}
	const float Desired = FMath::Atan2(Target.Position.Y - Position.Y, Target.Position.X - Position.X);
	if (FMath::Abs(GenLogic::AngleDiff(Desired, Orientation)) < FMath::Max(W.AcceptableAimDelta, 0.035f))
	{
		FireWeapon(Slot, Target, Frame);
	}
}

// ---- aircraft: shot down (JetSlowDeathBehavior) -------------------------------------------------

bool FGenObject::BeginJetDeath(const FString& DeathType, uint32 Frame)
{
	const FGenJetDeath& J = Template->JetDeath;
	if (!J.bValid)
	{
		return false;
	}
	const float Height = Position.Z - Logic->GetGroundHeight(Position.X, Position.Y);
	if (Height <= JetAirborneThreshold)
	{
		// Caught on the ground: it just blows up.
		Logic->DoFXObj(J.FXOnGroundDeath, *this);
		Logic->DoOCL(J.OCLOnGroundDeath, *this);
		DestroyFrame = Frame + 1;
		return true;
	}
	Logic->DoFXObj(J.FXInitialDeath, *this);
	Logic->DoOCL(J.OCLInitialDeath, *this);
	bJetDying = true;
	bFreePhysics = false;
	YawRate = 0.f;
	JetDeathFrame = Frame;
	JetGroundFrame = 0;
	JetRollRate = J.RollRate;
	DestroyFrame = MAX_uint32;
	SetModelCondition(JetAfterburnerBit(), false);
	return true;
}

void FGenObject::UpdateJetDying(uint32 Frame)
{
	const FGenJetDeath& J = Template->JetDeath;
	const float RateScale = 30.f / float(GenLogic::GetLogicFPS());
	const float GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
	if (JetGroundFrame == 0)
	{
		// No lift and no steering any more: it flies on, rolling, and comes down faster than a stone.
		Velocity.Z += FGenGameData::Get().Gravity * FMath::Max(J.FallHowFast, 0.f);
		Position += Velocity;
		Rotation = (Rotation * FQuat4f(FVector3f::ForwardVector, JetRollRate * Template->PitchRollYawFactor * RateScale)).GetNormalized();
		bFreeRotation = true;
		Orientation = GetYawFromRotation();
		JetRollRate *= J.RollRateDelta;

		if (Position.Z <= Logic->GetGroundHeight(Position.X, Position.Y))
		{
			Position.Z = Logic->GetGroundHeight(Position.X, Position.Y);
			Velocity.Z = 0.f;
			Logic->DoFXObj(J.FXHitGround, *this);
			Logic->DoOCL(J.OCLHitGround, *this);
			JetGroundFrame = Frame;
			PitchRate = J.PitchRate;
		}
		else if (JetDeathFrame != 0 && Frame - JetDeathFrame >= uint32(J.DelaySecondaryFromInitialDeath))
		{
			Logic->DoFXObj(J.FXSecondary, *this);
			Logic->DoOCL(J.OCLSecondary, *this);
			JetDeathFrame = 0;
		}
		return;
	}

	// On the ground: the wreck skids a little, then goes up.
	Velocity *= 0.85f;
	Position.X += Velocity.X;
	Position.Y += Velocity.Y;
	Position.Z = GroundZ;
	if (PitchRate != 0.f)
	{
		Rotation = (Rotation * FQuat4f(FVector3f::RightVector, PitchRate * Template->PitchRollYawFactor * RateScale)).GetNormalized();
	}
	if (Frame - JetGroundFrame >= uint32(J.DelayFinalBlowUpFromHitGround))
	{
		Logic->DoFXObj(J.FXFinalBlowUp, *this);
		Logic->DoOCL(J.OCLFinalBlowUp, *this);
		bPendingDestroy = true;
	}
}

// ---- helicopters: shot down (HelicopterSlowDeathBehavior) ---------------------------------------

namespace
{
	/** The audio manager's slot for the sound of a helicopter going down (0 move, 1 ambient, 2-4 weapons). */
	constexpr int32 HeliDeathSoundSlot = 5;
}

bool FGenObject::BeginHeliDeath(uint32 Frame)
{
	const FGenHeliDeath& H = Template->HeliDeath;
	bHeliDying = true;
	bFreePhysics = false;
	YawRate = 0.f;
	DestroyFrame = MAX_uint32;
	JetGroundFrame = 0;
	// The big downward circle starts off the way the helicopter was pointing; the hull spins on its own axis as well.
	HeliForwardAngle = Orientation;
	HeliForwardSpeed = H.SpiralOrbitForwardSpeed;
	HeliSelfSpin = H.MinSelfSpin;
	bHeliSpinTowardsMax = true;
	HeliLastSpinFrame = Frame;
	HeliBladeFrames = Logic->GetRandom().RandRange(H.MinBladeFlyOffDelay, FMath::Max(H.MinBladeFlyOffDelay, H.MaxBladeFlyOffDelay));
	// "The fall of the helicopter": the locomotor is left with only so much lift against gravity.
	LocoMaxLift = -FGenGameData::Get().Gravity * (1.f - H.FallHowFast);
	bLocoPreciseZ = false;
	bLocoUltraAccurate = false;
	if (!H.AttachParticle.IsEmpty())
	{
		Logic->AttachParticleSystem(H.AttachParticle, ID);
	}
	if (UGenAudioManager* Audio = Logic->GetAudio())
	{
		Audio->SetObjectLoop(ID, HeliDeathSoundSlot, H.SoundDeathLoop);
	}
	return true;
}

void FGenObject::UpdateHeliDying(uint32 Frame)
{
	const FGenHeliDeath& H = Template->HeliDeath;
	if (JetGroundFrame == 0)
	{
		SetOrientation(Orientation + HeliSelfSpin);
		// The spin speeds up to MaxSelfSpin, slows to MinSelfSpin, and so on.
		if (H.SelfSpinUpdateDelay > 0 && Frame - HeliLastSpinFrame > uint32(H.SelfSpinUpdateDelay))
		{
			HeliLastSpinFrame = Frame;
			const float Step = H.SelfSpinUpdateAmount / float(GenLogic::GetLogicFPS());
			HeliSelfSpin += bHeliSpinTowardsMax ? Step : -Step;
			if (HeliSelfSpin > H.MaxSelfSpin) { HeliSelfSpin = H.MaxSelfSpin; bHeliSpinTowardsMax = false; }
			else if (HeliSelfSpin < H.MinSelfSpin) { HeliSelfSpin = H.MinSelfSpin; bHeliSpinTowardsMax = true; }
		}

		// Pushed round the spiral, held back by the air, sinking with what lift is left.
		Velocity.X += FMath::Cos(HeliForwardAngle) * HeliForwardSpeed / Mass;
		Velocity.Y += FMath::Sin(HeliForwardAngle) * HeliForwardSpeed / Mass;
		HeliForwardAngle += H.SpiralOrbitTurnRate;
		HeliForwardSpeed *= H.SpiralOrbitForwardSpeedDamping;
		const float Drag = 1.f - FMath::Clamp(LateralFriction, 0.01f, 0.99f);
		Velocity.X *= Drag;
		Velocity.Y *= Drag;
		// Gravity, against what lift the locomotor still gives: all it has left while the helicopter is
		// sinking (so it comes down at FallHowFast of gravity), none while it is still on its way up.
		const float Gravity = FGenGameData::Get().Gravity;
		Velocity.Z += Gravity + ((Locomotor && Locomotor->UsesLift()) ? CalcLiftToUse() : -Gravity * (1.f - FMath::Max(H.FallHowFast, 0.f)));
		Position += Velocity;
		UE_CLOG(Logic->IsTracing() && Frame % 30 == 0, LogGenerals, Log, TEXT("  [jet f%u] #%u %s going down: %.0f above the ground, %.2f a frame"), Frame, ID, *Template->Name,
			Position.Z - Logic->GetGroundHeight(Position.X, Position.Y), Velocity.Z);

		if (HeliBladeFrames > 0 && --HeliBladeFrames == 0)
		{
			// The rotor comes off.
			FVector3f BladePos = Position + FVector3f(0.f, 0.f, Template->GeometryHeight);
			TArray<FTransform3f> Bones;
			const FGenDrawModule* Draw = Template->PrimaryDraw();
			UGenAssetSubsystem* Assets = UGenAssetSubsystem::Get();
			if (Assets && Draw && !H.BladeBone.IsEmpty() && Assets->GetPristineBones(Draw->DefaultModel(), H.BladeBone, 0, 1, Bones) > 0)
			{
				BladePos = GetTransform().TransformPositionNoScale(Bones[0].GetTranslation());
			}
			Logic->DoFXPos(H.FXBlade, BladePos, nullptr, 0.f, nullptr, 0.f);
			Logic->DoOCL(H.OCLBlade, *this);
			// "if we have (potentially) a pilot ejection, do it here. ... constrains to veteran-or-better status."
			if (!H.OCLEjectPilot.IsEmpty() && VeterancyLevel > 0)
			{
				EjectPilotBy(H.OCLEjectPilot);
			}
			static const int32 SpecialDamaged = GenConditions::Index(TEXT("SPECIAL_DAMAGED"));
			SetModelCondition(SpecialDamaged, true); // (the model without its rotor)
		}

		const float GroundZ = Logic->GetGroundHeight(Position.X, Position.Y);
		if (Position.Z <= GroundZ + 1.f)
		{
			Position.Z = GroundZ;
			Velocity = FVector3f::ZeroVector;
			JetGroundFrame = Frame;
			Logic->DoFXObj(H.FXHitGround, *this);
			Logic->DoOCL(H.OCLHitGround, *this);
			if (UGenAudioManager* Audio = Logic->GetAudio())
			{
				Audio->SetObjectLoop(ID, HeliDeathSoundSlot, FString());
			}
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [jet f%u] #%u %s hits the ground"), Frame, ID, *Template->Name);
			static const int32 SpecialDamaged = GenConditions::Index(TEXT("SPECIAL_DAMAGED"));
			SetModelCondition(SpecialDamaged, true);
		}
		return;
	}
	if (Frame - JetGroundFrame > uint32(H.DelayFromGroundToFinalDeath))
	{
		Logic->DoFXObj(H.FXFinalBlowUp, *this);
		Logic->DoOCL(H.OCLFinalBlowUp, *this);
		// What is left of it stays where it came down.
		if (const FGenThingTemplate* Rubble = FGenGameData::Get().FindTemplate(H.FinalRubbleObject))
		{
			Logic->CreateObject(Rubble, Owner, Position, Orientation);
		}
		bPendingDestroy = true;
	}
}