// IconWidget — a generic, drop-in UI icon tile.
//
// Renders a single UTexture2D icon at a caller-driven size, with the bits that make
// a small UI icon look right baked in: aspect preservation, an optional runtime TINT
// (for single-color "glyph-mono" art whose alpha is the shape), and optional content
// insets so the icon sits inside a frame's drawable area instead of on its rim.
// Pure-C++ UMG: no UMG-asset / Blueprint authoring required. Companion to the `icon`
// skill (which generates + imports the texture) and the `key-indicator-helper` widget.
//
// ── What is project-specific (you supply it) ───────────────────────────────────
//   1. The icon TEXTURE — import it with the `icon` skill (sRGB + EditorIcon
//      compression for crisp small art), then pass it via SetIcon().
//   2. (Optional) CONTENT INSETS — if the icon sits inside a decorative frame, run
//      the `/see` skill on the frame, take content_recommended.insets, and pass them
//      via SetContentInsets() so the icon lands on the frame's face, not its border.
//   3. The module API macro: replace ICONWIDGET_API with YOURMODULE_API.
//
// ── What is reusable (baked in) ────────────────────────────────────────────────
//   • Caller-driven sizing: the consumer computes a box size from its own layout and
//     calls SetIconSize() each layout pass; everything scales from it (idempotent).
//   • Aspect: square by default; SetPreserveAspect(true) keeps the texture's imported
//     aspect (width = height x aspect) for non-square art.
//   • Runtime tint via SetIconColor() — leave white for full-color icons; set a color
//     for "glyph-mono" silhouettes you recolor per state (enabled/disabled, team, …).
//   • Content insets applied as fractional padding that rescales with the box.
//
// See SKILL.md (icon) for generation/import and the `/see` workflow for insets.

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "Layout/Margin.h"
#include "IconWidget.generated.h"

class UImage;
class USizeBox;
class UOverlay;
class UTexture2D;

UCLASS()
class ICONWIDGET_API UIconWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** The icon texture. Captures the imported aspect (used when aspect is preserved).
	 *  Call once after Create, and again to swap the icon. */
	void SetIcon(UTexture2D* InTexture);

	/** Icon tint. White (default) shows full-color art as-is; a color recolors a
	 *  single-color "glyph-mono" silhouette (alpha is the shape) per state/team. */
	void SetIconColor(const FLinearColor& InColor);

	/** Keep the texture's imported aspect (width = height x aspect). Off ⇒ square. */
	void SetPreserveAspect(bool bInPreserve);

	/** Fractional content insets (L/T/R/B as fractions of the box) — typically the
	 *  `/see` content_recommended.insets of a frame the icon sits inside. Default 0. */
	void SetContentInsets(const FMargin& InFractionInsets);

	/** Box height in slate units; the icon (and insets) scale from it. Idempotent —
	 *  only re-applies when the size actually changes. Call each layout pass. */
	void SetIconSize(float BoxHeightPx);

protected:
	virtual void NativeOnInitialized() override;

private:
	void ApplyLayout();   // re-applies box size, aspect, and fractional insets

	UPROPERTY() TObjectPtr<USizeBox> Box;       // root: fixed desired size
	UPROPERTY() TObjectPtr<UOverlay> Frame;     // holds the image; carries inset padding
	UPROPERTY() TObjectPtr<UImage>   IconImage;

	UPROPERTY() TObjectPtr<UTexture2D> Texture;
	FLinearColor IconColor   = FLinearColor::White;
	FMargin      InsetFrac   = FMargin(0.f);   // L/T/R/B as fractions of the box
	float        Aspect      = 1.0f;           // texture width/height
	bool         bPreserve   = false;          // preserve texture aspect?
	float        CurrentBoxH = -1.f;           // last applied box height (slate px)
};
