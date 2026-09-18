// The ray-tracing hierarchy must find exactly what testing every triangle finds.
#include <SVGA/GPUBVH.H>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static float Rand(float lo, float hi) {
    return lo + (hi - lo) * (float)rand() / (float)RAND_MAX;
}

static float BruteForce(const float *tris, int count, const float o[3], const float d[3], float tMin, float tMax) {
    float best = -1.0f;
    for (int i = 0; i < count; i++) {
        const float *a = tris + i * 9;
        const float e1[3] = {a[3] - a[0], a[4] - a[1], a[5] - a[2]};
        const float e2[3] = {a[6] - a[0], a[7] - a[1], a[8] - a[2]};
        const float p[3] = {d[1] * e2[2] - d[2] * e2[1], d[2] * e2[0] - d[0] * e2[2], d[0] * e2[1] - d[1] * e2[0]};
        const float det = e1[0] * p[0] + e1[1] * p[1] + e1[2] * p[2];
        if (fabsf(det) < 1e-9f) {
            continue;
        }
        const float s[3] = {o[0] - a[0], o[1] - a[1], o[2] - a[2]};
        const float u = (s[0] * p[0] + s[1] * p[1] + s[2] * p[2]) / det;
        if (u < 0.0f || u > 1.0f) {
            continue;
        }
        const float q[3] = {s[1] * e1[2] - s[2] * e1[1], s[2] * e1[0] - s[0] * e1[2], s[0] * e1[1] - s[1] * e1[0]};
        const float v = (d[0] * q[0] + d[1] * q[1] + d[2] * q[2]) / det;
        if (v < 0.0f || u + v > 1.0f) {
            continue;
        }
        const float t = (e2[0] * q[0] + e2[1] * q[1] + e2[2] * q[2]) / det;
        if (t > tMin && t < tMax && (best < 0.0f || t < best)) {
            best = t;
        }
    }
    return best;
}

int main() {
    srand(1234);
    const int count = 3000;
    static float tris[count * 9];
    for (int i = 0; i < count; i++) {
        const float c[3] = {Rand(-8000, 8000), Rand(0, 3000), Rand(-8000, 8000)};
        for (int k = 0; k < 9; k++) {
            tris[i * 9 + k] = c[k % 3] + Rand(-300, 300);
        }
    }
    GpuBvh_Build(tris, count);
    if (GpuBvhNbTris != (U32)count || GpuBvhNbNodes == 0) {
        printf("FAIL: built %u triangles, %u nodes\n", GpuBvhNbTris, GpuBvhNbNodes);
        return 1;
    }
    int hits = 0;
    for (int r = 0; r < 4000; r++) {
        const float o[3] = {Rand(-9000, 9000), Rand(-500, 3500), Rand(-9000, 9000)};
        float d[3] = {Rand(-1, 1), Rand(-1, 1), Rand(-1, 1)};
        const float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        for (int k = 0; k < 3; k++) {
            d[k] /= len;
        }
        const float want = BruteForce(tris, count, o, d, 1.0f, 60000.0f);
        const float got = GpuBvh_Trace(o, d, 1.0f, 60000.0f);
        if ((want < 0.0f) != (got < 0.0f) || (want >= 0.0f && fabsf(want - got) > 0.01f)) {
            printf("FAIL: ray %d: brute force %f, hierarchy %f\n", r, want, got);
            return 1;
        }
        hits += want >= 0.0f;
    }
    if (hits < 100) {
        printf("FAIL: only %d rays hit: the test scene is too sparse\n", hits);
        return 1;
    }
    GpuBvh_Build(tris, 0);
    if (GpuBvhNbNodes != 0 || GpuBvh_Trace(tris, tris + 3, 0.0f, 1.0f) >= 0.0f) {
        printf("FAIL: an empty build must hold nothing\n");
        return 1;
    }
    printf("test_gpu_bvh: %d of 4000 rays hit, all as brute force\n", hits);
    return 0;
}
