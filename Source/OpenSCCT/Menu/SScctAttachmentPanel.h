#pragma once

#include "Runtime/ScctWeaponAttachmentsApi.h"
#include "Widgets/SCompoundWidget.h"

/** The same per-weapon workbench is used for saved Armoury choices and the Equipment draft. */
class SScctAttachmentPanel : public SCompoundWidget
{
public:
	SLATE_BEGIN_ARGS(SScctAttachmentPanel) {}
		SLATE_ARGUMENT(FString, Weapon)
		SLATE_ARGUMENT(FString, WeaponName)
		SLATE_ARGUMENT(TOptional<int32>, DraftMask)
		SLATE_ARGUMENT(TFunction<bool(int32)>, OnChoose)
	SLATE_END_ARGS()

	void Construct(const FArguments& Args);

private:
	int32 PointsUsed(int32 Selection) const;
	int32 SelectionAfterToggle(int32 Index) const;
	/** One attachment: its slot, name, effect and cost, and the action or what it still needs. */
	TSharedRef<SWidget> MakeCard(int32 Index);
	/** How far the kills are towards unlocking it, 0..1. */
	float KillShare(int32 Index) const;
	FString ActionLabel(int32 Index) const;
	bool CanToggle(int32 Index) const;
	void Toggle(int32 Index);
	EActiveTimerReturnType WatchProgress(double, float);

	FScctWeaponAttachmentState State = {};
	FString Weapon;
	TFunction<bool(int32)> OnChoose;
	int32 Mask = 0;
	bool bDraft = false;
	FString Notice;
};
