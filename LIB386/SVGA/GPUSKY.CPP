#include <SVGA/GPUSKY.H>

#include <SVGA/CLIP.H>
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

void GpuSky_SetCamera(S32 heightAboveClouds, S32 x, S32 z, S32 kind) {
    s_camera[0] = (float)heightAboveClouds;
    s_camera[1] = (float)x;
    s_camera[2] = (float)z;
    s_camera[3] = (float)kind;
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
static float s_planet[4] = {0.0f, 0.2f, -1.0f, 0.26f};

void GpuSky_SetCube(S32 cube) {
    s_cube = cube;
}

void GpuSky_GetPlanet(float *out4) {
    if (s_planetCube != s_cube) {
        /* Where the view shows it: high in the scene's window, right of its
           middle, turned into the world's frame, and kept above the horizon. */
        const float sx = (float)ClipWindowXMin + 0.7f * (float)(ClipWindowXMax - ClipWindowXMin);
        /* Close to the moon: a large disc, its top just inside the window. */
        const float sy = (float)ClipWindowYMin + s_planet[3] * 0.75f * GpuObjProj[2] * fabsf(GpuObjProj[3]);
        if (GpuObjProj[2] > 0.0f) {
            float v[3] = {(sx - GpuObjProj[0]) / GpuObjProj[2], (sy - GpuObjProj[1]) / (GpuObjProj[2] * GpuObjProj[3]),
                          -1.0f};
            const float lv = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            float w[3];
            w[0] = (v[0] * GpuObjWorldX[0] + v[1] * GpuObjWorldX[1] + v[2] * GpuObjWorldX[2]) / lv;
            w[1] = (v[0] * GpuObjUpDir[0] + v[1] * GpuObjUpDir[1] + v[2] * GpuObjUpDir[2]) / lv;
            w[2] = (v[0] * GpuObjWorldZ[0] + v[1] * GpuObjWorldZ[1] + v[2] * GpuObjWorldZ[2]) / lv;
            if (w[1] < 0.02f) {
                w[1] = 0.02f;
            }
            const float lw = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]);
            s_planet[0] = w[0] / lw;
            s_planet[1] = w[1] / lw;
            s_planet[2] = w[2] / lw;
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
