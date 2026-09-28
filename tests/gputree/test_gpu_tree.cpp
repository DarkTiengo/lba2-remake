/* GpuTree_*: the tree sway GPUOBJ.vert computes (TreeSway), mirrored in C. */
#include <SVGA/GPUTREE.H>

#include <cmath>
#include <cstdio>
#include <cstring>

static int failures;

#define CHECK(cond)                                                     \
    do {                                                                \
        if (!(cond)) {                                                  \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
            failures++;                                                 \
        }                                                               \
    } while (0)

static float Length(const float v[3]) {
    return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
}

/* A tree standing upright in its own model space, the wind along x. */
static T_GPUTREE Upright(float phase) {
    T_GPUTREE t;
    std::memset(&t, 0, sizeof(t));
    t.Up[1] = 1.0f;
    t.Wind[0] = 1.0f;
    t.Centre[1] = 2000.0f;
    t.Radius = 800.0f;
    t.Phase = phase;
    return t;
}

/* The base of a tree never moves, trunk or leaf, in any wind. */
static void TestBaseStands(void) {
    const T_GPUTREE t = Upright(1.3f);
    const float base[3] = {0.0f, 0.0f, 0.0f};
    const float below[3] = {120.0f, -40.0f, -60.0f}; /* a root under the ground */
    for (int i = 0; i < 200; i++) {
        const float time = (float)i * 0.37f;
        float out[3];
        GpuTree_Sway(&t, base, FALSE, time, 1.8f, out);
        CHECK(Length(out) == 0.0f);
        GpuTree_Sway(&t, base, TRUE, time, 1.8f, out);
        CHECK(Length(out) == 0.0f);
        GpuTree_Sway(&t, below, TRUE, time, 1.8f, out);
        CHECK(Length(out) == 0.0f);
    }
}

/* How far a point goes is bounded by its height: the bend grows with its
   square, and a leaf flutters by at most GPUTREE_FLUTTER in the wind. */
static void TestBounded(void) {
    const T_GPUTREE t = Upright(0.4f);
    const float heights[4] = {300.0f, 1200.0f, 2500.0f, 3000.0f};
    for (int k = 0; k < 4; k++) {
        const float h = heights[k] / GPUTREE_HEIGHT;
        const float rel[3] = {150.0f, heights[k], -90.0f};
        for (int i = 0; i < 500; i++) {
            const float strength = (i & 1) ? 1.8f : 1.0f;
            float out[3];
            GpuTree_Sway(&t, rel, FALSE, (float)i * 0.113f, strength, out);
            CHECK(Length(out) <= GPUTREE_BEND * h * h * (strength + 0.12f) + 1e-3f);
            GpuTree_Sway(&t, rel, TRUE, (float)i * 0.113f, strength, out);
            CHECK(Length(out) <= GPUTREE_BEND * h * h * (strength + 0.12f) + GPUTREE_FLUTTER * strength + 1e-3f);
        }
    }
}

/* Away from the leaves, a point moves by its height alone: a palm's trunk and
   the crown drawn as a model of its own, both placed at the same base, bend
   together where they meet. */
static void TestTrunkAndCrownAgree(void) {
    const T_GPUTREE trunk = Upright(2.2f);
    T_GPUTREE crown = Upright(2.2f);
    crown.Centre[1] = 2500.0f;
    const float top[3] = {10.0f, 2220.0f, 5.0f};      /* the trunk's top */
    const float bottom[3] = {-80.0f, 2220.0f, 60.0f}; /* the crown's bottom, beside it */
    for (int i = 0; i < 100; i++) {
        float a[3], b[3];
        GpuTree_Sway(&trunk, top, FALSE, (float)i * 0.29f, 1.0f, a);
        GpuTree_Sway(&crown, bottom, FALSE, (float)i * 0.29f, 1.0f, b);
        CHECK(std::fabs(a[0] - b[0]) < 1e-4f && std::fabs(a[1] - b[1]) < 1e-4f && std::fabs(a[2] - b[2]) < 1e-4f);
    }
}

/* The motion is smooth: a frame later a leaf has moved a little, not jumped. */
static void TestContinuous(void) {
    const T_GPUTREE t = Upright(0.9f);
    const float rel[3] = {400.0f, 2600.0f, 300.0f};
    for (int i = 0; i < 300; i++) {
        const float time = (float)i * 0.05f;
        float a[3], b[3];
        GpuTree_Sway(&t, rel, TRUE, time, 1.8f, a);
        GpuTree_Sway(&t, rel, TRUE, time + 1.0f / 60.0f, 1.8f, b);
        const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
        CHECK(Length(d) < 2.0f);
    }
}

/* The same tree seen in another space (the capture's view space rather than
   the model's) moves the same way, turned with it: a leaf's flutter follows
   where it lies on the tree, not the camera. */
static void TestSpaceIndependent(void) {
    const T_GPUTREE model = Upright(1.7f);
    /* A rotation: 30 degrees about z, then 50 about y. */
    const float cz = std::cos(0.5236f), sz = std::sin(0.5236f), cy = std::cos(0.8727f), sy = std::sin(0.8727f);
    const float r[9] = {cy * cz, -cy * sz, sy, sz, cz, 0.0f, -sy * cz, sy * sz, cy};
    T_GPUTREE view = model;
    for (int a = 0; a < 3; a++) {
        view.Up[a] = r[a * 3] * model.Up[0] + r[a * 3 + 1] * model.Up[1] + r[a * 3 + 2] * model.Up[2];
        view.Wind[a] = r[a * 3] * model.Wind[0] + r[a * 3 + 1] * model.Wind[1] + r[a * 3 + 2] * model.Wind[2];
    }
    const float rel[3] = {350.0f, 2400.0f, -210.0f};
    float relView[3];
    for (int a = 0; a < 3; a++) {
        relView[a] = r[a * 3] * rel[0] + r[a * 3 + 1] * rel[1] + r[a * 3 + 2] * rel[2];
    }
    for (int i = 0; i < 100; i++) {
        float m[3], v[3], turned[3];
        GpuTree_Sway(&model, rel, TRUE, (float)i * 0.31f, 1.0f, m);
        GpuTree_Sway(&view, relView, TRUE, (float)i * 0.31f, 1.0f, v);
        for (int a = 0; a < 3; a++) {
            turned[a] = r[a * 3] * m[0] + r[a * 3 + 1] * m[1] + r[a * 3 + 2] * m[2];
        }
        CHECK(std::fabs(turned[0] - v[0]) < 0.05f && std::fabs(turned[1] - v[1]) < 0.05f &&
              std::fabs(turned[2] - v[2]) < 0.05f);
    }
}

/* A gust reaches trees in the order the wind meets them, as it does the grass;
   a tree keeps its place in it. */
static void TestGustField(void) {
    CHECK(GpuTree_Phase(5376, 30464) == GpuTree_Phase(5376, 30464));
    const float step = 10000.0f;
    const S32 x = 4000, z = 9000;
    const S32 dx = (S32)(step * GPUTREE_WIND_X), dz = (S32)(step * GPUTREE_WIND_Z);
    const float along = GpuTree_Phase(x + dx, z + dz) - GpuTree_Phase(x, z);
    const float expected = (float)(dx * GPUTREE_WIND_X + dz * GPUTREE_WIND_Z) * 0.004f;
    CHECK(std::fabs(along - expected) <= 0.6f + 1e-3f);
}

static void TestFoliage(void) {
    const U32 leaves = (1u << 8) | GPUTREE_FOLIAGE_TEXTURED;
    CHECK(GpuTree_IsFoliage(leaves, FALSE, 8 * 16 + 5));
    CHECK(!GpuTree_IsFoliage(leaves, FALSE, 1 * 16 + 5)); /* the trunk's bank */
    CHECK(GpuTree_IsFoliage(leaves, TRUE, 3));
    CHECK(!GpuTree_IsFoliage(1u << 8, TRUE, 8 * 16)); /* textured, but not listed */
    CHECK(!GpuTree_IsFoliage(0, FALSE, 8 * 16));      /* a trunk model */
}

static void TestUniform(void) {
    float u[16];
    GpuTree_Uniform(NULL, u);
    for (int i = 0; i < 16; i++) {
        CHECK(u[i] == 0.0f);
    }
    const T_GPUTREE t = Upright(0.75f);
    GpuTree_Uniform(&t, u);
    CHECK(u[1] == 1.0f && u[3] == 1.0f); /* up, and "a tree" */
    CHECK(u[7] == 0.75f);                /* phase */
    CHECK(u[8] == 1.0f);                 /* wind */
    CHECK(u[13] == 2000.0f && u[15] == 800.0f);
}

int main() {
    TestBaseStands();
    TestBounded();
    TestTrunkAndCrownAgree();
    TestContinuous();
    TestSpaceIndependent();
    TestGustField();
    TestFoliage();
    TestUniform();
    if (failures != 0) {
        std::printf("%d failure(s)\n", failures);
        return 1;
    }
    std::printf("test_gpu_tree: OK\n");
    return 0;
}
