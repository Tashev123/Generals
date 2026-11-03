// A campaign mission from the game mode's side: starting one (from the campaign menu, or -GenCampaign= in a test run),
// and carrying out on the game thread what its scripts want seen and heard (FGenScriptClientEvent): the camera, the
// letterbox and captions, speech, music and sounds, and the input they take away.

#include "Game/GenGameMode.h"
#include "Audio/GenAudioManager.h"
#include "Client/GenFXManager.h"
#include "Core/GenCoords.h"
#include "Game/GenCameraPawn.h"
#include "Game/GenCampaign.h"
#include "Game/GenHUD.h"
#include "Game/GenPlayerController.h"
#include "Logic/GenGameLogic.h"
#include "Logic/GenMapScripts.h"
#include "Map/GenMapActor.h"
#include "Camera/CameraComponent.h"
#include "Generals1.h"

void AGenGameMode::RequestCampaignStart(const FString& Campaign, const FString& Mission, int32 Difficulty)
{
	if (Phase != EGenGamePhase::Menu)
	{
		return;
	}
	// ("Loading" goes up first; the next ticks read the map.)
	ShellError.Reset();
	PendingCampaign = Campaign;
	PendingMission = Mission;
	PendingDifficulty = Difficulty;
	Phase = EGenGamePhase::Loading;
	LoadingTicks = 0;
}

bool AGenGameMode::BeginCampaign(const FString& CampaignName, const FString& MissionName, int32 Difficulty, bool bFromMenu)
{
	// CampaignManager: the mission's map, played by the map's own sides, the local player the human one.
	const FGenCampaign* Campaign = FGenCampaignData::Get().FindCampaign(CampaignName);
	const FGenCampaignMission* Mission = Campaign ? Campaign->FindMission(MissionName.IsEmpty() ? Campaign->FirstMission : MissionName) : nullptr;
	if (!Mission)
	{
		ShellError = FString::Printf(TEXT("There is no mission '%s' in the campaign '%s'"), *MissionName, *CampaignName);
		UE_LOG(LogGenerals, Error, TEXT("%s"), *ShellError);
		return false;
	}
	FGenMatchSettings Settings = FGenMatchSettings::FromCommandLine();
	Settings.Campaign = Campaign->Name;
	Settings.Mission = Mission->Name;
	Settings.Map = Mission->Map;
	if (Campaign->bChallenge)
	{
		// (ChallengeGenerals::getCurrentPlayerTemplateNum: the general's own; not all of Campaign.ini's say it.)
		const FGenChallengeGeneral* General = FGenCampaignData::Get().FindGeneralByCampaign(Campaign->Name);
		Settings.ChallengeFaction = General ? General->PlayerTemplate : Campaign->PlayerFaction;
	}
	Settings.Slots.Reset();
	Settings.DefaultDifficulty = FMath::Clamp(Difficulty, 0, 2);
	UE_LOG(LogGenerals, Log, TEXT("Campaign: %s %s (%s), difficulty %d"), *Campaign->Name, *Mission->Name, *Mission->Map, Settings.DefaultDifficulty);
	if (!BeginMatch(Settings, bFromMenu))
	{
		return false;
	}
	if (bFromMenu)
	{
		SetMenuScreen(Campaign->bChallenge ? EGenMenuScreen::Challenge : EGenMenuScreen::Campaign); // (the menu comes back here after the score screen)
	}
	// The original's view starts over the map's InitialCameraPosition (the scripts take it from there).
	APlayerController* PC = GetWorld()->GetFirstPlayerController();
	if (AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr)
	{
		FVector Start;
		// (The Challenge: "Using Player_N_Start as the multiplayer initial camera position".)
		if (MapActor->GetWaypointLocation(Campaign->bChallenge ? TEXT("Player_1_Start") : TEXT("InitialCameraPosition"), Start))
		{
			Cam->SetTarget(Start);
		}
	}
	ScriptEvents.Reset();
	IntroPhase = 0;
	IntroSkipHeld = 0.f;
	bSkippingIntro = false;
	bTestHoldSpace = false;
	SkippedTrack.Reset();
	return true;
}

void AGenGameMode::TickCampaign(float DeltaSeconds)
{
	UGenGameLogic* Logic = UGenGameLogic::Get(this);
	const FGenMapScripts* Scripts = Logic && Logic->IsRunning() ? Logic->GetMapScripts() : nullptr;
	if (!Scripts)
	{
		return;
	}
	AGenPlayerController* PC = Cast<AGenPlayerController>(GetWorld()->GetFirstPlayerController());
	AGenCameraPawn* Cam = PC ? Cast<AGenCameraPawn>(PC->GetPawn()) : nullptr;
	AGenHUD* HUD = PC ? Cast<AGenHUD>(PC->GetHUD()) : nullptr;
	UGenAudioManager* Audio = UGenAudioManager::Get(this);
	// (What the skipped part of an intro wants heard or read is past by the time it could be. So is what a saved game
	// wanted before it was saved.)
	const bool bSkipped = bSkippingIntro || bResumingSave;

	// NAMED_SELECTED asks what is selected: an order, as anything else the simulation learns from the player.
	if (PC && Logic->GetMapScripts() && PC->GetSelection() != LastScriptSelection)
	{
		LastScriptSelection = PC->GetSelection();
		FGenCommand Note(EGenCommandType::NoteSelection);
		Note.Units = LastScriptSelection;
		Logic->SubmitCommand(MoveTemp(Note));
	}
	ScriptEvents.Reset();
	Logic->TakeScriptClientEvents(ScriptEvents);
	for (const FGenScriptClientEvent& E : ScriptEvents)
	{
		const FVector2f Pos(E.Pos.X, E.Pos.Y);
		switch (E.Action)
		{
		// --- the camera (W3DView) ---
		case EGenScriptAction::A_SETUP_CAMERA:
			if (Cam && E.bHasPos && E.bHasPos2) { Cam->ScriptSetup(Pos, E.Real(1), E.Real(2), FVector2f(E.Pos2.X, E.Pos2.Y)); }
			break;
		case EGenScriptAction::A_MOVE_CAMERA_TO:
			if (Cam && E.bHasPos) { Cam->ScriptMoveTo(Pos, E.Real(1) * 1000.f, E.Real(3) * 1000.f, E.Real(4) * 1000.f, true); }
			break;
		case EGenScriptAction::A_RESET_CAMERA:
			if (Cam && E.bHasPos) { Cam->ScriptReset(Pos, E.Real(1) * 1000.f, E.Real(2) * 1000.f, E.Real(3) * 1000.f); }
			break;
		case EGenScriptAction::A_CAMERA_LOOK_TOWARD_WAYPOINT:
			if (Cam && E.bHasPos) { Cam->ScriptRotateToward(Pos, E.Real(1) * 1000.f, E.Real(2) * 1000.f, E.Real(3) * 1000.f, E.Int(4) != 0); }
			break;
		case EGenScriptAction::A_ZOOM_CAMERA:
			if (Cam) { Cam->ScriptZoomTo(E.Real(0), E.Real(1) * 1000.f, E.Real(2) * 1000.f, E.Real(3) * 1000.f); }
			break;
		case EGenScriptAction::A_PITCH_CAMERA:
			if (Cam) { Cam->ScriptPitchTo(E.Real(0), E.Real(1) * 1000.f, E.Real(2) * 1000.f, E.Real(3) * 1000.f); }
			break;
		case EGenScriptAction::A_ROTATE_CAMERA:
			if (Cam) { Cam->ScriptRotate(E.Real(0), E.Real(1) * 1000.f, E.Real(2) * 1000.f, E.Real(3) * 1000.f); }
			break;
		case EGenScriptAction::A_CAMERA_MOD_SET_FINAL_ZOOM:
			if (Cam) { Cam->ScriptModFinalZoom(E.Real(0), E.Real(1), E.Real(2)); }
			break;
		case EGenScriptAction::A_CAMERA_MOD_SET_FINAL_PITCH:
			if (Cam) { Cam->ScriptModFinalPitch(E.Real(0), E.Real(1), E.Real(2)); }
			break;
		case EGenScriptAction::A_MOVE_CAMERA_ALONG_WAYPOINT_PATH:
			if (Cam) { Cam->ScriptMoveAlong(E.Path, E.Real(1) * 1000.f, E.Real(3) * 1000.f, E.Real(4) * 1000.f); }
			break;
		case EGenScriptAction::A_CAMERA_LOOK_TOWARD_OBJECT:
			if (Cam && E.bHasPos)
			{
				Cam->ScriptRotateToward(Pos, E.Real(1) * 1000.f, E.Real(3) * 1000.f, E.Real(4) * 1000.f, false);
				ScriptCameraLook = E.Str(0);
				ScriptCameraLookFrom = E.Frame + uint32(E.Real(1) * float(GenLogic::GetLogicFPS()));
				ScriptCameraLookUntil = ScriptCameraLookFrom + uint32(E.Real(2) * float(GenLogic::GetLogicFPS()));
			}
			break;
		case EGenScriptAction::A_CAMERA_MOD_FINAL_LOOK_TOWARD:
			if (Cam && E.bHasPos) { Cam->ScriptModLookToward(Pos, true); }
			break;
		case EGenScriptAction::A_CAMERA_ADD_SHAKER_AT:
			if (UGenFXManager* FX = UGenFXManager::Get(this); FX && E.bHasPos && !bSkipped)
			{
				FX->ShakeAt(E.Pos, E.Real(3), E.Real(1));
			}
			break;
		case EGenScriptAction::A_WATER_CHANGE_HEIGHT_OVER_TIME:
			if (MapActor) { MapActor->RebuildWater(); }
			break;
		case EGenScriptAction::A_CAMERA_MOD_FREEZE_ANGLE:
			if (Cam) { Cam->ScriptModFreezeAngle(); }
			break;
		case EGenScriptAction::A_CAMERA_MOD_LOOK_TOWARD:
			// doModCameraLookToward: the move that is under way turns to look at the waypoint; nothing if the map
			// has no waypoint of that name (the event then carries no position).
			if (Cam && E.bHasPos) { Cam->ScriptModLookToward(Pos); }
			break;
		case EGenScriptAction::A_SCREEN_SHAKE:
			// doScreenShake: a shake where the camera looks.
			if (UGenFXManager* FX = UGenFXManager::Get(this); FX && Cam && !bSkipped)
			{
				const FVector Gen = GenCoords::FromUE(Cam->GetTarget());
				FX->Shake(FVector3f(Gen), E.Int(0));
			}
			break;
		case EGenScriptAction::A_CAMERA_BW_MODE_BEGIN:
		case EGenScriptAction::A_CAMERA_BW_MODE_END:
			// W3DView's black-and-white filter: the colour taken out of the view.
			if (Cam && Cam->GetCamera())
			{
				FPostProcessSettings& PP = Cam->GetCamera()->PostProcessSettings;
				PP.bOverride_ColorSaturation = E.Action == EGenScriptAction::A_CAMERA_BW_MODE_BEGIN;
				PP.ColorSaturation = FVector4(1.f, 1.f, 1.f, 0.f);
			}
			break;

		// --- the letterbox and captions ---
		case EGenScriptAction::A_CAMERA_LETTERBOX_BEGIN:
		case EGenScriptAction::A_CAMERA_LETTERBOX_END:
			if (HUD) { HUD->OnScriptEvent(E, Logic); }
			break;
		case EGenScriptAction::A_SHOW_MILITARY_CAPTION:
			if (HUD && !bSkipped) { HUD->OnScriptEvent(E, Logic); }
			break;

		// --- sound ---
		case EGenScriptAction::A_SPEECH_PLAY:
			if (Audio && !bSkipped) { Audio->PlaySpeech(E.Str(0), E.Int(1) != 0); }
			break;
		case EGenScriptAction::A_PLAY_SOUND_EFFECT:
			if (Audio && !bSkipped) { Audio->PlayUI(E.Str(0)); }
			break;
		case EGenScriptAction::A_SOUND_PLAY_NAMED:
		case EGenScriptAction::A_PLAY_SOUND_EFFECT_AT:
			if (Audio && E.bHasPos && !bSkipped) { Audio->PlayAt(E.Str(0), E.Pos); }
			break;
		case EGenScriptAction::A_MUSIC_SET_TRACK:
			if (bSkipped) { SkippedTrack = E.Str(0); }
			else if (Audio) { Audio->PlayScriptTrack(E.Str(0)); }
			break;
		case EGenScriptAction::A_MUSIC_SET_VOLUME:
			if (Audio) { Audio->SetScriptMusicVolume(E.Real(0) / 100.f); }
			break;
		case EGenScriptAction::A_SOUND_SET_VOLUME:
			if (Audio) { Audio->SetScriptSoundVolume(E.Real(0) / 100.f); }
			break;
		case EGenScriptAction::A_SOUND_DISABLE_TYPE:
			if (Audio) { Audio->SetScriptEventVolume(E.Str(0), 0.f); }
			break;
		case EGenScriptAction::A_SOUND_ENABLE_TYPE:
			if (Audio) { Audio->SetScriptEventVolume(E.Str(0), -1.f); }
			break;
		case EGenScriptAction::A_AUDIO_OVERRIDE_VOLUME_TYPE:
			if (Audio) { Audio->SetScriptEventVolume(E.Str(0), E.Real(1) / 100.f); }
			break;
		case EGenScriptAction::A_SOUND_ENABLE_ALL:
		case EGenScriptAction::A_AUDIO_RESTORE_VOLUME_ALL_TYPE:
			if (Audio) { Audio->ClearScriptEventVolumes(); }
			break;
		case EGenScriptAction::A_SOUND_REMOVE_ALL_DISABLED:
			if (Audio) { Audio->StopDisabledVoices(); }
			break;
		case EGenScriptAction::A_EVA_SET_ENABLED_DISABLED:
			if (Audio) { Audio->SetEvaEnabled(E.Int(0) != 0); }
			break;

		// --- the view ---
		case EGenScriptAction::A_DRAW_SKYBOX_BEGIN:
		case EGenScriptAction::A_DRAW_SKYBOX_END:
			if (MapActor) { MapActor->SetSkyBoxVisible(E.Action == EGenScriptAction::A_DRAW_SKYBOX_BEGIN); }
			break;
		case EGenScriptAction::A_ENABLE_BORDER_SHROUD:
		case EGenScriptAction::A_DISABLE_BORDER_SHROUD:
			Logic->SetBorderShroud(E.Action == EGenScriptAction::A_ENABLE_BORDER_SHROUD);
			break;
		case EGenScriptAction::A_OVERSIZE_TERRAIN:          // (All the terrain is drawn, always.)
		case EGenScriptAction::A_RESIZE_VIEW_GUARDBAND:     // (Nothing is left out by the old view's guard band.)
		case EGenScriptAction::A_OPTIONS_SET_PARTICLE_CAP_MODE: // (GameLODManager's dynamic detail: there is none here to turn off.)
		case EGenScriptAction::A_OPTIONS_SET_DRAWICON_UI_MODE:  // (The HUD asks the scripts.)
			break;
		case EGenScriptAction::A_SPEECH_SET_VOLUME: // (The missions only ever set it back to 100.)
			break;
		case EGenScriptAction::A_CAMERA_FOLLOW_NAMED:
			ScriptCameraFollow = E.Str(0);
			break;
		case EGenScriptAction::A_CAMERA_STOP_FOLLOW:
			ScriptCameraFollow.Reset();
			break;
		case EGenScriptAction::A_CAMERA_MOTION_BLUR_JUMP:
			if (Cam && E.bHasPos) { Cam->ScriptMoveTo(Pos, 0.f, 0.f, 0.f, false); }
			break;
		case EGenScriptAction::A_SHOW_WEATHER:
			if (UGenFXManager* FX = UGenFXManager::Get(this)) { FX->SetSnowVisible(E.Int(0) != 0); }
			break;
		case EGenScriptAction::A_SOUND_REMOVE_TYPE:
			// The loop the scripts started is over; the event is not disabled, so it can be asked for again.
			if (Audio) { Audio->RemoveEvent(E.Str(0)); }
			break;

		default:
			// (What isn't carried out here is either the logic's, or has nothing to show in this game yet.)
			UE_CLOG(Logic->IsTracing(), LogGenerals, Log, TEXT("  [script f%u] client action %d: nothing to do here"), E.Frame, int32(E.Action));
			break;
		}
	}
	if (!ScriptCameraLook.IsEmpty() && Cam && Logic->GetMapScripts())
	{
		const FGenObject* Looked = Logic->GetMapScripts()->GetUnitNamed(ScriptCameraLook);
		if (!Looked || Logic->GetFrame() >= ScriptCameraLookUntil)
		{
			ScriptCameraLook.Reset();
		}
		else if (Logic->GetFrame() >= ScriptCameraLookFrom)
		{
			const FVector3f At = Looked->GetDrawPosition();
			Cam->ScriptRotateToward(FVector2f(At.X, At.Y), 1.f, 0.f, 0.f, false);
		}
	}
	if (!ScriptCameraFollow.IsEmpty() && Cam && Logic->GetMapScripts())
	{
		if (const FGenObject* Followed = Logic->GetMapScripts()->GetUnitNamed(ScriptCameraFollow))
		{
			const FVector3f At = Followed->GetDrawPosition();
			Cam->ScriptFollow(FVector2f(At.X, At.Y));
		}
	}

	// DISABLE_INPUT: no orders, nothing selected, and the camera stays the scripts' until it is back.
	const bool bInputOff = Scripts->IsInputDisabled();
	// Hold Space to skip the intro: the first time the scripts have the input, near the start of the mission.
	if (IntroPhase == 0 && bInputOff)
	{
		IntroPhase = 1;
	}
	else if (IntroPhase == 0 && Logic->GetFrame() > uint32(GenLogic::GetLogicFPS()) * 5)
	{
		IntroPhase = 2; // (no intro on this map)
	}
	else if (IntroPhase == 1 && !bInputOff)
	{
		IntroPhase = 2;
	}
	if (bSkippingIntro && !Logic->IsCinematicSkipping())
	{
		bSkippingIntro = false;
		if (Audio && !SkippedTrack.IsEmpty())
		{
			Audio->PlayScriptTrack(SkippedTrack);
		}
		SkippedTrack.Reset();
	}
	if (IntroPhase == 1 && !bSkippingIntro && !Logic->IsGameOver())
	{
		const bool bHeld = !bResumingSave && (bTestHoldSpace || (PC && PC->IsInputKeyDown(EKeys::SpaceBar) && !bQuitMenuOpen));
		IntroSkipHeld = bHeld ? IntroSkipHeld + DeltaSeconds : 0.f;
		if (IntroSkipHeld >= 1.f)
		{
			IntroSkipHeld = 0.f;
			bSkippingIntro = true;
			if (Audio)
			{
				Audio->StopAllVoices();
			}
			if (HUD)
			{
				HUD->ClearMilitaryCaption();
			}
			Logic->StartCinematicSkip();
			UE_LOG(LogGenerals, Log, TEXT("Intro: skipping from frame %u"), Logic->GetFrame());
		}
	}
	if (PC)
	{
		PC->SetScriptInputDisabled(bInputOff);
	}
	if (Cam)
	{
		Cam->SetScriptHold(bInputOff);
	}
}
