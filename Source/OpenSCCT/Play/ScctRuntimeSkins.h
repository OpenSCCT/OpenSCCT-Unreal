#pragma once

#include "CoreMinimal.h"

class USkeletalMesh;
class UStaticMesh;
class UMaterialInterface;
class USkinnedAsset;
class UAnimSequence;
class UObject;

/**
 * Characters, weapons and effect meshes: the release's cooked /Game/Imported assets when it has them, otherwise
 * the converted GLBs of the active Steam build or a selected map, read at run time.
 */
namespace ScctRuntimeSkins
{
	USkeletalMesh* Load(const FString& File);
	UStaticMesh* LoadEffectMesh(const FString& ObjectPath);
	void LoadEffectMeshes(TArray<UStaticMesh*>& Out);
	/** The GLB a run-time mesh was read from; empty for a cooked asset. */
	FString SourceOf(const USkinnedAsset* Mesh);
	/** The content file a mesh stands for, cooked or read at run time ("<root>/shared/chars/<pkg>/<mesh>.glb"). */
	FString ContentFileOf(const USkinnedAsset* Mesh);
	/** The asset lives in a cooked /Game package rather than one built at run time. */
	bool IsCooked(const UObject* Asset);
	UMaterialInterface* LoadEquipmentSkin(const FString& Package, const FString& Texture);
	UAnimSequence* LoadAnimation(USkeletalMesh* Mesh, const FString& Name);
}
