#include "GRILLE_GPU.H"

#include <3D/CAMERA.H>
#include <3D/PROJ.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>
#include <SVGA/SCREEN.H>

#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

/* Atlas cells hold one brick sprite each (bricks are 48x38). */
#define CELL_W 50
#define CELL_H 40
#define CELLS_X (GPUOBJ_ATLAS_SIZE / CELL_W)
#define CELLS_Y (GPUOBJ_ATLAS_SIZE / CELL_H)
#define MAX_SLOTS (CELLS_X * CELLS_Y)

#define BRICK_XZ 512.0f
#define BRICK_Y 256.0f

// --- Private state -----------------------------------------------------------
static T_GPUOBJ_DRAW *s_draw = NULL;
static U8 s_slotState[MAX_SLOTS]; ///< 0 unknown, 1 in atlas, 2 unusable
static S32 s_atlasValid = FALSE;

/* --- Lamps -------------------------------------------------------------------
   The game data has no light sources, so the bricks that are lamps are listed
   here by their number in BKG.HQR, found by looking at every room that uses
   them. The light's colour comes from the lamp's own brightest pixels in the
   room's palette. */
typedef struct {
    U16 Brick;
    U8 Radius;    ///< in quarter cells (128 world units)
    U8 Intensity; ///< in hundredths
} T_LAMP_BRICK;

static const T_LAMP_BRICK kLampBricks[] = {
    {89, 16, 55},    /* wall sconce shade (Twinsen's house, Citadel) */
    {90, 20, 70},    /* table lamp shade */
    {337, 20, 70},   /* floor lamp shade */
    {1816, 20, 70},  /* lamp shade */
    {4342, 20, 70},  /* hanging brass lamp (tavern) */
    {6903, 20, 70},  /* stacked globe lamp */
    {6958, 20, 70},  /* hanging lamp */
    {7100, 20, 70},  /* glowing ceiling fixture */
    {7699, 20, 70},  /* stacked globe lamp */
    {7747, 20, 70},  /* stacked globe lamp */
    {9487, 24, 85},  /* crystal lamp */
    {9490, 24, 85},  /* crystal lamp */
    {12761, 20, 70}, /* globe lamp */
    {13289, 20, 70}, /* globe lamp */
    {13489, 16, 55}, /* wall globe */
    {13496, 16, 55}, /* wall globe */
    {14902, 20, 70}, /* glowing wall fixture */
    {15901, 10, 40}, /* candle */
};
#define NB_LAMP_BRICKS ((S32)(sizeof(kLampBricks) / sizeof(kLampBricks[0])))

static U8 s_lampOf[MAX_SLOTS]; ///< local sprite -> kLampBricks index + 1, 0 none

typedef struct {
    S32 X, Y, Z;
    S32 Type;
} T_LAMP;

#define MAX_LAMPS 64
static T_LAMP s_lamps[MAX_LAMPS];
static S32 s_nbLamps = 0;
static const U8 *s_palette = NULL;
static float s_lampColor[MAX_SLOTS][3];
static U8 s_lampColorValid[MAX_SLOTS];

// --- Private functions -------------------------------------------------------
/* Decodes sprite brick of bank (the GPH run format AffGraph reads) into its
   atlas cell as (palette index, coverage) pairs. */
static S32 DecodeBrick(S32 brick, const U8 *bank) {
    const U8 *data = bank + ((const U32 *)bank)[brick];
    const S32 w = data[0];
    const S32 h = data[1];
    if (w > CELL_W || h > CELL_H) {
        return FALSE;
    }

    U8 *atlas = GpuObjAtlas;
    const S32 cellX = (brick % CELLS_X) * CELL_W;
    const S32 cellY = (brick / CELLS_X) * CELL_H;
    const U8 *p = data + 4;

    for (S32 y = 0; y < h; y++) {
        U8 *row = atlas + ((size_t)(cellY + y) * GPUOBJ_ATLAS_SIZE + (size_t)cellX) * 2;
        S32 x = 0;
        S32 blocks = *p++;
        for (S32 b = 0; b < blocks; b++) {
            const U8 control = *p;
            const S32 op = control >> 6;
            const S32 count = (control & 0x3F) + 1;
            if (op == 0) {
                /* AffGraph skips counter + 1 transparent pixels. */
                p++;
                x += count;
            } else if (op == 1) {
                p++;
                for (S32 i = 0; i < count; i++, x++) {
                    if (x < w) {
                        row[x * 2] = *p;
                        row[x * 2 + 1] = 255;
                    }
                    p++;
                }
            } else {
                const U8 color = p[1];
                p += 2;
                for (S32 i = 0; i < count; i++, x++) {
                    if (x < w) {
                        row[x * 2] = color;
                        row[x * 2 + 1] = 255;
                    }
                }
            }
        }
    }
    GpuObjAtlasDirty = TRUE;
    return TRUE;
}

/* The average of the lamp sprite's brightest pixels, scaled to full brightness. */
static void LampColor(S32 brick, float *rgb) {
    rgb[0] = 1.0f;
    rgb[1] = 0.8f;
    rgb[2] = 0.55f;
    if (s_slotState[brick] != 1 || s_palette == NULL) {
        return;
    }
    const U8 *atlas = GpuObjAtlas;
    const S32 cellX = (brick % CELLS_X) * CELL_W;
    const S32 cellY = (brick / CELLS_X) * CELL_H;
    S32 histogram[256];
    memset(histogram, 0, sizeof(histogram));
    for (S32 y = 0; y < CELL_H; y++) {
        const U8 *row = atlas + ((size_t)(cellY + y) * GPUOBJ_ATLAS_SIZE + (size_t)cellX) * 2;
        for (S32 x = 0; x < CELL_W; x++) {
            if (row[x * 2 + 1] != 0) {
                const U8 *c = s_palette + row[x * 2] * 3;
                histogram[(c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8]++;
            }
        }
    }
    /* Luma threshold keeping the brightest fifth of the sprite. */
    S32 total = 0;
    for (S32 l = 0; l < 256; l++) {
        total += histogram[l];
    }
    S32 threshold = 255;
    for (S32 kept = 0; threshold > 0 && kept * 5 < total; threshold--) {
        kept += histogram[threshold];
    }
    float sum[3] = {0.0f, 0.0f, 0.0f};
    for (S32 y = 0; y < CELL_H; y++) {
        const U8 *row = atlas + ((size_t)(cellY + y) * GPUOBJ_ATLAS_SIZE + (size_t)cellX) * 2;
        for (S32 x = 0; x < CELL_W; x++) {
            if (row[x * 2 + 1] != 0) {
                const U8 *c = s_palette + row[x * 2] * 3;
                if (((c[0] * 77 + c[1] * 150 + c[2] * 29) >> 8) > threshold) {
                    sum[0] += c[0];
                    sum[1] += c[1];
                    sum[2] += c[2];
                }
            }
        }
    }
    const float peak = sum[0] > sum[1] ? (sum[0] > sum[2] ? sum[0] : sum[2]) : (sum[1] > sum[2] ? sum[1] : sum[2]);
    if (peak > 0.0f) {
        /* Towards white: a lamp lights with its glow, not its lampshade's dye. */
        for (S32 i = 0; i < 3; i++) {
            rgb[i] = 0.35f + 0.65f * sum[i] / peak;
        }
    }
}

/* One lamp brick drawn in cell (x, y, z). Lamps are several bricks stacked or
   side by side: one light per lamp. */
static void AddLamp(S32 brick, S32 x, S32 y, S32 z) {
    for (S32 k = 0; k < s_nbLamps; k++) {
        T_LAMP *l = &s_lamps[k];
        if (l->X - x <= 1 && x - l->X <= 1 && l->Z - z <= 1 && z - l->Z <= 1 && l->Y - y <= 2 && y - l->Y <= 2) {
            return;
        }
    }
    if (s_nbLamps >= MAX_LAMPS) {
        return;
    }
    if (!s_lampColorValid[brick]) {
        LampColor(brick, s_lampColor[brick]);
        s_lampColorValid[brick] = TRUE;
    }
    s_lamps[s_nbLamps].X = x;
    s_lamps[s_nbLamps].Y = y;
    s_lamps[s_nbLamps].Z = z;
    s_lamps[s_nbLamps].Type = brick;
    s_nbLamps++;
}

// --- Interface ---------------------------------------------------------------
void GrilleGpu_MapBrick(S32 local, S32 global) {
    if (local == 0) {
        memset(s_lampOf, 0, sizeof(s_lampOf));
    }
    if (local < 0 || local >= MAX_SLOTS) {
        return;
    }
    for (S32 i = 0; i < NB_LAMP_BRICKS; i++) {
        if (kLampBricks[i].Brick == global) {
            s_lampOf[local] = (U8)(i + 1);
            break;
        }
    }
}

void GrilleGpu_Reset(void) {
    memset(s_slotState, 0, sizeof(s_slotState));
    memset(s_lampColorValid, 0, sizeof(s_lampColorValid));
    s_nbLamps = 0;
    s_atlasValid = FALSE;
    if (GpuObj_Active()) {
        s_atlasValid = GpuObj_AtlasReset() != NULL;
    }
}

void GrilleGpu_Begin(const U8 *palette) {
    s_draw = NULL;
    s_palette = palette;
    s_nbLamps = 0;
    if (!GpuObj_Active() || TypeProj == TYPE_3D) {
        return;
    }
    if (!s_atlasValid) {
        /* The layer came on after the room was loaded. */
        GrilleGpu_Reset();
        if (!s_atlasValid) {
            return;
        }
    }
    s_draw = GpuObj_BeginDraw();
    if (s_draw == NULL) {
        return;
    }
    s_draw->X0 = ClipXMin;
    s_draw->Y0 = ClipYMin;
    s_draw->X1 = ClipXMax;
    s_draw->Y1 = ClipYMax;
    s_draw->ZBuffer = TRUE;
    /* Brick sprites overlap by design and the last drawn wins, as in software;
       their depth only has to hold actors back. */
    s_draw->Painter = TRUE;
}

void GrilleGpu_Brick(S32 brick, S32 x, S32 y, S32 z, S32 xs, S32 ys, const U8 *bank) {
    if (s_draw == NULL || brick < 0 || brick >= MAX_SLOTS) {
        return;
    }
    if (s_slotState[brick] == 0) {
        s_slotState[brick] = DecodeBrick(brick, bank) ? 1 : 2;
    }
    if (s_lampOf[brick] != 0) {
        AddLamp(brick, x, y, z);
    }

    const U8 *data = bank + ((const U32 *)bank)[brick];
    const S32 w = data[0];
    const S32 h = data[1];
    const float x0 = (float)(xs + (S8)data[2]);
    const float y0 = (float)(ys + (S8)data[3]);

    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v == NULL) {
        return;
    }

    /* Unusable sprites still cover their pixels, as software-coloured pass geometry. */
    const float mode = (float)(s_slotState[brick] == 1 ? GPUOBJ_MODE_BRICK : GPUOBJ_MODE_PASS);
    const float atlasX = (float)((brick % CELLS_X) * CELL_W);
    const float atlasY = (float)((brick / CELLS_X) * CELL_H);
    const float boxX = (float)x * BRICK_XZ - (float)CameraX;
    const float boxY = (float)y * BRICK_Y - (float)CameraY;
    const float boxZ = (float)z * BRICK_XZ - (float)CameraZ;
    const float sw = 2.0f / (float)ModeDesiredX;
    const float sh = 2.0f / (float)ModeDesiredY;

    static const S32 order[6] = {0, 1, 2, 0, 2, 3};
    static const float cornerX[4] = {0.0f, 1.0f, 1.0f, 0.0f};
    static const float cornerY[4] = {0.0f, 0.0f, 1.0f, 1.0f};

    for (S32 i = 0; i < 6; i++) {
        const S32 k = order[i];
        const float px = x0 + cornerX[k] * (float)w;
        const float py = y0 + cornerY[k] * (float)h;
        memset(&v[i], 0, sizeof(v[i]));
        v[i].clip[0] = px * sw - 1.0f;
        v[i].clip[1] = 1.0f - py * sh;
        v[i].clip[2] = 0.5f; /* the fragment writes the real depth */
        v[i].clip[3] = 1.0f;
        v[i].normal[0] = px; /* frame position, for the depth ray */
        v[i].normal[1] = py;
        v[i].light[3] = mode;
        v[i].vpos[0] = boxX;
        v[i].vpos[1] = boxY;
        v[i].vpos[2] = boxZ;
        v[i].vpos[3] = (float)(GPUOBJ_FLAG_ISO | GPUOBJ_FLAG_BAKED);
        v[i].mat[0] = (float)XCentre;
        v[i].mat[1] = (float)YCentre;
        v[i].uv[0] = atlasX + cornerX[k] * (float)w;
        v[i].uv[1] = atlasY + cornerY[k] * (float)h;
    }
}

void GrilleGpu_AddRoomLights(void) {
    if (!GpuObj_Active() || s_nbLamps == 0) {
        return;
    }
    typedef struct {
        float X, Y, Z, Dist;
        S32 Lamp;
    } T_NEAR;
    T_NEAR nearList[MAX_LAMPS];
    for (S32 k = 0; k < s_nbLamps; k++) {
        /* Camera-relative cell centre: the space bricks and iso bodies use. */
        nearList[k].X = ((float)s_lamps[k].X + 0.5f) * BRICK_XZ - (float)CameraX;
        nearList[k].Y = ((float)s_lamps[k].Y + 0.5f) * BRICK_Y - (float)CameraY;
        nearList[k].Z = ((float)s_lamps[k].Z + 0.5f) * BRICK_XZ - (float)CameraZ;
        nearList[k].Dist = nearList[k].X * nearList[k].X + nearList[k].Z * nearList[k].Z;
        nearList[k].Lamp = k;
    }
    /* Leave room for the lights that move (magic, the hero's). */
    for (S32 n = 0; n < s_nbLamps && GpuObjNbLights < GPUOBJ_MAX_LIGHTS - 2; n++) {
        S32 best = n;
        for (S32 k = n + 1; k < s_nbLamps; k++) {
            if (nearList[k].Dist < nearList[best].Dist) {
                best = k;
            }
        }
        const T_NEAR tmp = nearList[n];
        nearList[n] = nearList[best];
        nearList[best] = tmp;

        const S32 brick = s_lamps[nearList[n].Lamp].Type;
        const T_LAMP_BRICK *def = &kLampBricks[s_lampOf[brick] - 1];
        const float *c = s_lampColor[brick];
        GpuObj_AddLight(nearList[n].X, nearList[n].Y, nearList[n].Z, (float)def->Radius * 128.0f, c[0], c[1], c[2],
                        (float)def->Intensity / 100.0f);
    }
}

void GrilleGpu_End(void) {
    if (s_draw == NULL) {
        return;
    }
    s_draw = NULL;
    GpuObj_EndDraw();
}

// =============================================================================
#ifdef __cplusplus
}
#endif
