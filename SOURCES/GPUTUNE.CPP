#include "GPUTUNE.H"

#include "C_EXTERN.H"
#include "COMMON.H"
#include "3DEXT/TERRAIN_GPU.H"
#include <SVGA/GPUOBJ.H>
#include <SVGA/GPUSKY.H>
#include <SVGA/GPUSTORM.H>
#include <SVGA/GPUWATER.H>

#include <SDL3/SDL.h>
#include <stdlib.h>

extern void WriteConfigFile(void);

S32 GpuAutoTuned = FALSE;

/* A frame may take this long and still hold 60 frames a second (with a little
   slack for the timer): above it, the next effect is switched off. */
#define TUNE_BUDGET_MS 17.5
/* Frames skipped after a change (buffers rebuilt, cubes reloaded), then measured. */
#define TUNE_SETTLE 30
#define TUNE_SAMPLES 90

/* The steps down, from the costliest for the least seen. */
typedef enum {
    STEP_VIEW_VERY_FAR, ///< very far view down to far
    STEP_RAYTRACE,
    STEP_VIEW_FAR,      ///< far view down to classic
    STEP_SMOOTH_TERRAIN,
    STEP_SHADOWS,
    STEP_WATER,
    STEP_SUNLIGHT,
    STEP_COUNT
} T_TUNE_STEP;

static S32 s_running = FALSE;
static S32 s_step = 0;       ///< next step to take if the frame is too slow
static S32 s_frame = 0;      ///< frames since the last change
static double s_samples[TUNE_SAMPLES];
static U64 s_last = 0;

static const char *const kStepNames[STEP_COUNT] = {"view distance: far", "ray tracing: off", "view distance: classic",
                                                    "smooth terrain: off", "soft shadows: off",
                                                    "modern water: off", "global light: off"};

static void TakeStep(S32 step) {
    switch (step) {
    case STEP_VIEW_VERY_FAR:
        TerrainGpuViewDistance = 1;
        break;
    case STEP_RAYTRACE:
        GpuObjRayTrace = FALSE;
        break;
    case STEP_VIEW_FAR:
        TerrainGpuViewDistance = 0;
        break;
    case STEP_SMOOTH_TERRAIN:
        TerrainGpuSmooth = FALSE;
        break;
    case STEP_SHADOWS:
        GpuObjShadows = FALSE;
        break;
    case STEP_WATER:
        GpuWaterEnabled = FALSE;
        break;
    default:
        GpuObjSunlight = FALSE;
        break;
    }
}

static int CompareDouble(const void *a, const void *b) {
    const double x = *(const double *)a, y = *(const double *)b;
    return x < y ? -1 : (x > y ? 1 : 0);
}

static void Finish(const char *why) {
    s_running = FALSE;
    GpuAutoTuned = TRUE;
    SDL_Log("[GPU] auto-adjust: %s", why);
    WriteConfigFile();
}

void GpuTune_Start(void) {
    /* Every effect on, the farthest view: the steps take them back down. */
    TerrainGpuViewDistance = 2;
    GpuObjRayTrace = TRUE;
    TerrainGpuSmooth = TRUE;
    GpuObjShadows = TRUE;
    GpuWaterEnabled = TRUE;
    GpuObjSunlight = TRUE;
    GpuObjLightsEnabled = TRUE;
    GpuSkyEnabled = TRUE;
    GpuStormEnabled = TRUE;
    s_step = 0;
    s_frame = 0;
    s_last = 0;
    s_running = TRUE;
    SDL_Log("[GPU] auto-adjust: measuring");
}

S32 GpuTune_Running(void) {
    return s_running;
}

void GpuTune_Frame(void) {
    const U64 now = SDL_GetPerformanceCounter();
    const double ms = s_last != 0 ? (double)(now - s_last) * 1000.0 / (double)SDL_GetPerformanceFrequency() : 0.0;
    s_last = now;

    /* Measured in an exterior with the GPU renderer, where the effects cost. */
    if (!GpuObjEnabled || !GpuObjAvailable || CubeMode != CUBE_EXTERIEUR) {
        s_frame = 0;
        return;
    }
    if (!s_running) {
        if (GpuAutoTuned) {
            return;
        }
        GpuTune_Start();
    }
    if (ms <= 0.0 || ms > 1000.0) {
        /* A pause (menu, loading): start this measure over. */
        s_frame = 0;
        return;
    }
    s_frame++;
    if (s_frame <= TUNE_SETTLE) {
        return;
    }
    s_samples[s_frame - TUNE_SETTLE - 1] = ms;
    if (s_frame < TUNE_SETTLE + TUNE_SAMPLES) {
        return;
    }
    /* The median: a stutter while measuring does not count. */
    qsort(s_samples, TUNE_SAMPLES, sizeof(double), CompareDouble);
    const double median = s_samples[TUNE_SAMPLES / 2];
    s_frame = 0;
    if (median <= TUNE_BUDGET_MS) {
        char why[96];
        SDL_snprintf(why, sizeof(why), "%.1f ms a frame, done after %d step(s)", median, (int)s_step);
        Finish(why);
        return;
    }
    if (s_step >= STEP_COUNT) {
        Finish("every costly effect is off");
        return;
    }
    SDL_Log("[GPU] auto-adjust: %.1f ms a frame, %s", median, kStepNames[s_step]);
    TakeStep(s_step++);
}
