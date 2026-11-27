#include "Play/ScctFirstPersonView.h"

#include "Camera/CameraComponent.h"
#include "Components/PointLightComponent.h"
#include "Components/PoseableMeshComponent.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/World.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/CommandLine.h"
#include "Misc/Parse.h"
#include "OpenSCCT.h"
#include "Play/ScctMercOpticCap.h"
#include "Play/ScctEquipmentData.h"
#include "Play/ScctEquipmentModels.h"
#include "Play/ScctExposure.h"
#include "Play/ScctPlayerController.h"
#include "Play/ScctPoseAim.h"
#include "Play/ScctViewPawn.h"
#include "Runtime/ScctPoseAimApi.h"
#include "Runtime/ScctWeaponAttachmentsApi.h"
#include "UObject/ConstructorHelpers.h"

namespace
{
	constexpr int32 MaxViewBones = 160;

	// The flash is over in a few frames: long enough to be seen at any frame rate, short enough to read as a
	// crack of light and not a glow. Its light outlasts it by a little, as the eye's impression does.
	constexpr float FlashSeconds = 0.05f;
	constexpr float FlashLightSeconds = 0.085f;
	// How far in front of the muzzle the flash's light is (cm), FlashLightPlace.
	constexpr double FlashLightAhead = 40.0;
	// The light's strength is every muzzle flash's (FScctExposure::MuzzleFlashCandelas), scaled by the picture's
	// exposure when it fires, so the flash does not white out the screen when the eye is open for the dark, and
	// held down by the nearest wall (FScctExposure::WallHold) so a wall in front of the gun is not burned white.

	struct FFlashShape
	{
		FVector Offset;  // cm from the muzzle, X along the barrel
		FVector Size;    // cm across
		float Intensity;
		float Sharpness;
	};

	// A hot core, a tongue along the barrel and two thin flares across it (turned to a new angle every shot).
	const FFlashShape FlashShapeList[] = {
		{ FVector(1.5, 0.0, 0.0), FVector(4.5, 4.5, 4.5), 90.0f, 1.5f },
		{ FVector(8.0, 0.0, 0.0), FVector(17.0, 3.5, 3.5), 45.0f, 2.0f },
		{ FVector(2.0, 0.0, 0.0), FVector(1.8, 15.0, 1.8), 35.0f, 2.5f },
		{ FVector(2.0, 0.0, 0.0), FVector(1.8, 1.8, 10.0), 35.0f, 2.5f },
	};
}

AScctFirstPersonView::AScctFirstPersonView()
{
	PrimaryActorTick.bCanEverTick = false;

	Root = CreateDefaultSubobject<USceneComponent>(TEXT("Root"));
	SetRootComponent(Root);
	MercOpticCap = CreateDefaultSubobject<UStaticMeshComponent>(TEXT("MercOpticCap"));
	MercOpticCap->SetupAttachment(Root);
	ScctMercOpticCap::Configure(*MercOpticCap);

	FlashRoot = CreateDefaultSubobject<USceneComponent>(TEXT("FlashRoot"));
	FlashRoot->SetupAttachment(Root);
	FlashRoot->SetUsingAbsoluteLocation(true);
	FlashRoot->SetUsingAbsoluteRotation(true);
	FlashRoot->SetUsingAbsoluteScale(true);

	static ConstructorHelpers::FObjectFinder<UStaticMesh> Sphere(TEXT("/Engine/BasicShapes/Sphere.Sphere"));
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(FlashShapeList); ++Index)
	{
		UStaticMeshComponent* Shape = CreateDefaultSubobject<UStaticMeshComponent>(*FString::Printf(TEXT("Flash%d"), Index));
		Shape->SetupAttachment(FlashRoot);
		Shape->SetCollisionEnabled(ECollisionEnabled::NoCollision);
		Shape->SetCastShadow(false);
		Shape->SetVisibility(false);
		// Drawn with the gun, in front of any wall the gun is in front of.
		Shape->SetFirstPersonPrimitiveType(EFirstPersonPrimitiveType::FirstPerson);
		if (Sphere.Succeeded())
		{
			Shape->SetStaticMesh(Sphere.Object);
		}
		FlashShapes.Add(Shape);
	}

	FlashLight = CreateDefaultSubobject<UPointLightComponent>(TEXT("FlashLight"));
	FlashLight->SetupAttachment(Root);
	FlashLight->SetUsingAbsoluteLocation(true);
	FlashLight->SetIntensityUnits(ELightUnits::Candelas);
	FlashLight->SetIntensity(0.0f);
	FlashLight->SetAttenuationRadius(900.0f);
	FlashLight->SetSourceRadius(3.0f);
	FlashLight->SetLightColor(FLinearColor(1.0f, 0.74f, 0.42f));
	// No shadows: the player's own body, unseen at the real place of the arms, stood between the light and the arms
	// as drawn (nearer the eye) and laid hard black bands across the sleeves. A flash is too short for its shadows
	// in the room to be missed, and FlashLightPlace keeps it on this side of a wall.
	FlashLight->SetCastShadows(false);
	// A flash is too short to hang in the air: almost none of it scatters in the fog.
	FlashLight->SetVolumetricScatteringIntensity(0.2f);
	FlashLight->SetVisibility(false);
}

void AScctFirstPersonView::AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector)
{
	AScctFirstPersonView* View = CastChecked<AScctFirstPersonView>(InThis);
	View->CloakLook.AddReferencedObjects(Collector);
	for (FSightBinding& Binding : View->SightBindings)
	{
		Collector.AddReferencedObject(Binding.Original);
	}
	Super::AddReferencedObjects(InThis, Collector);
}

FTransform AScctFirstPersonView::MercWeaponPose(const FString& Model, float Aim, float Speed, bool bFresh, const FScctWeaponMotion& Motion, float DeltaSeconds)
{
	const bool bStock = Model.StartsWith(TEXT("SWeaponFirst.1DEF_"));
	if (!bStock && !Model.StartsWith(TEXT("OpenScctRifles.")))
	{
		return FTransform::Identity;
	}
	// Carry the authored hand and rifle animation in the same frame through reload and idle.
	// The reload clip supplies all motion relative to that grip.
	const float Ease = bFresh ? 1.0f : 1.0f - FMath::Exp(-12.0f * DeltaSeconds);
	CarryPace = FMath::Lerp(CarryPace, FMath::Clamp(Speed / 280.0f, 0.0f, 1.0f), Ease);
	FVector Carry(-9.0, bStock ? -8.0 : -10.5, -7.5 - 2.0 * CarryPace);
	if (Model == TEXT("OpenScctRifles.compactsmg_first"))
	{
		// Its short receiver sits behind the lower edge in the long-rifle carry pose.
		const float Idle = Motion.bFiring || Motion.bReloading ? 0.0f : 1.0f - CarryPace;
		CompactIdleBlend = bFresh ? Idle : FMath::Lerp(CompactIdleBlend, Idle, Ease);
		Carry += FVector(4.5, 1.2, 3.2) * CompactIdleBlend;
	}
	else if (Model == TEXT("OpenScctRifles.boltrifle_first"))
	{
		Carry += FVector(0, -3.4, 7.75);
	}
	else if (Model == TEXT("OpenScctRifles.designatedmarksman_first"))
	{
		Carry += FVector(0, -3.4, 8.15);
	}
	const int32 Attachments = Motion.AttachmentMask;
	FTransform Shoulder = FTransform::Identity;
	if ((Attachments & EScctWeaponAttachment::Optics) == EScctWeaponAttachment::RedDot && Model == TEXT("OpenScctRifles.m4a1s_first"))
	{
		// At 26 cm the rear ring stays ahead of the near plane throughout the recoil animation.
		// At that distance the hood and the view through its 5.7 cm tube have the reference proportions.
		Shoulder.SetTranslation(FVector(8.0, 0, 0));
	}
	else if (!(Attachments & EScctWeaponAttachment::Optics) && Model == TEXT("SWeaponFirst.1DEF_01"))
	{
		// The stock FN has a solid upper receiver. Look just over its top, measured from the idle
		// mesh in eye space. The yellow panel on this model is attached to the left forearm.
		const FVector ReceiverTop(13.62, 12.63, -4.83);
		const FVector Bore = FVector(0.9998, -0.01685, -0.01055).GetSafeNormal();
		const FQuat Align = FQuat::FindBetweenNormals(Bore, FVector::ForwardVector);
		Shoulder = FTransform(Align, FVector(15.0, 0, -1.2) - Align.RotateVector(ReceiverTop));
	}
	// Other rifles already have a sight socket and eye relief in the bridge. At full ADS their
	// authored sight pose is used directly. Never track the moving muzzle back to the crosshair:
	// that would cancel recoil, reloads and the motion of the original clips.
	FTransform Pose;
	Pose.Blend(FTransform(Carry), Shoulder, FMath::Clamp(Aim, 0.0f, 1.0f));
	// The hands lead the stock into the shoulder, then settle. On lowering, the heavier stock
	// follows for a little longer. Both terms go to zero while the button is held or released.
	const float FollowRate = Aim > ShoulderFollow ? 11.0f : 7.5f;
	ShoulderFollow = bFresh ? Aim : FMath::Lerp(ShoulderFollow, Aim, 1.0f - FMath::Exp(-FollowRate * DeltaSeconds));
	const float Travel = Aim - ShoulderFollow;
	const FQuat Settle = FRotator(4.0f * Travel, 0, -2.0f * Travel).Quaternion();
	const FVector Grip(10.0, 0, -14.0);
	Pose = Pose * FTransform(Settle, Grip - Settle.RotateVector(Grip));
	return Pose;
}

void AScctFirstPersonView::SetSightProjection(UCameraComponent* Camera, float Aim, float RaisedHalfTan)
{
	if (SightCamera.Get() != Camera)
	{
		if (UCameraComponent* Previous = SightCamera.Get())
		{
			Previous->SetEnableFirstPersonFieldOfView(false);
		}
		SightCamera = Camera;
	}
	SightFocalScale = 1.0f;
	if (Camera)
	{
		const float WorldHalfTan = FMath::Tan(FMath::DegreesToRadians(Camera->FieldOfView * 0.5f));
		const float Raise = FMath::SmoothStep(0.0f, 1.0f, Aim);
		SightFocalScale = FMath::Lerp(1.0f, WorldHalfTan / RaisedHalfTan, Raise);
		const float HalfTan = WorldHalfTan / SightFocalScale;
		Camera->SetFirstPersonFieldOfView(FMath::RadiansToDegrees(2.0f * FMath::Atan(HalfTan)));
		Camera->SetEnableFirstPersonFieldOfView(Raise > UE_SMALL_NUMBER);
	}
}

FVector AScctFirstPersonView::ProjectSocket(const FVector& World, float FirstPersonScale) const
{
	const UCameraComponent* Camera = SightCamera.Get();
	const FTransform& Eye = Camera ? Camera->GetComponentTransform() : LastCamera;
	FVector Relative = Eye.InverseTransformPositionNoScale(World);
	Relative.Y *= SightFocalScale;
	Relative.Z *= SightFocalScale;
	return Eye.TransformPositionNoScale(Relative * FirstPersonScale);
}

void AScctFirstPersonView::SetStockSight(EStockSight Mode)
{
	if (StockSight == Mode) return;
	for (const FHiddenSightSection& Section : HiddenSightMaterials)
	{
		if (UPoseableMeshComponent* Mesh = Section.Mesh.Get())
		{
			Mesh->ShowMaterialSection(Section.Slot, INDEX_NONE, true, Section.Lod);
		}
	}
	HiddenSightMaterials.Reset();
	StockSight = Mode;
	if (Mode == EStockSight::Visible) return;

	// All five MicroRedDot material families are exclusive to the authored M4 optic.
	// Walk every slot: import can split the shell over several slots or follower components.
	Parts[0].ForEachMesh([this, Mode](UMeshComponent& Component)
	{
		UPoseableMeshComponent* Mesh = Cast<UPoseableMeshComponent>(&Component);
		if (!Mesh) return;
		const TArray<FName> Slots = Mesh->GetMaterialSlotNames();
		for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
		{
			const FString Name = Slots[Slot].ToString();
			if (!Name.StartsWith(TEXT("OpenScctRifles_Optic"))) continue;
			if (Mode == EStockSight::ReplaceHousing && (Name.StartsWith(TEXT("OpenScctRifles_OpticLens"))
				|| Name.StartsWith(TEXT("OpenScctRifles_OpticReticle")))) continue;
			for (int32 Lod = 0; Lod < Mesh->GetNumLODs(); ++Lod)
			{
				if (Mesh->IsMaterialSectionShown(Slot, Lod))
				{
					Mesh->ShowMaterialSection(Slot, INDEX_NONE, false, Lod);
					HiddenSightMaterials.Add({Mesh, Slot, Lod});
				}
			}
		}
	});
}

void AScctFirstPersonView::SetM4SightGlass(bool bEnabled)
{
	if (bEnabled == bM4SightGlass)
	{
		return;
	}
	bM4SightGlass = bEnabled;
	if (!bEnabled)
	{
		for (const FSightBinding& Binding : SightBindings)
		{
			if (UMeshComponent* Mesh = Binding.Mesh.Get())
			{
				// The suit snapshots the underlying materials. Release its snapshot before changing a slot.
				CloakLook.Apply(*Mesh, 0.0f);
				if (Mesh->GetMaterial(Binding.Slot) == M4SightGlass)
				{
					Mesh->SetMaterial(Binding.Slot, Binding.Original);
				}
			}
		}
		SightBindings.Reset();
		return;
	}
	if (!M4SightGlass)
	{
		M4SightGlass = LoadObject<UMaterialInterface>(nullptr,
			TEXT("/Game/OpenSCCT/Materials/M_ScctMercSightLens.M_ScctMercSightLens"), nullptr, LOAD_NoWarn);
	}
	if (M4SightGlass)
	{
		static const FName LensSlot(TEXT("OpenScctRifles_OpticLens"));
		Parts[0].ForEachMesh([this](UMeshComponent& Mesh)
		{
			const TArray<FName> Slots = Mesh.GetMaterialSlotNames();
			for (int32 Slot = 0; Slot < Slots.Num(); ++Slot)
			{
				if (Slots[Slot] == LensSlot)
				{
					CloakLook.Apply(Mesh, 0.0f);
					SightBindings.Add({&Mesh, Slot, Mesh.GetMaterial(Slot)});
					Mesh.SetMaterial(Slot, M4SightGlass);
				}
			}
		});
	}
}

void AScctFirstPersonView::Present(const FScctEquipmentApi& Api, FScctEquipmentModels& Models, const UScctWeaponSet* Weapons,
	const FScctFirstPerson& View, const FTransform& Camera, float Cloak, const FScctWeaponMotion& Motion, float DeltaSeconds)
{
	const int32 PartCount = View.Kind != EScctFirstPersonKind::None ? FMath::Clamp(View.PartCount, 0, 2) : 0;
	const auto* Player = Cast<AScctPlayerController>(GetWorld()->GetFirstPlayerController());
	const auto* ViewPawn = Player ? Cast<AScctViewPawn>(Player->GetViewTarget()) : nullptr;
	const bool bMercWeapon = View.Kind == EScctFirstPersonKind::Merc && PartCount > 0;
	FTransform WeaponFrame = Camera;
	const bool bM4 = bMercWeapon
		&& Models.Name(Api, View.Parts[0].Mesh) == TEXT("OpenScctRifles.m4a1s_first");
	const bool bAcog = (Motion.AttachmentMask & EScctWeaponAttachment::Acog) != 0;
	const bool bHasDot = !bAcog && (Motion.AttachmentMask & EScctWeaponAttachment::RedDot) != 0;
	const bool bHasOptic = bHasDot || bAcog;
	const bool bM4Sight = bM4 && bHasDot;
	// Every other rifle carries the M4's optic as a separate part (SM_MercRedDot). It gets the same raised focal
	// length and the same eye relief, so its aperture, glass and dot look as they do on the M4.
	const bool bRifleDot = bMercWeapon && bHasDot && !bM4
		&& !Models.Name(Api, View.Parts[0].Mesh).StartsWith(TEXT("OpenScctSidearms."));
	const float Aim = Player && View.Kind == EScctFirstPersonKind::Merc ? Player->GetLookView().RifleAimBlend : 0.0f;
	if (bMercWeapon)
	{
		const bool bFresh = !bShown || MercWeaponActor != View.ActorId || PartMeshes[0] != View.Parts[0].Mesh;
		const float Speed = !bFresh && DeltaSeconds > UE_SMALL_NUMBER
			? static_cast<float>(FVector::Dist2D(LastCamera.GetLocation(), Camera.GetLocation()) / DeltaSeconds) : 0.0f;
		const FString& Model = Models.Name(Api, View.Parts[0].Mesh);
		WeaponFrame = MercWeaponPose(Model, Aim, Speed, bFresh, Motion, DeltaSeconds) * Camera;
	}
	const float ScopePower = Player && View.Kind == EScctFirstPersonKind::Merc && !bHasDot ? Player->GetLookView().ScopeLensPower : 0.0f;
	UCameraComponent* Lens = (bM4Sight || bRifleDot || ScopePower > 0.0f) && ViewPawn ? ViewPawn->FindComponentByClass<UCameraComponent>() : nullptr;
	// The raised precision eyepieces fill about 46% of the viewport height at their
	// authored eye relief. Only the weapon uses this focal length.
	SetSightProjection(Lens, Aim, ScopePower > 0.0f ? 0.194f : 0.260f);
	if (Lens && !FMath::IsNearlyEqual(SightFocalScale, 1.0f))
	{
		// Aiming is independent of chest-camera rotation. Keep the physical dot on the simulation's
		// ray by undoing the sight projection on that ray alone. The gun's own recoil remains visible.
		const FQuat ViewRotation = Lens->GetComponentQuat();
		const FVector AimRay = Camera.GetRotation().GetForwardVector();
		FVector Ray = ViewRotation.UnrotateVector(AimRay);
		Ray.Y /= SightFocalScale;
		Ray.Z /= SightFocalScale;
		const FQuat Correction = FQuat::FindBetweenNormals(AimRay, ViewRotation.RotateVector(Ray.GetSafeNormal()));
		WeaponFrame.SetLocation(Camera.GetLocation() + Correction.RotateVector(WeaponFrame.GetLocation() - Camera.GetLocation()));
		WeaponFrame.SetRotation(Correction * WeaponFrame.GetRotation());
	}
	MercWeaponActor = bMercWeapon ? View.ActorId : 0;
	if (!bM4 || PartMeshes[0] != View.Parts[0].Mesh) SetStockSight(EStockSight::Visible);
	if (!bM4Sight || (PartCount > 0 && PartMeshes[0] != View.Parts[0].Mesh))
	{
		SetM4SightGlass(false);
		ScctMercOpticCap::Update(*MercOpticCap, nullptr);
	}
	SetActorTransform(WeaponFrame);
	LastCamera = Camera;
	bHasMuzzle = false;
	bHasLaser = false;

	// The part that is the weapon: the merc's one model, the spy's second (the first is his arms).
	const int32 WeaponPart = View.Kind == EScctFirstPersonKind::Spy ? 1 : 0;
	const bool bWasShown = bShown;
	bShown = false;

	PoseBuffer.SetNumUninitialized(MaxViewBones);
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(Parts); ++Index)
	{
		FScctEquipmentView& Drawn = Parts[Index];
		if (Index >= PartCount)
		{
			if (Drawn.IsShown() && Drawn.GetBody()->IsVisible())
			{
				Drawn.GetBody()->SetVisibility(false, true);
			}
			continue;
		}

		const FScctFirstPersonPart& Part = View.Parts[Index];
		if (PartMeshes[Index] != Part.Mesh || !Drawn.IsShown())
		{
			const FScctWeaponPresentation* Row = Weapons != nullptr && Index == WeaponPart ? Weapons->Find(FName(*Models.Name(Api, View.WeaponClass))) : nullptr;
			if (Row != nullptr && !Row->bFirstPerson)
			{
				Row = nullptr;
			}
			const FScctEquipmentModel* Model = Models.Find(Api, Part.Mesh, Part.MeshPath, Row != nullptr && Row->Mesh.IsValid() ? &Row->Mesh : nullptr);
			Drawn.Show(*this, Model, Row, true);
			PartMeshes[Index] = Part.Mesh;
			RateOfFireSkin = -1;
			if (Model != nullptr)
			{
				UE_LOG(LogOpenSCCT, Log, TEXT("First person: part %d is '%s' (weapon class %s), %d bones posed by the bridge, muzzle bone %d, laser bone %d"),
					Index, *Model->Name, *Models.Name(Api, View.WeaponClass), Part.BoneCount, Part.MuzzleBone, Part.LaserBone);
			}
		}
		if (!Drawn.IsShown())
		{
			continue;
		}
		if (Index == 0 && bM4)
		{
			SetM4SightGlass(bM4Sight);
			SetStockSight(!bHasDot ? EStockSight::Removed : bM4Sight && ScctMercOpticCap::HasHousing(*MercOpticCap)
				? EStockSight::ReplaceHousing : EStockSight::Visible);
		}

		UPoseableMeshComponent* Body = Drawn.GetBody();
		// A model can come with a scale of its own above its skeleton (OpenSCCT's pistols are authored a hundred
		// times too large under a frame scaled 0.01). On the component that scale also shrinks the mesh's bounds
		// to a speck, and Unreal then does not draw the mesh at all. It is moved onto the skeleton's root bones,
		// which puts every bone in the same place with the same size and leaves the bounds as the mesh has them.
		FTransform ModelToCamera = FScctPose::ToUnreal(Part.ModelToCamera);
		const float ModelScale = static_cast<float>(ModelToCamera.GetScale3D().X);
		const bool bModelScaled = ModelScale > UE_SMALL_NUMBER && !FMath::IsNearlyEqual(ModelScale, 1.0f, 0.001f) && ModelToCamera.GetScale3D().IsUniform(0.001);
		if (bModelScaled)
		{
			ModelToCamera.SetScale3D(FVector::OneVector);
		}
		Body->SetRelativeTransform(ModelToCamera);
		if (!Body->IsVisible())
		{
			Body->SetVisibility(true, true);
		}
		const int32 Bones = FMath::Clamp(Api.GetFirstPersonPose(Index, PoseBuffer.GetData(), PoseBuffer.Num()), 0, PoseBuffer.Num());
		if (bModelScaled && Drawn.GetModel() != nullptr)
		{
			const TArray<FScctBridgeBoneInfo>& Skeleton = Drawn.GetModel()->Skeleton;
			for (int32 Bone = 0; Bone < Bones && Bone < Skeleton.Num(); ++Bone)
			{
				if (Skeleton[Bone].Parent < 0)
				{
					FScctBridgeBone& Top = PoseBuffer[Bone];
					Top.Px *= ModelScale; Top.Py *= ModelScale; Top.Pz *= ModelScale;
					Top.Sx *= ModelScale; Top.Sy *= ModelScale; Top.Sz *= ModelScale;
				}
			}
		}
		Drawn.ApplyPose(TConstArrayView<FScctBridgeBone>(PoseBuffer.GetData(), Bones));
		const bool bMercGun = Index == 0 && View.Kind == EScctFirstPersonKind::Merc;
		Drawn.UpdateRailModule(bMercGun ? Motion.AttachmentMask : INDEX_NONE);
		if (bMercGun && Aim > 0.0f && !(bM4 && bHasDot))
		{
			// Use the animated reference so the sights retain walk, reload and recoil motion.
			static const FName Ads(TEXT("ADS"));
			FTransform Reference, Selected;
			bool bHasReference = Body->DoesSocketExist(Ads);
			if (bHasReference)
				Reference = Body->GetSocketTransform(Ads);
			else if (bHasOptic || Models.Name(Api, Part.Mesh).StartsWith(TEXT("OpenScctSidearms.")))
				bHasReference = Drawn.FindPoint(NAME_None, TEXT("FX"), Reference);
			FQuat Alignment = FQuat::Identity;
			if (bHasReference && (Drawn.FindAttachmentSight(Selected) || Drawn.FindIronSight(Reference, Selected, Alignment)))
			{
				if (!Alignment.IsIdentity())
				{
					const FQuat Turn = FQuat::Slerp(FQuat::Identity, Alignment, Aim);
					const FVector Pivot = Selected.GetLocation();
					WeaponFrame.SetLocation(Pivot + Turn.RotateVector(WeaponFrame.GetLocation() - Pivot));
					WeaponFrame.SetRotation(Turn * WeaponFrame.GetRotation());
				}
				WeaponFrame.AddToTranslation((Reference.GetLocation() - Selected.GetLocation()) * Aim);
				SetActorTransform(WeaponFrame);
				if (bRifleDot && Drawn.FindAttachmentSight(Selected))
				{
					// The ADS sockets of the other rifles put the optic about 11 cm from the eye, where the M4's housing
					// fills the screen. Push the gun out along the view to the M4's eye relief (its housing sits 25.8 cm
					// out at full aim), so the shared optic has the M4's proportions.
					constexpr double M4Relief = 25.8;
					const FVector Forward = Camera.GetRotation().GetForwardVector();
					const double Relief = FVector::DotProduct(Selected.GetLocation() - Camera.GetLocation(), Forward);
					WeaponFrame.AddToTranslation(Forward * ((M4Relief - Relief) * Aim));
					SetActorTransform(WeaponFrame);
				}
			}
		}
		if (Index == 0 && bM4Sight)
		{
			ScctMercOpticCap::Update(*MercOpticCap, Body);
		}
		if (Index == WeaponPart && Drawn.HasReplacement())
		{
			// A model drawn in place of the original gun: its parts move with the simulation and the left hand
			// of the arms (this same model) closes on its grip.
			Drawn.Animate(Motion, DeltaSeconds);
			Drawn.HoldWithLeftHand(*Body);
		}
		Drawn.LogProbe();
		LogViewProbe(Index, *Body, Camera);
		Drawn.ForEachMesh([this, Cloak](UMeshComponent& Mesh) { CloakLook.Apply(Mesh, Cloak); });
		bShown = true;

		// The bridge's own muzzle and laser socket are exact for the original model; a replacement gun has its own.
		if (Index == WeaponPart || PartCount == 1)
		{
			if (Part.MuzzleBone >= 0)
			{
				Muzzle = FScctPose::ToUnreal(Part.Muzzle) * WeaponFrame;
				bHasMuzzle = true;
			}
			if (Part.LaserBone >= 0)
			{
				Laser = FScctPose::ToUnreal(Part.Laser) * WeaponFrame;
				bHasLaser = true;
			}
			FTransform Socket;
			if (bMercGun && Drawn.FindMuzzle(Socket))
			{
				Muzzle = Socket;
				bHasMuzzle = true;
			}
			if (Drawn.HasReplacement())
			{
				if (Drawn.FindPoint(Drawn.GetRow()->MuzzleSocket, TEXT(""), Socket))
				{
					Muzzle = Socket;
					bHasMuzzle = true;
				}
				if (Drawn.FindPoint(Drawn.GetRow()->LaserSocket, TEXT(""), Socket))
				{
					Laser = Socket;
					bHasLaser = true;
				}
			}
		}

		// The stock rifle has a firing-mode display. Other meshes can use that section index for an optic.
		if (Index == 0 && View.Kind == EScctFirstPersonKind::Merc && RateOfFireSkin != View.RateOfFireSkin && Drawn.GetModel() != nullptr)
		{
			RateOfFireSkin = View.RateOfFireSkin;
			static const FName DisplayName(TEXT("SWeaponTex_SI_rof_single"));
			const int32 DisplaySlot = Body->GetMaterialIndex(DisplayName);
			if (DisplaySlot != INDEX_NONE)
			{
				UMaterialInterface* Skin = View.RateOfFireSkin > 0 ? Models.FindSkin(Drawn.GetModel()->Package, Models.Name(Api, View.RateOfFireSkin)) : nullptr;
				if (Skin != nullptr || View.RateOfFireSkin <= 0)
				{
					Body->SetMaterial(DisplaySlot, Skin);
				}
			}
		}
	}

	UpdatePrecisionImage(ScopePower > 0.0f && bShown ? Parts[0].GetMountedOptic() : nullptr, Lens, ScopePower, bAcog);

	// A wall between the eye and the spy pistol's sockets: the pistol stays where it is drawn, but its flash and laser
	// do not start inside or beyond the wall (SpyFirstPersonView.TryGetSocket of the Godot client).
	const int32 Hidden = bShown && View.Kind == EScctFirstPersonKind::Spy ? ScctPoseAim::ReadHiddenSockets() : 0;
	bMuzzleHidden = (Hidden & EScctHiddenSocket::Muzzle) != 0;
	bLaserHidden = (Hidden & EScctHiddenSocket::Laser) != 0;
	if (Hidden != LoggedHiddenSockets)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("First person: %s (muzzle socket %.1f cm from the eye)"),
			Hidden == 0 ? TEXT("the pistol's sockets are clear of walls again")
				: Hidden == (EScctHiddenSocket::Muzzle | EScctHiddenSocket::Laser) ? TEXT("muzzle and laser sockets hidden behind a wall")
				: (Hidden & EScctHiddenSocket::Muzzle) != 0 ? TEXT("muzzle socket hidden behind a wall") : TEXT("laser socket hidden behind a wall"),
			ScctPoseAim::LastMuzzleDistance());
		LoggedHiddenSockets = Hidden;
	}
	if (bShown && View.Kind == EScctFirstPersonKind::Merc)
	{
		ScctPoseAim::LogViewModelClipRates();
	}

	if (bShown != bWasShown)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("First person: view model %s (kind %d, %d parts); muzzle %.1f cm from the eye"),
			bShown ? TEXT("shown") : TEXT("hidden"), View.Kind, PartCount, bHasMuzzle ? FVector::Dist(Muzzle.GetLocation(), Camera.GetLocation()) : 0.0);
	}
	CloakLook.Trim();
	TickFlash(DeltaSeconds);
}

void AScctFirstPersonView::LogViewProbe(int32 Index, UPoseableMeshComponent& Body, const FTransform& Camera)
{
	// -ScctWeaponProbe: where the view model's bones are from the eye (X ahead, Y right, Z up), every few seconds.
	static const bool bProbe = FParse::Param(FCommandLine::Get(), TEXT("ScctWeaponProbe"));
	if (!bProbe || FPlatformTime::Seconds() < NextViewProbe[Index])
	{
		return;
	}
	NextViewProbe[Index] = FPlatformTime::Seconds() + 4.0;

	const FTransform Model = Body.GetRelativeTransform();
	const FVector Centre = Camera.InverseTransformPosition(Body.Bounds.Origin);
	UE_LOG(LogOpenSCCT, Log, TEXT("First person probe: part %d %s sits at (%.1f %.1f %.1f) rotation (pitch %.1f yaw %.1f roll %.1f) scale (%.2f %.2f %.2f) from the eye; bounds centre (%.1f %.1f %.1f) radius %.1f; %d bones, from the eye:"),
		Index, *GetNameSafe(Body.GetSkinnedAsset()), Model.GetLocation().X, Model.GetLocation().Y, Model.GetLocation().Z,
		Model.Rotator().Pitch, Model.Rotator().Yaw, Model.Rotator().Roll, Model.GetScale3D().X, Model.GetScale3D().Y, Model.GetScale3D().Z,
		Centre.X, Centre.Y, Centre.Z, Body.Bounds.SphereRadius, Body.GetNumBones());
	for (int32 Bone = 0; Bone < Body.GetNumBones(); ++Bone)
	{
		const FTransform World = Body.GetBoneTransform(Bone);
		const FVector At = Camera.InverseTransformPosition(World.GetLocation());
		const FTransform Rest = Body.GetSkinnedAsset() != nullptr && Body.GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose().IsValidIndex(Bone)
			? Body.GetSkinnedAsset()->GetRefSkeleton().GetRefBonePose()[Bone] : FTransform::Identity;
		UE_LOG(LogOpenSCCT, Log, TEXT("First person probe:   bone %2d '%s' at (%.1f %.1f %.1f) scale (%.2f %.2f %.2f); at rest (%.1f %.1f %.1f) scale %.3f from its parent"), Bone, *Body.GetBoneName(Bone).ToString(),
			At.X, At.Y, At.Z, World.GetScale3D().X, World.GetScale3D().Y, World.GetScale3D().Z, Rest.GetLocation().X, Rest.GetLocation().Y, Rest.GetLocation().Z, Rest.GetScale3D().X);
	}
}

void AScctFirstPersonView::Hide(float DeltaSeconds)
{
	UpdatePrecisionImage(nullptr, nullptr, 0.0f);
	SetSightProjection(nullptr, 0.0f);
	ScctMercOpticCap::Update(*MercOpticCap, nullptr);
	SetM4SightGlass(false);
	SetStockSight(EStockSight::Visible);
	MercWeaponActor = 0;
	if (bShown)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("First person: view model hidden"));
	}
	bShown = false;
	bHasMuzzle = false;
	bHasLaser = false;
	bMuzzleHidden = false;
	bLaserHidden = false;
	for (FScctEquipmentView& Drawn : Parts)
	{
		if (Drawn.IsShown() && Drawn.GetBody()->IsVisible())
		{
			Drawn.GetBody()->SetVisibility(false, true);
		}
	}
	TickFlash(DeltaSeconds);
}

bool AScctFirstPersonView::FindMuzzle(FTransform& OutWorld) const
{
	OutWorld = Muzzle;
	return bShown && bHasMuzzle && !bMuzzleHidden;
}

bool AScctFirstPersonView::FindCasingPort(FTransform& OutWorld) const
{
	if (!bShown)
	{
		return false;
	}
	// The merc's one model carries the gun; the spy's pistol has no port. A replacement gun's EjectionPort socket,
	// else the original's bone, which the replacement is fixed to.
	for (const FScctEquipmentView& Drawn : Parts)
	{
		if (Drawn.IsShown() && Drawn.GetBody()->IsVisible() && Drawn.FindPoint(TEXT("EjectionPort"), TEXT("FX_DOUILLE"), OutWorld))
		{
			return true;
		}
	}
	return false;
}

bool AScctFirstPersonView::FindMercEmitter(bool bTorch, float FirstPersonScale, FTransform& OutWorld, bool bRequireRail) const
{
	const FScctEquipmentView& Gun = Parts[0];
	FTransform Socket;
	if (!bShown || !Gun.IsShown() || !Gun.GetBody()->IsVisible()
		|| (!Gun.FindMercEmitter(bTorch, Socket, bRequireRail) && (bRequireRail || !FindLaserStart(Socket))))
	{
		return false;
	}
	// First-person FOV stretches Y and Z in camera space. Transform the socket axis too,
	// so the light tracks the visible module through a raise, a turn and recoil.
	const FVector At = ProjectSocket(Socket.GetLocation(), FirstPersonScale);
	const FVector Forward = ProjectSocket(Socket.TransformPositionNoScale(FVector(100, 0, 0)), FirstPersonScale) - At;
	const FVector Up = ProjectSocket(Socket.TransformPositionNoScale(FVector(0, 0, 100)), FirstPersonScale) - At;
	OutWorld = FTransform(FRotationMatrix::MakeFromXZ(Forward, Up).ToQuat(), At);
	return true;
}

bool AScctFirstPersonView::FindLaserStart(FTransform& OutWorld) const
{
	// A socket a wall hides from the eye is not a start: the laser then starts where the simulation puts it.
	const bool bLaserClear = bHasLaser && !bLaserHidden;
	const bool bMuzzleClear = bHasMuzzle && !bMuzzleHidden;
	OutWorld = bLaserClear ? Laser : Muzzle;
	return bShown && (bLaserClear || bMuzzleClear);
}

void AScctFirstPersonView::PlayMuzzleFlash()
{
	if (!bShown || !bHasMuzzle || bMuzzleHidden)
	{
		return;
	}

	if (FlashMaterials.Num() == 0)
	{
		UMaterialInterface* Glow = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/OpenSCCT/Weapons/Materials/M_ScctGlow.M_ScctGlow"), nullptr, LOAD_NoWarn);
		if (Glow == nullptr)
		{
			UE_LOG(LogOpenSCCT, Warning, TEXT("First person: no /Game/OpenSCCT/Weapons/Materials/M_ScctGlow (Tools/Import/Import-Characters.ps1 builds it); the muzzle flash is only its light"));
		}
		for (int32 Index = 0; Index < FlashShapes.Num(); ++Index)
		{
			UMaterialInstanceDynamic* Material = Glow != nullptr ? UMaterialInstanceDynamic::Create(Glow, this) : nullptr;
			if (Material != nullptr)
			{
				Material->SetVectorParameterValue(TEXT("Color"), FLinearColor(1.0f, 0.62f, 0.26f));
				Material->SetScalarParameterValue(TEXT("Intensity"), FlashShapeList[Index].Intensity);
				Material->SetScalarParameterValue(TEXT("Sharpness"), FlashShapeList[Index].Sharpness);
				FlashShapes[Index]->SetMaterial(0, Material);
			}
			FlashMaterials.Add(Material);
		}
	}

	// No two flashes alike: a new size and a new turn about the barrel each shot.
	const float Size = FMath::FRandRange(0.8f, 1.2f);
	const FRotator Turn(0.0, 0.0, FMath::FRandRange(0.0f, 180.0f));
	for (int32 Index = 0; Index < FlashShapes.Num(); ++Index)
	{
		const FFlashShape& Shape = FlashShapeList[Index];
		// The engine's sphere is 100 across.
		FlashShapes[Index]->SetRelativeTransform(FTransform(Turn, Shape.Offset * Size, Shape.Size * Size / 100.0));
		FlashShapes[Index]->SetVisibility(FlashMaterials[Index] != nullptr);
	}
	FlashLight->SetVisibility(true);
	FlashAge = 0.0f;
	FlashExposureScale = FScctExposure::FlashScale();
	// A wall close in front of the gun takes the light's full strength in one spot: held down so it is warmed,
	// not burned white, whatever the exposure.
	const FVector LightAt = FlashLightPlace();
	float Nearest = 0.0f;
	FlashWallHold = FScctExposure::WallHold(LightAt, FScctExposure::MuzzleFlashCandelas() * FlashExposureScale,
		FlashLight->AttenuationRadius, &Nearest);
	FScctExposure::NoteFlash();
	++FlashesPlayed;
	UE_LOG(LogOpenSCCT, Log, TEXT("First person: muzzle flash %d at (%.0f %.0f %.0f), %.1f cm from the eye, its light %.0f cd (exposure %.3f, settled %.3f, scale %.2f, wall hold %.2f, nearest surface %.0f cm, picture level %.3f) %.1f cm from the eye"), FlashesPlayed,
		Muzzle.GetLocation().X, Muzzle.GetLocation().Y, Muzzle.GetLocation().Z, FVector::Dist(Muzzle.GetLocation(), LastCamera.GetLocation()),
		FScctExposure::MuzzleFlashCandelas() * FlashExposureScale * FlashWallHold, FScctExposure::Get(), FScctExposure::FlashExposure(), FlashExposureScale, FlashWallHold, Nearest,
		FScctExposure::PictureLevel(), FVector::Dist(LightAt, LastCamera.GetLocation()));
	TickFlash(0.0f);
}

FVector AScctFirstPersonView::FlashLightPlace() const
{
	// The light is out in the flash's plume, FlashLightAhead in front of the muzzle. The view model is drawn pulled
	// in towards the eye (to 0.42 of its distance), so from there the arms as drawn are most of a metre away and
	// take a little of its light, not all of it: from a place 60% of the way to the muzzle, a hand's width from
	// the drawn sleeves, it burned them white. Only the level's own geometry between the eye and that place holds
	// it back, 10 cm in front of the wall, so it never lights the far side of a wall instead of this one. The
	// simulation's collision is asked (FScctExposure::TraceLevel): Unreal's copy of the level has none, and a trace
	// of it never met the wall.
	const FVector Eye = LastCamera.GetLocation();
	const FVector At = ProjectSocket(Muzzle.GetLocation() + Muzzle.GetRotation().GetForwardVector() * FlashLightAhead, 1.0f);
	float Distance = 0.0f;
	if (FScctExposure::TraceLevel(Eye, At, Distance))
	{
		return Eye + (At - Eye).GetSafeNormal() * FMath::Max(Distance - 10.0, 0.0);
	}
	return At;
}

void AScctFirstPersonView::TickFlash(float DeltaSeconds)
{
	if (FlashAge < 0.0f)
	{
		return;
	}

	FlashAge += DeltaSeconds;
	// -ScctFlashHold=S keeps every flash at full strength for S seconds first, so a screenshot can catch one.
	static const float Hold = []
	{
		float Seconds = 0.0f;
		FParse::Value(FCommandLine::Get(), TEXT("ScctFlashHold="), Seconds);
		return FMath::Max(Seconds, 0.0f);
	}();
	const float Age = FMath::Max(FlashAge - Hold, 0.0f);
	const bool bGunShown = bShown && bHasMuzzle && !bMuzzleHidden;
	const float Fade = bGunShown ? 1.0f - FMath::Square(FMath::Clamp(Age / FlashSeconds, 0.0f, 1.0f)) : 0.0f;
	if (bGunShown)
	{
		FlashRoot->SetWorldLocationAndRotation(Muzzle.GetLocation(), Muzzle.GetRotation());
		FlashLight->SetWorldLocation(FlashLightPlace());
	}
	for (int32 Index = 0; Index < FlashShapes.Num(); ++Index)
	{
		if (FlashMaterials.IsValidIndex(Index) && FlashMaterials[Index] != nullptr)
		{
			FlashMaterials[Index]->SetScalarParameterValue(TEXT("Fade"), Fade);
		}
		if (Fade <= 0.0f && FlashShapes[Index]->IsVisible())
		{
			FlashShapes[Index]->SetVisibility(false);
		}
	}

	const float LightFade = bGunShown ? 1.0f - FMath::Clamp(Age / FlashLightSeconds, 0.0f, 1.0f) : 0.0f;
	FlashLight->SetIntensity(FScctExposure::MuzzleFlashCandelas() * FlashExposureScale * FlashWallHold * LightFade);
	if (LightFade <= 0.0f)
	{
		FlashLight->SetVisibility(false);
		FlashAge = -1.0f;
	}
}
