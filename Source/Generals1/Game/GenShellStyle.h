#pragma once

#include "CoreMinimal.h"

/** The shell's colours (the menus, the options, the score screen), for every file that draws a part of it. */
namespace GenShellStyle
{
	inline const FLinearColor ShellGold(1.f, 0.8f, 0.22f);
	inline const FLinearColor ShellTextColor(0.86f, 0.9f, 1.f);
	inline const FLinearColor ShellDim(0.45f, 0.5f, 0.6f);
	inline const FLinearColor ShellFill(0.03f, 0.06f, 0.13f, 0.92f);
	inline const FLinearColor ShellHot(0.1f, 0.24f, 0.5f, 0.95f);
	inline const FLinearColor ShellEdge(0.22f, 0.42f, 0.78f);
	inline const FLinearColor ShellRed(1.f, 0.3f, 0.25f);

	/** The layout every shell screen is designed in, scaled to fit the screen and centred. */
	inline constexpr float DesignWidth = 1600.f;
	inline constexpr float DesignHeight = 900.f;
}
