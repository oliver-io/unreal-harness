// KeyIndicatorWidget — a generic, drop-in "press this key" indicator.
//
// Renders a single key-shaped tile (a keycap / square / pill texture you supply)
// with a glyph painted on its TOP FACE — e.g. "F", "W", "SPACE". Genericized from a
// proven implementation; it owns the hard part (placing a glyph on a beveled cap's
// face, not its box center) and the resolution-relative font so every consumer gets
// an identical cap. Pure-C++ UMG: no UMG-asset / Blueprint authoring required, and it
// paints crisp Slate fonts the immediate-mode HUD canvas cannot.
//
// ── What is project-specific (you MUST supply it) ──────────────────────────────
//   1. The cap TEXTURES — a square cap (single-char keys) and an optional wide cap
//      (multi-char labels like SPACE/SHIFT). Pass them via SetCapTextures().
//   2. The measured FACE-CENTER fractions for each texture — run the `/see` skill on
//      your cap art, confirm the top-face center on its annotated PNG, and pass the
//      (cx, cy) fractions via SetFaceCenters(). The defaults below fit a *typical*
//      front-on beveled keycap, but every texture differs — measure yours.
//   3. The module API macro: replace KEYINDICATOR_API with YOURMODULE_API.
//
// ── What is reusable (baked in, tune only if needed) ───────────────────────────
//   • Anchor the glyph to the measured face center (NOT the box center), nudged down
//     a hair so a capital letter's ink lands ON the face.
//   • Font size = cap-box height × FontOfBox (~0.30) so the glyph reads as a label.
//   • Square cap for 1-char labels; wide cap (texture aspect preserved) for multi-char.
//   • Caller-driven sizing: the consumer computes a cap-box height from its own layout
//     and calls SetCapSize() each layout pass; everything scales from it.
//   • Optional looping press-down call-to-action, with a per-cap phase offset so a row
//     ripples (W→A→S→D) instead of pressing in unison.
//
// See SKILL.md (key-indicator-helper) for the method, the /see workflow, and the
// layout math a consumer uses to drive SetCapSize().

#pragma once

#include "CoreMinimal.h"
#include "Blueprint/UserWidget.h"
#include "KeyIndicatorWidget.generated.h"

class UImage;
class UTextBlock;
class USizeBox;
class UCanvasPanel;
class UTexture2D;

UCLASS()
class KEYINDICATOR_API UKeyIndicatorWidget : public UUserWidget
{
	GENERATED_BODY()

public:
	/** Cap art. Square = single-char keys; Wide = multi-char labels (optional — if
	 *  null, multi-char labels stay on the square cap). Call once after Create. */
	void SetCapTextures(UTexture2D* InSquare, UTexture2D* InWide);

	/** Measured top-face center as canvas fractions, from the `/see` skill. Square
	 *  applies to 1-char labels; Wide to multi-char. Defaults fit a typical beveled
	 *  cap — measure YOUR texture and pass the real numbers. */
	void SetFaceCenters(FVector2D InSquareFaceCenter, FVector2D InWideFaceCenter);

	/** Glyph ink color (default near-black for a light cap). */
	void SetGlyphColor(const FLinearColor& InColor);

	/** Glyph painted on the cap's top face (e.g. "F", "W", "SPACE"). Multi-char labels
	 *  switch to the wide cap (if set). */
	void SetGlyph(const FText& InGlyph);

	/** Convenience: set the glyph from an FKey, mapped to a terse cap label (SPACE,
	 *  SHIFT, LMB, …) via KeyLabel(). To show the player's *bound* (rebindable) key,
	 *  resolve the FKey from your input/settings system first, then pass it here. */
	void SetGlyphFromKey(const FKey& Key);

	/** Terse, cap-sized label for an FKey ("SPACE", "SHIFT", "CTRL", "LMB", "ENT", or a
	 *  single uppercase letter). Static so a controls table can share it. */
	static FText KeyLabel(const FKey& Key);

	/** Cap-box height in slate units; the on-face font scales from it. Idempotent —
	 *  only re-applies when the size actually changes. Call each layout pass. */
	void SetCapSize(float BoxHeightPx);

	/** Looping press-down call-to-action (translate the cap down → hold → release →
	 *  pause). Off by default. */
	void SetPressCTAEnabled(bool bEnabled);

	/** Phase-offset the press CTA so a row of caps ripples instead of pressing in
	 *  unison (e.g. W→A→S→D). Seconds added to this cap's cycle clock. */
	void SetPressPhaseOffset(float Seconds) { PressPhaseOffset = Seconds; }

protected:
	virtual void NativeOnInitialized() override;
	virtual void NativeTick(const FGeometry& MyGeometry, float InDeltaTime) override;

private:
	void ReanchorGlyph();   // re-applies the face-center anchor for the current cap

	UPROPERTY() TObjectPtr<USizeBox>     CapBox;
	UPROPERTY() TObjectPtr<UCanvasPanel> CapPanel;   // holds image + glyph; render-translated for the CTA
	UPROPERTY() TObjectPtr<UImage>       CapImage;
	UPROPERTY() TObjectPtr<UTextBlock>   KeyText;

	// Square cap (single-char keys) vs. wide cap (multi-char keys). The widget swaps
	// texture + box aspect + glyph anchor by label length.
	UPROPERTY() TObjectPtr<UTexture2D>   SquareTex;
	UPROPERTY() TObjectPtr<UTexture2D>   WideTex;
	bool  bWide       = false;
	float WideAspect  = 3.0f;        // WideTex width/height, computed in SetCapTextures

	// Measured top-face centers (canvas fractions) — defaults for a typical beveled cap.
	FVector2D SquareFaceCenter = FVector2D(0.50f, 0.41f);
	FVector2D WideFaceCenter   = FVector2D(0.50f, 0.46f);

	float CurrentBoxH      = -1.f;   // last applied cap-box height (slate px); -1 until first SetCapSize
	bool  bPressCTA        = false;
	float PressClock       = 0.f;    // seconds into the looping press cycle
	float PressPhaseOffset = 0.f;
};
