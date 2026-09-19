#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * Automatic effect level for the GPU renderer: measures how long this machine
 * takes to draw an exterior scene and switches the costliest effects off, one
 * at a time, until the game holds its frame rate. It runs by itself the first
 * time the GPU renderer shows an exterior (GpuAutoTuned in lba2.cfg), and again
 * on request (Display > Auto-adjust, console `gfx_autotune`).
 * Only drawing changes: the simulation never reads these settings.
 */

extern S32 GpuAutoTuned; ///< the effects were adjusted to this machine (GpuAutoTuned)

/** Once per scene frame, after it is drawn: measures and steps the tuning. */
void GpuTune_Frame(void);

/** Starts over: every effect on, then down to what the machine holds. */
void GpuTune_Start(void);

/** TRUE while it is measuring. */
S32 GpuTune_Running(void);
