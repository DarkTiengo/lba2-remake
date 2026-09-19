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
S32 GpuObjEnabled = TRUE, GpuObjAvailable = TRUE;
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
extern "C" void GpuObj_EndDraw(void) {}
extern "C" void AffGpu_ViewVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth) {
    std::memset(v, 0, sizeof(*v));
    v->vpos[0] = x;
    v->vpos[1] = y;
    v->vpos[2] = -depth;
}

static void Check(bool ok, const char *what) {
    if (!ok) {
        std::printf("FAIL: %s\n", what);
        failures++;
    }
}

static void Plane(S32 sea, const STRUC_CLIPVERTEX quad[4]) {
    static U8 page[65536];
    allocated = 0;
    TerrainGpu_BeginPlane(page, sea ? 0 : 128, 0x7F7F, 100, 1000, 0, sea);
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
    Check(allocated == 384, "sea emits a bounded subdivided mesh");
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) != 0, "Citadel sea is marked");
    Check(vertices[0].uv[2] == 513.0f && vertices[0].uv[3] == 574.0f, "island-space phase");
    Check(vertices[0].uv[0] == 1.0f && vertices[0].uv[1] == 2.0f, "original UVs retained");
    Check(vertices[0].light[3] == GPUOBJ_MODE_TEX, "toggle off can use original texture mode");
    const float worldX = vertices[0].uv[2], worldZ = vertices[0].uv[3];
    const float crestY = vertices[0].vpos[1];
    S32 varyingHeights = FALSE;
    for (U32 i = 1; i < allocated; i++) {
        if (vertices[i].vpos[1] != crestY) {
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
    Check(vertices[0].vpos[1] != crestY && vertices[0].normal[1] != 1.0f,
          "analytic waves displace mesh vertices and normals");
    const float translatedHeight = vertices[0].vpos[1];

    /* The same point from a translated camera has exactly the same phase. */
    CameraX = 512;
    CameraZ = -512;
    quad[0].V_X0 = 0;
    quad[0].V_Z0 = 512;
    TerrainGpu_SetWaterScene(0, 8, 9);
    Plane(TRUE, quad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ, "camera translation does not slide waves");
    Check(vertices[0].vpos[1] == translatedHeight, "camera translation keeps geometric wave height continuous");

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
    const U8 corners[3] = {0, 1, 2};
    const S32 lights[3] = {0, 0, 0};
    const U16 terrainUv[6] = {0, 0, 256, 0, 256, 256};
    static U8 terrainPage[65536];
    TerrainGpu_BeginCube(heights, terrainPage, NULL, 0, 0, 0);
    allocated = 0;
    TerrainGpu_Tri(0, 0, corners, lights, POLY_TEXTURE, 0, terrainUv, TRUE, 0);
    TerrainGpu_End();
    Check(allocated == 3 && ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER_TERRAIN) != 0 &&
              vertices[0].vpos[1] == 0.0f && vertices[0].uv[2] != 0.0f,
          "authored CodeJeu water keeps its animated shoreline geometry and texture");
    TerrainGpu_BeginCube(heights, terrainPage, NULL, 0, 0, 0);
    allocated = 0;
    TerrainGpu_Tri(0, 0, corners, lights, POLY_TEXTURE, 0, terrainUv, FALSE, 0);
    TerrainGpu_End();
    Check(allocated == 3 && ((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) == 0,
          "unmarked terrain such as animated lava or gas keeps its original material");

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
        bool above = true, raised = false, flagged = true;
        for (U32 i = 0; i < allocated; i++) {
            const S32 flags = (S32)vertices[i].vpos[3];
            flagged = flagged && (flags & GPUOBJ_FLAG_DETAIL) != 0 &&
                      ((flags & GPUOBJ_FLAG_DETAIL_ORIGIN) != 0) == (i < 3);
            /* The plane through corners (3,3) 1000, (3,4) 1000, (4,4) 2000. */
            const float x = vertices[i].vpos[0] / 512.0f - 3.0f;
            const float plane = 1000.0f + 1000.0f * x;
            above = above && vertices[i].vpos[1] >= plane - 0.5f;
            raised = raised || vertices[i].vpos[1] > plane + 1.0f;
        }
        Check(allocated == 48 && flagged, "smooth terrain cuts a land triangle into sixteen, flagged");
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
                Check(allocated == 9 && (flags & GPUOBJ_FLAG_GRASS) != 0, "grass ground grows tufts of three blades");
                Check(vertices[2].vpos[1] > vertices[0].vpos[1] + 40.0f, "a blade stands up from the ground");
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
    Check(allocated == 384 && vertices[0].vpos[2] > 0.0f && vertices[337].vpos[2] < 0.0f,
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
    std::printf("GPU water: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
