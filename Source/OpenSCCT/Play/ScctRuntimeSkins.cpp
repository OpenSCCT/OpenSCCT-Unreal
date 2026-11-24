#include "Play/ScctRuntimeSkins.h"

#include "Animation/AnimSequence.h"
#include "AssetRegistry/AssetData.h"
#include "AssetRegistry/IAssetRegistry.h"
#include "Misc/PackageName.h"
#include "Engine/SkeletalMesh.h"
#include "Engine/StaticMesh.h"
#include "Engine/Texture2D.h"
#include "HAL/FileManager.h"
#include "ImageUtils.h"
#include "Materials/MaterialInstanceDynamic.h"
#include "Misc/Paths.h"
#include "Misc/SecureHash.h"
#include "OpenSCCT.h"
#include "UObject/Package.h"
#include "glTFRuntimeAsset.h"
#include "glTFRuntimeFunctionLibrary.h"

namespace
{
	const TCHAR* const Surface = TEXT("/Game/OpenSCCT/Materials/M_ScctSurface.M_ScctSurface");
	const TCHAR* const RifleFinish = TEXT("/Game/OpenSCCT/Materials/M_ScctRifleFinish.M_ScctRifleFinish");
	const TCHAR* const EquipmentFinish = TEXT("/Game/OpenSCCT/Materials/MI_ScctEquipmentFinish.MI_ScctEquipmentFinish");
	const TCHAR* const LongGun = TEXT("/Game/OpenSCCT/Weapons/Materials/M_ScctLongGun.M_ScctLongGun");
	const TCHAR* const Goggle = TEXT("/Game/OpenSCCT/Weapons/Materials/M_ScctGoggleGlow.M_ScctGoggleGlow");

	struct FModel
	{
		TWeakObjectPtr<USkeletalMesh> Mesh;
		TMap<FString, TWeakObjectPtr<UAnimSequence>> Animations;
		TSet<FString> MissingAnimations;
	};
	TMap<FString, FModel> Models;
	TMap<FString, TWeakObjectPtr<UStaticMesh>> EffectMeshes;

	FString SafeName(FString Name)
	{
		for (TCHAR& C : Name)
			if (!FChar::IsAlnum(C) && C != TEXT('_')) C = TEXT('_');
		return Name;
	}

	FString Absolute(const FString& Input)
	{
		if (Input.IsEmpty()) return {};
		FString File = Input;
		if (FPaths::IsRelative(File))
		{
			const FString Root = FOpenSCCTModule::FindContentRoot();
			if (Root.IsEmpty()) return {};
			File = FPaths::ConvertRelativePathToFull(Root, Input);
			if (!FPaths::IsUnderDirectory(File, Root)) return {};
		}
		FPaths::NormalizeFilename(File);
		return File;
	}

	// Runtime packages preserve the package and model names used by camo, vision materials and first-person arms.
	UPackage* PackageFor(const FString& File)
	{
		const FString Model = SafeName(FPaths::GetBaseFilename(File));
		const FString Folder = SafeName(FPaths::GetCleanFilename(FPaths::GetPath(File)));
		const FString Group = File.Contains(TEXT("/shared/chars/")) ? TEXT("Chars") : File.Contains(TEXT("/shared/fx/")) ? TEXT("Fx") : TEXT("Outfits");
		const FString Name = TEXT("/Temp/OpenSCCTSteam") / FMD5::HashAnsiString(*File) / Group / Folder / Model;
		UPackage* Package = CreatePackage(*Name);
		Package->SetFlags(RF_Transient);
		return Package;
	}

	UTexture2D* TextureAt(UPackage* Package, const FString& File, TextureCompressionSettings Usage = TC_Default)
	{
		if (File.IsEmpty()) return nullptr;
		const FName Name(*SafeName(FPaths::GetBaseFilename(File)));
		if (UTexture2D* Existing = FindObjectFast<UTexture2D>(Package, Name)) return Existing;
		if (!FPaths::FileExists(File)) return nullptr;
		UTexture2D* Texture = FImageUtils::ImportFileAsTexture2D(File);
		if (Texture)
		{
			Texture->Rename(*Name.ToString(), Package, REN_DontCreateRedirectors | REN_NonTransactional);
			if (Usage != TC_Default)
			{
				Texture->SRGB = false;
				Texture->CompressionSettings = Usage;
				Texture->UpdateResource();
			}
		}
		return Texture;
	}

	FString LocalImage(const FString& Directory, const FString& Uri)
	{
		if (Uri.IsEmpty() || !FPaths::IsRelative(Uri)) return {};
		const FString Path = FPaths::ConvertRelativePathToFull(Directory, Uri);
		return FPaths::IsUnderDirectory(Path, Directory) ? Path : FString();
	}

	FString TextureFile(const FglTFRuntimeParser& Parser, const TSharedPtr<FJsonObject>& Fields, const TCHAR* Key, const FString& Directory)
	{
		const TSharedPtr<FJsonObject>* Base = nullptr;
		int32 Index = INDEX_NONE;
		if (!Fields || !Fields->TryGetObjectField(Key, Base)
			|| !(*Base)->TryGetNumberField(TEXT("index"), Index)) return {};
		auto Ref = Parser.GetJsonObjectFromRootIndex(TEXT("textures"), Index);
		if (!Ref || !Ref->TryGetNumberField(TEXT("source"), Index)) return {};
		auto Image = Parser.GetJsonObjectFromRootIndex(TEXT("images"), Index);
		FString Uri;
		return Image && Image->TryGetStringField(TEXT("uri"), Uri) ? LocalImage(Directory, Uri) : FString();
	}

	UglTFRuntimeAsset* Open(const FString& File)
	{
		FglTFRuntimeConfig Config;
		// Bridge poses use (X,Z,Y) with metres converted to centimetres. This is glTFRuntime's YForward basis.
		Config.TransformBaseType = EglTFRuntimeTransformBaseType::YForward;
		Config.SceneScale = 100;
		Config.bAllowExternalFiles = false;
		return UglTFRuntimeFunctionLibrary::glTFLoadAssetFromFilename(File, false, Config);
	}

	FglTFRuntimeStaticMeshConfig EffectOptions(UglTFRuntimeAsset& Asset, const FString& File)
	{
		FglTFRuntimeStaticMeshConfig Config;
		UPackage* Owner = PackageFor(File);
		Config.Outer = Owner;
		const FString Directory = FPaths::GetPath(File);
		for (int32 Index = 0;; ++Index)
		{
			const auto Image = Asset.GetParser()->GetJsonObjectFromRootIndex(TEXT("images"), Index);
			if (!Image) break;
			FString Uri;
			if (!Image->TryGetStringField(TEXT("uri"), Uri)) continue;
			if (UTexture2D* Picture = TextureAt(Owner, LocalImage(Directory, Uri)))
				Config.MaterialsConfig.ImagesOverrideMap.Add(Index, Picture);
		}
		return Config;
	}


	// ---- cooked content ---------------------------------------------------------------------------------------
	// A release cooks the characters, weapons and effects imported in the editor (Tools/Import/Import-Characters.ps1,
	// Tools/Effects/Build-EffectAssets.ps1) under /Game/Imported. Those carry the editor's materials (glass, lenses,
	// view-model sections) and are preferred. The Steam conversion's GLBs are read at run time only without them.
	const TCHAR* const CookedChars = TEXT("/Game/Imported/Chars");
	const TCHAR* const CookedFx = TEXT("/Game/Imported/Fx");

	/** "pkg" and "mesh" of ".../shared/chars/<pkg>/<mesh>.glb", relative or absolute. */
	bool SplitCharFile(const FString& Input, FString& OutPackage, FString& OutMesh)
	{
		FString File = Input;
		FPaths::NormalizeFilename(File);
		const FString Marker = TEXT("shared/chars/");
		const int32 At = File.Find(Marker, ESearchCase::IgnoreCase);
		if (At == INDEX_NONE || !File.EndsWith(TEXT(".glb"), ESearchCase::IgnoreCase)) return false;
		TArray<FString> Parts;
		File.Mid(At + Marker.Len()).ParseIntoArray(Parts, TEXT("/"));
		if (Parts.Num() != 2) return false;
		OutPackage = Parts[0];
		OutMesh = FPaths::GetBaseFilename(Parts[1]);
		return !OutPackage.IsEmpty() && !OutMesh.IsEmpty() && OutPackage == SafeName(OutPackage) && OutMesh == SafeName(OutMesh);
	}

	USkeletalMesh* CookedCharMesh(const FString& Input)
	{
		FString Package, Mesh;
		if (!SplitCharFile(Input, Package, Mesh)) return nullptr;
		const FString PackageName = FString::Printf(TEXT("%s/%s/%s/SkeletalMeshes/%s"), CookedChars, *Package, *Mesh, *Mesh);
		return FPackageName::DoesPackageExist(PackageName)
			? LoadObject<USkeletalMesh>(nullptr, *(PackageName + TEXT(".") + Mesh), nullptr, LOAD_NoWarn | LOAD_Quiet) : nullptr;
	}

	/** A name reduced to lower-case letters and digits: how the imported effect meshes match the game's object paths. */
	FString NameKey(const FString& Name)
	{
		FString Key;
		for (const TCHAR Character : Name)
			if (FChar::IsAlnum(Character)) Key.AppendChar(FChar::ToLower(Character));
		return Key;
	}

	const TMap<FString, FSoftObjectPath>& CookedFxMeshes()
	{
		static TMap<FString, FSoftObjectPath> Found;
		static bool bScanned = false;
		if (!bScanned)
		{
			bScanned = true;
			if (IAssetRegistry* Registry = IAssetRegistry::Get())
			{
				Registry->ScanPathsSynchronous({ CookedFx }, false);
				TArray<FAssetData> Assets;
				Registry->GetAssetsByPath(FName(CookedFx), Assets, true);
				for (const FAssetData& Asset : Assets)
					if (Asset.AssetClassPath == UStaticMesh::StaticClass()->GetClassPathName())
						Found.Add(NameKey(Asset.AssetName.ToString()), Asset.GetSoftObjectPath());
			}
			UE_LOG(LogOpenSCCT, Log, TEXT("Effects: %d cooked effect meshes under %s"), Found.Num(), CookedFx);
		}
		return Found;
	}
}

USkeletalMesh* ScctRuntimeSkins::Load(const FString& Input)
{
	if (USkeletalMesh* Cooked = CookedCharMesh(Input)) return Cooked;
	const FString File = Absolute(Input);
	if (File.IsEmpty() || !FPaths::FileExists(File)) return nullptr;
	if (const FModel* Entry = Models.Find(File); Entry && Entry->Mesh.IsValid()) return Entry->Mesh.Get();
	UglTFRuntimeAsset* Asset = Open(File);
	if (!Asset || !Asset->GetParser()) return nullptr;
	UPackage* Package = PackageFor(File);
	FglTFRuntimeSkeletalMeshConfig Config;
	Config.Outer = Package;
	Config.SkeletonConfig.bAppendNodeIndexOnNameCollision = true;
	Config.SkeletonConfig.bAddRootNodeIfMissing = true;
	const FString Directory = FPaths::GetPath(File);
	const bool bBody = Directory.EndsWith(TEXT("/SPerso"), ESearchCase::IgnoreCase);
	const FString FileName = FPaths::GetBaseFilename(File);
	const bool bFirstPersonFinish = (Directory.EndsWith(TEXT("/SWeaponFirst")) && FileName.StartsWith(TEXT("1DEF_"))) ||
		(Directory.EndsWith(TEXT("/OpenScctSidearms")) && FileName.EndsWith(TEXT("_first")));
	const bool bRifles = Directory.EndsWith(TEXT("/OpenScctRifles"), ESearchCase::IgnoreCase);
	FString ModelName = FileName;
	ModelName.RemoveFromEnd(TEXT("_first"));
	const bool bLongGun = bRifles && (ModelName == TEXT("boltrifle") || ModelName == TEXT("designatedmarksman") ||
		ModelName == TEXT("compactsmg") || ModelName == TEXT("pumpshotgun"));
	int32 Index = 0;
	TMap<FString, int32> SlotNames;
	TMap<UMaterialInterface*, FName> MaterialSlots;
	const auto Parser = Asset->GetParser();
	for (const TSharedRef<FJsonObject>& Material : Parser->GetMaterials())
	{
		FString Name, AlphaMode;
		Material->TryGetStringField(TEXT("name"), Name);
		Material->TryGetStringField(TEXT("alphaMode"), AlphaMode);
		const bool bRifleFinish = bRifles && !bLongGun && !Name.EndsWith(TEXT(".Arms")) &&
			(AlphaMode.IsEmpty() || AlphaMode == TEXT("OPAQUE")) && !Material->HasField(TEXT("emissiveTexture")) &&
			!Material->HasField(TEXT("extensions"));
		const TSharedPtr<FJsonObject>* PbrField = nullptr;
		Material->TryGetObjectField(TEXT("pbrMetallicRoughness"), PbrField);
		const TSharedPtr<FJsonObject> Pbr = PbrField ? *PbrField : nullptr;
		FString SlotName = SafeName(Name.IsEmpty() ? FString::FromInt(Index) : Name);
		const int32 Occurrence = SlotNames.FindOrAdd(SlotName)++;
		if (Occurrence) SlotName += FString::FromInt(Occurrence);
		const FName Slot(*SlotName);
		const TSharedPtr<FJsonObject>* Extensions = nullptr;
		const bool bUnlit = Material->TryGetObjectField(TEXT("extensions"), Extensions) && (*Extensions)->HasField(TEXT("KHR_materials_unlit"));
		if (bUnlit || AlphaMode == TEXT("BLEND") || AlphaMode == TEXT("MASK"))
		{
			// These slots need the glTF blend mode, opacity cutoff or unlit shader. External
			// images still go through our map-local resolver rather than arbitrary file access.
			auto BindTexture = [&](const TSharedPtr<FJsonObject>& Fields, const TCHAR* Key, TextureCompressionSettings Usage)
			{
				const TSharedPtr<FJsonObject>* Info = nullptr;
				int32 TextureIndex = INDEX_NONE;
				if (Fields && Fields->TryGetObjectField(Key, Info) && (*Info)->TryGetNumberField(TEXT("index"), TextureIndex))
				{
					if (UTexture2D* Picture = TextureAt(Package, TextureFile(*Parser, Fields, Key, Directory), Usage))
						Config.MaterialsConfig.TexturesOverrideMap.Add(TextureIndex, Picture);
				}
			};
			BindTexture(Pbr, TEXT("baseColorTexture"), TC_Default);
			BindTexture(Pbr, TEXT("metallicRoughnessTexture"), TC_Masks);
			BindTexture(Material, TEXT("normalTexture"), TC_Normalmap);
			BindTexture(Material, TEXT("emissiveTexture"), TC_Default);
			BindTexture(Material, TEXT("occlusionTexture"), TC_Masks);
			FString SourceName;
			UMaterialInterface* Optical = Parser->LoadMaterial(Index, Config.MaterialsConfig, false, SourceName, nullptr);
			if (!Optical) return nullptr;
			MaterialSlots.Add(Optical, Slot);
			Config.MaterialsConfig.MaterialsOverrideMap.Add(Index++, Optical);
			continue;
		}
		FString FinishFile;
		bool bPackedFinish = false;
		if (bFirstPersonFinish)
		{
			FinishFile = TextureFile(*Parser, Pbr, TEXT("metallicRoughnessTexture"), Directory);
			bPackedFinish = !FinishFile.IsEmpty();
			const TSharedPtr<FJsonObject>* Extras = nullptr;
			FString Uri;
			if (!bPackedFinish && Material->TryGetObjectField(TEXT("extras"), Extras) && (*Extras)->TryGetStringField(TEXT("scct_spec_mask"), Uri))
				FinishFile = LocalImage(Directory, Uri);
		}
		const TCHAR* Path = bLongGun ? LongGun : bRifleFinish ? RifleFinish : !FinishFile.IsEmpty() ? EquipmentFinish : Surface;
		UMaterialInterface* Parent = LoadObject<UMaterialInterface>(nullptr, Path);
		if (!Parent) return nullptr;
		const FName InstanceName(*(TEXT("MI_") + SlotName));
		auto* Instance = FindObjectFast<UMaterialInstanceDynamic>(Package, InstanceName);
		if (!Instance) Instance = UMaterialInstanceDynamic::Create(Parent, Package, InstanceName);
		UTexture2D* Colour = TextureAt(Package, TextureFile(*Parser, Pbr, TEXT("baseColorTexture"), Directory));
		if (Colour) Instance->SetTextureParameterValue(bLongGun ? TEXT("GunBase") : bRifleFinish ? TEXT("BaseColorTexture") : TEXT("BaseColor"), Colour);
		if (bLongGun || bRifleFinish)
		{
			if (UTexture2D* Normal = TextureAt(Package, TextureFile(*Parser, Material, TEXT("normalTexture"), Directory), TC_Normalmap))
				Instance->SetTextureParameterValue(bLongGun ? TEXT("GunNormal") : TEXT("NormalTexture"), Normal);
			UTexture2D* Packed = TextureAt(Package, TextureFile(*Parser, Pbr, TEXT("metallicRoughnessTexture"), Directory), TC_Masks);
			if (Packed) Instance->SetTextureParameterValue(bLongGun ? TEXT("GunORM") : TEXT("MetallicRoughnessTexture"), Packed);
			double Roughness = 1, Metallic = 1;
			if (Pbr)
			{
				Pbr->TryGetNumberField(TEXT("roughnessFactor"), Roughness);
				Pbr->TryGetNumberField(TEXT("metallicFactor"), Metallic);
			}
			Instance->SetScalarParameterValue(TEXT("RoughnessFactor"), Roughness);
			Instance->SetScalarParameterValue(TEXT("MetallicFactor"), Metallic);
			if (bLongGun)
			{
				const TSharedPtr<FJsonObject>* Occlusion = nullptr;
				double Strength = 1;
				if (Material->TryGetObjectField(TEXT("occlusionTexture"), Occlusion)) (*Occlusion)->TryGetNumberField(TEXT("strength"), Strength);
				else Strength = 0;
				Instance->SetScalarParameterValue(TEXT("OcclusionStrength"), Strength);
			}
			else
			{
				Instance->SetScalarParameterValue(TEXT("HasSourceORM"), Packed ? 1.f : 0.f);
				const TArray<TSharedPtr<FJsonValue>>* Factor = nullptr;
				if (Pbr && Pbr->TryGetArrayField(TEXT("baseColorFactor"), Factor) && Factor->Num() == 4)
					Instance->SetVectorParameterValue(TEXT("BaseColorFactor"), FLinearColor((*Factor)[0]->AsNumber(), (*Factor)[1]->AsNumber(),
						(*Factor)[2]->AsNumber(), (*Factor)[3]->AsNumber()));
			}
		}
		else
		{
			if (UTexture2D* Normal = TextureAt(Package, TextureFile(*Parser, Material, TEXT("normalTexture"), Directory), TC_Normalmap))
				Instance->SetTextureParameterValue(TEXT("Normal"), Normal);
			if (UTexture2D* Finish = TextureAt(Package, FinishFile, TC_Masks))
			{
				double Roughness = 1, Metallic = 1;
				if (Pbr && bPackedFinish) Pbr->TryGetNumberField(TEXT("roughnessFactor"), Roughness);
				if (Pbr) Pbr->TryGetNumberField(TEXT("metallicFactor"), Metallic);
				Instance->SetTextureParameterValue(TEXT("WeaponFinishTexture"), Finish);
				Instance->SetScalarParameterValue(TEXT("WeaponPackedPBR"), bPackedFinish ? 1.f : 0.f);
				Instance->SetScalarParameterValue(TEXT("WeaponRoughnessFactor"), Roughness);
				Instance->SetScalarParameterValue(TEXT("WeaponMetallicFactor"), Metallic);
			}
			const bool bDisplay = Name.Contains(TEXT("si_rof")) || Name.EndsWith(TEXT("_si"));
			const bool bCloth = Name.Contains(TEXT("1def")) || Name.Contains(TEXT("arms")) || Name.Contains(TEXT("att_01")) ||
				Name.Contains(TEXT("shadw")) || Name.Contains(TEXT("heat"));
			const bool bSight = Name.Contains(TEXT("viseur"));
			Instance->SetScalarParameterValue(TEXT("Specular"), bBody ? .18f : .5f);
			Instance->SetScalarParameterValue(TEXT("RoughnessDark"), bBody ? .92f : bDisplay ? .5f : bCloth ? .8f : bSight ? .25f : .62f);
			Instance->SetScalarParameterValue(TEXT("RoughnessBright"), bBody ? .62f : bDisplay ? .3f : bCloth ? .5f : bSight ? .1f : .4f);
			Instance->SetScalarParameterValue(TEXT("Metallic"), bBody || bDisplay || bCloth ? 0.f : bSight ? .3f : .35f);
			Instance->SetScalarParameterValue(TEXT("Emissive"), bDisplay ? 4.f : 0.f);
			Instance->SetScalarParameterValue(TEXT("AlphaMask"), 0.f);
		}
		MaterialSlots.Add(Instance, Slot);
		Config.MaterialsConfig.MaterialsOverrideMap.Add(Index++, Instance);
	}
	// A first-person file can have several skinned nodes for the arms, equipment and magazine. Use the whole scene.
	USkeletalMesh* Mesh = Asset->LoadSkeletalMeshRecursive(TEXT(""), {}, Config);
	if (Mesh)
	{
		// glTFRuntime creates one material entry per primitive. Gameplay identifies every
		// repeated optic, lens, suppressor and arm section by its authored material name.
		for (FSkeletalMaterial& Section : Mesh->GetMaterials())
		{
			if (const FName* Slot = MaterialSlots.Find(Section.MaterialInterface.Get()))
				Section.MaterialSlotName = *Slot;
		}
		Mesh->Rename(*SafeName(FPaths::GetBaseFilename(File)), Package, REN_DontCreateRedirectors | REN_NonTransactional);
		Models.Add(File, {Mesh});
		UE_LOG(LogOpenSCCT, Log, TEXT("Local skeletal mesh %s: %d bones, %d material slots"),
			*File, Mesh->GetRefSkeleton().GetNum(), Mesh->GetMaterials().Num());
	}
	else
	{
		UE_LOG(LogOpenSCCT, Warning, TEXT("Could not build the local skeletal mesh %s"), *File);
	}
	return Mesh;
}

UStaticMesh* ScctRuntimeSkins::LoadEffectMesh(const FString& ObjectPath)
{
	// Bridge paths include optional UE2 groups. The conversion names each glTF mesh after its package and object.
	FString Package, Tail;
	if (!ObjectPath.Split(TEXT("."), &Package, &Tail) || Package != SafeName(Package)) return nullptr;
	FString Object;
	if (!Tail.Split(TEXT("."), nullptr, &Object, ESearchCase::IgnoreCase, ESearchDir::FromEnd)) Object = Tail;
	if (Object.IsEmpty() || Object != SafeName(Object)) return nullptr;
	// "SGameplayObjects.Gadgets.frag_grenade" was imported as the mesh "SGameplayObjects_frag_grenade".
	if (const FSoftObjectPath* Cooked = CookedFxMeshes().Find(NameKey(Package + Object)))
		if (UStaticMesh* Mesh = Cast<UStaticMesh>(Cooked->TryLoad())) return Mesh;
	const FString File = Absolute(TEXT("shared/fx/meshes") / (Package + TEXT(".glb")));
	if (File.IsEmpty() || !FPaths::FileExists(File)) return nullptr;
	const FString MeshName = Package + TEXT("_") + Object;
	const FString CacheKey = File + TEXT("|") + MeshName;
	if (const auto* Cached = EffectMeshes.Find(CacheKey); Cached && Cached->IsValid()) return Cached->Get();
	UglTFRuntimeAsset* Asset = Open(File);
	if (!Asset || !Asset->GetParser()) return nullptr;
	const auto Parser = Asset->GetParser();
	const auto Meshes = Parser->GetMeshes();
	const int32 MeshIndex = Meshes.IndexOfByPredicate([&MeshName](const TSharedRef<FJsonObject>& Entry)
	{
		FString Name;
		return Entry->TryGetStringField(TEXT("name"), Name) && Name.Equals(MeshName, ESearchCase::IgnoreCase);
	});
	if (MeshIndex == INDEX_NONE) return nullptr;
	const FglTFRuntimeStaticMeshConfig Config = EffectOptions(*Asset, File);
	UStaticMesh* Mesh = Asset->LoadStaticMesh(MeshIndex, Config);
	if (Mesh) EffectMeshes.Add(CacheKey, Mesh);
	return Mesh;
}

void ScctRuntimeSkins::LoadEffectMeshes(TArray<UStaticMesh*>& Out)
{
	if (!CookedFxMeshes().IsEmpty())
	{
		for (const TPair<FString, FSoftObjectPath>& Asset : CookedFxMeshes())
			if (UStaticMesh* Mesh = Cast<UStaticMesh>(Asset.Value.TryLoad())) Out.Add(Mesh);
		return;
	}
	const FString Directory = Absolute(TEXT("shared/fx/meshes"));
	if (Directory.IsEmpty()) return;
	TArray<FString> Files;
	IFileManager::Get().FindFiles(Files, *(Directory / TEXT("*.glb")), true, false);
	Files.Sort();
	for (const FString& Name : Files)
	{
		const FString File = Directory / Name;
		UglTFRuntimeAsset* Asset = Open(File);
		if (!Asset || !Asset->GetParser()) continue;
		const auto Config = EffectOptions(*Asset, File);
		const auto Meshes = Asset->GetParser()->GetMeshes();
		for (int32 Index = 0; Index < Meshes.Num(); ++Index)
		{
			FString MeshName;
			if (!Meshes[Index]->TryGetStringField(TEXT("name"), MeshName)) continue;
			auto& Entry = EffectMeshes.FindOrAdd(File + TEXT("|") + MeshName);
			if (!Entry.IsValid()) Entry = Asset->LoadStaticMesh(Index, Config);
			if (Entry.IsValid()) Out.Add(Entry.Get());
		}
	}
}

FString ScctRuntimeSkins::SourceOf(const USkinnedAsset* Mesh)
{
	for (const auto& Pair : Models)
		if (Mesh && Pair.Value.Mesh.Get() == Mesh) return Pair.Key;
	return {};
}

bool ScctRuntimeSkins::IsCooked(const UObject* Asset)
{
	return Asset && Asset->GetOutermost()->GetName().StartsWith(TEXT("/Game/"));
}

FString ScctRuntimeSkins::ContentFileOf(const USkinnedAsset* Mesh)
{
	FString Source = SourceOf(Mesh);
	if (!Source.IsEmpty() || !Mesh) return Source;
	// /Game/Imported/Chars/<Package>/<Mesh>/SkeletalMeshes/<Mesh> stands for shared/chars/<Package>/<Mesh>.glb.
	TArray<FString> Parts;
	Mesh->GetOutermost()->GetName().ParseIntoArray(Parts, TEXT("/"));
	const FString Root = FOpenSCCTModule::FindContentRoot();
	if (Parts.Num() == 7 && Parts[0] == TEXT("Game") && Parts[1] == TEXT("Imported") && Parts[2] == TEXT("Chars") && !Root.IsEmpty())
		return Root / TEXT("shared/chars") / Parts[3] / (Parts[4] + TEXT(".glb"));
	return {};
}

UMaterialInterface* ScctRuntimeSkins::LoadEquipmentSkin(const FString& Package, const FString& Texture)
{
	if (Package != SafeName(Package) || Texture != SafeName(Texture)) return nullptr;
	// import_characters.py makes MI_<texture package>_<texture> beside the package's meshes.
	const FString CookedSkin = FString::Printf(TEXT("%s/%s/Skins/MI_%s"), CookedChars, *Package, *Texture);
	if (FPackageName::DoesPackageExist(CookedSkin))
		if (auto* Material = LoadObject<UMaterialInterface>(nullptr, *FString::Printf(TEXT("%s.MI_%s"), *CookedSkin, *Texture), nullptr, LOAD_NoWarn | LOAD_Quiet))
			return Material;
	const FString File = Absolute(TEXT("shared/chars") / Package / TEXT("textures") / (Texture + TEXT(".png")));
	if (File.IsEmpty() || !FPaths::FileExists(File)) return nullptr;
	UPackage* Owner = PackageFor(File);
	UTexture2D* Picture = TextureAt(Owner, File);
	const bool bLens = Package.Equals(TEXT("SGoggle"), ESearchCase::IgnoreCase);
	auto* Parent = LoadObject<UMaterialInterface>(nullptr, bLens ? Goggle : Surface);
	if (!Picture || !Parent) return nullptr;
	auto* Material = UMaterialInstanceDynamic::Create(Parent, Owner, FName(*(TEXT("MI_") + Texture)));
	Material->SetTextureParameterValue(bLens ? TEXT("Lens") : TEXT("BaseColor"), Picture);
	if (!bLens) Material->SetScalarParameterValue(TEXT("Emissive"), 4.0f);
	return Material;
}

UAnimSequence* ScctRuntimeSkins::LoadAnimation(USkeletalMesh* Mesh, const FString& Name)
{
	const FString File = SourceOf(Mesh);
	if (File.IsEmpty()) return nullptr;
	FModel& Model = Models.FindChecked(File);
	if (const auto* Known = Model.Animations.Find(Name); Known && Known->IsValid()) return Known->Get();
	if (Model.MissingAnimations.Contains(Name)) return nullptr;
	UglTFRuntimeAsset* Asset = Open(File);
	if (!Asset) return nullptr;
	const TArray<FString> Clips = Asset->GetAnimationsNames();
	const FString* Clip = Clips.FindByPredicate([&Name](const FString& C) { return C.Equals(Name, ESearchCase::IgnoreCase); });
	if (!Clip) Clip = Clips.FindByPredicate([&Name](const FString& C) { return C.EndsWith(Name, ESearchCase::IgnoreCase); });
	if (!Clip && Clips.Num() == 1) Clip = &Clips[0];
	UAnimSequence* Sequence = Clip ? Asset->LoadSkeletalAnimationByName(Mesh, *Clip, {}) : nullptr;
	if (Sequence) Model.Animations.Add(Name, Sequence);
	else Model.MissingAnimations.Add(Name);
	return Sequence;
}
