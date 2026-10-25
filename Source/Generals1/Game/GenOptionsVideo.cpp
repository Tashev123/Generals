// The options' video page: window mode, resolution, VSync, frame rate limit, quality, resolution scale, ray-traced
// lighting, the frame rate read-out - and the "Keep these settings?" countdown after the display was changed.
// The original's OptionsMenu.wnd had a resolution list and a detail setting (Low / Medium / High / Custom over
// GameLOD.ini's options); this is the same idea over the engine's scalability groups.
//
// Ids for test automation (see GenOptions.cpp; drop-downs take "Id=Item label", the slider "Id=75"):
//   Video.DisplayMode, Video.Resolution, Video.FrameRate, Video.Preset,
//   Video.Shadows, Video.Textures, Video.Effects, Video.ViewDistance, Video.AntiAliasing, Video.PostProcess,
//   Video.Foliage, Video.Shading                  the drop-downs
//   Video.ResolutionScale                         the slider (percent)
//   Video.VSync, Video.ShowFPS, Video.RayTracing  the check boxes (a click toggles)
//   Video.Keep, Video.Revert                      the countdown's buttons, after Apply changed the display

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"
#include "Logic/GenGameLogic.h"
#include "Map/GenMapActor.h"
#include "Generals1.h"
#include "Components/PostProcessComponent.h"
#include "Containers/Ticker.h"
#include "Engine/Canvas.h"
#include "Engine/Engine.h"
#include "EngineUtils.h"
#include "HAL/IConsoleManager.h"
#include "Kismet/KismetSystemLibrary.h"
#include "Misc/App.h"
#include "RenderResource.h"
#include "RenderUtils.h"

using namespace GenShellStyle;

namespace
{
	const TArray<FString>& WindowModeNames()
	{
		// (EWindowMode's order: Fullscreen, WindowedFullscreen, Windowed.)
		static const TArray<FString> Names = { TEXT("Fullscreen"), TEXT("Borderless"), TEXT("Windowed") };
		return Names;
	}

	const TArray<FString>& LevelNames()
	{
		static const TArray<FString> Names = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic") };
		return Names;
	}

	const TArray<FString>& PresetNames()
	{
		static const TArray<FString> Names = { TEXT("Low"), TEXT("Medium"), TEXT("High"), TEXT("Epic"), TEXT("Custom") };
		return Names;
	}

	constexpr int32 CustomPreset = 4;
	/** The game's preset when nothing is stored: High, not Epic - the laptop this is played on runs hot. */
	constexpr int32 DefaultPreset = 2;

	/** The frame rate limits offered (0: none). */
	const TArray<float>& FrameRates()
	{
		static const TArray<float> Rates = { 30.f, 60.f, 90.f, 120.f, 144.f, 165.f, 240.f, 0.f };
		return Rates;
	}

	FString FrameRateName(float Rate)
	{
		return Rate <= 0.f ? FString(TEXT("Unlimited")) : FString::Printf(TEXT("%d fps"), FMath::RoundToInt32(Rate));
	}

	FString ResolutionName(const FIntPoint& R)
	{
		return FString::Printf(TEXT("%d x %d"), R.X, R.Y);
	}

	int32 ClampLevel(int32 Level)
	{
		// (Cinematic, 4, is the engine's level for offline renders: shown as Epic.)
		return FMath::Clamp(Level, 0, 3);
	}

	/** The scalability groups of the page, in the draft, in the order they are shown. */
	struct FGroupRow
	{
		const TCHAR* Id;
		const TCHAR* Label;
		const TCHAR* Tooltip;
		int32 FGenVideoDraft::* Field;
	};

	const FGroupRow GroupRows[] =
	{
		{ TEXT("Video.Shadows"), TEXT("Shadows"), TEXT("The sun's shadows: their resolution, softness and how far out they reach."), &FGenVideoDraft::Shadows },
		{ TEXT("Video.Textures"), TEXT("Textures"), TEXT("How much video memory texture streaming may use. Lower it on a graphics card with little memory."), &FGenVideoDraft::Textures },
		{ TEXT("Video.Effects"), TEXT("Effects"), TEXT("Particles, explosions, smoke and translucency."), &FGenVideoDraft::Effects },
		{ TEXT("Video.ViewDistance"), TEXT("View distance"), TEXT("How far away small things are still drawn, and at what detail."), &FGenVideoDraft::ViewDistance },
		{ TEXT("Video.AntiAliasing"), TEXT("Anti-aliasing"), TEXT("The quality of the temporal anti-aliasing (TSR) that smooths jagged edges."), &FGenVideoDraft::AntiAliasing },
		{ TEXT("Video.PostProcess"), TEXT("Post-processing"), TEXT("Bloom and the other effects laid over the finished picture."), &FGenVideoDraft::PostProcess },
		{ TEXT("Video.Foliage"), TEXT("Foliage"), TEXT("How much grass and small vegetation is drawn."), &FGenVideoDraft::Foliage },
		{ TEXT("Video.Shading"), TEXT("Shading"), TEXT("The quality of the materials' lighting calculations."), &FGenVideoDraft::Shading },
	};

	/** The preset the groups make: their common level, or Custom when they differ. */
	int32 PresetOf(const FGenVideoDraft& D)
	{
		const int32 First = D.*(GroupRows[0].Field);
		for (const FGroupRow& Row : GroupRows)
		{
			if (D.*(Row.Field) != First)
			{
				return CustomPreset;
			}
		}
		return First;
	}

	/**
	 * Why the display may not be changed here, or empty when it may: in the editor the window is the editor's;
	 * a test run (-ResX, -GenTestMenu, ...) asks for its own size and its pictures must come out at that size.
	 */
	FString DisplayChangeBlocked()
	{
		if (GIsEditor)
		{
			return TEXT("running in the editor");
		}
		const TCHAR* Cmd = FCommandLine::Get();
		int32 Ignored = 0;
		if (FParse::Value(Cmd, TEXT("ResX="), Ignored) || FParse::Value(Cmd, TEXT("ResY="), Ignored))
		{
			return TEXT("a resolution on the command line");
		}
		for (const TCHAR* Switch : { TEXT("GenTestMenu"), TEXT("GenShot"), TEXT("RenderOffscreen"), TEXT("unattended") })
		{
			if (FParse::Param(Cmd, Switch) || FCString::Stristr(Cmd, *FString::Printf(TEXT("-%s="), Switch)))
			{
				return FString::Printf(TEXT("a test run (-%s)"), Switch);
			}
		}
		return FString();
	}

	/** The resolutions offered for a window mode: what the display supports, the larger ones first, at most 14 (the list has to fit on the screen). */
	TArray<FIntPoint> ResolutionsFor(int32 WindowMode, const FIntPoint& Current)
	{
		TArray<FIntPoint> Found;
		const UGameUserSettings* Settings = UGenUserSettings::Get();
		const FIntPoint Desktop = Settings ? Settings->GetDesktopResolution() : FIntPoint(1920, 1080);
		if (WindowMode == EWindowMode::WindowedFullscreen)
		{
			// Borderless covers the desktop at its own resolution (the engine ignores any other).
			Found.Add(Desktop);
			return Found;
		}
		if (WindowMode == EWindowMode::Fullscreen)
		{
			UKismetSystemLibrary::GetSupportedFullscreenResolutions(Found);
		}
		else
		{
			UKismetSystemLibrary::GetConvenientWindowedResolutions(Found);
		}
		TArray<FIntPoint> List;
		for (const FIntPoint& R : Found)
		{
			// (Below 1024 x 720 the shell's 1600 x 900 layout gets too small to read.)
			if (R.X >= 1024 && R.Y >= 720)
			{
				List.AddUnique(R);
			}
		}
		if (List.Num() == 0)
		{
			List.Add(Desktop);
		}
		// A window may be any size: the one it has stays in the list. Fullscreen only offers the display's modes.
		if (Current.X > 0 && Current.Y > 0 && WindowMode == EWindowMode::Windowed)
		{
			List.AddUnique(Current);
		}
		List.Sort([](const FIntPoint& A, const FIntPoint& B) { return A.X != B.X ? A.X > B.X : A.Y > B.Y; });
		if (List.Num() > 14)
		{
			const bool bKeepCurrent = List.Find(Current) >= 14;
			List.SetNum(14);
			if (bKeepCurrent)
			{
				List[13] = Current;
			}
		}
		return List;
	}

	/** The resolution closest to (and not above) the one wanted, from a list (largest first). */
	FIntPoint PickResolution(const TArray<FIntPoint>& List, const FIntPoint& Wanted)
	{
		if (List.Num() == 0 || List.Contains(Wanted))
		{
			return Wanted;
		}
		for (const FIntPoint& R : List)
		{
			if (R.X <= Wanted.X && R.Y <= Wanted.Y)
			{
				return R;
			}
		}
		return List.Last();
	}

	// --- ray-traced lighting ------------------------------------------------------------------------------

	/**
	 * What the ray tracing switch can do on this machine. The project is built with ray tracing (r.RayTracing)
	 * and may switch it on and off as it runs (r.RayTracing.EnableOnDemand, on by default: "Dynamic"). Without
	 * a card that traces rays it is Disabled: Lumen then traces in software.
	 */
	ERayTracingMode RayTracingMode()
	{
		return GIsRHIInitialized ? GetRayTracingMode() : ERayTracingMode::Disabled;
	}

	const TCHAR* RayTracingModeName()
	{
		switch (RayTracingMode())
		{
		case ERayTracingMode::Dynamic: return TEXT("hardware, switchable");
		case ERayTracingMode::Enabled: return TEXT("hardware, always on");
		default: return TEXT("no hardware ray tracing");
		}
	}

	void SetCVarInt(const TCHAR* Name, int32 Value)
	{
		// Only when it differs: r.RayTracing.Enable rebuilds every component's render state when it is set.
		if (IConsoleVariable* CVar = IConsoleManager::Get().FindConsoleVariable(Name))
		{
			if (CVar->GetInt() != Value)
			{
				CVar->Set(Value, ECVF_SetByGameSetting);
			}
		}
	}

	/**
	 * The map's look. The original lit everything the fixed-function way (texture * (ambient + diffuse * N.L)),
	 * and GenMapActor's post-process copies that: no global illumination, no reflections. Ray-traced lighting
	 * gives that post-process Lumen instead: sunlight bouncing off the ground and the buildings, and Lumen's
	 * reflections. Cheap enough for every frame: it only writes when the map's settings differ.
	 */
	void SyncMapLighting(AGenMapActor* Map, bool bRayTraced)
	{
		UPostProcessComponent* PP = Map ? Map->GetPostProcess() : nullptr;
		if (!PP)
		{
			return;
		}
		FPostProcessSettings& S = PP->Settings;
		const EDynamicGlobalIlluminationMethod::Type GI = bRayTraced ? EDynamicGlobalIlluminationMethod::Lumen : EDynamicGlobalIlluminationMethod::None;
		const EReflectionMethod::Type Reflections = bRayTraced ? EReflectionMethod::Lumen : EReflectionMethod::None;
		if (S.DynamicGlobalIlluminationMethod != GI || S.ReflectionMethod != Reflections)
		{
			S.bOverride_DynamicGlobalIlluminationMethod = true;
			S.DynamicGlobalIlluminationMethod = GI;
			S.bOverride_ReflectionMethod = true;
			S.ReflectionMethod = Reflections;
		}
	}

	// --- "Keep these settings?" ---------------------------------------------------------------------------

	/**
	 * After Apply changed the resolution or the window mode, the new mode stands for 15 seconds unless it is
	 * kept (UGameUserSettings' ConfirmVideoMode / RevertVideoMode): a mode the display can't show leaves the
	 * player looking at a black screen, so not answering reverts. The clock is the core ticker's, not the HUD's:
	 * it runs whatever is drawn, and whether the dialog can be seen or not.
	 */
	struct FVideoModeConfirm
	{
		bool bPending = false;
		bool bDryRun = false;           // the display wasn't changed (DisplayChangeBlocked): only the dialog
		bool bSafeSaved = false;        // the file holds the last kept mode in the new one's place
		double Deadline = 0.0;
		FString Blocked;                // why it is a dry run
		FTSTicker::FDelegateHandle Ticker;
	};
	FVideoModeConfirm ModeConfirm;
	constexpr double ConfirmSeconds = 15.0;

	void FinishModeConfirm(bool bKeep, const TCHAR* Why)
	{
		if (!ModeConfirm.bPending)
		{
			return;
		}
		ModeConfirm.bPending = false;
		if (ModeConfirm.Ticker.IsValid())
		{
			FTSTicker::RemoveTicker(ModeConfirm.Ticker);
			ModeConfirm.Ticker.Reset();
		}
		UGenUserSettings* Settings = UGenUserSettings::Get();
		if (!Settings)
		{
			return;
		}
		if (bKeep)
		{
			Settings->ConfirmVideoMode();
		}
		else
		{
			Settings->RevertVideoMode();
			if (!ModeConfirm.bDryRun)
			{
				Settings->ApplyResolutionSettings(false);
			}
		}
		Settings->SaveSettings();
		const FIntPoint R = Settings->GetScreenResolution();
		UE_LOG(LogGenerals, Log, TEXT("Video: display %s (%s): %dx%d %s"), bKeep ? TEXT("kept") : TEXT("reverted"), Why, R.X, R.Y,
			*WindowModeNames()[FMath::Clamp(int32(Settings->GetFullscreenMode()), 0, 2)]);
	}

	bool TickModeConfirm(float)
	{
		if (!ModeConfirm.bPending)
		{
			ModeConfirm.Ticker.Reset();
			return false;
		}
		UGenUserSettings* Settings = UGenUserSettings::Get();
		if (!Settings)
		{
			return true;
		}
		// The options screen saved the new mode as it closed. Until it is kept the file holds the last kept one:
		// a game that dies while its screen is black starts the next time as it was before.
		if (!ModeConfirm.bSafeSaved)
		{
			ModeConfirm.bSafeSaved = true;
			const FIntPoint New = Settings->GetScreenResolution();
			const EWindowMode::Type NewMode = Settings->GetFullscreenMode();
			Settings->SetScreenResolution(Settings->GetLastConfirmedScreenResolution());
			Settings->SetFullscreenMode(Settings->GetLastConfirmedFullscreenMode());
			Settings->SaveSettings();
			Settings->SetScreenResolution(New);
			Settings->SetFullscreenMode(NewMode);
		}
		if (FPlatformTime::Seconds() >= ModeConfirm.Deadline)
		{
			ModeConfirm.Ticker.Reset(); // (returning false removes it)
			FinishModeConfirm(false, TEXT("not kept within 15 seconds"));
			return false;
		}
		return true;
	}

	void BeginModeConfirm(const FString& Blocked)
	{
		ModeConfirm.bPending = true;
		ModeConfirm.bDryRun = !Blocked.IsEmpty();
		ModeConfirm.Blocked = Blocked;
		ModeConfirm.bSafeSaved = false;
		ModeConfirm.Deadline = FPlatformTime::Seconds() + ConfirmSeconds;
		if (!ModeConfirm.Ticker.IsValid())
		{
			ModeConfirm.Ticker = FTSTicker::GetCoreTicker().AddTicker(FTickerDelegate::CreateStatic(&TickModeConfirm));
		}
	}

	// --- the frame rate read-out -------------------------------------------------------------------------

	/** Averaged over half a second: a number that changes every frame can't be read. */
	struct FFrameRateMeter
	{
		double Time = 0.0;
		int32 Frames = 0;
		float Fps = 0.f;
		float Ms = 0.f;

		void Add(double DeltaSeconds)
		{
			Time += DeltaSeconds;
			++Frames;
			if (Time >= 0.5)
			{
				Fps = float(Frames / Time);
				Ms = float(Time * 1000.0 / Frames);
				Time = 0.0;
				Frames = 0;
			}
		}
	};
	FFrameRateMeter FrameMeter;
}

void UGenUserSettings::ApplyVideoExtras()
{
	// Runs as the engine starts (ApplyNonResolutionSettings), at every StartPlay and after Apply: everything here
	// writes only what differs from what is in effect.

	// The game's defaults, once. On a first start the engine does call SetToDefaults, but then takes VSync and
	// the quality levels back from the console variables (ValidateSettings, LoadSettings): Epic, no VSync, which
	// is what overheated the laptop. So they are put in effect here, over whatever the engine settled on.
	constexpr int32 GameDefaultsVersion = 1;
	if (VideoExtras.DefaultsVersion < GameDefaultsVersion)
	{
		VideoExtras.DefaultsVersion = GameDefaultsVersion;
		SetVideoDefaults();
		UGameUserSettings::ApplyNonResolutionSettings(); // (the engine's part only: this function is called from ours)
		Scalability::SetQualityLevels(ScalabilityQuality); // (which the engine skips while it starts)
		SaveSettings();
		UE_LOG(LogGenerals, Log, TEXT("Video: the game's defaults put in effect (High, 60 fps, VSync, full resolution)"));
	}

	const bool bRayTraced = VideoExtras.bRayTracing;
	const ERayTracingMode Mode = RayTracingMode();
	if (Mode == ERayTracingMode::Dynamic)
	{
		// Off, the ray tracing scene isn't kept up at all (its acceleration structures, its geometry's memory).
		SetCVarInt(TEXT("r.RayTracing.Enable"), bRayTraced ? 1 : 0);
	}
	if (Mode != ERayTracingMode::Disabled)
	{
		SetCVarInt(TEXT("r.RayTracing.Shadows"), bRayTraced ? 1 : 0);
	}

	// Below 100% the picture is drawn smaller and scaled up: TSR, the engine's temporal upscaler, does that best.
	// (It is the engine's default anti-aliasing; this only overrides an ini that chose another one.)
	if (ScalabilityQuality.ResolutionQuality > 0.f && ScalabilityQuality.ResolutionQuality < 100.f)
	{
		SetCVarInt(TEXT("r.AntiAliasingMethod"), 4 /* AAM_TSR */);
	}

	if (GEngine)
	{
		for (const FWorldContext& Context : GEngine->GetWorldContexts())
		{
			UWorld* World = Context.World();
			if (World && (Context.WorldType == EWorldType::Game || Context.WorldType == EWorldType::PIE))
			{
				for (TActorIterator<AGenMapActor> It(World); It; ++It)
				{
					SyncMapLighting(*It, bRayTraced);
				}
			}
		}
	}
}

void UGenUserSettings::SetVideoDefaults()
{
	// A laptop runs hot at an unlimited frame rate: the game's default is 60 with VSync, at High.
	FrameRateLimit = 60.f;
	bUseVSync = true;
	SetOverallScalabilityLevel(DefaultPreset);
	// (The overall level picks a screen percentage of its own; the game draws at the full resolution.)
	ScalabilityQuality.ResolutionQuality = 100.f;
}

void AGenHUD::OptionsLoadVideo()
{
	// Opening the options again answers the countdown: whoever can find the button can see the screen.
	FinishModeConfirm(true, TEXT("the options were opened"));

	FGenVideoDraft& D = Options.Video;
	D = FGenVideoDraft();
	const UGenUserSettings* S = UGenUserSettings::Get();
	if (!S)
	{
		return;
	}
	D.WindowMode = FMath::Clamp(int32(S->GetFullscreenMode()), 0, 2);
	D.Resolution = D.WindowMode == EWindowMode::WindowedFullscreen ? S->GetDesktopResolution() : S->GetScreenResolution();
	D.bVSync = S->IsVSyncEnabled();
	D.FrameRateLimit = S->GetFrameRateLimit();
	D.Shadows = ClampLevel(S->GetShadowQuality());
	D.Textures = ClampLevel(S->GetTextureQuality());
	D.Effects = ClampLevel(S->GetVisualEffectQuality());
	D.ViewDistance = ClampLevel(S->GetViewDistanceQuality());
	D.AntiAliasing = ClampLevel(S->GetAntiAliasingQuality());
	D.PostProcess = ClampLevel(S->GetPostProcessingQuality());
	D.Foliage = ClampLevel(S->GetFoliageQuality());
	D.Shading = ClampLevel(S->GetShadingQuality());
	D.Preset = PresetOf(D);
	// (0 is "the project's default screen percentage": the full resolution, as far as this page is concerned.)
	const float Scale = S->ScalabilityQuality.ResolutionQuality;
	D.ResolutionScale = Scale <= 0.f ? 100.f : FMath::Clamp(FMath::RoundToFloat(Scale / 5.f) * 5.f, 50.f, 100.f);
	D.Extras = S->VideoExtras;
}

void AGenHUD::DrawOptionsVideo(const FBox2D& Body)
{
	FGenVideoDraft& D = Options.Video;
	const float X0 = float(Body.Min.X), Y0 = float(Body.Min.Y);
	const float ColW = 540.f;
	const float LeftX = X0, RightX = X0 + 600.f;
	const float ControlDX = 250.f, ControlW = ColW - ControlDX - 10.f, ControlH = 36.f;
	const TArray<FLinearColor> NoColors;

	auto Header = [&](const TCHAR* Text, float X, float Y)
	{
		const FBox2D R = ShellRect(X, Y, ColW, 30);
		ShellText(Text, R.Min.X, R.Min.Y, ShellGold, 19.f, 0, true);
		const FBox2D Line = ShellRect(X, Y + 30, ColW, 0);
		DrawRect(FLinearColor(ShellEdge.R, ShellEdge.G, ShellEdge.B, 0.6f), Line.Min.X, Line.Min.Y, Line.GetSize().X, FMath::Max(1.f, Shell.Scale));
	};
	// A row: its label at the left, its control at the right; the tooltip covers both.
	auto Label = [&](const FString& Text, float X, float Y, const FString& Tooltip, bool bEnabled)
	{
		const FBox2D R = ShellRect(X + 8.f, Y, ControlDX - 8.f, ControlH);
		const float TextH = float(ShellMeasure(TEXT("Ag"), 18.f).Y);
		ShellText(Text, R.Min.X, R.Min.Y + (R.GetSize().Y - TextH) * 0.5f, bEnabled ? ShellTextColor : ShellDim, 18.f);
		ShellTooltip(ShellRect(X, Y, ColW, ControlH), Tooltip);
	};
	auto Control = [&](float X, float Y) { return ShellRect(X + ControlDX, Y, ControlW, ControlH); };
	auto Check = [&](const TCHAR* Id, float X, float Y, bool& bValue, const TCHAR* On, const TCHAR* Off)
	{
		if (ShellCheckbox(Id, ShellRect(X + ControlDX, Y + 4.f, 28.f, 28.f), bValue ? On : Off, bValue))
		{
			bValue = !bValue;
		}
	};

	// --- the display ---
	Header(TEXT("DISPLAY"), LeftX, Y0);
	float Y = Y0 + 46.f;
	const float Step = 50.f;

	Label(TEXT("Display mode"), LeftX, Y, TEXT("Fullscreen takes the display over and can change its resolution. Borderless is a window covering the desktop at the desktop's resolution: switching to other programs is instant. Windowed is an ordinary window."), true);
	int32 Picked = 0;
	if (ShellDropdown(TEXT("Video.DisplayMode"), Control(LeftX, Y), WindowModeNames(), NoColors, D.WindowMode, Picked))
	{
		D.WindowMode = Picked;
		D.Resolution = PickResolution(ResolutionsFor(D.WindowMode, D.Resolution), D.Resolution);
	}
	Y += Step;

	const bool bBorderless = D.WindowMode == EWindowMode::WindowedFullscreen;
	const TArray<FIntPoint> Resolutions = ResolutionsFor(D.WindowMode, D.Resolution);
	TArray<FString> ResolutionNames;
	for (const FIntPoint& R : Resolutions)
	{
		ResolutionNames.Add(ResolutionName(R));
	}
	Label(TEXT("Resolution"), LeftX, Y, bBorderless
		? TEXT("Borderless always covers the desktop at its own resolution. To draw fewer pixels, lower the resolution scale.")
		: TEXT("The size of the picture in pixels. Only the sizes this display supports are listed."), !bBorderless);
	if (ShellDropdown(TEXT("Video.Resolution"), Control(LeftX, Y), ResolutionNames, NoColors, Resolutions.Find(D.Resolution), Picked) && !bBorderless)
	{
		D.Resolution = Resolutions[Picked];
	}
	Y += Step;

	Label(TEXT("VSync"), LeftX, Y, TEXT("Waits for the display to finish showing a picture before sending the next: no tearing, at the cost of a little input lag."), true);
	Check(TEXT("Video.VSync"), LeftX, Y, D.bVSync, TEXT("On"), TEXT("Off"));
	Y += Step;

	TArray<FString> RateNames;
	for (float Rate : FrameRates())
	{
		RateNames.Add(FrameRateName(Rate));
	}
	// (A limit set elsewhere that isn't in the list shows as the next one up.)
	int32 RateIndex = FrameRates().IndexOfByPredicate([&D](float Rate) { return Rate > 0.f && Rate >= D.FrameRateLimit - 0.5f; });
	if (D.FrameRateLimit <= 0.f || RateIndex == INDEX_NONE)
	{
		RateIndex = FrameRates().Num() - 1;
	}
	Label(TEXT("Frame rate limit"), LeftX, Y, TEXT("The most pictures drawn in a second. The game itself runs at 30 logic frames a second whatever this is: a higher limit only makes the view smoother, and the computer hotter. 60 is the default."), true);
	if (ShellDropdown(TEXT("Video.FrameRate"), Control(LeftX, Y), RateNames, NoColors, RateIndex, Picked))
	{
		D.FrameRateLimit = FrameRates()[Picked];
	}
	Y += Step;

	Label(TEXT("Resolution scale"), LeftX, Y, TEXT("Draws the world at a fraction of the resolution and scales it up with TSR, the engine's temporal upscaler; the interface stays sharp. Lower it if the game runs slowly or hot."), true);
	ShellSlider(TEXT("Video.ResolutionScale"), Control(LeftX, Y), D.ResolutionScale, 50.f, 100.f, 5.f, FString::Printf(TEXT("%d%%"), FMath::RoundToInt32(D.ResolutionScale)));
	Y += Step;

	Label(TEXT("Show frame rate"), LeftX, Y, TEXT("A small read-out in the top right corner during a game: frames per second, and the time each frame takes."), true);
	Check(TEXT("Video.ShowFPS"), LeftX, Y, D.Extras.bShowFPS, TEXT("On"), TEXT("Off"));
	Y += Step + 16.f;

	// What Apply will do to the display, said before it happens.
	const UGenUserSettings* S = UGenUserSettings::Get();
	const bool bDisplayChanges = S && (D.WindowMode != int32(S->GetFullscreenMode())
		|| (!bBorderless && D.Resolution != S->GetScreenResolution()));
	if (bDisplayChanges)
	{
		const FString Blocked = DisplayChangeBlocked();
		const FBox2D R = ShellRect(LeftX + 8.f, Y, ColW, 24.f);
		ShellText(Blocked.IsEmpty() ? FString(TEXT("Apply changes the display, then asks whether to keep it."))
			: FString::Printf(TEXT("Apply saves the display mode; it isn't changed now (%s)."), *Blocked), R.Min.X, R.Min.Y, ShellGold, 15.f);
	}

	// --- the quality ---
	Header(TEXT("QUALITY"), RightX, Y0);
	Y = Y0 + 46.f;
	Label(TEXT("Quality preset"), RightX, Y, TEXT("Sets every group below at once; changing a group by itself makes this Custom. High is the default: Epic asks a lot of the graphics card."), true);
	if (ShellDropdown(TEXT("Video.Preset"), Control(RightX, Y), PresetNames(), NoColors, D.Preset, Picked) && Picked != CustomPreset)
	{
		D.Preset = Picked;
		for (const FGroupRow& Row : GroupRows)
		{
			D.*(Row.Field) = Picked;
		}
	}
	Y += 52.f;
	for (const FGroupRow& Row : GroupRows)
	{
		Label(Row.Label, RightX, Y, Row.Tooltip, true);
		int32& Level = D.*(Row.Field);
		if (ShellDropdown(Row.Id, Control(RightX, Y), LevelNames(), NoColors, ClampLevel(Level), Picked))
		{
			Level = Picked;
			D.Preset = PresetOf(D);
		}
		Y += 42.f;
	}
	Y += 12.f;

	const bool bSoftware = RayTracingMode() == ERayTracingMode::Disabled;
	Label(TEXT("Ray-traced lighting"), RightX, Y, bSoftware
		? TEXT("Lumen global illumination and reflections: sunlight bounces off the ground and the buildings. This graphics card can't trace rays, so Lumen traces in software. Off, the game is lit as the original was. Applies right away.")
		: TEXT("Ray-traced shadows, and Lumen global illumination and reflections traced in hardware: sunlight bounces off the ground and the buildings. Off, the game is lit as the original was. Asks a lot of the graphics card. Applies right away."), true);
	Check(TEXT("Video.RayTracing"), RightX, Y, D.Extras.bRayTracing, bSoftware ? TEXT("On (software Lumen)") : TEXT("On"), TEXT("Off"));
}

void AGenHUD::OptionsApplyVideo()
{
	UGenUserSettings* S = UGenUserSettings::Get();
	if (!S)
	{
		return;
	}
	const FGenVideoDraft& D = Options.Video;

	S->SetVSyncEnabled(D.bVSync);
	S->SetFrameRateLimit(D.FrameRateLimit);
	S->SetShadowQuality(D.Shadows);
	S->SetTextureQuality(D.Textures);
	S->SetVisualEffectQuality(D.Effects);
	S->SetViewDistanceQuality(D.ViewDistance);
	S->SetAntiAliasingQuality(D.AntiAliasing);
	S->SetPostProcessingQuality(D.PostProcess);
	S->SetFoliageQuality(D.Foliage);
	S->SetShadingQuality(D.Shading);
	// The groups the page doesn't show follow the ones nearest to them: global illumination and reflections only
	// matter with ray-traced lighting (they are Lumen's quality); the landscape's isn't used (the terrain is the game's own).
	S->SetGlobalIlluminationQuality(D.Shading);
	S->SetReflectionQuality(D.Shading);
	S->ScalabilityQuality.LandscapeQuality = D.ViewDistance;
	S->ScalabilityQuality.ResolutionQuality = FMath::Clamp(D.ResolutionScale, 50.f, 100.f);
	S->VideoExtras = D.Extras;

	// The display: only when it changes, and then it has to be kept (or it goes back by itself).
	const EWindowMode::Type Mode = EWindowMode::ConvertIntToWindowMode(D.WindowMode);
	const FIntPoint Resolution = Mode == EWindowMode::WindowedFullscreen ? S->GetDesktopResolution() : D.Resolution;
	const bool bDisplayChanged = Mode != S->GetFullscreenMode() || Resolution != S->GetScreenResolution();
	FString Blocked;
	if (bDisplayChanged)
	{
		Blocked = DisplayChangeBlocked();
		S->ConfirmVideoMode(); // (what is on the screen now is what a revert goes back to)
		S->SetFullscreenMode(Mode);
		S->SetScreenResolution(Resolution);
		if (Blocked.IsEmpty())
		{
			S->ApplyResolutionSettings(false);
		}
		BeginModeConfirm(Blocked);
	}
	S->ApplyNonResolutionSettings();

	const FString DisplayNote = !bDisplayChanged ? FString(TEXT("unchanged"))
		: (Blocked.IsEmpty() ? FString(TEXT("changed, to be kept")) : FString::Printf(TEXT("saved, not changed: %s"), *Blocked));
	UE_LOG(LogGenerals, Log, TEXT("Video: applied %dx%d %s (%s), VSync %s, limit %s, quality %s (shadows %d textures %d effects %d view %d AA %d post %d foliage %d shading %d), scale %d%%, ray-traced lighting %s (%s), frame rate read-out %s"),
		Resolution.X, Resolution.Y, *WindowModeNames()[FMath::Clamp(D.WindowMode, 0, 2)], *DisplayNote,
		D.bVSync ? TEXT("on") : TEXT("off"), *FrameRateName(D.FrameRateLimit), *PresetNames()[FMath::Clamp(D.Preset, 0, CustomPreset)],
		D.Shadows, D.Textures, D.Effects, D.ViewDistance, D.AntiAliasing, D.PostProcess, D.Foliage, D.Shading,
		FMath::RoundToInt32(D.ResolutionScale), D.Extras.bRayTracing ? TEXT("on") : TEXT("off"), RayTracingModeName(),
		D.Extras.bShowFPS ? TEXT("on") : TEXT("off"));
	static IConsoleVariable* MaxFPS = IConsoleManager::Get().FindConsoleVariable(TEXT("t.MaxFPS"));
	static IConsoleVariable* VSync = IConsoleManager::Get().FindConsoleVariable(TEXT("r.VSync"));
	static IConsoleVariable* ScreenPercentage = IConsoleManager::Get().FindConsoleVariable(TEXT("r.ScreenPercentage"));
	static IConsoleVariable* RTEnable = IConsoleManager::Get().FindConsoleVariable(TEXT("r.RayTracing.Enable"));
	UE_LOG(LogGenerals, Log, TEXT("Video: in effect t.MaxFPS=%g r.VSync=%d r.ScreenPercentage=%g sg.ShadowQuality=%d r.RayTracing.Enable=%d"),
		MaxFPS ? MaxFPS->GetFloat() : -1.f, VSync ? VSync->GetInt() : -1, ScreenPercentage ? ScreenPercentage->GetFloat() : -1.f,
		Scalability::GetQualityLevels().ShadowQuality, RTEnable ? RTEnable->GetInt() : -1);
}

void AGenHUD::OptionsDefaultsVideo()
{
	// The game's defaults (SetVideoDefaults): High at 60 fps with VSync, at the full resolution - and borderless
	// at the desktop's resolution, the one mode every display shows.
	FGenVideoDraft& D = Options.Video;
	D = FGenVideoDraft();
	D.WindowMode = EWindowMode::WindowedFullscreen;
	if (const UGenUserSettings* S = UGenUserSettings::Get())
	{
		D.Resolution = S->GetDesktopResolution();
	}
	D.Preset = DefaultPreset;
	for (const FGroupRow& Row : GroupRows)
	{
		D.*(Row.Field) = DefaultPreset;
	}
	D.FrameRateLimit = 60.f;
	D.bVSync = true;
	D.ResolutionScale = 100.f;
	D.Extras = FGenVideoExtras();
}

void AGenHUD::OptionsCancelVideo()
{
	// (Nothing on this page is tried out before Apply.)
}

void AGenHUD::DrawVideoOverlay()
{
	const UGenUserSettings* S = UGenUserSettings::Get();
	if (!S || !Canvas)
	{
		return;
	}

	// The map's lighting follows the switch (the map actor may have come after the settings were applied).
	if (const UGenGameLogic* Logic = UGenGameLogic::Get(this))
	{
		SyncMapLighting(Logic->GetMapActor(), S->VideoExtras.bRayTracing);
	}

	FrameMeter.Add(FApp::GetDeltaTime());
	if (S->VideoExtras.bShowFPS && FrameMeter.Fps > 0.f)
	{
		// Small, in the corner, out of the way of the radar and the bar at the bottom.
		const FString Text = FString::Printf(TEXT("%d fps  %.1f ms"), FMath::RoundToInt32(FrameMeter.Fps), FrameMeter.Ms);
		const float Size = 14.f;
		const FVector2D TextSize = ShellMeasure(Text, Size);
		const float Pad = FMath::Max(3.f, 5.f * Shell.Scale);
		const float X = float(Canvas->SizeX - TextSize.X) - Pad * 3.f, Y = Pad;
		DrawRect(FLinearColor(0.f, 0.f, 0.f, 0.45f), X - Pad, Y, float(TextSize.X) + Pad * 2.f, float(TextSize.Y) + Pad);
		ShellText(Text, X, Y + Pad * 0.5f, FLinearColor(ShellTextColor.R, ShellTextColor.G, ShellTextColor.B, 0.85f), Size);
	}

	// "Keep these settings?" - at the top of the screen, clear of the in-game menu (which may still be open
	// under it, and whose buttons see a click before this does).
	if (ModeConfirm.bPending && !Options.bOpen)
	{
		const int32 Seconds = FMath::Max(0, FMath::CeilToInt32(ModeConfirm.Deadline - FPlatformTime::Seconds()));
		ShellPanel(ShellRect(500, 40, 600, 190), 0.97f);
		const FBox2D Title = ShellRect(800, 56, 0, 0);
		ShellText(TEXT("KEEP THESE SETTINGS?"), Title.Min.X, Title.Min.Y, ShellGold, 24.f, 1, true);
		const FBox2D Line = ShellRect(800, 98, 0, 0);
		ShellText(FString::Printf(TEXT("The display goes back to how it was in %d second%s."), Seconds, Seconds == 1 ? TEXT("") : TEXT("s")),
			Line.Min.X, Line.Min.Y, ShellTextColor, 17.f, 1);
		if (ModeConfirm.bDryRun)
		{
			const FBox2D Note = ShellRect(800, 124, 0, 0);
			ShellText(FString::Printf(TEXT("(The display itself wasn't changed: %s.)"), *ModeConfirm.Blocked), Note.Min.X, Note.Min.Y, ShellDim, 14.f, 1);
		}
		if (ShellButton(TEXT("Video.Keep"), ShellRect(585, 162, 205, 48), TEXT("KEEP")))
		{
			FinishModeConfirm(true, TEXT("kept by the player"));
		}
		else if (ShellButton(TEXT("Video.Revert"), ShellRect(810, 162, 205, 48), TEXT("REVERT")))
		{
			FinishModeConfirm(false, TEXT("reverted by the player"));
		}
	}
}
