#include "3DEXT/TERRAIN_GPU.H"
#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <OBJECT/AFF_GPU.H>
#include <POLYGON/POLY.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>
#include <SVGA/GPUWATER.H>

#include <cmath>
#include <cstdio>
#include <cstring>

/* Exercise the production capture without a window or a retail asset. */
extern "C" {
TYPE_MAT MatriceWorld;
S32 CameraX, CameraY, CameraZ, CameraXr, CameraYr, CameraZr;
S32 XCentre = 320, YCentre = 240;
float FRatioX = 1000.0f, FRatioY = 1.0f;
S32 CameraXLight = 0, CameraYLight = 1, CameraZLight = 0;
S32 TypeProj = TYPE_3D;
S32 ClipXMin = 0, ClipYMin = 0, ClipXMax = 639, ClipYMax = 479;
S32 GpuObjEnabled = TRUE, GpuObjAvailable = TRUE, GpuObjLava = TRUE;
}

static T_GPUOBJ_DRAW draw;
static T_GPUOBJ_VERTEX vertices[512];
static S32 active = TRUE;
static U32 allocated;
static int failures;

extern "C" S32 GpuObj_Active(void) { return active; }
extern "C" T_GPUOBJ_DRAW *GpuObj_BeginDraw(void) {
    std::memset(&draw, 0, sizeof(draw));
    return &draw;
}
extern "C" T_GPUOBJ_VERTEX *GpuObj_AllocVerts(U32 count) {
    allocated = count;
    std::memset(vertices, 0, sizeof(vertices));
    return vertices;
}
static U32 indices[2048];
static U32 indexed;
extern "C" U32 *GpuObj_AllocIndices(U32 count) {
    indexed = count;
    return count <= (U32)(sizeof indices / sizeof indices[0]) ? indices : NULL;
}
extern "C" void GpuObj_EndDraw(void) {}
static const S16 *heightMap;
extern "C" S32 GpuObj_HeightMap(S32, S32, S32, const S16 *heights, S32 *fresh) {
    heightMap = heights;
    if (fresh != NULL) {
        *fresh = FALSE;
    }
    return 7;
}
extern "C" {
U32 GpuObjLandRecords[GPUOBJ_MAX_HEIGHTMAPS * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS];
S32 GpuObjLandCount[GPUOBJ_MAX_HEIGHTMAPS];
void GpuObj_CubeSlotChanged(S32) {}
}
extern "C" void AffGpu_ViewVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth) {
    std::memset(v, 0, sizeof(*v));
    v->vpos[0] = x;
    v->vpos[1] = y;
    v->vpos[2] = -depth;
}

/* GPUTERRAIN.vert's rise of a point toward the curve, over heightMap. */
static float GridH(int x, int z) {
    x = x < 0 ? 0 : (x > 64 ? 64 : x);
    z = z < 0 ? 0 : (z > 64 ? 64 : z);
    return (float)heightMap[z * 65 + x];
}
static float Catmull(float p0, float p1, float p2, float p3, float t) {
    return p1 + 0.5f * t * (p2 - p0 + t * (2.0f * p0 - 5.0f * p1 + 4.0f * p2 - p3 + t * (3.0f * (p1 - p2) + p3 - p0)));
}
static float GrowRise(float gx, float gz, float plane, float cornerWeight, float depth) {
    const int ix = (int)gx, iz = (int)gz;
    const float u = gx - (float)ix, t = gz - (float)iz;
    float row[4];
    for (int k = 0; k < 4; k++) {
        const int z = iz - 1 + k;
        row[k] = Catmull(GridH(ix - 1, z), GridH(ix, z), GridH(ix + 1, z), GridH(ix + 2, z), u);
    }
    float w = (26000.0f - depth) / 8000.0f;
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    float shore = (plane - 64.0f) / 320.0f;
    shore = shore < 0.0f ? 0.0f : (shore > 1.0f ? 1.0f : shore);
    float border = std::fmin(std::fmin(gx, gz), std::fmin(64.0f - gx, 64.0f - gz));
    border = border > 1.0f ? 1.0f : border;
    float offset = Catmull(row[0], row[1], row[2], row[3], t) - plane;
    offset = offset < 0.0f ? 0.0f : (offset > 220.0f ? 220.0f : offset);
    return offset * w * shore * border * cornerWeight;
}

/* GPUTERRAIN.vert's GrassHash. */
static float BladeHash(U32 a, U32 b, U32 c) {
    U32 h = a * 0x8DA6B343u ^ b * 0xD8163841u ^ c * 0xCB1AB31Fu;
    h ^= h >> 13;
    h *= 0x5BD1E995u;
    h ^= h >> 15;
    return (float)(h & 0xFFFFFF) / 16777216.0f;
}

static void Check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

/* What GPUOBJ.vert does to a sea vertex marked GPUOBJ_FLAG_WAVES: the swell at
   its world phase, raised along the world's up in the space it was captured in
   (the capture rotation's rows are MatriceWorld's), the normal turned the same
   way, the height kept in normal.w for the contact pass. */
static void Raise(const T_GPUOBJ_VERTEX *v, float vpos[3], float normal[4]) {
    float n[3];
    float h;
    GpuWater_SampleSurface(v->uv[2] * 512.0f, v->uv[3] * 512.0f, &h, n);
    const TYPE_MAT *m = &MatriceWorld;
    const float up[3] = {m->F.M12, m->F.M22, m->F.M32};
    for (S32 axis = 0; axis < 3; axis++) {
        vpos[axis] = v->vpos[axis] + h * up[axis];
    }
    normal[0] = n[0] * m->F.M11 + n[1] * m->F.M12 + n[2] * m->F.M13;
    normal[1] = n[0] * m->F.M21 + n[1] * m->F.M22 + n[2] * m->F.M23;
    normal[2] = n[0] * m->F.M31 + n[1] * m->F.M32 + n[2] * m->F.M33;
    normal[3] = h;
}

static float RaisedY(const T_GPUOBJ_VERTEX *v) {
    float p[3], n[4];
    Raise(v, p, n);
    return p[1];
}

/* What GPUTERRAIN.vert's Sea() grows a sea record into: the tile's four
   corners cut into 8 x 8 cells, bilinear in the corners (PlanePoint's
   weights), in the order the CPU once emitted them. */
static T_GPUOBJ_VERTEX grown[GPUOBJ_PROC_SEA_VERTS];
static U8 planePage[65536];

static void GrowSea(const T_GPUOBJ_VERTEX corner[4]) {
    static const S32 stepX[6] = {0, 0, 1, 0, 1, 1};
    static const S32 stepZ[6] = {0, 1, 1, 0, 1, 0};
    for (S32 i = 0; i < GPUOBJ_PROC_SEA_VERTS; i++) {
        const S32 cell = i / 6, k = i % 6;
        const float u = (float)(cell % GPUOBJ_PROC_SEA_SUB + stepX[k]) / (float)GPUOBJ_PROC_SEA_SUB;
        const float t = (float)(cell / GPUOBJ_PROC_SEA_SUB + stepZ[k]) / (float)GPUOBJ_PROC_SEA_SUB;
        const float w[4] = {(1.0f - u) * (1.0f - t), (1.0f - u) * t, u * t, u * (1.0f - t)};
        T_GPUOBJ_VERTEX *v = &grown[i];
        *v = corner[0];
        for (S32 axis = 0; axis < 3; axis++) {
            v->vpos[axis] = w[0] * corner[0].vpos[axis] + w[1] * corner[1].vpos[axis] + w[2] * corner[2].vpos[axis] +
                            w[3] * corner[3].vpos[axis];
        }
        for (S32 axis = 0; axis < 4; axis++) {
            v->uv[axis] = w[0] * corner[0].uv[axis] + w[1] * corner[1].uv[axis] + w[2] * corner[2].uv[axis] +
                          w[3] * corner[3].uv[axis];
        }
    }
}

static void Plane(S32 sea, const STRUC_CLIPVERTEX quad[4]) {
    allocated = 0;
    TerrainGpu_BeginPlane(planePage, sea ? 0 : 128, 0x7F7F, 100, 1000, 0, sea);
    TerrainGpu_Quad(quad);
    TerrainGpu_End();
}

int main() {
    std::memset(&MatriceWorld, 0, sizeof(MatriceWorld));
    MatriceWorld.F.M11 = MatriceWorld.F.M22 = MatriceWorld.F.M33 = 1.0f;
    STRUC_CLIPVERTEX quad[4] = {};
    quad[0].V_X0 = 512;
    quad[0].V_Z0 = 1024;
    quad[0].V_MapU = 256;
    quad[0].V_MapV = 512;
    quad[1] = quad[0];
    quad[2] = quad[0];
    quad[3] = quad[0];
    quad[1].V_Z0 += 8192;
    quad[2].V_X0 += 8192;
    quad[2].V_Z0 += 8192;
    quad[3].V_X0 += 8192;

    TerrainGpu_SetWaterScene(0, 8, 9);
    GpuWater_SetTime(1000);
    Plane(TRUE, quad);
    Check(GpuWater_GetSeaPage() == planePage, "sea capture publishes its authored texture page");
    Check(allocated == 4 && draw.Procedural == GPUOBJ_PROC_SEA, "sea emits its tile's corners for the shader to cut");
    GrowSea(vertices);
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) != 0, "Citadel sea is marked");
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_TERRAIN) == 0, "sea vertices are not terrain");
    Check(vertices[0].uv[2] == 513.0f && vertices[0].uv[3] == 574.0f, "island-space phase");
    Check(vertices[0].uv[0] == 1.0f && vertices[0].uv[1] == 2.0f, "original UVs retained");
    Check(vertices[0].light[3] == GPUOBJ_MODE_TEX, "toggle off can use original texture mode");
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WAVES) != 0 && vertices[0].vpos[1] == (float)quad[0].V_Y0 &&
              vertices[0].normal[3] == 0.0f,
          "the mesh leaves the CPU flat, for the vertex shader to raise");
    const float worldX = vertices[0].uv[2], worldZ = vertices[0].uv[3];
    const float crestY = RaisedY(&vertices[0]);
    S32 varyingHeights = FALSE;
    for (U32 i = 1; i < GPUOBJ_PROC_SEA_VERTS; i++) {
        if (RaisedY(&grown[i]) != crestY) {
            varyingHeights = TRUE;
            break;
        }
    }
    Check(varyingHeights, "one sea mesh carries varying vertex heights");
    STRUC_CLIPVERTEX seamQuad[4];
    std::memcpy(seamQuad, quad, sizeof(seamQuad));
    for (S32 i = 0; i < 4; i++) {
        seamQuad[i].V_X0 -= 32768;
    }
    TerrainGpu_SetWaterScene(0, 9, 9);
    Plane(TRUE, seamQuad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ,
          "adjacent sea cubes share the island-global wave phase");
    TerrainGpu_SetWaterScene(0, 8, 9);
    GpuWater_SetTime(2000);
    Plane(TRUE, quad);
    {
        float raised[3], normal[4];
        Raise(&vertices[0], raised, normal);
        Check(raised[1] != crestY && normal[1] != 1.0f, "analytic waves displace mesh vertices and normals");
        Check(std::fabs(raised[1] - normal[3] - (float)quad[0].V_Y0) < 0.01f,
              "contact height keeps the undisplaced sea datum while depth stays raised");
    }
    const float translatedHeight = RaisedY(&vertices[0]);

    /* Test the production mesh against its original plane under two camera
       rotations. The contact attachment projects onto the rotated world up. */
    {
        const TYPE_MAT saved = MatriceWorld;
        STRUC_CLIPVERTEX tiltedQuad[4];
        std::memcpy(tiltedQuad, quad, sizeof(tiltedQuad));
        for (S32 i = 0; i < 4; i++) {
            tiltedQuad[i].V_Y0 = 125;
        }
        const float c = 0.70710678f;
        for (S32 rotation = 0; rotation < 2; rotation++) {
            std::memset(&MatriceWorld, 0, sizeof(MatriceWorld));
            if (rotation == 0) {
                MatriceWorld.F.M11 = 1.0f;
                MatriceWorld.F.M22 = MatriceWorld.F.M33 = c;
                MatriceWorld.F.M23 = -c;
                MatriceWorld.F.M32 = c;
            } else {
                MatriceWorld.F.M11 = MatriceWorld.F.M22 = c;
                MatriceWorld.F.M12 = -c;
                MatriceWorld.F.M21 = c;
                MatriceWorld.F.M33 = 1.0f;
            }
            TerrainGpu_SetWaterScene(0, 8, 9);
            Plane(TRUE, tiltedQuad);
            bool datum = allocated == 4;
            GrowSea(vertices);
            bool raised = false, lowered = false;
            const S32 cornerX[6] = {0, 0, 1, 0, 1, 1};
            const S32 cornerZ[6] = {0, 1, 1, 0, 1, 0};
            const float up[3] = {MatriceWorld.F.M12, MatriceWorld.F.M22, MatriceWorld.F.M32};
            for (S32 zi = 0; zi < 8; zi++) {
                for (S32 xi = 0; xi < 8; xi++) {
                    for (S32 k = 0; k < 6; k++) {
                        const T_GPUOBJ_VERTEX *flat = &grown[(zi * 8 + xi) * 6 + k];
                        T_GPUOBJ_VERTEX shaded = *flat;
                        Raise(flat, shaded.vpos, shaded.normal);
                        const T_GPUOBJ_VERTEX *v = &shaded;
                        const float original[3] = {
                            512.0f + 1024.0f * (float)(xi + cornerX[k]),
                            125.0f,
                            -1024.0f - 1024.0f * (float)(zi + cornerZ[k])};
                        const float originalHeight = original[0] * up[0] + original[1] * up[1] + original[2] * up[2];
                        const float displacedHeight = v->vpos[0] * up[0] + v->vpos[1] * up[1] + v->vpos[2] * up[2];
                        const float height = v->normal[3];
                        datum = datum && std::fabs(displacedHeight - height - originalHeight) < 0.1f;
                        for (S32 axis = 0; axis < 3; axis++) {
                            datum = datum && std::fabs(v->vpos[axis] - original[axis] - height * up[axis]) < 0.1f;
                        }
                        float sampledNormal[3];
                        GpuWater_SampleSurface(v->uv[2] * 512.0f, v->uv[3] * 512.0f, NULL, sampledNormal);
                        const float expectedNormal[3] = {
                            sampledNormal[0] * MatriceWorld.F.M11 + sampledNormal[1] * MatriceWorld.F.M12 +
                                sampledNormal[2] * MatriceWorld.F.M13,
                            sampledNormal[0] * MatriceWorld.F.M21 + sampledNormal[1] * MatriceWorld.F.M22 +
                                sampledNormal[2] * MatriceWorld.F.M23,
                            sampledNormal[0] * MatriceWorld.F.M31 + sampledNormal[1] * MatriceWorld.F.M32 +
                                sampledNormal[2] * MatriceWorld.F.M33};
                        for (S32 axis = 0; axis < 3; axis++) {
                            datum = datum && std::fabs(v->normal[axis] - expectedNormal[axis]) < 0.001f;
                        }
                        raised = raised || height > 1.0f;
                        lowered = lowered || height < -1.0f;
                    }
                }
            }
            Check(datum, "rotated sea mesh preserves the original world-up contact datum");
            Check(raised && lowered, "rotated sea mesh carries positive and negative displacement");
        }
        MatriceWorld = saved;
    }

    /* The same point from a translated camera has exactly the same phase. */
    CameraX = 512;
    CameraZ = -512;
    quad[0].V_X0 = 0;
    quad[0].V_Z0 = 512;
    TerrainGpu_SetWaterScene(0, 8, 9);
    Plane(TRUE, quad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ, "camera translation does not slide waves");
    Check(RaisedY(&vertices[0]) == translatedHeight, "camera translation keeps geometric wave height continuous");

    /* Horizon cameras shift after the reference is captured. */
    CameraX -= 32767;
    Plane(TRUE, quad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ, "horizon draw uses main camera reference");

    CameraX = CameraZ = 0;
    MatriceWorld.F.M11 = MatriceWorld.F.M33 = 0.0f;
    MatriceWorld.F.M13 = 1.0f;
    MatriceWorld.F.M31 = -1.0f;
    quad[0].V_X0 = -1024;
    quad[0].V_Z0 = 512;
    TerrainGpu_SetWaterScene(0, 8, 9);
    Plane(TRUE, quad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ, "camera rotation does not rotate waves");
    Plane(FALSE, quad);
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) == 0, "Citadel sky is not water");
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_SKY) != 0, "sky carries the horizon exclusion marker");
    for (S32 island = 1; island < 16; island++) {
        TerrainGpu_SetWaterScene(island, 8, 9);
        Plane(TRUE, quad);
        Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) != 0, "every island sea uses modern water");
    }
    active = FALSE;
    Plane(TRUE, quad);
    Check(allocated == 0, "classic renderer does not capture water");
    active = TRUE;
    TypeProj = TYPE_ISO;
    Plane(TRUE, quad);
    Check(allocated == 0, "interior projection does not capture water");

    TypeProj = TYPE_3D;
    active = TRUE;
    std::memset(&MatriceWorld, 0, sizeof(MatriceWorld));
    MatriceWorld.F.M11 = MatriceWorld.F.M22 = MatriceWorld.F.M33 = 1.0f;
    GpuWaterEnabled = FALSE;
    Plane(TRUE, quad);
    Check(allocated == 6 && vertices[0].vpos[1] == (float)quad[0].V_Y0,
          "disabled water keeps the original flat two-triangle sea");
    GpuWaterEnabled = TRUE;
    static U8 sentinelSeaPage[65536];
    GpuWater_SetSeaPage(sentinelSeaPage);
    TerrainGpu_SetLavaSea(TRUE);
    Plane(TRUE, quad);
    Check(allocated == 4 && draw.Procedural == GPUOBJ_PROC_SEA &&
              ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_LAVA) != 0 &&
              ((S32)vertices[0].vpos[3] & (GPUOBJ_FLAG_WATER | GPUOBJ_FLAG_WAVES)) == 0 &&
              GpuWater_GetSeaPage() == sentinelSeaPage,
          "lava broad plane uses its own animated mesh and keeps the water material/page path separate");
    GpuObjLava = FALSE;
    Plane(TRUE, quad);
    Check(allocated == 6 && draw.Procedural != GPUOBJ_PROC_SEA &&
              ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_LAVA) != 0,
          "disabled modern lava keeps the original flat two-triangle plane");
    GpuObjLava = TRUE;
    TerrainGpu_SetLavaSea(FALSE);
    GpuWater_SetSeaPage(planePage);
    float calmHeight, stormHeight;
    float surfaceNormal[3];
    GpuWater_SetWeather(FALSE);
    GpuWater_SampleSurface(1234.0f, 5432.0f, &calmHeight, surfaceNormal);
    GpuWater_SetWeather(TRUE);
    GpuWater_SampleSurface(1234.0f, 5432.0f, &stormHeight, NULL);
    Check(calmHeight != stormHeight && surfaceNormal[1] > 0.9f && surfaceNormal[1] <= 1.0f,
          "storm changes the sampled geometric swell without invalid normals");
    GpuWater_SetWeather(FALSE);
    /* Material checks on flat triangles: smooth terrain is checked below. */
    TerrainGpuSmooth = FALSE;
    S16 heights[65 * 65] = {};
    heights[65] = 125;
    heights[66] = 250;
    const U8 corners[3] = {0, 1, 2};
    const S32 lights[3] = {0, 0, 0};
    const U16 terrainUv[6] = {0, 0, 256, 0, 256, 256};
    static U8 terrainPage[65536];
    TerrainGpu_BeginCube(heights, terrainPage, NULL, 0, 0, 0);
    allocated = 0;
    TerrainGpu_Tri(0, 0, corners, lights, POLY_TEXTURE, 0, terrainUv, TRUE, 0);
    TerrainGpu_End();
    Check(allocated == 3 && ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER_TERRAIN) != 0 &&
              ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_TERRAIN) == 0 &&
              vertices[0].vpos[1] == 0.0f && vertices[0].uv[2] != 0.0f &&
              vertices[0].mat[3] == 0.0f && vertices[1].mat[3] == 125.0f &&
              vertices[2].mat[3] == 250.0f,
          "authored CodeJeu water keeps its animated shoreline geometry and texture");
    TerrainGpu_BeginCube(heights, terrainPage, NULL, 0, 0, 0);
    allocated = 0;
    TerrainGpu_Tri(0, 0, corners, lights, POLY_TEXTURE, 0, terrainUv, FALSE, 0);
    TerrainGpu_End();
    Check(allocated == 3 && ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) == 0 &&
              ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_TERRAIN) != 0,
          "ordinary non-water terrain carries the terrain marker");
    for (S32 lavaCode = 9; lavaCode <= 13; lavaCode += 4) {
        TerrainGpu_BeginCube(heights, terrainPage, NULL, 0, 0, 0);
        allocated = 0;
        TerrainGpu_Tri(0, 0, corners, lights, POLY_TEXTURE, 0, terrainUv, lavaCode, 0);
        TerrainGpu_End();
        Check(allocated == 3 && ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_LAVA) != 0 &&
                  ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) == 0 &&
                  ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_TERRAIN) != 0 && vertices[0].uv[2] >= 0.0f &&
                  vertices[0].uv[3] >= 0.0f,
              "textured CodeJeu 9/13 lava carries a world-stable material phase");
    }

    /* Smooth terrain: a land triangle becomes sixteen on the curve, never below
       its plane, flagged for the shadow rays, the first carrying the original. */
    {
        const TYPE_MAT saved = MatriceWorld;
        std::memset(&MatriceWorld, 0, sizeof(MatriceWorld));
        MatriceWorld.F.M11 = MatriceWorld.F.M22 = MatriceWorld.F.M33 = 1.0f;
        static S16 hills[65 * 65];
        for (int i = 0; i < 65 * 65; i++) {
            hills[i] = 1000;
        }
        hills[4 * 65 + 4] = 2000;
        hills[5 * 65 + 5] = 2600;
        TerrainGpuSmooth = TRUE;
        TerrainGpu_BeginCube(hills, terrainPage, NULL, 0, 0, 0);
        allocated = 0;
        TerrainGpu_Tri(3, 3, corners, lights, POLY_TEXTURE, 0, terrainUv, FALSE, 0);
        Check(allocated == 0, "land triangles wait for the cube's end");
        TerrainGpu_End();
        /* A record of the three flat corners, for GPUTERRAIN.vert to grow. */
        const float gridX[3] = {3.0f, 3.0f, 4.0f}, gridZ[3] = {3.0f, 4.0f, 4.0f};
        bool record = allocated == 3 && draw.Procedural && heightMap == hills;
        for (int k = 0; k < 3; k++) {
            record = record && vertices[k].uv[2] == gridX[k] && vertices[k].uv[3] == gridZ[k] &&
                     vertices[k].normal[0] == 1.0f && vertices[k].normal[1] == 7.0f &&
                     ((S32)vertices[k].vpos[3] & GPUOBJ_FLAG_DETAIL) == 0 &&
                     ((S32)vertices[k].vpos[3] & GPUOBJ_FLAG_TERRAIN) != 0;
        }
        Check(record, "smooth terrain leaves the CPU as a record of its three flat corners");
        /* What GPUTERRAIN.vert makes of it, every point of the four-way
           subdivision: above the plane, and above it somewhere. */
        bool above = true, raised = false;
        for (int i = 0; i <= 4; i++) {
            for (int j = 0; i + j <= 4; j++) {
                const float b[3] = {i / 4.0f, j / 4.0f, 1.0f - i / 4.0f - j / 4.0f};
                float p[3] = {0.0f, 0.0f, 0.0f}, gx = 0.0f, gz = 0.0f, plane = 0.0f;
                for (int k = 0; k < 3; k++) {
                    for (int a = 0; a < 3; a++) {
                        p[a] += b[k] * vertices[k].vpos[a];
                    }
                    gx += b[k] * vertices[k].uv[2];
                    gz += b[k] * vertices[k].uv[3];
                    plane += b[k] * (float)heightMap[(int)vertices[k].uv[3] * 65 + (int)vertices[k].uv[2]];
                }
                const float y = p[1] + GrowRise(gx, gz, plane, 1.0f, -p[2]);
                above = above && y >= plane - 0.5f;
                raised = raised || y > plane + 1.0f;
            }
        }
        Check(above && raised, "smooth terrain curves above the original plane, never below");
        TerrainGpuSmooth = FALSE;
        MatriceWorld = saved;
    }

    /* Grass: tufts of blades on ground marked as grass (footstep 2) whose
       texel is green; none on earth. */
    {
        const TYPE_MAT saved = MatriceWorld;
        const S32 savedZ = CameraZr;
        std::memset(&MatriceWorld, 0, sizeof(MatriceWorld));
        MatriceWorld.F.M11 = MatriceWorld.F.M22 = MatriceWorld.F.M33 = 1.0f;
        CameraZr = 20000;
        static U8 palette[768];
        palette[5 * 3] = 40, palette[5 * 3 + 1] = 120, palette[5 * 3 + 2] = 30; /* grass */
        palette[6 * 3] = 110, palette[6 * 3 + 1] = 80, palette[6 * 3 + 2] = 50; /* earth */
        TerrainGpuPalette = palette;
        static U8 grassPage[65536];
        static S16 flatGround[65 * 65];
        for (U8 texel = 5; texel <= 6; texel++) {
            std::memset(grassPage, texel, sizeof(grassPage));
            TerrainGpu_BeginCube(flatGround, grassPage, NULL, 0, 0, 0);
            TerrainGpu_Tri(3, 3, corners, lights, POLY_TEXTURE, 0, terrainUv, FALSE, 2);
            allocated = 0;
            TerrainGpu_End();
            if (texel == 5) {
                const S32 flags = (S32)vertices[0].vpos[3];
                /* A tuft leaves the CPU as its root; GPUTERRAIN.vert grows it. */
                const T_GPUOBJ_VERTEX *tuft = &vertices[0];
                const U32 seed = (U32)tuft->normal[1], cell = (U32)tuft->normal[2];
                Check(allocated == 1 && draw.Procedural == GPUOBJ_PROC_GRASS && (flags & GPUOBJ_FLAG_GRASS) != 0 &&
                          (seed & 15u) < 12u && cell == 3u + 256u * 3u && tuft->vpos[1] == 0.0f,
                      "grass ground grows tufts, each a root on the ground for the shader");
                /* The shader's tallest blade of the tuft, from the same hash. */
                float tallest = 0.0f;
                for (U32 j = 0; j < 3; j++) {
                    const float h0 = BladeHash((seed & 15u) * 3u + j, 3u * 31u + (seed >> 4), 3u * 17u);
                    tallest = std::fmax(tallest, (55.0f + 60.0f * h0) * tuft->normal[0]);
                }
                Check(tallest > 40.0f, "a blade stands up from the ground");
            } else {
                Check(allocated == 0, "no grass grows on earth, even where the island marks grass");
            }
        }
        TerrainGpuPalette = NULL;
        CameraZr = savedZ;
        MatriceWorld = saved;
    }
    quad[0].V_Z0 = -100;
    quad[1].V_Z0 = 100;
    quad[2].V_Z0 = 100;
    quad[3].V_Z0 = -100;
    Plane(TRUE, quad);
    GrowSea(vertices);
    Check(allocated == 4 && grown[0].vpos[2] > 0.0f && grown[337].vpos[2] < 0.0f,
          "near-plane crossing is retained for GPU clipping after displacement");

    float first[GPUWATER_UNIFORM_FLOATS], next[GPUWATER_UNIFORM_FLOATS];
    GpuWater_SetTime(123000);
    GpuWater_GetUniforms(first);
    GpuWater_GetUniforms(next);
    Check(std::memcmp(first, next, sizeof(first)) == 0, "presenting without a scene tick freezes waves");
    Check(first[0] == 123.0f && first[1] == 1.0f, "game time and default enable reach shader");
    GpuWater_SetWeather(TRUE);
    GpuWater_GetUniforms(next);
    Check(next[2] == 1.0f && GpuWater_GetWeather() == 1.0f, "rain reaches the water material");
    GpuWater_SetWeather(FALSE);
    GpuWater_SetTime(379000);
    GpuWater_GetUniforms(next);
    Check(first[0] == next[0], "wave time has a reproducible 256-second period");
    GpuWaterEnabled = FALSE;
    GpuWater_GetUniforms(next);
    Check(next[1] == 0.0f, "toggle applies without recapturing geometry");
    Check(next[4] == 0.0f && next[6] == -1.0f && next[12] == 1.0f, "reflection basis follows view rotation");

    GpuWaterEnabled = TRUE;
    GpuWater_SetScene(4);
    GpuWater_SetTime(500);
    GpuWater_AddImpact(1024.0f, -50.0f, 2048.0f, 255500, 1.2f);
    GpuWater_GetUniforms(next);
    Check(next[16] == 2.0f && next[17] == 4.0f, "impact reaches world-space ripple uniforms");
    Check(next[18] == 255.5f && next[19] == 1.2f, "impact time wraps with the wave clock");
    float impact[5];
    Check(GpuWater_GetImpactWorld(0, impact) && impact[0] == 1024.0f && impact[2] == 2048.0f,
          "active impact remains available for projection");
    GpuWater_SetImpactScreen(0, 0.25f, 0.75f, TRUE);
    float screen[GPUWATER_MAX_IMPACTS * 4];
    GpuWater_GetCompositeImpacts(screen);
    Check(screen[0] == 0.25f && screen[1] == 0.75f && screen[3] == 1.2f,
          "projected impact reaches spray compositor");
    GpuWater_SetScene(5);
    GpuWater_GetUniforms(next);
    Check(next[19] == 0.0f, "impacts do not leak between islands");
    GpuWater_SetScene(4);
    GpuWater_SetTime(5000);
    Check(!GpuWater_GetImpactWorld(0, impact), "impact expires after its ripple and spray");
    /* One authored half-cell defines a continuous envelope in island grid
       coordinates, including points on the edge between two cube slots. */
    {
        static U8 shoreCells[64 * 64];
        std::memset(shoreCells, 0, sizeof(shoreCells));
        S32 side = -1;
        U32 before = 0, revision = 0;
        TerrainGpu_ResetShore();
        Check(GpuWater_GetShoreField(&side, &before) == NULL && side == 0,
              "shore reset withdraws the previous field");
        shoreCells[20 * 64 + 63] = 1;
        TerrainGpu_SetShoreCube(7, 4, shoreCells);
        TerrainGpu_FinishShore();
        const float *field = GpuWater_GetShoreField(&side, &revision);
        Check(field != NULL && side == GPUWATER_SHORE_SIDE && revision > before,
              "shore field publishes all 1025 rows and columns");
        if (field != NULL) {
            const S32 z = 4 * 64 + 20, x = 7 * 64 + 63;
            Check(field[(size_t)z * side + x] == 0.0f && field[(size_t)z * side + x + 5] == 1.0f,
                  "authored water has zero swell and distant sea has full swell");
            float left, edge, right, dx, dz;
            TerrainGpu_SampleShore((float)(x + 1) * 512.0f - 1.0f, (float)z * 512.0f,
                                   &left, &dx, &dz);
            TerrainGpu_SampleShore((float)(x + 1) * 512.0f, (float)z * 512.0f,
                                   &edge, &dx, &dz);
            TerrainGpu_SampleShore((float)(x + 1) * 512.0f + 1.0f, (float)z * 512.0f,
                                   &right, &dx, &dz);
            Check(std::fabs(left - edge) < 0.01f && std::fabs(right - edge) < 0.01f,
                  "shore weight is continuous across neighboring cube coordinates");
            TerrainGpu_SampleShore(((float)x + 2.5f) * 512.0f, (float)z * 512.0f,
                                   &edge, &dx, &dz);
            Check(edge > 0.0f && edge < 1.0f && dx > 0.0f && std::fabs(dz) < 0.001f,
                  "shore sample interpolates the distance gradient in world units");
        }
        TerrainGpu_ResetShore();
        Check(GpuWater_GetShoreField(&side, &revision) == NULL && side == 0,
              "next island withdraws its predecessor's coast");
        std::memset(shoreCells, 0, sizeof(shoreCells));
        shoreCells[20 * 64 + 63] = 8;
        TerrainGpu_SetLavaSea(TRUE);
        TerrainGpu_SetShoreCube(7, 4, shoreCells);
        TerrainGpu_FinishShore();
        field = GpuWater_GetShoreField(&side, &revision);
        if (field != NULL) {
            const S32 z = 4 * 64 + 20, x = 7 * 64 + 63;
            Check(field[(size_t)z * side + x] == 0.0f && field[(size_t)z * side + x + 5] == 1.0f,
                  "lava sea selects the authored lava coastline for mesh damping");
        }
        TerrainGpu_ResetShore();
    }
    /* The land the GPU grows whole: one record a fill of a half-cell, built
       once a cube, and a draw that carries the camera the cube is drawn from. */
    {
        static S16 ground[65 * 65];
        static U8 light[65 * 65];
        static T_TERRAIN_HALF halves[64 * 64 * 2];
        std::memset(halves, 0, sizeof(halves));
        std::memset(light, 15, sizeof(light));
        const S32 cell = 3 * 64 + 3;
        T_TERRAIN_HALF *colour = &halves[cell * 2], *water = &halves[cell * 2 + 1];
        colour->PolyFlag = 1;
        colour->Bank = 2;
        colour->PolyType = POLY_SOLID;
        water->TexFlag = 1;
        water->TexType = POLY_TEXTURE;
        water->CodeJeu = 12;
        water->IndexTex = 1;
        static const U16 defs[12] = {0, 0, 0, 0, 0, 0, 256, 512, 768, 1024, 1280, 1536};
        GpuObjLandCount[7] = -1;
        CameraXr = 100;
        CameraYr = 200;
        CameraZr = 300;
        const bool build = TerrainGpu_LandSlot(ground) != FALSE;
        TerrainGpu_LandBuild(halves, light, defs);
        const U32 *r = GpuObjLandRecords + (size_t)7 * GPUOBJ_LAND_MAX * GPUOBJ_LAND_WORDS;
        const U32 *w = r + GPUOBJ_LAND_WORDS;
        const S32 expected = ((2 << 4) + 11 + ((3 * 3585 * 21845) >> 24)) & 0xFF;
        Check(build && GpuObjLandCount[7] == 2 && (r[0] & 0xFFFu) == (U32)cell &&
                  ((r[0] >> 17) & 15u) == (U32)GPUOBJ_MODE_SOLID && (S32)(r[1] & 0xFFu) == expected,
              "a cell's colour fill becomes a record with its bank's shaded colour");
        /* The water triangle (corners 2, 3, 0) keeps its corners flat, and the
           colour triangle (0, 1, 2) shares two of them. */
        Check((w[0] & (1u << 14)) != 0 && (w[0] & (1u << 24)) != 0 && ((w[0] >> 21) & 7u) == 7u &&
                  ((r[0] >> 21) & 7u) == 5u,
              "shoreline water is marked, flat, and flattens the corners it shares");
        Check(w[2] == (256u | 512u << 16) && w[3] == (768u | 1024u << 16) && w[4] == (1280u | 1536u << 16),
              "a texture fill carries its three texture corners");
        TerrainGpu_LandDraw(NULL, NULL, 0, 30000, 0, 50000);
        Check(draw.Procedural == GPUOBJ_PROC_LAND && draw.LandSlot == 7 && !draw.LandSmooth &&
                  draw.LandOrigin[0] == -100.0f && draw.LandOrigin[1] == -200.0f && draw.LandOrigin[2] == -300.0f &&
                  draw.LandFar == 30000.0f,
              "the land draw carries its slot and where the cube lies in view");
        water->CodeJeu = 1;
        GpuObjLandCount[7] = -1;
        TerrainGpu_LandBuild(halves, light, defs);
        Check(GpuObjLandCount[7] == 2 && (w[0] & (1u << 14)) != 0,
              "CodeJeu 1 water remains marked in indexed land records");
        water->CodeJeu = 9;
        GpuObjLandCount[7] = -1;
        TerrainGpu_LandBuild(halves, light, defs);
        Check(GpuObjLandCount[7] == 2 && (w[0] & (1u << 26)) != 0 && (w[0] & (1u << 14)) == 0 &&
                  (w[0] & (1u << 25)) == 0,
              "indexed CodeJeu 9 lava stays pinned, marked lava, and excluded from grass");
        water->CodeJeu = 13;
        GpuObjLandCount[7] = -1;
        TerrainGpu_LandBuild(halves, light, defs);
        Check(GpuObjLandCount[7] == 2 && (w[0] & (1u << 26)) != 0,
              "indexed CodeJeu 13 lava receives the same material");
        TerrainGpu_End();
        CameraXr = CameraYr = CameraZr = 0;
    }
    std::printf("GPU water: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
