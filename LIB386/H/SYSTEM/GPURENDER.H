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

/** Releases the pipeline; call before destroying the renderer. */
void GpuRender_Shutdown(void);

/** Renders the captured bodies and, when any pixel shows one, arms the
 *  composite state on the renderer. Returns TRUE when EndComposite is owed. */
/** The next composite reads the software frame itself: Log's palette indices,
 *  sent as they are and looked up in the palette by the composite, instead of
 *  a frame the CPU converted to colours and the renderer uploaded. FALSE when
 *  the GPU renderer cannot composite now; the caller converts as before. */
bool GpuRender_TakeFrame(void);

/** The frame is shown smaller than its pixels: read it filtered. */
void GpuRender_SetFrameFiltered(bool filtered);

bool GpuRender_BeginComposite(void *sdlRenderer);
void GpuRender_EndComposite(void *sdlRenderer);

#ifdef __cplusplus
}
#endif
