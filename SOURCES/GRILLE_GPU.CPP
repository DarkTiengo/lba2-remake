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

// --- Interface ---------------------------------------------------------------
void GrilleGpu_Reset(void) {
    memset(s_slotState, 0, sizeof(s_slotState));
    s_atlasValid = FALSE;
    if (GpuObj_Active()) {
        s_atlasValid = GpuObj_AtlasReset() != NULL;
    }
}

void GrilleGpu_Begin(void) {
    s_draw = NULL;
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
