// Trains: RailroadBehavior (RailroadGuideAIUpdate.cpp). A locomotive takes the chain of linked waypoints it stands on
// as its track (TrainTrack), hitches the cars standing behind it (or makes them from its CarriageTemplateName list),
// and runs the line: accelerating, braking for a ...Station / ...Disembark / ...PingPong waypoint, waiting there, and
// on again. Each car is placed behind the one pulling it; what the locomotive runs into is pushed aside, hurt, or -
// at speed - killed and thrown. A car that loses its locomotive coasts to a stop.

#include "Logic/GenObject.h"
#include "Logic/GenGameData.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenPartition.h"
#include "Logic/GenPathfinder.h"
#include "Map/GenMapData.h"
#include "Generals1.h"

namespace
{
	constexpr int32 FramesUnpulledToUnhitch = 2; // FRAMES_UNPULLED_LONG_ENOUGH_TO_UNHITCH

	/** The data's speeds and factors are per 30 Hz frame. */
	float RateScale() { return 30.f / float(GenLogic::GetLogicFPS()); }
}

bool FGenObject::IsDangerousRailroad() const
{
	if (!Train || !Train->Track || Train->bWaitingInWings || Train->bEndOfLine)
	{
		return false;
	}
	return Train->bLead || Train->Trailer == GenInvalidObjectID;
}

void FGenObject::TrainLoadTrack()
{
	// RailroadBehavior::loadTrackData: the waypoint nearest the locomotive anchors the track; from it the first link of
	// each waypoint leads on, until one leads nowhere - or back to the anchor (a loop).
	const FGenMapData* Map = Logic->GetMap();
	if (!Map || Train->Track)
	{
		return;
	}
	const FGenMapObject* Anchor = nullptr;
	float BestDistSq = FLT_MAX;
	for (const FGenMapObject& W : Map->Objects)
	{
		if (!W.IsWaypoint())
		{
			continue;
		}
		const float DistSq = FVector2f::DistSquared(FVector2f(W.Location.X, W.Location.Y), FVector2f(Position.X, Position.Y));
		if (DistSq < BestDistSq)
		{
			BestDistSq = DistSq;
			Anchor = &W;
		}
	}
	if (!Anchor)
	{
		return;
	}
	auto NextOf = [Map](int32 Id) -> const FGenMapObject*
	{
		for (const FIntPoint& L : Map->WaypointLinks)
		{
			if (L.X == Id)
			{
				return Map->FindWaypointByID(L.Y);
			}
		}
		return nullptr;
	};
	auto Ground = [this](const FGenMapObject& W) { return FVector3f(W.Location.X, W.Location.Y, Logic->GetGroundHeight(W.Location.X, W.Location.Y)); };
	TSharedPtr<FGenTrainTrack> Track = MakeShared<FGenTrainTrack>();
	const int32 AnchorId = Anchor->GetInt(TEXT("waypointID"), -1);
	{
		FGenTrainTrack::FPoint P;
		const FString Name = Anchor->GetString(TEXT("waypointName"));
		P.Pos = Ground(*Anchor);
		P.Handle = AnchorId;
		P.bTunnel = Name.EndsWith(TEXT("Tunnel"), ESearchCase::IgnoreCase);
		P.bStation = Name.EndsWith(TEXT("Station"), ESearchCase::IgnoreCase);
		P.bDisembark = Name.EndsWith(TEXT("Disembark"), ESearchCase::IgnoreCase);
		Track->Points.Add(P);
	}
	const FGenMapObject* Scanner = Anchor;
	TSet<int32> Seen;
	Seen.Add(AnchorId);
	while (Scanner)
	{
		const int32 ScannerId = Scanner->GetInt(TEXT("waypointID"), -1);
		const FGenMapObject* Next = NextOf(ScannerId);
		if (!Next)
		{
			break;
		}
		const FString ScannerName = Scanner->GetString(TEXT("waypointName")), NextName = Next->GetString(TEXT("waypointName"));
		FGenTrainTrack::FPoint P;
		P.Pos = Ground(*Next);
		const float Dist = FVector3f::Distance(Ground(*Scanner), P.Pos);
		Track->Length += Dist;
		P.DistFromFirst = Track->Length;
		P.Handle = ScannerId;
		P.bTunnel = NextName.EndsWith(TEXT("Tunnel"), ESearchCase::IgnoreCase);
		P.bStation = NextName.EndsWith(TEXT("Station"), ESearchCase::IgnoreCase);
		P.bPingPong = ScannerName.EndsWith(TEXT("PingPong"), ESearchCase::IgnoreCase);
		P.bDisembark = ScannerName.EndsWith(TEXT("Disembark"), ESearchCase::IgnoreCase);
		Track->Points.Add(P);
		const int32 NextId = Next->GetInt(TEXT("waypointID"), -1);
		if (NextId == AnchorId)
		{
			Track->bLooping = true; // "it must be a looping track. Cool."
			break;
		}
		if (Seen.Contains(NextId))
		{
			break; // (a loop that doesn't come back to the anchor: as far as it goes)
		}
		Seen.Add(NextId);
		Scanner = Next;
	}
	if (Track->Points.Num() < 2 || Track->Length <= 0.f)
	{
		return;
	}
	Train->Track = Track;
	int32 Stations = 0;
	for (const FGenTrainTrack::FPoint& P : Track->Points) { Stations += P.bStation || P.bDisembark ? 1 : 0; }
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [train f%u] #%u %s: a %s track of %d points, %.0f long, %d stops, from waypoint '%s'"), Logic->GetFrame(), ID, *Template->Name,
		Track->bLooping ? TEXT("looping") : TEXT("open"), Track->Points.Num(), Track->Length, Stations, *Anchor->GetString(TEXT("waypointName")));
}

FGenObject* FGenObject::TrainFindCarBehind() const
{
	// PartitionFilterIsValidCarriage + getClosestObject: an allied car nobody has hitched yet, just behind this one.
	const float MaxRadius = Template->GeometryMajorRadius * 2.f;
	const FVector2f Hitch = FVector2f(Position.X, Position.Y) - GetDirection2D() * MaxRadius;
	FGenObject* Best = nullptr;
	float BestDistSq = FMath::Square(MaxRadius);
	Logic->GetPartition().ForEachInRange(Hitch, MaxRadius, [&](FGenObject& O)
	{
		if (&O == this || !O.Train || O.Train->bEverHitched || O.Train->bLocomotive || O.bDead
			|| Logic->GetRelationship(Owner, O.Owner) != EGenRelationship::Allies)
		{
			return;
		}
		const float DistSq = FVector2f::DistSquared(Hitch, FVector2f(O.Position.X, O.Position.Y));
		if (DistSq <= BestDistSq && (!Best || DistSq < BestDistSq || O.ID < Best->ID))
		{
			Best = &O;
			BestDistSq = DistSq;
		}
	});
	return Best;
}

void FGenObject::TrainCreateCarriages()
{
	// RailroadBehavior::createCarriages: the cars standing behind it, one after another; failing that, new ones of the
	// templates its data lists.
	const TArray<FString>& List = Template->Railroad.CarriageTemplates;
	FGenObject* First = TrainFindCarBehind();
	const bool bByProximity = First != nullptr;
	if (!First && List.Num() > 0)
	{
		if (const FGenThingTemplate* T = FGenGameData::Get().FindTemplate(List[0]))
		{
			First = Logic->CreateObject(T, Owner, Position, Orientation);
		}
	}
	if (!First || !First->Train)
	{
		return;
	}
	First->Producer = ID;
	Train->Trailer = First->ID;
	First->TrainHitch(ID, 1, bByProximity);
	int32 Cars = 0;
	for (const FGenObject* C = First; C && C->Train && Cars < 100; C = Logic->FindObject(C->Train->Trailer)) { ++Cars; }
	UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [train f%u] #%u %s pulls %d cars (%s)"), Logic->GetFrame(), ID, *Template->Name, Cars,
		bByProximity ? TEXT("standing behind it") : TEXT("made from its list"));
}

void FGenObject::TrainHitch(FGenObjectID Loco, int32 TemplateIndex, bool bByProximity)
{
	// hitchNewCarriagebyTemplate / hitchNewCarriagebyProximity: it shares the locomotive's track, and hitches the next.
	FGenObject* Locomotive = Logic->FindObject(Loco);
	if (Train->bLocomotive || !Locomotive || !Locomotive->Train)
	{
		return;
	}
	Train->Track = Locomotive->Train->Track;
	Train->bEverHitched = true;
	FGenObject* Next = nullptr;
	if (bByProximity)
	{
		Next = TrainFindCarBehind();
	}
	else
	{
		const TArray<FString>& List = Locomotive->Template->Railroad.CarriageTemplates;
		const FGenThingTemplate* T = List.IsValidIndex(TemplateIndex) ? FGenGameData::Get().FindTemplate(List[TemplateIndex]) : nullptr;
		Next = T ? Logic->CreateObject(T, Locomotive->Owner, Position, Orientation) : nullptr;
	}
	if (Next && Next->Train)
	{
		Next->Producer = bByProximity ? ID : Loco;
		Train->Trailer = Next->ID;
		Next->TrainHitch(Loco, TemplateIndex + 1, bByProximity);
	}
}

FVector3f FGenObject::TrainFindPos(float Dist, bool bSetState)
{
	// RailroadBehavior::FindPosByPathDistance: the spot that far along the track. An open track's ends set the car
	// "waiting in the wings" (not on it yet) or at its "end of the line"; crossing into a new stretch, a locomotive
	// sees the stop its waypoint asks for, and every car clacks over the join.
	FGenTrain& T = *Train;
	const FGenTrainTrack& Track = *T.Track;
	const float Length = Track.Length;
	T.bWaitingInWings = false;
	float Actual = Dist;
	if (Track.bLooping)
	{
		Actual = FMath::Fmod(Actual, Length);
		if (Actual < 0.f) { Actual += Length; }
	}
	else
	{
		if (Dist < 0.f) { T.bWaitingInWings = true; }
		else if (Dist >= Length) { T.bEndOfLine = true; }
		Actual = FMath::Clamp(Dist, 0.f, Length);
	}
	FVector3f Pos = Track.Points[0].Pos;
	for (int32 i = 0; i < Track.Points.Num(); ++i)
	{
		const FGenTrainTrack::FPoint& This = Track.Points[i];
		if (This.DistFromFirst >= Actual)
		{
			continue;
		}
		const FGenTrainTrack::FPoint* Next = Track.Points.IsValidIndex(i + 1) ? &Track.Points[i + 1] : nullptr;
		if (!Next || Next->DistFromFirst <= Actual)
		{
			Pos = This.Pos;
			continue;
		}
		if (bSetState)
		{
			const bool bEdge = T.CurrentPointHandle != This.Handle;
			T.bInTunnel = This.bTunnel;
			if (bEdge)
			{
				T.CurrentPointHandle = This.Handle; // (every car's, not only the locomotive's: each clacks once over a join)
				if (T.bLocomotive)
				{
					if (This.bStation || This.bDisembark)
					{
						T.State = FGenTrain::EConductor::ApplyBrakes;
						T.bDisembark = This.bDisembark;
						Logic->SetObjectSound(*this, 0, FString());
					}
					else if (This.bPingPong && T.Conductor.MostRecentSpecial != This.Handle)
					{
						T.Conductor.MostRecentSpecial = This.Handle;
						T.State = FGenTrain::EConductor::ApplyBrakes;
						T.bDisembark = false;
						Logic->SetObjectSound(*this, 0, FString());
						T.Conductor.Direction = -T.Conductor.Direction;
					}
				}
				if (!T.bInTunnel && !Template->Railroad.ClicketyClackSound.IsEmpty() && T.bPlaced)
				{
					Logic->PlaySoundAt(Template->Railroad.ClicketyClackSound, *this);
				}
			}
		}
		const float Along = Actual - This.DistFromFirst;
		const FVector3f Delta = (Next->Pos - This.Pos).GetSafeNormal() * Along;
		return This.Pos + Delta;
	}
	return Pos;
}

void FGenObject::TrainPlace(const FGenTrain::FPull& Puller, FGenTrain::FPull& Mine)
{
	// RailroadBehavior::updatePositionTrackDistance: its back two radii behind what pulls it, on the track; it faces the
	// puller's tow hitch, its middle a radius ahead of its back.
	FGenTrain& T = *Train;
	const float R = Template->GeometryMajorRadius;
	Mine.TrackDistance = Puller.TrackDistance - 2.f * R;
	Mine.Speed = Puller.Speed;
	Mine.Direction = Puller.Direction;
	Mine.TowHitch = TrainFindPos(Mine.TrackDistance, false);
	const FVector3f Rear = TrainFindPos(Mine.TrackDistance, true);
	FVector2f Dir(Puller.TowHitch.X - Rear.X, Puller.TowHitch.Y - Rear.Y);
	Dir = Dir.SizeSquared() > 0.01f ? Dir.GetSafeNormal() : GetDirection2D();
	FVector3f Center(Rear.X + Dir.X * R, Rear.Y + Dir.Y * R, Position.Z);
	if (!T.bInTunnel || !T.bPlaced)
	{
		Center.Z = Logic->GetGroundHeight(Center.X, Center.Y);
	}
	SetOrientation(FMath::Atan2(Dir.Y, Dir.X));
	Position = Center;
	if (!T.bPlaced)
	{
		T.bPlaced = true;
		SnapshotPrevious(); // (its first place on the track: no sliding there from where it was made)
	}
}

void FGenObject::TrainGetPulled(FGenTrain::FPull& Info)
{
	// RailroadBehavior::getPulled: placed behind its puller, and it pulls the car behind it in turn. ("copy my puller
	// to my conductor, so that if I ever get detached" it coasts on as it was going.)
	FGenTrain& T = *Train;
	T.WantsToBeLead = 0;
	if (!T.Track)
	{
		return;
	}
	T.Conductor = Info;
	TrainPlace(Info, T.Mine);
	FGenObject* Trailer = Logic->FindObject(T.Trailer);
	if (Trailer && Trailer->Train && !Trailer->bDead)
	{
		Trailer->TrainGetPulled(T.Mine);
	}
	else
	{
		T.Trailer = GenInvalidObjectID; // the last car now
		if (T.bEndOfLine)
		{
			bPendingDestroy = true; // run off the end of an open line
		}
	}
}

void FGenObject::TrainSetWall(bool bOn)
{
	// makeAWallOutOfThisTrain: every car of it, down the train.
	for (FGenObject* C = this; C && C->Train; C = Logic->FindObject(C->Train->Trailer))
	{
		if (C->Train->bWall != bOn)
		{
			C->Train->bWall = bOn;
			Logic->SetTemporaryObstacle(*C, bOn);
		}
	}
}

void FGenObject::TrainDisembark()
{
	// RailroadBehavior::disembark: "orderAllPassengersToExit", every car of it.
	for (FGenObject* C = this; C && C->Train; C = Logic->FindObject(C->Train->Trailer))
	{
		if (C->Contain)
		{
			Logic->EvacuateContainer(*C);
		}
	}
}

void FGenObject::UpdateTrain(uint32 Frame)
{
	// RailroadBehavior::update
	FGenTrain& T = *Train;
	const FGenThingTemplate::FRailroad& R = Template->Railroad;
	const float RS = RateScale();
	if (!T.bTrackLoaded && T.bLocomotive)
	{
		TrainLoadTrack();
		if (T.Track)
		{
			TrainCreateCarriages();
			Logic->SetObjectSound(*this, 0, R.RunningSound);
		}
		T.bTrackLoaded = true;
	}
	if (!T.Track)
	{
		return;
	}
	if (T.bLocomotive)
	{
		FGenTrain::FPull& C = T.Conductor;
		switch (T.State)
		{
		case FGenTrain::EConductor::ApplyBrakes:
			C.Speed *= FMath::Pow(R.Braking, RS);
			if (FMath::Abs(C.Speed) < 0.1f)
			{
				C.Speed = 0.f;
				T.WaitTimer = R.WaitAtStationFrames;
				T.State = FGenTrain::EConductor::WaitAtStation;
				TrainSetWall(true);
				if (T.bDisembark)
				{
					TrainDisembark();
					T.bDisembark = false;
				}
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [train f%u] #%u %s stops at %.0f along its track (%.0f, %.0f)"), Frame, ID, *Template->Name,
					C.TrackDistance, Position.X, Position.Y);
			}
			break;
		case FGenTrain::EConductor::WaitAtStation:
			if (--T.WaitTimer <= 0)
			{
				T.State = FGenTrain::EConductor::Accelerate;
				C.Speed = 0.05f * C.Direction;
				Logic->SetObjectSound(*this, 0, R.RunningSound);
				TrainSetWall(false);
				UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [train f%u] #%u %s leaves the station"), Frame, ID, *Template->Name);
			}
			else if (T.WaitTimer == R.WaitAtStationFrames / 4 && !R.WhistleSound.IsEmpty())
			{
				Logic->PlaySoundAt(R.WhistleSound, *this);
			}
			break;
		case FGenTrain::EConductor::Accelerate:
			C.Speed += 0.02f * C.Direction * RS; // "push start multiplier"
			C.Speed *= FMath::Pow(R.Acceleration, RS);
			C.Speed = FMath::Clamp(C.Speed, -R.SpeedMax, R.SpeedMax);
			break;
		default:
			break;
		}
	}
	if (T.WantsToBeLead > FramesUnpulledToUnhitch && !T.bLead)
	{
		T.bLead = true; // its puller is gone: it runs on by itself
		T.State = FGenTrain::EConductor::Coast;
	}
	if (T.bLead)
	{
		FGenTrain::FPull& C = T.Conductor;
		if (T.State == FGenTrain::EConductor::Coast)
		{
			C.Speed *= FMath::Pow(R.Friction, RS);
			Logic->SetObjectSound(*this, 0, FString());
		}
		C.TrackDistance += C.Speed * RS;
		if (T.Track->bLooping)
		{
			C.TrackDistance = FMath::Fmod(C.TrackDistance, T.Track->Length);
			if (C.TrackDistance < 0.f) { C.TrackDistance += T.Track->Length; }
		}
		C.TowHitch = TrainFindPos(C.TrackDistance, false);
		TrainPlace(C, T.Mine);
		FGenObject* Trailer = Logic->FindObject(T.Trailer);
		if (Trailer && Trailer->Train && !Trailer->bDead)
		{
			Trailer->TrainGetPulled(T.Mine);
		}
		else
		{
			T.Trailer = GenInvalidObjectID;
			if (T.bEndOfLine)
			{
				bPendingDestroy = true;
			}
		}
	}
	else if (T.WantsToBeLead <= FramesUnpulledToUnhitch)
	{
		++T.WantsToBeLead; // ("like every young carriage, I aspire to be the lead carriage some day")
	}
	if (!T.Track->bLooping)
	{
		bHidden = T.bWaitingInWings || T.bEndOfLine; // (off the ends of an open line it isn't there to see)
	}
	TrainCollide(Frame);
}

void FGenObject::TrainCollide(uint32 Frame)
{
	// RailroadBehavior::onCollide, for whatever its box overlaps this frame. ("if (m_waitingInWings) return".)
	FGenTrain& T = *Train;
	if (T.bWaitingInWings || T.bEndOfLine || bPendingDestroy)
	{
		return;
	}
	const float Major = Template->GeometryMajorRadius, Minor = Template->IsBox() ? Template->GeometryMinorRadius : Major;
	const FVector2f Me(Position.X, Position.Y), Dir = GetDirection2D(), Side(-Dir.Y, Dir.X);
	TArray<FGenObject*, TInlineAllocator<16>> Hit;
	Logic->GetPartition().ForEachInRange(Me, Major + 30.f, [&](FGenObject& O)
	{
		if (&O == this || O.bPendingDestroy || O.IsContained() || O.IsProjectile() || O.IsAirborneTarget() || O.bHidden
			|| (O.Train && O.Train->Track == T.Track))
		{
			return; // (nor the cars of its own train)
		}
		const float R = O.Template->GetBoundingCircleRadius();
		const FVector2f D(O.Position.X - Me.X, O.Position.Y - Me.Y);
		if (FMath::Abs(FVector2f::DotProduct(D, Dir)) <= Major + R && FMath::Abs(FVector2f::DotProduct(D, Side)) <= Minor + R)
		{
			Hit.Add(&O);
		}
	});
	Hit.Sort([](const FGenObject& A, const FGenObject& B) { return A.ID < B.ID; });
	for (FGenObject* O : Hit)
	{
		TrainOnCollide(*O);
		if (bDead || bPendingDestroy)
		{
			return;
		}
	}
}

void FGenObject::TrainOnCollide(FGenObject& Other)
{
	FGenTrain& T = *Train;
	const FGenThingTemplate::FRailroad& R = Template->Railroad;
	// A train running into another one: the locomotive smashes it; a runaway car dies with what it hits.
	if (Other.Train)
	{
		if (Other.IsDangerousRailroad() && !Other.bDead)
		{
			if (T.bLocomotive)
			{
				Other.Kill();
			}
			else if (T.bLead)
			{
				Other.Kill();
				Kill();
			}
		}
		return;
	}
	if (Other.bDead && !Other.Template->bHasPhysics)
	{
		return;
	}
	// "we ignore buildings, except for hostile, nasty ones that can blow us up"
	if (Other.IsKindOf(TEXT("STRUCTURE")))
	{
		if (Other.bDead)
		{
			return;
		}
		if (Other.IsKindOf(TEXT("FS_POWER")) || Other.IsKindOf(TEXT("FS_FACTORY")) || Other.IsKindOf(TEXT("FS_BASE_DEFENSE"))
			|| Other.IsKindOf(TEXT("FS_TECHNOLOGY")) || Other.IsKindOf(TEXT("REBUILD_HOLE")))
		{
			TrainImpactSound(Other);
			Other.Kill();
		}
		else if (Other.Template->DemoTrap.bValid)
		{
			if (!Other.IsUnderConstruction())
			{
				Kill(); // "it can only detonate on me if it is ready"
			}
			TrainImpactSound(Other);
			Other.Kill();
		}
		return;
	}
	if (!Other.CanMove() && !Other.Template->bHasPhysics)
	{
		return; // ("PhysicsBehavior *theirPhys = other->getPhysics(); if( ! theirPhys ) return;")
	}
	const float Speed = FMath::Abs(T.Mine.Speed);
	// Climbing aboard at a station.
	if (T.State == FGenTrain::EConductor::WaitAtStation && Speed < R.RunningGarrisonSpeedMax && Other.EnterTarget == ID)
	{
		return;
	}
	const bool bInfantry = Other.IsKindOf(TEXT("INFANTRY"));
	const FVector3f Delta3 = Other.Position - Position;
	const float Dist = Delta3.Size();
	if (!bInfantry && Dist > 0.001f)
	{
		// A vehicle in the way is shoved out of it, a quarter of the overlap a frame.
		const float Overlap = (Template->GeometryMajorRadius + Other.Template->GeometryMajorRadius - Dist + 1.f) / 4.f;
		if (Overlap > 0.f)
		{
			const FVector3f Shove = Delta3 / Dist * Overlap;
			Other.Position.X += Shove.X;
			Other.Position.Y += Shove.Y;
		}
	}
	if (T.State == FGenTrain::EConductor::WaitAtStation || (T.State == FGenTrain::EConductor::Coast && Speed < R.RunningGarrisonSpeedMax) || !T.bLocomotive)
	{
		return; // "let those trying to board pass through unhindered"
	}
	const uint32 Now = Logic->GetFrame();
	if (!R.WhistleSound.IsEmpty() && Now >= T.WhistleFrame)
	{
		Logic->PlaySoundAt(R.WhistleSound, *this);
		T.WhistleFrame = Now + uint32(GenLogic::GetLogicFPS()) * 3;
	}
	FRandomStream& Rand = Logic->GetRandom();
	const FVector2f Dir = GetDirection2D();
	FVector3f Away = Delta3.GetSafeNormal();
	const float Dot = Away.X * Dir.X + Away.Y * Dir.Y;
	if (Other.bDead)
	{
		Away *= FMath::Min(0.3f, Speed * 0.66f); // "we just run over debris, instead of shoving it around"
	}
	else
	{
		Away *= FMath::Min(1.4f, Speed * 0.66f); // "the faster I go, the harder I slam!"
		if (Speed >= R.KillSpeedMin)
		{
			Other.Kill(TEXT("NORMAL"), ID);
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [train f%u] #%u %s runs over #%u %s at %.1f"), Now, ID, *Template->Name, Other.ID, *Other.Template->Name, Speed);
		}
		else
		{
			TrainImpactSound(Other);
			Other.AttemptDamage(Speed * 10.f, TEXT("CRUSH"), TEXT("CRUSHED"), ID); // "hurt!"
		}
	}
	// What it hit flies off a little ("for some fake heft"), tumbling - the dead with free physics.
	if (Other.bDead && Other.Template->bHasPhysics)
	{
		Away.Z = Rand.FRandRange(0.05f, FMath::Max(0.05f, Speed / 10.f));
		Away *= Dot;
		Other.Velocity += Away * (1.f / RateScale());
		Other.PitchRate = Rand.FRandRange(-0.03f, 0.03f);
		Other.RollRate = Rand.FRandRange(-0.03f, 0.03f);
		Other.bFreePhysics = true;
		Other.bFreeRotation = true;
	}
}

void FGenObject::TrainImpactSound(const FGenObject& Victim)
{
	// RailroadBehavior::playImpactSound: meaty for soldiers, big metal for huge vehicles and buildings, small metal for the rest.
	const FGenThingTemplate::FRailroad& R = Template->Railroad;
	const FString& Sound = Victim.IsKindOf(TEXT("INFANTRY")) ? R.MeatyBounceSound
		: (Victim.IsKindOf(TEXT("HUGE_VEHICLE")) || Victim.IsKindOf(TEXT("STRUCTURE"))) ? R.BigMetalBounceSound
		: Victim.IsKindOf(TEXT("VEHICLE")) ? R.SmallMetalBounceSound : FString();
	if (!Sound.IsEmpty())
	{
		Logic->PlaySoundAt(Sound, Victim);
	}
}

void UGenGameLogic::SetTemporaryObstacle(FGenObject& Obj, bool bOn)
{
	// Pathfinder::createAWallFromMyFootprint: an obstacle for as long as it stands there (and gone with it if it dies so).
	if (bOn == ObstacleOwners.Contains(Obj.GetID()))
	{
		return;
	}
	if (bOn)
	{
		Pathfinder.AddObstacle(Obj);
		ObstacleOwners.Add(Obj.GetID());
	}
	else
	{
		Pathfinder.RemoveObstacle(Obj);
		ObstacleOwners.Remove(Obj.GetID());
	}
}
