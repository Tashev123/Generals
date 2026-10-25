// The options' controls page: scroll and zoom speeds, edge scrolling, the mouse buttons' layout, the pointer.
// Ids for test automation: see GenOptions.cpp; this page's own are "Controls.*":
//   Controls.EdgeScroll=1.5, Controls.KeyScroll=, Controls.DragScroll=, Controls.Zoom=, Controls.Rotate=,
//   Controls.Pointer=    the speeds, as multipliers (0.25 .. 3; 1 is the game's own)
//   Controls.Scheme=<Classic|Modern>
//   Controls.EdgeScrollOn, Controls.EdgeScrollWindowed, Controls.InvertDrag, Controls.Confine   (toggled)
// What they do is AGenPlayerController's (UpdateCamera, WorldLeftClick / RightClick, UpdateMouseConfine,
// UpdatePointerSpeed), which reads the settings as they are each frame: Apply is all it takes.

#include "Game/GenHUD.h"
#include "Game/GenShellStyle.h"
#include "Game/GenSettings.h"

using namespace GenShellStyle;

namespace
{
	/** What the speed sliders go between, as multipliers of the game's own speed. */
	constexpr float SpeedMin = 0.25f;
	constexpr float SpeedMax = 3.f;
	constexpr float SpeedStep = 0.05f;

	FString PercentText(float Multiplier)
	{
		return FString::Printf(TEXT("%d%%"), FMath::RoundToInt(Multiplier * 100.f));
	}
}

void AGenHUD::OptionsLoadControls()
{
	if (const UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Options.Controls = Settings->Controls;
	}
}

void AGenHUD::DrawOptionsControls(const FBox2D& Body)
{
	FGenControlSettings& C = Options.Controls;
	const float X0 = Body.Min.X, Y0 = Body.Min.Y;
	const float ColumnW = 540.f;
	const float LeftX = 0.f, RightX = 600.f;     // the two columns, from the page's left
	const float SliderX = 230.f, SliderW = 310.f; // within a column

	// A column's heading, with a rule under it.
	auto Heading = [&](float X, const TCHAR* Text)
	{
		const FBox2D R = ShellRect(X0 + X, Y0, ColumnW, 30);
		ShellText(Text, R.Min.X, R.Min.Y, ShellGold, 20.f, 0, true);
		DrawRect(ShellEdge, R.Min.X, R.Max.Y, R.GetSize().X, FMath::Max(1.f, Shell.Scale));
	};
	// A line of text centred on a row of height H.
	auto Label = [&](float X, float Y, float H, const FString& Text, const FLinearColor& Color, float Size)
	{
		const FBox2D R = ShellRect(X0 + X, Y0 + Y, 0, H);
		const float TextH = float(ShellMeasure(Text, Size).Y);
		ShellText(Text, R.Min.X, R.Min.Y + (float(R.GetSize().Y) - TextH) * 0.5f, Color, Size);
	};
	// A speed: its name, the slider (a percentage of the game's own), the tooltip over the whole row.
	auto SpeedRow = [&](float X, float Y, const TCHAR* Id, const TCHAR* Name, float& Value, const TCHAR* Tip)
	{
		Label(X, Y, 30, Name, ShellTextColor, 18.f);
		ShellSlider(Id, ShellRect(X0 + X + SliderX, Y0 + Y, SliderW, 30), Value, SpeedMin, SpeedMax, SpeedStep, PercentText(Value));
		ShellTooltip(ShellRect(X0 + X, Y0 + Y - 8, ColumnW, 46), Tip);
	};
	// A toggle, with the tooltip over its row.
	auto Toggle = [&](float X, float Y, const TCHAR* Id, const TCHAR* Name, bool& bValue, const TCHAR* Tip)
	{
		if (ShellCheckbox(Id, ShellRect(X0 + X, Y0 + Y, ColumnW - X, 26), Name, bValue))
		{
			bValue = !bValue;
		}
		ShellTooltip(ShellRect(X0 + X, Y0 + Y - 6, ColumnW - X, 38), Tip);
	};

	// --- the camera: how fast it goes, and what moves it ---
	Heading(LeftX, TEXT("CAMERA"));
	SpeedRow(LeftX, 50, TEXT("Controls.EdgeScroll"), TEXT("Edge scroll speed"), C.EdgeScrollSpeed,
		TEXT("How fast the view scrolls when the mouse is held against the edge of the screen. 100% is the game's own speed."));
	SpeedRow(LeftX, 100, TEXT("Controls.KeyScroll"), TEXT("Keyboard scroll speed"), C.KeyScrollSpeed,
		TEXT("How fast the view scrolls with the arrow keys (or the keys the hotkeys page gives scrolling). 100% is the game's own speed."));
	SpeedRow(LeftX, 150, TEXT("Controls.DragScroll"), TEXT("Drag scroll speed"), C.DragScrollSpeed,
		TEXT("How fast the view scrolls while the right mouse button is held and dragged. 100% is the game's own speed."));
	SpeedRow(LeftX, 200, TEXT("Controls.Zoom"), TEXT("Zoom speed"), C.ZoomSpeed,
		TEXT("How far each step of the mouse wheel zooms the view in or out. 100% is the game's own step."));
	SpeedRow(LeftX, 250, TEXT("Controls.Rotate"), TEXT("Rotate speed"), C.RotateSpeed,
		TEXT("How fast the view turns while the middle mouse button (the wheel) is held and dragged. 100% is the game's own speed."));

	Toggle(LeftX, 316, TEXT("Controls.EdgeScrollOn"), TEXT("Scroll at the edge of the screen"), C.bEdgeScroll,
		TEXT("Moving the mouse against the edge of the screen scrolls the view that way."));
	Toggle(LeftX + 36, 356, TEXT("Controls.EdgeScrollWindowed"), TEXT("Also when the game is in a window"), C.bEdgeScrollWindowed,
		TEXT("In a window the edge is easy to touch on the way to something else: off, edge scrolling works only in fullscreen and borderless modes."));
	Toggle(LeftX, 396, TEXT("Controls.InvertDrag"), TEXT("Invert drag scrolling"), C.bInvertDragScroll,
		TEXT("Right-drag pulls the map with the mouse, the other way round from the original: drag left and the view goes right."));

	// --- the mouse: which button does what, the pointer ---
	Heading(RightX, TEXT("MOUSE"));
	Label(RightX, 46, 36, TEXT("Button layout"), ShellTextColor, 18.f);
	{
		static const TArray<FString> Schemes = { TEXT("Classic"), TEXT("Modern") };
		int32 Picked = INDEX_NONE;
		if (ShellDropdown(TEXT("Controls.Scheme"), ShellRect(X0 + RightX + SliderX, Y0 + 46, SliderW, 36), Schemes, TArray<FLinearColor>(), int32(C.MouseScheme), Picked))
		{
			C.MouseScheme = Picked == 1 ? EGenMouseScheme::Modern : EGenMouseScheme::Classic;
		}
		ShellTooltip(ShellRect(X0 + RightX, Y0 + 40, ColumnW, 48),
			TEXT("Classic is the original's: the left button selects and gives orders. Modern: the left button selects, the right gives orders. The cursor always shows the order a click would give."));
	}
	// What the chosen layout does, in a few lines.
	{
		const bool bModern = C.MouseScheme == EGenMouseScheme::Modern;
		const TCHAR* Lines[] =
		{
			bModern ? TEXT("Left click: select. On the ground: deselect.") : TEXT("Left click: select, or give an order (move, attack, ...)."),
			bModern ? TEXT("Right click: give an order (move, attack, ...).") : TEXT("Right click: deselect."),
			TEXT("Left drag: select a group.   Right drag: scroll."),
			TEXT("Placing or aiming: left to do it, right to cancel."),
		};
		const FBox2D Box = ShellRect(X0 + RightX, Y0 + 98, ColumnW, 112);
		DrawRect(ShellFill, Box.Min.X, Box.Min.Y, Box.GetSize().X, Box.GetSize().Y);
		DrawRect(ShellEdge, Box.Min.X, Box.Min.Y, FMath::Max(2.f, 2.f * Shell.Scale), Box.GetSize().Y);
		for (int32 i = 0; i < int32(UE_ARRAY_COUNT(Lines)); ++i)
		{
			Label(RightX + 16, 106 + i * 25, 22, Lines[i], i < 2 ? ShellTextColor : ShellDim, 15.f);
		}
	}
	Label(RightX, 234, 30, TEXT("Pointer speed"), ShellTextColor, 18.f);
	Label(RightX, 258, 20, TEXT("experimental"), ShellDim, 13.f);
	ShellSlider(TEXT("Controls.Pointer"), ShellRect(X0 + RightX + SliderX, Y0 + 234, SliderW, 30), C.CursorSpeed, SpeedMin, SpeedMax, SpeedStep, PercentText(C.CursorSpeed));
	ShellTooltip(ShellRect(X0 + RightX, Y0 + 226, ColumnW, 56),
		TEXT("Experimental. How fast the pointer moves in a game, over Windows' own pointer speed (100%: untouched). Done by moving the cursor itself; the menus keep Windows' speed."));
	Toggle(RightX, 316, TEXT("Controls.Confine"), TEXT("Keep the mouse in the game's window"), C.bConfineMouse,
		TEXT("While a game is on the pointer can't leave the window - for edge scrolling on more than one screen. The menus, and switching to another program, let it go."));
	Label(RightX, 396, 26, TEXT("Middle drag turns the view, the wheel zooms."), ShellDim, 15.f);
}

void AGenHUD::OptionsApplyControls()
{
	if (UGenUserSettings* Settings = UGenUserSettings::Get())
	{
		Settings->Controls = Options.Controls;
	}
}

void AGenHUD::OptionsDefaultsControls()
{
	Options.Controls = FGenControlSettings();
}

void AGenHUD::OptionsCancelControls()
{
}
