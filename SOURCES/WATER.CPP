#include "WATER.H"
#include <3D/MOVE.H>
#include "COMMON.H"
#include "SETTINGS.H"
#include "3DEXT/TERRAIN.H"

#include <SVGA/GPUWATER.H>

extern U8 PtrWorldCodeBrick(S32 xw, S32 yw, S32 zw);

static const T_SETTING WaterSettings[] = {
    {"GpuWater", NULL, "gfx_water", &GpuWaterEnabled, SETTING_OR_DEFAULT, TRUE, 0, 1,
     NULL, NULL, NULL, NULL, "GPU renderer: modern sea water, shore foam and splashes"},
};

static float s_waterOriginX = 0.0f;
static float s_waterOriginZ = 0.0f;

void Water_ReadConfig(void) {
    Settings_ReadConfig(WaterSettings, 1);
}

void Water_WriteConfig(void) {
    Settings_WriteConfig(WaterSettings, 1);
}

void Water_RegisterCvars(void) {
    Settings_RegisterCvars(WaterSettings, 1);
}

void Water_SetScene(S32 island, S32 cubeX, S32 cubeZ) {
    s_waterOriginX = (float)cubeX * 32768.0f;
    s_waterOriginZ = (float)cubeZ * 32768.0f;
    GpuWater_SetScene(island);
}

S32 Water_IsGameCode(S32 code) {
    return code == CJ_WATER || code == CJ_FOOT_WATER || code == CJ_ANIMATED_WATER;
}

void Water_Impact(S32 x, S32 z, U32 milliseconds, float strength) {
    GpuWater_AddImpact(s_waterOriginX + (float)x, (float)Y_SEA,
                       s_waterOriginZ + (float)z, milliseconds, strength);
}

void Water_AnimationContact(S32 animation, S32 frame, S32 x, S32 z, U32 milliseconds) {
    /* The harbour fisher's authored animation contains the rod, line and fish
       in one body, so no projectile crosses CJ_WATER. Its catch frame emits at
       the nearest coded water point instead of depending on scene coordinates. */
    if (animation != 807 || frame != 4 || !GpuWaterEnabled) {
        return;
    }
    static const S32 directions[8][2] = {
        {1, 0}, {-1, 0}, {0, 1}, {0, -1},
        {1, 1}, {-1, 1}, {1, -1}, {-1, -1},
    };
    for (S32 radius = 512; radius <= 3072; radius += 512) {
        for (S32 i = 0; i < 8; i++) {
            S32 px = x + directions[i][0] * radius;
            S32 pz = z + directions[i][1] * radius;
            if (Water_IsGameCode(PtrWorldCodeBrick(px, 0, pz) >> 4)) {
                /* Pulling the fish breaks the surface sharply, then leaves a
                   short, wider patch of churn where the line came out. */
                Water_Impact(px, pz, milliseconds, 2.15f);
                S32 sideX = -directions[i][1] * 192;
                S32 sideZ = directions[i][0] * 192;
                if (Water_IsGameCode(PtrWorldCodeBrick(px + sideX, 0, pz + sideZ) >> 4)) {
                    Water_Impact(px + sideX, pz + sideZ, milliseconds + 90, 1.05f);
                }
                if (Water_IsGameCode(PtrWorldCodeBrick(px - sideX, 0, pz - sideZ) >> 4)) {
                    Water_Impact(px - sideX, pz - sideZ, milliseconds + 180, 0.85f);
                }
                return;
            }
        }
    }
}
