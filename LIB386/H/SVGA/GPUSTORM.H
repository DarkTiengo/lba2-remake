#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * The storm as the GPU renderer draws it (gfx_storm): rain streaks and ground
 * splashes, lightning bolts in the sky and the flash lighting the scene, in
 * place of the software rain lines and the lightning palette. Purely visual:
 * strikes are drawn from a generator of their own, never the game's.
 */

#ifdef __cplusplus
extern "C" {
#endif

extern S32 GpuStormEnabled; ///< player choice (gfx_storm)

/** TRUE while the GPU renderer draws the storm (the classic rain and palette
 *  flash are then skipped). */
S32 GpuStorm_Modern(void);

/** Once per scene frame: the game time in ms, whether rain falls in view, and
 *  whether the scene is an exterior (lightning bolts can be seen). */
void GpuStorm_SetScene(S32 timeMs, S32 raining, S32 exterior);

/** A scripted lightning (LM_ECLAIR) is under way this frame. */
void GpuStorm_Eclair(void);

/** A strike at the next scene frame (console `lightning`). */
void GpuStorm_Strike(void);

/** Rain falls or a flash is lit: the composite must run. */
S32 GpuStorm_Active(void);

#define GPUSTORM_UNIFORM_FLOATS 20

/** The composite's storm block, for a frame of frameW x frameH pixels:
 *  rain (on, seconds, density, flash), bolt (x and horizon in frame uv, seed,
 *  visibility), toward the flash in scene space (w exterior), up (w age), the
 *  scene's window in frame uv (the cinema bars outside it). */
void GpuStorm_GetUniforms(float *out, float frameW, float frameH);

/** Flash brightness at age seconds after a strike (0 before and after). */
float GpuStorm_FlashCurve(float age);

#ifdef __cplusplus
}
#endif
