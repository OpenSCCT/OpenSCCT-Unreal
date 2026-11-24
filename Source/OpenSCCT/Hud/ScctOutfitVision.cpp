#include "Hud/ScctOutfitVision.h"

#include "Components/PoseableMeshComponent.h"
#include "Components/SkeletalMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/Texture2D.h"
#include "Engine/World.h"
#include "GameFramework/Actor.h"
#include "Hud/ScctVisionSubsystem.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Play/ScctPawnLookSubsystem.h"
#include "Play/ScctRuntimeSkins.h"
#include "Misc/Paths.h"
#include "Runtime/ScctFeatures.h"

namespace
{
	// Custom stencil is per primitive. Disjoint UV masks write different heat/EMF levels
	// while keeping the existing wall-depth, cloak opacity and carried-equipment rules.
	constexpr int32 OutfitBandSteps = 7;
}

bool FScctOutfitVision::Mark(UPoseableMeshComponent& Body, int32 Kind, bool bWet, bool bLocalSpy, bool bHeat,
	float Intensity, float Opacity)
{
	const FScctOutfitArmsApi* Api = ScctFeature<FScctOutfitArmsApi>();
	USkinnedAsset* Mesh = Body.GetSkinnedAsset();
	if (!Api || !Mesh || (bHeat && Intensity <= 0.f) || !Body.ShouldRender() || !Body.GetWorld()) return false;
	FBody& State = Bodies.FindOrAdd(&Body);
	State.bUsed = true;
	const int32 Skin = Kind * 4 + (bWet ? 2 : 0) + (bLocalSpy ? 1 : 0);
	if (State.Mesh.Get() != Mesh || State.Skin != Skin)
	{
		if (State.Actor.IsValid()) State.Actor->Destroy();
		State = FBody{};
		State.bUsed = true;
		State.Mesh = Mesh;
		State.Skin = Skin;
		TArray<UTexture2D*> Masks;
		bool bHasMask = false;
		const FString Outfit = UScctPawnLookSubsystem::OutfitModelPath(Kind);
		const bool bWearsModel = !Outfit.IsEmpty() && FPaths::IsSamePath(ScctRuntimeSkins::ContentFileOf(Mesh), Outfit);
		for (const FSkeletalMaterial& Slot : Mesh->GetMaterials())
		{
			const FString Name = Slot.MaterialInterface ? Slot.MaterialInterface->GetName() : Slot.MaterialSlotName.ToString();
			TCHAR Buffer[1024];
			int32 N = Api->VisionTexture(Kind, bWearsModel, *Name, Name.Len(), bWet, bLocalSpy, Buffer, UE_ARRAY_COUNT(Buffer));
			if (N == 0 && !Slot.MaterialSlotName.IsNone())
			{
				const FString SlotName = Slot.MaterialSlotName.ToString();
				if (SlotName != Name)
					N = Api->VisionTexture(Kind, bWearsModel, *SlotName, SlotName.Len(), bWet, bLocalSpy, Buffer, UE_ARRAY_COUNT(Buffer));
			}
			UTexture2D* Mask = nullptr;
			if (N > 0)
			{
				const FString Path(N, Buffer);
				TWeakObjectPtr<UTexture2D>& Cached = Textures.FindOrAdd(Path);
				if (!Cached.IsValid())
				{
					Mask = FImageUtils::ImportFileAsTexture2D(Path);
					if (Mask)
					{
						Mask->SRGB = false;
						Mask->UpdateResource();
						Cached = Mask;
					}
				}
				Mask = Cached.Get();
			}
			Masks.Add(Mask);
			bHasMask |= Mask != nullptr;
		}
		if (!bHasMask) return false;
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, TEXT("/Game/OpenSCCT/Materials/M_ScctVisionBand.M_ScctVisionBand"));
		if (!Parent) return false;
		FActorSpawnParameters Spawn;
		Spawn.ObjectFlags |= RF_Transient;
		State.Actor = Body.GetWorld()->SpawnActor<AActor>(Spawn);
		if (!State.Actor.IsValid()) return false;
		for (int32 Band = 0; Band <= OutfitBandSteps; ++Band)
		{
			auto* Part = NewObject<USkeletalMeshComponent>(State.Actor.Get());
			Part->SetSkinnedAssetAndUpdate(Mesh);
			Part->SetLeaderPoseComponent(&Body);
			Part->SetCollisionEnabled(ECollisionEnabled::NoCollision);
			Part->SetGenerateOverlapEvents(false);
			Part->SetCastShadow(false);
			Part->SetRenderInMainPass(false);
			Part->SetRenderInDepthPass(false);
			Part->SetVisibleInRayTracing(false);
			Part->SetRenderCustomDepth(true);
			Part->bUseBoundsFromLeaderPoseComponent = true;
			for (int32 Slot = 0; Slot < Masks.Num(); ++Slot)
			{
				auto* Mat = UMaterialInstanceDynamic::Create(Parent, Part);
				if (Masks[Slot])
				{
					Mat->SetTextureParameterValue(TEXT("VisionMask"), Masks[Slot]);
					Mat->SetScalarParameterValue(TEXT("HasMask"), 1.f);
				}
				Mat->SetScalarParameterValue(TEXT("Band"), static_cast<float>(Band));
				Part->SetMaterial(Slot, Mat);
				State.Materials.Add(Mat);
			}
			Part->SetWorldTransform(Body.GetComponentTransform());
			Part->RegisterComponent();
			State.Bands.Add(Part);
		}
	}
	if (State.Bands.IsEmpty()) return false;
	const bool bModeChanged = State.bHeat != bHeat;
	if (State.Intensity != Intensity || bModeChanged)
	{
		// Heat scales the suit texture by the actor's intensity. EMF inverts the finished
		// image: its brightness is 1 - texture * (1 - glow), even on a zero-glow spy.
		const float MaskScale = bHeat ? Intensity : 1.f - FMath::Clamp(Intensity, 0.f, 1.f);
		for (auto& Weak : State.Materials)
		{
			if (auto* Mat = Weak.Get())
			{
				Mat->SetScalarParameterValue(TEXT("Heat"), bHeat ? 1.f : 0.f);
				Mat->SetScalarParameterValue(TEXT("Intensity"), MaskScale);
			}
		}
		State.Intensity = Intensity;
		State.bHeat = bHeat;
	}
	for (int32 Band = 0; Band < State.Bands.Num(); ++Band)
	{
		if (auto* Part = State.Bands[Band].Get())
		{
			Part->SetWorldTransform(Body.GetComponentTransform());
			if (State.Opacity != Opacity || bModeChanged)
			{
				const int32 Level = bHeat ? Band : OutfitBandSteps - Band;
				Part->SetCustomDepthStencilValue(UScctVisionSubsystem::PawnStencil(static_cast<float>(Level) / OutfitBandSteps, Opacity));
			}
		}
	}
	State.Opacity = Opacity;
	return true;
}

void FScctOutfitVision::EndFrame()
{
	for (auto It = Bodies.CreateIterator(); It; ++It)
	{
		FBody& Entry = It.Value();
		if (!Entry.bUsed || !It.Key().IsValid())
		{
			if (Entry.Actor.IsValid()) Entry.Actor->Destroy();
			It.RemoveCurrent();
		}
		else Entry.bUsed = false;
	}
}

void FScctOutfitVision::Reset()
{
	for (auto& Pair : Bodies)
		if (Pair.Value.Actor.IsValid()) Pair.Value.Actor->Destroy();
	Bodies.Reset();
	Textures.Reset();
}
