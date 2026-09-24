#include "SPRITE_GPU.H"

#include "C_EXTERN.H"

#include <3D/CAMERA.H>
#include <3D/PROJ.H>
#include <OBJECT/AFF_GPU.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>

#include <string.h>

S32 GpuObjSprites = TRUE;

/* The atlas holds one sprite a cell. A cell is generous for an extra (the
   largest in the game are a few dozen pixels across) and a sprite that does not
   fit is left to the software, which draws it as it always has. */
#define CELL 128
#define CELLS_X (GPUOBJ_ATLAS_SIZE / CELL)
#define MAX_SLOTS (CELLS_X * CELLS_X)
#define MAX_SPRITE_NUM 2048

/* Sprite header, as SCALESPI.CPP reads it. */
typedef struct {
    U8 DeltaX;
    U8 DeltaY;
    S8 HotX;
    S8 HotY;
} T_SPRITE_HEAD;

static S16 s_slotOf[MAX_SPRITE_NUM]; ///< -1 unseen, -2 refused, else the cell
static S32 s_nbSlots = 0;
static S32 s_atlasValid = FALSE;
static S32 s_mapped = FALSE; ///< s_slotOf holds -1s rather than the zeros it starts life with

/* The extra being drawn, between Begin and End. */
static T_GPUOBJ_DRAW *s_draw = NULL;
static S32 s_slot = 0;
static float s_x0 = 0.0f, s_y0 = 0.0f, s_x1 = 0.0f, s_y1 = 0.0f; ///< screen rect
static float s_w = 0.0f, s_h = 0.0f;                             ///< sprite size in texels

void SpriteGpu_Reset(void) {
    for (S32 i = 0; i < MAX_SPRITE_NUM; i++) {
        s_slotOf[i] = -1;
    }
    s_mapped = TRUE;
    s_nbSlots = 0;
    s_draw = NULL;
    s_atlasValid = FALSE;
}

/* The sprite's pixels into its cell: index and coverage, as the bricks store
   them. Raw sprites are a row of indices with 0 for transparent. */
static S32 DecodeSprite(S32 slot, const T_SPRITE_HEAD *head) {
    const S32 w = head->DeltaX;
    const S32 h = head->DeltaY;
    if (w <= 0 || h <= 0 || w > CELL || h > CELL) {
        return FALSE;
    }
    U8 *atlas = GpuObjAtlas;
    if (atlas == NULL) {
        return FALSE;
    }
    const S32 cellX = (slot % CELLS_X) * CELL;
    const S32 cellY = (slot / CELLS_X) * CELL;
    const U8 *p = (const U8 *)head + sizeof(T_SPRITE_HEAD);

    for (S32 y = 0; y < h; y++) {
        U8 *row = atlas + ((size_t)(cellY + y) * GPUOBJ_ATLAS_SIZE + (size_t)cellX) * 2;
        for (S32 x = 0; x < w; x++) {
            const U8 index = *p++;
            row[x * 2] = index;
            row[x * 2 + 1] = index != 0 ? 255 : 0;
        }
    }
    GpuObjAtlasDirty = TRUE;
    return TRUE;
}

/* mul_shift16, as ScaleSprite scales a sprite's size and hot spot. */
static S32 Scaled(S32 v, S32 factor) {
    return (S32)(((S64)v * (S64)factor) >> 16);
}

S32 SpriteGpu_Begin(S32 num, const void *data, S32 spriteX, S32 spriteY, S32 scale) {
    s_draw = NULL;
    if (!GpuObjSprites || !GpuObj_Active() || TypeProj != TYPE_3D || data == NULL) {
        return FALSE;
    }
    if (num < 0 || num >= MAX_SPRITE_NUM || scale <= 0) {
        return FALSE;
    }
    if (!s_mapped) {
        SpriteGpu_Reset(); /* an untouched table is all zeros, and zero is a slot */
    }
    if (!s_atlasValid) {
        s_atlasValid = GpuObj_AtlasReset() != NULL;
        if (!s_atlasValid) {
            return FALSE;
        }
    }

    /* The bank entry starts with the offset table ScaleSprite indexes. */
    const U8 *entry = (const U8 *)data;
    const T_SPRITE_HEAD *head = (const T_SPRITE_HEAD *)(entry + ((const U32 *)entry)[0]);

    if (s_slotOf[num] == -1) {
        if (s_nbSlots >= MAX_SLOTS) {
            s_slotOf[num] = -2;
        } else {
            s_slotOf[num] = DecodeSprite(s_nbSlots, head) ? (S16)s_nbSlots++ : (S16)-2;
        }
    }
    if (s_slotOf[num] < 0) {
        return FALSE;
    }

    /* The rectangle ScaleSprite is about to fill, before it clips. */
    const S32 w = Scaled(head->DeltaX, scale);
    const S32 h = Scaled(head->DeltaY, scale);
    if (w <= 0 || h <= 0) {
        return FALSE;
    }
    s_x0 = (float)(spriteX + Scaled((S8)head->HotX, scale));
    s_y0 = (float)(spriteY + Scaled((S8)head->HotY, scale));
    s_x1 = s_x0 + (float)w;
    s_y1 = s_y0 + (float)h;
    s_w = (float)head->DeltaX;
    s_h = (float)head->DeltaY;
    s_slot = s_slotOf[num];

    s_draw = GpuObj_BeginDraw();
    if (s_draw == NULL) {
        return FALSE;
    }
    s_draw->X0 = ClipXMin;
    s_draw->Y0 = ClipYMin;
    s_draw->X1 = ClipXMax;
    s_draw->Y1 = ClipYMax;
    s_draw->ZBuffer = TRUE;

    /* The software blit that follows is what tags these pixels as this draw's. */
    GpuObj_SnapshotBegin((S32)s_x0, (S32)s_y0, (S32)s_x1, (S32)s_y1);
    return TRUE;
}

void SpriteGpu_End(S32 depth) {
    if (s_draw == NULL) {
        return;
    }
    /* FRatioY is negative: the projection's vertical ratio carries the screen's
       downward Y. The same formula the engine projects with, run backwards. */
    if (depth > NearClip && FRatioX != 0.0f && FRatioY != 0.0f) {
        T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
            if (v != NULL) {
            /* The same rectangle the software fills, put back into view space at
               the extra's depth: the size on screen is unchanged and the GPU has
               something to depth-test. */
            /* Wound for the projection's negative vertical ratio, which mirrors the
               quad: the other way round is back-facing and culled. */
            static const S32 order[6] = {0, 2, 1, 0, 3, 2};
            static const float cornerX[4] = {0.0f, 1.0f, 1.0f, 0.0f};
            static const float cornerY[4] = {0.0f, 0.0f, 1.0f, 1.0f};
            const float atlasX = (float)((s_slot % CELLS_X) * CELL);
            const float atlasY = (float)((s_slot / CELLS_X) * CELL);
            const float d = (float)depth;

            for (S32 i = 0; i < 6; i++) {
                const S32 k = order[i];
                const float sx = s_x0 + cornerX[k] * (s_x1 - s_x0);
                const float sy = s_y0 + cornerY[k] * (s_y1 - s_y0);
                const float vx = (sx - (float)XCentre) * d / FRatioX;
                const float vy = (sy - (float)YCentre) * d / (FRatioX * FRatioY);
                AffGpu_ViewVertex(&v[i], vx, vy, d);
                v[i].light[3] = (float)GPUOBJ_MODE_SPRITE;
                v[i].vpos[3] = (float)GPUOBJ_FLAG_BAKED;
                v[i].normal[3] = 15.0f; /* the software frame carries the shade */
                v[i].uv[0] = atlasX + cornerX[k] * s_w;
                v[i].uv[1] = atlasY + cornerY[k] * s_h;
            }
        }
    }
    GpuObj_EndDraw();
    s_draw = NULL;
}
