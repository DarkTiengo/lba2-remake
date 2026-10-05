#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

enum {
    CITADEL_VEHICLE_ORIGINAL = -1,
    CITADEL_VEHICLE_YELLOW = 0,
    CITADEL_VEHICLE_RED = 1,
    CITADEL_VEHICLE_NEUTRAL = 5,
    CITADEL_VEHICLE_PANEL = 6
};

/** The harbour boat can be a scene actor instead of its hidden decor twin. */
static inline S32 CitadelBoat_IsVisibleActor(S32 island, S32 cube, S32 actor, S32 body) {
    return island == 0 && cube == 43 && actor == 10 && body == 165;
}

/** Project-authored painted metal over the buggy's palette-only polygons.
 *  Retain the original palette hue and shade; leave glass, tires and trim. */
static inline S32 CitadelVehicle_Material(U8 color) {
    switch (color >> 4) {
    case 6:
    case 4:
    case 3:
        return CITADEL_VEHICLE_PANEL;
    default:
        return CITADEL_VEHICLE_ORIGINAL;
    }
}

/** The harbour boat's painted hull, roof, trim and exhaust. Brown timber,
 *  glazing and authored image-mapped details stay untouched. */
static inline S32 CitadelBoat_Material(U8 color) {
    switch (color >> 4) {
    case 4:
    case 6:
    case 11:
    case 14:
        return CITADEL_VEHICLE_PANEL;
    default:
        return CITADEL_VEHICLE_ORIGINAL;
    }
}

/** Boat glazing is palette bank 10 in the cabin. The small door pane (164)
 *  sits at a lower model-space Y than the other windows. */
static inline S32 CitadelBoat_IsGlass(U8 color, S32 modelY) {
    return (color >> 4) == 10 && (modelY >= 700 || color == 164);
}
