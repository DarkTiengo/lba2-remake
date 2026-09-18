#include <SVGA/GPUBVH.H>

#include <algorithm>
#include <math.h>
#include <string.h>

float GpuBvhNodes[GPUBVH_MAX_NODES * GPUBVH_NODE_FLOATS];
float GpuBvhTris[GPUBVH_MAX_TRIS * GPUBVH_TRI_FLOATS];
U32 GpuBvhNbNodes = 0;
U32 GpuBvhNbTris = 0;

/* Build scratch: each triangle's centroid and bounds, and the order leaves
   take them in. */
static float s_centre[GPUBVH_MAX_TRIS][3];
static float s_min[GPUBVH_MAX_TRIS][3];
static float s_max[GPUBVH_MAX_TRIS][3];
static U32 s_order[GPUBVH_MAX_TRIS];

#define LEAF_TRIS 4

struct CentreLess {
    int Axis;
    bool operator()(U32 a, U32 b) const {
        return s_centre[a][Axis] < s_centre[b][Axis];
    }
};

typedef struct {
    U32 Node;
    U32 First;
    U32 Count;
} T_BUILD_TASK;

static void SetBounds(U32 node, U32 first, U32 count) {
    float lo[3] = {1e30f, 1e30f, 1e30f};
    float hi[3] = {-1e30f, -1e30f, -1e30f};
    for (U32 i = first; i < first + count; i++) {
        const U32 t = s_order[i];
        for (int k = 0; k < 3; k++) {
            lo[k] = s_min[t][k] < lo[k] ? s_min[t][k] : lo[k];
            hi[k] = s_max[t][k] > hi[k] ? s_max[t][k] : hi[k];
        }
    }
    float *n = GpuBvhNodes + node * GPUBVH_NODE_FLOATS;
    n[0] = lo[0];
    n[1] = lo[1];
    n[2] = lo[2];
    n[4] = hi[0];
    n[5] = hi[1];
    n[6] = hi[2];
}

void GpuBvh_Build(const float *triangles, U32 count) {
    GpuBvhNbNodes = 0;
    GpuBvhNbTris = 0;
    if (count > GPUBVH_MAX_TRIS) {
        count = GPUBVH_MAX_TRIS;
    }
    if (count == 0) {
        return;
    }
    for (U32 t = 0; t < count; t++) {
        const float *v = triangles + t * 9;
        for (int k = 0; k < 3; k++) {
            const float a = v[k], b = v[3 + k], c = v[6 + k];
            s_min[t][k] = a < b ? (a < c ? a : c) : (b < c ? b : c);
            s_max[t][k] = a > b ? (a > c ? a : c) : (b > c ? b : c);
            s_centre[t][k] = (a + b + c) * (1.0f / 3.0f);
        }
        s_order[t] = t;
    }

    /* Median split on the widest axis of the centroids, depth first. */
    static T_BUILD_TASK stack[128];
    int sp = 0;
    GpuBvhNbNodes = 1;
    stack[sp].Node = 0;
    stack[sp].First = 0;
    stack[sp].Count = count;
    sp++;
    while (sp > 0) {
        const T_BUILD_TASK task = stack[--sp];
        SetBounds(task.Node, task.First, task.Count);
        float *n = GpuBvhNodes + task.Node * GPUBVH_NODE_FLOATS;
        if (task.Count <= LEAF_TRIS || GpuBvhNbNodes + 2 > GPUBVH_MAX_NODES || sp + 2 > 128) {
            n[3] = (float)task.First;
            n[7] = (float)task.Count;
            continue;
        }
        float lo[3] = {1e30f, 1e30f, 1e30f};
        float hi[3] = {-1e30f, -1e30f, -1e30f};
        for (U32 i = task.First; i < task.First + task.Count; i++) {
            const U32 t = s_order[i];
            for (int k = 0; k < 3; k++) {
                lo[k] = s_centre[t][k] < lo[k] ? s_centre[t][k] : lo[k];
                hi[k] = s_centre[t][k] > hi[k] ? s_centre[t][k] : hi[k];
            }
        }
        CentreLess less;
        less.Axis = 0;
        if (hi[1] - lo[1] > hi[less.Axis] - lo[less.Axis]) {
            less.Axis = 1;
        }
        if (hi[2] - lo[2] > hi[less.Axis] - lo[less.Axis]) {
            less.Axis = 2;
        }
        const U32 half = task.Count / 2;
        std::nth_element(s_order + task.First, s_order + task.First + half, s_order + task.First + task.Count, less);

        const U32 left = GpuBvhNbNodes;
        GpuBvhNbNodes += 2;
        n[3] = (float)left;
        n[7] = 0.0f;
        stack[sp].Node = left;
        stack[sp].First = task.First;
        stack[sp].Count = half;
        sp++;
        stack[sp].Node = left + 1;
        stack[sp].First = task.First + half;
        stack[sp].Count = task.Count - half;
        sp++;
    }

    /* Triangles in leaf order: a vertex and two edges. */
    for (U32 i = 0; i < count; i++) {
        const float *v = triangles + s_order[i] * 9;
        float *o = GpuBvhTris + i * GPUBVH_TRI_FLOATS;
        for (int k = 0; k < 3; k++) {
            o[k] = v[k];
            o[4 + k] = v[3 + k] - v[k];
            o[8 + k] = v[6 + k] - v[k];
        }
        o[3] = o[7] = o[11] = 0.0f;
    }
    GpuBvhNbTris = count;
}

static int SlabHit(const float *n, const float o[3], const float inv[3], float tMax) {
    float t0 = 0.0f, t1 = tMax;
    for (int k = 0; k < 3; k++) {
        float a = (n[k] - o[k]) * inv[k];
        float b = (n[4 + k] - o[k]) * inv[k];
        if (a > b) {
            const float s = a;
            a = b;
            b = s;
        }
        t0 = a > t0 ? a : t0;
        t1 = b < t1 ? b : t1;
    }
    return t0 <= t1;
}

float GpuBvh_Trace(const float origin[3], const float dir[3], float tMin, float tMax) {
    if (GpuBvhNbNodes == 0) {
        return -1.0f;
    }
    float inv[3];
    for (int k = 0; k < 3; k++) {
        inv[k] = 1.0f / (fabsf(dir[k]) > 1e-8f ? dir[k] : 1e-8f);
    }
    U32 stack[64];
    int sp = 0;
    stack[sp++] = 0;
    float best = -1.0f;
    while (sp > 0) {
        const float *n = GpuBvhNodes + stack[--sp] * GPUBVH_NODE_FLOATS;
        if (!SlabHit(n, origin, inv, best > 0.0f ? best : tMax)) {
            continue;
        }
        const U32 first = (U32)n[3];
        const U32 leafCount = (U32)n[7];
        if (leafCount == 0) {
            if (sp + 2 <= 64) {
                stack[sp++] = first;
                stack[sp++] = first + 1;
            }
            continue;
        }
        for (U32 i = first; i < first + leafCount; i++) {
            const float *t = GpuBvhTris + i * GPUBVH_TRI_FLOATS;
            const float *e1 = t + 4, *e2 = t + 8;
            const float p[3] = {dir[1] * e2[2] - dir[2] * e2[1], dir[2] * e2[0] - dir[0] * e2[2],
                                dir[0] * e2[1] - dir[1] * e2[0]};
            const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
            if (fabsf(det) < 1e-9f) {
                continue;
            }
            const float id = 1.0f / det;
            const float s[3] = {origin[0] - t[0], origin[1] - t[1], origin[2] - t[2]};
            const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) * id;
            if (u < 0.0f || u > 1.0f) {
                continue;
            }
            const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
            const float v = (dir[0] * q[0] + dir[1] * q[1] + dir[2] * q[2]) * id;
            if (v < 0.0f || u + v > 1.0f) {
                continue;
            }
            const float d = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) * id;
            if (d > tMin && d < tMax && (best < 0.0f || d < best)) {
                best = d;
            }
        }
    }
    return best;
}
