#include <SVGA/GPUSKY.H>

#include <SVGA/GPUOBJ.H>

#include <math.h>

S32 GpuSkyEnabled = TRUE;

static S32 s_fogIndex = -1;
static float s_storm = 0.0f;
static S32 s_cloudsBelow = FALSE;

S32 GpuSky_Modern(void) {
    return GpuSkyEnabled && GpuObjEnabled && GpuObjAvailable;
}

void GpuSky_SetScene(S32 fogIndex, S32 exterior, float storm, S32 cloudsBelow) {
    s_fogIndex = exterior ? (fogIndex & 255) : -1;
    s_storm = storm;
    s_cloudsBelow = cloudsBelow;
}

static float s_fogStart = 0.0f;
static float s_fogEnd = 0.0f;

void GpuSky_SetFog(S32 start, S32 end) {
    s_fogStart = (float)start;
    s_fogEnd = (float)end;
}

void GpuSky_GetFog(float *start, float *end) {
    *start = s_fogStart;
    *end = s_fogEnd;
}

static float s_camera[4] = {0.0f, 0.0f, 0.0f, 0.0f};

void GpuSky_SetCamera(S32 heightAboveClouds, S32 x, S32 z, S32 space) {
    s_camera[0] = (float)heightAboveClouds;
    s_camera[1] = (float)x;
    s_camera[2] = (float)z;
    s_camera[3] = space ? 1.0f : 0.0f;
}

void GpuSky_GetCamera(float *out4) {
    for (int i = 0; i < 4; i++) {
        out4[i] = s_camera[i];
    }
}

static float s_cover = 0.42f;

void GpuSky_SetCover(float cover) {
    s_cover = cover;
}

float GpuSky_Cover(void) {
    return s_cover;
}

static S32 s_cube = -1;
static S32 s_planetCube = -2;
static float s_planet[4] = {0.0f, 0.1f, -1.0f, 0.16f};

void GpuSky_SetCube(S32 cube) {
    s_cube = cube;
}

void GpuSky_GetPlanet(float *out4) {
    if (s_planetCube != s_cube) {
        /* The view's forward in the world's frame, levelled, turned aside. */
        float fx = -GpuObjWorldX[2], fz = -GpuObjWorldZ[2];
        const float l = sqrtf(fx * fx + fz * fz);
        if (l > 1e-4f) {
            fx /= l;
            fz /= l;
            const float a = 0.3f;
            const float rx = fx * cosf(a) - fz * sinf(a);
            const float rz = fx * sinf(a) + fz * cosf(a);
            const float el = 0.05f;
            s_planet[0] = rx * cosf(el);
            s_planet[1] = sinf(el);
            s_planet[2] = rz * cosf(el);
            s_planetCube = s_cube;
        }
    }
    for (int i = 0; i < 4; i++) {
        out4[i] = s_planet[i];
    }
}

S32 GpuSky_CloudsBelow(void) {
    return s_cloudsBelow;
}

S32 GpuSky_FogIndex(void) {
    return GpuSky_Modern() ? s_fogIndex : -1;
}

float GpuSky_Storm(void) {
    return s_storm;
}
