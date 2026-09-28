#include "WATER.H"
#include <3D/MOVE.H>
#include "COMMON.H"
#include "SETTINGS.H"
#include <SVGA/GPUWATER.H>

#include <cstdio>

static int failures;
static S32 waterX, waterZ;
static S32 sideX, sideZ;
static bool sideWater;
static S32 otherSideX, otherSideZ;
static bool otherSideWater;

void Settings_ReadConfig(const T_SETTING *, S32) {}
void Settings_WriteConfig(const T_SETTING *, S32) {}
void Settings_RegisterCvars(const T_SETTING *, S32) {}

U8 PtrWorldCodeBrick(S32 x, S32, S32 z) {
    return ((x == waterX && z == waterZ) ||
            (sideWater && x == sideX && z == sideZ) ||
            (otherSideWater && x == otherSideX && z == otherSideZ)) ? (U8)(CJ_WATER << 4) : 0;
}

static void Check(bool condition, const char *message) {
    if (!condition) {
        std::printf("FAIL: %s\n", message);
        ++failures;
    }
}

static int CountImpacts() {
    int count = 0;
    for (int i = 0; i < GPUWATER_MAX_IMPACTS; ++i) {
        float impact[5];
        if (GpuWater_GetImpactWorld(i, impact)) {
            ++count;
        }
    }
    return count;
}

static bool HasImpact(float x, float z, float strength) {
    for (int i = 0; i < GPUWATER_MAX_IMPACTS; ++i) {
        float impact[5];
        if (GpuWater_GetImpactWorld(i, impact) && impact[0] == x &&
            impact[1] == -50.0f && impact[2] == z && impact[4] == strength) {
            return true;
        }
    }
    return false;
}

int main() {
    GpuWaterEnabled = TRUE;
    GpuWater_SetTime(1000);
    waterX = 1024;
    waterZ = 0;
    sideWater = false;
    Water_SetScene(101, 2, 3);

    Check(Water_IsGameCode(CJ_WATER) && Water_IsGameCode(CJ_FOOT_WATER) &&
          Water_IsGameCode(CJ_ANIMATED_WATER) && !Water_IsGameCode(0),
          "all authored water codes are recognized");
    Water_AnimationContact(806, 4, 0, 0, 1000);
    Water_AnimationContact(807, 3, 0, 0, 1000);
    Check(CountImpacts() == 0, "only the fisher catch frame emits");

    Water_AnimationContact(807, 4, 0, 0, 1000);
    Check(CountImpacts() == 1 && HasImpact(66560.0f, 98304.0f, 2.15f),
          "fisher searches coded water and applies the cube origin");

    GpuWaterEnabled = FALSE;
    Water_SetScene(104, 2, 3);
    Water_AnimationContact(807, 4, 0, 0, 1000);
    Check(CountImpacts() == 0, "disabled GPU water emits no fisherman contacts");
    GpuWaterEnabled = TRUE;

    Water_SetScene(105, 0, 0);
    waterX = 512;
    sideX = 512;
    sideZ = 192;
    sideWater = true;
    Water_AnimationContact(807, 4, 0, 0, 1000);
    GpuWater_SetTime(1200);
    Check(CountImpacts() == 2 && HasImpact(512.0f, 0.0f, 2.15f) &&
          HasImpact(512.0f, 192.0f, 1.05f),
          "a coded side point creates an additional churn contact");

    Water_SetScene(106, 0, 0);
    waterX = 512;
    sideX = 512;
    sideZ = 192;
    sideWater = true;
    otherSideX = 512;
    otherSideZ = -192;
    otherSideWater = true;
    Water_AnimationContact(807, 4, 0, 0, 1000);
    Check(CountImpacts() == 3 && HasImpact(512.0f, 0.0f, 2.15f) &&
          HasImpact(512.0f, 192.0f, 1.05f) && HasImpact(512.0f, -192.0f, 0.85f),
          "both coded side points create three distinct-strength contacts");

    Water_SetScene(103, 0, 0);
    waterX = 99999;
    sideWater = false;
    otherSideWater = false;
    Water_AnimationContact(807, 4, 0, 0, 1000);
    Check(CountImpacts() == 0, "no coded water emits no contact");

    return failures ? 1 : 0;
}
