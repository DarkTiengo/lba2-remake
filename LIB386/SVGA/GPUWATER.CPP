#include <SVGA/GPUWATER.H>

#include <string.h>

S32 GpuWaterEnabled = TRUE;
static float s_water[16] = {0.0f, 0.0f, 0.0f, 0.0f,
                            1.0f, 0.0f, 0.0f, 0.0f,
                            0.0f, 1.0f, 0.0f, 0.0f,
                            0.0f, 0.0f, 1.0f, 0.0f};

typedef struct {
    float X;
    float Y;
    float Z;
    float Start;
    float Strength;
    float ScreenX;
    float ScreenY;
    S32 Island;
    S32 Visible;
} T_GPUWATER_IMPACT;

static T_GPUWATER_IMPACT s_impacts[GPUWATER_MAX_IMPACTS];
static S32 s_sceneIsland = -1;
static S32 s_nextImpact = 0;

static float ImpactAge(const T_GPUWATER_IMPACT *impact) {
    float age = s_water[0] - impact->Start;
    if (age < 0.0f) {
        age += 256.0f;
    }
    return age;
}

static S32 ImpactActive(const T_GPUWATER_IMPACT *impact) {
    return impact->Strength > 0.0f && impact->Island == s_sceneIsland && ImpactAge(impact) < 4.0f;
}

void GpuWater_SetTime(U32 milliseconds) {
    /* The wave frequencies are integer multiples of a 256-second cycle. */
    s_water[0] = (float)(milliseconds % 256000u) / 1000.0f;
}

float GpuWater_GetTime(void) {
    return s_water[0];
}

void GpuWater_SetWeather(S32 raining) {
    s_water[2] = raining ? 1.0f : 0.0f;
}

float GpuWater_GetWeather(void) {
    return s_water[2];
}

void GpuWater_SetScene(S32 island) {
    s_sceneIsland = island;
}

void GpuWater_AddImpact(float x, float y, float z, U32 milliseconds, float strength) {
    if (!GpuWaterEnabled) {
        return;
    }
    if (strength < 0.35f) {
        strength = 0.35f;
    } else if (strength > 2.5f) {
        strength = 2.5f;
    }
    T_GPUWATER_IMPACT *impact = &s_impacts[s_nextImpact];
    impact->X = x;
    impact->Y = y;
    impact->Z = z;
    impact->Start = (float)(milliseconds % 256000u) / 1000.0f;
    impact->Strength = strength;
    impact->Island = s_sceneIsland;
    impact->Visible = FALSE;
    s_nextImpact = (s_nextImpact + 1) % GPUWATER_MAX_IMPACTS;
}

void GpuWater_SetView(const float x[3], const float y[3], const float z[3]) {
    memcpy(s_water + 4, x, 3 * sizeof(float));
    memcpy(s_water + 8, y, 3 * sizeof(float));
    memcpy(s_water + 12, z, 3 * sizeof(float));
}

S32 GpuWater_GetImpactWorld(S32 index, float out[5]) {
    if (index < 0 || index >= GPUWATER_MAX_IMPACTS || !ImpactActive(&s_impacts[index])) {
        return FALSE;
    }
    const T_GPUWATER_IMPACT *impact = &s_impacts[index];
    out[0] = impact->X;
    out[1] = impact->Y;
    out[2] = impact->Z;
    out[3] = impact->Start;
    out[4] = impact->Strength;
    return TRUE;
}

void GpuWater_SetImpactScreen(S32 index, float x, float y, S32 visible) {
    if (index < 0 || index >= GPUWATER_MAX_IMPACTS) {
        return;
    }
    s_impacts[index].ScreenX = x;
    s_impacts[index].ScreenY = y;
    s_impacts[index].Visible = visible;
}

void GpuWater_GetCompositeImpacts(float out[GPUWATER_MAX_IMPACTS * 4]) {
    memset(out, 0, GPUWATER_MAX_IMPACTS * 4 * sizeof(float));
    if (!GpuWaterEnabled) {
        return;
    }
    for (S32 i = 0; i < GPUWATER_MAX_IMPACTS; i++) {
        const T_GPUWATER_IMPACT *impact = &s_impacts[i];
        if (!ImpactActive(impact) || !impact->Visible) {
            continue;
        }
        out[i * 4 + 0] = impact->ScreenX;
        out[i * 4 + 1] = impact->ScreenY;
        out[i * 4 + 2] = impact->Start;
        out[i * 4 + 3] = impact->Strength;
    }
}

void GpuWater_GetUniforms(float out[GPUWATER_UNIFORM_FLOATS]) {
    memset(out, 0, GPUWATER_UNIFORM_FLOATS * sizeof(float));
    memcpy(out, s_water, sizeof(s_water));
    out[1] = GpuWaterEnabled ? 1.0f : 0.0f;
    for (S32 i = 0; i < GPUWATER_MAX_IMPACTS; i++) {
        const T_GPUWATER_IMPACT *impact = &s_impacts[i];
        if (!GpuWaterEnabled || !ImpactActive(impact)) {
            continue;
        }
        out[16 + i * 4 + 0] = impact->X / 512.0f;
        out[16 + i * 4 + 1] = impact->Z / 512.0f;
        out[16 + i * 4 + 2] = impact->Start;
        out[16 + i * 4 + 3] = impact->Strength;
    }
}
