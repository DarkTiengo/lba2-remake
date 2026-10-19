#include <OBJECT/CITADEL_BUGGY.H>
#include <OBJECT/AFF_GPU.H>
#include <3D/CAMERA.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <cassert>
#include <cmath>
#include <cstring>
#include <vector>

S32 TypeProj = TYPE_3D;
S32 CameraXLight = 100, CameraYLight = 100, CameraZLight = 100;
static std::vector<T_GPUOBJ_VERTEX> captured;
static bool full = false;
T_GPUOBJ_VERTEX *GpuObj_AllocVerts(U32 count) {
    if (full) return NULL;
    captured.resize(count);
    return &captured[0];
}
void AffGpu_ViewVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth) {
    std::memset(v, 0, sizeof(*v));
    v->vpos[0] = x;
    v->vpos[1] = y;
    v->vpos[2] = -depth;
}

int main() {
    struct Fixture {
        T_BODY_HEADER body;
        T_OBJ_POINT points[20];
    } f = {};
    f.body.OffPoints = (S32)((const U8 *)f.points - (const U8 *)&f.body);
    f.body.NbPoints = 20;
    for (int i = 0; i < 20; i++) f.points[i].Group = i;
    CitadelBuggy_Select(14);
    U16 poly[6] = {2, 5, 12, 3, 0, 0};
    assert(CitadelBuggy_ReplacesEntity(&f.body, 0, poly));
    assert(CitadelBuggy_ReplacesEntity(&f.body, 1, poly));
    // Hands/wheel, rider and weapon parts must all survive replacement.
    for (int group = 13; group < 20; group++) {
        poly[2] = group;
        assert(!CitadelBuggy_ReplacesEntity(&f.body, 0, poly));
        assert(!CitadelBuggy_ReplacesEntity(&f.body, 1, poly));
        assert(!CitadelBuggy_ReplacesEntity(&f.body, 96, poly));
        assert(!CitadelBuggy_ReplacesEntity(&f.body, 97, poly));
        assert(!CitadelBuggy_ReplacesEntity(&f.body, 98, poly));
    }
    poly[2] = 13;
    CitadelBuggy_Select(219);
    assert(CitadelBuggy_ReplacesEntity(&f.body, 0, poly));
    poly[2] = 20;
    assert(!CitadelBuggy_ReplacesEntity(&f.body, 97, poly));
    assert(!CitadelBuggy_ReplacesEntity(NULL, 0, poly));
    assert(!CitadelBuggy_ReplacesEntity(&f.body, 0, NULL));

    TYPE_MAT matrices[30] = {};
    for (int i = 0; i < 30; i++)
        matrices[i].F.M11 = matrices[i].F.M22 = matrices[i].F.M33 = 1;
    matrices[2].F.TY = 199;
    const int groups[] = {5, 8, 10, 12};
    const float pivots[4][3] = {{500,133,400}, {-500,133,400}, {-400,199,-333}, {399,199,-333}};
    for (int k = 0; k < 4; k++) {
        matrices[groups[k]].F.TX = pivots[k][0];
        matrices[groups[k]].F.TY = pivots[k][1];
        matrices[groups[k]].F.TZ = pivots[k][2];
    }
    const float origin[3] = {100, 200, 300};
    T_GPUOBJ_DRAW draw = {};
    assert(CitadelBuggy_Available());
    f.body.NbGroupes = 1;
    assert(CitadelBuggy_Emit(&draw, &f.body, matrices, FALSE, origin));
    const std::vector<T_GPUOBJ_VERTEX> decor = captured;
    assert(draw.HasGlass && captured.size() == 119760*3);
    f.body.NbGroupes = 16;
    assert(CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    assert(decor.size() == captured.size());
    // The actor and decor share a rest silhouette; only float cancellation differs.
    for (size_t i = 0; i < captured.size(); i++) {
        for (int j = 0; j < 3; j++)
            assert(std::fabs(decor[i].vpos[j]-captured[i].vpos[j]) < .0002f);
        const float *n = captured[i].normal;
        assert(std::fabs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1) < .0001f);
        assert(captured[i].light[3] == GPUOBJ_MODE_RGB);
    }
    const std::vector<T_GPUOBJ_VERTEX> rest = captured;
    // Each original wheel matrix affects only its own wheel, around a fixed centre.
    for (int k = 0; k < 4; k++) {
        TYPE_MAT &m = matrices[groups[k]];
        m.F.M22 = m.F.M33 = 0;
        m.F.M23 = 1;
        m.F.M32 = -1;
        assert(CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
        unsigned moved = 0, fixed = 0;
        const float cy = origin[1]+213.6f;
        const float cz = origin[2]+(k < 2 ? 420.0f : -332.4f);
        for (size_t i = 0; i < captured.size(); i++) {
            const float *a = rest[i].vpos, *b = captured[i].vpos;
            assert(a[0] == b[0]);
            if (a[1] == b[1] && a[2] == b[2]) fixed++;
            else {
                moved++;
                assert(std::fabs((b[1]-cy)-(a[2]-cz)) < .0002f);
                assert(std::fabs((b[2]-cz)+(a[1]-cy)) < .0002f);
            }
        }
        assert(moved > 1000 && fixed > moved);
        m.F.M22 = m.F.M33 = 1;
        m.F.M23 = m.F.M32 = 0;
    }
    // Front steering changes yaw and the offset axle, independently of the rear.
    matrices[5].F.M11 = matrices[5].F.M33 = 0;
    matrices[5].F.M13 = 1;
    matrices[5].F.M31 = -1;
    matrices[5].F.TX = 400;
    matrices[5].F.TZ = 300;
    assert(CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    unsigned steered = 0;
    for (size_t i = 0; i < captured.size(); i++) {
        const float *a = rest[i].vpos, *b = captured[i].vpos;
        assert(a[1] == b[1]);
        if (a[0] != b[0] || a[2] != b[2]) {
            steered++;
            assert(std::fabs(b[0]-(origin[0]+404+(a[2]-origin[2]-420))) < .0002f);
            assert(std::fabs(b[2]-(origin[2]+320-(a[0]-origin[0]-504))) < .0002f);
        }
    }
    assert(steered > 1000 && steered < captured.size()/2);
    matrices[5].F.M11 = matrices[5].F.M33 = 1;
    matrices[5].F.M13 = matrices[5].F.M31 = 0;
    matrices[5].F.TX = 500;
    matrices[5].F.TZ = 400;
    const int bodies[] = {14,15,22,23};
    for (int i = 0; i < 4; i++) {
        CitadelBuggy_Select(bodies[i]);
        f.body.NbGroupes = i < 2 ? 16 : 20;
        assert(CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
        assert(captured.size() < rest.size()); // no duplicate authored steering wheel
        f.body.NbGroupes = 17;
        assert(!CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    }
    f.body.NbGroupes = 16;
    CitadelBuggy_Select(-1);
    assert(!CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    CitadelBuggy_Select(219);
    full = true;
    assert(!CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    full = false;
    assert(!CitadelBuggy_Emit(NULL, &f.body, matrices, TRUE, origin));
    assert(!CitadelBuggy_Emit(&draw, NULL, matrices, TRUE, origin));
    assert(!CitadelBuggy_Emit(&draw, &f.body, NULL, TRUE, origin));
    TypeProj = TYPE_ISO;
    assert(!CitadelBuggy_Emit(&draw, &f.body, matrices, TRUE, origin));
    return 0;
}
