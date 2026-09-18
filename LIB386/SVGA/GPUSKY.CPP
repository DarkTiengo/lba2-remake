#include <SVGA/GPUSKY.H>

#include <SVGA/GPUOBJ.H>

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

S32 GpuSky_CloudsBelow(void) {
    return s_cloudsBelow;
}

S32 GpuSky_FogIndex(void) {
    return GpuSky_Modern() ? s_fogIndex : -1;
}

float GpuSky_Storm(void) {
    return s_storm;
}
