#ifndef TEXTURE_PACK_H
#define TEXTURE_PACK_H

#include <SYSTEM/ADELINE_TYPES.H>

/* Runtime half of the optional user-authored 4x world texture pack. */
void TexturePack_ApplyMode(S32 value);
void TexturePack_SetIsland(const char *islandName, U8 *ground, U8 *objects, U8 *skySea);
void TexturePack_UpdateGroundAnimationMask(void);

#endif /* TEXTURE_PACK_H */
