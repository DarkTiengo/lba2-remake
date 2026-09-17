#include <SYSTEM/UIOVERLAY.H>

#include <SYSTEM/MOUSE.H>
#include <SYSTEM/STRING.H>

#include "FONTS/NUNITO_SDF.H"

#include <SDL3/SDL.h>
#include <math.h>
#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_ITEMS 256
#define MAX_TEXT 160
#define CORNER_STEPS 8
#define CONTOUR_POINTS (4 * (CORNER_STEPS + 1))
#define CAP_HEIGHT 0.705f ///< Nunito's capital height, in ems
#define MAX_FONT_TEXTURES 4

enum { ITEM_CARD,
       ITEM_TEXT,
       ITEM_SPARKLE,
       ITEM_CHEVRON,
       ITEM_IMAGE };

typedef struct {
    S32 Kind;
    S32 Layer;
    float X0, Y0, X1, Y1;
    T_UIOVERLAY_CARD Card;
    char Text[MAX_TEXT];
    float Size;
    U32 Color;
    S32 Align;
    S32 Bold;
    float MaxWidth;
    const U8 *Pixels;
    S32 PixelsW, PixelsH;
    U32 Version;
} T_ITEM;

typedef struct {
    SDL_Texture *Texture;
    S32 Size; ///< em size in output pixels
    S32 Bold;
    U32 LastUse;
} T_FONT_TEXTURE;

// --- Private state -----------------------------------------------------------
static T_ITEM s_items[MAX_ITEMS];
static S32 s_count = 0;
static S32 s_layer = UIOVERLAY_LAYER_MENU; ///< layer new items go to

static T_FONT_TEXTURE s_fonts[MAX_FONT_TEXTURES];

/* One cached image texture: the menus show one picture at a time. */
static SDL_Texture *s_imageTexture = NULL;
static const U8 *s_imagePixels = NULL;
static S32 s_imageW = 0, s_imageH = 0;
static U32 s_imageVersion = 0;
static SDL_Renderer *s_fontRenderer = NULL;
static U32 s_frameCounter = 0;

static U16 s_cp850ToUnicode[256];
static S32 s_cp850Ready = FALSE;

/* Output transform: frame pixels to window pixels. */
static float s_scale = 1.0f;
static float s_offsetX = 0.0f;
static float s_offsetY = 0.0f;

static SDL_Vertex s_verts[4096];
static int s_indices[8192];
static S32 s_nbVerts = 0;
static S32 s_nbIndices = 0;

// --- Private functions -------------------------------------------------------
static void InitCp850(void) {
    if (s_cp850Ready) {
        return;
    }
    for (S32 i = 0; i < 128; i++) {
        s_cp850ToUnicode[i] = (U16)i;
    }
    for (U32 cp = 0xA0; cp < 0x2600; cp++) {
        const U8 c = UnicodeToCp850(cp);
        if (c >= 128 && s_cp850ToUnicode[c] == 0) {
            s_cp850ToUnicode[c] = (U16)cp;
        }
    }
    s_cp850Ready = TRUE;
}

static const T_NUNITO_GLYPH *FindGlyph(U32 code) {
    S32 lo = 0;
    S32 hi = NUNITO_SDF_GLYPHS - 1;
    while (lo <= hi) {
        const S32 mid = (lo + hi) / 2;
        if (NunitoGlyphs[mid].Code == code) {
            return &NunitoGlyphs[mid];
        }
        if (NunitoGlyphs[mid].Code < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static const T_NUNITO_GLYPH *GlyphOf(U8 c) {
    InitCp850();
    const T_NUNITO_GLYPH *g = FindGlyph(s_cp850ToUnicode[c]);
    return g != NULL ? g : FindGlyph('?');
}

static SDL_FColor ToColor(U32 argb, float alphaScale) {
    SDL_FColor c;
    c.r = (float)((argb >> 16) & 0xFF) / 255.0f;
    c.g = (float)((argb >> 8) & 0xFF) / 255.0f;
    c.b = (float)(argb & 0xFF) / 255.0f;
    c.a = (float)((argb >> 24) & 0xFF) / 255.0f * alphaScale;
    return c;
}

static SDL_FColor Mix(SDL_FColor a, SDL_FColor b, float t) {
    SDL_FColor c;
    c.r = a.r + (b.r - a.r) * t;
    c.g = a.g + (b.g - a.g) * t;
    c.b = a.b + (b.b - a.b) * t;
    c.a = a.a + (b.a - a.a) * t;
    return c;
}

static void Flush(SDL_Renderer *renderer, SDL_Texture *texture) {
    if (s_nbIndices > 0) {
        SDL_RenderGeometry(renderer, texture, s_verts, s_nbVerts, s_indices, s_nbIndices);
    }
    s_nbVerts = 0;
    s_nbIndices = 0;
}

static S32 Reserve(SDL_Renderer *renderer, SDL_Texture *texture, S32 verts, S32 indices) {
    if (s_nbVerts + verts > (S32)SDL_arraysize(s_verts) || s_nbIndices + indices > (S32)SDL_arraysize(s_indices)) {
        Flush(renderer, texture);
    }
    return verts <= (S32)SDL_arraysize(s_verts) && indices <= (S32)SDL_arraysize(s_indices);
}

/* Rounded-rectangle outline pushed out by offset (in output pixels). */
static void Contour(float x0, float y0, float x1, float y1, float radius, float offset, SDL_FPoint *out) {
    static const float cornerAngle[4] = {180.0f, 270.0f, 0.0f, 90.0f};
    const float r = SDL_min(radius, SDL_min((x1 - x0) * 0.5f, (y1 - y0) * 0.5f));
    const float cx[4] = {x0 + r, x1 - r, x1 - r, x0 + r};
    const float cy[4] = {y0 + r, y0 + r, y1 - r, y1 - r};
    const float rr = SDL_max(r + offset, 0.0f);
    S32 n = 0;
    for (S32 k = 0; k < 4; k++) {
        for (S32 i = 0; i <= CORNER_STEPS; i++) {
            const float a = (cornerAngle[k] + 90.0f * (float)i / (float)CORNER_STEPS) * (float)M_PI / 180.0f;
            /* A corner smaller than the offset still moves the edge outwards. */
            const float grow = r + offset < 0.0f ? r + offset : 0.0f;
            out[n].x = cx[k] + (float)cos(a) * rr + (float)cos(a) * grow;
            out[n].y = cy[k] + (float)sin(a) * rr + (float)sin(a) * grow;
            n++;
        }
    }
}

typedef struct {
    float Y0, Y1;
    SDL_FColor Top, Bottom;
} T_GRADIENT;

static SDL_FColor Shade(const T_GRADIENT *g, float y) {
    const float t = g->Y1 > g->Y0 ? SDL_clamp((y - g->Y0) / (g->Y1 - g->Y0), 0.0f, 1.0f) : 0.0f;
    return Mix(g->Top, g->Bottom, t);
}

/* The band between two outlines of the same card, each with its own colour
   ramp (alpha 0 on one side gives an antialiased or blurred edge). */
static void Ring(SDL_Renderer *renderer, float x0, float y0, float x1, float y1, float radius, float offA,
                 const T_GRADIENT *colA, float offB, const T_GRADIENT *colB) {
    SDL_FPoint a[CONTOUR_POINTS];
    SDL_FPoint b[CONTOUR_POINTS];
    Contour(x0, y0, x1, y1, radius, offA, a);
    Contour(x0, y0, x1, y1, radius, offB, b);
    if (!Reserve(renderer, NULL, CONTOUR_POINTS * 2, CONTOUR_POINTS * 6)) {
        return;
    }
    const S32 base = s_nbVerts;
    for (S32 i = 0; i < CONTOUR_POINTS; i++) {
        SDL_Vertex *va = &s_verts[s_nbVerts++];
        va->position = a[i];
        va->color = Shade(colA, a[i].y);
        va->tex_coord.x = va->tex_coord.y = 0.0f;
        SDL_Vertex *vb = &s_verts[s_nbVerts++];
        vb->position = b[i];
        vb->color = Shade(colB, b[i].y);
        vb->tex_coord.x = vb->tex_coord.y = 0.0f;
    }
    for (S32 i = 0; i < CONTOUR_POINTS; i++) {
        const S32 j = (i + 1) % CONTOUR_POINTS;
        const int ia = base + i * 2, ib = base + i * 2 + 1, ja = base + j * 2, jb = base + j * 2 + 1;
        s_indices[s_nbIndices++] = ia;
        s_indices[s_nbIndices++] = ib;
        s_indices[s_nbIndices++] = jb;
        s_indices[s_nbIndices++] = ia;
        s_indices[s_nbIndices++] = jb;
        s_indices[s_nbIndices++] = ja;
    }
}

/* The inside of an outline, as a fan from the card's centre. */
static void Fill(SDL_Renderer *renderer, float x0, float y0, float x1, float y1, float radius, float offset,
                 const T_GRADIENT *col) {
    SDL_FPoint p[CONTOUR_POINTS];
    Contour(x0, y0, x1, y1, radius, offset, p);
    if (!Reserve(renderer, NULL, CONTOUR_POINTS + 1, CONTOUR_POINTS * 3)) {
        return;
    }
    const S32 base = s_nbVerts;
    SDL_Vertex *c = &s_verts[s_nbVerts++];
    c->position.x = (x0 + x1) * 0.5f;
    c->position.y = (y0 + y1) * 0.5f;
    c->color = Shade(col, c->position.y);
    c->tex_coord.x = c->tex_coord.y = 0.0f;
    for (S32 i = 0; i < CONTOUR_POINTS; i++) {
        SDL_Vertex *v = &s_verts[s_nbVerts++];
        v->position = p[i];
        v->color = Shade(col, p[i].y);
        v->tex_coord.x = v->tex_coord.y = 0.0f;
    }
    for (S32 i = 0; i < CONTOUR_POINTS; i++) {
        s_indices[s_nbIndices++] = base;
        s_indices[s_nbIndices++] = base + 1 + i;
        s_indices[s_nbIndices++] = base + 1 + (i + 1) % CONTOUR_POINTS;
    }
}

static T_GRADIENT Flat(float y0, float y1, SDL_FColor c) {
    T_GRADIENT g;
    g.Y0 = y0;
    g.Y1 = y1;
    g.Top = c;
    g.Bottom = c;
    return g;
}

static void DrawCard(SDL_Renderer *renderer, const T_ITEM *item) {
    const T_UIOVERLAY_CARD *s = &item->Card;
    const float k = s_scale;
    const float x0 = s_offsetX + item->X0 * k;
    const float y0 = s_offsetY + item->Y0 * k;
    const float x1 = s_offsetX + item->X1 * k;
    const float y1 = s_offsetY + item->Y1 * k;
    const float radius = s->Radius * k;

    if ((s->Shadow >> 24) != 0) {
        const float blur = SDL_max(s->ShadowBlur * k, 1.0f);
        const float dx = s->ShadowX * k;
        const float dy = s->ShadowY * k;
        const SDL_FColor c = ToColor(s->Shadow, 1.0f);
        SDL_FColor clear = c;
        clear.a = 0.0f;
        const T_GRADIENT solid = Flat(y0 + dy, y1 + dy, c);
        const T_GRADIENT none = Flat(y0 + dy, y1 + dy, clear);
        Fill(renderer, x0 + dx, y0 + dy, x1 + dx, y1 + dy, radius, -blur * 0.5f, &solid);
        Ring(renderer, x0 + dx, y0 + dy, x1 + dx, y1 + dy, radius, -blur * 0.5f, &solid, blur, &none);
    }

    if ((s->Glow >> 24) != 0 && s->GlowSize > 0.0f) {
        const SDL_FColor c = ToColor(s->Glow, 1.0f);
        SDL_FColor clear = c;
        clear.a = 0.0f;
        const T_GRADIENT solid = Flat(y0, y1, c);
        const T_GRADIENT none = Flat(y0, y1, clear);
        Ring(renderer, x0, y0, x1, y1, radius, 0.0f, &solid, s->GlowSize * k, &none);
    }

    T_GRADIENT fill;
    fill.Y0 = y0;
    fill.Y1 = y1;
    fill.Top = ToColor(s->FillTop, 1.0f);
    fill.Bottom = ToColor(s->FillBottom, 1.0f);
    T_GRADIENT fillEdge = fill;
    fillEdge.Top.a = 0.0f;
    fillEdge.Bottom.a = 0.0f;
    Fill(renderer, x0, y0, x1, y1, radius, -0.5f, &fill);
    Ring(renderer, x0, y0, x1, y1, radius, -0.5f, &fill, 0.5f, &fillEdge);

    if ((s->Border >> 24) != 0 && s->BorderWidth > 0.0f) {
        const float bw = SDL_max(s->BorderWidth * k, 1.0f);
        const SDL_FColor c = ToColor(s->Border, 1.0f);
        SDL_FColor clear = c;
        clear.a = 0.0f;
        const T_GRADIENT solid = Flat(y0, y1, c);
        const T_GRADIENT none = Flat(y0, y1, clear);
        Ring(renderer, x0, y0, x1, y1, radius, -bw - 0.5f, &none, -bw + 0.5f, &solid);
        Ring(renderer, x0, y0, x1, y1, radius, -bw + 0.5f, &solid, -0.5f, &solid);
        Ring(renderer, x0, y0, x1, y1, radius, -0.5f, &solid, 0.5f, &none);
    }
}

static void DrawSparkle(SDL_Renderer *renderer, const T_ITEM *item) {
    const float k = s_scale;
    const float cx = s_offsetX + item->X0 * k;
    const float cy = s_offsetY + item->Y0 * k;
    const float r = item->Size * k * 0.5f;
    const float w = r * 0.28f;
    const SDL_FColor c = ToColor(item->Color, 1.0f);
    SDL_FColor clear = c;
    clear.a = 0.0f;
    static const float dirX[4] = {0.0f, 1.0f, 0.0f, -1.0f};
    static const float dirY[4] = {-1.0f, 0.0f, 1.0f, 0.0f};
    if (!Reserve(renderer, NULL, 1 + 8 + 8, 16 * 3)) {
        return;
    }
    const S32 base = s_nbVerts;
    SDL_Vertex *centre = &s_verts[s_nbVerts++];
    centre->position.x = cx;
    centre->position.y = cy;
    centre->color = c;
    centre->tex_coord.x = centre->tex_coord.y = 0.0f;
    /* Points alternate: a tip, then a waist between tips. */
    for (S32 i = 0; i < 4; i++) {
        SDL_Vertex *tip = &s_verts[s_nbVerts++];
        tip->position.x = cx + dirX[i] * r;
        tip->position.y = cy + dirY[i] * r;
        tip->color = clear;
        tip->tex_coord.x = tip->tex_coord.y = 0.0f;
        SDL_Vertex *waist = &s_verts[s_nbVerts++];
        waist->position.x = cx + (dirX[i] + dirX[(i + 1) % 4]) * w;
        waist->position.y = cy + (dirY[i] + dirY[(i + 1) % 4]) * w;
        waist->color = c;
        waist->tex_coord.x = waist->tex_coord.y = 0.0f;
    }
    for (S32 i = 0; i < 8; i++) {
        s_indices[s_nbIndices++] = base;
        s_indices[s_nbIndices++] = base + 1 + i;
        s_indices[s_nbIndices++] = base + 1 + (i + 1) % 8;
    }
}

/* A thick stroke from a to b, extended by half its width at both ends. */
static void Stroke(SDL_Renderer *renderer, float ax, float ay, float bx, float by, float width, SDL_FColor c) {
    const float dx = bx - ax, dy = by - ay;
    const float len = SDL_max((float)sqrt(dx * dx + dy * dy), 0.001f);
    const float ux = dx / len * width * 0.5f, uy = dy / len * width * 0.5f;
    const float nx = -uy, ny = ux;
    SDL_FColor clear = c;
    clear.a = 0.0f;
    if (!Reserve(renderer, NULL, 8, 18)) {
        return;
    }
    const S32 base = s_nbVerts;
    /* Core quad, then a one-pixel transparent fringe on both long sides. */
    const float fx = nx / SDL_max(width * 0.5f, 0.001f), fy = ny / SDL_max(width * 0.5f, 0.001f);
    const float px[8] = {ax - ux + nx, bx + ux + nx, bx + ux - nx, ax - ux - nx,
                         ax - ux + nx + fx, bx + ux + nx + fx, bx + ux - nx - fx, ax - ux - nx - fx};
    const float py[8] = {ay - uy + ny, by + uy + ny, by + uy - ny, ay - uy - ny,
                         ay - uy + ny + fy, by + uy + ny + fy, by + uy - ny - fy, ay - uy - ny - fy};
    for (S32 i = 0; i < 8; i++) {
        SDL_Vertex *v = &s_verts[s_nbVerts++];
        v->position.x = px[i];
        v->position.y = py[i];
        v->color = i < 4 ? c : clear;
        v->tex_coord.x = v->tex_coord.y = 0.0f;
    }
    static const int tri[6][3] = {{0, 1, 2}, {0, 2, 3}, {4, 5, 1}, {4, 1, 0}, {3, 2, 6}, {3, 6, 7}};
    for (S32 i = 0; i < 6; i++) {
        s_indices[s_nbIndices++] = base + tri[i][0];
        s_indices[s_nbIndices++] = base + tri[i][1];
        s_indices[s_nbIndices++] = base + tri[i][2];
    }
}

static void DrawChevron(SDL_Renderer *renderer, const T_ITEM *item) {
    const float k = s_scale;
    const float cx = s_offsetX + item->X0 * k;
    const float cy = s_offsetY + item->Y0 * k;
    const float h = item->Size * k * 0.5f;
    const float width = SDL_max(item->Size * k * 0.2f, 2.0f);
    const SDL_FColor c = ToColor(item->Color, 1.0f);
    if (item->Align == -2 || item->Align == 2) {
        /* Vertical: the tip up (-2) or down (2). */
        const float w = h * 0.62f * (item->Align < 0 ? -1.0f : 1.0f);
        const float tipY = cy + w * 0.5f;
        Stroke(renderer, cx - h, cy - w * 0.5f, cx, tipY, width, c);
        Stroke(renderer, cx, tipY, cx + h, cy - w * 0.5f, width, c);
        return;
    }
    const float w = h * 0.62f * (item->Align < 0 ? -1.0f : 1.0f);
    const float tipX = cx + w * 0.5f;
    Stroke(renderer, cx - w * 0.5f, cy - h, tipX, cy, width, c);
    Stroke(renderer, tipX, cy, cx - w * 0.5f, cy + h, width, c);
}

/* An arrow pointer at the mouse, in place of the software cursor the overlay
   would cover. */
static void DrawPointer(SDL_Renderer *renderer) {
    if (!MousePointerVisible()) {
        return;
    }
    const float k = SDL_max(s_scale, 1.0f);
    /* The window's own mouse position: finer than the frame's pixels when the
       frame is magnified, and current at the moment of the present. */
    float mx = s_offsetX + (float)MouseX * s_scale;
    float my = s_offsetY + (float)MouseY * s_scale;
    float wx = 0.0f, wy = 0.0f;
    SDL_GetMouseState(&wx, &wy);
    float rx = 0.0f, ry = 0.0f;
    if (SDL_RenderCoordinatesFromWindow(renderer, wx, wy, &rx, &ry)) {
        mx = rx;
        my = ry;
    }
    static const float shape[7][2] = {{0, 0}, {0, 17}, {4.5f, 13}, {7.5f, 19.5f}, {10, 18.5f}, {7, 12}, {12.5f, 12}};
    const float outline = 1.6f;
    const SDL_FColor dark = {0.05f, 0.07f, 0.12f, 0.95f};
    const SDL_FColor light = {1.0f, 1.0f, 1.0f, 1.0f};
    for (S32 pass = 0; pass < 2; pass++) {
        if (!Reserve(renderer, NULL, 8, 7 * 3)) {
            return;
        }
        const S32 base = s_nbVerts;
        /* Centroid-based inflate for the outline pass. */
        const float cx = 5.0f, cy = 11.0f;
        for (S32 i = 0; i < 7; i++) {
            float px = shape[i][0], py = shape[i][1];
            if (pass == 0) {
                const float dx = px - cx, dy = py - cy;
                const float len = SDL_max((float)sqrt(dx * dx + dy * dy), 0.001f);
                px += dx / len * outline;
                py += dy / len * outline;
            }
            SDL_Vertex *v = &s_verts[s_nbVerts++];
            v->position.x = mx + px * k;
            v->position.y = my + py * k;
            v->color = pass == 0 ? dark : light;
            v->tex_coord.x = v->tex_coord.y = 0.0f;
        }
        /* Fan around the waist vertex (5), which sees every other corner. */
        static const int tri[7][3] = {{5, 6, 0}, {5, 0, 1}, {5, 1, 2}, {5, 2, 3}, {5, 3, 4}, {0, 0, 0}, {0, 0, 0}};
        for (S32 t = 0; t < 5; t++) {
            s_indices[s_nbIndices++] = base + tri[t][0];
            s_indices[s_nbIndices++] = base + tri[t][1];
            s_indices[s_nbIndices++] = base + tri[t][2];
        }
        Flush(renderer, NULL);
    }
}

/* The distance field turned into coverage at an em size of size output pixels. */
static SDL_Texture *FontTexture(SDL_Renderer *renderer, S32 size, S32 bold) {
    if (s_fontRenderer != renderer) {
        UiOverlay_ReleaseTextures();
        s_fontRenderer = renderer;
    }
    T_FONT_TEXTURE *slot = &s_fonts[0];
    for (S32 i = 0; i < MAX_FONT_TEXTURES; i++) {
        if (s_fonts[i].Texture != NULL && s_fonts[i].Size == size && s_fonts[i].Bold == bold) {
            s_fonts[i].LastUse = s_frameCounter;
            return s_fonts[i].Texture;
        }
        if (s_fonts[i].Texture == NULL || s_fonts[i].LastUse < slot->LastUse) {
            slot = &s_fonts[i];
        }
    }
    if (slot->Texture != NULL) {
        SDL_DestroyTexture(slot->Texture);
        slot->Texture = NULL;
    }

    const float k = (float)size / (float)NUNITO_SDF_EM;
    const S32 w = SDL_max((S32)ceil(NUNITO_SDF_ATLAS_W * k), 1);
    const S32 h = SDL_max((S32)ceil(NUNITO_SDF_ATLAS_H * k), 1);
    SDL_Surface *surface = SDL_CreateSurface(w, h, SDL_PIXELFORMAT_ABGR8888);
    if (surface == NULL) {
        return NULL;
    }
    /* Field units per output pixel of distance, and how far a bold face grows. */
    const float toPixels = (float)NUNITO_SDF_SPREAD / 127.0f * k;
    const float grow = bold ? SDL_max(0.35f, 0.022f * (float)size) : 0.0f;
    U8 *pixels = (U8 *)surface->pixels;
    for (S32 y = 0; y < h; y++) {
        const float fy = SDL_min(((float)y + 0.5f) / k - 0.5f, (float)(NUNITO_SDF_ATLAS_H - 1));
        const S32 iy = SDL_max((S32)fy, 0);
        const S32 iy1 = SDL_min(iy + 1, NUNITO_SDF_ATLAS_H - 1);
        const float ty = SDL_max(fy - (float)iy, 0.0f);
        U8 *row = pixels + y * surface->pitch;
        for (S32 x = 0; x < w; x++) {
            const float fx = SDL_min(((float)x + 0.5f) / k - 0.5f, (float)(NUNITO_SDF_ATLAS_W - 1));
            const S32 ix = SDL_max((S32)fx, 0);
            const S32 ix1 = SDL_min(ix + 1, NUNITO_SDF_ATLAS_W - 1);
            const float tx = SDL_max(fx - (float)ix, 0.0f);
            const float a = NunitoAtlas[iy * NUNITO_SDF_ATLAS_W + ix];
            const float b = NunitoAtlas[iy * NUNITO_SDF_ATLAS_W + ix1];
            const float c = NunitoAtlas[iy1 * NUNITO_SDF_ATLAS_W + ix];
            const float d = NunitoAtlas[iy1 * NUNITO_SDF_ATLAS_W + ix1];
            const float v = (a + (b - a) * tx) + ((c + (d - c) * tx) - (a + (b - a) * tx)) * ty;
            const float coverage = SDL_clamp((v - 128.0f) * toPixels + 0.5f + grow, 0.0f, 1.0f);
            row[x * 4 + 0] = 255;
            row[x * 4 + 1] = 255;
            row[x * 4 + 2] = 255;
            row[x * 4 + 3] = (U8)(coverage * 255.0f + 0.5f);
        }
    }
    slot->Texture = SDL_CreateTextureFromSurface(renderer, surface);
    SDL_DestroySurface(surface);
    if (slot->Texture == NULL) {
        return NULL;
    }
    SDL_SetTextureBlendMode(slot->Texture, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(slot->Texture, SDL_SCALEMODE_LINEAR);
    slot->Size = size;
    slot->Bold = bold;
    slot->LastUse = s_frameCounter;
    return slot->Texture;
}

static float TextWidthAt(const char *text, float size) {
    float width = 0.0f;
    for (const U8 *p = (const U8 *)text; *p; p++) {
        const T_NUNITO_GLYPH *g = GlyphOf(*p);
        if (g != NULL) {
            width += g->Advance;
        }
    }
    return width * size / (float)NUNITO_SDF_EM;
}

static void DrawText(SDL_Renderer *renderer, const T_ITEM *item) {
    float size = item->Size;
    if (item->MaxWidth > 0.0f) {
        const float width = TextWidthAt(item->Text, size);
        if (width > item->MaxWidth) {
            size *= item->MaxWidth / width;
        }
    }
    const S32 px = SDL_max((S32)(size * s_scale + 0.5f), 4);
    SDL_Texture *texture = FontTexture(renderer, px, item->Bold);
    if (texture == NULL) {
        return;
    }
    const float k = (float)px / (float)NUNITO_SDF_EM;
    const float texW = (float)NUNITO_SDF_ATLAS_W * k;
    const float texH = (float)NUNITO_SDF_ATLAS_H * k;
    float textureW = 0.0f, textureH = 0.0f;
    SDL_GetTextureSize(texture, &textureW, &textureH);

    const float width = TextWidthAt(item->Text, (float)px / s_scale) * s_scale;
    float penX = s_offsetX + item->X0 * s_scale;
    if (item->Align == UIOVERLAY_ALIGN_CENTER) {
        penX -= width * 0.5f;
    } else if (item->Align == UIOVERLAY_ALIGN_RIGHT) {
        penX -= width;
    }
    penX = (float)floor(penX + 0.5f);
    const float baseline = (float)floor(s_offsetY + item->Y0 * s_scale + CAP_HEIGHT * (float)px * 0.5f + 0.5f);
    const float top = baseline - NUNITO_SDF_ASCENT * k;
    const SDL_FColor color = ToColor(item->Color, 1.0f);

    for (const U8 *p = (const U8 *)item->Text; *p; p++) {
        const T_NUNITO_GLYPH *g = GlyphOf(*p);
        if (g == NULL) {
            continue;
        }
        if (g->W > 0 && Reserve(renderer, texture, 4, 6)) {
            const float gx0 = (float)floor(penX + g->Left * k + 0.5f);
            const float gy0 = (float)floor(top + g->Top * k + 0.5f);
            const float gx1 = gx0 + (float)g->W * k;
            const float gy1 = gy0 + (float)g->H * k;
            const float u0 = (float)g->X * k / textureW, v0 = (float)g->Y * k / textureH;
            const float u1 = u0 + (float)g->W * k / textureW, v1 = v0 + (float)g->H * k / textureH;
            const S32 base = s_nbVerts;
            const float qx[4] = {gx0, gx1, gx1, gx0};
            const float qy[4] = {gy0, gy0, gy1, gy1};
            const float qu[4] = {u0, u1, u1, u0};
            const float qv[4] = {v0, v0, v1, v1};
            for (S32 i = 0; i < 4; i++) {
                SDL_Vertex *v = &s_verts[s_nbVerts++];
                v->position.x = qx[i];
                v->position.y = qy[i];
                v->color = color;
                v->tex_coord.x = qu[i];
                v->tex_coord.y = qv[i];
            }
            s_indices[s_nbIndices++] = base;
            s_indices[s_nbIndices++] = base + 1;
            s_indices[s_nbIndices++] = base + 2;
            s_indices[s_nbIndices++] = base;
            s_indices[s_nbIndices++] = base + 2;
            s_indices[s_nbIndices++] = base + 3;
        }
        penX += g->Advance * k;
    }
    (void)texW;
    (void)texH;
    Flush(renderer, texture);
}

static void DrawImage(SDL_Renderer *renderer, const T_ITEM *item) {
    if (s_fontRenderer != renderer) {
        UiOverlay_ReleaseTextures();
        s_fontRenderer = renderer;
    }
    if (s_imageTexture == NULL || s_imagePixels != item->Pixels || s_imageVersion != item->Version ||
        s_imageW != item->PixelsW || s_imageH != item->PixelsH) {
        if (s_imageTexture != NULL && (s_imageW != item->PixelsW || s_imageH != item->PixelsH)) {
            SDL_DestroyTexture(s_imageTexture);
            s_imageTexture = NULL;
        }
        if (s_imageTexture == NULL) {
            s_imageTexture = SDL_CreateTexture(renderer, SDL_PIXELFORMAT_ABGR8888, SDL_TEXTUREACCESS_STATIC,
                                               item->PixelsW, item->PixelsH);
            if (s_imageTexture == NULL) {
                return;
            }
            SDL_SetTextureBlendMode(s_imageTexture, SDL_BLENDMODE_BLEND);
            SDL_SetTextureScaleMode(s_imageTexture, SDL_SCALEMODE_LINEAR);
        }
        SDL_UpdateTexture(s_imageTexture, NULL, item->Pixels, item->PixelsW * 4);
        s_imagePixels = item->Pixels;
        s_imageVersion = item->Version;
        s_imageW = item->PixelsW;
        s_imageH = item->PixelsH;
    }
    SDL_FRect dst;
    dst.x = s_offsetX + item->X0 * s_scale;
    dst.y = s_offsetY + item->Y0 * s_scale;
    dst.w = (item->X1 - item->X0) * s_scale;
    dst.h = (item->Y1 - item->Y0) * s_scale;
    SDL_RenderTexture(renderer, s_imageTexture, NULL, &dst);
}

static T_ITEM *NewItem(S32 kind) {
    if (s_count >= MAX_ITEMS) {
        return NULL;
    }
    T_ITEM *item = &s_items[s_count++];
    memset(item, 0, sizeof(*item));
    item->Kind = kind;
    item->Layer = s_layer;
    return item;
}

// --- Interface ---------------------------------------------------------------
void UiOverlay_Clear(void) {
    s_count = 0;
    s_layer = UIOVERLAY_LAYER_MENU;
}

void UiOverlay_ClearLayer(S32 layer) {
    S32 kept = 0;
    for (S32 i = 0; i < s_count; i++) {
        if (s_items[i].Layer != layer) {
            if (kept != i) {
                s_items[kept] = s_items[i];
            }
            kept++;
        }
    }
    s_count = kept;
    s_layer = layer;
}

S32 UiOverlay_Active(void) {
    return s_count > 0;
}

void UiOverlay_Card(float x0, float y0, float x1, float y1, const T_UIOVERLAY_CARD *style) {
    T_ITEM *item = NewItem(ITEM_CARD);
    if (item == NULL) {
        return;
    }
    item->X0 = x0;
    item->Y0 = y0;
    item->X1 = x1;
    item->Y1 = y1;
    item->Card = *style;
}

void UiOverlay_Text(float x, float y, const char *cp850, float size, U32 color, S32 align, S32 bold, float maxWidth) {
    T_ITEM *item = NewItem(ITEM_TEXT);
    if (item == NULL) {
        return;
    }
    item->X0 = x;
    item->Y0 = y;
    SDL_strlcpy(item->Text, cp850, sizeof(item->Text));
    item->Size = size;
    item->Color = color;
    item->Align = align;
    item->Bold = bold ? 1 : 0;
    item->MaxWidth = maxWidth;
}

float UiOverlay_TextWidth(const char *cp850, float size) {
    return TextWidthAt(cp850, size);
}

void UiOverlay_Sparkle(float x, float y, float size, U32 color) {
    T_ITEM *item = NewItem(ITEM_SPARKLE);
    if (item == NULL) {
        return;
    }
    item->X0 = x;
    item->Y0 = y;
    item->Size = size;
    item->Color = color;
}

void UiOverlay_Image(float x0, float y0, float x1, float y1, const U8 *rgba, S32 w, S32 h, U32 version) {
    if (rgba == NULL || w <= 0 || h <= 0) {
        return;
    }
    T_ITEM *item = NewItem(ITEM_IMAGE);
    if (item == NULL) {
        return;
    }
    item->X0 = x0;
    item->Y0 = y0;
    item->X1 = x1;
    item->Y1 = y1;
    item->Pixels = rgba;
    item->PixelsW = w;
    item->PixelsH = h;
    item->Version = version;
}

void UiOverlay_Chevron(float x, float y, float size, S32 dir, U32 color) {
    T_ITEM *item = NewItem(ITEM_CHEVRON);
    if (item == NULL) {
        return;
    }
    item->X0 = x;
    item->Y0 = y;
    item->Size = size;
    item->Align = dir;
    item->Color = color;
}

void UiOverlay_Render(void *sdlRenderer) {
    if (s_count == 0 || sdlRenderer == NULL) {
        return;
    }
    SDL_Renderer *renderer = (SDL_Renderer *)sdlRenderer;
    s_frameCounter++;

    /* Drawn in window pixels, so text and edges are rasterized at the size they
       are seen; frame coordinates map through the letterboxed frame rect. */
    int logicalW = 0, logicalH = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_FRect rect;
    if (!SDL_GetRenderLogicalPresentation(renderer, &logicalW, &logicalH, &mode) ||
        !SDL_GetRenderLogicalPresentationRect(renderer, &rect) || logicalW <= 0 || logicalH <= 0) {
        return;
    }
    SDL_SetRenderLogicalPresentation(renderer, logicalW, logicalH, SDL_LOGICAL_PRESENTATION_DISABLED);
    s_scale = rect.w / (float)logicalW;
    s_offsetX = rect.x;
    s_offsetY = rect.y;

    SDL_SetRenderDrawBlendMode(renderer, SDL_BLENDMODE_BLEND);
    for (S32 pass = 0; pass < UIOVERLAY_LAYERS * s_count; pass++) {
        const T_ITEM *item = &s_items[pass % s_count];
        if (item->Layer != pass / s_count) {
            continue;
        }
        switch (item->Kind) {
        case ITEM_CARD:
            DrawCard(renderer, item);
            break;
        case ITEM_SPARKLE:
            DrawSparkle(renderer, item);
            break;
        case ITEM_CHEVRON:
            DrawChevron(renderer, item);
            break;
        case ITEM_IMAGE:
            Flush(renderer, NULL);
            DrawImage(renderer, item);
            break;
        case ITEM_TEXT:
            Flush(renderer, NULL);
            DrawText(renderer, item);
            break;
        }
    }
    Flush(renderer, NULL);
    DrawPointer(renderer);

    SDL_SetRenderLogicalPresentation(renderer, logicalW, logicalH, mode);
}

void UiOverlay_ReleaseTextures(void) {
    for (S32 i = 0; i < MAX_FONT_TEXTURES; i++) {
        if (s_fonts[i].Texture != NULL) {
            SDL_DestroyTexture(s_fonts[i].Texture);
            s_fonts[i].Texture = NULL;
        }
    }
    if (s_imageTexture != NULL) {
        SDL_DestroyTexture(s_imageTexture);
        s_imageTexture = NULL;
    }
    s_imagePixels = NULL;
    s_fontRenderer = NULL;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
