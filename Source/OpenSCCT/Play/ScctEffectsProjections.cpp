// The abandoned projections of the client's world: UScctEffectsSubsystem::UpdateProjections and its helpers.
//
// A projector the game abandons (Projector.AbandonProjector) stays drawn for a while without its actor: the
// footprints of dirty feet (20 s), the burn mark a flare leaves where it caught (30 s) and the shotgun's pellet
// projectors (10 s; this port's shotgun leaves ordinary impact marks instead, so none come today). The bridge places
// each one once on the surface along its projector's axis (Runtime/ScctProjectionsApi.h, the Godot client's
// CombatFxView.TrackProjections); here each is a decal of the original texture, taken from a small pool, riding the
// surface it lies on as an impact mark does and fading out over the end of its life.

#include "Components/DecalComponent.h"
#include "Engine/Texture2D.h"
#include "ImageUtils.h"
#include "Misc/Paths.h"
#include "Misc/PackageName.h"
#include "GameFramework/Actor.h"
#include "Hud/ScctVisionSubsystem.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Materials/MaterialInterface.h"
#include "OpenSCCT.h"
#include "Play/ScctEffectsSubsystem.h"
#include "Runtime/ScctFeatures.h"

namespace
{
	/** Projections drawn at once. Footprints are the many: four players walking leave about 40 in 20 s. */
	constexpr int32 MaxProjections = 128;

	/** How deep a projection reaches into and out of its surface, cm: a floor's bumps, not the step below it. */
	constexpr float ProjectionDepth = 6.0f;

	/** An opacity change smaller than this is not handed to the material. */
	constexpr float OpacityStep = 0.01f;

	const TCHAR* const ProjectionMaterialPath = TEXT("/Game/OpenSCCT/Effects/M_ScctFxProjection.M_ScctFxProjection");
}

UTexture2D* UScctEffectsSubsystem::FindProjectionTexture(int32 AssetId)
{
	if (const TObjectPtr<UTexture2D>* Found = ProjectionTextures.Find(AssetId))
	{
		return *Found;
	}
	const FString& Name = GetName(AssetId);
	const FString Root = FOpenSCCTModule::FindContentRoot();
	UTexture2D* Texture = nullptr;
	// The release's cooked copy (Tools/Effects/Build-EffectAssets.ps1) first, the Steam conversion's picture after.
	const FString Cooked = FString::Printf(TEXT("/Game/Imported/Fx/Textures/%s"), *Name);
	if (!Name.IsEmpty() && FPackageName::IsValidLongPackageName(Cooked) && FPackageName::DoesPackageExist(Cooked))
	{
		Texture = LoadObject<UTexture2D>(nullptr, *FString::Printf(TEXT("%s.%s"), *Cooked, *Name), nullptr, LOAD_NoWarn | LOAD_Quiet);
	}
	if (!Texture && !Root.IsEmpty() && !Name.IsEmpty() && Name == FPaths::GetCleanFilename(Name))
	{
		const FString Folder = Root / TEXT("shared/fx/textures");
		const FString File = FPaths::ConvertRelativePathToFull(Folder, Name + TEXT(".png"));
		if (FPaths::IsUnderDirectory(File, Folder) && FPaths::FileExists(File))
		{
			Texture = FImageUtils::ImportFileAsTexture2D(File);
			if (Texture)
			{
				Texture->AddressX = Texture->AddressY = TA_Clamp;
				Texture->UpdateResource();
			}
		}
	}
	if (!Texture) UE_LOG(LogOpenSCCT, Warning, TEXT("Effects: local projection texture '%s' is missing"), *Name);
	ProjectionTextures.Add(AssetId, Texture);
	return Texture;
}

void UScctEffectsSubsystem::ResetProjections()
{
	ProjectionCount = 0;
	if (NumProjections > 0 || NumProjectionsUndrawn > 0)
	{
		UE_LOG(LogOpenSCCT, Log, TEXT("Effects: %d abandoned projections drawn this match, %d not drawn (no texture or no free decal)"), NumProjections, NumProjectionsUndrawn);
	}
	for (UDecalComponent* Decal : ProjectionDecals)
	{
		if (Decal != nullptr)
		{
			Decal->SetVisibility(false);
		}
	}
	for (FProjectionSlot& Slot : ProjectionSlots)
	{
		Slot = FProjectionSlot();
	}
	ProjectionBySerial.Reset();
	NumProjections = NumProjectionsUndrawn = 0;
}

void UScctEffectsSubsystem::UpdateProjections()
{
	if (Projections == nullptr)
	{
		FString Error;
		Projections = ScctFeature<FScctProjectionsApi>(&Error);
		if (Projections == nullptr)
		{
			if (!bProjectionsReported)
			{
				bProjectionsReported = true;
				UE_LOG(LogOpenSCCT, Warning, TEXT("Effects: no footprints, flare burns or pellet projections, the bridge's projections are missing: %s"), *Error);
			}
			return;
		}
	}
	if (ProjectionMaterial == nullptr)
	{
		ProjectionMaterial = LoadObject<UMaterialInterface>(nullptr, ProjectionMaterialPath);
		if (ProjectionMaterial == nullptr)
		{
			if (!bProjectionsReported)
			{
				bProjectionsReported = true;
				UE_LOG(LogOpenSCCT, Warning, TEXT("Effects: no footprints, flare burns or pellet projections: %s is missing (run Tools/Effects/Build-EffectAssets.ps1)"), ProjectionMaterialPath);
			}
			return;
		}
		ProjectionDecals.SetNum(MaxProjections);
		ProjectionLooks.SetNum(MaxProjections);
		ProjectionSlots.SetNum(MaxProjections);
		ProjectionBuffer.SetNumUninitialized(MaxProjections);
	}

	const UScctVisionSubsystem* ProjectionVision = UScctVisionSubsystem::Find(GetWorld());
	const bool bHeatOrEmf = ProjectionVision != nullptr && ProjectionVision->IsHeatOrEmf();
	ProjectionCount = Projections->GetProjections(ProjectionBuffer.GetData(), ProjectionBuffer.Num());
	for (int32 Index = 0; Index < ProjectionCount; ++Index)
	{
		const FScctProjection& Record = ProjectionBuffer[Index];
		int32 SlotIndex = INDEX_NONE;
		if (const int32* Found = ProjectionBySerial.Find(Record.Serial))
		{
			SlotIndex = *Found;
		}
		else
		{
			SlotIndex = ProjectionSlots.IndexOfByPredicate([](const FProjectionSlot& Slot) { return Slot.Serial == 0; });
			UTexture2D* Texture = FindProjectionTexture(Record.AssetId);
			if (SlotIndex == INDEX_NONE || Texture == nullptr)
			{
				// Remembered as undrawn, so it is counted once and not looked at again.
				ProjectionBySerial.Add(Record.Serial, INDEX_NONE);
				++NumProjectionsUndrawn;
				continue;
			}
			ProjectionBySerial.Add(Record.Serial, SlotIndex);

			TObjectPtr<UDecalComponent>& Decal = ProjectionDecals[SlotIndex];
			if (Decal == nullptr)
			{
				Decal = NewObject<UDecalComponent>(Host);
				Decal->SetDecalMaterial(ProjectionMaterial);
				Decal->SetupAttachment(Host->GetRootComponent());
				Decal->RegisterComponent();
				Decal->SetFadeScreenSize(0.0005f);
				// Over the bullet marks: a footprint on a shot floor is printed on top of the holes.
				Decal->SortOrder = 1;
				ProjectionLooks[SlotIndex] = Decal->CreateDynamicMaterialInstance();
			}
			UMaterialInstanceDynamic* Look = ProjectionLooks[SlotIndex];
			const bool bTinted = (Record.Flags & EScctProjectionFlags::Tinted) != 0;
			Look->SetTextureParameterValue(TEXT("Texture"), Texture);
			// The tint is an sRGB colour (the game's ConstantColor); the material works in linear colour.
			const FLinearColor Tint = FLinearColor::FromSRGBColor(FColor(
				FMath::RoundToInt(FMath::Clamp(Record.TintR, 0.0f, 1.0f) * 255.0f), FMath::RoundToInt(FMath::Clamp(Record.TintG, 0.0f, 1.0f) * 255.0f),
				FMath::RoundToInt(FMath::Clamp(Record.TintB, 0.0f, 1.0f) * 255.0f)));
			Look->SetVectorParameterValue(TEXT("Tint"), Tint);
			Look->SetScalarParameterValue(TEXT("TintAlpha"), FMath::Clamp(Record.TintA, 0.0f, 1.0f));
			Look->SetScalarParameterValue(TEXT("UseTint"), bTinted ? 1.0f : 0.0f);
			Look->SetScalarParameterValue(TEXT("FlipV"), (Record.Flags & EScctProjectionFlags::FlipV) != 0 ? 1.0f : 0.0f);

			// A decal projects along its own X axis, into the surface, and lays the texture over its Y (U) and
			// Z (V) axes: U runs the way the bridge says (along the walk for a footprint).
			const FVector Normal = FVector(Record.NormalX, Record.NormalY, Record.NormalZ).GetSafeNormal(UE_SMALL_NUMBER, FVector::UpVector);
			const FVector U(Record.UX, Record.UY, Record.UZ);
			const FRotator Facing = FRotationMatrix::MakeFromXY(-Normal, U).Rotator();
			Decal->DecalSize = FVector(ProjectionDepth, FMath::Max(Record.Width * 0.5f, 0.5f), FMath::Max(Record.Height * 0.5f, 0.5f));
			Decal->SetWorldLocationAndRotation(FVector(Record.X, Record.Y, Record.Z), Facing);

			FProjectionSlot& Slot = ProjectionSlots[SlotIndex];
			Slot = FProjectionSlot();
			Slot.Serial = Record.Serial;
			AttachToSurface(Slot.Surface, Record.SurfaceActorId, *Decal, TEXT("projection"), Record.Serial);
			++NumProjections;
			if (bLog)
			{
				UE_LOG(LogOpenSCCT, Log, TEXT("Effects: projection start %s life %.1f s (#%d '%s', %.0f x %.0f cm at (%.0f, %.0f, %.0f) facing (%.2f, %.2f, %.2f), %s, surface #%d)"),
					*GetName(Record.TextureId), Record.Life, Record.Serial, *GetName(Record.ClassId), Record.Width, Record.Height, Record.X, Record.Y, Record.Z,
					Normal.X, Normal.Y, Normal.Z, bTinted ? *FString::Printf(TEXT("tint (%.2f, %.2f, %.2f, %.2f)"), Record.TintR, Record.TintG, Record.TintB, Record.TintA) : TEXT("its own colours"),
					Record.SurfaceActorId);
			}
		}
		if (SlotIndex == INDEX_NONE)
		{
			continue;
		}

		FProjectionSlot& Slot = ProjectionSlots[SlotIndex];
		UDecalComponent* Decal = ProjectionDecals[SlotIndex];
		Slot.SeenFrame = Frame;
		if (FMath::Abs(Record.Opacity - Slot.Opacity) >= OpacityStep || (Record.Opacity <= 0.0f && Slot.Opacity > 0.0f))
		{
			Slot.Opacity = Record.Opacity;
			ProjectionLooks[SlotIndex]->SetScalarParameterValue(TEXT("Opacity"), Record.Opacity);
		}
		// Thermal and EMF read the frame as heat and field strength: a footprint or a burn mark has neither.
		const bool bDrawn = FollowSurface(Slot.Surface, *Decal, TEXT("projection"), Record.Serial) && Record.Opacity > 0.0f && !bHeatOrEmf;
		if (Decal->IsVisible() != bDrawn)
		{
			Decal->SetVisibility(bDrawn);
		}
	}

	// A projection no longer listed has run its time.
	for (int32 SlotIndex = 0; SlotIndex < ProjectionSlots.Num(); ++SlotIndex)
	{
		FProjectionSlot& Slot = ProjectionSlots[SlotIndex];
		if (Slot.Serial == 0 || Slot.SeenFrame == Frame)
		{
			continue;
		}
		if (bLog)
		{
			UE_LOG(LogOpenSCCT, Log, TEXT("Effects: projection end #%d"), Slot.Serial);
		}
		ProjectionDecals[SlotIndex]->SetVisibility(false);
		ProjectionBySerial.Remove(Slot.Serial);
		Slot = FProjectionSlot();
	}
	// Undrawn serials are forgotten once the bridge no longer lists them.
	if (ProjectionBySerial.Num() > ProjectionCount)
	{
		TSet<int32> Listed;
		for (int32 Index = 0; Index < ProjectionCount; ++Index)
		{
			Listed.Add(ProjectionBuffer[Index].Serial);
		}
		for (auto It = ProjectionBySerial.CreateIterator(); It; ++It)
		{
			if (It.Value() == INDEX_NONE && !Listed.Contains(It.Key()))
			{
				It.RemoveCurrent();
			}
		}
	}
}
