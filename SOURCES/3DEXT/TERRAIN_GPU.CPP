#include "TERRAIN_GPU.H"

#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/PROJ.H>
#include <OBJECT/AFF_GPU.H>
#include <POLYGON/POLY.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>

#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

#define GRID_SIDE 65
#define CELL_SIZE 512

// --- Private state -----------------------------------------------------------
static T_GPUOBJ_DRAW *s_draw = NULL;

/* Texel addressing of the plane being drawn (sea, sky). */
static float s_planeTexOffset = 0.0f;
static float s_planeTexMask = 65535.0f;

/* Camera-space position of every vertex of the cube being drawn. */
static float s_grid[GRID_SIDE * GRID_SIDE][3];

// --- Private functions -------------------------------------------------------
static void OpenDraw(U8 *page, U8 *clut, S32 fogStart, S32 fogEnd, S32 fogColor) {
    s_draw = GpuObj_BeginDraw();
    if (s_draw == NULL) {
        return;
    }
    s_draw->X0 = ClipXMin;
    s_draw->Y0 = ClipYMin;
    s_draw->X1 = ClipXMax;
    s_draw->Y1 = ClipYMax;
    s_draw->ZBuffer = TRUE;
    s_draw->Page = page;
    s_draw->Clut = clut;
    s_draw->FogStart = fogStart;
    s_draw->FogEnd = fogEnd;
    s_draw->FogColor = fogColor;
}

// --- Interface ---------------------------------------------------------------
void TerrainGpu_BeginCube(const S16 *heights, U8 *groundTexture, U8 *clutBase, S32 fogStart, S32 fogEnd,
                          S32 fogColor) {
    s_draw = NULL;
    if (!GpuObj_Active() || heights == NULL || TypeProj != TYPE_3D) {
        return;
    }
    OpenDraw(groundTexture, clutBase, fogStart, fogEnd, fogColor);
    if (s_draw == NULL) {
        return;
    }

    /* LongWorldRotatePoint then LongProjectPoint's camera offsets, in float. */
    const TYPE_MAT *m = &MatriceWorld;
    for (S32 zi = 0; zi < GRID_SIDE; zi++) {
        for (S32 xi = 0; xi < GRID_SIDE; xi++) {
            const float x = (float)(xi * CELL_SIZE);
            const float y = (float)heights[zi * GRID_SIDE + xi];
            const float z = (float)(zi * CELL_SIZE);
            float *out = s_grid[zi * GRID_SIDE + xi];
            out[0] = x * m->F.M11 + y * m->F.M12 + z * m->F.M13 - (float)CameraXr;
            out[1] = x * m->F.M21 + y * m->F.M22 + z * m->F.M23 - (float)CameraYr;
            out[2] = (float)CameraZr - (x * m->F.M31 + y * m->F.M32 + z * m->F.M33);
        }
    }
}

void TerrainGpu_Tri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], S32 fillType, S32 color,
                    const U16 *uv) {
    if (s_draw == NULL) {
        return;
    }

    float mode;
    S32 flags = GPUOBJ_FLAG_BAKED;
    S32 perVertexShade = FALSE;
    float constantShade = 0.0f;

    switch (fillType) {
    case POLY_SOLID:
        mode = (float)GPUOBJ_MODE_SOLID;
        break;
    case POLY_GOURAUD_TABLE:
    case POLY_DITHER_TABLE:
        mode = (float)GPUOBJ_MODE_CLUT;
        perVertexShade = TRUE;
        break;
    case POLY_TEXTURE:
    case POLY_TEXTURE_INCRUST:
        mode = (float)GPUOBJ_MODE_TEX;
        break;
    case POLY_TEXTURE_FLAT:
    case POLY_TEXTURE_INCRUST_FLAT:
        mode = (float)GPUOBJ_MODE_TEXSHADED;
        constantShade = (float)(color & 0xFF);
        break;
    case POLY_TEXTURE_GOURAUD:
    case POLY_TEXTURE_INCRUST_GOURAUD:
        mode = (float)GPUOBJ_MODE_TEXSHADED;
        perVertexShade = TRUE;
        break;
    default:
        /* Anything else keeps the software colour. */
        mode = (float)GPUOBJ_MODE_PASS;
        break;
    }
    if (fillType >= POLY_TEXTURE_INCRUST && fillType <= POLY_TEXTURE_INCRUST_DITHER) {
        flags |= GPUOBJ_FLAG_CHROMAKEY;
    }

    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(3);
    if (v == NULL) {
        return;
    }
    static const S32 kCornerX[4] = {0, 0, 1, 1};
    static const S32 kCornerZ[4] = {0, 1, 1, 0};
    for (S32 i = 0; i < 3; i++) {
        const S32 gx = xi + kCornerX[corners[i] & 3];
        const S32 gz = zi + kCornerZ[corners[i] & 3];
        const float *p = s_grid[gz * GRID_SIDE + gx];
        AffGpu_ViewVertex(&v[i], p[0], p[1], p[2]);
        v[i].normal[3] = perVertexShade ? (float)lights[i] / 256.0f : constantShade;
        v[i].light[3] = mode;
        v[i].vpos[3] = (float)flags;
        v[i].mat[0] = (float)(color & 0xFF);
        v[i].mat[1] = 0.0f;
        v[i].mat[2] = 65535.0f;
        if (uv != NULL) {
            v[i].uv[0] = (float)uv[i * 2] / 256.0f;
            v[i].uv[1] = (float)uv[i * 2 + 1] / 256.0f;
        }
    }
}

void TerrainGpu_BeginPlane(U8 *page, S32 texOffset, S32 texMask, S32 fogStart, S32 fogEnd, S32 fogColor) {
    s_draw = NULL;
    if (!GpuObj_Active() || TypeProj != TYPE_3D) {
        return;
    }
    OpenDraw(page, NULL, fogStart, fogEnd, fogColor);
    s_planeTexOffset = (float)texOffset;
    s_planeTexMask = (float)(texMask & 0xFFFF);
}

void TerrainGpu_Quad(const STRUC_CLIPVERTEX src[4]) {
    if (s_draw == NULL) {
        return;
    }
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v == NULL) {
        return;
    }
    static const S32 order[6] = {0, 1, 2, 0, 2, 3};
    for (S32 i = 0; i < 6; i++) {
        const STRUC_CLIPVERTEX *p = &src[order[i]];
        AffGpu_ViewVertex(&v[i], (float)p->V_X0, (float)p->V_Y0, (float)p->V_Z0);
        v[i].light[3] = (float)GPUOBJ_MODE_TEX;
        v[i].vpos[3] = (float)GPUOBJ_FLAG_BAKED;
        v[i].mat[1] = s_planeTexOffset;
        v[i].mat[2] = s_planeTexMask;
        v[i].uv[0] = (float)p->V_MapU / 256.0f;
        v[i].uv[1] = (float)p->V_MapV / 256.0f;
    }
}

void TerrainGpu_End(void) {
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
