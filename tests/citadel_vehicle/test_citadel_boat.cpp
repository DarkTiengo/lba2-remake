#include <OBJECT/CITADEL_BOAT.H>
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
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, 0) == 0);
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, 815) == 1);
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, -95) == 0);
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, 935) == 1);
    assert(CitadelBoat_DoorFraction(TYPE_ROTATE, 815) == 0);
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, 307) > 0);
    assert(CitadelBoat_DoorFraction(TYPE_TRANSLATE, 307) < 1);

    TYPE_MAT matrices[14] = {};
    for (int i = 0; i < 14; i++)
        matrices[i].F.M11 = matrices[i].F.M22 = matrices[i].F.M33 = 1;
    matrices[2].F.TY = 276;
    const float origin[3] = {100, 200, 300};
    T_OBJ_3D actor = {};
    actor.NbGroups = 14;
    actor.CurrentFrame[4].Type = TYPE_TRANSLATE;
    T_GPUOBJ_DRAW draw = {};
    assert(CitadelBoat_Available());
    CitadelBoat_SetPose(&actor);
    assert(CitadelBoat_Emit(&draw, matrices, TRUE, origin));
    assert(draw.HasGlass);
    assert(captured.size() > 90000 && captured.size() % 3 == 0);
    const std::vector<T_GPUOBJ_VERTEX> closed = captured;

    actor.CurrentFrame[4].Gamma = 815;
    CitadelBoat_SetPose(&actor);
    assert(CitadelBoat_Emit(&draw, matrices, TRUE, origin));
    unsigned moved = 0, fixed = 0;
    for (size_t i = 0; i < captured.size(); i++) {
        const float *a = closed[i].vpos, *b = captured[i].vpos;
        assert(a[1] == b[1]);
        if (a[0] != b[0] || a[2] != b[2]) moved++;
        else fixed++;
        const float *n = captured[i].normal;
        assert(std::fabs(n[0]*n[0]+n[1]*n[1]+n[2]*n[2]-1) < 0.0001f);
    }
    assert(moved > 0 && fixed > moved * 10);
    // Closed restores the exact pose; seeking, pause and reload need no timer.
    actor.CurrentFrame[4].Gamma = 0;
    CitadelBoat_SetPose(&actor);
    assert(CitadelBoat_Emit(&draw, matrices, TRUE, origin));
    assert(std::memcmp(&captured[0], &closed[0], closed.size()*sizeof(closed[0])) == 0);
    full = true;
    assert(!CitadelBoat_Emit(&draw, matrices, TRUE, origin));
    assert(!CitadelBoat_Emit(NULL, matrices, TRUE, origin));
    CitadelBoat_SetPose(NULL);
    return 0;
}
