#pragma once

#include "CoreMinimal.h"

/**
 * The shell's colours (the menus, the options, the score screen), for every file that draws a part of it.
 * Dark blue panels with gold for what matters - the colours of the original's interface, in a plainer,
 * more modern cut: layered panels with soft gradients and thin borders, the corners cut at an angle.
 */
namespace GenShellStyle
{
	// text
	inline const FLinearColor ShellGold(1.f, 0.78f, 0.24f);             // titles, what is chosen, what the mouse is on
	inline const FLinearColor ShellTextColor(0.86f, 0.9f, 1.f);         // ordinary text
	inline const FLinearColor ShellDim(0.47f, 0.53f, 0.66f);            // labels, hints, what can't be used
	inline const FLinearColor ShellRed(1.f, 0.32f, 0.27f);
	inline const FLinearColor ShellGreen(0.4f, 0.9f, 0.45f);
	// surfaces
	inline const FLinearColor ShellFill(0.03f, 0.055f, 0.12f, 0.94f);   // a control at rest
	inline const FLinearColor ShellHot(0.09f, 0.2f, 0.42f, 0.96f);      // ... with the mouse on it, or chosen
	inline const FLinearColor ShellEdge(0.22f, 0.4f, 0.74f);            // a control's border
	inline const FLinearColor ShellPanelTop(0.022f, 0.04f, 0.085f);      // a panel's gradient, top to bottom
	inline const FLinearColor ShellPanelBottom(0.006f, 0.012f, 0.03f);
	inline const FLinearColor ShellInset(0.01f, 0.02f, 0.05f, 0.85f);   // fields and lists sunk into a panel
	inline const FLinearColor ShellLine(0.3f, 0.45f, 0.8f, 0.28f);      // dividers
	inline const FLinearColor ShellAccent(0.36f, 0.62f, 1.f);           // the blue light of a control in use

	/** The layout every shell screen is designed in, scaled to fit the screen and centred. */
	inline constexpr float DesignWidth = 1600.f;
	inline constexpr float DesignHeight = 900.f;
}
