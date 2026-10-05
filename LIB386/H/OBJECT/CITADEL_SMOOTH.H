#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/* Smoothly shaded triangles with substantially different, but not opposing,
   corner normals can be rounded. Flat walls and hard-edged panels stay planar. */
static inline S32 CitadelSmooth_Curved(const float normals[3][3]) {
    S32 varied = FALSE;
    for (S32 a = 0; a < 3; a++) {
        for (S32 b = a + 1; b < 3; b++) {
            const float dot = normals[a][0] * normals[b][0] + normals[a][1] * normals[b][1] +
                              normals[a][2] * normals[b][2];
            if (dot < 0.5f)
                return FALSE;
            if (dot < 0.97f)
                varied = TRUE;
        }
    }
    return varied;
}

/* A Phong patch of one original triangle. Retail corners stay fixed. On an
   edge the third weight is zero, so neighbouring triangles agree there. */
static inline void CitadelSmooth_PatchPoint(const float points[3][3], const float normals[3][3],
                                            const float weights[3], float rounding, float out[3]) {
    float plane[3] = {0.0f, 0.0f, 0.0f};
    for (S32 axis = 0; axis < 3; axis++) {
        plane[axis] = weights[0] * points[0][axis] + weights[1] * points[1][axis] +
                      weights[2] * points[2][axis];
        out[axis] = plane[axis];
    }
    for (S32 corner = 0; corner < 3; corner++) {
        float distance = 0.0f;
        for (S32 axis = 0; axis < 3; axis++) {
            distance += (plane[axis] - points[corner][axis]) * normals[corner][axis];
        }
        for (S32 axis = 0; axis < 3; axis++) {
            out[axis] -= rounding * weights[corner] * distance * normals[corner][axis];
        }
    }
}
