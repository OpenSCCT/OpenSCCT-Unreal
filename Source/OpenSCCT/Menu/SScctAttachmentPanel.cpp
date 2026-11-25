#include "Menu/SScctAttachmentPanel.h"

#include "Brushes/SlateRoundedBoxBrush.h"
#include "Menu/ScctMenuKit.h"
#include "Menu/ScctMenuStyle.h"
#include "Styling/CoreStyle.h"
#include "Widgets/Layout/SBorder.h"
#include "Widgets/Layout/SBox.h"
#include "Widgets/Layout/SWidgetSwitcher.h"
#include "Widgets/SBoxPanel.h"
#include "Widgets/SOverlay.h"
#include "Widgets/Text/STextBlock.h"

namespace
{
	constexpr const TCHAR* AttachmentNames[] = {TEXT("Laser"), TEXT("Red dot sight"), TEXT("Silencer"), TEXT("30-round magazine"), TEXT("ACOG")};
	// Where each one goes on the gun. The two optics share the top rail, so only one of them fits at a time.
	constexpr const TCHAR* AttachmentSlots[] = {TEXT("RAIL"), TEXT("OPTIC"), TEXT("MUZZLE"), TEXT("MAGAZINE"), TEXT("OPTIC")};
	// What each one does, from Runtime/src/OpenScct.Game/Equipment/WeaponAttachments.cs and the rules that read the mask.
	constexpr const TCHAR* AttachmentEffects[] = {
		TEXT("Adds the laser mode to the weapon torch."),
		TEXT("A 1× reflex sight with a clear aiming dot."),
		TEXT("The shot is heard at 40% of the usual distance."),
		TEXT("Thirty rounds in the magazine instead of twenty."),
		TEXT("A 4× magnified optic for longer sight lines."),
	};

	const FSlateBrush* Rounded(float Radius)
	{
		static TMap<int32, TSharedPtr<FSlateRoundedBoxBrush>> Brushes;
		TSharedPtr<FSlateRoundedBoxBrush>& Brush = Brushes.FindOrAdd(FMath::RoundToInt(Radius * 10.0f));
		if (!Brush.IsValid()) { Brush = MakeShared<FSlateRoundedBoxBrush>(FLinearColor::White, Radius); }
		return Brush.Get();
	}

	const FSlateBrush* Outlined(float Radius)
	{
		static TMap<int32, TSharedPtr<FSlateRoundedBoxBrush>> Brushes;
		TSharedPtr<FSlateRoundedBoxBrush>& Brush = Brushes.FindOrAdd(FMath::RoundToInt(Radius * 10.0f));
		if (!Brush.IsValid()) { Brush = MakeShared<FSlateRoundedBoxBrush>(FLinearColor::Transparent, Radius, FLinearColor::White, 1.0f); }
		return Brush.Get();
	}
}

void SScctAttachmentPanel::Construct(const FArguments& Args)
{
	Weapon = Args._Weapon;
	OnChoose = Args._OnChoose;
	const FScctWeaponAttachmentsApi* Api = ScctFeature<FScctWeaponAttachmentsApi>();
	if (!Api || !Api->GetState(*Weapon, &State))
	{
		ChildSlot[ScctMenuStyle::Label(TEXT("No attachments available for this weapon"))];
		return;
	}
	bDraft = Args._DraftMask.IsSet();
	Mask = Args._DraftMask.Get(State.Equipped);
	const FLinearColor Accent = ScctHudStyle::Accent(1);

	const TSharedRef<SVerticalBox> Rows = SNew(SVerticalBox);

	// ---- the weapon and its points budget
	const TSharedRef<SHorizontalBox> Meter = SNew(SHorizontalBox);
	for (int32 Point = 0; Point < FMath::Max(State.Budget, 1); ++Point)
	{
		Meter->AddSlot().AutoWidth().Padding(0, 0, 4, 0).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(30.0f).HeightOverride(8.0f)
			[
				SNew(SBorder).BorderImage(Rounded(2.0f))
				.BorderBackgroundColor_Lambda([this, Point, Accent]
				{
					return Point < PointsUsed(Mask) ? Accent : FLinearColor(1.0f, 1.0f, 1.0f, 0.12f);
				})
			]
		];
	}
	Meter->AddSlot().AutoWidth().Padding(8, 0, 0, 0).VAlign(VAlign_Center)
	[
		SNew(STextBlock).Font(ScctUi::Caps(11)).ColorAndOpacity(ScctHudStyle::TextDim())
		.Text_Lambda([this]
		{
			return FText::FromString(FString::Printf(TEXT("%d / %d POINTS USED"), PointsUsed(Mask), State.Budget));
		})
	];
	Rows->AddSlot().AutoHeight()
	[
		SNew(SHorizontalBox)
		+ SHorizontalBox::Slot().FillWidth(1.0f).VAlign(VAlign_Bottom)
		[
			ScctMenuStyle::Title(FText::FromString(Args._WeaponName), 22)
		]
		+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Bottom).Padding(16, 0, 0, 6)
		[
			SNew(STextBlock).Font(ScctUi::Caps(11)).ColorAndOpacity(Accent)
			.Text_Lambda([this] { return FText::FromString(FString::Printf(TEXT("%d %s"), State.Kills, State.Kills == 1 ? TEXT("KILL") : TEXT("KILLS"))); })
		]
	];
	Rows->AddSlot().AutoHeight().Padding(0, 8, 0, 6)[Meter];

	// What the gun always carries, as small tags.
	TArray<FString> Standard = {TEXT("Torch"), FString::Printf(TEXT("%d-round magazine"), State.BaseMagazine)};
	if (!(State.Supported & EScctWeaponAttachment::Suppressor)) { Standard.Add(TEXT("Integral silencer")); }
	if (State.NativeOpticPower > 1.0f) { Standard.Add(FString::Printf(TEXT("%.0f× scope"), State.NativeOpticPower)); }
	const TSharedRef<SHorizontalBox> Tags = SNew(SHorizontalBox);
	Tags->AddSlot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
	[
		SNew(STextBlock).Font(ScctUi::Caps(10)).ColorAndOpacity(ScctHudStyle::TextFaint()).Text(FText::FromString(TEXT("STANDARD")))
	];
	for (const FString& Item : Standard)
	{
		Tags->AddSlot().AutoWidth().Padding(0, 0, 6, 0)
		[
			SNew(SBorder).BorderImage(Outlined(3.0f)).BorderBackgroundColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.18f))
			.Padding(FMargin(8.0f, 3.0f))
			[
				SNew(STextBlock).Font(ScctUi::Small()).ColorAndOpacity(ScctHudStyle::TextDim()).Text(FText::FromString(Item))
			]
		];
	}
	Rows->AddSlot().AutoHeight().Padding(0, 4, 0, 14)[Tags];

	// ---- one card per attachment the weapon takes, in unlock order
	TArray<int32, TInlineAllocator<UE_ARRAY_COUNT(AttachmentNames)>> SortedSlots;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(AttachmentNames); ++Index)
	{
		if (State.Supported & (1 << Index)) { SortedSlots.Add(Index); }
	}
	SortedSlots.Sort([this](int32 A, int32 B) { return State.KillsRequired[A] < State.KillsRequired[B]; });
	for (const int32 Index : SortedSlots)
	{
		Rows->AddSlot().AutoHeight().Padding(0, 0, 0, 6)[MakeCard(Index)];
	}
	Rows->AddSlot().AutoHeight().Padding(0, 4, 0, 0)
	[
		SNew(STextBlock).Font(ScctUi::Small()).ColorAndOpacity(ScctHudStyle::TextDim()).AutoWrapText(true)
		.Text_Lambda([this] { return FText::FromString(Notice); })
	];
	ChildSlot[Rows];
	RegisterActiveTimer(1.0f, FWidgetActiveTimerDelegate::CreateSP(this, &SScctAttachmentPanel::WatchProgress));
}

TSharedRef<SWidget> SScctAttachmentPanel::MakeCard(int32 Index)
{
	const int32 Bit = 1 << Index;
	const FLinearColor Accent = ScctHudStyle::Accent(1);
	auto Equipped = [this, Bit] { return (Mask & Bit) != 0; };
	auto Unlocked = [this, Bit] { return (State.Available & Bit) != 0; };

	// Cost as pips, lit while it is fitted.
	const TSharedRef<SHorizontalBox> Cost = SNew(SHorizontalBox);
	for (int32 Point = 0; Point < State.Costs[Index]; ++Point)
	{
		Cost->AddSlot().AutoWidth().Padding(0, 0, 3, 0).VAlign(VAlign_Center)
		[
			SNew(SBox).WidthOverride(14.0f).HeightOverride(6.0f)
			[
				SNew(SBorder).BorderImage(Rounded(1.5f))
				.BorderBackgroundColor_Lambda([Equipped, Accent] { return Equipped() ? Accent : FLinearColor(1.0f, 1.0f, 1.0f, 0.3f); })
			]
		];
	}
	Cost->AddSlot().AutoWidth().Padding(4, 0, 0, 0).VAlign(VAlign_Center)
	[
		SNew(STextBlock).Font(ScctUi::Caps(10)).ColorAndOpacity(ScctHudStyle::TextFaint())
		.Text(FText::FromString(State.Costs[Index] == 1 ? TEXT("1 PT") : FString::Printf(TEXT("%d PTS"), State.Costs[Index])))
	];

	// Right side: the action while it can be fitted or taken off, otherwise why not.
	const TSharedRef<SWidget> Progress = SNew(SVerticalBox)
		+ SVerticalBox::Slot().AutoHeight().HAlign(HAlign_Right)
		[
			SNew(STextBlock).Font(ScctUi::Caps(11)).ColorAndOpacity(ScctHudStyle::TextDim())
			.Text_Lambda([this, Index] { return FText::FromString(FString::Printf(TEXT("LOCKED   %d / %d KILLS"), FMath::Min(State.Kills, State.KillsRequired[Index]), State.KillsRequired[Index])); })
		]
		+ SVerticalBox::Slot().AutoHeight().Padding(0, 6, 0, 0)
		[
			SNew(SBox).WidthOverride(180.0f).HeightOverride(4.0f)
			[
				SNew(SOverlay)
				+ SOverlay::Slot()[SNew(SBorder).BorderImage(Rounded(2.0f)).BorderBackgroundColor(FLinearColor(1.0f, 1.0f, 1.0f, 0.1f))]
				+ SOverlay::Slot()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([this, Index] { return KillShare(Index); }))
					[
						SNew(SBorder).BorderImage(Rounded(2.0f)).BorderBackgroundColor(ScctHudStyle::Fade(Accent, 0.8f))
					]
					+ SHorizontalBox::Slot().FillWidth(TAttribute<float>::CreateLambda([this, Index] { return 1.0f - KillShare(Index); }))
				]
			]
		];
	const TSharedRef<SWidget> Action = SNew(SWidgetSwitcher)
		.WidgetIndex_Lambda([this, Index, Unlocked] { return CanToggle(Index) ? 0 : Unlocked() ? 1 : 2; })
		+ SWidgetSwitcher::Slot()
		[
			SNew(SScctButton).Look(EScctButtonLook::Secondary).MinWidth(150.0f).Colour(Accent)
			.Text_Lambda([this, Index] { return FText::FromString(ActionLabel(Index)); })
			.Description(FText::FromString(FString(AttachmentNames[Index]) + TEXT(": ") + AttachmentEffects[Index]))
			.OnPressed(FSimpleDelegate::CreateSP(this, &SScctAttachmentPanel::Toggle, Index))
		]
		+ SWidgetSwitcher::Slot().VAlign(VAlign_Center)
		[
			SNew(STextBlock).Font(ScctUi::Caps(11)).ColorAndOpacity(ScctHudStyle::TextDim()).Justification(ETextJustify::Right)
			.Text_Lambda([this, Index]
			{
				const int32 Shortfall = PointsUsed(SelectionAfterToggle(Index)) - State.Budget;
				return FText::FromString(FString::Printf(TEXT("NEEDS %d MORE %s"), Shortfall, Shortfall == 1 ? TEXT("POINT") : TEXT("POINTS")));
			})
		]
		+ SWidgetSwitcher::Slot().VAlign(VAlign_Center)[Progress];

	return SNew(SBorder).BorderImage(Rounded(5.0f)).Padding(0.0f)
		.BorderBackgroundColor_Lambda([Equipped, Accent]
		{
			return Equipped() ? FLinearColor(Accent.R, Accent.G, Accent.B, 0.14f) : FLinearColor(0.25f, 0.31f, 0.36f, 0.16f);
		})
		[
			SNew(SHorizontalBox)
			// A bar down the left edge: lit while fitted.
			+ SHorizontalBox::Slot().AutoWidth()
			[
				SNew(SBox).WidthOverride(4.0f)
				[
					SNew(SBorder).BorderImage(Rounded(2.0f))
					.BorderBackgroundColor_Lambda([Equipped, Unlocked, Accent]
					{
						return Equipped() ? Accent : Unlocked() ? FLinearColor(1.0f, 1.0f, 1.0f, 0.22f) : FLinearColor(1.0f, 1.0f, 1.0f, 0.06f);
					})
				]
			]
			+ SHorizontalBox::Slot().FillWidth(1.0f).Padding(FMargin(16.0f, 11.0f, 12.0f, 11.0f)).VAlign(VAlign_Center)
			[
				SNew(SVerticalBox)
				+ SVerticalBox::Slot().AutoHeight()
				[
					SNew(SHorizontalBox)
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(0, 0, 10, 0)
					[
						SNew(SBorder).BorderImage(Outlined(3.0f))
						.BorderBackgroundColor_Lambda([Equipped, Accent] { return Equipped() ? Accent : FLinearColor(1.0f, 1.0f, 1.0f, 0.25f); })
						.Padding(FMargin(6.0f, 1.0f))
						[
							SNew(STextBlock).Font(ScctUi::Caps(9)).Text(FText::FromString(AttachmentSlots[Index]))
							.ColorAndOpacity_Lambda([Equipped, Accent] { return FSlateColor(Equipped() ? Accent : ScctHudStyle::TextDim()); })
						]
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center)
					[
						SNew(STextBlock).Font(ScctUi::Body(true)).Text(FText::FromString(AttachmentNames[Index]))
						.ColorAndOpacity_Lambda([Unlocked] { return FSlateColor(Unlocked() ? ScctHudStyle::Text() : ScctHudStyle::TextDim()); })
					]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14, 0, 0, 0)[Cost]
					+ SHorizontalBox::Slot().AutoWidth().VAlign(VAlign_Center).Padding(14, 0, 0, 0)
					[
						SNew(STextBlock).Font(ScctUi::Caps(10)).ColorAndOpacity(Accent).Text(FText::FromString(TEXT("EQUIPPED")))
						.Visibility_Lambda([Equipped] { return Equipped() ? EVisibility::HitTestInvisible : EVisibility::Collapsed; })
					]
				]
				+ SVerticalBox::Slot().AutoHeight().Padding(0, 4, 0, 0)
				[
					SNew(STextBlock).Font(ScctUi::Small()).ColorAndOpacity(ScctHudStyle::TextDim())
					.Text(FText::FromString(AttachmentEffects[Index])).AutoWrapText(true)
				]
			]
			+ SHorizontalBox::Slot().AutoWidth().Padding(FMargin(8.0f, 8.0f, 14.0f, 8.0f)).VAlign(VAlign_Center).HAlign(HAlign_Right)
			[
				Action
			]
		];
}

EActiveTimerReturnType SScctAttachmentPanel::WatchProgress(double, float)
{
	const FScctWeaponAttachmentsApi* Api = ScctFeature<FScctWeaponAttachmentsApi>();
	FScctWeaponAttachmentState Current = {};
	if (Api && Api->GetState(*Weapon, &Current))
	{
		State = Current;
		if (!bDraft) { Mask = Current.Equipped; }
	}
	return EActiveTimerReturnType::Continue;
}

int32 SScctAttachmentPanel::PointsUsed(int32 Selection) const
{
	int32 Used = 0;
	for (int32 Index = 0; Index < UE_ARRAY_COUNT(AttachmentNames); ++Index)
	{
		if (Selection & (1 << Index)) { Used += State.Costs[Index]; }
	}
	return Used;
}

float SScctAttachmentPanel::KillShare(int32 Index) const
{
	const int32 Required = State.KillsRequired[Index];
	return Required > 0 ? FMath::Clamp(static_cast<float>(State.Kills) / Required, 0.0f, 1.0f) : 1.0f;
}

int32 SScctAttachmentPanel::SelectionAfterToggle(int32 Index) const
{
	const int32 Bit = 1 << Index;
	if (Mask & Bit) { return Mask & ~Bit; }
	return ((Bit & EScctWeaponAttachment::Optics) ? (Mask & ~EScctWeaponAttachment::Optics) : Mask) | Bit;
}

FString SScctAttachmentPanel::ActionLabel(int32 Index) const
{
	const int32 Bit = 1 << Index;
	if (Mask & Bit) { return TEXT("REMOVE"); }
	if ((Bit & EScctWeaponAttachment::Optics) && (Mask & EScctWeaponAttachment::Optics))
	{
		return Bit == EScctWeaponAttachment::Acog ? TEXT("REPLACE RED DOT") : TEXT("REPLACE ACOG");
	}
	return TEXT("EQUIP");
}

bool SScctAttachmentPanel::CanToggle(int32 Index) const
{
	const int32 Bit = 1 << Index;
	return (Mask & Bit) || ((State.Available & Bit) && PointsUsed(SelectionAfterToggle(Index)) <= State.Budget);
}

void SScctAttachmentPanel::Toggle(int32 Index)
{
	if (!CanToggle(Index)) { return; }
	const int32 Requested = SelectionAfterToggle(Index);
	if (OnChoose && OnChoose(Requested))
	{
		Mask = Requested;
		Notice.Reset();
	}
	else
	{
		Notice = TEXT("The attachment choice could not be saved.");
	}
}
