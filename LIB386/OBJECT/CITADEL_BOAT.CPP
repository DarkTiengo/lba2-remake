#include <OBJECT/CITADEL_BOAT.H>
#include <OBJECT/AFF_GPU.H>
#include <3D/PROJ.H>
#include <3D/CAMERA.H>
#include <3D/LIGHT.H>

#include <math.h>

typedef struct {
    float Position[3], Normal[3], Color[3];
    float Roughness, Metallic;
    S32 Door, Glass;
} T_CITADEL_BOAT_VERTEX;

#include "citadel_boat_data.h"

static float s_doorOpen = 0.0f;

void CitadelBoat_SetPose(const T_OBJ_3D *obj) {
    s_doorOpen = obj != NULL && obj->NbGroups >= 6
                     ? CitadelBoat_DoorFraction(obj->CurrentFrame[4].Type, obj->CurrentFrame[4].Gamma)
                     : 0.0f;
}

S32 CitadelBoat_Available(void) {
    return sizeof(s_boatIndices) / sizeof(s_boatIndices[0]) > 1;
}

S32 CitadelBoat_Emit(T_GPUOBJ_DRAW *draw, const TYPE_MAT *matrices,
                    S32 animated, const float origin[3]) {
    if (draw == NULL || TypeProj != TYPE_3D || !CitadelBoat_Available())
        return FALSE;
    const U32 count = (U32)(sizeof(s_boatIndices) / sizeof(s_boatIndices[0]));
    T_GPUOBJ_VERTEX *out = GpuObj_AllocVerts(count);
    if (out == NULL)
        return FALSE;
    /* Group 2 carries the hull's buoyancy. Group 0 is the actor's route. */
    const TYPE_MAT *m = &matrices[animated ? 2 : 0];
    const float *r = &m->F.M11;
    const float angle = (animated ? s_doorOpen : 0.0f) * 1.745329252f;
    const float ca = cosf(angle), sa = sinf(angle);
    /* Hinge at the forward jamb; the door swings into the cabin. */
    const float hingeX = 1078.0f, hingeZ = -346.5f;
    for (U32 i = 0; i < count; i++) {
        const T_CITADEL_BOAT_VERTEX *src = &s_boatVertices[s_boatIndices[i]];
        float p[3] = {src->Position[0], src->Position[1], src->Position[2]};
        float n[3] = {src->Normal[0], src->Normal[1], src->Normal[2]};
        if (src->Door) {
            const float x = p[0] - hingeX, z = p[2] - hingeZ;
            p[0] = hingeX + ca * x + sa * z;
            p[2] = hingeZ - sa * x + ca * z;
            const float nx = n[0];
            n[0] = ca * nx + sa * n[2];
            n[2] = -sa * nx + ca * n[2];
        }
        float view[3], normal[3];
        for (S32 j = 0; j < 3; j++) {
            view[j] = r[j * 3] * p[0] + r[j * 3 + 1] * p[1] + r[j * 3 + 2] * p[2] + r[9 + j] + origin[j];
            normal[j] = r[j * 3] * n[0] + r[j * 3 + 1] * n[1] + r[j * 3 + 2] * n[2];
        }
        T_GPUOBJ_VERTEX *v = &out[i];
        AffGpu_ViewVertex(v, view[0], view[1], -view[2]);
        for (S32 j = 0; j < 3; j++) {
            v->normal[j] = normal[j];
            v->mat[j] = src->Color[j];
        }
        v->light[0] = (float)CameraXLight;
        v->light[1] = (float)CameraYLight;
        v->light[2] = (float)CameraZLight;
        v->light[3] = GPUOBJ_MODE_RGB;
        v->normal[3] = 1.0f;
        v->uv[2] = src->Roughness;
        v->uv[3] = src->Metallic;
        v->vpos[3] = (float)(GPUOBJ_FLAG_AUTHORED_MODEL | (src->Glass ? GPUOBJ_FLAG_CITADEL_BOAT_GLASS : 0));
        if (src->Glass)
            draw->HasGlass = TRUE;
    }
    return TRUE;
}
