#pragma once
#include <OBJECT/AFF_OBJ.H>
#include <3D/DATAMAT.H>
#include <SVGA/GPUOBJ.H>

/* The scooter, driver and passenger share one retail body. */
static inline S32 CitadelScooter_IsBody(S32 body) {
    return body >= 148 && body <= 150;
}
static inline S32 CitadelScooter_IsVehicleGroup(S32 group) {
    return group == 2 || group == 20;
}

void CitadelScooter_Select(S32 body);
S32 CitadelScooter_Emit(T_GPUOBJ_DRAW *draw, const T_BODY_HEADER *body,
                       const TYPE_MAT *matrices, const float origin[3]);
S32 CitadelScooter_ReplacesEntity(const T_BODY_HEADER *body, U32 type, const U16 *poly);
