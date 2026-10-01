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
#define GPUOBJ_MAX_VERTS (1u << 21) ///< cached vertices across live draws (192 MB at most): a camera cut keeps the old view and the new one live together
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
#define GPUOBJ_MODE_SHADOW 9    ///< soft ground shadow: ellipse coords in uv.zw, strength and softness in mat.yz
#define GPUOBJ_MODE_SILHOUETTE 10 ///< a body's shadow projected on the ground from the sun or a light
#define GPUOBJ_MODE_SPRITE 11

/* The depth range a perspective scene's vertices are placed in (AFF_GPU.CPP's
   SCENERY_DEPTH_RANGE / PERSPECTIVE_DEPTH_RANGE). The presenter needs it to
   rebuild the clip position of a draw kept from an earlier frame. */
#define GPUOBJ_SCENE_DEPTH_RANGE 131072.0f     ///< an extra's sprite: coverage from the atlas, colour from the software frame, its own depth

/* Indices, for geometry whose points are shared between its triangles: the
   smooth terrain's sub-grid has fifteen distinct points where its sixteen
   triangles have forty-eight corners. They are relative to their draw's first
   vertex, so compacting moves a draw's block without rewriting it. */
#define GPUOBJ_MAX_INDICES (1u << 22)
extern U32 *GpuObjIndices;

/* Height maps the vertex shader grows the smooth terrain from (GPUTERRAIN.vert):
   a cube's 65 x 65 heights, one slot a cube, kept by the cube's place so a
   cube seen frame after frame is sent once. */
#define GPUOBJ_HEIGHTMAP_SIDE 65
#define GPUOBJ_MAX_HEIGHTMAPS 64

/* What GPUTERRAIN.vert grows a Procedural draw's vertices into. */
#define GPUOBJ_PROC_TERRAIN 1 ///< three vertices, a land triangle's corners: sixteen triangles on the curve
#define GPUOBJ_PROC_GRASS 2   ///< one vertex, a tuft's root: three blades
#define GPUOBJ_PROC_LAND 3    ///< one header vertex; the triangles are the cube slot's land records
#define GPUOBJ_PROC_SEA 4     ///< four vertices, a sea tile's corners: an 8 x 8 mesh the swell raises
#define GPUOBJ_PROC_SEA_SUB 8
#define GPUOBJ_PROC_SEA_VERTS (GPUOBJ_PROC_SEA_SUB * GPUOBJ_PROC_SEA_SUB * 6) ///< drawn a record of four
#define GPUOBJ_PROC_TERRAIN_VERTS 48 ///< drawn a record of three
#define GPUOBJ_PROC_GRASS_VERTS 9    ///< drawn a record of one
extern float GpuObjHeightMaps[GPUOBJ_MAX_HEIGHTMAPS * GPUOBJ_HEIGHTMAP_SIDE * GPUOBJ_HEIGHTMAP_SIDE];
extern S32 GpuObjHeightMapsDirtyFrom; ///< first slot changed since the presenter's last copy, -1 when none
extern S32 GpuObjHeightMapsDirtyTo;   ///< one past the last

/** The slot holding the height map of the cube keyed (island, x, z), filled
 *  from heights when the key is new. The same place is always the same map:
 *  a cube's heights do not change while the island is played. *fresh (when not
 *  NULL) says whether the slot was just taken, and the rest of the cube's data
 *  must be written into it too. */
S32 GpuObj_HeightMap(S32 island, S32 cubeX, S32 cubeZ, const S16 *heights, S32 *fresh);

/* A cube's land as records the GPU grows every triangle of (GPUTERRAIN.vert),
   two uvec4 each (TERRAIN_GPU.CPP's LAND_* bits, the shading, the three
   texture corners), kept in the cube's slot. */
#define GPUOBJ_LAND_MAX 16384 ///< 64 x 64 cells, two halves, two layers (colour and texture)
#define GPUOBJ_LAND_WORDS 8   ///< U32 a record
extern U32 GpuObjLandRecords[GPUOBJ_MAX_HEIGHTMAPS * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS];
extern S32 GpuObjLandCount[GPUOBJ_MAX_HEIGHTMAPS]; ///< records in each slot, -1 before they are built


/** The slot's data (height map, land records) changed: send it again. */
void GpuObj_CubeSlotChanged(S32 slot);

/* Bodies kept whole on the GPU, in their own model space: the scenery's decor
   is a few dozen models placed hundreds of times, so each model is sent once
   (GpuObj_MeshAlloc) and every placement is a draw of the same vertices
   through its own rotation and origin (T_GPUOBJ_DRAW Mesh, MeshRows). */
#define GPUOBJ_MESH_MAX_VERTS (1u << 17)
extern struct T_GPUOBJ_VERTEX_S *GpuObjMeshVerts;
extern U32 GpuObjMeshCount; ///< vertices written in GpuObjMeshVerts
extern U32 GpuObjMeshEpoch; ///< bumped when the meshes are dropped: everything is sent again

/** Room for n more mesh vertices, NULL when the store is full. *first gets
 *  the index of the first. */
struct T_GPUOBJ_VERTEX_S *GpuObj_MeshAlloc(U32 n, U32 *first);

/** Drops every mesh (a new island's models). */
void GpuObj_MeshReset(void);

#define GPUOBJ_FLAG_CHROMAKEY 1 ///< texel 0 is transparent
#define GPUOBJ_FLAG_ISO 2       ///< isometric projection (constant view vector)
#define GPUOBJ_FLAG_BAKED 4     ///< shade comes from normal.w as authored, not from lighting
#define GPUOBJ_FLAG_EMISSIVE 8  ///< glows: a lamp's globe (bloom halo, brighter core)
#define GPUOBJ_FLAG_WATER 16    ///< eligible sea plane; shader toggle keeps the original texture available
#define GPUOBJ_FLAG_SKY 32      ///< sky plane marker used to keep shoreline foam off the horizon
#define GPUOBJ_FLAG_WATER_TERRAIN 64  ///< CodeJeu water retains its authored animated terrain page
#define GPUOBJ_FLAG_DETAIL 128 ///< smooth terrain's small triangles: shadow rays take the original instead, a block at a time
#define GPUOBJ_FLAG_GRASS 512        ///< a grass blade: the tip sways by uv.zw (clip offset of the wind), phase in mat.w
#define GPUOBJ_FLAG_WAVES 1024       ///< a point of the sea's mesh, undisplaced: the vertex shader raises the swell at world uv.zw * 512
#define GPUOBJ_FLAG_TERRAIN 2048     ///< exterior ground, distinct from actors/decor at the waterline
#define GPUOBJ_FLAG_LAVA 4096        ///< authored lava surface, with palette-preserving procedural detail
#define GPUOBJ_FLAG_FOLIAGE 8192     ///< a tree's leaf (GPUTREE.H): shaded as foliage and fluttering while its draw is a tree

/* One vertex; every attribute is a vec4 so the layout is trivial on all backends. */
typedef struct T_GPUOBJ_VERTEX_S {
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
    float FireRect[4]; ///< their screen box in frame pixels (x0, y0, x1, y1), for the flame sprite
    S32 FireRises;     ///< they stand up in the world (a flame), not lie flat (a laser bar)
    S32 HasGlow;      ///< a lamp's bright globe: 1 in an isometric room, 2 in a perspective scene
    float GlowPos[3]; ///< where they are, in the draw's scene space
    float GlowColor[3]; ///< their colour, 0..1
    S32 GlowListed;     ///< its light is in GpuObjSceneryLamps: the glass glows, the light is not taken from it
    U8 Logical[256];
    /* The camera the draw's vertices were placed with: the rotation's rows and
       where the eye stood, both as the capture had them (a vertex is
       M * world - eye). A draw still alive in a later frame is drawn through
       the difference between this and that frame's camera, which is what lets
       the scenery outlive the camera that captured it. */
    float CapMat[9];
    float CapCam[3];
    S32 HasCapCamera; ///< the two above are set (a 3D capture, not an isometric one)
    U32 FirstIndex;   ///< first index of the draw, when it has any
    U32 NbIndices;    ///< 0: drawn straight from its vertices, in threes
    S32 Retainable;   ///< the exterior scenery: placed by AffGpu_ViewVertex, so it can be re-placed
    S32 Retained;     ///< kept from an earlier frame: drawn through the camera difference
    S32 Procedural;   ///< grown by GPUTERRAIN.vert: GPUOBJ_PROC_*, 0 for plain vertices
    /* GPUOBJ_PROC_LAND: a cube's land, from its cached records (GpuObj_CubeSlot). */
    S32 LandSlot;       ///< the cube's slot
    S32 LandSmooth;     ///< grown on the curve (48 vertices a triangle) or flat (3)
    float LandOrigin[3]; ///< the cube's local origin in the draw's view space (vpos = rotation * local + origin)
    float LandPhase[2];  ///< the cube's place in the sea's phase, in 512-unit steps (cube * 64)
    float LandFar;       ///< view depth past which a cell is not drawn (ClipZFar)
    /* A placement of a mesh: First and Count are in GpuObjMeshVerts, whose
       vertices are in model space. */
    S32 Mesh;
    float MeshRows[12]; ///< model to the draw's view space: rotation rows, translation in the fourth
    float MeshLight[4]; ///< the view-space light, and the scale the normals' shade takes (w)
    /* A tree in the wind (GPUTREE.H): the draw uniform's four vec4, in the
       space the draw's vertices are written in. */
    S32 Tree;
    float TreeUniform[16];
} T_GPUOBJ_DRAW;

// --- State consumed by the presenter -----------------------------------------
/* The camera the scene is being captured with this frame: the rotation's rows
   and the eye, in the form a vertex is placed with (M * world - eye). Recorded
   where the scene light is, and copied into every draw opened after it. */
extern float GpuObjCapMat[9];
extern float GpuObjCapCam[3];
extern S32 GpuObjCapValid;

extern S32 GpuObjEnabled;   ///< player choice (lba2.cfg GpuRenderer, gfx_gpu); the window follows it
extern S32 GpuObjAvailable; ///< set by the presenter once its pipeline is live

extern T_GPUOBJ_VERTEX *GpuObjVerts;
extern U32 GpuObjVertsExhausted; ///< draws left to the software frame for want of room

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
extern S32 GpuObjShadows;     ///< soft shadows on the real ground (gfx_shadows)
extern S32 GpuObjSunlight;    ///< global light: sun shadows, sky fill, rim light (gfx_sunlight)

/* The scene's global light, in the scene space lights use: the direction toward
   the sun (the light that shades bodies), the world's up, and whether the scene
   is an exterior (a real sky) or a room. Set while the frame is captured. */
extern float GpuObjSunDir[3];
extern float GpuObjUpDir[3];
extern float GpuObjWorldX[3]; ///< the world's X axis in scene space (3D)
extern float GpuObjWorldZ[3]; ///< and its Z axis
extern float GpuObjCameraPos[3]; ///< where the camera stands in the world, taken with those axes: a
                                 ///< scene point's world coordinate is this plus its projection on them
extern S32 GpuObjSunExterior;
extern float GpuObjFocal; ///< frame pixels per scene unit at depth 1 (3D), per unit (iso)
extern S32 GpuObjRayTrace;  ///< shadows ray traced over every body and the terrain outdoors (gfx_raytrace)
extern S32 GpuObjRayScene;  ///< this frame's shadows are ray traced (an exterior)
extern float GpuObjProj[4]; ///< the 3D projection: XCentre, YCentre, FRatioX, FRatioY

/* Soft shadows of the current scene frame, in scene space; set with the lights. */
#define GPUOBJ_MAX_SHADOWS 96
/* A shadow is a grid of cells: its depth is linear in view depth, which the GPU
   interpolates linearly on screen, so large triangles would sink under the ground. */
#define GPUOBJ_SHADOW_CELLS 8
#define GPUOBJ_SHADOW_VERTS (GPUOBJ_SHADOW_CELLS * GPUOBJ_SHADOW_CELLS * 6)
/* Contact shadows and the bodies' projected silhouettes share one list. */
#define GPUOBJ_SHADOW_CAPACITY 98304
extern T_GPUOBJ_VERTEX GpuObjShadowVerts[GPUOBJ_SHADOW_CAPACITY];
extern U32 GpuObjNbShadowVerts;
void GpuObj_ClearShadows(void);
/** GPUOBJ_SHADOW_VERTS vertices for one shadow, NULL when the list is full. */
T_GPUOBJ_VERTEX *GpuObj_AllocShadow(void);
/** n more shadow vertices, NULL when they do not fit. */
T_GPUOBJ_VERTEX *GpuObj_AllocShadowVerts(U32 n);

extern S32 GpuObjGlow;        ///< lamp globes on 3D models cast light (gfx_lamps)
extern S32 GpuObjLampModel;   ///< set by the caller while it draws a lamp model
extern const U8 *GpuObjLampPalette; ///< the scene's 768-byte RGB palette, for the lamp glass
extern S32 GpuObjLampListed;        ///< the lamp being drawn already lights the scenery from GpuObjSceneryLamps

/* The scenery's lamps as lights, whether their models are on screen or not:
   the glass's centre and colour taken from the whole model, placed in the
   view space of the scenery group they were listed in. A lamp's light reaches
   well past its post, so one just off screen, or behind the camera, still
   lights what is seen; and the glass seen from one side or the other does not
   move the light. */
#define GPUOBJ_MAX_SCENERY_LAMPS 64
typedef struct {
    float Pos[3];
    float Color[3]; ///< the glass's colour, 0..1
} T_GPUOBJ_LAMP;
extern T_GPUOBJ_LAMP GpuObjSceneryLamps[GPUOBJ_MAX_SCENERY_LAMPS];
extern U32 GpuObjNbSceneryLamps;
extern U32 GpuObjSceneryLampGroup; ///< the group they belong to: they are the scenery's while it lives

/** A lamp of the scenery in the open group, the nearest to the camera kept
 *  when there are more than the list holds. FALSE outside a group. */
S32 GpuObj_AddSceneryLamp(const float pos[3], const float color[3]);
extern S32 GpuObjFire;        ///< procedural flames and firelight on fire textures (gfx_fire)
extern S32 GpuObjLava;        ///< authored lava shading (gfx_lava)

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

/* What the scene is about -- the one the camera follows -- in scene space: the
   lights are chosen by how near they are to it, a choice the camera turning
   around it leaves alone. */
extern float GpuObjFocus[3];
extern S32 GpuObjFocusValid;
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

/** Reserves n indices in the open draw; NULL when the index cache is full, and
 *  the caller then writes its triangles out in threes as usual. They are
 *  relative to the draw's own first vertex. A draw either has indices for all
 *  of its triangles or for none. */
U32 *GpuObj_AllocIndices(U32 n);

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

/** The id of the group being captured, 0 when none is open. */
U32 GpuObj_GroupId(void);

/** The id of the last group opened, open or not. */
U32 GpuObj_LastGroupId(void);

/** A group that owns every pixel of the box [x0, x1] x [y0, y1] when it ends,
 *  whether its fills changed the pixel or not: the exterior scenery, when the
 *  software leaves the terrain the GPU draws unpainted. What the frame shows
 *  there is then the GPU's nearest scene surface, until a later draw changes
 *  the pixel. No snapshot is taken nor compared at its end: the box is the
 *  group's whatever it holds. Owns nothing when it ends without a draw. */
void GpuObj_BeginClaimedGroup(S32 x0, S32 y0, S32 x1, S32 y1);

/** The last group to end claimed its box: the software frame under the scenery
 *  is the clear colour, not a picture of the ground. */
extern S32 GpuObjSceneryClaimed;

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

/** The box [x0, x1] x [y0, y1] of the frame being drawn shows its software
 *  pixels from now on, whatever GPU draw owned them: a modal panel drawn over
 *  the scene (its fills and shading can leave pixels unchanged, which would
 *  keep the scene's GPU colour over them). Later draws tag it again. */
void GpuObj_UntagBox(S32 x0, S32 y0, S32 x1, S32 y1);

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

/** Vertices written in GpuObjVerts so far. Draws are only appended, so a
 *  presenter mirroring the array uploads just what lies past its last copy. */
U32 GpuObj_VertexCount(void);

/** Changes whenever vertices already written move (repacked or reset): the
 *  mirror must then be copied whole again. */
U32 GpuObj_VertexEpoch(void);

/** Indices written in GpuObjIndices so far. */
U32 GpuObj_IndexCount(void);

#ifdef __cplusplus
}
#endif
