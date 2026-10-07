#include <OBJECT/CITADEL_SCOOTER.H>
#include <OBJECT/AFF_GPU.H>
#include <3D/PROJ.H>
#include <3D/CAMERA.H>
#include <3D/LIGHT.H>
#include <stddef.h>

typedef struct {
    float Position[3], Normal[3], Color[3];
    float Roughness, Metallic;
    S32 Steering, Glass;
} T_CITADEL_SCOOTER_VERTEX;

#include "citadel_scooter_data.h"

static S32 s_body = -1;

void CitadelScooter_Select(S32 body) {
    s_body = body;
}

S32 CitadelScooter_Emit(T_GPUOBJ_DRAW *draw, const T_BODY_HEADER *body,
                       const TYPE_MAT *matrices, const float origin[3]) {
    const U32 count = (U32)(sizeof(s_scooterIndices) / sizeof(s_scooterIndices[0]));
    if (draw == NULL || body == NULL || TypeProj != TYPE_3D || count < 3 ||
        !CitadelScooter_IsBody(s_body) ||
        body->NbGroupes != (s_body == 148 ? 21 : 30))
        return FALSE;
    T_GPUOBJ_VERTEX *out = GpuObj_AllocVerts(count);
    if (out == NULL)
        return FALSE;
    for (U32 i = 0; i < count; i++) {
        const T_CITADEL_SCOOTER_VERTEX *src = &s_scooterVertices[s_scooterIndices[i]];
        const float *r = &matrices[src->Steering ? 20 : 2].F.M11;
        float view[3], normal[3];
        for (S32 j = 0; j < 3; j++) {
            view[j] = r[j * 3] * src->Position[0] + r[j * 3 + 1] * src->Position[1] +
                      r[j * 3 + 2] * src->Position[2] + r[9 + j] + origin[j];
            normal[j] = r[j * 3] * src->Normal[0] + r[j * 3 + 1] * src->Normal[1] +
                        r[j * 3 + 2] * src->Normal[2];
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
        v->vpos[3] = GPUOBJ_FLAG_AUTHORED_MODEL;
    }
    return TRUE;
}

S32 CitadelScooter_ReplacesEntity(const T_BODY_HEADER *body, U32 type, const U16 *poly) {
    if (body == NULL || poly == NULL)
        return FALSE;
    const T_OBJ_POINT *points = (const T_OBJ_POINT *)((const U8 *)body + body->OffPoints);
    const S32 first = type >= 96 ? 2 : 0;
    const S32 count = type == 96 ? 2 : type >= 97 ? 1 : (type & 1) ? 4 : 3;
    for (S32 i = 0; i < count; i++) {
        const U16 index = poly[first + i];
        if (index >= body->NbPoints || !CitadelScooter_IsVehicleGroup(points[index].Group))
            return FALSE;
    }
    return TRUE;
}
