#pragma once

#include "CoreMinimal.h"

struct FGenImage;

/** The three sections of the bar: each stands on its own over the world. */
enum EGenBarPod : int32 { BarPodRadar, BarPodCommands, BarPodRight, BarPodCount };

/**
 * Where things go on the sections of the bar. Rectangles are in a section's own units: (0, 0) at its top left corner,
 * FGenControlBar::Scale pixels to a unit. The cameo cells have the cameos' own 5:4 shape.
 */
struct FGenBarShape
{
	enum ESide : uint8 { America, China, GLA };

	ESide Side = America;
	FVector2D PodSize[BarPodCount];
	TArray<FVector2f> Outline[BarPodCount];   // the section's edge, closed
	// The radar's section (its size follows the map's shape)
	FBox2D RadarWell = FBox2D(ForceInit);     // the pocket the radar lies in
	FBox2D Radar = FBox2D(ForceInit);
	float RadarCorner = 0.f;
	FBox2D Lamp = FBox2D(ForceInit);          // the "under attack" light
	// The commands' section
	FBox2D Money = FBox2D(ForceInit);
	FBox2D Power = FBox2D(ForceInit);
	FBox2D Keys[2] = { FBox2D(ForceInit), FBox2D(ForceInit) };  // the options and idle worker buttons
	FBox2D CommandWindow = FBox2D(ForceInit);
	FVector2D Grid = FVector2D::ZeroVector;   // the first command cell
	// The right section
	FBox2D RightWell = FBox2D(ForceInit);
	FBox2D General = FBox2D(ForceInit);
	FVector2D Queue = FVector2D::ZeroVector;  // the first of the production queue's nine cells
	FBox2D ExpFill = FBox2D(ForceInit);       // GeneralsExp: what fills up
	FVector2D ExpStar = FVector2D::ZeroVector;
};

namespace GenBarLayout
{
	constexpr float CellW = 49.f, CellH = 39.2f, CellGapX = 4.f, CellGapY = 4.f;
	constexpr float QueueW = 36.f, QueueH = 28.8f, QueueGap = 2.4f;
	constexpr float ScreenMargin = 6.f;     // between a section and the edge of the screen
	constexpr float SectionGap = 8.f;       // between the sections, side by side (gen.HudSpread 0)
	constexpr float ShadowPad = 12.f;       // what a section's picture has round it, for its shadow
	// The power shortcuts' rail: on the screen's right edge, its bottom RailLift above the screen's, its cells from the top.
	constexpr float RailW = 46.f, RailTop = 6.f, RailPitch = 33.5f, RailCellX = 5.5f, RailCellW = 35.f, RailCellH = 28.f, RailLift = 230.f;
}

/** The parts of the bar a style draws besides its sections. */
enum class EGenHudPart : uint8
{
	OptionsKey,       // Variant: EGenHudKeyState
	WorkerKey,        // Variant: EGenHudKeyState
	GeneralKey,       // Variant: EGenHudKeyState (Hover: the stars lit)
	LampOff,
	LampOn,           // the lamp's rectangle grown by Variant pixels all round, for its glow
	PowerFillGood,    // a capsule the whole length of the power bar, in the colours of its three states
	PowerFillTight,
	PowerFillShort,
	ExpFill,          // a capsule the whole height of the experience gauge
	ShortcutRail,     // the power shortcuts' rail with ShadowPad units round it; Variant: how many powers it holds
	CellRing,         // white, for tinting: the outline of a cell; Variant: its corner in pixels
	CellFill,         // white, for tinting: a filled cell (and the hotkey's tab); Variant: its corner in pixels
	CellCorners,      // the corners outside a cell's rounded rectangle, in the colour of the glass; Variant: its corner in pixels
};

enum EGenHudKeyState : int32 { HudKeyNormal, HudKeyHover, HudKeyDisabled };

/**
 * A look of the in-game bar, chosen on the options' video page. To add one: derive from this, say what its sections
 * are, make its parts, and add it to the list in GenHudStyles::All (GenHudStyle.cpp). Everything is made from code when
 * the game runs, at the pixels it takes on the screen; the cameos are the game's own pictures, drawn as they are.
 */
class FGenHudStyle
{
public:
	FGenHudStyle(const TCHAR* InId, const TCHAR* InName) : Id(InId), Name(InName) {}
	virtual ~FGenHudStyle() = default;

	/** What the settings file stores: never changes once a style is out. */
	const FString Id;
	/** What the options page shows. */
	const FString Name;

	/** Where things go on this style's sections, for that side (FGenBarShape::ESide) and a map of that width over height. */
	virtual const FGenBarShape& GetShape(int32 Side, float MapAspect) const = 0;
	/** One section with ShadowPad units round it, at Scale pixels to the unit; straight alpha. False: it can't be made. */
	virtual bool BuildPod(const FGenBarShape& Shape, int32 Pod, float Scale, FGenImage& Out) const = 0;
	/** Whether a section covers this place of itself (in its own units): what of it takes the mouse. */
	virtual bool IsPodSolid(const FGenBarShape& Shape, int32 Pod, const FVector2D& Place) const = 0;
	/** One part at Width x Height pixels. False: this style doesn't make it. */
	virtual bool BuildPart(const FGenBarShape& Shape, EGenHudPart Part, int32 Variant, int32 Width, int32 Height, float Scale, FGenImage& Out) const { return false; }
	/** The corners of the cameos, in units. */
	virtual float GetCameoCorner() const { return 0.f; }
};

namespace GenHudStyles
{
	/** Every style, in the order the options page lists them. */
	const TArray<const FGenHudStyle*>& All();
	/** The one in use: the settings' (gen.HudStyle over them), or the first if that one isn't known. */
	const FGenHudStyle& Current();
	const FGenHudStyle* Find(const FString& Id);

	/** GenHudMetal.cpp: adds the machined metal styles. */
	void AddMetalStyles(TArray<const FGenHudStyle*>& List);
}
