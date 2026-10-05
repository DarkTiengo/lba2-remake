#pragma once

#include <OBJECT/AFF_OBJ.H>
#include <3D/DATAMAT.H>
#include <SVGA/GPUOBJ.H>

/* Render-only binding: retail group 5 translates from 0 to 815 while the
   boarding script opens the door. The interpolated pose owns its timing. */
static inline float CitadelBoat_DoorFraction(S32 type, S32 slide) {
    if (type != TYPE_TRANSLATE || slide <= 0)
        return 0.0f;
    return slide >= 815 ? 1.0f : (float)slide / 815.0f;
}

#ifdef __cplusplus
extern "C" {
#endif
void CitadelBoat_SetPose(const T_OBJ_3D *obj);
S32 CitadelBoat_Available(void);
S32 CitadelBoat_Emit(T_GPUOBJ_DRAW *draw, const TYPE_MAT *matrices,
                    S32 animated, const float origin[3]);
#ifdef __cplusplus
}
#endif
