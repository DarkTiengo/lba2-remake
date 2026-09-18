#include <SVGA/GPUSTORM.H>

#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>

#include <math.h>

S32 GpuStormEnabled = TRUE;

static S32 s_time = 0;        ///< game ms of the last scene frame
static S32 s_raining = FALSE; ///< rain in view
static S32 s_exterior = FALSE;
static S32 s_eclairFrame = -1;   ///< game ms a scripted lightning was last seen
static S32 s_strikeTime = -100000; ///< game ms of the last strike
static float s_strikeSeed = 0.0f;
static float s_strikeAz = 0.0f;       ///< bolt azimuth off the view, radians
static float s_strikeStrength = 1.0f;
static S32 s_nextScripted = 0;
static S32 s_nextAmbient = -1;
static U32 s_rng = 0x2545F491u;
static S32 s_window[4] = {0, 0, 0, 0}; ///< the scene's clip window, frame pixels

/* A generator of its own: strikes never touch the game's random sequence. */
static float Rand01(void) {
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) / 16777216.0f;
}

static void Strike(S32 now, float strength) {
    s_strikeTime = now;
    s_strikeSeed = Rand01() * 97.0f;
    s_strikeAz = (Rand01() * 2.0f - 1.0f) * 0.55f;
    s_strikeStrength = strength;
}

S32 GpuStorm_Modern(void) {
    return GpuStormEnabled && GpuObjEnabled && GpuObjAvailable;
}

static S32 s_forced = FALSE;

void GpuStorm_Strike(void) {
    s_forced = TRUE;
}

void GpuStorm_Eclair(void) {
    s_eclairFrame = s_time;
}

void GpuStorm_SetScene(S32 timeMs, S32 raining, S32 exterior) {
    /* A scripted lightning seen last frame: strike now and then while it lasts. */
    const S32 scripted = s_eclairFrame >= 0 && s_time - s_eclairFrame <= 100;
    if (timeMs < s_time) {
        /* A new game or a loaded save rewinds the clock. */
        s_strikeTime = -100000;
        s_nextScripted = 0;
        s_nextAmbient = -1;
        s_eclairFrame = -1;
    }
    s_time = timeMs;
    s_window[0] = ClipWindowXMin;
    s_window[1] = ClipWindowYMin;
    s_window[2] = ClipWindowXMax;
    s_window[3] = ClipWindowYMax;
    s_raining = raining;
    s_exterior = exterior;
    if (!GpuStorm_Modern()) {
        return;
    }
    if (s_forced) {
        s_forced = FALSE;
        Strike(timeMs, 1.0f);
    }
    if (scripted && timeMs >= s_nextScripted) {
        Strike(timeMs, 0.85f + 0.15f * Rand01());
        s_nextScripted = timeMs + 700 + (S32)(Rand01() * 1600.0f);
    }
    if (raining && exterior) {
        if (s_nextAmbient < 0) {
            s_nextAmbient = timeMs + 2500 + (S32)(Rand01() * 4000.0f);
        } else if (timeMs >= s_nextAmbient) {
            Strike(timeMs, 0.55f + 0.45f * Rand01());
            s_nextAmbient = timeMs + 6000 + (S32)(Rand01() * 10000.0f);
        }
    } else {
        s_nextAmbient = -1;
    }
}

float GpuStorm_FlashCurve(float age) {
    if (age < 0.0f || age > 1.2f) {
        return 0.0f;
    }
    /* A sharp stroke, the return strokes that make lightning flicker, a glow. */
    const float a = (age - 0.09f) / 0.025f;
    const float b = (age - 0.22f) / 0.03f;
    return expf(-age * 11.0f) + 0.65f * expf(-a * a) + 0.45f * expf(-b * b) + 0.12f * expf(-age * 3.0f);
}

static float Age(void) {
    return (float)(s_time - s_strikeTime) / 1000.0f;
}

S32 GpuStorm_Active(void) {
    return GpuStorm_Modern() && (s_raining || GpuStorm_FlashCurve(Age()) > 0.01f);
}

static void Normalize(float *v) {
    const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) {
        v[0] /= l;
        v[1] /= l;
        v[2] /= l;
    }
}

void GpuStorm_GetUniforms(float *out, float frameW, float frameH) {
    for (int i = 0; i < GPUSTORM_UNIFORM_FLOATS; i++) {
        out[i] = 0.0f;
    }
    if (!GpuStorm_Modern()) {
        return;
    }
    const float age = Age();
    const float flash = GpuStorm_FlashCurve(age) * s_strikeStrength;
    const float *up = GpuObjUpDir;

    /* Toward the strike: off the view's forward, above the horizon. */
    float fwd[3] = {0.0f, 0.0f, -1.0f};
    if (!s_exterior) {
        /* A room's light through its windows, from above the view. */
        fwd[0] = -0.6f;
        fwd[1] = 0.0f;
        fwd[2] = -0.6f;
    }
    const float fu = fwd[0] * up[0] + fwd[1] * up[1] + fwd[2] * up[2];
    float ground[3] = {fwd[0] - up[0] * fu, fwd[1] - up[1] * fu, fwd[2] - up[2] * fu};
    Normalize(ground);
    float side[3] = {up[1] * ground[2] - up[2] * ground[1], up[2] * ground[0] - up[0] * ground[2],
                     up[0] * ground[1] - up[1] * ground[0]};
    Normalize(side);
    const float el = 0.62f;
    const float ca = cosf(s_strikeAz), sa = sinf(s_strikeAz), ce = cosf(el), se = sinf(el);
    float toward[3];
    for (int k = 0; k < 3; k++) {
        toward[k] = ground[k] * ce * ca + side[k] * ce * sa + up[k] * se;
    }
    Normalize(toward);

    /* Where the bolt stands on screen and where the horizon is (3D only). */
    float boltX = 0.5f, horizon = 1.0f, visible = 0.0f;
    if (s_exterior && frameW > 0.0f && frameH > 0.0f) {
        float dir[3] = {ground[0] * ca + side[0] * sa, ground[1] * ca + side[1] * sa, ground[2] * ca + side[2] * sa};
        if (-dir[2] > 0.05f) {
            boltX = (GpuObjProj[0] + dir[0] * GpuObjProj[2] / -dir[2]) / frameW;
            horizon = (GpuObjProj[1] + dir[1] * GpuObjProj[2] * GpuObjProj[3] / -dir[2]) / frameH;
            /* The visible stroke: the first return strokes. */
            visible = age >= 0.0f && age < 0.34f ? 1.0f : 0.0f;
        }
    }

    out[0] = s_raining && s_exterior ? 1.0f : 0.0f;
    out[1] = (float)fmod((double)s_time / 1000.0, 1000.0);
    out[2] = 1.0f;
    out[3] = flash;
    out[4] = boltX;
    out[5] = horizon;
    out[6] = s_strikeSeed;
    out[7] = visible * s_strikeStrength;
    out[8] = toward[0];
    out[9] = toward[1];
    out[10] = toward[2];
    out[11] = s_exterior ? 1.0f : 0.0f;
    out[12] = up[0];
    out[13] = up[1];
    out[14] = up[2];
    out[15] = age;
    if (frameW > 0.0f && frameH > 0.0f) {
        out[16] = (float)s_window[0] / frameW;
        out[17] = (float)s_window[1] / frameH;
        out[18] = (float)(s_window[2] + 1) / frameW;
        out[19] = (float)(s_window[3] + 1) / frameH;
    }
}
