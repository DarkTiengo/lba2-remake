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
#define GPUOBJ_MAX_VERTS (1u << 20) ///< cached vertices across live draws (about 96 MB)
#define GPUOBJ_MAX_IDS (1u << 23)   ///< ids are exact in a float32 target, below GPUOBJ_SCENE_ID_BIT
#define GPUOBJ_SCENE_ID_BIT (1u << 23) ///< added to the id the GPU sees for a scene draw
#define GPUOBJ_SLICES 4096.0f       ///< depth slices ordering draws

/* Fragment modes, read by GPUOBJ.frag. */
#define GPUOBJ_MODE_PASS 0      ///< Not drawn by the GPU: pixel keeps software colour
#define GPUOBJ_MODE_SOLID 1     ///< palette[color]
#define GPUOBJ_MODE_SHADED 2    ///< palette ramp color + shade
#define GPUOBJ_MODE_TEX 3       ///< palette[texel]
#define GPUOBJ_MODE_TEXSHADED 4 ///< CLUT[shade][texel]
#define GPUOBJ_MODE_DISC 5      ///< Sphere: palette[color] inside the unit disc
#define GPUOBJ_MODE_CLUT 6      ///< CLUT[shade][color]: the terrain's Gouraud table fill
#define GPUOBJ_MODE_BRICK 7     ///< interior brick sprite from the atlas, depth from its grid cell
#define GPUOBJ_MODE_ORB 8       ///< lit glossy sphere inside the unit disc, colour in mat.yzw

#define GPUOBJ_FLAG_CHROMAKEY 1 ///< texel 0 is transparent
#define GPUOBJ_FLAG_ISO 2       ///< isometric projection (constant view vector)
#define GPUOBJ_FLAG_BAKED 4     ///< shade comes from normal.w as authored, not from lighting

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
    S32 Scene;          ///< part of the perspective scene: depth-tests every other scene draw
    S32 Painter;        ///< overwrites like the software painter (interior bricks) but still writes depth
    U8 *Page;           ///< texture page, NULL when untextured
    U8 *Clut;           ///< 16x256 Gouraud CLUT, NULL when unused
    S32 HasLogical;     ///< Logical holds the fog remap
    S32 FogStart;       ///< distance fog in view depth; FogEnd <= FogStart disables it
    S32 FogEnd;
    S32 FogColor; ///< palette index the fog fades to
    S32 HasFire;  ///< some of its polygons show an animated fire texture
    float FirePos[3]; ///< where they are, in the draw's scene space
    S32 HasGlow;      ///< a lamp's bright globe: 1 in an isometric room, 2 in a perspective scene
    float GlowPos[3]; ///< where they are, in the draw's scene space
    float GlowColor[3]; ///< their colour, 0..1
    U8 Logical[256];
} T_GPUOBJ_DRAW;

// --- State consumed by the presenter -----------------------------------------
extern S32 GpuObjEnabled;   ///< player choice (lba2.cfg GpuRenderer, gfx_gpu); the window follows it
extern S32 GpuObjAvailable; ///< set by the presenter once its pipeline is live

extern T_GPUOBJ_VERTEX *GpuObjVerts;

/* Dynamic point lights for the scene, in the scene's own space (camera-relative
   world units in isometric rooms, view space in perspective scenes, the spaces
   the capture stores vertex positions in). Set once per scene frame. */
#define GPUOBJ_MAX_LIGHTS 16

typedef struct {
    float Pos[4];   ///< xyz, radius
    float Color[4]; ///< rgb, intensity
} T_GPUOBJ_LIGHT;

extern T_GPUOBJ_LIGHT GpuObjLights[GPUOBJ_MAX_LIGHTS];
extern U32 GpuObjNbLights;
extern S32 GpuObjLightsEnabled; ///< player toggle (gfx_lights)

/* Player toggles for the GPU look (Display > GPU effects). */
extern S32 GpuObjPixelFilter; ///< xBR pixel-art upscaler on software pixels (gfx_pixelfilter)
extern S32 GpuObjDeband;      ///< in-between colours on the palette's ramps (gfx_deband)
extern S32 GpuObjSpecular;    ///< specular highlights on models (gfx_specular)
extern S32 GpuObjUiScale;     ///< 2D drawn at its classic size and upscaled (gfx_uiscale)
extern S32 GpuObjGlow;        ///< lamp globes on 3D models cast light (gfx_lamps)
extern S32 GpuObjFire;        ///< procedural flames and firelight on fire textures (gfx_fire)

/* The animated fire regions of the object texture page (ANIMTEX.CPP): polygons
   sampling them get procedural flames and cast flickering light. */
#define GPUOBJ_MAX_FIRES 4
typedef struct {
    S32 X, Y, W, H; ///< texels in the page
} T_GPUOBJ_RECT;

extern const U8 *GpuObjFirePage;
extern T_GPUOBJ_RECT GpuObjFireRects[GPUOBJ_MAX_FIRES];
extern U32 GpuObjNbFires;

void GpuObj_SetFireRects(const U8 *page, const T_GPUOBJ_RECT *rects, U32 count);

/** The fire rect holding texel (x, y) of the fire page, -1 when none. */
S32 GpuObj_FireAt(S32 x, S32 y);

void GpuObj_ClearLights(void);
void GpuObj_AddLight(float x, float y, float z, float radius, float r, float g, float b, float intensity);

/* Sprite atlas for interior bricks: GPUOBJ_ATLAS_SIZE square, two bytes a texel
   (palette index, coverage). Written by the capture, uploaded by the presenter
   when GpuObjAtlasDirty is set. */
#define GPUOBJ_ATLAS_SIZE 2048
extern U8 *GpuObjAtlas;
extern S32 GpuObjAtlasDirty;

/* Size the player chose (it can exceed both the software frame and the window).
   The GPU renders bodies at least this large and scales them down to the
   window, so a resolution above the display's supersamples the models. 0 when
   unknown. */
extern U32 GpuObjTargetHintW;
extern U32 GpuObjTargetHintH;

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

/** A group gathers every draw opened until EndGroup under one id and tags the
 *  frame once at the end: the exterior scenery (terrain, sea, sky, decors) is
 *  hundreds of fills that depth-test one another, far cheaper to diff once.
 *  Draws inside a group take no snapshot of their own. */
void GpuObj_BeginGroup(void);
void GpuObj_EndGroup(void);

/** Clears the atlas (the brick set changed with the room) and returns it, or
 *  NULL when it cannot be allocated. */
U8 *GpuObj_AtlasReset(void);

/** Draws opened while set belong to the perspective scene (exterior scenery,
 *  actors, extras). The GPU renders all scene draws against one real depth
 *  buffer, and any pixel a scene draw tagged shows whichever scene surface is
 *  nearest there, at the GPU's resolution; other draws (menu and inventory
 *  models) stay in front of the scene in draw order and keep exact id matching. */
void GpuObj_SetSceneDepth(S32 scene);

/** A masked copy from src into Log over the box [x0,x1] x [y0,y1] (DrawOverBrick
 *  re-blitting bricks over an actor): copied pixels take src's tags. */
void GpuObj_OnCopyMask(const void *src, S32 x0, S32 y0, S32 x1, S32 y1);

/** The pixels of the box were redrawn with the same geometry (animated water):
 *  keep their tags and take the new palette indices as what they show. */
void GpuObj_RefreshTags(S32 x0, S32 y0, S32 x1, S32 y1);

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
