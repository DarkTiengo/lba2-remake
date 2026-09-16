#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * GPU object layer: 3D bodies re-rendered by the GPU with per-pixel lighting
 * and true colour, composited over the software frame.
 *
 * The software rasterizer still draws every body into Log, so visibility
 * (bricks drawn over actors, menus, text) is decided exactly as in 1997. While
 * a body is rasterized, every pixel it changes is tagged with the body's draw
 * id and the palette index it wrote. Tags travel with the pixels when frame
 * buffers are copied into each other, so a body baked into the background, or
 * a scene saved behind a menu, keeps its id.
 *
 * Each draw keeps its geometry for as long as some pixel still carries its id,
 * and every present re-renders the live draws with the current palette and
 * textures, so fades and palette flashes reach GPU pixels too. A pixel shows
 * the GPU colour only if Log still holds the index its body wrote and the GPU
 * wrote the same id there; anything else falls back to the software colour.
 *
 * This header is platform-free: the capture side (OBJECT) and the copy hooks
 * (SVGA) fill it, the SDL GPU presenter (SYSTEM/GPURENDER) consumes it.
 */

#ifdef __cplusplus
extern "C" {
#endif

// -----------------------------------------------------------------------------
#define GPUOBJ_MAX_VERTS (1u << 18) ///< cached vertices across live draws (about 24 MB)
#define GPUOBJ_MAX_IDS (1u << 24)   ///< ids are exact in a float32 target
#define GPUOBJ_SLICES 4096.0f       ///< depth slices ordering draws

/* Fragment modes, read by GPUOBJ.frag. */
#define GPUOBJ_MODE_PASS 0      ///< Not drawn by the GPU: pixel keeps software colour
#define GPUOBJ_MODE_SOLID 1     ///< palette[color]
#define GPUOBJ_MODE_SHADED 2    ///< palette ramp color + shade
#define GPUOBJ_MODE_TEX 3       ///< palette[texel]
#define GPUOBJ_MODE_TEXSHADED 4 ///< CLUT[shade][texel]
#define GPUOBJ_MODE_DISC 5      ///< Sphere: palette[color] inside the unit disc

#define GPUOBJ_FLAG_CHROMAKEY 1 ///< texel 0 is transparent
#define GPUOBJ_FLAG_ISO 2       ///< isometric projection (constant view vector)

/* One vertex; every attribute is a vec4 so the layout is trivial on all backends. */
typedef struct {
    float clip[4];   ///< ndc.x * w, ndc.y * w, depth inside the draw * w, w
    float normal[4]; ///< view-space normal, w = shade scale (0..15 range at full light)
    float light[4];  ///< view-space light vector, w = mode
    float vpos[4];   ///< view-space position, w = flags
    float mat[4];    ///< color index, texture offset in page, repeat mask, unused
    float uv[4];     ///< texel u, v, sphere local x, y
} T_GPUOBJ_VERTEX;

typedef struct {
    U32 Id;
    U32 First;
    U32 Count;
    S32 X0, Y0, X1, Y1; ///< inclusive scissor (the software clip window)
    S32 ZBuffer;        ///< drawn in z-buffer mode: shares depth with its neighbours
    U8 *Page;           ///< texture page, NULL when untextured
    U8 *Clut;           ///< 16x256 Gouraud CLUT, NULL when unused
    S32 HasLogical;     ///< Logical holds the fog remap
    U8 Logical[256];
} T_GPUOBJ_DRAW;

// --- State consumed by the presenter -----------------------------------------
extern S32 GpuObjEnabled;   ///< player toggle
extern S32 GpuObjAvailable; ///< set by the presenter once its pipeline is live

extern T_GPUOBJ_VERTEX *GpuObjVerts;

// --- Capture -----------------------------------------------------------------
/** TRUE when bodies should be captured. */
S32 GpuObj_Active(void);

/** Opens a draw for the body about to be rasterized and returns it, or NULL
 *  when none is available (the body then stays software only). */
T_GPUOBJ_DRAW *GpuObj_BeginDraw(void);

/** Reserves n vertices in the open draw; NULL when the cache is full. */
T_GPUOBJ_VERTEX *GpuObj_AllocVerts(U32 n);

/** Snapshot Log inside the box before a body is rasterized, then close the
 *  draw and tag every pixel it changed. The box is clipped to the clip window. */
void GpuObj_SnapshotBegin(S32 x0, S32 y0, S32 x1, S32 y1);
void GpuObj_EndDraw(void);

// --- Pixel copies between frame buffers --------------------------------------
/** Box [x0,x1) x [y0,y1) copied from src to dst at the same place. */
void GpuObj_OnCopyArea(const void *dst, const void *src, S32 x0, S32 y0, S32 x1, S32 y1);
/** w x h pixels from (sx,sy) in src to (dx,dy) in dst. */
void GpuObj_OnCopyBlock(const void *dst, const void *src, S32 sx, S32 sy, S32 dx, S32 dy, S32 w, S32 h);

// --- Present -----------------------------------------------------------------
/** Fills out[] (ModeDesiredX * ModeDesiredY) inside box[] (x0, y0, x1, y1,
 *  half-open) with the id of every pixel still showing what its body wrote, 0
 *  elsewhere in the box, and returns the live draws in draw order through
 *  *draws. Pixels outside the box carry no tag. Returns the number of live
 *  draws; 0 means no pixel needs the GPU and out[] and box[] are untouched. */
U32 GpuObj_BuildPresent(float *out, const T_GPUOBJ_DRAW *const **draws, S32 *box);

#ifdef __cplusplus
}
#endif
