#include "3DEXT/TERRAIN_GPU.H"
#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <OBJECT/AFF_GPU.H>
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
}

static T_GPUOBJ_DRAW draw;
static T_GPUOBJ_VERTEX vertices[6];
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

    TerrainGpu_SetWaterScene(0, 8, 9);
    Plane(TRUE, quad);
    Check(allocated == 6, "sea emits two triangles");
    Check(((S32)vertices[0].vpos[3] & GPUOBJ_FLAG_WATER) != 0, "Citadel sea is marked");
    Check(vertices[0].uv[2] == 513.0f && vertices[0].uv[3] == 574.0f, "island-space phase");
    Check(vertices[0].uv[0] == 1.0f && vertices[0].uv[1] == 2.0f, "original UVs retained");
    Check(vertices[0].light[3] == GPUOBJ_MODE_TEX, "toggle off can use original texture mode");
    const float worldX = vertices[0].uv[2], worldZ = vertices[0].uv[3];

    /* The same point from a translated camera has exactly the same phase. */
    CameraX = 512;
    CameraZ = -512;
    quad[0].V_X0 = 0;
    quad[0].V_Z0 = 512;
    TerrainGpu_SetWaterScene(0, 8, 9);
    Plane(TRUE, quad);
    Check(vertices[0].uv[2] == worldX && vertices[0].uv[3] == worldZ, "camera translation does not slide waves");

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
    Check(GpuWater_ContactHeight(-50.0f, -50.0f), "water-height predicate accepts a touching surface");
    Check(GpuWater_ContactHeight(46.0f, -50.0f), "water-height predicate accepts the contact tolerance edge");
    Check(!GpuWater_ContactHeight(47.0f, -50.0f), "water-height predicate rejects a dry projected body");
    GpuWater_SetScene(5);
    GpuWater_GetUniforms(next);
    Check(next[19] == 0.0f, "impacts do not leak between islands");
    GpuWater_SetScene(4);
    GpuWater_SetTime(5000);
    Check(!GpuWater_GetImpactWorld(0, impact), "impact expires after its ripple and spray");
    std::printf("GPU water: %s\n", failures ? "FAILED" : "passed");
    return failures ? 1 : 0;
}
