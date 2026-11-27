#include "Play/ScctFirstPersonView.h"

#include "Camera/CameraComponent.h"
#include "Components/SceneCaptureComponent2D.h"
#include "Components/StaticMeshComponent.h"
#include "Engine/StaticMesh.h"
#include "Engine/TextureRenderTarget2D.h"
#include "Engine/World.h"
#include "GameFramework/PlayerController.h"
#include "Materials/MaterialInstanceDynamic.h"

void AScctFirstPersonView::SetScopePhysicalCoating(float Weight)
{
	UStaticMeshComponent* Optic = BoundScope.Get();
	if (!Optic || BoundCoatingSlot == INDEX_NONE || !ScopeImage) return;
	UMaterialInterface* Coating = nullptr;
	if (Weight <= UE_SMALL_NUMBER)
	{
		// The captured image includes its own surface reflection. A second refractive draw
		// would sample the depth of the tube behind it and expose a hard circle in the glass.
		Coating = ScopeImage->Parent; // ScopeBlend defaults to zero on this material.
	}
	else
	{
		UMaterialInterface* Base = BoundScopeMesh->GetMaterial(BoundCoatingSlot);
		if (!Base) return;
		if (!ScopeCoating || ScopeCoating->Parent != Base)
		{
			ScopeCoating = UMaterialInstanceDynamic::Create(Base, this);
		}
		ScopeCoating->SetScalarParameterValue(TEXT("ScopeGlassWeight"), Weight);
		Coating = ScopeCoating;
	}
	if (Optic->GetMaterial(BoundCoatingSlot) != Coating) Optic->SetMaterial(BoundCoatingSlot, Coating);
}

void AScctFirstPersonView::UpdatePrecisionImage(UStaticMeshComponent* Optic, const UCameraComponent* Camera, float Power, bool bHasEtchedReticle)
{
	static const FName ImageSlot(TEXT("M_ScctPrecisionImage"));
	static const FName CoatingSlot(TEXT("M_ScctAcogGlass"));
	static const FName Ocular(TEXT("Ocular"));
	static const FName Edge(TEXT("ApertureEdge"));
	static const FName Objective(TEXT("Objective"));
	static const FName Scene(TEXT("ScopeScene"));
	static const FName Blend(TEXT("ScopeBlend"));
	static const FName EyeBox(TEXT("ScopeEyeBox"));
	static const FName Dot(TEXT("ScopeDot"));
	UStaticMesh* Mesh = Optic ? Optic->GetStaticMesh() : nullptr;
	const int32 Slot = Mesh && Camera && Power > 1.0f ? Optic->GetMaterialIndex(ImageSlot) : INDEX_NONE;
	if (Slot == INDEX_NONE || BoundScope.Get() != Optic || BoundScopeMesh.Get() != Mesh || BoundScopeSlot != Slot)
	{
		if (UStaticMeshComponent* Previous = BoundScope.Get())
		{
			if (Previous->GetMaterial(BoundScopeSlot) == ScopeImage)
			{
				Previous->SetMaterial(BoundScopeSlot, nullptr);
			}
			if (BoundCoatingSlot != INDEX_NONE)
			{
				UMaterialInterface* Current = Previous->GetMaterial(BoundCoatingSlot);
				if (Current == ScopeCoating || (ScopeImage && Current == ScopeImage->Parent))
				{
					Previous->SetMaterial(BoundCoatingSlot, nullptr);
				}
			}
		}
		BoundScope.Reset();
		BoundScopeMesh.Reset();
		BoundScopeSlot = INDEX_NONE;
		BoundCoatingSlot = INDEX_NONE;
		bScopeCapturing = false;
		if (Slot == INDEX_NONE) return;
	}

	const FTransform Glass = Optic->GetSocketTransform(Ocular);
	const FVector Eye = Camera->GetComponentLocation();
	const FVector ToEye = Glass.GetRotation().UnrotateVector(Eye - Glass.GetLocation());
	const float Radius = FVector::Distance(Glass.GetLocation(), Optic->GetSocketLocation(Edge));
	const float Length = FVector::Distance(Glass.GetLocation(), Optic->GetSocketLocation(Objective));
	const float Relief = -ToEye.X;
	const float Depth = FVector::DotProduct(Glass.GetLocation() - Eye, Camera->GetForwardVector());
	// Project the eye offset along the tube to the rear glass. The overlapping pupil
	// moves across the eyepiece as the player brings it up to the camera.
	const FVector2D Pupil = FVector2D(ToEye.Y, ToEye.Z) * (Length / FMath::Max(1.f, Length + Relief))
		/ FMath::Max(.01f, Radius);
	int32 Width = 0, Height = 0;
	if (const APlayerController* Player = GetWorld()->GetFirstPlayerController())
	{
		Player->GetViewportSize(Width, Height);
	}
	const float Offset = Pupil.Size();
	const float ViewHalfTan = FMath::Tan(FMath::DegreesToRadians(Camera->FieldOfView * .5f));
	const float PixelsPerRadius = .5f * Width * Radius * SightFocalScale / FMath::Max(.01f, Depth * ViewHalfTan);
	// A carried pistol can expose a fraction of one pixel. Hand off to the physical
	// glass over the last two pixels before skipping the extra scene render.
	const float SubpixelCoverage = FMath::SmoothStep(1.f, 3.f, (2.f - Offset) * PixelsPerRadius);
	if (Relief <= 0.f || Depth <= 1.f || Radius <= .01f || Length <= .01f || SubpixelCoverage <= 0.f)
	{
		if (bScopeCapturing)
		{
			ScopeImage->SetScalarParameterValue(Blend, 0.f);
			SetScopePhysicalCoating(1.f);
		}
		bScopeCapturing = false;
		return;
	}
	if (!ScopeCapture)
	{
		ScopeTarget = NewObject<UTextureRenderTarget2D>(this, NAME_None, RF_Transient);
		ScopeTarget->RenderTargetFormat = RTF_RGBA16f;
		ScopeTarget->Filter = TF_Bilinear;
		ScopeTarget->ClearColor = FLinearColor::Black;
		ScopeCapture = NewObject<USceneCaptureComponent2D>(this, TEXT("PrecisionSight"), RF_Transient);
		ScopeCapture->bCaptureEveryFrame = false;
		ScopeCapture->bCaptureOnMovement = false;
		ScopeCapture->bAlwaysPersistRenderingState = true;
		// Scene captures otherwise disable the map's Lumen GI and reflections. Share
		// the local view's lighting settings while keeping the scope's own projection.
		ScopeCapture->bMainViewFamily = true;
		ScopeCapture->bInheritMainViewCameraPostProcessSettings = true;
		// The map uses hardware-traced lighting when it is available. Without the
		// capture opt-in, even unlit corners can receive unobstructed room light here.
		ScopeCapture->bUseRayTracingIfEnabled = true;
		ScopeCapture->PostProcessBlendWeight = 0.f;
		ScopeCapture->CaptureSource = ESceneCaptureSource::SCS_SceneColorHDRNoAlpha;
		ScopeCapture->TextureTarget = ScopeTarget;
		ScopeCapture->HiddenActors.Add(this);
		ScopeCapture->ShowFlags.SetMotionBlur(false);
		ScopeCapture->ShowFlags.SetDepthOfField(false);
		ScopeCapture->ShowFlags.SetLensFlares(false);
		// Scene color is sampled as linear radiance in the glass material. Apply exposure
		// once in the main view, including when either side of the lens is in deep shade.
		ScopeCapture->ShowFlags.SetEyeAdaptation(false);
		ScopeCapture->RegisterComponent();
		AddInstanceComponent(ScopeCapture);
	}
	const int32 Size = FMath::Clamp(FMath::DivideAndRoundUp(FMath::RoundToInt(Height * 0.46f), 64) * 64, 384, 1024);
	if (ScopeTarget->SizeX != Size || ScopeTarget->SizeY != Size)
	{
		ScopeTarget->InitAutoFormat(Size, Size);
		bScopeCapturing = false;
	}
	if (!BoundScope.IsValid())
	{
		UMaterialInterface* Base = Mesh->GetMaterial(Slot);
		if (!Base) return;
		if (!ScopeImage || ScopeImage->Parent != Base)
		{
			ScopeImage = UMaterialInstanceDynamic::Create(Base, this);
			ScopeImage->SetTextureParameterValue(Scene, ScopeTarget);
		}
		ScopeImage->SetScalarParameterValue(Dot, bHasEtchedReticle ? 0.f : 1.f);
		Optic->SetMaterial(Slot, ScopeImage);
		BoundScope = Optic;
		BoundScopeMesh = Mesh;
		BoundScopeSlot = Slot;
		BoundCoatingSlot = Optic->GetMaterialIndex(CoatingSlot);
	}
	// The main camera sees this diameter at the first-person focal length. Divide its
	// angular size by the current optical power to get the square capture's field of view.
	const float HalfTan = Radius * SightFocalScale / (FMath::Max(Depth, 1.0f) * Power);
	ScopeCapture->FOVAngle = FMath::Clamp(FMath::RadiansToDegrees(2.0f * FMath::Atan(HalfTan)), .1f, 120.0f);
	// Shots leave along the view, not along the optic: the posed rifle tilts the optic's axis a little, and through
	// the magnification that put the impacts well below the reticle. Look along the camera, so the reticle at the
	// picture's centre is where the shots land (a zeroed sight).
	ScopeCapture->SetWorldLocationAndRotation(Eye, FRotationMatrix::MakeFromXZ(Camera->GetForwardVector(), Camera->GetUpVector()).Rotator());
	ScopeCapture->bCameraCutThisFrame = !bScopeCapturing;
	ScopeCapture->CaptureSceneDeferred();
	ScopeImage->SetVectorParameterValue(EyeBox, FLinearColor(Pupil.X, Pupil.Y, 0.f, 0.f));
	ScopeImage->SetScalarParameterValue(Blend, SubpixelCoverage);
	SetScopePhysicalCoating(FMath::Lerp(1.f, FMath::SmoothStep(1.f, 2.f, Offset), SubpixelCoverage));
	bScopeCapturing = true;
}
