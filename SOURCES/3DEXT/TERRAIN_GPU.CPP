#include "TERRAIN_GPU.H"

#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <OBJECT/AFF_GPU.H>
#include <POLYGON/POLY.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>
#include <SVGA/GPUWATER.H>

#include <stdlib.h>
#include <string.h>
#include <math.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

#define GRID_SIDE 65
#define CELL_SIZE 512
/* The CodeJeu of shoreline water (LBA_EXT.H), whose fill keeps its authored page. */
#define LAND_CJ_FOOT_WATER 12
#define LAND_CJ_ANIMATED_WATER 15
#define SEA_SUBDIVISIONS 8

// --- Private state -----------------------------------------------------------
static T_GPUOBJ_DRAW *s_draw = NULL;

/* Texel addressing of the plane being drawn (sea, sky). */
static float s_planeTexOffset = 0.0f;
static float s_planeTexMask = 65535.0f;
static S32 s_waterPlane = FALSE;
static S32 s_skyPlane = FALSE;
static S32 s_skyBehind = FALSE; ///< the sky plane is drawn behind everything, as the painter does
static float s_waterCameraX = 0.0f;
static float s_waterCameraZ = 0.0f;

/* Which cube the capture is in, which is also the key of everything cached per
   cube below. Set before each cube is loaded (TerrainGpu_SetWaterCube) and for
   the island as a whole (TerrainGpu_SetWaterScene). */
static S32 s_island = 0;
static S32 s_cubeX = 0;
static S32 s_cubeZ = 0;

/* Camera-space position of every vertex of the cube being drawn. */
static float s_grid[GRID_SIDE * GRID_SIDE][3];

/* Smooth terrain (gfx_smoothterrain): the cube's heights, to curve its cells. */
S32 TerrainGpuSmooth = TRUE;
S32 TerrainGpuViewDistance = 1;

static S32 ViewDistanceLevel(void) {
    if (!GpuObjEnabled || !GpuObjAvailable) {
        return 0;
    }
    return TerrainGpuViewDistance < 0 ? 0 : (TerrainGpuViewDistance > 2 ? 2 : TerrainGpuViewDistance);
}

S32 TerrainGpu_HorizonRadius(void) {
    static const S32 kRadius[3] = {1, 2, 3};
    return kRadius[ViewDistanceLevel()];
}

float TerrainGpu_FogScale(void) {
    static const float kScale[3] = {1.0f, 1.6f, 2.4f};
    return kScale[ViewDistanceLevel()];
}
static S16 s_heights[GRID_SIDE * GRID_SIDE];
static S32 s_mapSlot = 0; ///< the cube's height map for GPUTERRAIN.vert

/* Grass (gfx_grass): blades standing on the ground the island marks as grass. */
S32 TerrainGpuGrass = TRUE;
S32 TerrainGpuFastFill = TRUE;
const U8 *TerrainGpuPalette = NULL;

/* The footstep sound of grass (T_HALF_POLY.SampleStep): the island's own mark
   of where grass grows. */
#define GRASS_STEP 2
#define GRASS_TUFTS 12   ///< per triangle (half a cell) near the camera: a lawn, not a thicket
#define GRASS_NEAR 20000.0f
#define GRASS_FAR 28000.0f

typedef struct {
    S16 Xi, Zi;
    U8 Corners[3];
    U8 HasUv;
    U8 Smoothed; ///< lies on the smooth terrain's curve
    S32 Lights[3];
    S32 PerVertexShade;
    S32 Color;
    float Mode;
    float ConstantShade;
    U16 Uv[6];
} T_GRASS_TRI;
#define MAX_GRASS_TRIS 4096
static T_GRASS_TRI s_grassTris[MAX_GRASS_TRIS];
static S32 s_nbGrassTris = 0;

/* Land triangles waiting for the cube's end, when every flat-drawn triangle is
   known: along their edges the curve must vanish, or the two would not meet. */
typedef struct {
    S16 Xi, Zi;
    U8 Corners[3];
    U8 HasUv;
    S32 Lights[3];
    S32 Flags;
    S32 PerVertexShade;
    S32 Color;
    float Mode;
    float ConstantShade;
    U16 Uv[6];
} T_SMOOTH_TRI;
#define MAX_SMOOTH_TRIS (GRID_SIDE * GRID_SIDE * 2)
static T_SMOOTH_TRI s_smoothTris[MAX_SMOOTH_TRIS];
static S32 s_nbSmoothTris = 0;
static U8 s_flatCorner[GRID_SIDE * GRID_SIDE]; ///< a corner of a triangle drawn flat

static float GridHeight(S32 x, S32 z) {
    x = x < 0 ? 0 : (x >= GRID_SIDE ? GRID_SIDE - 1 : x);
    z = z < 0 ? 0 : (z >= GRID_SIDE ? GRID_SIDE - 1 : z);
    return (float)s_heights[z * GRID_SIDE + x];
}

static float CatmullRom(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}

/* The curve through the heights around grid position (x, z): Catmull-Rom
   passes through every original height. A smoothed point is its triangle's
   plane moved toward this curve, the difference capped so the ground the game
   collides with stays close; it depends only on where the point lies, so
   neighbouring triangles meet exactly. */
static float CurveHeight(float x, float z) {
    const S32 ix = (S32)x;
    const S32 iz = (S32)z;
    const float u = x - (float)ix;
    const float t = z - (float)iz;
    float row[4];
    for (S32 k = 0; k < 4; k++) {
        const S32 gz = iz - 1 + k;
        row[k] = CatmullRom(GridHeight(ix - 1, gz), GridHeight(ix, gz), GridHeight(ix + 1, gz), GridHeight(ix + 2, gz), u);
    }
    return CatmullRom(row[0], row[1], row[2], row[3], t);
}

/* The curve is a property of the cube's height map, not of the view: the camera
   turns and every point of it is unchanged. It used to be worked out again for
   every smoothed vertex of every frame -- four Catmull-Rom runs over a 4x4
   neighbourhood, forty-eight vertices a triangle, every cube in the ring -- which
   is most of what the capture costs while the camera moves.

   Cached per cube at the resolution the subdivision asks for, and filled point by
   point as cells are drawn, so a cube seen edge-on never pays for the rest of
   itself. The key is the cube's place in the island: a cube's heights are read
   from the island's data when it loads and do not change while it is played, so
   the same place is always the same curve. Points that are not on the
   subdivision's grid (a grass tuft stands anywhere in its cell) are worked out
   as before. */
#define CURVE_SUBDIV 4 ///< SmoothTri's n: sub-points per cell side
#define CURVE_SIDE ((GRID_SIDE - 1) * CURVE_SUBDIV + 1)
#define CURVE_CACHE_CUBES 9

typedef struct {
    S32 Island, CubeX, CubeZ;
    S32 Used;      ///< nothing cached yet
    float *Height; ///< CURVE_SIDE * CURVE_SIDE, valid where Filled says so
    U8 *Filled;
} T_CURVE_CACHE;

static T_CURVE_CACHE s_curveCache[CURVE_CACHE_CUBES];
static T_CURVE_CACHE *s_curve = NULL; ///< the entry for the cube being captured

/* The entry for this cube, taking a spare one or the next in turn. */
static void CurveCacheSelect(void) {
    s_curve = NULL;
    T_CURVE_CACHE *spare = NULL;
    for (S32 i = 0; i < CURVE_CACHE_CUBES; i++) {
        T_CURVE_CACHE *c = &s_curveCache[i];
        if (c->Used AND c->Island == s_island AND c->CubeX == s_cubeX AND c->CubeZ == s_cubeZ) {
            s_curve = c;
            return;
        }
        if (spare == NULL AND !c->Used) {
            spare = c;
        }
    }
    if (spare == NULL) {
        /* Every entry is another cube's. The ring is drawn in a fixed order, so
           taking them in turn keeps the cube being walked away from. */
        static S32 next = 0;
        spare = &s_curveCache[next];
        next = (next + 1) % CURVE_CACHE_CUBES;
    }
    if (spare->Height == NULL) {
        spare->Height = (float *)malloc((size_t)CURVE_SIDE * CURVE_SIDE * sizeof(float));
        spare->Filled = (U8 *)malloc((size_t)CURVE_SIDE * CURVE_SIDE);
        if (spare->Height == NULL OR spare->Filled == NULL) {
            /* No cache: the curve is still computed, just never kept. */
            return;
        }
    }
    spare->Island = s_island;
    spare->CubeX = s_cubeX;
    spare->CubeZ = s_cubeZ;
    spare->Used = TRUE;
    memset(spare->Filled, 0, (size_t)CURVE_SIDE * CURVE_SIDE);
    s_curve = spare;
}

/* CurveHeight through the cube's cache where the point is on its grid. */
static float CachedCurveHeight(float x, float z) {
    if (s_curve == NULL OR s_curve->Height == NULL) {
        return CurveHeight(x, z);
    }
    const float gx = x * (float)CURVE_SUBDIV;
    const float gz = z * (float)CURVE_SUBDIV;
    const S32 ix = (S32)(gx + 0.5f);
    const S32 iz = (S32)(gz + 0.5f);
    if (ix < 0 OR ix >= CURVE_SIDE OR iz < 0 OR iz >= CURVE_SIDE OR gx - (float)ix > 0.01f OR
        (float)ix - gx > 0.01f OR gz - (float)iz > 0.01f OR (float)iz - gz > 0.01f) {
        return CurveHeight(x, z); /* not on the grid: a grass tuft, say */
    }
    const S32 at = iz * CURVE_SIDE + ix;
    if (!s_curve->Filled[at]) {
        s_curve->Height[at] = CurveHeight(x, z);
        s_curve->Filled[at] = 1;
    }
    return s_curve->Height[at];
}

#define SMOOTH_MAX_OFFSET 220.0f
#define SMOOTH_MAX_SINK 0.0f
#define SMOOTH_FADE_NEAR 18000.0f
#define SMOOTH_FADE_FAR 26000.0f

/* A cube-local world point in camera space, as the grid's vertices. */
static void GridPoint(float x, float y, float z, float out[3]) {
    const TYPE_MAT *m = &MatriceWorld;
    out[0] = x * m->F.M11 + y * m->F.M12 + z * m->F.M13 - (float)CameraXr;
    out[1] = x * m->F.M21 + y * m->F.M22 + z * m->F.M23 - (float)CameraYr;
    out[2] = (float)CameraZr - (x * m->F.M31 + y * m->F.M32 + z * m->F.M33);
}

// --- Private functions -------------------------------------------------------
static void OpenDraw(U8 *page, U8 *clut, S32 fogStart, S32 fogEnd, S32 fogColor) {
    s_draw = GpuObj_BeginDraw();
    if (s_draw == NULL) {
        return;
    }
    s_draw->X0 = ClipXMin;
    s_draw->Y0 = ClipYMin;
    s_draw->X1 = ClipXMax;
    s_draw->Y1 = ClipYMax;
    s_draw->ZBuffer = TRUE;
    /* The scenery's vertices are placed by AffGpu_ViewVertex from the camera
       alone, so the draw can be placed again from another one. */
    s_draw->Retainable = TRUE;
    s_draw->Page = page;
    s_draw->Clut = clut;
    s_draw->FogStart = fogStart;
    s_draw->FogEnd = fogEnd;
    s_draw->FogColor = fogColor;
}

/* Another draw with the same material as the one given, for geometry that
   cannot share a draw with what came before it: a draw is either indexed or
   not, and the curved land is indexed where the flat land and the grass are
   not. Returns FALSE when no draw is available, and the caller then emits
   nothing. */
static S32 ReopenDraw(const T_GPUOBJ_DRAW *like) {
    GpuObj_EndDraw();
    s_draw = GpuObj_BeginDraw();
    if (s_draw == NULL) {
        return FALSE;
    }
    s_draw->X0 = like->X0;
    s_draw->Y0 = like->Y0;
    s_draw->X1 = like->X1;
    s_draw->Y1 = like->Y1;
    s_draw->ZBuffer = like->ZBuffer;
    s_draw->Retainable = like->Retainable;
    s_draw->Page = like->Page;
    s_draw->Clut = like->Clut;
    s_draw->HasLogical = like->HasLogical;
    if (like->HasLogical) {
        memcpy(s_draw->Logical, like->Logical, sizeof(s_draw->Logical));
    }
    s_draw->FogStart = like->FogStart;
    s_draw->FogEnd = like->FogEnd;
    s_draw->FogColor = like->FogColor;
    return TRUE;
}

static void PlanePoint(const STRUC_CLIPVERTEX src[4], float u, float t, float out[5]) {
    const float a = (1.0f - u) * (1.0f - t);
    const float b = (1.0f - u) * t;
    const float c = u * t;
    const float d = u * (1.0f - t);
    out[0] = a * src[0].V_X0 + b * src[1].V_X0 + c * src[2].V_X0 + d * src[3].V_X0;
    out[1] = a * src[0].V_Y0 + b * src[1].V_Y0 + c * src[2].V_Y0 + d * src[3].V_Y0;
    out[2] = a * src[0].V_Z0 + b * src[1].V_Z0 + c * src[2].V_Z0 + d * src[3].V_Z0;
    out[3] = a * src[0].V_MapU + b * src[1].V_MapU + c * src[2].V_MapU + d * src[3].V_MapU;
    out[4] = a * src[0].V_MapV + b * src[1].V_MapV + c * src[2].V_MapV + d * src[3].V_MapV;
}

/* A point of the sea's mesh, where the flat sea lies. The swell is the vertex
   shader's (GPUOBJ.vert): it raises the point before projecting it, so the
   depth buffer sees the crest and homogeneous clipping still handles cells
   straddling the near plane -- and the waves move every frame, not only on the
   frames the scenery is captured again. The shader writes the height into
   normal.w for the contact pass, which compares the fixed sea datum to the
   fixed shoreline. */
static void WaterMeshVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth, float texU, float texV) {
    const TYPE_MAT *m = &MatriceWorld;
    const float worldX = x * m->F.M11 + y * m->F.M21 - depth * m->F.M31 + s_waterCameraX;
    const float worldZ = x * m->F.M13 + y * m->F.M23 - depth * m->F.M33 + s_waterCameraZ;
    AffGpu_ViewVertex(v, x, y, depth);
    v->normal[0] = m->F.M12;
    v->normal[1] = m->F.M22;
    v->normal[2] = m->F.M32;
    v->normal[3] = 0.0f;
    v->vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_WATER | GPUOBJ_FLAG_WAVES);
    v->light[0] = (float)CameraXLight;
    v->light[1] = (float)CameraYLight;
    v->light[2] = (float)CameraZLight;
    v->light[3] = (float)GPUOBJ_MODE_TEX;
    v->mat[1] = s_planeTexOffset;
    v->mat[2] = s_planeTexMask;
    v->uv[0] = texU;
    v->uv[1] = texV;
    v->uv[2] = worldX / 512.0f;
    v->uv[3] = worldZ / 512.0f;
}

static void WaterVertex(T_GPUOBJ_VERTEX *v, const STRUC_CLIPVERTEX src[4], float u, float t) {
    float point[5];
    PlanePoint(src, u, t, point);
    WaterMeshVertex(v, point[0], point[1], point[2], point[3] / 256.0f, point[4] / 256.0f);
}

/* How far smooth terrain lifts the point of grid position (gx, gz) whose
   original triangle lies at height flat; cornerWeight is the share of its
   corners that may curve (none at a flat-drawn triangle's). */
/* The curve's rise at a grid point, given how far away the point already is.
   The caller has placed the flat point in camera space to draw it, and that
   placement is what the distance fade needs, so it is passed in rather than
   computed again: at four subdivisions a land triangle is 48 vertices, and the
   second placement was half the arithmetic of each one. */
static float SmoothOffsetAt(float gx, float gz, float flat, float cornerWeight, float depth) {
    /* How much of the curve this point takes: none far away or at the sea. */
    float w = (SMOOTH_FADE_FAR - depth) / (SMOOTH_FADE_FAR - SMOOTH_FADE_NEAR);
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    float shore = (flat - 64.0f) / 320.0f;
    shore = shore < 0.0f ? 0.0f : (shore > 1.0f ? 1.0f : shore);
    /* None on the cube's border: the next cube curves its own side. */
    float border = gx < gz ? gx : gz;
    border = (float)(GRID_SIDE - 1) - gx < border ? (float)(GRID_SIDE - 1) - gx : border;
    border = (float)(GRID_SIDE - 1) - gz < border ? (float)(GRID_SIDE - 1) - gz : border;
    border = border > 1.0f ? 1.0f : border;
    float offset = CachedCurveHeight(gx, gz) - flat;
    /* Rounding a hill or a crest raises it; only hollows sink, and
       barely: a sunk fold would open a gap onto what lies behind. */
    offset = offset < -SMOOTH_MAX_SINK ? -SMOOTH_MAX_SINK : (offset > SMOOTH_MAX_OFFSET ? SMOOTH_MAX_OFFSET : offset);
    return offset * w * shore * border * cornerWeight;
}

/* The same, for callers that have no placed point to hand (the grass). */
static float SmoothOffset(float gx, float gz, float flat, float cornerWeight) {
    float p[3];
    GridPoint(gx * (float)CELL_SIZE, flat, gz * (float)CELL_SIZE, p);
    return SmoothOffsetAt(gx, gz, flat, cornerWeight, p[2]);
}

/* A land triangle of cell (xi, zi) on the curved surface. The curve itself is
   GPUTERRAIN.vert's: the triangle goes to it as a record of its three flat
   corners, each with its grid position (uv.zw), its share of the curve (none
   at the corners of a flat-drawn triangle: along an edge the curve then depends
   on its two ends only, the same from either side; normal.x) and the cube's
   height map (normal.y), and the shader grows it into sixteen small triangles
   moved toward the Catmull-Rom curve through the heights around them, fading
   with the distance and down at the sea's level. */
static S32 SmoothTri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], float mode, S32 flags,
                     S32 perVertexShade, float constantShade, S32 color, const U16 *uv) {
    static const S32 kCornerX[4] = {0, 0, 1, 1};
    static const S32 kCornerZ[4] = {0, 1, 1, 0};
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(3);
    if (v == NULL) {
        return FALSE;
    }
    for (S32 i = 0; i < 3; i++) {
        const S32 gx = xi + kCornerX[corners[i] & 3];
        const S32 gz = zi + kCornerZ[corners[i] & 3];
        const S32 g = gz * GRID_SIDE + gx;
        const float *p = s_grid[g];
        T_GPUOBJ_VERTEX *vert = &v[i];
        AffGpu_ViewVertex(vert, p[0], p[1], p[2]);
        vert->normal[0] = s_flatCorner[g] ? 0.0f : 1.0f;
        vert->normal[1] = (float)s_mapSlot;
        vert->normal[3] = perVertexShade ? (float)lights[i] / 256.0f : constantShade;
        vert->light[3] = mode;
        vert->vpos[3] = (float)flags;
        vert->mat[0] = (float)(color & 0xFF);
        vert->mat[1] = 0.0f;
        vert->mat[2] = 65535.0f;
        if (uv != NULL) {
            vert->uv[0] = (float)uv[i * 2] / 256.0f;
            vert->uv[1] = (float)uv[i * 2 + 1] / 256.0f;
        }
        vert->uv[2] = (float)gx;
        vert->uv[3] = (float)gz;
    }
    return TRUE;
}

/* A hash of integers to [0, 1): the same blades wherever the camera stands. */
static float GrassHash(U32 a, U32 b, U32 c) {
    U32 h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

/* The ground's colour under texel (u, v) (8.8), or under the fill colour. */
static S32 GrassGround(const T_GRASS_TRI *t, float u, float v) {
    S32 index;
    if (t->HasUv && s_draw->Page != NULL) {
        index = s_draw->Page[(((S32)v >> 8 & 255) << 8) | ((S32)u >> 8 & 255)];
        if (index == 0) {
            return FALSE;
        }
    } else {
        index = t->Color & 255;
    }
    if (s_draw->Clut != NULL) {
        index = s_draw->Clut[8 * 256 + index];
    }
    if (TerrainGpuPalette == NULL) {
        return TRUE;
    }
    const S32 r = TerrainGpuPalette[index * 3];
    const S32 g = TerrainGpuPalette[index * 3 + 1];
    const S32 b = TerrainGpuPalette[index * 3 + 2];
    /* Grass green or dry grass; not the earth or the stones its patches border. */
    return g * 10 >= r * 9 && g * 100 > b * 112;
}

/* Tufts of blades over a grass triangle, on the ground as it is drawn (curved
   by smooth terrain), coloured by the texel under their root. Their number
   and height fade out with the distance, blade by blade, so none pops. */
static void GrassTri(const T_GRASS_TRI *t) {
    static const S32 kCornerX[4] = {0, 0, 1, 1};
    static const S32 kCornerZ[4] = {0, 1, 1, 0};
    float cx[3], cz[3], ch[3], cw[3];
    for (S32 i = 0; i < 3; i++) {
        cx[i] = (float)(t->Xi + kCornerX[t->Corners[i] & 3]);
        cz[i] = (float)(t->Zi + kCornerZ[t->Corners[i] & 3]);
        const S32 g = (S32)cz[i] * GRID_SIDE + (S32)cx[i];
        ch[i] = (float)s_heights[g];
        cw[i] = s_flatCorner[g] ? 0.0f : 1.0f;
    }
    /* The blades face the camera: its right, level. */
    const TYPE_MAT *m = &MatriceWorld;
    if (m->F.M11 * m->F.M11 + m->F.M13 * m->F.M13 < 1e-6f) {
        return;
    }
    /* One wind over the island. */
    const float windX = 0.94f, windZ = 0.34f;
    const U32 key = (U32)(t->Corners[0] | t->Corners[1] << 2 | t->Corners[2] << 4);
    for (S32 k = 0; k < GRASS_TUFTS; k++) {
        float b0 = GrassHash((U32)t->Xi, (U32)t->Zi, key * 64u + (U32)k);
        float b1 = GrassHash((U32)t->Zi, (U32)t->Xi, key * 64u + (U32)k + 4096u);
        if (b0 + b1 > 1.0f) {
            b0 = 1.0f - b0;
            b1 = 1.0f - b1;
        }
        const float b2 = 1.0f - b0 - b1;
        const float gx = b0 * cx[0] + b1 * cx[1] + b2 * cx[2];
        const float gz = b0 * cz[0] + b1 * cz[1] + b2 * cz[2];
        const float flat = b0 * ch[0] + b1 * ch[1] + b2 * ch[2];
        float p[3];
        GridPoint(gx * (float)CELL_SIZE, flat, gz * (float)CELL_SIZE, p);
        /* Thinned out with the distance: each tuft has its own threshold, and
           shrinks into the ground as the distance reaches it. */
        const float density = (GRASS_FAR - p[2]) / (GRASS_FAR - GRASS_NEAR);
        const float rank = GrassHash((U32)t->Xi * 7u, (U32)t->Zi * 13u, key * 64u + (U32)k + 8192u) * 0.999f;
        float grow = (density - rank) * 6.0f;
        if (grow <= 0.0f || p[2] < 64.0f) {
            continue;
        }
        grow = grow > 1.0f ? 1.0f : grow;
        float u = 0.0f, tv = 0.0f;
        if (t->HasUv) {
            u = b0 * (float)t->Uv[0] + b1 * (float)t->Uv[2] + b2 * (float)t->Uv[4];
            tv = b0 * (float)t->Uv[1] + b1 * (float)t->Uv[3] + b2 * (float)t->Uv[5];
        }
        if (!GrassGround(t, u, tv)) {
            continue;
        }
        const float y =
            flat + (t->Smoothed ? SmoothOffset(gx, gz, flat, b0 * cw[0] + b1 * cw[1] + b2 * cw[2]) : 0.0f);
        const float shade = t->PerVertexShade ? (b0 * (float)t->Lights[0] + b1 * (float)t->Lights[1] +
                                                 b2 * (float)t->Lights[2]) /
                                                    256.0f
                                              : t->ConstantShade;
        const float wx = gx * (float)CELL_SIZE, wz = gz * (float)CELL_SIZE;
        /* Neighbouring tufts sway together: the gust travels along the wind. */
        const float phase = (wx * windX + wz * windZ) * 0.004f + GrassHash((U32)k, (U32)t->Xi, (U32)t->Zi) * 1.2f;
        /* The tuft goes to GPUTERRAIN.vert as its root, which grows its three
           blades: their spread, lean and height from the same hashes, facing
           the camera, bent by the wind. */
        T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(1);
        if (v == NULL) {
            return;
        }
        GridPoint(wx, y, wz, p);
        AffGpu_ViewVertex(v, p[0], p[1], p[2]);
        v->normal[0] = grow;
        v->normal[1] = (float)(k + 16 * (S32)key);
        v->normal[2] = (float)(t->Xi + 256 * t->Zi);
        v->normal[3] = shade;
        v->light[3] = t->Mode;
        v->vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_GRASS);
        v->mat[0] = (float)(t->Color & 0xFF);
        v->mat[1] = 0.0f;
        v->mat[2] = 65535.0f;
        v->mat[3] = phase;
        v->uv[0] = u / 256.0f;
        v->uv[1] = tv / 256.0f;
    }
}

/* --- The land, grown on the GPU ---------------------------------------------
   A record (GPUOBJ_LAND_*) is one fill of one half-cell: its colour layer or
   its texture layer, as DrawFeuillePolyZBuf would fill it. */
#define LAND_CELL_MASK 0xFFFu
#define LAND_HALF (1u << 12)
#define LAND_SENS (1u << 13)
#define LAND_WATER (1u << 14)
#define LAND_CHROMAKEY (1u << 15)
#define LAND_VERTEX_SHADE (1u << 16)
#define LAND_MODE_SHIFT 17 ///< 4 bits
#define LAND_FLAT_SHIFT 21 ///< 3 bits: the triangle's corners that do not curve
#define LAND_UV (1u << 24)
#define LAND_GRASS (1u << 25)

static U8 s_slotFlat[GPUOBJ_MAX_HEIGHTMAPS][GRID_SIDE * GRID_SIDE]; ///< corners of triangles drawn flat
static S32 s_landSlot = -1;

/* A triangle of half h of a cell whose diagonal runs one way or the other,
   as DrawFeuillePolyZBuf takes its corners. */
static const U8 kHalfCorners[2][2][3] = {{{0, 1, 2}, {2, 3, 0}}, {{3, 0, 1}, {1, 2, 3}}};

/* The mode and the shading of a fill of the given type, as TerrainGpu_Tri has
   them. PASS for a type the GPU does not draw. */
static float LandMode(S32 fillType, S32 color, S32 *perVertexShade, float *constantShade, S32 *chromakey) {
    *perVertexShade = FALSE;
    *constantShade = 0.0f;
    *chromakey = fillType >= POLY_TEXTURE_INCRUST && fillType <= POLY_TEXTURE_INCRUST_DITHER;
    switch (fillType) {
    case POLY_SOLID:
        return (float)GPUOBJ_MODE_SOLID;
    case POLY_GOURAUD_TABLE:
    case POLY_DITHER_TABLE:
        *perVertexShade = TRUE;
        return (float)GPUOBJ_MODE_CLUT;
    case POLY_TEXTURE:
    case POLY_TEXTURE_INCRUST:
        return (float)GPUOBJ_MODE_TEX;
    case POLY_TEXTURE_FLAT:
    case POLY_TEXTURE_INCRUST_FLAT:
        *constantShade = (float)(color & 0xFF);
        return (float)GPUOBJ_MODE_TEXSHADED;
    case POLY_TEXTURE_GOURAUD:
    case POLY_TEXTURE_INCRUST_GOURAUD:
        *perVertexShade = TRUE;
        return (float)GPUOBJ_MODE_TEXSHADED;
    default:
        return (float)GPUOBJ_MODE_PASS;
    }
}

S32 TerrainGpu_LandSlot(const S16 *heights) {
    s_landSlot = -1;
    if (!GpuObj_Active() || heights == NULL || TypeProj != TYPE_3D) {
        return FALSE;
    }
    s_landSlot = GpuObj_HeightMap(s_island, s_cubeX, s_cubeZ, heights, NULL);
    memcpy(s_heights, heights, sizeof(s_heights));
    return GpuObjLandCount[s_landSlot] < 0;
}

void TerrainGpu_LandBuild(const T_TERRAIN_HALF *halves, const U8 *intensity, const U16 *texDefs) {
    if (s_landSlot < 0) {
        return;
    }
    static const S32 kCX[4] = {0, 0, 1, 1};
    static const S32 kCZ[4] = {0, 1, 1, 0};
    U32 *out = GpuObjLandRecords + (size_t)s_landSlot * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS;
    U8 *flat = s_slotFlat[s_landSlot];
    memset(flat, 0, GRID_SIDE * GRID_SIDE);
    S32 n = 0;
    for (S32 cell = 0; cell < 64 * 64; cell++) {
        const S32 xi = cell & 63, zi = cell >> 6;
        const T_TERRAIN_HALF *pair = &halves[cell * 2];
        /* The classic walk's own shortcut: an empty cell draws nothing. */
        if (!(pair[0].PolyFlag + pair[0].TexFlag + pair[1].PolyFlag + pair[1].TexFlag)) {
            continue;
        }
        const S32 sens = pair[0].Sens;
        for (S32 half = 0; half < 2; half++) {
            const T_TERRAIN_HALF *h = &pair[half];
            const U8 *corners = kHalfCorners[sens][half];
            S32 light[3];
            U32 raw = 0;
            for (S32 i = 0; i < 3; i++) {
                const U8 r = intensity[(zi + kCZ[corners[i]]) * 65 + xi + kCX[corners[i]]] & 15;
                light[i] = r * 256 - 255 < 0 ? 0 : r * 256 - 255;
                raw |= (U32)r << (4 * i);
            }
            const S32 average = ((light[0] + light[1] + light[2]) * 21845) >> 24;
            S32 color = (h->Bank << 4) + 11;
            for (S32 layer = 0; layer < 2; layer++) {
                S32 fillType;
                S32 water = FALSE;
                if (layer == 0) {
                    if (!h->PolyFlag) {
                        continue;
                    }
                    if (h->PolyFlag == 1) {
                        color += average;
                    }
                    fillType = h->PolyType;
                } else {
                    if (!h->TexFlag) {
                        continue;
                    }
                    if (h->TexFlag == 2) {
                        color = average;
                    }
                    fillType = h->TexType;
                    water = h->CodeJeu == LAND_CJ_FOOT_WATER || h->CodeJeu == LAND_CJ_ANIMATED_WATER;
                }
                S32 perVertexShade, chromakey;
                float constantShade;
                const float mode = LandMode(fillType, color, &perVertexShade, &constantShade, &chromakey);
                if (water || mode == (float)GPUOBJ_MODE_PASS) {
                    for (S32 i = 0; i < 3; i++) {
                        flat[(zi + kCZ[corners[i]]) * GRID_SIDE + xi + kCX[corners[i]]] = 1;
                    }
                }
                U32 *r = out + n * GPUOBJ_LAND_WORDS;
                r[0] = (U32)cell | (half ? LAND_HALF : 0) | (sens ? LAND_SENS : 0) | (water ? LAND_WATER : 0) |
                       (chromakey ? LAND_CHROMAKEY : 0) | (perVertexShade ? LAND_VERTEX_SHADE : 0) |
                       ((U32)mode << LAND_MODE_SHIFT) | (layer == 1 ? LAND_UV : 0) |
                       (h->Step == GRASS_STEP && !water && mode != (float)GPUOBJ_MODE_PASS ? LAND_GRASS : 0);
                r[1] = (U32)(color & 0xFF) | ((U32)constantShade << 8) | raw << 16;
                /* The texture's corners, in the triangle's order. */
                for (S32 i = 0; i < 3; i++) {
                    r[2 + i] = layer == 1 && texDefs != NULL
                                   ? (U32)texDefs[h->IndexTex * 6 + i * 2] | (U32)texDefs[h->IndexTex * 6 + i * 2 + 1] << 16
                                   : 0;
                }
                r[5] = r[6] = r[7] = 0;
                n++;
            }
        }
    }
    /* Which corners do not curve is known once every triangle is. */
    for (S32 k = 0; k < n; k++) {
        U32 *r = out + k * GPUOBJ_LAND_WORDS;
        const S32 cell = (S32)(r[0] & LAND_CELL_MASK);
        const U8 *corners = kHalfCorners[(r[0] & LAND_SENS) != 0][(r[0] & LAND_HALF) != 0];
        for (S32 i = 0; i < 3; i++) {
            if (flat[((cell >> 6) + kCZ[corners[i]]) * GRID_SIDE + (cell & 63) + kCX[corners[i]]]) {
                r[0] |= 1u << (LAND_FLAT_SHIFT + i);
            }
        }
    }
    GpuObjLandCount[s_landSlot] = n;
    GpuObj_CubeSlotChanged(s_landSlot);
}

void TerrainGpu_LandDraw(U8 *groundTexture, U8 *clutBase, S32 fogStart, S32 fogEnd, S32 fogColor, S32 nearest) {
    s_draw = NULL;
    if (s_landSlot < 0 || GpuObjLandCount[s_landSlot] <= 0) {
        return;
    }
    OpenDraw(groundTexture, clutBase, fogStart, fogEnd, fogColor);
    if (s_draw == NULL) {
        return;
    }
    if (GpuObj_AllocVerts(1) == NULL) { /* a draw holds a vertex, or it is dropped */
        s_draw = NULL;
        GpuObj_EndDraw();
        return;
    }
    s_draw->Procedural = GPUOBJ_PROC_LAND;
    /* The rotation the records are placed with is the one the cube is drawn
       with, whether or not the scene's camera has been recorded yet this frame. */
    {
        const TYPE_MAT *m = &MatriceWorld;
        const float rows[9] = {m->F.M11, m->F.M12, m->F.M13, m->F.M21, m->F.M22, m->F.M23, m->F.M31, m->F.M32, m->F.M33};
        memcpy(s_draw->CapMat, rows, sizeof(rows));
        s_draw->HasCapCamera = TRUE;
    }
    s_draw->LandSlot = s_landSlot;
    s_draw->LandSmooth = TerrainGpuSmooth && (float)nearest < SMOOTH_FADE_FAR;
    s_draw->LandOrigin[0] = -(float)CameraXr;
    s_draw->LandOrigin[1] = -(float)CameraYr;
    s_draw->LandOrigin[2] = -(float)CameraZr;
    s_draw->LandPhase[0] = (float)s_cubeX * 64.0f;
    s_draw->LandPhase[1] = (float)s_cubeZ * 64.0f;
    s_draw->LandFar = (float)fogEnd;

    /* The grass, which the CPU still decides a tuft at a time: the texel under
       it, its root on the ground as drawn. */
    s_nbGrassTris = 0;
    memcpy(s_flatCorner, s_slotFlat[s_landSlot], sizeof(s_flatCorner));
    if (TerrainGpuGrass) {
        CurveCacheSelect();
        static const S32 kCX[4] = {0, 0, 1, 1};
        static const S32 kCZ[4] = {0, 1, 1, 0};
        const U32 *rec = GpuObjLandRecords + (size_t)s_landSlot * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS;
        const S32 n = GpuObjLandCount[s_landSlot];
        for (S32 k = 0; k < n && s_nbGrassTris < MAX_GRASS_TRIS; k++, rec += GPUOBJ_LAND_WORDS) {
            if (!(rec[0] & LAND_GRASS)) {
                continue;
            }
            const S32 cell = (S32)(rec[0] & LAND_CELL_MASK);
            const S32 xi = cell & 63, zi = cell >> 6;
            const U8 *corners = kHalfCorners[(rec[0] & LAND_SENS) != 0][(rec[0] & LAND_HALF) != 0];
            float depth = 1e30f;
            for (S32 i = 0; i < 3; i++) {
                float p[3];
                const S32 gx = xi + kCX[corners[i]], gz = zi + kCZ[corners[i]];
                GridPoint((float)(gx * CELL_SIZE), (float)s_heights[gz * GRID_SIDE + gx], (float)(gz * CELL_SIZE), p);
                depth = p[2] < depth ? p[2] : depth;
            }
            if (depth >= GRASS_FAR || depth <= 0.0f) {
                continue;
            }
            T_GRASS_TRI *t = &s_grassTris[s_nbGrassTris++];
            t->Xi = (S16)xi;
            t->Zi = (S16)zi;
            t->HasUv = (rec[0] & LAND_UV) != 0;
            t->Smoothed = s_draw->LandSmooth && depth < SMOOTH_FADE_FAR;
            for (S32 i = 0; i < 3; i++) {
                t->Corners[i] = corners[i];
                const S32 r = (S32)((rec[1] >> (16 + 4 * i)) & 15);
                t->Lights[i] = r * 256 - 255 < 0 ? 0 : r * 256 - 255;
                const U32 uv = rec[2 + i];
                t->Uv[i * 2] = (U16)(uv & 0xFFFF);
                t->Uv[i * 2 + 1] = (U16)(uv >> 16);
            }
            t->Mode = (float)((rec[0] >> LAND_MODE_SHIFT) & 15);
            t->PerVertexShade = (rec[0] & LAND_VERTEX_SHADE) != 0;
            t->Color = (S32)(rec[1] & 0xFF);
            t->ConstantShade = (float)((rec[1] >> 8) & 0xFF);
        }
    }
}

// --- Interface ---------------------------------------------------------------
void TerrainGpu_BeginCube(const S16 *heights, U8 *groundTexture, U8 *clutBase, S32 fogStart, S32 fogEnd,
                          S32 fogColor) {
    s_draw = NULL;
    if (!GpuObj_Active() || heights == NULL || TypeProj != TYPE_3D) {
        return;
    }
    OpenDraw(groundTexture, clutBase, fogStart, fogEnd, fogColor);
    if (s_draw == NULL) {
        return;
    }
    s_nbSmoothTris = 0;
    s_nbGrassTris = 0;
    memset(s_flatCorner, 0, sizeof(s_flatCorner));
    CurveCacheSelect();
    if (TerrainGpuSmooth) {
        s_mapSlot = GpuObj_HeightMap(s_island, s_cubeX, s_cubeZ, heights, NULL);
    }

    /* LongWorldRotatePoint then LongProjectPoint's camera offsets, in float. */
    const TYPE_MAT *m = &MatriceWorld;
    for (S32 zi = 0; zi < GRID_SIDE; zi++) {
        for (S32 xi = 0; xi < GRID_SIDE; xi++) {
            const float x = (float)(xi * CELL_SIZE);
            const float y = (float)heights[zi * GRID_SIDE + xi];
            const float z = (float)(zi * CELL_SIZE);
            s_heights[zi * GRID_SIDE + xi] = heights[zi * GRID_SIDE + xi];
            float *out = s_grid[zi * GRID_SIDE + xi];
            out[0] = x * m->F.M11 + y * m->F.M12 + z * m->F.M13 - (float)CameraXr;
            out[1] = x * m->F.M21 + y * m->F.M22 + z * m->F.M23 - (float)CameraYr;
            out[2] = (float)CameraZr - (x * m->F.M31 + y * m->F.M32 + z * m->F.M33);
        }
    }
}

S32 TerrainGpu_Tri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], S32 fillType, S32 color,
                   const U16 *uv, S32 water, S32 step) {
    if (s_draw == NULL) {
        return FALSE;
    }

    float mode;
    S32 flags = GPUOBJ_FLAG_BAKED;
    S32 perVertexShade = FALSE;
    float constantShade = 0.0f;

    switch (fillType) {
    case POLY_SOLID:
        mode = (float)GPUOBJ_MODE_SOLID;
        break;
    case POLY_GOURAUD_TABLE:
    case POLY_DITHER_TABLE:
        mode = (float)GPUOBJ_MODE_CLUT;
        perVertexShade = TRUE;
        break;
    case POLY_TEXTURE:
    case POLY_TEXTURE_INCRUST:
        mode = (float)GPUOBJ_MODE_TEX;
        break;
    case POLY_TEXTURE_FLAT:
    case POLY_TEXTURE_INCRUST_FLAT:
        mode = (float)GPUOBJ_MODE_TEXSHADED;
        constantShade = (float)(color & 0xFF);
        break;
    case POLY_TEXTURE_GOURAUD:
    case POLY_TEXTURE_INCRUST_GOURAUD:
        mode = (float)GPUOBJ_MODE_TEXSHADED;
        perVertexShade = TRUE;
        break;
    default:
        /* Anything else keeps the software colour. */
        mode = (float)GPUOBJ_MODE_PASS;
        break;
    }
    if (fillType >= POLY_TEXTURE_INCRUST && fillType <= POLY_TEXTURE_INCRUST_DITHER) {
        flags |= GPUOBJ_FLAG_CHROMAKEY;
    }

    if (TerrainGpuGrass && step == GRASS_STEP && !water && mode != (float)GPUOBJ_MODE_PASS &&
        s_nbGrassTris < MAX_GRASS_TRIS) {
        static const S32 kGX[4] = {0, 0, 1, 1};
        static const S32 kGZ[4] = {0, 1, 1, 0};
        float depth = 1e30f;
        for (S32 i = 0; i < 3; i++) {
            const S32 g = (zi + kGZ[corners[i] & 3]) * GRID_SIDE + xi + kGX[corners[i] & 3];
            depth = s_grid[g][2] < depth ? s_grid[g][2] : depth;
        }
        if (depth < GRASS_FAR) {
            T_GRASS_TRI *t = &s_grassTris[s_nbGrassTris++];
            t->Xi = (S16)xi;
            t->Zi = (S16)zi;
            t->HasUv = uv != NULL;
            t->Smoothed = TerrainGpuSmooth && depth < SMOOTH_FADE_FAR && s_nbSmoothTris < MAX_SMOOTH_TRIS;
            for (S32 i = 0; i < 3; i++) {
                t->Corners[i] = corners[i];
                t->Lights[i] = lights[i];
            }
            for (S32 i = 0; i < 6; i++) {
                t->Uv[i] = uv != NULL ? uv[i] : 0;
            }
            t->Mode = mode;
            t->PerVertexShade = perVertexShade;
            t->ConstantShade = constantShade;
            t->Color = color;
        }
    }

    if (TerrainGpuSmooth) {
        static const S32 kCX[4] = {0, 0, 1, 1};
        static const S32 kCZ[4] = {0, 1, 1, 0};
        float depth = 1e30f;
        for (S32 i = 0; i < 3; i++) {
            const S32 g = (zi + kCZ[corners[i] & 3]) * GRID_SIDE + xi + kCX[corners[i] & 3];
            depth = s_grid[g][2] < depth ? s_grid[g][2] : depth;
        }
        if (!water && mode != (float)GPUOBJ_MODE_PASS && depth < SMOOTH_FADE_FAR && s_nbSmoothTris < MAX_SMOOTH_TRIS) {
            T_SMOOTH_TRI *t = &s_smoothTris[s_nbSmoothTris++];
            t->Xi = (S16)xi;
            t->Zi = (S16)zi;
            t->HasUv = uv != NULL;
            for (S32 i = 0; i < 3; i++) {
                t->Corners[i] = corners[i];
                t->Lights[i] = lights[i];
            }
            for (S32 i = 0; i < 6; i++) {
                t->Uv[i] = uv != NULL ? uv[i] : 0;
            }
            t->Mode = mode;
            t->Flags = flags;
            t->PerVertexShade = perVertexShade;
            t->ConstantShade = constantShade;
            t->Color = color;
            return TRUE;
        }
        for (S32 i = 0; i < 3; i++) {
            s_flatCorner[(zi + kCZ[corners[i] & 3]) * GRID_SIDE + xi + kCX[corners[i] & 3]] = 1;
        }
    }

    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(3);
    if (v == NULL) {
        return FALSE;
    }
    static const S32 kCornerX[4] = {0, 0, 1, 1};
    static const S32 kCornerZ[4] = {0, 1, 1, 0};
    for (S32 i = 0; i < 3; i++) {
        const S32 gx = xi + kCornerX[corners[i] & 3];
        const S32 gz = zi + kCornerZ[corners[i] & 3];
        const float *p = s_grid[gz * GRID_SIDE + gx];
        /* CodeJeu shoreline triangles carry a textured foam transition over
           dry land. Keep that authored boundary fixed; only DrawOneSea's
           broad, all-water tiles are subdivided and displaced. */
        AffGpu_ViewVertex(&v[i], p[0], p[1], p[2]);
        v[i].normal[3] = perVertexShade ? (float)lights[i] / 256.0f : constantShade;
        v[i].light[3] = mode;
        v[i].vpos[3] = (float)(flags | (water ? GPUOBJ_FLAG_WATER | GPUOBJ_FLAG_WATER_TERRAIN : 0));
        v[i].mat[0] = (float)(color & 0xFF);
        v[i].mat[1] = 0.0f;
        v[i].mat[2] = 65535.0f;
        if (uv != NULL) {
            v[i].uv[0] = (float)uv[i * 2] / 256.0f;
            v[i].uv[1] = (float)uv[i * 2 + 1] / 256.0f;
        }
        if (water) {
            const TYPE_MAT *m = &MatriceWorld;
            v[i].uv[2] = (p[0] * m->F.M11 + p[1] * m->F.M21 - p[2] * m->F.M31 + s_waterCameraX) / 512.0f;
            v[i].uv[3] = (p[0] * m->F.M13 + p[1] * m->F.M23 - p[2] * m->F.M33 + s_waterCameraZ) / 512.0f;
        }
    }

    /* The colour is the GPU's from here on, unless the material said otherwise:
       the software fill only has to leave a tagged, depth-correct pixel. */
    return mode != (float)GPUOBJ_MODE_PASS AND !water;
}
void TerrainGpu_SetWaterScene(S32 island, S32 cubeX, S32 cubeZ) {
    s_island = island;
    TerrainGpu_SetWaterCube(cubeX, cubeZ);
    const TYPE_MAT *m = &MatriceWorld;
    const float x[3] = {m->F.M11, m->F.M21, m->F.M31};
    const float y[3] = {m->F.M12, m->F.M22, m->F.M32};
    const float z[3] = {m->F.M13, m->F.M23, m->F.M33};
    GpuWater_SetScene(island);
    GpuWater_SetView(x, y, z);

    /* Project the same island-global impacts for the compositor's spray. */
    for (S32 i = 0; i < GPUWATER_MAX_IMPACTS; i++) {
        float impact[5];
        if (!GpuWater_GetImpactWorld(i, impact)) {
            GpuWater_SetImpactScreen(i, 0.0f, 0.0f, FALSE);
            continue;
        }
        const float px = impact[0] - (float)cubeX * 32768.0f;
        float height;
        GpuWater_SampleSurface(impact[0], impact[2], &height, NULL);
        const float py = impact[1] + height;
        const float pz = impact[2] - (float)cubeZ * 32768.0f;
        const float vx = px * m->F.M11 + py * m->F.M12 + pz * m->F.M13 - (float)CameraXr;
        const float vy = px * m->F.M21 + py * m->F.M22 + pz * m->F.M23 - (float)CameraYr;
        const float depth = (float)CameraZr - (px * m->F.M31 + py * m->F.M32 + pz * m->F.M33);
        if (depth <= 1.0f || ClipXMax < 1 || ClipYMax < 1) {
            GpuWater_SetImpactScreen(i, 0.0f, 0.0f, FALSE);
            continue;
        }
        const float sx = (float)XCentre + vx * FRatioX / depth;
        const float sy = (float)YCentre + vy * FRatioX * FRatioY / depth;
        const float u = sx / (float)(ClipXMax + 1);
        const float v = sy / (float)(ClipYMax + 1);
        GpuWater_SetImpactScreen(i, u, v, u > -0.1f && u < 1.1f && v > -0.2f && v < 1.1f);
    }
}

void TerrainGpu_SetWaterCube(S32 cubeX, S32 cubeZ) {
    s_cubeX = cubeX;
    s_cubeZ = cubeZ;
    s_waterCameraX = (float)CameraX + (float)cubeX * 32768.0f;
    s_waterCameraZ = (float)CameraZ + (float)cubeZ * 32768.0f;
}

void TerrainGpu_BeginPlane(U8 *page, S32 texOffset, S32 texMask, S32 fogStart, S32 fogEnd, S32 fogColor,
                           S32 sea) {
    s_draw = NULL;
    s_nbSmoothTris = 0;
    s_waterPlane = sea;
    s_skyPlane = !sea;
    s_skyBehind = FALSE;
    if (!GpuObj_Active() || TypeProj != TYPE_3D) {
        return;
    }
    OpenDraw(page, NULL, fogStart, fogEnd, fogColor);
    s_planeTexOffset = (float)texOffset;
    s_planeTexMask = (float)(texMask & 0xFFFF);
}

S32 TerrainGpu_Quad(const STRUC_CLIPVERTEX src[4]) {
    if (s_draw == NULL) {
        return FALSE;
    }
    if (s_waterPlane && GpuWaterEnabled) {
        /* 8x8 cells per original sea tile bounds a horizon frame to 55,296
           vertices while resolving the broad waves that move the silhouette. */
        T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(SEA_SUBDIVISIONS * SEA_SUBDIVISIONS * 6);
        if (v == NULL) {
            return FALSE;
        }
        for (S32 z = 0; z < SEA_SUBDIVISIONS; z++) {
            for (S32 x = 0; x < SEA_SUBDIVISIONS; x++) {
                const float u0 = (float)x / (float)SEA_SUBDIVISIONS;
                const float u1 = (float)(x + 1) / (float)SEA_SUBDIVISIONS;
                const float t0 = (float)z / (float)SEA_SUBDIVISIONS;
                const float t1 = (float)(z + 1) / (float)SEA_SUBDIVISIONS;
                WaterVertex(v++, src, u0, t0);
                WaterVertex(v++, src, u0, t1);
                WaterVertex(v++, src, u1, t1);
                WaterVertex(v++, src, u0, t0);
                WaterVertex(v++, src, u1, t1);
                WaterVertex(v++, src, u1, t0);
            }
        }
        return TRUE;
    }
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v == NULL) {
        return FALSE;
    }
    static const S32 order[6] = {0, 1, 2, 0, 2, 3};
    for (S32 i = 0; i < 6; i++) {
        const STRUC_CLIPVERTEX *p = &src[order[i]];
        AffGpu_ViewVertex(&v[i], (float)p->V_X0, (float)p->V_Y0, (float)p->V_Z0);
        v[i].light[3] = (float)GPUOBJ_MODE_TEX;
        v[i].vpos[3] = (float)GPUOBJ_FLAG_BAKED;
        v[i].mat[1] = s_planeTexOffset;
        v[i].mat[2] = s_planeTexMask;
        v[i].uv[0] = (float)p->V_MapU / 256.0f;
        v[i].uv[1] = (float)p->V_MapV / 256.0f;
        if (s_waterPlane) {
            v[i].vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_WATER);
            v[i].light[0] = (float)CameraXLight;
            v[i].light[1] = (float)CameraYLight;
            v[i].light[2] = (float)CameraZLight;
            /* Undo the view rotation, then add the island-space main camera.
               Horizon cubes use shifted cameras but share this reference. */
            const TYPE_MAT *m = &MatriceWorld;
            v[i].uv[2] = (p->V_X0 * m->F.M11 + p->V_Y0 * m->F.M21 - p->V_Z0 * m->F.M31 +
                           s_waterCameraX) / 512.0f;
            v[i].uv[3] = (p->V_X0 * m->F.M13 + p->V_Y0 * m->F.M23 - p->V_Z0 * m->F.M33 +
                           s_waterCameraZ) / 512.0f;
        } else if (s_skyPlane) {
            v[i].vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_SKY);
            if (s_skyBehind) {
                /* At the back of the scene's depth, wherever it lies. */
                v[i].clip[2] = 0.999f * v[i].clip[3];
            }
        }
    }
    return TRUE;
}

void TerrainGpu_SkyBehind(void) {
    s_skyBehind = s_skyPlane;
}

void TerrainGpu_End(void) {
    if (s_draw == NULL) {
        return;
    }
    /* The land triangles, now that every flat corner is known. They go in a
       draw of their own, which GPUTERRAIN.vert grows from their records: the
       flat land before them and the grass after them are drawn as they are. */
    const T_GPUOBJ_DRAW material = *s_draw;
    if (s_nbSmoothTris > 0 && ReopenDraw(&material)) {
        s_draw->Procedural = GPUOBJ_PROC_TERRAIN;
        for (S32 k = 0; k < s_nbSmoothTris; k++) {
            const T_SMOOTH_TRI *t = &s_smoothTris[k];
            SmoothTri(t->Xi, t->Zi, t->Corners, t->Lights, t->Mode, t->Flags, t->PerVertexShade, t->ConstantShade,
                      t->Color, t->HasUv ? t->Uv : NULL);
        }
    }
    s_nbSmoothTris = 0;
    if (s_nbGrassTris > 0 && s_draw != NULL && ReopenDraw(&material)) {
        s_draw->Procedural = GPUOBJ_PROC_GRASS;
        for (S32 k = 0; k < s_nbGrassTris; k++) {
            GrassTri(&s_grassTris[k]);
        }
    }
    s_nbGrassTris = 0;
    s_draw = NULL;
    GpuObj_EndDraw();
}

// =============================================================================
#ifdef __cplusplus
}
#endif
