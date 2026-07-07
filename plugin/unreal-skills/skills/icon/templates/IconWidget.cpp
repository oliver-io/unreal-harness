// IconWidget — see IconWidget.h.

#include "IconWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/SizeBox.h"
#include "Components/Overlay.h"
#include "Components/OverlaySlot.h"
#include "Components/Image.h"
#include "Engine/Texture2D.h"

void UIconWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Root is a fixed-size box (set by SetIconSize) so this widget has a definite
	// desired size inside a horizontal box / canvas slot.
	Box = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("IconBox"));
	WidgetTree->RootWidget = Box;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	// Overlay carries the fractional content insets as padding on its single slot.
	Frame = WidgetTree->ConstructWidget<UOverlay>(UOverlay::StaticClass(), TEXT("IconFrame"));
	Frame->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	Box->AddChild(Frame);

	IconImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("IconImg"));
	IconImage->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	IconImage->SetColorAndOpacity(IconColor);
	if (Texture)
	{
		IconImage->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
	}
	UOverlaySlot* ImgSlot = Frame->AddChildToOverlay(IconImage);
	ImgSlot->SetHorizontalAlignment(HAlign_Fill);
	ImgSlot->SetVerticalAlignment(VAlign_Fill);

	ApplyLayout();
}

void UIconWidget::SetIcon(UTexture2D* InTexture)
{
	Texture = InTexture;
	if (Texture)
	{
		const FIntPoint S = Texture->GetImportedSize();
		if (S.Y > 0) Aspect = (float)S.X / (float)S.Y;
	}
	if (IconImage && Texture)
	{
		IconImage->SetBrushFromTexture(Texture, /*bMatchSize=*/false);
	}
	// Re-apply size so a changed aspect takes effect.
	const float H = CurrentBoxH;
	CurrentBoxH = -1.f;
	if (H > 0.f) SetIconSize(H);
}

void UIconWidget::SetIconColor(const FLinearColor& InColor)
{
	IconColor = InColor;
	if (IconImage) IconImage->SetColorAndOpacity(IconColor);
}

void UIconWidget::SetPreserveAspect(bool bInPreserve)
{
	if (bPreserve == bInPreserve) return;
	bPreserve = bInPreserve;
	const float H = CurrentBoxH;
	CurrentBoxH = -1.f;
	if (H > 0.f) SetIconSize(H);
}

void UIconWidget::SetContentInsets(const FMargin& InFractionInsets)
{
	InsetFrac = InFractionInsets;
	ApplyLayout();
}

void UIconWidget::SetIconSize(float BoxHeightPx)
{
	if (BoxHeightPx <= 0.f || FMath::IsNearlyEqual(BoxHeightPx, CurrentBoxH, 0.5f)) return;
	CurrentBoxH = BoxHeightPx;
	ApplyLayout();
}

void UIconWidget::ApplyLayout()
{
	if (CurrentBoxH <= 0.f) return;

	if (Box)
	{
		// Square by default; preserve-aspect makes width = height x texture aspect.
		const float BoxW = bPreserve ? CurrentBoxH * Aspect : CurrentBoxH;
		Box->SetWidthOverride(BoxW);
		Box->SetHeightOverride(CurrentBoxH);
	}
	if (Frame)
	{
		// Fractional insets → pixel padding that rescales with the box. Uses the box
		// height for vertical insets and the (aspect-adjusted) width for horizontal.
		const float BoxW = bPreserve ? CurrentBoxH * Aspect : CurrentBoxH;
		if (UOverlaySlot* ImgSlot = Cast<UOverlaySlot>(IconImage ? IconImage->Slot : nullptr))
		{
			ImgSlot->SetPadding(FMargin(
				InsetFrac.Left   * BoxW,
				InsetFrac.Top    * CurrentBoxH,
				InsetFrac.Right  * BoxW,
				InsetFrac.Bottom * CurrentBoxH));
		}
	}
}
