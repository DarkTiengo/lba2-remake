#include <OBJECT/CITADEL_SCOOTER.H>
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
        T_OBJ_POINT points[5];
    } f = {};
    f.body.OffPoints = (S32)((const U8 *)f.points - (const U8 *)&f.body);
    f.body.NbPoints = 5;
    f.points[0].Group = 2;
    f.points[1].Group = 2;
    f.points[2].Group = 20;
    f.points[3].Group = 17; // driver
    f.points[4].Group = 29; // passenger
    U16 poly[6] = {0, 1, 2, 0, 0, 0};
    assert(CitadelScooter_ReplacesEntity(&f.body, 0, poly));
    assert(CitadelScooter_ReplacesEntity(&f.body, 1, poly));
    poly[3] = 3;
    assert(!CitadelScooter_ReplacesEntity(&f.body, 1, poly));
    poly[2] = 3;
    assert(!CitadelScooter_ReplacesEntity(&f.body, 0, poly));
    assert(!CitadelScooter_ReplacesEntity(&f.body, 97, poly));
    poly[2] = 4;
    assert(!CitadelScooter_ReplacesEntity(&f.body, 98, poly));
    poly[2] = 2;
    assert(CitadelScooter_ReplacesEntity(&f.body, 97, poly));
    assert(!CitadelScooter_ReplacesEntity(&f.body, 96, poly));
    poly[3] = 1;
    assert(CitadelScooter_ReplacesEntity(&f.body, 96, poly));
    poly[2] = 5;
    assert(!CitadelScooter_ReplacesEntity(&f.body, 97, poly));

    TYPE_MAT matrices[30] = {};
    for (int i = 0; i < 30; i++)
        matrices[i].F.M11 = matrices[i].F.M22 = matrices[i].F.M33 = 1;
    matrices[2].F.TY = 416;
    matrices[20].F.TY = 166;
    matrices[20].F.TZ = 250;
    const float origin[3] = {100, 200, 300};
    T_GPUOBJ_DRAW draw = {};
    f.body.NbGroupes = 21;
    CitadelScooter_Select(-1);
    assert(!CitadelScooter_Emit(&draw, &f.body, matrices, origin));
    CitadelScooter_Select(148);
    assert(CitadelScooter_Emit(&draw, &f.body, matrices, origin));
    assert(captured.size() > 90000 && captured.size() % 3 == 0);
    const std::vector<T_GPUOBJ_VERTEX> straight = captured;
    matrices[20].F.M11 = matrices[20].F.M33 = 0;
    matrices[20].F.M13 = 1;
    matrices[20].F.M31 = -1;
    assert(CitadelScooter_Emit(&draw, &f.body, matrices, origin));
    unsigned moved = 0, fixed = 0;
    for (size_t i = 0; i < captured.size(); i++) {
        const float *a = straight[i].vpos, *b = captured[i].vpos;
        assert(a[1] == b[1]);
        if (a[0] != b[0] || a[2] != b[2]) moved++;
        else fixed++;
        const float *n = captured[i].normal;
        assert(std::fabs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1) < 0.0001f);
        assert(captured[i].light[3] == GPUOBJ_MODE_RGB);
    }
    assert(moved > 100 && fixed > 100);
    for (int body = 149; body <= 150; body++) {
        CitadelScooter_Select(body);
        assert(!CitadelScooter_Emit(&draw, &f.body, matrices, origin));
        f.body.NbGroupes = 30;
        assert(CitadelScooter_Emit(&draw, &f.body, matrices, origin));
        f.body.NbGroupes = 21;
    }
    f.body.NbGroupes = 30;
    full = true;
    assert(!CitadelScooter_Emit(&draw, &f.body, matrices, origin));
    assert(!CitadelScooter_Emit(NULL, &f.body, matrices, origin));
    assert(!CitadelScooter_Emit(&draw, NULL, matrices, origin));
    full = false;
    TypeProj = TYPE_ISO;
    assert(!CitadelScooter_Emit(&draw, &f.body, matrices, origin));
    return 0;
}
