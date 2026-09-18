#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * The sky as the GPU renderer draws it (gfx_sky): a procedural sky over every
 * sky pixel of an exterior (the fog-coloured background and the textured
 * ceiling), a gradient from the island's fog at the horizon to a deeper
 * zenith, the sun, drifting clouds lit from the sun's side, stars at night and
 * a closed, dark cover in the storm.
 */

#ifdef __cplusplus
extern "C" {
#endif

extern S32 GpuSkyEnabled; ///< player choice (gfx_sky)

/** TRUE while the GPU renderer draws the sky. */
S32 GpuSky_Modern(void);

/** Once per scene frame: the fog colour's palette index (the horizon), whether
 *  the scene is an exterior, how stormy it is (0 clear, 1 storm), and whether
 *  the island's cloud ceiling lies below the camera (an island above the
 *  clouds: a sea of clouds under the horizon). */
void GpuSky_SetScene(S32 fogIndex, S32 exterior, float storm, S32 cloudsBelow);

/** The view depths where the island's fog starts and where it is total: what
 *  fades into the fog fades into the sky instead. */
void GpuSky_SetFog(S32 start, S32 end);
void GpuSky_GetFog(float *start, float *end);

/** TRUE when the cloud ceiling lies below the camera. */
S32 GpuSky_CloudsBelow(void);

/** The fog colour's palette index, or -1 when the sky is not drawn. */
S32 GpuSky_FogIndex(void);

/** How stormy the sky is, 0..1. */
float GpuSky_Storm(void);

#ifdef __cplusplus
}
#endif
