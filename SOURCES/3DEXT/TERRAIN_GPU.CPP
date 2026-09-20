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
#define SEA_SUBDIVISIONS 8

// --- Private state -----------------------------------------------------------
static T_GPUOBJ_DRAW *s_draw = NULL;

/* Texel addressing of the plane being drawn (sea, sky). */
static float s_planeTexOffset = 0.0f;
static float s_planeTexMask = 65535.0f;
static S32 s_waterPlane = FALSE;
static S32 s_skyPlane = FALSE;
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

/* Grass (gfx_grass): blades standing on the ground the island marks as grass. */
S32 TerrainGpuGrass = TRUE;
S32 TerrainGpuFastFill = TRUE;
const U8 *TerrainGpuPalette = NULL;

/* The footstep sound of grass (T_HALF_POLY.SampleStep): the island's own mark
   of where grass grows. */
#define GRASS_STEP 2
#define GRASS_TUFTS 12   ///< per triangle (half a cell) near the camera: a lawn, not a thicket
#define GRASS_BLADES 3   ///< per tuft
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
    s_draw->Page = page;
    s_draw->Clut = clut;
    s_draw->FogStart = fogStart;
    s_draw->FogEnd = fogEnd;
    s_draw->FogColor = fogColor;
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

static void DisplacedWaterVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth, float texU, float texV) {
    float normal[3];
    const TYPE_MAT *m = &MatriceWorld;
    const float worldX = x * m->F.M11 + y * m->F.M21 - depth * m->F.M31 + s_waterCameraX;
    const float worldZ = x * m->F.M13 + y * m->F.M23 - depth * m->F.M33 + s_waterCameraZ;
    float height;
    GpuWater_SampleSurface(worldX, worldZ, &height, normal);

    /* Displace before projection. Homogeneous clipping then handles cells
       straddling the near plane, and the scene depth buffer sees the crest. */
    AffGpu_ViewVertex(v, x + height * m->F.M12, y + height * m->F.M22, depth - height * m->F.M32);
    v->normal[0] = normal[0] * m->F.M11 + normal[1] * m->F.M12 + normal[2] * m->F.M13;
    v->normal[1] = normal[0] * m->F.M21 + normal[1] * m->F.M22 + normal[2] * m->F.M23;
    v->normal[2] = -(normal[0] * m->F.M31 + normal[1] * m->F.M32 + normal[2] * m->F.M33);
    /* The contact pass compares the fixed sea datum to the fixed shoreline;
       the renderer subtracts this interpolated vertical displacement there. */
    v->normal[3] = height;
    v->vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_WATER);
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
    DisplacedWaterVertex(v, point[0], point[1], point[2], point[3] / 256.0f, point[4] / 256.0f);
}

/* How far smooth terrain lifts the point of grid position (gx, gz) whose
   original triangle lies at height flat; cornerWeight is the share of its
   corners that may curve (none at a flat-drawn triangle's). */
static float SmoothOffset(float gx, float gz, float flat, float cornerWeight) {
    float p[3];
    GridPoint(gx * (float)CELL_SIZE, flat, gz * (float)CELL_SIZE, p);
    /* How much of the curve this point takes: none far away or at the sea. */
    float w = (SMOOTH_FADE_FAR - p[2]) / (SMOOTH_FADE_FAR - SMOOTH_FADE_NEAR);
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

/* A land triangle of cell (xi, zi) cut into smaller ones lying on the curved
   surface. The curve fades out with the distance and down at the sea's level,
   so where cells are drawn flat (far away, water) their edges stay straight
   and still meet the curved cells beside them. FALSE: far enough to stay flat. */
static S32 SmoothTri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], float mode, S32 flags,
                     S32 perVertexShade, float constantShade, S32 color, const U16 *uv) {
    static const S32 kCornerX[4] = {0, 0, 1, 1};
    static const S32 kCornerZ[4] = {0, 1, 1, 0};
    float cx[3], cz[3], ch[3], cw[3];
    for (S32 i = 0; i < 3; i++) {
        cx[i] = (float)(xi + kCornerX[corners[i] & 3]);
        cz[i] = (float)(zi + kCornerZ[corners[i] & 3]);
        const S32 g = (S32)cz[i] * GRID_SIDE + (S32)cx[i];
        ch[i] = (float)s_heights[g];
        /* No curve at the corners of flat-drawn triangles: along an edge it
           then depends on its two ends only, the same from either side. */
        cw[i] = s_flatCorner[g] ? 0.0f : 1.0f;
    }
    const S32 n = 4;
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts((U32)(n * n * 3));
    if (v == NULL) {
        return FALSE;
    }
    /* Rows of small triangles over barycentric steps (i, j) of corners 0 and 1. */
    S32 out = 0;
    for (S32 i = 0; i < n; i++) {
        for (S32 j = 0; j < n - i; j++) {
            for (S32 half = 0; half < 2; half++) {
                if (half == 1 && i + j >= n - 1) {
                    break;
                }
                const S32 pi[3] = {half ? i + 1 : i, i + 1, i};
                const S32 pj[3] = {j, half ? j + 1 : j, j + 1};
                for (S32 k = 0; k < 3; k++) {
                    const float b0 = (float)pi[k] / (float)n;
                    const float b1 = (float)pj[k] / (float)n;
                    const float b2 = 1.0f - b0 - b1;
                    const float gx = b0 * cx[0] + b1 * cx[1] + b2 * cx[2];
                    const float gz = b0 * cz[0] + b1 * cz[1] + b2 * cz[2];
                    /* The original triangle's plane: straight along its edges and the
                       cell's diagonal, as the classic terrain lies. */
                    const float flat = b0 * ch[0] + b1 * ch[1] + b2 * ch[2];
                    float p[3];
                    GridPoint(gx * (float)CELL_SIZE,
                              flat + SmoothOffset(gx, gz, flat, b0 * cw[0] + b1 * cw[1] + b2 * cw[2]),
                              gz * (float)CELL_SIZE, p);
                    T_GPUOBJ_VERTEX *vert = &v[out++];
                    AffGpu_ViewVertex(vert, p[0], p[1], p[2]);
                    vert->normal[3] = perVertexShade ? (b0 * (float)lights[0] + b1 * (float)lights[1] +
                                                        b2 * (float)lights[2]) /
                                                           256.0f
                                                     : constantShade;
                    vert->light[3] = mode;
                    /* Marked for the shadow rays; the first triangle carries the
                       original one's corners for them. */
                    vert->vpos[3] = (float)(flags | GPUOBJ_FLAG_DETAIL | (out <= 3 ? GPUOBJ_FLAG_DETAIL_ORIGIN : 0));
                    if (out <= 3) {
                        const float *c = s_grid[(S32)cz[out - 1] * GRID_SIDE + (S32)cx[out - 1]];
                        vert->normal[0] = c[0];
                        vert->normal[1] = c[1];
                        vert->normal[2] = -c[2];
                    }
                    vert->mat[0] = (float)(color & 0xFF);
                    vert->mat[1] = 0.0f;
                    vert->mat[2] = 65535.0f;
                    if (uv != NULL) {
                        vert->uv[0] = (b0 * (float)uv[0] + b1 * (float)uv[2] + b2 * (float)uv[4]) / 256.0f;
                        vert->uv[1] = (b0 * (float)uv[1] + b1 * (float)uv[3] + b2 * (float)uv[5]) / 256.0f;
                    }
                }
            }
        }
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

/* One grass vertex: cube-local world point, its shade, the tip's sway. */
static void GrassVertex(T_GPUOBJ_VERTEX *v, const float w[3], float shade, float mode, S32 color, float u, float tv,
                        const float *sway, float phase) {
    float p[3];
    GridPoint(w[0], w[1], w[2], p);
    AffGpu_ViewVertex(v, p[0], p[1], p[2]);
    v->normal[3] = shade;
    v->light[3] = mode;
    v->vpos[3] = (float)(GPUOBJ_FLAG_BAKED | GPUOBJ_FLAG_GRASS);
    v->mat[0] = (float)(color & 0xFF);
    v->mat[1] = 0.0f;
    v->mat[2] = 65535.0f;
    v->mat[3] = phase;
    v->uv[0] = u;
    v->uv[1] = tv;
    if (sway != NULL) {
        /* The clip-space offset of a unit of wind: the vertex shader moves
           the tip along it (clip coordinates are linear in the position). */
        float q[3];
        T_GPUOBJ_VERTEX moved;
        GridPoint(w[0] + sway[0], w[1] + sway[1], w[2] + sway[2], q);
        AffGpu_ViewVertex(&moved, q[0], q[1], q[2]);
        v->uv[2] = moved.clip[0] - v->clip[0];
        v->uv[3] = moved.clip[1] - v->clip[1];
    }
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
    float rx = m->F.M11, rz = m->F.M13;
    const float rl = rx * rx + rz * rz;
    if (rl < 1e-6f) {
        return;
    }
    const float inv = 1.0f / sqrtf(rl);
    rx *= inv;
    rz *= inv;
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
        T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(GRASS_BLADES * 3);
        if (v == NULL) {
            return;
        }
        for (S32 j = 0; j < GRASS_BLADES; j++) {
            const float h0 = GrassHash((U32)k * 3u + (U32)j, (U32)t->Xi * 31u + key, (U32)t->Zi * 17u);
            const float h1 = GrassHash((U32)t->Zi * 5u + key, (U32)k * 3u + (U32)j, (U32)t->Xi * 11u);
            const float height = (55.0f + 60.0f * h0) * grow;
            const float half = 9.0f + 5.0f * h1;
            /* A little apart from the tuft's centre, leaning out of it. */
            const float angle = ((float)j + h1) * 2.0944f;
            const float ox = cosf(angle), oz = sinf(angle);
            const float base[3] = {wx + ox * 18.0f * h0, y, wz + oz * 18.0f * h0};
            const float left[3] = {base[0] - rx * half, y - 6.0f, base[2] - rz * half};
            const float right[3] = {base[0] + rx * half, y - 6.0f, base[2] + rz * half};
            const float lean = height * (0.18f + 0.22f * h1);
            const float tip[3] = {base[0] + ox * lean, y + height, base[2] + oz * lean};
            const float sway[3] = {windX * height * 0.3f, -height * 0.05f, windZ * height * 0.3f};
            /* Darker at the root, in the grass's own shadow; lit at the tip. */
            const float root = shade * 0.62f;
            float top = shade * 1.08f + 0.6f;
            top = top > 15.0f ? 15.0f : top;
            GrassVertex(&v[j * 3], left, root, t->Mode, t->Color, u / 256.0f, tv / 256.0f, NULL, phase);
            GrassVertex(&v[j * 3 + 1], right, root, t->Mode, t->Color, u / 256.0f, tv / 256.0f, NULL, phase);
            GrassVertex(&v[j * 3 + 2], tip, top, t->Mode, t->Color, u / 256.0f, tv / 256.0f, sway, phase);
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
    if (!GpuObj_Active() || TypeProj != TYPE_3D) {
        return;
    }
    OpenDraw(page, NULL, fogStart, fogEnd, fogColor);
    s_planeTexOffset = (float)texOffset;
    s_planeTexMask = (float)(texMask & 0xFFFF);
}

void TerrainGpu_Quad(const STRUC_CLIPVERTEX src[4]) {
    if (s_draw == NULL) {
        return;
    }
    if (s_waterPlane && GpuWaterEnabled) {
        /* 8x8 cells per original sea tile bounds a horizon frame to 55,296
           vertices while resolving the broad waves that move the silhouette. */
        T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(SEA_SUBDIVISIONS * SEA_SUBDIVISIONS * 6);
        if (v == NULL) {
            return;
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
        return;
    }
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v == NULL) {
        return;
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
        }
    }
}

void TerrainGpu_End(void) {
    if (s_draw == NULL) {
        return;
    }
    /* The land triangles, now that every flat corner is known. */
    for (S32 k = 0; k < s_nbSmoothTris; k++) {
        const T_SMOOTH_TRI *t = &s_smoothTris[k];
        SmoothTri(t->Xi, t->Zi, t->Corners, t->Lights, t->Mode, t->Flags, t->PerVertexShade, t->ConstantShade,
                  t->Color, t->HasUv ? t->Uv : NULL);
    }
    s_nbSmoothTris = 0;
    for (S32 k = 0; k < s_nbGrassTris; k++) {
        GrassTri(&s_grassTris[k]);
    }
    s_nbGrassTris = 0;
    s_draw = NULL;
    GpuObj_EndDraw();
}

// =============================================================================
#ifdef __cplusplus
}
#endif
