#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * Exterior sprites (the extras: thrown objects, sparks, collectibles) with a
 * real depth.
 *
 * The software still blits them, which is what tags their pixels and what keeps
 * the pixel-art filter on their colour. The GPU draws the same rectangle at the
 * extra's own depth, so the terrain, a building or a body can hide it — the
 * software blit alone cannot, since it paints over whatever the painter left
 * there. Classic behaviour is the sprite over everything; this is the modern
 * one, and it is what the renderer's own depth has always implied.
 *
 * The sprite's pixels reach the GPU through the atlas the interior bricks use
 * (SVGA/GPUOBJ.H), which no exterior touches.
 */

/** Sprites are depth-tested outdoors (gfx_sprites). */
extern S32 GpuObjSprites;

/** The scene changed: the atlas holds another island's sprites, or a room's
 *  bricks. Called where the brick atlas is reset. */
void SpriteGpu_Reset(void);

/** Opens the draw for one extra and snapshots its rectangle, so the software
 *  blit that follows is tagged as this draw's. FALSE when the sprite cannot be
 *  taken (not an exterior, the atlas is full, the setting is off): the caller
 *  then blits as it always did and the sprite keeps the classic behaviour.
 *  data is the sprite bank entry ScaleSprite is given. */
S32 SpriteGpu_Begin(S32 num, const void *data, S32 spriteX, S32 spriteY, S32 scale);

/** Closes the draw opened by SpriteGpu_Begin, emitting the quad at depth
 *  (the extra's view depth, from the projection that placed it). */
void SpriteGpu_End(S32 depth);
