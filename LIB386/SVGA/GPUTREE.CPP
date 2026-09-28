#include <SVGA/GPUTREE.H>

#include <math.h>
#include <string.h>

S32 GpuTreeEnabled = TRUE;
S32 GpuObjTreeModel = FALSE;
U32 GpuObjTreeFoliage = 0;
float GpuObjTreePhase = 0.0f;

void GpuTree_Begin(U32 foliage, S32 worldX, S32 worldZ) {
    GpuObjTreeModel = TRUE;
    GpuObjTreeFoliage = foliage;
    GpuObjTreePhase = GpuTree_Phase(worldX, worldZ);
}

void GpuTree_End(void) {
    GpuObjTreeModel = FALSE;
    GpuObjTreeFoliage = 0;
}

float GpuTree_Phase(S32 worldX, S32 worldZ) {
    /* The grass's gust field (TERRAIN_GPU.CPP GrassTri), so a gust bends the
       lawn and the trees standing in it together. */
    U32 h = (U32)worldX * 73856093u ^ (U32)worldZ * 19349663u;
    h ^= h >> 13;
    h *= 0x5bd1e995u;
    h ^= h >> 15;
    const float own = (float)(h & 0xFFFF) / 65535.0f;
    return ((float)worldX * GPUTREE_WIND_X + (float)worldZ * GPUTREE_WIND_Z) * 0.004f + own * 0.6f;
}

S32 GpuTree_IsFoliage(U32 foliage, S32 textured, U32 color) {
    if (textured) {
        return (foliage & GPUTREE_FOLIAGE_TEXTURED) != 0;
    }
    return (foliage >> ((color >> 4) & 15) & 1) != 0;
}

void GpuTree_Uniform(const T_GPUTREE *tree, float out[16]) {
    memset(out, 0, 16 * sizeof(float));
    if (tree == NULL) {
        return;
    }
    out[0] = tree->Up[0];
    out[1] = tree->Up[1];
    out[2] = tree->Up[2];
    out[3] = 1.0f;
    out[4] = tree->Base[0];
    out[5] = tree->Base[1];
    out[6] = tree->Base[2];
    out[7] = tree->Phase;
    out[8] = tree->Wind[0];
    out[9] = tree->Wind[1];
    out[10] = tree->Wind[2];
    out[12] = tree->Centre[0];
    out[13] = tree->Centre[1];
    out[14] = tree->Centre[2];
    out[15] = tree->Radius;
}

static float Dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

void GpuTree_Sway(const T_GPUTREE *tree, const float rel[3], S32 foliage, float time, float strength, float out[3]) {
    float h = Dot3(rel, tree->Up);
    h = (h > 0.0f ? h : 0.0f) / GPUTREE_HEIGHT;
    const float gust = 0.5f + 0.5f * sinf(time * 1.1f - tree->Phase * 0.8f);
    const float lean = GPUTREE_BEND * h * h * strength * (0.15f + 0.85f * gust);
    const float bob = GPUTREE_BEND * h * h * 0.12f * sinf(time * 1.9f + tree->Phase * 2.3f);
    for (S32 a = 0; a < 3; a++) {
        out[a] = tree->Wind[a] * (lean + bob);
    }
    if (!foliage) {
        return;
    }
    /* The leaves flutter out of step, by where they lie on the tree: a phase
       that changes smoothly with the place, so the rounding of a point turned
       into another space (the capture's camera, the model's own) cannot move
       it, as a hash of the place would. */
    const float side[3] = {tree->Up[1] * tree->Wind[2] - tree->Up[2] * tree->Wind[1],
                           tree->Up[2] * tree->Wind[0] - tree->Up[0] * tree->Wind[2],
                           tree->Up[0] * tree->Wind[1] - tree->Up[1] * tree->Wind[0]};
    const float leaf = Dot3(rel, tree->Wind) * 0.021f + Dot3(rel, side) * 0.017f + Dot3(rel, tree->Up) * 0.013f;
    const float reach = h * 4.0f < 1.0f ? h * 4.0f : 1.0f;
    const float f = GPUTREE_FLUTTER * reach * strength * (0.3f + 0.7f * gust) * sinf(time * 4.3f + leaf);
    for (S32 a = 0; a < 3; a++) {
        out[a] += (tree->Up[a] * 0.5f + side[a] * 0.85f) * f;
    }
}
