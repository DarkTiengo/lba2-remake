#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

extern S32 GpuWaterEnabled;

#define GPUWATER_MAX_IMPACTS 8
#define GPUWATER_UNIFORM_FLOATS (16 + GPUWATER_MAX_IMPACTS * 4)

/* Scene time is supplied by the game, never advanced by menu presents. */
void GpuWater_SetTime(U32 milliseconds);
float GpuWater_GetTime(void);
/* Weather drives rougher waves and wider breakers without changing gameplay. */
void GpuWater_SetWeather(S32 raining);
float GpuWater_GetWeather(void);
/* Impacts are stored in island-global coordinates and filtered by scene. */
void GpuWater_SetScene(S32 island);
void GpuWater_AddImpact(float x, float y, float z, U32 milliseconds, float strength);
/* World axes in view space, captured with the sea geometry. */
void GpuWater_SetView(const float x[3], const float y[3], const float z[3]);
/* Returns one active impact as x/y/z/start/strength for projection. */
S32 GpuWater_GetImpactWorld(S32 index, float out[5]);
void GpuWater_SetImpactScreen(S32 index, float x, float y, S32 visible);
/* vec4 per impact: screen x/y, start time and strength. */
void GpuWater_GetCompositeImpacts(float out[GPUWATER_MAX_IMPACTS * 4]);
/* std140: time/enable/padding, axes, then impact x/z/start/strength. */
void GpuWater_GetUniforms(float out[GPUWATER_UNIFORM_FLOATS]);

#ifdef __cplusplus
}
#endif
