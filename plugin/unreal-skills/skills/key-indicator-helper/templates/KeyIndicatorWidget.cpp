// KeyIndicatorWidget — see KeyIndicatorWidget.h.

#include "KeyIndicatorWidget.h"
#include "Blueprint/WidgetTree.h"
#include "Components/CanvasPanel.h"
#include "Components/CanvasPanelSlot.h"
#include "Components/SizeBox.h"
#include "Components/Image.h"
#include "Components/TextBlock.h"
#include "Engine/Texture2D.h"
#include "Styling/CoreStyle.h"   // FCoreStyle::GetDefaultFontStyle for the glyph

namespace
{
	// ── Reusable tuning (texture-independent — change only if your art demands it) ──

	// A capital letter's ink sits HIGH in its line box; nudge the anchor down by this
	// fraction of the box so the ink centers on the face rather than the line box.
	constexpr float InkDownFrac = 0.03f;

	// Glyph font size ÷ cap-box height. ~0.30 reads as a label on the face, not a
	// filled tile. (A clean bold sans reads sharper/"key-like" than an ornate face.)
	constexpr float FontOfBox = 0.30f;

	// Press-down CTA cycle (seconds): down → hold → release → pause → loop.
	constexpr float PressDownDur   = 0.16f;
	constexpr float PressHoldDur   = 0.10f;
	constexpr float PressUpDur     = 0.14f;
	constexpr float PressPause     = 0.55f;
	constexpr float PressDepthFrac = 0.11f;   // peak translate-down ÷ cap-box height

	// Default glyph ink for a light cap (near-black). Override via SetGlyphColor.
	const FLinearColor DefaultGlyphColor(0.11f, 0.094f, 0.078f, 1.0f);
}

void UKeyIndicatorWidget::NativeOnInitialized()
{
	Super::NativeOnInitialized();

	// Root is a fixed-size box (set by SetCapSize) so this widget has a definite
	// desired size inside a horizontal box / canvas slot.
	CapBox = WidgetTree->ConstructWidget<USizeBox>(USizeBox::StaticClass(), TEXT("CapBox"));
	WidgetTree->RootWidget = CapBox;
	SetVisibility(ESlateVisibility::SelfHitTestInvisible);

	CapPanel = WidgetTree->ConstructWidget<UCanvasPanel>(UCanvasPanel::StaticClass(), TEXT("CapPanel"));
	CapPanel->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	CapBox->AddChild(CapPanel);

	CapImage = WidgetTree->ConstructWidget<UImage>(UImage::StaticClass(), TEXT("CapImg"));
	CapImage->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	if (SquareTex)
	{
		CapImage->SetBrushFromTexture(SquareTex, /*bMatchSize=*/false);
	}
	UCanvasPanelSlot* ImgSlot = CapPanel->AddChildToCanvas(CapImage);
	ImgSlot->SetAnchors(FAnchors(0.f, 0.f, 1.f, 1.f));   // fill the box
	ImgSlot->SetOffsets(FMargin(0.f));

	KeyText = WidgetTree->ConstructWidget<UTextBlock>(UTextBlock::StaticClass(), TEXT("KeyGlyph"));
	KeyText->SetText(FText::FromString(TEXT("F")));
	KeyText->SetColorAndOpacity(FSlateColor(DefaultGlyphColor));
	KeyText->SetJustification(ETextJustify::Center);
	KeyText->SetVisibility(ESlateVisibility::SelfHitTestInvisible);
	UCanvasPanelSlot* TextSlot = CapPanel->AddChildToCanvas(KeyText);
	TextSlot->SetAlignment(FVector2D(0.5f, 0.5f));   // centered pivot on the anchor point
	TextSlot->SetAutoSize(true);
	ReanchorGlyph();
}

void UKeyIndicatorWidget::SetCapTextures(UTexture2D* InSquare, UTexture2D* InWide)
{
	SquareTex = InSquare;
	WideTex   = InWide;
	if (WideTex)
	{
		const FIntPoint WS = WideTex->GetImportedSize();
		if (WS.Y > 0) WideAspect = (float)WS.X / (float)WS.Y;
	}
	// Apply the texture matching the current label width.
	if (CapImage)
	{
		UTexture2D* T = (bWide && WideTex) ? WideTex : SquareTex;
		if (T) CapImage->SetBrushFromTexture(T, /*bMatchSize=*/false);
	}
	// Re-apply size so aspect (square vs wide) takes effect.
	const float H = CurrentBoxH;
	CurrentBoxH = -1.f;
	if (H > 0.f) SetCapSize(H);
}

void UKeyIndicatorWidget::SetFaceCenters(FVector2D InSquareFaceCenter, FVector2D InWideFaceCenter)
{
	SquareFaceCenter = InSquareFaceCenter;
	WideFaceCenter   = InWideFaceCenter;
	ReanchorGlyph();
}

void UKeyIndicatorWidget::SetGlyphColor(const FLinearColor& InColor)
{
	if (KeyText) KeyText->SetColorAndOpacity(FSlateColor(InColor));
}

void UKeyIndicatorWidget::ReanchorGlyph()
{
	if (!KeyText) return;
	if (UCanvasPanelSlot* TextSlot = Cast<UCanvasPanelSlot>(KeyText->Slot))
	{
		// Anchor at the measured top-face center, nudged DOWN by InkDownFrac so the
		// cap letter's ink (which sits high in its line box) lands on the face center.
		const FVector2D Face = bWide ? WideFaceCenter : SquareFaceCenter;
		TextSlot->SetAnchors(FAnchors(Face.X, Face.Y + InkDownFrac));
	}
}

void UKeyIndicatorWidget::SetGlyph(const FText& InGlyph)
{
	// Multi-character labels (SPACE, SHIFT, …) use the wide cap when one is set;
	// single characters (F, W, …) keep the square cap.
	const bool bNewWide = (InGlyph.ToString().Len() > 1) && (WideTex != nullptr);
	if (bNewWide != bWide)
	{
		bWide = bNewWide;
		if (CapImage)
		{
			if (UTexture2D* T = bWide ? WideTex : SquareTex)
			{
				CapImage->SetBrushFromTexture(T, /*bMatchSize=*/false);
			}
		}
		ReanchorGlyph();
		// Aspect changed → force the box size to re-apply.
		const float H = CurrentBoxH;
		CurrentBoxH = -1.f;
		if (H > 0.f) SetCapSize(H);
	}

	if (KeyText && !KeyText->GetText().EqualTo(InGlyph))
	{
		KeyText->SetText(InGlyph);
	}
}

void UKeyIndicatorWidget::SetGlyphFromKey(const FKey& Key)
{
	SetGlyph(KeyLabel(Key));
}

FText UKeyIndicatorWidget::KeyLabel(const FKey& Key)
{
	if (Key == EKeys::SpaceBar)                                  return FText::FromString(TEXT("SPACE"));
	if (Key == EKeys::LeftShift   || Key == EKeys::RightShift)   return FText::FromString(TEXT("SHIFT"));
	if (Key == EKeys::LeftControl || Key == EKeys::RightControl) return FText::FromString(TEXT("CTRL"));
	if (Key == EKeys::LeftAlt     || Key == EKeys::RightAlt)     return FText::FromString(TEXT("ALT"));
	if (Key == EKeys::Tab)              return FText::FromString(TEXT("TAB"));
	if (Key == EKeys::CapsLock)         return FText::FromString(TEXT("CAPS"));
	if (Key == EKeys::Enter)            return FText::FromString(TEXT("ENT"));
	if (Key == EKeys::Escape)           return FText::FromString(TEXT("ESC"));
	if (Key == EKeys::LeftMouseButton)  return FText::FromString(TEXT("LMB"));
	if (Key == EKeys::RightMouseButton) return FText::FromString(TEXT("RMB"));
	if (Key == EKeys::MiddleMouseButton)return FText::FromString(TEXT("MMB"));
	if (!Key.IsValid())                 return FText::FromString(TEXT("?"));
	const FString Name = Key.GetDisplayName(/*bLongDisplayName=*/false).ToString().ToUpper();
	return FText::FromString(Name.IsEmpty() ? TEXT("?") : Name);
}

void UKeyIndicatorWidget::SetCapSize(float BoxHeightPx)
{
	if (BoxHeightPx <= 0.f || FMath::IsNearlyEqual(BoxHeightPx, CurrentBoxH, 0.5f)) return;
	CurrentBoxH = BoxHeightPx;

	if (CapBox)
	{
		// Square cap is 1:1; wide cap keeps its texture aspect (width = H × aspect).
		const float BoxW = bWide ? BoxHeightPx * WideAspect : BoxHeightPx;
		CapBox->SetWidthOverride(BoxW);
		CapBox->SetHeightOverride(BoxHeightPx);
	}
	if (KeyText)
	{
		const int32 KeyFontPx = FMath::Max(8, FMath::RoundToInt(BoxHeightPx * FontOfBox));
		KeyText->SetFont(FCoreStyle::GetDefaultFontStyle("Bold", KeyFontPx));
	}
}

void UKeyIndicatorWidget::SetPressCTAEnabled(bool bEnabled)
{
	if (bPressCTA == bEnabled) return;
	bPressCTA = bEnabled;
	PressClock = 0.f;
	if (!bEnabled && CapPanel)
	{
		CapPanel->SetRenderTranslation(FVector2D::ZeroVector);
	}
}

void UKeyIndicatorWidget::NativeTick(const FGeometry& MyGeometry, float InDeltaTime)
{
	Super::NativeTick(MyGeometry, InDeltaTime);

	if (!bPressCTA || !CapPanel || CurrentBoxH <= 0.f) return;

	constexpr float Period = PressDownDur + PressHoldDur + PressUpDur + PressPause;
	PressClock += InDeltaTime;
	float t = FMath::Fmod(PressClock + PressPhaseOffset, Period);
	if (t < 0.f) t += Period;

	// Fraction of full press depth at this point in the cycle (0 = up).
	float Depth;
	if (t < PressDownDur)
	{
		const float a = t / PressDownDur;             // ease-out into the press
		Depth = 1.f - (1.f - a) * (1.f - a);
	}
	else if (t < PressDownDur + PressHoldDur)
	{
		Depth = 1.f;
	}
	else if (t < PressDownDur + PressHoldDur + PressUpDur)
	{
		const float a = (t - PressDownDur - PressHoldDur) / PressUpDur;
		Depth = 1.f - a;                              // linear release
	}
	else
	{
		Depth = 0.f;                                  // pause, cap at rest
	}

	CapPanel->SetRenderTranslation(FVector2D(0.f, Depth * CurrentBoxH * PressDepthFrac));
}
