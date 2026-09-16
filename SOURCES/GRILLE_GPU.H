#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * Interior bricks for the GPU object layer (SVGA/GPUOBJ.H). AffGrille draws the
 * room as isometric brick sprites; each drawn brick is recorded as a quad over
 * its sprite, textured from a brick atlas, whose depth the GPU derives per
 * pixel from the brick's grid cell. Bricks and actors then occlude each other
 * by depth at the GPU's resolution. Every call is a no-op unless the layer is
 * active.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** The room's brick set was (re)loaded: forget the atlas. */
void GrilleGpu_Reset(void);

/** Around the brick loop of AffGrille. */
void GrilleGpu_Begin(void);
void GrilleGpu_End(void);

/** One brick drawn by AffGraph: brick is the 0-based sprite in bank, (x, y, z)
 *  its grid cell, (xs, ys) the screen anchor AffGraph was given. */
void GrilleGpu_Brick(S32 brick, S32 x, S32 y, S32 z, S32 xs, S32 ys, const U8 *bank);

#ifdef __cplusplus
}
#endif
