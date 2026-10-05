#include <OBJECT/CITADEL_VEHICLE.H>
#include <OBJECT/CITADEL_SMOOTH.H>

#include <math.h>

static S32 Near(float a, float b) {
    return fabsf(a - b) < 0.0001f;
}

int main() {
    if (!CitadelBoat_IsVisibleActor(0, 43, 10, 165) || CitadelBoat_IsVisibleActor(0, 43, 9, 165) ||
        CitadelBoat_IsVisibleActor(0, 43, 10, 56) || CitadelBoat_IsVisibleActor(0, 49, 10, 165) ||
        CitadelBoat_IsVisibleActor(1, 43, 10, 165))
        return 263;
    for (S32 color = 0; color < 256; color++) {
        const S32 bank = color >> 4;
        const S32 expected = bank == 6 || bank == 4 || bank == 3 ? CITADEL_VEHICLE_PANEL
                                         : CITADEL_VEHICLE_ORIGINAL;
        if (CitadelVehicle_Material((U8)color) != expected)
            return color + 1;
        const S32 boat = bank == 4 || bank == 6 || bank == 11 || bank == 14
                             ? CITADEL_VEHICLE_PANEL
                             : CITADEL_VEHICLE_ORIGINAL;
        if (CitadelBoat_Material((U8)color) != boat)
            return color + 300;
        if (CitadelBoat_IsGlass((U8)color, 1415) != (bank == 10) ||
            CitadelBoat_IsGlass((U8)color, -77) != (color == 164))
            return color + 600;
    }
    const float points[3][3] = {{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 2.0f, 0.0f}};
    const float normals[3][3] = {{-0.7071068f, 0.0f, 0.7071068f},
                                 {0.7071068f, 0.0f, 0.7071068f},
                                 {0.0f, 0.7071068f, 0.7071068f}};
    float result[3];
    const float corner[3] = {1.0f, 0.0f, 0.0f};
    CitadelSmooth_PatchPoint(points, normals, corner, 0.45f, result);
    if (!Near(result[0], 0.0f) || !Near(result[1], 0.0f) || !Near(result[2], 0.0f))
        return 257;
    const float edge[3] = {0.5f, 0.5f, 0.0f};
    CitadelSmooth_PatchPoint(points, normals, edge, 1.0f, result);
    if (!Near(result[0], 1.0f) || !Near(result[1], 0.0f) || !Near(result[2], 0.5f))
        return 258;
    CitadelSmooth_PatchPoint(points, normals, edge, 0.0f, result);
    if (!Near(result[0], 1.0f) || !Near(result[1], 0.0f) || !Near(result[2], 0.0f))
        return 259;
    const float flat[3][3] = {{0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}};
    const float centre[3] = {1.0f / 3.0f, 1.0f / 3.0f, 1.0f / 3.0f};
    CitadelSmooth_PatchPoint(points, flat, centre, 1.0f, result);
    if (!Near(result[0], 2.0f / 3.0f) || !Near(result[1], 2.0f / 3.0f) || !Near(result[2], 0.0f))
        return 260;
    const float rounded[3][3] = {{0.0f, 0.0f, 1.0f}, {0.3f, 0.0f, 0.9539392f},
                                 {0.0f, 0.3f, 0.9539392f}};
    if (!CitadelSmooth_Curved(rounded) || CitadelSmooth_Curved(flat))
        return 261;
    const float hard[3][3] = {{1.0f, 0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}};
    if (CitadelSmooth_Curved(hard))
        return 262;
    return 0;
}
