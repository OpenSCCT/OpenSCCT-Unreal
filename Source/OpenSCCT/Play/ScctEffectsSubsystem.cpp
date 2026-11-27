#include "Play/ScctEffectsSubsystem.h"
#include "Play/ScctEchelonLightFilter.h"
#include "Demo/ScctDemoPlayback.h"

#include "Play/ScctRuntimeSkins.h"
#include "Components/DecalComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "Misc/CommandLine.h"
#include "HAL/IConsoleManager.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Misc/Parse.h"
#include "Hud/ScctVisionSubsystem.h"
#include "Misc/ScopeExit.h"
#include "OpenSCCT.h"
#include "Play/ScctLevelStateSubsystem.h"
#include "Play/ScctEquipmentSubsystem.h"
#include "Play/ScctExposure.h"
#include "Play/ScctFirstPersonView.h"
#include "Play/ScctLightPlan.h"
#include "Play/ScctLightRules.h"
#include "Play/ScctPawnLighting.h"
#include "Play/ScctPawnProxy.h"
#include "Play/ScctPlayerController.h"
#include "Play/ScctSessionSubsystem.h"
#include "Runtime/ScctFeatures.h"

namespace
{
	// The lights on the bodies of the local player's side (UpdateBodyFill): their strengths in a moderately lit
	// room, in the map lights' units, and how far they reach. The fill stands above the camera's right shoulder,
	// the rim behind the local body and above it. scct.BodyFill and scct.BodyRim change the strengths while the
	// game runs (0 switches a light off).
	constexpr float BodyFillIntensity = 1.6f;
	constexpr float BodyRimIntensity = 3.0f;
	constexpr float BodyFillRadius = 700.0f;
	constexpr float BodyRimRadius = 520.0f;
	constexpr float BodyFillRaise = 55.0f;
	constexpr float BodyFillAside = 70.0f;
	constexpr float BodyRimBehind = 150.0f;
	constexpr float BodyRimRaise = 115.0f;
	constexpr float BodyRimAside = 60.0f;
	// How fast the lights follow the exposure, per second.
	constexpr float BodyLightFollow = 6.0f;

	TAutoConsoleVariable<float> CVarBodyFill(TEXT("scct.BodyFill"), BodyFillIntensity,
		TEXT("Strength of the soft light from over the camera's shoulder on the bodies of the local player's own side (0: off)."), ECVF_Default);
	TAutoConsoleVariable<float> CVarBodyRim(TEXT("scct.BodyRim"), BodyRimIntensity,
		TEXT("Strength of the edge light from behind the local player's body on the bodies of his own side (0: off)."), ECVF_Default);

	// The local body's near fade (UpdateBodyFade): what of it is nearer the camera than this, in cm, thins out of
	// the picture and is gone at half the distance. The camera of a spy who walks stands about 90 cm behind him;
	// with the gun drawn it stands 30 to 40 cm behind his back, and the arm and gun reach from there to 70 cm
	// (at 70 they were a see-through ghost in front of the picture). The head goes whole a little further out
	// (AScctPawnProxy::SetNearFade).
	TAutoConsoleVariable<float> CVarBodyFade(TEXT("scct.BodyFade"), 40.0f,
		TEXT("Distance from the camera, cm, under which the local player's own body fades out of the picture (gone at half of it; 0: never)."), ECVF_Default);

	constexpr int32 MaxLights = 16;
	constexpr int32 MaxMeshes = 96;
	constexpr int32 MaxTracers = 48;
	constexpr int32 MaxImpactSlots = 64;

	// A tracer is drawn as the last stretch of its flight.
	constexpr float TracerLength = 260.0f;
	constexpr float TracerWidth = 3.5f;

	// The simulation's light brightness (0..255) in candelas. Level lights are imported at 0.45 per unit
	// (Tools/Import/import_map.py); an effect's light is a flash that has to read against them.
	constexpr float CandelasPerBrightness = 2.0f;

	// Half the width of a bullet mark, cm, by what was hit (the mark's own DrawScale is laid over it): the hole
	// is the middle third of that and the chipped ring around it the rest (the original's mark was about 12 cm
	// across; smaller than these it cannot be made out from two metres). A rifle round leaves a crater the
	// size of a coin in concrete, a clean hole with a bright lip in sheet metal, a wider star in glass.
	constexpr float DecalHalfSizeConcrete = 5.5f;
	constexpr float DecalHalfSizeMetal = 3.6f;
	constexpr float DecalHalfSizeWood = 4.4f;
	constexpr float DecalHalfSizeGlass = 6.5f;
	constexpr float DecalHalfSizeSoft = 4.6f;

	const TCHAR* const GlowMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxGlow.M_ScctFxGlow");
	const TCHAR* const SmokeMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxSmoke.M_ScctFxSmoke");
	const TCHAR* const CloudMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxCloud.M_ScctFxCloud");
	const TCHAR* const DecalMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxDecal.M_ScctFxDecal");

	// The spy's smoke grenade cloud, SSmokeEmitter0's numbers (shared/fx/effects.json; spec/equipment/smoke-grenade.md):
	// fifteen sprites let out at 200 a second, thrown out sideways at up to 2000 a second and up at 400 to 1500 and
	// stopped within a quarter of a second by what each axis loses, so the cloud stands at once: about four metres by
	// five across and three high. Each sprite lives the cloud's ten seconds, is there within a tenth of one and fades
	// out over the last two, and swells from 17 to 30 times its start size (a half width of 8 to 10) the whole time.
	// The sprites go through walls, as the original's do (no collision), and are one of the four cells of its picture.
	constexpr int32 GrenadeCloudSprites = 15;
	constexpr float GrenadeCloudPerSecond = 200.0f;
	constexpr float GrenadeCloudLife = 10.0f;
	constexpr float GrenadeCloudFadeInEnd = 0.1f;
	constexpr float GrenadeCloudFadeOutStart = 8.0f;
	constexpr float GrenadeCloudSpeedSide = 2000.0f;
	constexpr float GrenadeCloudSpeedUpMin = 400.0f;
	constexpr float GrenadeCloudSpeedUpMax = 1500.0f;
	const FVector GrenadeCloudLossMin(10.0, 8.0, 5.0);
	const FVector GrenadeCloudLossMax(10.0, 8.0, 9.0);
	constexpr float GrenadeCloudRise = 2.0f;
	constexpr float GrenadeCloudHalfSizeMin = 8.0f;
	constexpr float GrenadeCloudHalfSizeMax = 10.0f;
	constexpr float GrenadeCloudScaleStart = 17.0f;
	constexpr float GrenadeCloudScaleEnd = 30.0f;
	constexpr float GrenadeCloudSpin = 0.03f; // turns a second, either way
	constexpr int32 GrenadeCloudCells = 4;    // the picture is two cells by two
	// SSmokeEmitter.HeatOpacity: what thermal vision keeps of the cloud. EMF vision draws no particle at all.
	constexpr float GrenadeCloudHeatOpacity = 0.2f;
	// The cloud's picture (sfx.Emitter.smoke_grenade2), read from the content folder: original content, never in the project.
	const TCHAR* const GrenadeCloudPicture = TEXT("shared/fx/textures/sfx_smoke_grenade2.png");
	const TCHAR* const StickyGasPicture = TEXT("shared/fx/textures/sfx_smoke_extincteur.png");
	const TCHAR* const SolidMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxSolid.M_ScctFxSolid");
	/** The simulation's light colour bytes: hue around the wheel, and saturation where 255 is white. */
	FLinearColor LightColor(float Hue, float Saturation)
	{
		return FLinearColor(Hue / 255.0f * 360.0f, 1.0f - FMath::Clamp(Saturation / 255.0f, 0.0f, 1.0f), 1.0f).HSVToLinearRGB();
	}
}

bool UScctEffectsSubsystem::DoesSupportWorldType(const EWorldType::Type WorldType) const
{
	return WorldType == EWorldType::Game || WorldType == EWorldType::PIE;
}

void UScctEffectsSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	Super::Initialize(Collection);
	Session = Collection.InitializeDependency<UScctSessionSubsystem>();
	if (Session == nullptr)
	{
		return;
	}

	Session->OnSessionChanged.AddUObject(this, &UScctEffectsSubsystem::HandleSessionChanged);
	Session->OnMatchStarted.AddUObject(this, &UScctEffectsSubsystem::HandleMatchStarted);
	Session->OnBeforeTicks.AddUObject(this, &UScctEffectsSubsystem::HandleBeforeTicks);
	Session->OnFramePresented.AddUObject(this, &UScctEffectsSubsystem::HandleFrame);

	// Flashes are scaled by the picture's exposure (Play/ScctExposure.h).
	FScctExposure::Start();
	FScctSmokeVolumes::RegisterSettings();

	bLog = FParse::Param(FCommandLine::Get(), TEXT("ScctFxLog"));
	bAutoFire = FParse::Param(FCommandLine::Get(), TEXT("ScctAutoFire"));
	FParse::Value(FCommandLine::Get(), TEXT("ScctFxTimeScale="), TimeScale);
	TimeScale = FMath::Clamp(TimeScale, 0.01f, 4.0f);

	Random.Initialize(0x5CC7);
	ShotBuffer.SetNumUninitialized(128);
	ActorBuffer.SetNumUninitialized(320);
	MarkBuffer.SetNumUninitialized(MaxImpactSlots);
	Tracers.SetNum(MaxTracers);
	ImpactSerials.Init(0, MaxImpactSlots);
	ImpactSeenFrame.Init(0, MaxImpactSlots);
	MarkSurfaces.SetNum(MaxImpactSlots);
	Actors.Reserve(320);
}

void UScctEffectsSubsystem::Deinitialize()
{
	if (Session != nullptr)
	{
		Session->OnSessionChanged.RemoveAll(this);
		Session->OnMatchStarted.RemoveAll(this);
		Session->OnBeforeTicks.RemoveAll(this);
		Session->OnFramePresented.RemoveAll(this);
	}
	Super::Deinitialize();
}

// ---------------------------------------------------------------------------------------------------------------
// Session
// ---------------------------------------------------------------------------------------------------------------

void UScctEffectsSubsystem::HandleSessionChanged(int32 OldId, int32 NewId)
{
	if (OldId != 0)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects summary: %d muzzle flashes, %d tracers, %d impacts, %d effect actors (%d drawn as stand-in shapes), at most %d sprites at once; rain on bodies: %d splashes, %d drips, %d puffs; run-time skins put on effect meshes: %d"),
			NumFlashes, NumTracers, NumImpacts, NumActors, NumStandIns, PeakSprites, NumFlicSplashes, NumFlicDrips, NumFlicPuffs, EffectSkins.NumApplied);
	}
	// Actor ids are reused by the next match: nothing drawn may carry over.
	ResetMatch();
}

void UScctEffectsSubsystem::ResetMatch()
{
	for (TPair<int32, FActorFx>& Pair : Actors)
	{
		EndActor(Pair.Value, false);
	}
	Actors.Reset();
	for (int32 Index = 0; Index < Lights.Num(); ++Index)
	{
		ReleaseLight(Index);
	}
	for (UDecalComponent* Decal : Decals)
	{
		if (Decal != nullptr)
		{
			Decal->SetVisibility(false);
		}
	}
	for (FTracer& Tracer : Tracers)
	{
		Tracer.bLive = false;
	}
	for (int32& Serial : ImpactSerials)
	{
		Serial = 0;
	}
	for (FMarkSurface& Surface : MarkSurfaces)
	{
		Surface = FMarkSurface();
	}
	ResetProjections();
	ResetDevices();
	ResetCasings();
	SmokeVolumes.Reset();
	Sprites.Clear();
	EffectSkins.ResetMatch();
	MatchSeconds = 0.0f;
	bFireHeld = bAltFireHeld = false;
	NumFlashes = NumFlashesLeftToViewModel = NumTracers = NumImpacts = NumActors = NumStandIns = PeakSprites = 0;
	NumFlicSplashes = NumFlicDrips = NumFlicPuffs = 0;
	NumExplosions = 0;
	FMemory::Memzero(NumActorsByKind);
	StartedByClass.Reset();
}

UScctEffectsSubsystem::FStats UScctEffectsSubsystem::GetStats() const
{
	FStats Stats;
	Stats.MuzzleFlashes = NumFlashes;
	Stats.FlashesLeftToViewModel = NumFlashesLeftToViewModel;
	Stats.Tracers = NumTracers;
	Stats.Impacts = NumImpacts;
	for (const UDecalComponent* Decal : Decals)
	{
		Stats.DecalsShown += Decal != nullptr && Decal->IsVisible() ? 1 : 0;
	}
	Stats.EffectActors = NumActors;
	FMemory::Memcpy(Stats.EffectActorsByKind, NumActorsByKind, sizeof(NumActorsByKind));
	Stats.Explosions = NumExplosions;
	Stats.LiveEffectActors = Actors.Num();
	return Stats;
}

void UScctEffectsSubsystem::HandleMatchStarted()
{
	FString Error;
	Events = ScctFeature<FScctEventsApi>(&Error);
	if (Events == nullptr)
	{
		UE_LOG(LogOpenSCCT, Error, TEXT("Effects: none this match, the bridge's events are missing: %s"), *Error);
		return;
	}
	Events->Begin();
	if (EnsureScene())
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects: ready (%s, time scale %.2f%s)"),
			Sprites.IsDrawable() ? TEXT("sprite materials found") : TEXT("sprite materials MISSING: run Tools/Effects/build_effect_assets.py"),
			TimeScale, bAutoFire ? TEXT(", auto fire on") : TEXT(""));
	}
}

bool UScctEffectsSubsystem::EnsureScene()
{
	if (bSceneReady)
	{
		return true;
	}

	FActorSpawnParameters Parameters;
	Parameters.ObjectFlags |= RF_Transient;
	Host = GetWorld()->SpawnActor<AActor>(AActor::StaticClass(), FTransform::Identity, Parameters);
	if (Host == nullptr)
	{
		return false;
	}
	USceneComponent* Root = NewObject<USceneComponent>(Host, TEXT("Root"));
	Root->SetMobility(EComponentMobility::Movable);
	Host->SetRootComponent(Root);
	Root->RegisterComponent();

	UStaticMesh* Quad = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Plane.Plane"));
	FlareCard = Quad;
	FlareMaterial = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/OpenSCCT/Effects/M_ScctFxFlare.M_ScctFxFlare"));
	// The smoke grenade's cloud is drawn with the game's own picture of it; without the picture or its material the
	// cloud is the soft grey discs of the smoke layer, in the same places and sizes.
	UMaterialInterface* SmokeMaterial = LoadObject<UMaterialInterface>(nullptr, SmokeMaterialPath);
	UMaterialInterface* CloudBase = LoadObject<UMaterialInterface>(nullptr, CloudMaterialPath, nullptr, LOAD_NoWarn | LOAD_Quiet);
	auto CloudAtlas = [this, CloudBase](const TCHAR* RelativePath, int32 Grid) -> UMaterialInstanceDynamic*
	{
		const FString Path = FOpenSCCTModule::FindContentRoot() / RelativePath;
		UTexture2D* Texture = CloudBase && FPaths::FileExists(Path) ? FImageUtils::ImportFileAsTexture2D(Path) : nullptr;
		if (!Texture)
		{
			UE_LOG(LogOpenSCCT, Warning, TEXT("Effects: using soft smoke sprites for %s (%s unavailable)"), RelativePath, CloudBase ? *Path : CloudMaterialPath);
			return nullptr;
		}
		Texture->AddressX = Texture->AddressY = TA_Clamp;
		Texture->UpdateResource();
		UMaterialInstanceDynamic* Cloud = UMaterialInstanceDynamic::Create(CloudBase, Host);
		Cloud->SetTextureParameterValue(TEXT("Texture"), Texture);
		Cloud->SetVectorParameterValue(TEXT("Grid"), FLinearColor(Grid, Grid, 0, 0));
		return Cloud;
	};
	UMaterialInterface* CloudMaterial = CloudAtlas(GrenadeCloudPicture, 2);
	UMaterialInterface* GasMaterial = CloudAtlas(StickyGasPicture, 4);
	bCloudPicture = CloudMaterial != nullptr;
	Sprites.Init(Host, Quad, LoadObject<UMaterialInterface>(nullptr, GlowMaterialPath), SmokeMaterial, CloudMaterial, GasMaterial);

	DecalMaterial = LoadObject<UMaterialInterface>(nullptr, DecalMaterialPath);
	StandInMesh = LoadObject<UStaticMesh>(nullptr, TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	if (UMaterialInterface* Solid = LoadObject<UMaterialInterface>(nullptr, SolidMaterialPath))
	{
		StandInMaterial = UMaterialInstanceDynamic::Create(Solid, this);
		StandInMaterial->SetVectorParameterValue(TEXT("Color"), FLinearColor(0.02f, 0.02f, 0.02f));
		StandInMaterial->SetVectorParameterValue(TEXT("Glow"), FLinearColor(3.0f, 0.25f, 0.1f));
	}

	Lights.SetNum(MaxLights);
	for (int32 Index = 0; Index < MaxLights; ++Index)
	{
		UPointLightComponent* Light = NewObject<UPointLightComponent>(Host);
		Light->SetMobility(EComponentMobility::Movable);
		Light->SetIntensityUnits(ELightUnits::Candelas);
		Light->SetVisibility(false);
		Light->SetupAttachment(Root);
		UScctEchelonLightFilter::RouteNewWorldLight(*Light);
		Light->RegisterComponent();
		LightComponents.Add(Light);
	}

	BodyFill = MakeBodyLight(BodyFillRadius, 0.35f);
	// The edge is mostly sheen: light that grazes the cloth towards the camera.
	BodyRim = MakeBodyLight(BodyRimRadius, 1.0f);

	SmokeVolumes.Init(Host);
	MeshUsed.Init(false, MaxMeshes);
	MeshPool.SetNum(MaxMeshes);

	Decals.SetNum(MaxImpactSlots);
	DecalTints.SetNum(MaxImpactSlots);

	FlareCartridge = ScctRuntimeSkins::LoadEffectMesh(TEXT("SGameplayObjects.flare_grenade"));
	bSceneReady = true;
	return true;
}

void UScctEffectsSubsystem::HandleBeforeTicks(float DeltaSeconds)
{
	if (!bAutoFire || !Session->IsPlaying())
	{
		return;
	}

	// -ScctAutoFire: the fire keys go down and come up on the player controller as under a finger, so the shots
	// take the path real ones take (Unreal's key state, the bridge's bindings, the simulation).
	AScctPlayerController* Player = Cast<AScctPlayerController>(GetWorld()->GetFirstPlayerController());
	if (Player == nullptr)
	{
		return;
	}
	const bool bFire = MatchSeconds > 3.0f && FMath::Fmod(MatchSeconds, 1.0f) < 0.5f;
	const bool bAltFire = MatchSeconds > 3.0f && FMath::Fmod(MatchSeconds, 6.0f) > 5.6f;
	if (bFire != bFireHeld)
	{
		Player->HoldScriptedAction(TEXT("fire"), bFire);
	}
	if (bAltFire != bAltFireHeld)
	{
		Player->HoldScriptedAction(TEXT("altfire"), bAltFire);
	}
	bFireHeld = bFire;
	bAltFireHeld = bAltFire;
}

void UScctEffectsSubsystem::HandleFrame(float DeltaSeconds)
{
	MatchSeconds += DeltaSeconds;
	// -ScctKnockCheck (Runtime/ScctCameraBoneApi.h); nothing without the switch.
	ScctRunCameraBoneCheck(MatchSeconds);
	if (Events == nullptr || !bSceneReady || !IsValid(Host))
	{
		return;
	}

	++Frame;
	if (const APlayerController* Viewer = GetWorld()->GetFirstPlayerController())
	{
		// -ScctMineLight (Play/ScctEffectSkins.h); nothing without the switch.
		FVector Eye;
		FRotator Look;
		Viewer->GetPlayerViewPoint(Eye, Look);
		EffectSkins.RunCheck(MatchSeconds, Eye, Look.Vector());
	}
	const float FxSeconds = DeltaSeconds * TimeScale * UScctDemoPlayback::PresentationRate(GetWorld());
	DrainShots();
	UpdateTracers(FxSeconds);
	UpdateCasings(FxSeconds);
	UpdateImpactMarks();
	UpdateProjections();
	SmokeVolumes.Update(DeltaSeconds);
	UpdateEffectActors(FxSeconds);
	UpdateDevices(FxSeconds);
	UpdateLights(FxSeconds);
	UpdateBodyFill(DeltaSeconds);
	UpdateBodyFade();

	// The smoke grenade's cloud in the goggles, as the original's particles: thermal vision keeps a fifth of it
	// (SSmokeEmitter.HeatOpacity), EMF vision draws none of it; night vision and motion tracking see it as the eye does.
	const UScctVisionSubsystem* Goggles = UScctVisionSubsystem::Find(GetWorld());
	const int32 GogglesMode = Goggles != nullptr ? Goggles->GetVisionMode() : 0;
	Sprites.SetLayerOpacity(EScctFxLayer::Cloud, GogglesMode == 38 ? 0.0f : GogglesMode == 36 ? GrenadeCloudHeatOpacity : 1.0f);
	Sprites.SetLayerOpacity(EScctFxLayer::StickyGas, GogglesMode == 38 ? 0.0f : 1.0f);

	const FScctBridgeView& View = Session->GetView();
	Sprites.Tick(FxSeconds, FVector(View.X, View.Y, View.Z), FRotator(View.Pitch, View.Yaw, View.Roll).Quaternion());
	PeakSprites = FMath::Max(PeakSprites, Sprites.GetNumLive());
}

UPointLightComponent* UScctEffectsSubsystem::MakeBodyLight(float Radius, float Specular)
{
	UPointLightComponent* Light = NewObject<UPointLightComponent>(Host);
	Light->SetMobility(EComponentMobility::Movable);
	Light->bUseInverseSquaredFalloff = false; // the same units as the map's lights (Tools/Import/import_map.py)
	Light->SetLightFalloffExponent(1.0f);
	Light->SetAttenuationRadius(Radius);
	Light->SetCastShadows(false);
	Light->SetSpecularScale(Specular);
	Light->SetLightingChannels(false, true, false);
	// Lighting channels do not hold for the air: without this a light near the camera lights the haze right in
	// front of the lens and a foggy yard turns into a flat grey wash.
	Light->SetVolumetricScatteringIntensity(0.0f);
	// Nor for Lumen's bounce: the light is for the bodies alone.
	Light->SetIndirectLightingIntensity(0.0f);
	Light->SetVisibility(false);
	Light->SetupAttachment(Host->GetRootComponent());
	Light->RegisterComponent();
	return Light;
}

void UScctEffectsSubsystem::UpdateBodyFill(float DeltaSeconds)
{
	if (BodyFill == nullptr || BodyRim == nullptr)
	{
		return;
	}
	const AScctPawnProxy* Local = Session->FindLocalProxy();

	// Each pawn and its equipment share the same ordinary lamps. Channel 0 carries live world lights and flares.
	// Channel 2 is reserved for the optional own-side fill and rim.
	const bool bPlan = FScctLightPlan::IsOn();
	if (bPlan)
	{
		const bool bAids = FScctLightRules::BodyAids();
		if (!BodyFill->LightingChannels.bChannel2)
		{
			BodyFill->SetLightingChannels(false, false, true);
			BodyRim->SetLightingChannels(false, false, true);
		}
		const UScctEquipmentSubsystem* Equipment = GetWorld()->GetSubsystem<UScctEquipmentSubsystem>();
		const AScctFirstPersonView* FirstPerson = Equipment ? Equipment->GetFirstPersonView() : nullptr;
		for (const FScctBridgePawn& Pawn : Session->GetPawns())
		{
			const AScctPawnProxy* Proxy = Session->FindProxy(Pawn.ActorId);
			if (Proxy == nullptr)
			{
				continue;
			}
			const bool bOwnSide = bAids && Local != nullptr && Proxy->GetTeam() == Local->GetTeam();
			const FVector Centre(Pawn.X, Pawn.Y, Pawn.Z);
			const bool bLocalGear = Proxy == Local && FirstPerson && FirstPerson->IsShown() && Proxy->GetKind() != 0;
			FScctPawnLighting Lighting = FScctPawnLighting::Sample(*GetWorld(), Centre, Pawn.CollisionRadius + 0.5f * Pawn.CollisionHeight,
				Proxy->GetKind() == 0 ? Pawn.ActorId : INDEX_NONE, bLocalGear);
			TArray<USceneComponent*> Parts;
			Proxy->GetRootComponent()->GetChildrenComponents(true, Parts);
			const int32 WorldPartCount = Parts.Num();
			if (Proxy == Local && FirstPerson && FirstPerson->IsShown())
			{
				TArray<USceneComponent*> ViewParts;
				FirstPerson->GetRootComponent()->GetChildrenComponents(true, ViewParts);
				Parts.Append(ViewParts);
			}
			for (int32 Index = 0; Index < Parts.Num(); ++Index)
			{
				if (bLocalGear && Index == WorldPartCount)
				{
					Lighting.UseViewAmbient();
				}
				if (UMeshComponent* Mesh = Cast<UMeshComponent>(Parts[Index]))
				{
					const bool bLegs = Mesh == Proxy->GetFirstPersonLegs();
					if (bLegs && !Mesh->IsVisible())
					{
						continue;
					}
					const bool bBody = Mesh == Proxy->GetBodyMesh() || bLegs;
					Lighting.Apply(*Mesh, bOwnSide && bBody, bBody);
				}
			}
		}
		if (!bAids)
		{
			BodyFill->SetVisibility(false);
			BodyRim->SetVisibility(false);
			BodyLightScale = 0.0f;
			return;
		}
	}
	if (Local == nullptr)
	{
		BodyFill->SetVisibility(false);
		BodyRim->SetVisibility(false);
		BodyLightScale = 0.0f;
		return;
	}

	// The lights' channel on the bodies of the local player's side, off every other body (sides can change
	// between rounds while the proxies stay).
	for (const FScctBridgePawn& Pawn : Session->GetPawns())
	{
		const AScctPawnProxy* Proxy = Session->FindProxy(Pawn.ActorId);
		UPrimitiveComponent* Body = Proxy != nullptr ? Proxy->GetBodyMesh() : nullptr;
		if (Body == nullptr || bPlan)
		{
			continue;
		}
		const bool bOwnSide = Proxy->GetTeam() == Local->GetTeam();
		if (static_cast<bool>(Body->LightingChannels.bChannel1) != bOwnSide)
		{
			Body->SetLightingChannels(true, bOwnSide, false);
		}
		if (UPrimitiveComponent* Legs = Proxy->GetFirstPersonLegs(); Legs && Legs->IsVisible() && static_cast<bool>(Legs->LightingChannels.bChannel1) != bOwnSide)
		{
			Legs->SetLightingChannels(true, bOwnSide, false);
		}
	}

	// Over the whole exposure range, not a flash's narrower one: the lights fall on nothing but these bodies, and
	// with the flash's upper bound a spy in a brightly lit room was a flat black cut-out again. Eased, because the
	// exposure read is the last frame's and moves fast in a match's first second: followed frame by frame the
	// figure flashed pale while the eye was still opening.
	const float Wanted = FScctExposure::PictureScale(0.03f, 16.0f);
	BodyLightScale = BodyLightScale > 0.0f
		? FMath::Exp(FMath::FInterpTo(FMath::Loge(BodyLightScale), FMath::Loge(Wanted), DeltaSeconds, BodyLightFollow))
		: Wanted;

	const FScctBridgeView& View = Session->GetView();
	const FVector Eye(View.X, View.Y, View.Z);
	const FRotationMatrix Axes(FRotator(0.0, View.Yaw, 0.0));
	const FVector Forward = Axes.GetUnitAxis(EAxis::X);
	const FVector Right = Axes.GetUnitAxis(EAxis::Y);

	BodyFill->SetWorldLocation(Eye + Right * BodyFillAside + FVector(0.0, 0.0, BodyFillRaise));
	BodyFill->SetIntensity(FMath::Max(CVarBodyFill.GetValueOnGameThread(), 0.0f) * BodyLightScale);
	BodyFill->SetVisibility(true);

	// Behind the body as the camera sees it, on the side the fill does not come from. From his own eyes the
	// player sees no body of his own: the edge light then stands ahead of the camera, behind whoever he looks at.
	const FScctBridgePawn& Pawn = Local->GetPawn();
	const FVector Centre(Pawn.X, Pawn.Y, Pawn.Z);
	FVector Away = Local->IsFirstPerson() ? Forward : (Centre - Eye).GetSafeNormal2D();
	if (Away.IsNearlyZero())
	{
		Away = Forward;
	}
	const FVector From = Local->IsFirstPerson() ? Eye + Away * 250.0f : Centre;
	BodyRim->SetWorldLocation(From + Away * BodyRimBehind - Right * BodyRimAside + FVector(0.0, 0.0, BodyRimRaise));
	BodyRim->SetIntensity(FMath::Max(CVarBodyRim.GetValueOnGameThread(), 0.0f) * BodyLightScale);
	BodyRim->SetVisibility(true);
}

void UScctEffectsSubsystem::UpdateBodyFade()
{
	// The proxies stay while the local player changes (a new round, another side): only his body has the fade.
	// A tour camera is not the player's and shows him whole.
	const AScctPawnProxy* Local = Session->FindLocalProxy();
	const float Distance = Session->IsTourView() ? 0.0f : FMath::Max(CVarBodyFade.GetValueOnGameThread(), 0.0f);
	const FScctBridgeView& View = Session->GetView();
	const FVector Eye(View.X, View.Y, View.Z);
	for (const TPair<int32, TObjectPtr<AScctPawnProxy>>& Pair : Session->GetProxies())
	{
		if (AScctPawnProxy* Proxy = Pair.Value)
		{
			Proxy->SetNearFade(Proxy == Local ? Distance : 0.0f, Eye);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Shots
// ---------------------------------------------------------------------------------------------------------------

FVector UScctEffectsSubsystem::FindMuzzle(const FScctShot& Shot) const
{
	const FVector Estimate(Shot.X, Shot.Y, Shot.Z);
	if (Shot.Kind == EScctShotKind::FirstPersonMuzzle)
	{
		// The local player's own gun: its view model is not drawn by this subsystem; the simulation's estimate
		// (in front of the camera, low and to the right) is where that gun's muzzle is.
		return Estimate;
	}

	FTransform Barrel;
	const UScctEquipmentSubsystem* Equipment = GetWorld()->GetSubsystem<UScctEquipmentSubsystem>();
	const AScctPawnProxy* Proxy = Session->FindProxy(Shot.ShooterId);
	const bool bEyeTracer = Shot.Kind == EScctShotKind::Tracer && Equipment &&
		Equipment->GetFirstPerson().ActorId == Shot.ShooterId && Equipment->DrawsLocalMuzzleFlash();
	if (!bEyeTracer && (!Proxy || !Proxy->HasModel() || !Proxy->IsShown() || Proxy->IsFirstPerson()))
	{
		return Estimate;
	}
	if (Equipment != nullptr && Equipment->FindMuzzle(Shot.ShooterId, Barrel))
	{
		return Barrel.GetLocation();
	}
	if (Shot.Kind == EScctShotKind::Tracer)
	{
		return Estimate;
	}
	const FName Hand = Proxy->FindMeshBoneName(TEXT("B R Hand"));
	if (Hand.IsNone())
	{
		return Estimate;
	}
	// The gun's barrel ends about this far in front of the hand that holds it.
	return Proxy->GetBodyMesh()->GetSocketLocation(Hand) + FVector(Shot.DirX, Shot.DirY, Shot.DirZ) * 24.0;
}

void UScctEffectsSubsystem::DrainShots()
{
	for (;;)
	{
		const int32 Count = Events->DrainShots(ShotBuffer.GetData(), ShotBuffer.Num());
		for (int32 Index = 0; Index < Count; ++Index)
		{
			const FScctShot& Shot = ShotBuffer[Index];
			const FVector Direction = FVector(Shot.DirX, Shot.DirY, Shot.DirZ).GetSafeNormal(UE_SMALL_NUMBER, FVector::ForwardVector);
			if (Shot.Kind == EScctShotKind::Tracer)
			{
				++NumTracers;
				const FVector Start = FindMuzzle(Shot);
				const FVector Impact(Shot.ImpactX, Shot.ImpactY, Shot.ImpactZ);
				// The original hides a tracer for the last stretch before its impact; a short shot shows none.
				const float Distance = static_cast<float>(FVector::Dist(Start, Impact)) - Shot.HideDistance;
				if (bLog)
				{
					UE_LOG(LogOpenSCCT, Log, TEXT("Effects: tracer of pawn %d from (%.0f, %.0f, %.0f) to (%.0f, %.0f, %.0f), visible for %.0f cm at %.0f cm/s"),
						Shot.ShooterId, Start.X, Start.Y, Start.Z, Impact.X, Impact.Y, Impact.Z, FMath::Max(Distance, 0.0f), Shot.Speed);
				}
				if (Distance <= 0.0f)
				{
					continue;
				}
				for (FTracer& Tracer : Tracers)
				{
					if (!Tracer.bLive)
					{
						Tracer.Start = Start;
						Tracer.Direction = (Impact - Start).GetSafeNormal(UE_SMALL_NUMBER, Direction);
						Tracer.Distance = Distance;
						Tracer.Travelled = 0.0f;
						Tracer.Speed = Shot.Speed > 0.0f ? Shot.Speed : 7000.0f;
						Tracer.bLive = true;
						break;
					}
				}
				continue;
			}

			const bool bSticky = Shot.Kind == EScctShotKind::MuzzleSticky;
			const bool bFirstPerson = Shot.Kind == EScctShotKind::FirstPersonMuzzle;
			if ((Shot.Flags & EScctShotFlags::Casing) != 0)
			{
				SpawnCasing(Shot, Direction);
			}
			if (bFirstPerson)
			{
				// One owner for the local player's own flash: while the equipment draws a view model the flash
				// sits on that model's muzzle and is drawn there; a second one here would double it.
				const UScctEquipmentSubsystem* Equipment = GetWorld()->GetSubsystem<UScctEquipmentSubsystem>();
				if (Equipment != nullptr && Equipment->DrawsLocalMuzzleFlash())
				{
					++NumFlashesLeftToViewModel;
					if (bLog)
					{
						UE_LOG(LogOpenSCCT, Log, TEXT("Effects: the local first-person flash of pawn %d is the view model's"), Shot.ShooterId);
					}
					continue;
				}
			}
			++NumFlashes;
			const FVector Muzzle = FindMuzzle(Shot);
			const float Scale = (Shot.Flags & EScctShotFlags::Pistol) != 0 ? 0.6f : 1.0f;
			if (bLog)
			{
				UE_LOG(LogOpenSCCT, Log, TEXT("Effects: muzzle flash kind %d of pawn %d at (%.0f, %.0f, %.0f) (the simulation's estimate (%.0f, %.0f, %.0f)), flags %d"),
					Shot.Kind, Shot.ShooterId, Muzzle.X, Muzzle.Y, Muzzle.Z, Shot.X, Shot.Y, Shot.Z, Shot.Flags);
			}
			// Seen down the sights a flash fills far more of the picture than from outside: it is kept small.
			SpawnMuzzleFlash(Muzzle, Direction, bFirstPerson ? Scale * 0.35f : Scale, bSticky,
				(Shot.Flags & EScctShotFlags::Suppressed) != 0, (Shot.Flags & EScctShotFlags::Shotgun) != 0);
		}
		if (Count < ShotBuffer.Num())
		{
			return;
		}
	}
}

void UScctEffectsSubsystem::UpdateTracers(float DeltaSeconds)
{
	for (FTracer& Tracer : Tracers)
	{
		if (!Tracer.bLive)
		{
			continue;
		}
		Tracer.Travelled += Tracer.Speed * DeltaSeconds;
		if (Tracer.Travelled - TracerLength >= Tracer.Distance)
		{
			Tracer.bLive = false;
			continue;
		}
		const float Head = FMath::Min(Tracer.Travelled, Tracer.Distance);
		const float Tail = FMath::Max(Tracer.Travelled - TracerLength, 0.0f);
		Sprites.DrawStreak(Tracer.Start + Tracer.Direction * Tail, Tracer.Start + Tracer.Direction * Head, TracerWidth, FLinearColor(28.0f, 17.0f, 6.0f, 1.0f));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Impact marks
// ---------------------------------------------------------------------------------------------------------------

void UScctEffectsSubsystem::UpdateImpactMarks()
{
	const UScctVisionSubsystem* Vision = UScctVisionSubsystem::Find(GetWorld());
	const bool bVisibleInVision = !Vision || !Vision->IsHeatOrEmf();
	const int32 Count = Events->GetImpactMarks(MarkBuffer.GetData(), MarkBuffer.Num());
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FScctImpactMark& Mark = MarkBuffer[Index];
		if (Mark.Slot < 0 || Mark.Slot >= MaxImpactSlots)
		{
			continue;
		}
		ImpactSeenFrame[Mark.Slot] = Frame;

		const FVector At(Mark.X, Mark.Y, Mark.Z);
		const FVector Normal = FRotator(Mark.Pitch, Mark.Yaw, Mark.Roll).Vector();
		const bool bNew = ImpactSerials[Mark.Slot] != Mark.Serial;
		const bool bShown = (Mark.Flags & EScctImpactFlags::DecalShown) != 0 && (Mark.Flags & EScctImpactFlags::Shocker) == 0;
		if (bNew)
		{
			ImpactSerials[Mark.Slot] = Mark.Serial;
			MarkSurfaces[Mark.Slot] = FMarkSurface();
			++NumImpacts;
			const FString& SparkClass = GetName(Mark.SparkClassId);
			const FString& TypeClass = GetName(Mark.TypeId);
			if (bLog)
			{
				UE_LOG(LogOpenSCCT, Log, TEXT("Effects: impact slot %d '%s' spark '%s' at (%.0f, %.0f, %.0f) facing (%.2f, %.2f, %.2f), decal %s"),
					Mark.Slot, *TypeClass, *SparkClass, At.X, At.Y, At.Z, Normal.X, Normal.Y, Normal.Z, bShown ? TEXT("shown") : TEXT("none"));
			}
			SpawnImpact(At, Normal, SparkClass, TypeClass);
		}

		TObjectPtr<UDecalComponent>& Decal = Decals[Mark.Slot];
		if (bShown && DecalMaterial != nullptr)
		{
			if (Decal == nullptr)
			{
				Decal = NewObject<UDecalComponent>(Host);
				Decal->SetDecalMaterial(DecalMaterial);
				Decal->SetupAttachment(Host->GetRootComponent());
				Decal->RegisterComponent();
				Decal->SetFadeScreenSize(0.0005f);
				// Each slot's decal has its own colours, set for the surface that was hit.
				DecalTints[Mark.Slot] = Decal->CreateDynamicMaterialInstance();
			}
			// A mark tied to a door that was hidden for a while comes back where it was on the door.
			if (!MarkSurfaces[Mark.Slot].bPlaced)
			{
				const float HalfSize = TintDecal(DecalTints[Mark.Slot], GetName(Mark.TypeId), GetName(Mark.SparkClassId));
				// A decal projects along its own X axis, into the surface; its turn about that axis is random so
				// a burst does not stamp one shape, and so is its size a little.
				const float Size = HalfSize * (Mark.DrawScale > 0.0f ? FMath::Clamp(Mark.DrawScale, 0.6f, 2.0f) : 1.0f) * Random.FRandRange(0.85f, 1.2f);
				FRotator Facing = (-Normal).Rotation();
				Facing.Roll = Random.FRandRange(0.0f, 360.0f);
				// Shallow, so a mark on a thin wall or a door edge is not projected onto what stands behind it.
				Decal->DecalSize = FVector(4.0, Size, Size);

				// A mark on a door, a lift or any other map actor that moves goes with it, and is drawn only while
				// that actor is (Play/ScctEffectsMarks.cpp). A mark on the level's own geometry stays put.
				Decal->SetWorldLocationAndRotation(At, Facing);
				AttachMark(Mark.Slot, Mark, *Decal);
				MarkSurfaces[Mark.Slot].bPlaced = true;
			}
			const bool bDrawn = FollowMarkSurface(Mark.Slot, *Decal) && bVisibleInVision;
			if (Decal->IsVisible() != bDrawn)
			{
				Decal->SetVisibility(bDrawn);
			}
		}
		else if (Decal != nullptr && Decal->IsVisible())
		{
			Decal->SetVisibility(false);
			FMarkSurface& Surface = MarkSurfaces[Mark.Slot];
			if (bLog && Surface.bAttached && !Surface.bHiddenWithSurface)
			{
				UE_LOG(LogOpenSCCT, Log, TEXT("Effects: mark slot %d hidden with its surface #%d (no longer drawn)"), Mark.Slot, Surface.ActorId);
			}
			Surface.bHiddenWithSurface = Surface.bAttached;
		}
	}

	// A slot that is no longer listed has expired.
	for (int32 Slot = 0; Slot < MaxImpactSlots; ++Slot)
	{
		if (ImpactSeenFrame[Slot] != Frame && Decals[Slot] != nullptr && Decals[Slot]->IsVisible())
		{
			Decals[Slot]->SetVisibility(false);
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Effect actors
// ---------------------------------------------------------------------------------------------------------------

void UScctEffectsSubsystem::UpdateEffectActors(float DeltaSeconds)
{
	const int32 Count = Events->GetEffectActors(ActorBuffer.GetData(), ActorBuffer.Num());
	const UScctVisionSubsystem* Vision = UScctVisionSubsystem::Find(GetWorld());
	const bool bShowVisionOnly = Vision != nullptr && Vision->ShowsVisionOnlyEffects();
	int32 NumVisionOnly = 0;
	ON_SCOPE_EXIT
	{
		const int32 Shown = bShowVisionOnly ? NumVisionOnly : 0;
		if (NumVisionOnly != LoggedVisionOnly || Shown != LoggedVisionOnlyShown)
		{
			LoggedVisionOnly = NumVisionOnly;
			LoggedVisionOnlyShown = Shown;
			UE_LOG(LogOpenSCCT, Log, TEXT("Effects: %d vision-only effects listed, %d shown (mode %d)"), NumVisionOnly, Shown, Vision != nullptr ? Vision->GetVisionMode() : 0);
		}
	};
	for (int32 Index = 0; Index < Count; ++Index)
	{
		const FScctEffectActor& Record = ActorBuffer[Index];
		FActorFx* Fx = Actors.Find(Record.ActorId);
		if ((Record.Flags & EScctEffectFlags::Deleted) != 0)
		{
			if (Fx != nullptr)
			{
				EndActor(*Fx, (Record.Flags & EScctEffectFlags::Killed) != 0);
				Actors.Remove(Record.ActorId);
			}
			continue;
		}
		const bool bRestoring = Fx == nullptr && (Record.Flags & EScctEffectFlags::RestoreAge) != 0;
		const bool bHistorical = (Record.Flags & EScctEffectFlags::HistoricalEmitter) != 0;
		if (Fx == nullptr)
		{
			Fx = &Actors.Add(Record.ActorId);
			TGuardValue<float> SpawnAge(Sprites.SpawnAge, bRestoring ? Record.Age : 0.0f);
			StartActor(*Fx, Record);
			if (bRestoring) Fx->Age = Record.Age;
		}
		Fx->SeenFrame = Frame;
		Fx->Age += DeltaSeconds;

		// Where it is: on a drawn pawn's bone, or where the simulation has it.
		const FVector Scale(Record.ScaleX != 0.0f ? Record.ScaleX : 1.0f, Record.ScaleY != 0.0f ? Record.ScaleY : 1.0f, Record.ScaleZ != 0.0f ? Record.ScaleZ : 1.0f);
		FTransform Place(FRotator(Record.Pitch, Record.Yaw, Record.Roll), FVector(Record.X, Record.Y, Record.Z));
		// bHideInNormalVision: shown only while the goggles show night or thermal vision.
		const bool bVisionOnly = (Record.Flags & EScctEffectFlags::VisionOnly) != 0;
		NumVisionOnly += bVisionOnly ? 1 : 0;
		bool bHidden = (Record.Flags & EScctEffectFlags::Hidden) != 0 || (bVisionOnly && !bShowVisionOnly);
		if ((Record.Flags & EScctEffectFlags::OnBone) != 0)
		{
			const AScctPawnProxy* Proxy = Session->FindProxy(Record.BaseActorId);
			const FName Bone = Proxy != nullptr && Proxy->HasModel() ? FindDrawnBone(*Proxy, Record.BoneId) : NAME_None;
			if (Bone.IsNone())
			{
				// Its wearer is not drawn (yet): nothing may show at the actor's own place in mid-air.
				bHidden = true;
			}
			else
			{
				const FTransform BoneWorld = Proxy->GetBodyMesh()->GetSocketTransform(Bone);
				const FTransform Relative(FRotator(Record.RelPitch, Record.RelYaw, Record.RelRoll), FVector(Record.RelX, Record.RelY, Record.RelZ));
				Place = Relative * FTransform(BoneWorld.GetRotation(), BoneWorld.GetLocation());
				// What a player wears is not in front of their own eyes.
				bHidden |= !Proxy->IsShown() || Proxy->IsFirstPerson();
			}
		}
		const FVector Before = Fx->Location;
		Fx->Location = Place.GetLocation();

		if (Fx->Mesh != INDEX_NONE)
		{
			UStaticMeshComponent* Component = MeshPool[Fx->Mesh];
			// A beam whose simulated start hangs in mid-air belongs to whoever draws the hand or device it comes from.
			const bool bMeshHidden = bHidden || (Record.Kind == EScctEffectKind::Beam && (Record.Flags & EScctEffectFlags::OwnView) != 0);
			if (Component->IsVisible() == bMeshHidden)
			{
				Component->SetVisibility(!bMeshHidden);
			}
			if (!bMeshHidden)
			{
				// The original draws a mesh moved by -PrePivot in its own, scaled frame. The stand-in shape is not
				// the game's mesh, so it keeps its own small size.
				const FVector MeshScale = Fx->bStandIn ? FVector(0.09) : Scale;
				const FVector Pivot = Fx->bStandIn ? FVector::ZeroVector : Scale * -FVector(Record.PrePivotX, Record.PrePivotY, Record.PrePivotZ);
				Component->SetWorldTransform(FTransform(Place.GetRotation(), Place.TransformPosition(Pivot), MeshScale));
				if (!Fx->bStandIn)
				{
					EffectSkins.Apply(Record.ActorId, Component, Fx->Skins, Events, bLog);
				}
			}
		}

		// A smoke grenade's cloud is the responsive smoke volume when the match runs it, not sprites.
		const bool bVolumeSmoke = (Fx->Style == EStyle::Smoke || Fx->Style == EStyle::Gas)
			&& SmokeVolumes.CoversEmitter(Record.ActorId, GetName(Fx->ClassId).Contains(TEXT("SmokeEmitter")), Fx->Age);
		// Rain on a pawn spreads over the drawn body (Play/ScctEffectsFlicFloc.cpp); without its table, a plain splash.
		const bool bFlicDrawn = Fx->bFlicFloc && UpdateFlicFloc(*Fx, Record, Fx->Location, bHidden, DeltaSeconds);
		FVector LightAt = Fx->Location;
		if (Fx->Style == EStyle::Flare)
		{
			LightAt = UpdateFlare(*Fx, Record, Place, Before, DeltaSeconds, bHidden, bRestoring, bHistorical);
		}
		else if (Fx->bGrenadeCloud)
		{
			// The spy's smoke grenade: the original's cloud, let out once (a cloud first seen late lets out what is left of it).
			if (!bHidden && !bVolumeSmoke)
			{
				EmitGrenadeCloud(*Fx);
			}
		}
		else if (Fx->Style == EStyle::StickyGas && !bHidden)
		{
			TGuardValue<float> SpawnAge(Sprites.SpawnAge, bRestoring ? Record.Age : 0.0f);
			EmitStickyGas(*Fx, Place.GetRotation());
		}
		else if (Fx->Style == EStyle::BarrelSmoke)
		{
			if (!bHidden)
			{
				EmitBarrelSmoke(*Fx, bHistorical ? Record.LifeSpan : Fx->Age);
			}
		}
		else if (!bHidden && !bVolumeSmoke && !bFlicDrawn && Fx->Style != EStyle::None && Record.Kind == EScctEffectKind::Emitter)
		{
			const FVector Velocity(Record.VelX, Record.VelY, Record.VelZ);
			if (bRestoring)
			{
				const float SinceEnd = bHistorical ? FMath::Max(0.0f, Record.Age - Record.LifeSpan) : 0.0f;
				// Twenty seconds is the reference renderer's particle warm-up window.
				for (float Age = FMath::Min(Record.Age, 20.0f); Age > SinceEnd; )
				{
					const float Step = FMath::Min(Age - SinceEnd, 1.0f / 60.0f);
					Age -= Step;
					TGuardValue<float> SpawnAge(Sprites.SpawnAge, Age);
					Emit(Fx->Style, Fx->Location, Velocity, Step, Fx->EmitDebt);
				}
			}
			if (!bHistorical) Emit(Fx->Style, Fx->Location, Velocity, DeltaSeconds, Fx->EmitDebt);
		}
		if (!bHidden && Fx->TrailStyle != EStyle::None && !Before.Equals(Fx->Location, 0.5))
		{
			Emit(Fx->TrailStyle, Fx->Location, FVector::ZeroVector, DeltaSeconds, Fx->EmitDebt);
		}

		// Its light: the brightness changes every frame (the grenade's flash fades), and a hidden light still lights.
		const bool bLit = (Record.Kind == EScctEffectKind::Light || (Record.Flags & EScctEffectFlags::DynamicLight) != 0) && Record.LightBrightness > 0.5f;
		if (bLit)
		{
			const FLinearColor Color = LightColor(Record.LightHue, Record.LightSaturation);
			const float Candelas = Record.LightBrightness * CandelasPerBrightness;
			const float Radius = FMath::Max(Record.LightRadius, 100.0f);
			if (Fx->Light == INDEX_NONE)
			{
				Fx->Light = AcquireLight(LightAt, Color, Candelas, Radius, 0.0f, true);
			}
			if (Fx->Light != INDEX_NONE)
			{
				UPointLightComponent* Light = LightComponents[Fx->Light];
				Light->SetWorldLocation(LightAt);
				if (Fx->Style == EStyle::Flare && FScctLightPlan::IsOn())
				{
					FScctLightRules::SetFlareLight(*Light, Record.LightBrightness, Color);
				}
				else
				{
					Light->SetIntensity(Candelas);
					Light->SetLightColor(Color);
				}
				Light->SetAttenuationRadius(Radius);
			}
		}
		else if (Fx->Light != INDEX_NONE)
		{
			ReleaseLight(Fx->Light);
			Fx->Light = INDEX_NONE;
		}
	}

	// An actor that is simply no longer listed has ended too.
	for (auto It = Actors.CreateIterator(); It; ++It)
	{
		if (It.Value().SeenFrame != Frame)
		{
			EndActor(It.Value(), false);
			It.RemoveCurrent();
		}
	}
}

void UScctEffectsSubsystem::StartActor(FActorFx& Fx, const FScctEffectActor& Record)
{
	++NumActors;
	if (Record.Kind >= 0 && Record.Kind < UE_ARRAY_COUNT(NumActorsByKind))
	{
		++NumActorsByKind[Record.Kind];
	}
	++StartedByClass.FindOrAdd(GetName(Record.ClassId));
	Fx.ClassId = Record.ClassId;
	Fx.Kind = Record.Kind;
	Fx.Location = FVector(Record.X, Record.Y, Record.Z);
	Fx.Style = Record.Kind == EScctEffectKind::Emitter ? FindStyle(Record.ClassId) : EStyle::None;
	Fx.TrailStyle = Record.TrailClassId != 0 ? FindStyle(Record.TrailClassId) : EStyle::None;
	Fx.bFlicFloc = Record.Kind == EScctEffectKind::Emitter && GetName(Record.ClassId).Contains(TEXT("FlicFloc"));
	Fx.bGrenadeCloud = Record.Kind == EScctEffectKind::Emitter && GetName(Record.ClassId).EndsWith(TEXT("SSmokeEmitter"));

	const bool bDrawsMesh = Record.Kind == EScctEffectKind::Projectile || Record.Kind == EScctEffectKind::Mine || Record.Kind == EScctEffectKind::Gadget
		|| Record.Kind == EScctEffectKind::Beam || Record.Kind == EScctEffectKind::Decal;
	if (bDrawsMesh)
	{
		UStaticMesh* Mesh = FindMesh(Record.MeshId);
		// A beam or a puddle without its mesh is better left out than drawn as a ball.
		Fx.bStandIn = Mesh == nullptr && Record.Kind != EScctEffectKind::Beam && Record.Kind != EScctEffectKind::Decal
			&& !GetName(Record.ClassId).EndsWith(TEXT("SProjFlare"));
		if (Mesh != nullptr || Fx.bStandIn)
		{
			for (int32 Index = 0; Index < MaxMeshes; ++Index)
			{
				if (MeshUsed[Index])
				{
					continue;
				}
				TObjectPtr<UStaticMeshComponent>& Component = MeshPool[Index];
				if (Component == nullptr)
				{
					Component = NewObject<UStaticMeshComponent>(Host);
					Component->SetMobility(EComponentMobility::Movable);
					Component->SetCollisionEnabled(ECollisionEnabled::NoCollision);
					Component->SetupAttachment(Host->GetRootComponent());
					Component->RegisterComponent();
				}
				Component->SetStaticMesh(Fx.bStandIn ? StandInMesh.Get() : Mesh);
				FScctEffectSkins::Clear(Component);
				if (Fx.bStandIn && StandInMaterial != nullptr)
				{
					Component->SetMaterial(0, StandInMaterial);
				}
				// Thin glowing beams and flat puddles throw no shadow. Nor does a flying flare: its own light burns
				// at the cartridge's tip and would project the shell as a huge wedge (as the settled cartridge avoids).
				Component->SetCastShadow(Record.Kind != EScctEffectKind::Beam && Record.Kind != EScctEffectKind::Decal
					&& !GetName(Record.ClassId).EndsWith(TEXT("SProjFlare")));
				Component->SetVisibility(false);
				MeshUsed[Index] = true;
				Fx.Mesh = Index;
				NumStandIns += Fx.bStandIn ? 1 : 0;
				break;
			}
		}
	}

	if (bLog)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects: actor %d '%s' kind %d flags %d at (%.0f, %.0f, %.0f): %s%s"), Record.ActorId, *GetName(Record.ClassId), Record.Kind,
			Record.Flags, Record.X, Record.Y, Record.Z,
			Fx.Mesh != INDEX_NONE ? (Fx.bStandIn ? TEXT("stand-in shape for mesh ") : TEXT("mesh ")) : TEXT("particles"),
			Fx.Mesh != INDEX_NONE ? *GetName(Record.MeshId) : TEXT(""));
	}

	// What happens once, as it appears.
	if ((Record.Flags & EScctEffectFlags::Hidden) != 0)
	{
		return;
	}
	switch (Fx.Style)
	{
	case EStyle::Explosion:
		++NumExplosions;
		SpawnExplosion(Fx.Location);
		break;
	case EStyle::FlashBang:
		SpawnFlashBang(Fx.Location);
		break;
	case EStyle::Chaff:
		for (int32 Index = 0; Index < 140; ++Index)
		{
			FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Glow);
			Particle.Position = Fx.Location;
			Particle.Velocity = Random.VRand() * Random.FRandRange(150.0f, 900.0f) + FVector(0.0, 0.0, 300.0);
			Particle.Color = FLinearColor(3.0f, 3.2f, 3.6f, Random.FRandRange(0.4f, 1.0f));
			Particle.Life = Random.FRandRange(3.0f, 9.0f);
			Particle.SizeStart = Particle.SizeEnd = Random.FRandRange(2.0f, 5.0f);
			Particle.Drag = 3.5f;
			Particle.Gravity = 35.0f;
		}
		break;
	case EStyle::Sparks:
		SpawnSparks(Fx.Location, FVector::UpVector, 16, FLinearColor(24.0f, 14.0f, 4.0f), 600.0f);
		break;
	case EStyle::Puff:
		SpawnDust(Fx.Location, FVector::UpVector, 5, FLinearColor(0.35f, 0.33f, 0.3f, 0.35f), 30.0f);
		break;
	case EStyle::Ps2Modem:
		SpawnPs2ModemEffect(Fx.Location, FRotator(Record.Pitch, Record.Yaw, Record.Roll));
		break;
	default:
		break;
	}
}

void UScctEffectsSubsystem::EndActor(FActorFx& Fx, bool bKilled)
{
	if (Fx.Flare)
	{
		if (UStaticMeshComponent* Flame = Fx.Flare->Flame.Get()) Flame->DestroyComponent();
		if (UStaticMeshComponent* Case = Fx.Flare->Cartridge.Get()) Case->DestroyComponent();
		Fx.Flare.Reset();
	}
	if (Fx.Mesh != INDEX_NONE)
	{
		MeshPool[Fx.Mesh]->SetVisibility(false);
		MeshUsed[Fx.Mesh] = false;
		Fx.Mesh = INDEX_NONE;
	}
	if (Fx.Light != INDEX_NONE)
	{
		ReleaseLight(Fx.Light);
		Fx.Light = INDEX_NONE;
	}
	// Its particles are in the shared pool and die out where they are, killed or not.
}

void UScctEffectsSubsystem::EmitGrenadeCloud(FActorFx& Fx)
{
	// As many as the emitter's rate has let out by now, to its fifteen; none is ever let out again.
	const int32 Due = FMath::Min(GrenadeCloudSprites, FMath::CeilToInt32(Fx.Age * GrenadeCloudPerSecond));
	if (Fx.EmittedSprites == 0 && Due > 0)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects: smoke grenade cloud at (%.0f, %.0f, %.0f): %d sprites of %s, %.0f s"),
			Fx.Location.X, Fx.Location.Y, Fx.Location.Z, GrenadeCloudSprites, bCloudPicture ? TEXT("the game's picture") : TEXT("plain grey discs"), GrenadeCloudLife);
	}
	for (; Fx.EmittedSprites < Due; ++Fx.EmittedSprites)
	{
		// Born a little into its life when the cloud was first seen late, so every sprite ends with the cloud.
		const float Born = FMath::Max(Fx.Age - Fx.EmittedSprites / GrenadeCloudPerSecond, 0.0f);
		if (Born >= GrenadeCloudLife)
		{
			continue;
		}
		FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Cloud);
		Particle.Position = Fx.Location;
		Particle.Velocity = FVector(Random.FRandRange(-GrenadeCloudSpeedSide, GrenadeCloudSpeedSide), Random.FRandRange(-GrenadeCloudSpeedSide, GrenadeCloudSpeedSide),
			Random.FRandRange(GrenadeCloudSpeedUpMin, GrenadeCloudSpeedUpMax));
		Particle.Acceleration = FVector(0.0, 0.0, GrenadeCloudRise);
		Particle.LossMin = GrenadeCloudLossMin;
		Particle.LossMax = GrenadeCloudLossMax;
		// The game's picture is the smoke's own grey; the plain disc is given one.
		Particle.Color = bCloudPicture ? FLinearColor::White : FLinearColor(0.5f, 0.5f, 0.5f, 0.85f);
		Particle.Life = GrenadeCloudLife;
		Particle.FadeIn = GrenadeCloudFadeInEnd / GrenadeCloudLife;
		Particle.FadeOutStart = GrenadeCloudFadeOutStart / GrenadeCloudLife;
		// The emitter's size is half a sprite's width.
		const float HalfSize = Random.FRandRange(GrenadeCloudHalfSizeMin, GrenadeCloudHalfSizeMax);
		Particle.SizeStart = 2.0f * HalfSize * GrenadeCloudScaleStart;
		Particle.SizeEnd = 2.0f * HalfSize * GrenadeCloudScaleEnd;
		Particle.Spin = Random.FRandRange(-GrenadeCloudSpin, GrenadeCloudSpin) * UE_TWO_PI;
		Particle.Cell = static_cast<float>(Random.RandHelper(GrenadeCloudCells));
		if (Born > 0.0f)
		{
			// What it did before it was seen: it stands where its throw has taken it, at the age it has.
			const FVector Loss = (GrenadeCloudLossMin + GrenadeCloudLossMax) * 0.5;
			const FVector Left(FMath::Exp(-Loss.X * Born), FMath::Exp(-Loss.Y * Born), FMath::Exp(-Loss.Z * Born));
			Particle.Position += Particle.Velocity / Loss * (FVector::OneVector - Left);
			Particle.Velocity *= Left;
			Particle.Age = Born;
		}
	}
}

void UScctEffectsSubsystem::EmitBarrelSmoke(FActorFx& Fx, float EmissionAge)
{
	// SSmokeCanonEmitter stays alive for four seconds, but its sub-emitter spends fifteen slots in one second.
	// The slots never respawn. The last particle fades within 0.8 seconds of its birth.
	constexpr int32 ParticleCount = 15;
	constexpr float ParticlesPerSecond = 15.0f;
	const int32 Due = FMath::Min(ParticleCount, FMath::FloorToInt32(EmissionAge * ParticlesPerSecond));
	for (; Fx.EmittedSprites < Due; ++Fx.EmittedSprites)
	{
		const float BirthTime = (Fx.EmittedSprites + 1) / ParticlesPerSecond;
		const float Age = FMath::Max(0.0f, Fx.Age - BirthTime);
		const float Life = Random.FRandRange(0.6f, 0.8f);
		if (Age >= Life)
		{
			continue;
		}
		FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Smoke);
		Particle.SeekAge = Age;
		Particle.Position = Fx.Location;
		Particle.Velocity = FVector(0.0, 0.0, Random.FRandRange(10.0f, 30.0f));
		Particle.Color = FLinearColor(0.6f, 0.6f, 0.62f, 0.22f);
		Particle.Life = Life;
		Particle.SizeStart = 3.0f;
		Particle.SizeEnd = Random.FRandRange(16.0f, 30.0f);
		Particle.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Particle.Spin = Random.FRandRange(-1.0f, 1.0f);
		Particle.FadeIn = 0.1f / Life;
		Particle.FadeOutStart = Particle.FadeIn;
	}
}

void UScctEffectsSubsystem::EmitStickyGas(FActorFx& Fx, const FQuat& Facing)
{
	// SStickyCamEmitter.StikyCamEmitter0 in shared/fx/effects.json. One dose emits fifteen
	// world-space sprites along the mounted camera's +X, at 500/s, with no respawns.
	const int32 Due = FMath::Min(15, FMath::FloorToInt32(Fx.Age * 500.0f));
	for (; Fx.EmittedSprites < Due; ++Fx.EmittedSprites)
	{
		FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::StickyGas);
		Particle.Position = Fx.Location;
		Particle.Velocity = Facing.RotateVector(FVector(Random.FRandRange(400.0f, 1200.0f),
			Random.FRandRange(-30.0f, 30.0f), Random.FRandRange(-20.0f, 20.0f)));
		Particle.Acceleration = FVector(2.0, 2.0, -2.0);
		Particle.LossMin = FVector(4.0);
		Particle.LossMax = FVector(5.0);
		Particle.Life = Random.FRandRange(5.0f, 7.0f);
		Particle.FadeIn = 0.1f / Particle.Life;
		Particle.FadeOutStart = 0.8f / Particle.Life;
		Particle.SizeStart = 2.0f * Random.FRandRange(2.0f, 5.0f);
		Particle.SizeEnd = Particle.SizeStart * 40.0f;
		Particle.Spin = Random.FRandRange(-0.1f, 0.1f) * UE_TWO_PI;
		Particle.Cell = static_cast<float>(Random.RandHelper(16));
		Particle.Color = FLinearColor::White;
	}
}

void UScctEffectsSubsystem::Emit(EStyle Style, const FVector& At, const FVector& Velocity, float DeltaSeconds, float& Debt)
{
	float Rate = 0.0f;
	switch (Style)
	{
	case EStyle::Smoke: Rate = 16.0f; break;
	case EStyle::Gas: Rate = 12.0f; break;
	case EStyle::Wisp: Rate = 26.0f; break;
	case EStyle::Sparks: Rate = 14.0f; break;
	case EStyle::Electric: Rate = 40.0f; break;
	case EStyle::Water: Rate = 30.0f; break;
	default: return;
	}

	Debt += Rate * DeltaSeconds;
	for (; Debt >= 1.0f; Debt -= 1.0f)
	{
		switch (Style)
		{
		case EStyle::Smoke:
		case EStyle::Gas:
		{
			// A cloud that fills a room: big soft puffs that swell, drift and thin out over many seconds.
			const bool bGas = Style == EStyle::Gas;
			FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Smoke);
			Particle.Position = At + Random.VRand() * 15.0;
			Particle.Velocity = Random.VRand() * Random.FRandRange(40.0f, 160.0f) + FVector(0.0, 0.0, 45.0);
			Particle.Color = bGas ? FLinearColor(0.42f, 0.5f, 0.2f, 0.4f) : FLinearColor(0.55f, 0.55f, 0.57f, 0.6f);
			Particle.Life = Random.FRandRange(5.0f, 9.0f);
			Particle.SizeStart = 50.0f;
			Particle.SizeEnd = Random.FRandRange(260.0f, 420.0f);
			Particle.Drag = 1.1f;
			Particle.Gravity = -6.0f;
			Particle.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
			Particle.Spin = Random.FRandRange(-0.3f, 0.3f);
			Particle.FadeIn = 0.15f;
			break;
		}
		case EStyle::Wisp:
		{
			FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Smoke);
			Particle.Position = At;
			Particle.Velocity = Velocity * 0.1 + Random.VRand() * 8.0 + FVector(0.0, 0.0, 22.0);
			Particle.Color = FLinearColor(0.6f, 0.6f, 0.62f, 0.22f);
			Particle.Life = Random.FRandRange(0.7f, 1.4f);
			Particle.SizeStart = 3.0f;
			Particle.SizeEnd = Random.FRandRange(16.0f, 30.0f);
			Particle.Drag = 1.5f;
			Particle.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
			Particle.Spin = Random.FRandRange(-1.0f, 1.0f);
			Particle.FadeIn = 0.2f;
			break;
		}
		case EStyle::Sparks:
			SpawnSparks(At, FVector::UpVector, 1, FLinearColor(24.0f, 14.0f, 4.0f), 450.0f);
			break;
		case EStyle::Electric:
		{
			// Arcs: short blue-white dashes that jump about the source.
			FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Glow);
			Particle.Position = At + Random.VRand() * 6.0;
			Particle.Velocity = Random.VRand() * Random.FRandRange(150.0f, 520.0f);
			Particle.Color = FLinearColor(8.0f, 14.0f, 30.0f, 1.0f);
			Particle.Life = Random.FRandRange(0.05f, 0.16f);
			Particle.SizeStart = 2.5f;
			Particle.SizeEnd = 1.0f;
			Particle.Stretch = 0.04f;
			break;
		}
		case EStyle::Water:
		{
			FScctFxParticle& Particle = Sprites.Spawn(EScctFxLayer::Smoke);
			Particle.Position = At + Random.VRand() * 4.0;
			Particle.Velocity = Random.VRand() * Random.FRandRange(20.0f, 90.0f) + FVector(0.0, 0.0, 40.0);
			Particle.Color = FLinearColor(0.75f, 0.82f, 0.9f, 0.35f);
			Particle.Life = Random.FRandRange(0.25f, 0.5f);
			Particle.SizeStart = 1.5f;
			Particle.SizeEnd = 4.0f;
			Particle.Gravity = 700.0f;
			Particle.Stretch = 0.03f;
			break;
		}
		default:
			break;
		}
	}
}

// ---------------------------------------------------------------------------------------------------------------
// The looks
// ---------------------------------------------------------------------------------------------------------------

FVector UScctEffectsSubsystem::RandomCone(const FVector& Axis, float HalfAngleDegrees)
{
	return Random.VRandCone(Axis, FMath::DegreesToRadians(HalfAngleDegrees));
}

void UScctEffectsSubsystem::SpawnMuzzleFlash(const FVector& At, const FVector& Direction, float Scale, bool bSticky, bool bSuppressed, bool bShotgun)
{
	// The spy's pistol fires a charged dart: a cold blue flash. A merc's gun burns powder.
	const FLinearColor Hot = bSticky ? FLinearColor(3.0f, 7.0f, 20.0f) : FLinearColor(18.0f, 10.0f, 3.2f);
	const float Size = Scale * (bShotgun ? 1.5f : 1.0f) * (bSuppressed ? 0.45f : 1.0f);
	const float Level = bSuppressed ? 0.35f : 1.0f;

	FScctFxParticle& Core = Sprites.Spawn(EScctFxLayer::Glow);
	Core.Position = At + Direction * 6.0 * Size;
	Core.Color = FLinearColor(Hot.R, Hot.G, Hot.B, Level);
	Core.Life = 0.07f;
	Core.SizeStart = 18.0f * Size;
	Core.SizeEnd = 30.0f * Size;
	Core.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
	Core.FadeIn = 0.01f;

	if (!bSuppressed)
	{
		// The jet out of the barrel and the gas that escapes sideways: a star, different every shot.
		FScctFxParticle& Jet = Sprites.Spawn(EScctFxLayer::Glow);
		Jet.Position = At;
		Jet.Velocity = Direction * 1500.0 * Size;
		Jet.Color = FLinearColor(Hot.R, Hot.G, Hot.B, 1.0f);
		Jet.Life = 0.06f;
		Jet.SizeStart = 13.0f * Size;
		Jet.SizeEnd = 6.0f * Size;
		Jet.Stretch = 0.035f;
		Jet.FadeIn = 0.01f;
		const int32 Spikes = bShotgun ? 7 : 4;
		for (int32 Index = 0; Index < Spikes; ++Index)
		{
			FScctFxParticle& Spike = Sprites.Spawn(EScctFxLayer::Glow);
			Spike.Position = At;
			Spike.Velocity = RandomCone(Direction, 70.0f) * Random.FRandRange(500.0f, 1000.0f) * Size;
			Spike.Color = FLinearColor(Hot.R, Hot.G, Hot.B, 0.8f);
			Spike.Life = Random.FRandRange(0.04f, 0.07f);
			Spike.SizeStart = 8.0f * Size;
			Spike.SizeEnd = 3.0f * Size;
			Spike.Stretch = 0.03f;
			Spike.FadeIn = 0.01f;
		}
	}

	for (int32 Index = 0; Index < 2; ++Index)
	{
		FScctFxParticle& Smoke = Sprites.Spawn(EScctFxLayer::Smoke);
		Smoke.Position = At + Direction * 8.0;
		Smoke.Velocity = Direction * Random.FRandRange(40.0f, 120.0f) + FVector(0.0, 0.0, 18.0) + Random.VRand() * 12.0;
		Smoke.Color = FLinearColor(0.6f, 0.6f, 0.6f, 0.16f);
		Smoke.Life = Random.FRandRange(0.5f, 0.9f);
		Smoke.SizeStart = 6.0f * Size;
		Smoke.SizeEnd = 38.0f * Size;
		Smoke.Drag = 2.5f;
		Smoke.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Smoke.FadeIn = 0.15f;
	}

	const FLinearColor LightTint = bSticky ? FLinearColor(0.3f, 0.55f, 1.0f) : FLinearColor(1.0f, 0.68f, 0.36f);
	// The light sits a little way down the line of fire, so it lights the room and not just the shooter's hands.
	// As strong and as short as the view model's own flash (at 45 cd times an exposure scale of 0.04 in a lamp-lit
	// room a spy's or a bot's shot left the room as it was). It lives as long as the view model's flash light: a
	// rifle fires about ten rounds a second and the pool holds 16 lights.
	// A shooter against a wall would burn it white: the nearest surface holds it down (FScctExposure::WallHold, on
	// the strength AcquireLight will show).
	const FVector LightAt = At + Direction * 35.0;
	const float Candelas = FScctExposure::MuzzleFlashCandelas() * Level;
	const float Hold = FScctExposure::WallHold(LightAt, Candelas * FScctExposure::FlashScale(), 900.0f);
	AcquireLight(LightAt, LightTint, Candelas * Hold, 900.0f, 0.085f, true);
}

void UScctEffectsSubsystem::SpawnSparks(const FVector& At, const FVector& Normal, int32 Count, const FLinearColor& Color, float Speed)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FScctFxParticle& Spark = Sprites.Spawn(EScctFxLayer::Glow);
		Spark.Position = At + Normal * 1.5;
		Spark.Velocity = RandomCone(Normal, 65.0f) * Random.FRandRange(0.25f, 1.0f) * Speed;
		Spark.Color = FLinearColor(Color.R, Color.G, Color.B, 1.0f);
		Spark.Life = Random.FRandRange(0.18f, 0.6f);
		Spark.SizeStart = Random.FRandRange(1.4f, 2.6f);
		Spark.SizeEnd = 0.6f;
		Spark.Stretch = 0.025f;
		Spark.Gravity = 900.0f;
		Spark.Drag = 1.2f;
		Spark.FadeIn = 0.01f;
	}
}

void UScctEffectsSubsystem::SpawnDust(const FVector& At, const FVector& Normal, int32 Count, const FLinearColor& Color, float Size)
{
	for (int32 Index = 0; Index < Count; ++Index)
	{
		FScctFxParticle& Dust = Sprites.Spawn(EScctFxLayer::Smoke);
		Dust.Position = At + Normal * 2.0;
		Dust.Velocity = RandomCone(Normal, 50.0f) * Random.FRandRange(30.0f, 170.0f);
		Dust.Color = Color;
		Dust.Life = Random.FRandRange(0.5f, 1.3f);
		Dust.SizeStart = Size * 0.2f;
		Dust.SizeEnd = Size * Random.FRandRange(0.8f, 1.4f);
		Dust.Drag = 3.0f;
		Dust.Gravity = 25.0f;
		Dust.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Dust.Spin = Random.FRandRange(-1.0f, 1.0f);
		Dust.FadeIn = 0.1f;
	}
}

void UScctEffectsSubsystem::SpawnImpact(const FVector& At, const FVector& Normal, const FString& SparkClass, const FString& TypeClass)
{
	// The surface is in the impact's class and its spark class ("SImpactEffectMetal", "SBase.SSparkWood").
	const auto Is = [&SparkClass, &TypeClass](const TCHAR* Word)
	{
		return SparkClass.Contains(Word) || TypeClass.Contains(Word);
	};

	if (Is(TEXT("Shocker")))
	{
		SpawnSparks(At, Normal, 12, FLinearColor(8.0f, 14.0f, 34.0f), 380.0f);
		AcquireLight(At + Normal * 8.0, FLinearColor(0.35f, 0.55f, 1.0f), 160.0f, 500.0f, 0.12f, false);
	}
	else if (Is(TEXT("Blood")))
	{
		SpawnDust(At, Normal, 6, FLinearColor(0.22f, 0.005f, 0.005f, 0.7f), 22.0f);
	}
	else if (Is(TEXT("Water")))
	{
		SpawnDust(At, Normal, 6, FLinearColor(0.8f, 0.86f, 0.92f, 0.4f), 26.0f);
		float Debt = 8.0f;
		Emit(EStyle::Water, At, FVector::ZeroVector, 0.0f, Debt);
	}
	else if (Is(TEXT("Metal")) || Is(TEXT("Fence")) || Is(TEXT("Armor")))
	{
		SpawnSparks(At, Normal, 16, FLinearColor(30.0f, 20.0f, 8.0f), 750.0f);
		FScctFxParticle& Hit = Sprites.Spawn(EScctFxLayer::Glow);
		Hit.Position = At + Normal * 2.0;
		Hit.Color = FLinearColor(30.0f, 22.0f, 10.0f, 1.0f);
		Hit.Life = 0.06f;
		Hit.SizeStart = 14.0f;
		Hit.SizeEnd = 4.0f;
		SpawnDust(At, Normal, 1, FLinearColor(0.5f, 0.5f, 0.5f, 0.2f), 16.0f);
	}
	else if (Is(TEXT("Wood")))
	{
		SpawnDust(At, Normal, 5, FLinearColor(0.34f, 0.24f, 0.13f, 0.45f), 24.0f);
		SpawnSparks(At, Normal, 3, FLinearColor(4.0f, 2.2f, 0.8f), 300.0f);
	}
	else if (Is(TEXT("Glass")))
	{
		SpawnSparks(At, Normal, 10, FLinearColor(5.0f, 6.0f, 7.0f), 420.0f);
		SpawnDust(At, Normal, 2, FLinearColor(0.8f, 0.85f, 0.9f, 0.2f), 18.0f);
	}
	else if (Is(TEXT("Sand")) || Is(TEXT("Soft")))
	{
		SpawnDust(At, Normal, 6, FLinearColor(0.5f, 0.43f, 0.3f, 0.45f), 30.0f);
	}
	else
	{
		// Concrete and anything unnamed: a puff of grey dust and a few chips that glow for an instant.
		SpawnDust(At, Normal, 5, FLinearColor(0.5f, 0.49f, 0.47f, 0.4f), 26.0f);
		SpawnSparks(At, Normal, 4, FLinearColor(16.0f, 10.0f, 4.0f), 420.0f);
	}
}

float UScctEffectsSubsystem::TintDecal(UMaterialInstanceDynamic* Tint, const FString& TypeClass, const FString& SparkClass)
{
	const auto Is = [&SparkClass, &TypeClass](const TCHAR* Word)
	{
		return SparkClass.Contains(Word) || TypeClass.Contains(Word);
	};

	// The hole itself and the ring of broken surface around it: fresh splinters on wood, bare bright metal,
	// crushed white on glass, chipped grey on concrete. Strength is how much of the surface the mark covers.
	// Rim is how much of the chipped ring shows (1: a crater of broken surface, low: little more than the hole).
	FLinearColor Hole(0.012f, 0.011f, 0.01f);
	FLinearColor Rim(0.3f, 0.29f, 0.27f);
	float Strength = 0.9f;
	float RimStrength = 0.55f;
	float HalfSize = DecalHalfSizeConcrete;
	if (Is(TEXT("Wood")))
	{
		Hole = FLinearColor(0.02f, 0.012f, 0.006f);
		Rim = FLinearColor(0.46f, 0.34f, 0.2f); // fresh splinters, paler than the board
		RimStrength = 0.75f;
		HalfSize = DecalHalfSizeWood;
	}
	else if (Is(TEXT("Metal")) || Is(TEXT("Fence")) || Is(TEXT("Armor")))
	{
		Hole = FLinearColor(0.006f, 0.006f, 0.007f);
		Rim = FLinearColor(0.42f, 0.42f, 0.45f);
		RimStrength = 0.7f;
		HalfSize = DecalHalfSizeMetal;
	}
	else if (Is(TEXT("Glass")))
	{
		Hole = FLinearColor(0.05f, 0.055f, 0.06f);
		Rim = FLinearColor(0.7f, 0.74f, 0.78f);
		Strength = 0.6f;
		RimStrength = 0.45f;
		HalfSize = DecalHalfSizeGlass;
	}
	else if (Is(TEXT("Sand")) || Is(TEXT("Soft")) || Is(TEXT("Blood")))
	{
		Hole = FLinearColor(0.02f, 0.016f, 0.012f);
		Rim = FLinearColor(0.1f, 0.085f, 0.065f);
		Strength = 0.75f;
		RimStrength = 0.35f;
		HalfSize = DecalHalfSizeSoft;
	}
	if (Tint != nullptr)
	{
		Tint->SetVectorParameterValue(TEXT("Color"), Hole);
		Tint->SetVectorParameterValue(TEXT("RimColor"), Rim);
		Tint->SetScalarParameterValue(TEXT("Strength"), Strength);
		Tint->SetScalarParameterValue(TEXT("RimStrength"), RimStrength);
	}
	return HalfSize;
}

void UScctEffectsSubsystem::SpawnExplosion(const FVector& At)
{
	// The instant of the blast: one white-hot ball.
	FScctFxParticle& Flash = Sprites.Spawn(EScctFxLayer::Glow);
	Flash.Position = At + FVector(0.0, 0.0, 20.0);
	Flash.Color = FLinearColor(7.0f, 5.0f, 2.8f, 1.0f);
	Flash.Life = 0.14f;
	Flash.SizeStart = 140.0f;
	Flash.SizeEnd = 260.0f;
	Flash.FadeIn = 0.02f;

	// The fireball rolling outwards and burning out.
	for (int32 Index = 0; Index < 18; ++Index)
	{
		FScctFxParticle& Fire = Sprites.Spawn(EScctFxLayer::Glow);
		const FVector Out = RandomCone(FVector::UpVector, 100.0f);
		Fire.Position = At + Out * 20.0;
		Fire.Velocity = Out * Random.FRandRange(250.0f, 700.0f);
		Fire.Color = FLinearColor(9.0f, 3.2f, 0.6f, 0.9f);
		Fire.Life = Random.FRandRange(0.3f, 0.7f);
		Fire.SizeStart = Random.FRandRange(70.0f, 110.0f);
		Fire.SizeEnd = Random.FRandRange(180.0f, 260.0f);
		Fire.Drag = 4.0f;
		Fire.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Fire.Spin = Random.FRandRange(-2.0f, 2.0f);
		Fire.FadeIn = 0.08f;
	}

	// Burning fragments thrown far and falling.
	for (int32 Index = 0; Index < 60; ++Index)
	{
		FScctFxParticle& Spark = Sprites.Spawn(EScctFxLayer::Glow);
		Spark.Position = At + FVector(0.0, 0.0, 10.0);
		Spark.Velocity = RandomCone(FVector::UpVector, 95.0f) * Random.FRandRange(600.0f, 2400.0f);
		Spark.Color = FLinearColor(16.0f, 8.5f, 2.4f, 1.0f);
		Spark.Life = Random.FRandRange(0.5f, 1.5f);
		Spark.SizeStart = Random.FRandRange(2.5f, 4.5f);
		Spark.SizeEnd = 1.0f;
		Spark.Stretch = 0.02f;
		Spark.Gravity = 980.0f;
		Spark.Drag = 0.9f;
		Spark.FadeIn = 0.01f;
	}

	// The smoke that is left: dark at the heart, rising and thinning for seconds.
	for (int32 Index = 0; Index < 26; ++Index)
	{
		FScctFxParticle& Smoke = Sprites.Spawn(EScctFxLayer::Smoke);
		const FVector Out = RandomCone(FVector::UpVector, 100.0f);
		Smoke.Position = At + Out * 30.0;
		Smoke.Velocity = Out * Random.FRandRange(120.0f, 480.0f) + FVector(0.0, 0.0, 70.0);
		const float Grey = Random.FRandRange(0.05f, 0.2f);
		Smoke.Color = FLinearColor(Grey, Grey, Grey, 0.75f);
		Smoke.Life = Random.FRandRange(2.5f, 5.0f);
		Smoke.SizeStart = Random.FRandRange(80.0f, 130.0f);
		Smoke.SizeEnd = Random.FRandRange(300.0f, 460.0f);
		Smoke.Drag = 2.2f;
		Smoke.Gravity = -22.0f;
		Smoke.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Smoke.Spin = Random.FRandRange(-0.5f, 0.5f);
		Smoke.FadeIn = 0.06f;
	}

	// Dust blown along the ground.
	for (int32 Index = 0; Index < 12; ++Index)
	{
		FScctFxParticle& Dust = Sprites.Spawn(EScctFxLayer::Smoke);
		const float Angle = Random.FRandRange(0.0f, UE_TWO_PI);
		Dust.Position = At;
		Dust.Velocity = FVector(FMath::Cos(Angle), FMath::Sin(Angle), 0.05) * Random.FRandRange(500.0f, 1000.0f);
		Dust.Color = FLinearColor(0.42f, 0.4f, 0.36f, 0.35f);
		Dust.Life = Random.FRandRange(1.2f, 2.4f);
		Dust.SizeStart = 60.0f;
		Dust.SizeEnd = Random.FRandRange(200.0f, 320.0f);
		Dust.Drag = 3.5f;
		Dust.Rotation = Random.FRandRange(0.0f, UE_TWO_PI);
		Dust.FadeIn = 0.1f;
	}

	// The simulation lights the blast too (SFlashLight_FragGrenade, an effect actor of its own); this one is the
	// fireball's glow on the room, kept low so the two together do not white out the picture at a few metres.
	AcquireLight(At + FVector(0.0, 0.0, 40.0), FLinearColor(1.0f, 0.6f, 0.28f), 220.0f, 1600.0f, 0.45f, true);
}

void UScctEffectsSubsystem::SpawnFlashBang(const FVector& At)
{
	FScctFxParticle& Flash = Sprites.Spawn(EScctFxLayer::Glow);
	Flash.Position = At;
	Flash.Color = FLinearColor(60.0f, 60.0f, 66.0f, 1.0f);
	Flash.Life = 0.3f;
	Flash.SizeStart = 500.0f;
	Flash.SizeEnd = 900.0f;
	Flash.FadeIn = 0.02f;
	SpawnSparks(At, FVector::UpVector, 30, FLinearColor(40.0f, 40.0f, 46.0f), 1400.0f);
	SpawnDust(At, FVector::UpVector, 8, FLinearColor(0.7f, 0.7f, 0.7f, 0.3f), 160.0f);
	AcquireLight(At + FVector(0.0, 0.0, 30.0), FLinearColor(1.0f, 1.0f, 1.0f), 5000.0f, 3000.0f, 0.3f, true);
}

// ---------------------------------------------------------------------------------------------------------------
// Lights
// ---------------------------------------------------------------------------------------------------------------

int32 UScctEffectsSubsystem::AcquireLight(const FVector& At, const FLinearColor& Color, float Candelas, float Radius, float Life, bool bShadows)
{
	if (Life > 0.0f && Sprites.SpawnAge >= Life) return INDEX_NONE;
	for (int32 Index = 0; Index < Lights.Num(); ++Index)
	{
		if (Lights[Index].bUsed)
		{
			continue;
		}
		// A timed flash is as bright in the picture in a dark cellar as in a lit hall: without this the eye,
		// opened up for the dark, sees a muzzle flash a metre from a wall as a white screen. A light an effect
		// actor holds (a flare, a burning thing) is a lamp like any other and keeps its strength.
		const float Shown = Life > 0.0f ? Candelas * FScctExposure::FlashScale() : Candelas;
		if (Life > 0.0f)
		{
			FScctExposure::NoteFlash();
		}
		Lights[Index] = { Life, Sprites.SpawnAge, Shown, true };
		UPointLightComponent* Light = LightComponents[Index];
		// A returned flare may have used the fitted display-space falloff. Other effects use candelas.
		Light->SetUseInverseSquaredFalloff(true);
		Light->SetWorldLocation(At);
		Light->SetLightColor(Color);
		Light->SetIntensity(Shown);
		Light->SetAttenuationRadius(Radius);
		Light->SetCastShadows(bShadows);
		Light->SetVisibility(true);
		return Index;
	}
	return INDEX_NONE;
}

void UScctEffectsSubsystem::ReleaseLight(int32 Index)
{
	if (Lights.IsValidIndex(Index) && Lights[Index].bUsed)
	{
		Lights[Index].bUsed = false;
		LightComponents[Index]->SetVisibility(false);
	}
}

void UScctEffectsSubsystem::UpdateLights(float DeltaSeconds)
{
	for (int32 Index = 0; Index < Lights.Num(); ++Index)
	{
		FLight& Light = Lights[Index];
		if (!Light.bUsed || Light.Life <= 0.0f)
		{
			continue;
		}
		Light.Age += DeltaSeconds;
		if (Light.Age >= Light.Life)
		{
			ReleaseLight(Index);
			continue;
		}
		// A flash is brightest at once and dies away quickly.
		LightComponents[Index]->SetIntensity(Light.Candelas * FMath::Square(1.0f - Light.Age / Light.Life));
	}
}

// ---------------------------------------------------------------------------------------------------------------
// Names
// ---------------------------------------------------------------------------------------------------------------

const FString& UScctEffectsSubsystem::GetName(int32 Id)
{
	if (const FString* Known = Names.Find(Id))
	{
		return *Known;
	}
	TCHAR Buffer[256];
	const int32 Length = Id != 0 && Events != nullptr ? Events->GetName(Id, Buffer, UE_ARRAY_COUNT(Buffer)) : 0;
	return Names.Add(Id, Length > 0 ? FString(Buffer) : FString());
}

FName UScctEffectsSubsystem::FindDrawnBone(const AScctPawnProxy& Proxy, int32 BoneId)
{
	// The spy's and the merc's models are different meshes, so the answer is kept per kind.
	const int64 Key = (static_cast<int64>(Proxy.GetPawn().Kind) << 32) | static_cast<uint32>(BoneId);
	if (const FName* Known = DrawnBones.Find(Key))
	{
		return *Known;
	}
	return DrawnBones.Add(Key, Proxy.FindMeshBoneName(GetName(BoneId)));
}

UScctEffectsSubsystem::EStyle UScctEffectsSubsystem::FindStyle(int32 ClassId)
{
	if (const EStyle* Known = Styles.Find(ClassId))
	{
		return *Known;
	}

	// The class names are the original's (French and English mixed): the first word that fits decides.
	static const TPair<const TCHAR*, EStyle> Words[] = {
		{ TEXT("Ps2ModemEffect"), EStyle::Ps2Modem },
		{ TEXT("Douille"), EStyle::None },        // shell casings come with the shots (Play/ScctEffectsCasings.cpp)
		{ TEXT("FootPrint"), EStyle::None },
		{ TEXT("FlashBang"), EStyle::FlashBang },
		{ TEXT("Explosion"), EStyle::Explosion },
		{ TEXT("MineProxEmitter"), EStyle::Explosion },
		{ TEXT("Chaff"), EStyle::Chaff },
		{ TEXT("Flare"), EStyle::Flare },
		{ TEXT("SmokeCanon"), EStyle::BarrelSmoke },
		{ TEXT("Launch"), EStyle::Wisp },
		{ TEXT("Poison"), EStyle::Gas },
		{ TEXT("SStickyCamEmitter"), EStyle::StickyGas },
		{ TEXT("Smoke"), EStyle::Smoke },
		{ TEXT("Phospho"), EStyle::Smoke },
		{ TEXT("Extincteur"), EStyle::Smoke },
		{ TEXT("Steam"), EStyle::Smoke },
		{ TEXT("Shock"), EStyle::Electric },
		{ TEXT("Electric"), EStyle::Electric },
		{ TEXT("Malfunction"), EStyle::Electric },
		{ TEXT("Tazer"), EStyle::Electric },
		{ TEXT("Spark"), EStyle::Sparks },
		{ TEXT("Water"), EStyle::Water },
		{ TEXT("Sprinkler"), EStyle::Water },
		{ TEXT("FlicFloc"), EStyle::Water },
	};
	const FString& Name = GetName(ClassId);
	EStyle Style = Name.IsEmpty() ? EStyle::None : EStyle::Puff;
	for (const TPair<const TCHAR*, EStyle>& Word : Words)
	{
		if (Name.Contains(Word.Key))
		{
			Style = Word.Value;
			break;
		}
	}
	return Styles.Add(ClassId, Style);
}

UStaticMesh* UScctEffectsSubsystem::FindMesh(int32 MeshId)
{
	if (MeshId == 0)
	{
		return nullptr;
	}
	if (const TObjectPtr<UStaticMesh>* Known = Meshes.Find(MeshId))
	{
		return *Known;
	}

	const FString& Path = GetName(MeshId);
	UStaticMesh* Mesh = ScctRuntimeSkins::LoadEffectMesh(Path);
	if (Mesh == nullptr)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects: no locally converted mesh for '%s'; a stand-in shape is drawn"), *Path);
	}
	return Meshes.Add(MeshId, Mesh);
}

// ---------------------------------------------------------------------------------------------------------------
// For other features
// ---------------------------------------------------------------------------------------------------------------

void UScctEffectsSubsystem::SpawnSparkBurst(const FVector& At, const FVector& Normal, int32 Count)
{
	if (!bSceneReady || !IsValid(Host))
	{
		return;
	}
	const FVector Out = Normal.GetSafeNormal(UE_SMALL_NUMBER, FVector::DownVector);
	SpawnSparks(At, Out, Count, FLinearColor(16.0f, 20.0f, 34.0f), 420.0f);
	AcquireLight(At + Out * 10.0, FLinearColor(0.55f, 0.7f, 1.0f), 30.0f, 350.0f, 0.1f, false);
}

UStaticMeshComponent* UScctEffectsSubsystem::FindActorMesh(int32 ActorId) const
{
	// Beams are not here: the goggles draw the detector beams from the markers.
	const FActorFx* Fx = Actors.Find(ActorId);
	const bool bObject = Fx != nullptr && (Fx->Kind == EScctEffectKind::Projectile || Fx->Kind == EScctEffectKind::Mine || Fx->Kind == EScctEffectKind::Gadget);
	return bObject && MeshPool.IsValidIndex(Fx->Mesh) ? MeshPool[Fx->Mesh].Get() : nullptr;
}

void UScctEffectsSubsystem::SpawnGlassBurst(const FVector& At, const FVector& Direction)
{
	if (!bSceneReady || !IsValid(Host))
	{
		return;
	}
	// Shards read as cold glints that fall fast; the fine dust hangs a moment where the pane was.
	const FVector Out = Direction.GetSafeNormal(UE_SMALL_NUMBER, FVector::DownVector);
	SpawnSparks(At, Out, 36, FLinearColor(5.0f, 6.0f, 6.5f), 520.0f);
	SpawnDust(At, Out, 8, FLinearColor(0.82f, 0.87f, 0.9f, 0.3f), 55.0f);
}
