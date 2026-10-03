#include "Map/GenMapActor.h"
#include "Client/GenDrawable.h"
#include "Core/GenCoords.h"
#include "Core/GenFileSystem.h"
#include "Logic/GenGameData.h"
#include "Map/GenMapData.h"
#include "Map/GenTerrain.h"
#include "W3D/GenModelComponent.h"
#include "Generals1.h"
#include "Components/DirectionalLightComponent.h"
#include "Components/ExponentialHeightFogComponent.h"
#include "Components/PostProcessComponent.h"
#include "Engine/World.h"
#include "Materials/MaterialParameterCollection.h"
#include "Materials/MaterialParameterCollectionInstance.h"

AGenMapActor::AGenMapActor()
{
	PrimaryActorTick.bCanEverTick = false;
	RootComponent = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));

	Terrain = CreateDefaultSubobject<UGenTerrainComponent>(TEXT("Terrain"));
	Terrain->SetupAttachment(RootComponent);

	auto MakeLight = [this](const TCHAR* Name, bool bShadows)
	{
		UDirectionalLightComponent* L = CreateDefaultSubobject<UDirectionalLightComponent>(Name);
		L->SetupAttachment(RootComponent);
		L->SetCastShadows(bShadows);
		L->SetAtmosphereSunLight(false);
		L->SetIntensity(0.f);
		return L;
	};
	Sun = MakeLight(TEXT("Sun"), true);
	Sun->SetDynamicShadowCascades(4);
	Sun->SetDynamicShadowDistanceMovableLight(60000.f);
	Fill1 = MakeLight(TEXT("Fill1"), false);
	Fill2 = MakeLight(TEXT("Fill2"), false);

	Fog = CreateDefaultSubobject<UExponentialHeightFogComponent>(TEXT("Fog"));
	Fog->SetupAttachment(RootComponent);
	Fog->SetFogDensity(0.0002f);

	// Reproduce the original fixed-function look: color = tex * (ambient + diffuse * N.L).
	// Ambient comes from MPC_GenLighting (material emissive); no GI, no SSR, linear tone curve.
	PostProcess = CreateDefaultSubobject<UPostProcessComponent>(TEXT("PostProcess"));
	PostProcess->SetupAttachment(RootComponent);
	PostProcess->bUnbound = true;
	FPostProcessSettings& S = PostProcess->Settings;
	S.bOverride_AutoExposureMethod = true;
	S.AutoExposureMethod = AEM_Manual;
	S.bOverride_AutoExposureApplyPhysicalCameraExposure = true;
	S.AutoExposureApplyPhysicalCameraExposure = false;
	S.bOverride_AutoExposureBias = true;
	S.AutoExposureBias = 0.f;
	S.bOverride_DynamicGlobalIlluminationMethod = true;
	S.DynamicGlobalIlluminationMethod = EDynamicGlobalIlluminationMethod::None;
	S.bOverride_ReflectionMethod = true;
	S.ReflectionMethod = EReflectionMethod::None;
	S.bOverride_ToneCurveAmount = true;
	S.ToneCurveAmount = 0.f;
	S.bOverride_VignetteIntensity = true;
	S.VignetteIntensity = 0.f;
	S.bOverride_BloomIntensity = true;
	S.BloomIntensity = 0.2f;
}

void AGenMapActor::BeginDestroy()
{
	Super::BeginDestroy();
}

void AGenMapActor::LoadMap()
{
	LoadMapByName(MapName);
}

void AGenMapActor::ClearMap()
{
	for (UGenModelComponent* M : ObjectModels)
	{
		if (M) { M->DestroyComponent(); }
	}
	ObjectModels.Reset();
	if (Terrain) { Terrain->Clear(); }
	Map.Reset();
}

bool AGenMapActor::LoadMapByName(const FString& Name)
{
	ClearMap();
	MapName = Name;

	FGenGameData& GD = FGenGameData::Get();
	if (!GD.Initialize())
	{
		return false;
	}

	const double Start = FPlatformTime::Seconds();
	const FString Folder = FString::Printf(TEXT("Maps\\%s"), *Name);
	TSharedPtr<FGenMapData> NewMap = MakeShared<FGenMapData>();
	if (!NewMap->LoadFromVfs(Folder / (Name + TEXT(".map"))))
	{
		return false;
	}
	Map = NewMap;
	GD.ApplyMapIni(Folder);

	Terrain->Build(*Map);
	SetupLighting();
	if (bSpawnMapObjects)
	{
		SpawnMapObjects();
	}
	UE_LOG(LogGenerals, Log, TEXT("Loaded map '%s' (%dx%d, %d objects, %d rendered) in %.2fs"),
		*Name, Map->Width, Map->Height, Map->Objects.Num(), ObjectModels.Num(), FPlatformTime::Seconds() - Start);
	return true;
}

namespace
{
	/** The original lit in gamma space; convert map colors so linear-space lighting lands on the same output. */
	FLinearColor GammaToLinear(const FLinearColor& C)
	{
		auto F = [](float V) { return FMath::Pow(FMath::Clamp(V, 0.f, 4.f), 2.2f); };
		return FLinearColor(F(C.R), F(C.G), F(C.B), 1.f);
	}

	/** Manual exposure at bias 0 scales the scene by 1/1.2. */
	constexpr float ExposureCompensation = 1.2f;
}

void AGenMapActor::SetupLighting()
{
	const FGenMapLighting* L = Map ? Map->GetCurrentLighting() : nullptr;
	UDirectionalLightComponent* Lights[3] = { Sun, Fill1, Fill2 };
	for (int32 i = 0; i < 3; ++i)
	{
		const bool bValid = L && L->Objects.IsValidIndex(i) && !L->Objects[i].Diffuse.Equals(FLinearColor::Black, 0.001f);
		if (!bValid)
		{
			Lights[i]->SetIntensity(0.f);
			Lights[i]->SetVisibility(false);
			continue;
		}
		const FGenMapLight& Src = L->Objects[i];
		// Map "lightPos" is the direction the light travels (HeightMap.cpp negates it for N.L).
		const FVector3f D = GenCoords::DirToUE(Src.Direction.X, Src.Direction.Y, Src.Direction.Z);
		Lights[i]->SetWorldRotation(FVector(D).GetSafeNormal().Rotation());
		const FLinearColor Lin = GammaToLinear(Src.Diffuse);
		const float Max = FMath::Max3(Lin.R, Lin.G, Lin.B);
		Lights[i]->SetVisibility(true);
		Lights[i]->SetLightColor(Max > 0.f ? Lin / Max : FLinearColor::White);
		// Lambert: radiance = albedo * E * N.L / pi, so E = pi * diffuse reproduces albedo * diffuse * N.L.
		Lights[i]->SetIntensity(PI * Max * ExposureCompensation);
	}

	// Ambient terms go to the material parameter collection (emissive = color * ambient).
	UMaterialParameterCollection* MPC = LoadObject<UMaterialParameterCollection>(nullptr, TEXT("/Game/Generals/Materials/MPC_GenLighting.MPC_GenLighting"));
	UWorld* World = GetWorld();
	if (MPC && World)
	{
		if (UMaterialParameterCollectionInstance* Inst = World->GetParameterCollectionInstance(MPC))
		{
			const FLinearColor TerrainAmb = L && L->Terrain.Num() > 0 ? L->Terrain[0].Ambient : FLinearColor(0.3f, 0.3f, 0.3f);
			const FLinearColor ObjectAmb = L && L->Objects.Num() > 0 ? L->Objects[0].Ambient : FLinearColor(0.3f, 0.3f, 0.3f);
			Inst->SetVectorParameterValue(TEXT("TerrainAmbient"), GammaToLinear(TerrainAmb) * ExposureCompensation);
			Inst->SetVectorParameterValue(TEXT("ObjectAmbient"), GammaToLinear(ObjectAmb) * ExposureCompensation);
		}
	}
	if (L && L->Terrain.Num() > 0)
	{
		Fog->SetFogInscatteringColor(GammaToLinear(L->Terrain[0].Ambient + L->Terrain[0].Diffuse * 0.5f));
	}
}

void AGenMapActor::SpawnMapObjects()
{
	const FGenGameData& GD = FGenGameData::Get();
	TMap<FString, int32> Missing;
	for (const FGenMapObject& O : Map->Objects)
	{
		if (O.IsWaypoint() || (O.Flags & (FGenMapObject::FlagRoadFlags | FGenMapObject::FlagBridgeFlags)) != 0)
		{
			continue;
		}
		const FGenThingTemplate* T = GD.FindTemplate(O.TemplateName);
		if (!T)
		{
			Missing.FindOrAdd(O.TemplateName)++;
			continue;
		}
		SpawnObjectVisual(*T, O.Location, O.Angle);
	}
	for (const TPair<FString, int32>& It : Missing)
	{
		UE_LOG(LogGenerals, Verbose, TEXT("Map object template not found: %s (x%d)"), *It.Key, It.Value);
	}
}

UGenModelComponent* AGenMapActor::SpawnObjectVisual(const FGenThingTemplate& Template, const FVector3f& GenLocation, float Angle)
{
	if (!Map)
	{
		return nullptr;
	}
	UGenModelComponent* MC = CreateObjectVisual(Template, FLinearColor(0.2f, 0.4f, 1.f));
	if (MC)
	{
		const float GroundZ = Map->GetGroundHeight(GenLocation.X, GenLocation.Y);
		MC->SetRelativeLocation(GenCoords::ToUE(GenLocation.X, GenLocation.Y, GenLocation.Z + GroundZ));
		MC->SetRelativeRotation(FRotator(0.f, GenCoords::AngleToYaw(Angle), 0.f));
	}
	return MC;
}

UGenModelComponent* AGenMapActor::CreateObjectVisual(const FGenThingTemplate& Template, const FLinearColor& HouseColor)
{
	const FGenDrawModule* Draw = Template.PrimaryDraw();
	const FString Model = Draw ? Draw->DefaultModel() : FString();
	if (Model.IsEmpty())
	{
		return nullptr;
	}

	UGenModelComponent* MC = NewObject<UGenModelComponent>(this, NAME_None, RF_Transient);
	MC->SetupAttachment(RootComponent);
	MC->RegisterComponent();
	MC->SetHouseColor(HouseColor);
	if (!MC->SetModel(Model))
	{
		MC->DestroyComponent();
		return nullptr;
	}
	// TIME_OF_DAY_NIGHT -> MODELCONDITION_NIGHT
	const bool bNight = Map && Map->TimeOfDay == 4;
	FGenConditionBits Flags;
	Flags.Set(GenConditions::Night(), bNight);
	GenDrawable::ApplyState(MC, *Draw, Draw->FindBestState(Flags), bNight);
	ObjectModels.Add(MC);
	return MC;
}

UGenModelComponent* AGenMapActor::CreateModelVisual(const FString& Model, const FLinearColor& HouseColor)
{
	UGenModelComponent* MC = NewObject<UGenModelComponent>(this, NAME_None, RF_Transient);
	MC->SetupAttachment(RootComponent);
	MC->RegisterComponent();
	MC->SetHouseColor(HouseColor);
	if (!MC->SetModel(Model))
	{
		MC->DestroyComponent();
		return nullptr;
	}
	return MC;
}

bool AGenMapActor::GetWaypointLocation(const FString& WaypointName, FVector& OutLocation) const
{
	const FGenMapObject* W = Map ? Map->FindWaypoint(WaypointName) : nullptr;
	if (!W)
	{
		return false;
	}
	OutLocation = GetActorTransform().TransformPosition(GenCoords::ToUE(W->Location.X, W->Location.Y, Map->GetGroundHeight(W->Location.X, W->Location.Y)));
	return true;
}

float AGenMapActor::GetGroundZ(const FVector& UELocation) const
{
	if (!Map)
	{
		return 0.f;
	}
	const FVector Local = GetActorTransform().InverseTransformPosition(UELocation);
	const FVector Gen = GenCoords::FromUE(Local);
	return float(GenCoords::ToUE(0.f, 0.f, Map->GetGroundHeight(Gen.X, Gen.Y)).Z + GetActorLocation().Z);
}

FBox AGenMapActor::GetPlayableBoundsUE() const
{
	if (!Map)
	{
		return FBox(ForceInit);
	}
	const FBox2f B = Map->GetPlayableBounds();
	const FVector A = GenCoords::ToUE(B.Min.X, B.Min.Y, 0.f);
	const FVector C = GenCoords::ToUE(B.Max.X, B.Max.Y, 0.f);
	return FBox(FVector(FMath::Min(A.X, C.X), FMath::Min(A.Y, C.Y), -1e5), FVector(FMath::Max(A.X, C.X), FMath::Max(A.Y, C.Y), 1e5))
		.ShiftBy(GetActorLocation());
}

bool AGenMapActor::RaycastTerrain(const FVector& Start, const FVector& Dir, FVector& OutHit) const
{
	if (!Map)
	{
		return false;
	}
	// March along the ray, then refine with bisection.
	const FVector D = Dir.GetSafeNormal();
	const double Step = GenCoords::MapXYFactor * GenCoords::UnitsToCm * 0.5;
	FVector Prev = Start;
	double PrevDelta = Start.Z - GetGroundZ(Start);
	if (PrevDelta < 0.0)
	{
		return false;
	}
	for (int32 i = 1; i < 8000; ++i)
	{
		const FVector P = Start + D * (Step * i);
		const double Delta = P.Z - GetGroundZ(P);
		if (Delta <= 0.0)
		{
			FVector Lo = Prev, Hi = P;
			for (int32 k = 0; k < 16; ++k)
			{
				const FVector Mid = (Lo + Hi) * 0.5;
				if (Mid.Z - GetGroundZ(Mid) > 0.0) { Lo = Mid; } else { Hi = Mid; }
			}
			OutHit = (Lo + Hi) * 0.5;
			return true;
		}
		Prev = P;
		PrevDelta = Delta;
	}
	return false;
}
