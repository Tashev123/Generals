#pragma once

#include "CoreMinimal.h"

/** ControlBarSchemeImage: a picture of the bar's frame, in a layer (0 on top, 5 at the back; 3-5 under the windows). */
struct FGenControlBarImagePart
{
	FVector2D Position = FVector2D::ZeroVector;   // in the scheme's creation resolution
	FVector2D Size = FVector2D::ZeroVector;
	FString Image;
	int32 Layer = 0;
};

/** The images of one of the bar's own buttons ("...Enable", "...Hightlited", "...Pushed", "...Disabled"). */
struct FGenControlBarButtonImages
{
	FString Enable, Highlighted, Pushed, Disabled;
};

/**
 * ControlBarScheme (ControlBarScheme.ini): how a side dresses the control bar - its frame, the images of
 * its own buttons and where they go, the border colours of the command buttons by kind, the colour of the
 * build-up clock. Positions are in ScreenCreationRes (800 x 600).
 */
struct FGenControlBarScheme
{
	FString Name;
	FString Side;
	FIntPoint CreationRes = FIntPoint(800, 600);
	FString QueueButtonImage;
	FString RightHUDImage;             // the picture in the right panel when nothing is selected
	FString ExpBarForegroundImage;
	FString GenArrow;
	FString CommandMarkerImage;
	FString PowerPurchaseImage;        // the background of the promotions panel
	FColor CommandBarBorderColor = FColor::Black;
	FColor BuildUpClockColor = FColor(0, 0, 0, 160);
	FColor BorderBuild = FColor::White, BorderAction = FColor::White, BorderUpgrade = FColor::White, BorderSystem = FColor::White;
	FString GenBarButtonIn, GenBarButtonOn;
	FGenControlBarButtonImages ToggleUp, ToggleDown;   // (Enable = In, Highlighted = On)
	FGenControlBarButtonImages Options, IdleWorker, Buddy, Beacon, General, UAttack, MinMax;
	FBox2D MinMaxRect = FBox2D(ForceInit), GeneralRect = FBox2D(ForceInit), UAttackRect = FBox2D(ForceInit);
	FBox2D OptionsRect = FBox2D(ForceInit), WorkerRect = FBox2D(ForceInit), ChatRect = FBox2D(ForceInit), BeaconRect = FBox2D(ForceInit);
	FBox2D PowerBarRect = FBox2D(ForceInit), MoneyRect = FBox2D(ForceInit);
	TArray<FGenControlBarImagePart> ImageParts;
};

namespace GenControlBarSchemes
{
	/** ControlBarSchemeManager::setControlBarSchemeByPlayerTemplate: the scheme of that side ("America",
	 *  "AmericaSuperWeaponGeneral", ...), else "Default". */
	const FGenControlBarScheme* FindBySide(const FString& Side);
}
