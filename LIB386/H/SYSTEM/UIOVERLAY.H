#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * UI overlay: true-colour shapes and text drawn over the presented frame at the
 * window's resolution, for the modern menu. The game fills a retained list in
 * frame coordinates (the same pixels it draws Log in); every present redraws the
 * list on top of the frame until it is cleared, so a menu that only redraws its
 * cursor row still shows every card. Nothing is drawn while the list is empty.
 *
 * Text is Nunito (SIL OFL 1.1) from a distance field atlas, rasterized at the
 * size it lands on screen. Strings are CP850, the encoding of the game's text.
 * Colours are 0xAARRGGBB.
 */

#ifdef __cplusplus
extern "C" {
#endif

#define UIOVERLAY_ALIGN_LEFT 0
#define UIOVERLAY_ALIGN_CENTER 1
#define UIOVERLAY_ALIGN_RIGHT 2

typedef struct {
    float Radius;      ///< corner radius
    U32 FillTop;       ///< vertical gradient of the fill
    U32 FillBottom;
    U32 Border;        ///< 0 alpha: no border
    float BorderWidth;
    U32 Glow;          ///< soft halo outside the card, 0 alpha: none
    float GlowSize;
    U32 Shadow;        ///< soft drop shadow, 0 alpha: none
    float ShadowX, ShadowY, ShadowBlur;
} T_UIOVERLAY_CARD;

/* Layers, drawn in this order: a page's own cards (inventory), the dialogue
   box above it, menus on top. Each screen clears and refills only its layer. */
#define UIOVERLAY_LAYER_PAGE 0
#define UIOVERLAY_LAYER_DIALOG 1
#define UIOVERLAY_LAYER_MENU 2
#define UIOVERLAY_LAYERS 3

/** Empties every layer: the next present shows the frame alone. */
void UiOverlay_Clear(void);

/** Empties one layer and makes it the one later calls add to. */
void UiOverlay_ClearLayer(S32 layer);

/** TRUE while the list holds something. */
S32 UiOverlay_Active(void);

/** A rounded card over the box [x0, x1] x [y0, y1]. */
void UiOverlay_Card(float x0, float y0, float x1, float y1, const T_UIOVERLAY_CARD *style);

/** A line of text whose capital letters are centred on y. size is the em size in
 *  frame pixels, bold 0 (regular) or 1 (heavier). When maxWidth > 0 a wider
 *  string is drawn smaller to fit. */
void UiOverlay_Text(float x, float y, const char *cp850, float size, U32 color, S32 align, S32 bold, float maxWidth);

/** Width of a string at size, in frame pixels. */
float UiOverlay_TextWidth(const char *cp850, float size);

/** A four-pointed sparkle centred on (x, y). */
void UiOverlay_Sparkle(float x, float y, float size, U32 color);

/** An RGBA image (w x h, 4 bytes a pixel, caller-owned and kept alive while
 *  listed) stretched over [x0, x1] x [y0, y1], filtered. version identifies its
 *  contents: the texture is uploaded again only when it changes. */
void UiOverlay_Image(float x0, float y0, float x1, float y1, const U8 *rgba, S32 w, S32 h, U32 version);

/** A chevron centred on (x, y), size tall, pointing left (dir -1), right (1),
 *  up (-2) or down (2). */
void UiOverlay_Chevron(float x, float y, float size, S32 dir, U32 color);

/** Draws the list over the frame; called by the window between the frame and the
 *  present. sdlRenderer is the SDL_Renderer. */
void UiOverlay_Render(void *sdlRenderer);

/** The renderer is going away: drop its textures. */
void UiOverlay_ReleaseTextures(void);

#ifdef __cplusplus
}
#endif
