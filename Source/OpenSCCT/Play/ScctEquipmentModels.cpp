#include "Play/ScctEquipmentModels.h"
#include "Play/ScctRuntimeSkins.h"

#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Components/PoseableMeshComponent.h"
#include "Engine/SkeletalMesh.h"
#include "Materials/MaterialInterface.h"
#include "Misc/PackageName.h"
#include "Misc/Paths.h"
#include "OpenSCCT.h"

namespace
{
	constexpr int32 MaxEquipmentBones = 160;

	/** Bone names compared by letters and digits only: importers differ in how they treat spaces and dots. */
	FString BoneKey(const TCHAR* Name)
	{
		FString Key;
		for (const TCHAR* Character = Name; *Character != 0; ++Character)
		{
			if (FChar::IsAlnum(*Character))
			{
				Key.AppendChar(FChar::ToLower(*Character));
			}
		}
		return Key;
	}
}

int32 FScctEquipmentModel::MeshBoneByName(const TCHAR* BridgeName) const
{
	const FString Wanted = BoneKey(BridgeName);
	for (int32 Index = 0; Index < Skeleton.Num(); ++Index)
	{
		if (BoneKey(Skeleton[Index].Name) == Wanted)
		{
			return BoneMap[Index];
		}
	}
	return INDEX_NONE;
}

void FScctEquipmentModel::ApplyPose(UPoseableMeshComponent& Component, TConstArrayView<FScctBridgeBone> Pose) const
{
	TArray<FTransform>& Local = Component.BoneSpaceTransforms;
	const int32 Count = FMath::Min(Pose.Num(), BoneMap.Num());
	if (Count == 0 || Local.Num() != BridgeBoneOfMeshBone.Num())
	{
		return;
	}

	if (bDirect)
	{
		for (int32 Index = 0; Index < Count; ++Index)
		{
			Local[BoneMap[Index]] = FScctPose::ToUnreal(Pose[Index]);
		}
		Component.RefreshBoneTransforms();
		return;
	}

	// The two skeletons differ, so the pose goes through component space: where every bridge bone ends up, then
	// each mesh bone relative to its own parent. A mesh bone the bridge does not know keeps its rest place under
	// its parent. The bridge's parents can come after their children, hence the walk up instead of one pass.
	TArray<FTransform, TInlineAllocator<MaxEquipmentBones>> BridgeSpace;
	TArray<bool, TInlineAllocator<MaxEquipmentBones>> Done;
	BridgeSpace.SetNum(Count);
	Done.Init(false, Count);
	TArray<int32, TInlineAllocator<32>> Chain;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Chain.Reset();
		for (int32 Bone = Index; Bone >= 0 && Bone < Count && !Done[Bone] && Chain.Num() < Count; Bone = Skeleton[Bone].Parent)
		{
			Chain.Add(Bone);
		}
		for (int32 At = Chain.Num() - 1; At >= 0; --At)
		{
			const int32 Bone = Chain[At];
			const int32 Parent = Skeleton[Bone].Parent;
			const FTransform Own = FScctPose::ToUnreal(Pose[Bone]);
			BridgeSpace[Bone] = Parent >= 0 && Parent < Count ? Own * BridgeSpace[Parent] : Own;
			Done[Bone] = true;
		}
	}

	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	TArray<FTransform, TInlineAllocator<MaxEquipmentBones>> MeshSpace;
	MeshSpace.SetNum(Local.Num());
	for (int32 Bone = 0; Bone < Local.Num(); ++Bone)
	{
		const int32 Parent = Reference.GetParentIndex(Bone);
		const int32 BridgeBone = BridgeBoneOfMeshBone[Bone];
		if (BridgeBone >= 0 && BridgeBone < Count)
		{
			MeshSpace[Bone] = BridgeSpace[BridgeBone];
		}
		else
		{
			MeshSpace[Bone] = Parent >= 0 ? Reference.GetRefBonePose()[Bone] * MeshSpace[Parent] : Reference.GetRefBonePose()[Bone];
		}
		Local[Bone] = Parent >= 0 ? MeshSpace[Bone].GetRelativeTransform(MeshSpace[Parent]) : MeshSpace[Bone];
	}
	Component.RefreshBoneTransforms();
}

const FString& FScctEquipmentModels::Name(const FScctEquipmentApi& Api, int32 Id)
{
	if (const FString* Known = Names.Find(Id))
	{
		return *Known;
	}
	return Names.Add(Id, FScctEquipmentNames::Get(Api, Id));
}

const FScctEquipmentModel* FScctEquipmentModels::Find(const FScctEquipmentApi& Api, int32 MeshId, int32 MeshPathId, const FSoftObjectPath* Override)
{
	if (MeshId <= 0)
	{
		return nullptr;
	}

	const FString MeshName = Name(Api, MeshId);
	const bool bOverride = Override != nullptr && Override->IsValid();
	const FString Key = bOverride ? MeshName + TEXT("|") + Override->ToString() : MeshName;
	if (const TUniquePtr<FScctEquipmentModel>* Known = Models.Find(Key))
	{
		return Known->IsValid() && (*Known)->Mesh.IsValid() ? Known->Get() : nullptr;
	}

	// Kept even when it fails, so a missing file is looked for and reported once.
	TUniquePtr<FScctEquipmentModel>& Model = Models.Add(Key, MakeUnique<FScctEquipmentModel>());
	Model->Name = MeshName;
	const FString File = Name(Api, MeshPathId);
	Model->Package = FPaths::GetCleanFilename(FPaths::GetPath(File));
	const FString AssetPath = bOverride ? Override->ToString() : File;
	if (MeshPathId <= 0)
	{
		UE_LOG(LogOpenSCCT, Warning, TEXT("Equipment: the content has no file for mesh '%s'; it is not drawn"), *MeshName);
		return nullptr;
	}

	const FString StockPrefix = TEXT("/Game/Imported/Chars/");
	const bool bImportedStock = bOverride && AssetPath.StartsWith(StockPrefix);
	USkeletalMesh* Mesh = bOverride && !bImportedStock ? LoadObject<USkeletalMesh>(nullptr, *AssetPath, nullptr, LOAD_NoWarn) : nullptr;
	if (bImportedStock)
	{
		TArray<FString> Segments;
		AssetPath.Mid(StockPrefix.Len()).ParseIntoArray(Segments, TEXT("/"));
		if (Segments.Num() >= 3) Mesh = ScctRuntimeSkins::Load(TEXT("shared/chars") / Segments[0] / (Segments[1] + TEXT(".glb")));
	}
	if (Mesh == nullptr)
	{
		// The table's own choice is missing: the file the simulation names is better than nothing.
		Mesh = ScctRuntimeSkins::Load(File);
	}
	if (Mesh == nullptr)
	{
		UE_LOG(LogOpenSCCT, Warning, TEXT("Equipment: could not load local mesh %s for '%s'"), *AssetPath, *MeshName);
		return nullptr;
	}

	Model->Skeleton.SetNumUninitialized(MaxEquipmentBones);
	const int32 Bones = FMath::Clamp(Api.GetMeshSkeleton(MeshId, Model->Skeleton.GetData(), MaxEquipmentBones), 0, MaxEquipmentBones);
	Model->Skeleton.SetNum(Bones);

	const FReferenceSkeleton& Reference = Mesh->GetRefSkeleton();
	TMap<FString, int32> MeshBones;
	for (int32 Index = 0; Index < Reference.GetNum(); ++Index)
	{
		MeshBones.Add(BoneKey(*Reference.GetBoneName(Index).ToString()), Index);
	}

	Model->BridgeBoneOfMeshBone.Init(INDEX_NONE, Reference.GetNum());
	int32 Missing = 0;
	for (int32 Index = 0; Index < Bones; ++Index)
	{
		const int32* Found = MeshBones.Find(BoneKey(Model->Skeleton[Index].Name));
		const int32 MeshBone = Found != nullptr && Model->BridgeBoneOfMeshBone[*Found] == INDEX_NONE ? *Found : INDEX_NONE;
		Model->BoneMap.Add(MeshBone);
		if (MeshBone != INDEX_NONE)
		{
			Model->BridgeBoneOfMeshBone[MeshBone] = Index;
		}
		Missing += MeshBone == INDEX_NONE ? 1 : 0;
	}

	// A file can use one bone name twice (the UZI's forearm), and the importer then renames the second. Such a
	// bone is found under the same parent by the start of its name.
	for (int32 Index = 0; Index < Bones && Missing > 0; ++Index)
	{
		const int32 Parent = Model->Skeleton[Index].Parent;
		if (Model->BoneMap[Index] != INDEX_NONE || Parent < 0 || Parent >= Bones || Model->BoneMap[Parent] == INDEX_NONE)
		{
			continue;
		}
		const FString Wanted = BoneKey(Model->Skeleton[Index].Name);
		for (int32 MeshBone = 0; MeshBone < Reference.GetNum(); ++MeshBone)
		{
			if (Model->BridgeBoneOfMeshBone[MeshBone] == INDEX_NONE && Reference.GetParentIndex(MeshBone) == Model->BoneMap[Parent]
				&& BoneKey(*Reference.GetBoneName(MeshBone).ToString()).StartsWith(Wanted))
			{
				Model->BoneMap[Index] = MeshBone;
				Model->BridgeBoneOfMeshBone[MeshBone] = Index;
				--Missing;
				break;
			}
		}
	}

	// Bone for bone only when nothing is missing on either side and every bone hangs on the same parent.
	Model->bDirect = Missing == 0 && Bones == Reference.GetNum();
	for (int32 Index = 0; Index < Bones && Model->bDirect; ++Index)
	{
		const int32 Parent = Model->Skeleton[Index].Parent;
		const int32 MeshParent = Reference.GetParentIndex(Model->BoneMap[Index]);
		Model->bDirect = (Parent < 0 && MeshParent < 0) || (Parent >= 0 && Parent < Bones && Model->BoneMap[Parent] == MeshParent);
	}

	Model->Mesh.Reset(Mesh);

	// The file's other meshes: everything skinned in the same folder that shares the skeleton.
	if (IAssetRegistry* Registry = IAssetRegistry::Get(); ScctRuntimeSkins::IsCooked(Mesh) && Registry)
	{
		const FString Folder = FPackageName::GetLongPackagePath(Mesh->GetPackage()->GetName());
		Registry->ScanPathsSynchronous({ Folder });
		TArray<FAssetData> Assets;
		Registry->GetAssetsByPath(FName(*Folder), Assets, false);
		for (const FAssetData& Asset : Assets)
		{
			if (Asset.AssetClassPath != USkeletalMesh::StaticClass()->GetClassPathName() || Asset.PackageName == Mesh->GetPackage()->GetFName())
			{
				continue;
			}
			USkeletalMesh* Other = Cast<USkeletalMesh>(Asset.GetAsset());
			if (Other != nullptr && Other->GetSkeleton() == Mesh->GetSkeleton())
			{
				Model->ExtraMeshes.Emplace(Other);
			}
		}
	}

	UE_LOG(LogOpenSCCT, Log, TEXT("Equipment model '%s': %s, mesh %d bones, bridge %d, %d of the bridge's unmatched, posed %s, %d more meshes on the skeleton"),
		*MeshName, *AssetPath, Reference.GetNum(), Bones, Missing, Model->bDirect ? TEXT("bone for bone") : TEXT("through component space"), Model->ExtraMeshes.Num());
	return Model.Get();
}

UMaterialInterface* FScctEquipmentModels::FindSkin(const FString& Package, const FString& SkinName)
{
	const FString Key = Package + TEXT("|") + SkinName;
	if (const TStrongObjectPtr<UMaterialInterface>* Known = Skins.Find(Key))
	{
		return Known->Get();
	}

	// "SGoggleTextures.beam_goggle.TRD_goggle_orange": the converter writes the texture of package
	// SGoggleTextures named TRD_goggle_orange as SGoggleTextures_TRD_goggle_orange.png; the group is dropped.
	FString TexturePackage, Rest, Texture;
	if (!SkinName.Split(TEXT("."), &TexturePackage, &Rest))
	{
		TexturePackage = SkinName;
	}
	if (!Rest.Split(TEXT("."), nullptr, &Texture, ESearchCase::CaseSensitive, ESearchDir::FromEnd))
	{
		Texture = Rest;
	}
	const FString File = Texture.IsEmpty() ? TexturePackage : TexturePackage + TEXT("_") + Texture;
	UMaterialInterface* Material = ScctRuntimeSkins::LoadEquipmentSkin(Package, File);
	return Skins.Add(Key, TStrongObjectPtr<UMaterialInterface>(Material)).Get();
}

void FScctEquipmentModels::LoadCatalog(const FScctEquipmentApi& Api)
{
	if (bCatalogLoaded)
	{
		return;
	}
	bCatalogLoaded = true;

	TArray<FScctEquipmentMesh> Catalog;
	Catalog.SetNumUninitialized(64);
	const int32 Count = FMath::Clamp(Api.GetCatalog(Catalog.GetData(), Catalog.Num()), 0, Catalog.Num());
	int32 Loaded = 0;
	for (int32 Index = 0; Index < Count; ++Index)
	{
		Loaded += Find(Api, Catalog[Index].Mesh, Catalog[Index].MeshPath) != nullptr ? 1 : 0;
	}
	UE_LOG(LogOpenSCCT, Log, TEXT("Equipment catalog: %d meshes, %d imported"), Count, Loaded);
}
