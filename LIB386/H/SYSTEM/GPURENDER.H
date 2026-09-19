#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * SDL GPU presenter for the GPU object layer (SVGA/GPUOBJ.H).
 *
 * Opt-in: LBA2_GPU=1 in the environment asks for a GPU renderer at window
 * creation. Development aids: LBA2_GPU_CAPTURE=<file.bmp> keeps the last
 * composited frame on disk, LBA2_GPU_DEBUG=1 tints GPU pixels green, LBA2_GPU_STATS=1
 * logs the per-present cost. When the device, shaders or pipeline are unavailable the window
 * falls back to the default SDL renderer and the game renders as before.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** TRUE when the run asked for the GPU renderer. */
bool GpuRender_Requested(void);

/** Creates an SDL GPU renderer for the window and the object pipeline on its
 *  device. Returns the renderer, or NULL (nothing left allocated) on failure. */
void *GpuRender_CreateRenderer(void *sdlWindow);

/** Whether the live renderer was created on the power-saving card: it is
 *  recreated when GpuObjLowPower no longer matches. */
bool GpuRender_LowPower(void);

/** Releases the pipeline; call before destroying the renderer. */
void GpuRender_Shutdown(void);

/** Renders the captured bodies and, when any pixel shows one, arms the
 *  composite state on the renderer. Returns TRUE when EndComposite is owed. */
bool GpuRender_BeginComposite(void *sdlRenderer);
void GpuRender_EndComposite(void *sdlRenderer);

#ifdef __cplusplus
}
#endif
