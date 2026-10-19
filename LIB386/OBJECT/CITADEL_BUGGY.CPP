#include <OBJECT/CITADEL_BUGGY.H>
#include <OBJECT/AFF_GPU.H>
#include <3D/PROJ.H>
#include <3D/CAMERA.H>
#include <3D/LIGHT.H>
#include <stddef.h>

typedef struct {
    float Position[3], Normal[3], Color[3];
    float Roughness, Metallic;
    S32 Group, Glass;
} T_CITADEL_BUGGY_VERTEX;

#include "citadel_buggy_data.h"

static S32 s_body = -1;

void CitadelBuggy_Select(S32 body) {
    s_body = body;
}

S32 CitadelBuggy_Available(void) {
    return sizeof(s_buggyIndices) / sizeof(s_buggyIndices[0]) > 1;
}

/* Authored wheel centres and original group pivots, in actor coordinates.
   Offset the pivot in chassis space, not wheel space, to prevent orbiting
   when the differently sized tires spin. Steering retains the original axle. */
static void WheelPivot(S32 group, float centre[3], float pivot[3]) {
    const S32 front = group == 5 || group == 8;
    const S32 left = group == 5 || group == 12;
    centre[0] = left ? 504.0f : -504.0f;
    centre[1] = 213.6f;
    centre[2] = front ? 420.0f : -332.4f;
    pivot[0] = front ? (left ? 500.0f : -500.0f) : (left ? 399.0f : -400.0f);
    pivot[1] = front ? 133.0f : 199.0f;
    pivot[2] = front ? 400.0f : -333.0f;
}

S32 CitadelBuggy_Emit(T_GPUOBJ_DRAW *draw, const T_BODY_HEADER *body,
                     const TYPE_MAT *matrices, S32 animated, const float origin[3]) {
    const U32 total = (U32)(sizeof(s_buggyIndices) / sizeof(s_buggyIndices[0]));
    if (draw == NULL || body == NULL || matrices == NULL || origin == NULL ||
        TypeProj != TYPE_3D || total < 3)
        return FALSE;
    if (animated && (!CitadelBuggy_IsBody(s_body) ||
                     body->NbGroupes != ((s_body == 22 || s_body == 23) ? 20 : 16)))
        return FALSE;
    if (!animated && body->NbGroupes != 1)
        return FALSE;
    /* The original steering-wheel group also contains Twinsen's hands. */
    const S32 keepWheel = animated && s_body != 219;
    U32 count = total;
    if (keepWheel) {
        count = 0;
        for (U32 i = 0; i < total; i++)
            if (s_buggyVertices[s_buggyIndices[i]].Group != 13)
                count++;
    }
    T_GPUOBJ_VERTEX *out = GpuObj_AllocVerts(count);
    if (out == NULL)
        return FALSE;
    const float *chassis = &matrices[animated ? 2 : 0].F.M11;
    U32 emitted = 0;
    for (U32 i = 0; i < total; i++) {
        const T_CITADEL_BUGGY_VERTEX *src = &s_buggyVertices[s_buggyIndices[i]];
        if (keepWheel && src->Group == 13)
            continue;
        float p[3] = {src->Position[0], src->Position[1], src->Position[2]};
        float offset[3] = {0.0f, 0.0f, 0.0f};
        const S32 wheel = src->Group == 5 || src->Group == 8 || src->Group == 10 || src->Group == 12;
        const float *r = chassis;
        if (animated) {
            if (wheel) {
                float centre[3], pivot[3];
                WheelPivot(src->Group, centre, pivot);
                r = &matrices[src->Group].F.M11;
                for (S32 j = 0; j < 3; j++) {
                    p[j] -= centre[j];
                    offset[j] = centre[j] - pivot[j];
                }
            } else {
                p[1] -= 199.0f;
            }
        }
        float view[3], normal[3];
        for (S32 j = 0; j < 3; j++) {
            view[j] = r[j * 3] * p[0] + r[j * 3 + 1] * p[1] + r[j * 3 + 2] * p[2] +
                      r[9 + j] + origin[j] + chassis[j * 3] * offset[0] +
                      chassis[j * 3 + 1] * offset[1] + chassis[j * 3 + 2] * offset[2];
            normal[j] = r[j * 3] * src->Normal[0] + r[j * 3 + 1] * src->Normal[1] + r[j * 3 + 2] * src->Normal[2];
        }
        T_GPUOBJ_VERTEX *v = &out[emitted++];
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

S32 CitadelBuggy_ReplacesEntity(const T_BODY_HEADER *body, U32 type, const U16 *poly) {
    if (body == NULL || poly == NULL || !CitadelBuggy_IsBody(s_body))
        return FALSE;
    const T_OBJ_POINT *points = (const T_OBJ_POINT *)((const U8 *)body + body->OffPoints);
    const S32 first = type >= 96 ? 2 : 0;
    const S32 count = type == 96 ? 2 : type >= 97 ? 1 : (type & 1) ? 4 : 3;
    for (S32 i = 0; i < count; i++) {
        const U16 index = poly[first + i];
        if (index >= body->NbPoints)
            return FALSE;
        const S32 group = points[index].Group;
        if (group < 2 || group > (s_body == 219 ? 13 : 12))
            return FALSE;
    }
    return TRUE;
}
