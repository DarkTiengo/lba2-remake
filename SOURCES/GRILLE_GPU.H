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

/** While the brick set loads: sprite `local` of the room's bank is brick
 *  `global` of BKG.HQR, the number lamp bricks are listed by. */
void GrilleGpu_MapBrick(S32 local, S32 global);

/** Around the brick loop of AffGrille. palette is the room's 768-byte RGB
 *  palette, which gives each lamp its light colour. */
void GrilleGpu_Begin(const U8 *palette);
void GrilleGpu_End(void);

/** One brick drawn by AffGraph: brick is the 0-based sprite in bank, (x, y, z)
 *  its grid cell, (xs, ys) the screen anchor AffGraph was given. */
void GrilleGpu_Brick(S32 brick, S32 x, S32 y, S32 z, S32 xs, S32 ys, const U8 *bank);

/** Adds the lamps of the last drawn room (nearest to the camera first) to the
 *  GPU scene lights. Call once per scene frame in a room. */
void GrilleGpu_AddRoomLights(void);

#ifdef __cplusplus
}
#endif
