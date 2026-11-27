#pragma once

#include "CoreMinimal.h"
#include "GameFramework/Actor.h"
#include "Play/ScctEquipmentView.h"
#include "Runtime/ScctEquipmentApi.h"
#include "ScctFirstPersonView.generated.h"

class FScctEquipmentModels;
class UCameraComponent;
class UMaterialInstanceDynamic;
class UMaterialInterface;
class UMeshComponent;
class UPoseableMeshComponent;
class UPointLightComponent;
class USceneCaptureComponent2D;
class UStaticMesh;
class UStaticMeshComponent;
class UTextureRenderTarget2D;
class UScctWeaponSet;

/**
 * The local player's first-person view model: the merc's arms and gun, or the spy's arms and pistol in his eye
 * view. The bridge computes everything about it (which models, every bone, where the skeleton sits in front of
 * the camera); this actor draws it.
 *
 * It is placed from the frame's final camera, in the session's OnFrame, which runs after the camera has been
 * moved and before the engine caches it: the model is never a frame behind the view.
 *
 * Its meshes are first-person primitives (EFirstPersonPrimitiveType::FirstPerson): the renderer pulls them
 * towards the camera by the camera's first-person scale, which leaves them exactly where they were on screen
 * but nearer than any wall the player can stand against, so the gun never pokes into the level. They are lit by
 * the scene like everything else and shadow themselves (r.FirstPerson.SelfShadow). The hidden third-person body
 * casts the world shadow. The raised M4 uses its own projection to frame its physical optic.
 *
 * Unattended switch (off by default; for runs with nobody at the keyboard):
 *   -ScctFlashHold=<seconds>   keep every muzzle flash at full strength that long before it fades, so a
 *                              screenshot can catch one (a real flash is over in a twentieth of a second)
 */
UCLASS(NotPlaceable, Transient)
class OPENSCCT_API AScctFirstPersonView : public AActor
{
	GENERATED_BODY()

public:
	AScctFirstPersonView();
	static void AddReferencedObjects(UObject* InThis, FReferenceCollector& Collector);

	/**
	 * Draws the frame's view model as the bridge describes it, in front of Camera (the frame's final camera in
	 * the world). Cloak is the camo suit's amount on the arms and gun; Motion is what the weapon is doing, for
	 * the moving parts of a model drawn in the original gun's place.
	 */
	void Present(const FScctEquipmentApi& Api, FScctEquipmentModels& Models, const UScctWeaponSet* Weapons,
		const FScctFirstPerson& View, const FTransform& Camera, float Cloak, const FScctWeaponMotion& Motion, float DeltaSeconds);

	/** Nothing is drawn this frame (third person, the scope, a pistol put away, a tour camera). */
	void Hide(float DeltaSeconds);

	bool IsShown() const { return bShown; }
	bool UsesSightProjection() const { return SightCamera.IsValid(); }

	/** The muzzle in the world this frame, X along the barrel. False while no model with a muzzle is drawn. */
	bool FindMuzzle(FTransform& OutWorld) const;

	/** The ejection port of the gun drawn this frame (a replacement's EjectionPort socket, else bone FX_DOUILLE). */
	bool FindCasingPort(FTransform& OutWorld) const;

	/** Where the laser leaves the weapon this frame: the spy pistol's laser socket, else the muzzle. */
	bool FindLaserStart(FTransform& OutWorld) const;

	/** The drawn lamp or laser origin and forward direction in the world after first-person projection. */
	bool FindMercEmitter(bool bTorch, float FirstPersonScale, FTransform& OutWorld, bool bRequireRail = false) const;
	/** The point where a view-model socket is drawn after the renderer's first-person projection. */
	FVector ProjectSocket(const FVector& World, float FirstPersonScale) const;

	/** A shot left the gun: the flash and its light, at the muzzle. */
	void PlayMuzzleFlash();

private:
	FTransform MercWeaponPose(const FString& Model, float Aim, float Speed, bool bFresh, const FScctWeaponMotion& Motion, float DeltaSeconds);
	void SetM4SightGlass(bool bEnabled);
	enum class EStockSight : uint8 { Visible, ReplaceHousing, Removed };
	void SetStockSight(EStockSight Mode);
	void SetSightProjection(UCameraComponent* Camera, float Aim, float RaisedHalfTan = 0.260f);
	void UpdatePrecisionImage(UStaticMeshComponent* Optic, const UCameraComponent* Camera, float Power, bool bHasEtchedReticle = false);
	void SetScopePhysicalCoating(float Weight);
	UPROPERTY(Transient)
	TObjectPtr<USceneCaptureComponent2D> ScopeCapture;
	UPROPERTY(Transient)
	TObjectPtr<UTextureRenderTarget2D> ScopeTarget;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ScopeImage;
	UPROPERTY(Transient)
	TObjectPtr<UMaterialInstanceDynamic> ScopeCoating;
	TWeakObjectPtr<UStaticMeshComponent> BoundScope;
	TWeakObjectPtr<UStaticMesh> BoundScopeMesh;
	bool bScopeCapturing = false;
	int32 BoundScopeSlot = INDEX_NONE;
	int32 BoundCoatingSlot = INDEX_NONE;
	TWeakObjectPtr<UCameraComponent> SightCamera;
	float SightFocalScale = 1.0f;
	float CarryPace = 0.0f;
	float CompactIdleBlend = 0.0f;
	float ShoulderFollow = 0.0f;
	int32 MercWeaponActor = 0;
	bool bM4SightGlass = false;
	struct FSightBinding
	{
		TWeakObjectPtr<UMeshComponent> Mesh;
		int32 Slot;
		TObjectPtr<UMaterialInterface> Original;
	};
	TArray<FSightBinding> SightBindings;
	EStockSight StockSight = EStockSight::Visible;
	struct FHiddenSightSection
	{
		TWeakObjectPtr<UPoseableMeshComponent> Mesh;
		int32 Slot;
		int32 Lod;
	};
	TArray<FHiddenSightSection> HiddenSightMaterials;
	UPROPERTY()
	TObjectPtr<UMaterialInterface> M4SightGlass;
	UPROPERTY()
	TObjectPtr<UStaticMeshComponent> MercOpticCap;

	void TickFlash(float DeltaSeconds);
	/** Where the flash's light goes this frame: the muzzle, or short of the level's wall the barrel reaches through. */
	FVector FlashLightPlace() const;

	/** -ScctWeaponProbe: logs where a part's bones are from the eye. */
	void LogViewProbe(int32 Index, class UPoseableMeshComponent& Body, const FTransform& Camera);

	double NextViewProbe[2] = { 0.0, 0.0 };

	FScctEquipmentView Parts[2];
	int32 PartMeshes[2] = { 0, 0 };
	int32 RateOfFireSkin = -1;
	bool bShown = false;

	bool bHasMuzzle = false;
	bool bHasLaser = false;
	/**
	 * The spy eye view's muzzle and laser socket have a wall between them and the eye this frame (the bridge's trace,
	 * Runtime/ScctPoseAimApi.h): the pistol stays drawn, but no flash or laser starts at a hidden socket.
	 */
	bool bMuzzleHidden = false;
	bool bLaserHidden = false;
	int32 LoggedHiddenSockets = 0;
	FTransform Muzzle;
	FTransform Laser;
	FTransform LastCamera;
	FScctCloakLook CloakLook;

	UPROPERTY()
	TObjectPtr<USceneComponent> Root;

	UPROPERTY()
	TObjectPtr<USceneComponent> FlashRoot;

	/** The flash: a hot core, a tongue along the barrel and two thin flares across it. */
	UPROPERTY()
	TArray<TObjectPtr<UStaticMeshComponent>> FlashShapes;

	UPROPERTY()
	TArray<TObjectPtr<UMaterialInstanceDynamic>> FlashMaterials;

	UPROPERTY()
	TObjectPtr<UPointLightComponent> FlashLight;

	/** Seconds since the last shot; negative while no flash shows. */
	float FlashAge = -1.0f;
	/** The exposure scale the flash's light took when it fired (Play/ScctExposure.h). */
	float FlashExposureScale = 1.0f;
	/** How much of that the nearest wall let it have when it fired (FScctExposure::WallHold). */
	float FlashWallHold = 1.0f;
	int32 FlashesPlayed = 0;
	TArray<FScctBridgeBone> PoseBuffer;
};
