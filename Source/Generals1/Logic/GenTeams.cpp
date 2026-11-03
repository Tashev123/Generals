// The game's one team registry (Common/RTS/Team.cpp). The map's script engine and a computer player's AI both
// work on this one: a team the AI builds is the team a script names.

#include "Logic/GenTeams.h"
#include "Core/GenPerf.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenObject.h"
#include "Logic/GenPartition.h"
#include "Generals1.h"

namespace
{
	bool IsLiving(const FGenObject* O)
	{
		return O && !O->IsDead() && !O->IsDestroyed();
	}
}

FGenTeamFactory::FGenTeamFactory(UGenGameLogic* InLogic) : Logic(InLogic)
{
}

void FGenTeamFactory::AddProtos(const TArray<FGenTeamProto>& In, int32 Player)
{
	for (const FGenTeamProto& P : In)
	{
		Protos.Add(P);
		ProtoPlayers.Add(Player);
	}
}

int32 FGenTeamFactory::FindProto(const FString& Name) const
{
	return Protos.IndexOfByPredicate([&Name](const FGenTeamProto& P) { return P.Name.Equals(Name, ESearchCase::CaseSensitive); });
}

FGenTeam* FGenTeamFactory::FindTeam(int32 TeamId) const
{
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		if (T->Id == TeamId)
		{
			return T.Get();
		}
	}
	return nullptr;
}

FGenTeam* FGenTeamFactory::FindTeamNamed(const FString& Name) const
{
	const int32 P = FindProto(Name);
	if (P == INDEX_NONE)
	{
		return nullptr;
	}
	// (The definition's latest instance, as ScriptEngine::getTeamNamed takes it.)
	for (int32 i = Teams.Num() - 1; i >= 0; --i)
	{
		if (Teams[i]->Proto == P)
		{
			return Teams[i]->bActive ? Teams[i].Get() : nullptr;
		}
	}
	return nullptr;
}

FGenTeam* FGenTeamFactory::GetOrCreateTeamNamed(const FString& Name)
{
	if (FGenTeam* T = FindTeamNamed(Name))
	{
		return T;
	}
	const int32 P = FindProto(Name);
	return P != INDEX_NONE ? &CreateTeam(P, ProtoPlayers[P]) : nullptr;
}

FGenTeam& FGenTeamFactory::CreateTeam(int32 ProtoIndex, int32 Player)
{
	if (Protos.IsValidIndex(ProtoIndex) && Protos[ProtoIndex].bSingleton)
	{
		// (A definition with one team: the one that exists, made if this is the first object that asks.)
		for (const TUniquePtr<FGenTeam>& T : Teams)
		{
			if (T->Proto == ProtoIndex)
			{
				return *T;
			}
		}
	}
	TUniquePtr<FGenTeam>& T = Teams.Add_GetRef(MakeUnique<FGenTeam>());
	T->Id = NextTeamId++;
	T->Proto = ProtoIndex;
	T->Player = Player;
	if (Protos.IsValidIndex(ProtoIndex))
	{
		// Team's constructor: an enemy is only looked for when a script cares about seeing one.
		const FGenTeamProto& Proto = Protos[ProtoIndex];
		T->bCheckEnemySighted = !Proto.OnEnemySighted.IsEmpty() || !Proto.OnAllClear.IsEmpty();
	}
	return *T;
}

FGenTeam& FGenTeamFactory::EnsureDefaultTeam(int32 Player)
{
	if (const int32* Id = DefaultTeams.Find(Player))
	{
		if (FGenTeam* T = FindTeam(*Id))
		{
			return *T;
		}
	}
	// Player::setDefaultTeam: "team<PlayerName>", and the neutral player's is just "team".
	FGenTeam& Team = CreateTeam(FindProto(TEXT("team") + (Logic->GetPlayers().IsValidIndex(Player) ? Logic->GetPlayers()[Player].ScriptName : FString())), Player);
	Team.bPlayerDefault = true;
	Team.bActive = true;
	DefaultTeams.Add(Player, Team.Id);
	return Team;
}

int32 FGenTeamFactory::CountTeamInstances(int32 ProtoIndex) const
{
	int32 Count = 0;
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		Count += T->Proto == ProtoIndex ? 1 : 0;
	}
	return Count;
}

void FGenTeamFactory::SetObjectTeam(FGenObject& Obj, FGenTeam& Team)
{
	// Object::setTeam: on another player's team, the object is that player's.
	if (Obj.GetOwner() != Team.Player)
	{
		Logic->ChangeOwner(Obj, Team.Player);
	}
	ObjectTeams.Remove(Obj.GetID());
	for (const TUniquePtr<FGenTeam>& T : Teams)
	{
		T->Members.Remove(Obj.GetID());
	}
	if (!Team.bPlayerDefault)
	{
		ObjectTeams.Add(Obj.GetID(), Team.Id);
		Team.Members.AddUnique(Obj.GetID());
		// Object::setTeam: "ai->setAttackInfo(info)", if the team's definition has a set.
		if (Protos.IsValidIndex(Team.Proto) && !Protos[Team.Proto].AttackPriorityName.IsEmpty())
		{
			if (const FGenAttackPriority* Info = Logic->FindAttackPriority(Protos[Team.Proto].AttackPriorityName))
			{
				Obj.AttackInfo = Info;
			}
		}
	}
	// (Not one still being assembled. It would have nothing left to start with: Team::setActive only sets m_created, which
	// runs the on-create script - what sends a mission's teams on their way - on a team that wasn't active yet.)
	if (!Team.bAssembling)
	{
		Team.bActive = true;
	}
}

int32 FGenTeamFactory::GetObjectTeam(FGenObjectID Object) const
{
	const FGenObject* Obj = Logic->FindObject(Object);
	if (const int32* Id = ObjectTeams.Find(Object))
	{
		// (Object::defect: a captured object is on its new owner's default team.)
		if (FGenTeam* Team = FindTeam(*Id); Obj && Team && Team->Player == Obj->GetOwner())
		{
			return *Id;
		}
	}
	const int32 Owner = Obj ? Obj->GetOwner() : INDEX_NONE;
	const int32* Default = DefaultTeams.Find(Owner);
	return Default ? *Default : 0;
}

void FGenTeamFactory::DeleteTeam(int32 TeamId)
{
	if (TeamId == 0)
	{
		return;
	}
	for (int32 i = 0; i < Teams.Num(); ++i)
	{
		if (Teams[i]->Id != TeamId)
		{
			continue;
		}
		for (FGenObjectID Id : Teams[i]->Members)
		{
			ObjectTeams.Remove(Id);
		}
		Teams.RemoveAt(i);
		return;
	}
}

void FGenTeamFactory::GetTeamMembers(const FGenTeam& Team, TArray<FGenObject*>& Out, bool bIncludeDead) const
{
	Out.Reset();
	ForEachMember(Team, [bIncludeDead](const FGenObject& O) { return !O.IsProjectile() && (bIncludeDead || !O.IsDead()); },
		[&Out](FGenObject& O) { Out.Add(&O); });
}

void FGenTeamFactory::ForEachMember(const FGenTeam& Team, TFunctionRef<bool(const FGenObject&)> Wanted, TFunctionRef<void(FGenObject&)> Fn) const
{
	auto WantedOk = [&Wanted](const FGenObject* O) { return O && !O->IsDestroyed() && Wanted(*O); };
	if (Team.bPlayerDefault)
	{
		// The default team: everything of the player's that is on no other team.
		for (FGenObject* O : Logic->GetObjectsOf(Team.Player))
		{
			if (WantedOk(O) && GetObjectTeam(O->GetID()) == Team.Id)
			{
				Fn(*O);
			}
		}
		return;
	}
	for (const FGenObjectID Id : TArray<FGenObjectID>(Team.Members))
	{
		FGenObject* O = Logic->FindObject(Id);
		if (WantedOk(O) && O->GetOwner() == Team.Player)
		{
			Fn(*O);
		}
	}
}

bool FGenTeamFactory::GetTeamCenter(const FGenTeam& Team, FVector2f& OutCenter) const
{
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members, true);
	if (Members.Num() == 0)
	{
		return false;
	}
	FVector2f Sum = FVector2f::ZeroVector;
	for (const FGenObject* O : Members)
	{
		Sum += FVector2f(O->GetPosition().X, O->GetPosition().Y);
	}
	OutCenter = Sum / float(Members.Num());
	return true;
}

// ---- what a team does for itself every frame (Team::updateState) -------------------------------

EGenRelationship FGenTeamFactory::RelationTo(const FGenTeam& From, const FGenObject& To) const
{
	// Team::getRelationship: this team's override for that team, else what the players' relation says.
	if (const EGenRelationship* R = From.OverrideRelations.Find(GetObjectTeam(To.GetID())))
	{
		return *R;
	}
	if (const EGenRelationship* R = From.OverridePlayerRelations.Find(To.GetOwner()))
	{
		return *R;
	}
	return Logic->GetRelationship(From.Player, To);
}

bool FGenTeamFactory::TeamSeesEnemy(const FGenTeam& Team) const
{
	TArray<FGenObject*> Members;
	GetTeamMembers(Team, Members, true);
	for (const FGenObject* O : Members)
	{
		if (!IsLiving(O))
		{
			continue;
		}
		const float Range = O->GetVisionRange();
		const FVector2f Pos(O->GetPosition().X, O->GetPosition().Y);
		bool bSaw = false;
		Logic->GetPartition().ForEachInRange(Pos, Range, [this, &Team, O, Pos, Range, &bSaw](const FGenObject& Other)
		{
			if (bSaw || !IsLiving(&Other) || RelationTo(Team, Other) != EGenRelationship::Enemies)
			{
				return;
			}
			// (The partition answers a little wider than the question asked; the real distance is checked here.)
			if (FVector2f::DistSquared(Pos, FVector2f(Other.GetPosition().X, Other.GetPosition().Y)) <= Range * Range)
			{
				bSaw = true;
			}
		});
		if (bSaw)
		{
			return true; // the original stops at the first member that sees one
		}
	}
	return false;
}

void FGenTeamFactory::UpdateTeamStates(TFunctionRef<void(int32, const FString&)> RunTeamScript, int32 OnlyPlayer)
{
	GEN_PERF_SCOPE("Teams.States");
	// The teams are walked in the order they were made, not grouped by definition as the original's
	// PlayerList->Player->TeamPrototype walks them; a script can disband the team it runs on, so a team is
	// looked up again after each script.
	for (int32 i = 0; i < Teams.Num(); ++i)
	{
		FGenTeam* Team = Teams[i].Get();
		Team->bEnteredOrExited = false; // what the previous frame's objects set is read by this frame's scripts, then cleared
		if (OnlyPlayer != INDEX_NONE && Team->Player != OnlyPlayer)
		{
			continue;
		}
		if (!Team->bActive || !Protos.IsValidIndex(Team->Proto))
		{
			continue; // a team still being assembled has nothing to say
		}
		const FGenTeamProto& Proto = Protos[Team->Proto];
		const int32 TeamId = Team->Id;

		// Who is left, and whether all of it is standing still. (Only what has an AI can be busy, and only what
		// has an AI counts as "somebody is still there" for the all-clear.)
		TArray<FGenObject*> Members;
		GetTeamMembers(*Team, Members, true);
		int32 Alive = 0;
		bool bIdle = true, bAnyoneLeft = false;
		for (FGenObject* O : Members)
		{
			if (!IsLiving(O))
			{
				continue;
			}
			++Alive;
			if (O->CanMove())
			{
				bAnyoneLeft = true;
				bIdle &= O->IsAIIdle();
			}
		}

		if (Team->bCreated)
		{
			Team->bCreated = false;
			if (!Proto.OnDestroyed.IsEmpty())
			{
				Team->CurUnits = Alive;
				Team->DestroyThreshold = FMath::Clamp(Alive - int32(float(Alive) * Proto.DestroyedThreshold), 0, FMath::Max(0, Alive - 1));
			}
			if (!Proto.OnCreate.IsEmpty())
			{
				RunTeamScript(TeamId, Proto.OnCreate);
				Team = FindTeam(TeamId);
				if (!Team)
				{
					continue;
				}
			}
		}

		if (Team->bCheckEnemySighted)
		{
			Team->bPrevSeeEnemy = Team->bSeeEnemy;
			Team->bSeeEnemy = TeamSeesEnemy(*Team);
			if (bAnyoneLeft && Team->bPrevSeeEnemy != Team->bSeeEnemy)
			{
				const FString& Name = Team->bSeeEnemy ? Proto.OnEnemySighted : Proto.OnAllClear;
				if (!Name.IsEmpty())
				{
					RunTeamScript(TeamId, Name);
					Team = FindTeam(TeamId);
					if (!Team)
					{
						continue;
					}
				}
			}
		}

		if (Team && !Proto.OnDestroyed.IsEmpty())
		{
			const int32 Prev = Team->CurUnits;
			Team->CurUnits = Alive;
			if (Alive != Prev && Alive <= Team->DestroyThreshold)
			{
				Team->DestroyThreshold = -1; // once
				RunTeamScript(TeamId, Proto.OnDestroyed);
				Team = FindTeam(TeamId);
				if (!Team)
				{
					continue;
				}
			}
		}

		if (Team && !Proto.OnIdle.IsEmpty())
		{
			const bool bWasIdle = Team->bWasIdle;
			Team->bWasIdle = bIdle;
			if (bAnyoneLeft && bIdle && bWasIdle)
			{
				RunTeamScript(TeamId, Proto.OnIdle);
			}
		}
	}

	// A team with nobody left is gone: singletons, a player's default team and teams still being assembled stay.
	for (int32 i = Teams.Num() - 1; i >= 0; --i)
	{
		const FGenTeam& Team = *Teams[i];
		if (OnlyPlayer != INDEX_NONE && Team.Player != OnlyPlayer)
		{
			continue;
		}
		if (!Team.bPlayerDefault && Team.bActive && Team.Members.Num() == 0 && !(Protos.IsValidIndex(Team.Proto) && Protos[Team.Proto].bSingleton))
		{
			DeleteTeam(Team.Id);
		}
	}
}

void FGenTeamFactory::NotifyTeamOfObjectDeath(FGenObject& Obj, TFunctionRef<void(int32, const FString&)> RunTeamScript)
{
	// Object::destroy tells the team the object was on. It has to be looked up while the object is still here.
	const int32 TeamId = GetObjectTeam(Obj.GetID());
	const FGenTeam* Team = FindTeam(TeamId);
	if (!Team || !Protos.IsValidIndex(Team->Proto))
	{
		return;
	}
	const FString& Name = Protos[Team->Proto].OnUnitDestroyed;
	if (Name.IsEmpty())
	{
		return;
	}
	if (DeathNotifyDepth >= 16)
	{
		return; // (a script run here can destroy more of the same team's members)
	}
	++DeathNotifyDepth;
	RunTeamScript(TeamId, Name);
	--DeathNotifyDepth;
}
