#pragma once
#include <OBJECT/AFF_OBJ.H>
#include <3D/DATAMAT.H>
#include <SVGA/GPUOBJ.H>

static inline S32 CitadelBuggy_IsBody(S32 body) {
    return body == 14 || body == 15 || body == 22 || body == 23 || body == 219;
}

/* -1 disables actor replacement; decor selection belongs to the caller. */
void CitadelBuggy_Select(S32 body);
S32 CitadelBuggy_Available(void);
S32 CitadelBuggy_Emit(T_GPUOBJ_DRAW *draw, const T_BODY_HEADER *body,
                     const TYPE_MAT *matrices, S32 animated, const float origin[3]);
S32 CitadelBuggy_ReplacesEntity(const T_BODY_HEADER *body, U32 type, const U16 *poly);
