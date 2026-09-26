#include "DEFINES.H"
#include "../PERSO.H"
#include "../C_EXTERN.H" /* PtrPal */
#include "../FOLLOWCAM.H"  /* FollowCamera: the free camera reaches the near plane */

#include <SVGA/SCREEN.H>
#include <SVGA/GPUOBJ.H> /* GpuObj_RefreshTags */
#include "TERRAIN_GPU.H"
#include <SVGA/GPUSKY.H> /* GpuSky_Modern */

#define LIMITE_LOIN (60 * 1024)
#define COLOR_LIMITE 255
#define SIZE_TEXTURE 64

#define WORLD_SIZE 32768
#define NB_POLYS_COTE 4

// Taille 2D mini pour accepter un poly animé
#define ANI_POLY_STEP_Y 2
#define ANI_POLY_STEP_X 5

#include "DRAWSKY.CPP"

S32 Sky_Y;
//S32	NbDrawnPolys ;

S32 FlagMasque = TRUE;

#if defined(DEBUG_TOOLS) && defined(LBA_GAME)
extern S32 DebugFps;
#endif

#ifndef LBA_GAME
S32 Reglage = 0;
#endif

#ifdef LBA_EDITOR // Special PERSO.EXE
#define INFO_DECORS 1024
extern S32 FlagInfos;
#endif

//****************************************************************************
//****************************************************************************

S32 ListTypePolygone[4] = {0, POLY_SOLID, POLY_GOURAUD_TABLE, POLY_DITHER_TABLE};
S32 ListTypePTexture[] = {
    0,
    POLY_TEXTURE,
    POLY_TEXTURE_FLAT,
    POLY_TEXTURE_GOURAUD,
    0,
    POLY_TEXTURE_INCRUST,
    POLY_TEXTURE_INCRUST_FLAT,
    POLY_TEXTURE_INCRUST_GOURAUD,
    0,
    POLY_TEXTURE_INCRUST,
    POLY_TEXTURE_INCRUST_FLAT,
    POLY_TEXTURE_INCRUST_GOURAUD,
    0,
    POLY_TEXTURE_INCRUST,
    POLY_TEXTURE_INCRUST_FLAT,
    POLY_TEXTURE_INCRUST_GOURAUD,
};

/*══════════════════════════════════════════════════════════════════════════*
		   ▀▀█▀▀ █▀▀▀▀ █▀▀▀█ █▀▀▀█ █▀▀▀█  █    ██▄ █
		     ██  ██▀▀  ██▀█▀ ██▀█▀ ██▀▀█  ██   ██▀██
		     ▀▀  ▀▀▀▀▀ ▀▀  ▀ ▀▀  ▀ ▀▀  ▀  ▀▀   ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/

/*──────────────────────────────────────────────────────────────────────────*/

void DrawSkySeaZBuf() {
    // draw Sky
    //	Sky_Y = 12500 ;

    PtrMap = SkySeaTexture;
    RepMask = 0x7F7F;

    DrawSky(Sky_Y);
    //	DrawSea( Y_SEA ) ;
}

/*──────────────────────────────────────────────────────────────────────────*/
/* The terrain the GPU draws, left unpainted by the software.

   Outdoors the software fill of a terrain triangle the GPU covers is wanted for
   one thing only: its depth, which DrawRecover reads to decide whether the hero
   stands behind a hill (and the camera acts on that). So the triangle is kept
   as it would have been filled, and its depth is filled in later, a tile at a
   time, only where something asks for it: the box of a body DrawRecover checks.
   A tile only chooses the triangles: each is filled whole, once, with the
   filler and the clip window the terrain used, so the depth is the classic one
   to the bit (clipped to the tile, a triangle would get new corners, rounded).
   The depth buffer keeps the nearest of what is written into it, so filling
   later does not change what ends up in it. */

S32 TerrainGpuSkipFill = FALSE;

#define DEPTH_TILE 32
#define MAX_DEFERRED_TRIS (1 << 18)

typedef struct {
    Struc_Point P[3];
} T_DEFERRED_TRI;

static T_DEFERRED_TRI *s_deferred = NULL;
static U8 *s_deferredDone = NULL; /* the triangle's depth has been filled */
static S32 s_nbDeferred = 0;
static S32 s_maxDeferred = 0;
static S32 s_deferClip[4];        /* the clip window the terrain was filled in */
static S32 s_tilesX = 0;
static S32 s_tilesY = 0;
static U8 *s_tileDone = NULL;   /* the tile's depth has been filled */
static S32 *s_binStart = NULL;  /* per tile, first entry in s_binTris (tiles + 1) */
static S32 *s_binTris = NULL;   /* triangles overlapping each tile, tile after tile */
static S32 s_binCap = 0;
static S32 s_binsBuilt = FALSE;
static U8 *s_depthScratch = NULL; /* where the depth fill's colour goes */
static size_t s_depthScratchSize = 0;

S32 TerrainGpu_CanSkipFill(void) {
    return TerrainGpuFastFill AND GpuObj_Active() AND GpuObjShadows AND PtrZBuffer != NULL;
}

static void ResetDeferredDepth(void) {
    s_nbDeferred = 0;
    s_binsBuilt = FALSE;
}

/* Keeps the triangle in Tab_Points for a later depth fill. FALSE when the list
   is full: the caller fills it now. */
static S32 DeferDepth(void) {
    if (s_nbDeferred >= s_maxDeferred) {
        if (s_maxDeferred >= MAX_DEFERRED_TRIS) {
            return FALSE;
        }
        S32 grown = s_maxDeferred ? s_maxDeferred * 2 : 16384;
        T_DEFERRED_TRI *list = (T_DEFERRED_TRI *)realloc(s_deferred, (size_t)grown * sizeof(T_DEFERRED_TRI));
        if (list == NULL) {
            return FALSE;
        }
        s_deferred = list;
        U8 *done = (U8 *)realloc(s_deferredDone, (size_t)grown);
        if (done == NULL) {
            return FALSE;
        }
        s_deferredDone = done;
        s_maxDeferred = grown;
    }
    if (s_nbDeferred == 0) {
        s_deferClip[0] = ClipXMin;
        s_deferClip[1] = ClipYMin;
        s_deferClip[2] = ClipXMax;
        s_deferClip[3] = ClipYMax;
    }
    /* A cell's textured triangle lies over its coloured one, corner for corner. */
    if (s_nbDeferred > 0 AND !memcmp(s_deferred[s_nbDeferred - 1].P, Tab_Points, 3 * sizeof(Struc_Point))) {
        return TRUE;
    }
    memcpy(s_deferred[s_nbDeferred].P, Tab_Points, 3 * sizeof(Struc_Point));
    s_deferredDone[s_nbDeferred] = FALSE;
    s_nbDeferred++;
    s_binsBuilt = FALSE;
    return TRUE;
}

static void TriTiles(const T_DEFERRED_TRI *t, S32 *tx0, S32 *ty0, S32 *tx1, S32 *ty1) {
    S32 x0 = Min3(t->P[0].Pt_XE, t->P[1].Pt_XE, t->P[2].Pt_XE);
    S32 y0 = Min3(t->P[0].Pt_YE, t->P[1].Pt_YE, t->P[2].Pt_YE);
    S32 x1 = Max3(t->P[0].Pt_XE, t->P[1].Pt_XE, t->P[2].Pt_XE);
    S32 y1 = Max3(t->P[0].Pt_YE, t->P[1].Pt_YE, t->P[2].Pt_YE);
    *tx0 = x0 < 0 ? 0 : x0 / DEPTH_TILE;
    *ty0 = y0 < 0 ? 0 : y0 / DEPTH_TILE;
    *tx1 = x1 < 0 ? -1 : ade_min(x1 / DEPTH_TILE, s_tilesX - 1);
    *ty1 = y1 < 0 ? -1 : ade_min(y1 / DEPTH_TILE, s_tilesY - 1);
}

/* Sorts the kept triangles into the tiles their boxes overlap. */
static S32 BuildDepthBins(void) {
    S32 tilesX = ((S32)ModeDesiredX + DEPTH_TILE - 1) / DEPTH_TILE;
    S32 tilesY = ((S32)ModeDesiredY + DEPTH_TILE - 1) / DEPTH_TILE;
    if (tilesX != s_tilesX OR tilesY != s_tilesY OR s_tileDone == NULL) {
        free(s_tileDone);
        free(s_binStart);
        s_tilesX = tilesX;
        s_tilesY = tilesY;
        s_tileDone = (U8 *)malloc((size_t)(tilesX * tilesY));
        s_binStart = (S32 *)malloc((size_t)(tilesX * tilesY + 1) * sizeof(S32));
        if (s_tileDone == NULL OR s_binStart == NULL) {
            free(s_tileDone);
            free(s_binStart);
            s_tileDone = NULL;
            s_binStart = NULL;
            s_tilesX = s_tilesY = 0;
            return FALSE;
        }
    }
    S32 tiles = tilesX * tilesY;
    memset(s_tileDone, 0, (size_t)tiles);
    memset(s_binStart, 0, (size_t)(tiles + 1) * sizeof(S32));

    S32 total = 0;
    for (S32 i = 0; i < s_nbDeferred; i++) {
        S32 tx0, ty0, tx1, ty1;
        TriTiles(&s_deferred[i], &tx0, &ty0, &tx1, &ty1);
        for (S32 ty = ty0; ty <= ty1; ty++) {
            for (S32 tx = tx0; tx <= tx1; tx++) {
                s_binStart[ty * tilesX + tx + 1]++;
                total++;
            }
        }
    }
    if (total > s_binCap) {
        S32 *bins = (S32 *)realloc(s_binTris, (size_t)total * sizeof(S32));
        if (bins == NULL) {
            return FALSE;
        }
        s_binTris = bins;
        s_binCap = total;
    }
    for (S32 t = 0; t < tiles; t++) {
        s_binStart[t + 1] += s_binStart[t];
    }
    /* Fill each bin from its start, then shift the starts back. */
    for (S32 i = 0; i < s_nbDeferred; i++) {
        S32 tx0, ty0, tx1, ty1;
        TriTiles(&s_deferred[i], &tx0, &ty0, &tx1, &ty1);
        for (S32 ty = ty0; ty <= ty1; ty++) {
            for (S32 tx = tx0; tx <= tx1; tx++) {
                s_binTris[s_binStart[ty * tilesX + tx]++] = i;
            }
        }
    }
    for (S32 t = tiles; t > 0; t--) {
        s_binStart[t] = s_binStart[t - 1];
    }
    s_binStart[0] = 0;
    s_binsBuilt = TRUE;
    return TRUE;
}

void TerrainGpu_EnsureDepth(S32 x0, S32 y0, S32 x1, S32 y1) {
    if (s_nbDeferred == 0 OR Log == NULL) {
        return;
    }
    if (!s_binsBuilt AND !BuildDepthBins()) {
        return;
    }
    size_t frame = (size_t)ModeDesiredX * (size_t)ModeDesiredY;
    if (s_depthScratchSize < frame) {
        free(s_depthScratch);
        s_depthScratch = (U8 *)malloc(frame);
        s_depthScratchSize = s_depthScratch ? frame : 0;
        if (s_depthScratch == NULL) {
            return;
        }
    }
    S32 tx0 = ade_max(x0, 0) / DEPTH_TILE;
    S32 ty0 = ade_max(y0, 0) / DEPTH_TILE;
    S32 tx1 = ade_min(x1 / DEPTH_TILE, s_tilesX - 1);
    S32 ty1 = ade_min(y1 / DEPTH_TILE, s_tilesY - 1);
    if (x1 < 0 OR y1 < 0 OR tx0 > tx1 OR ty0 > ty1) {
        return;
    }

    /* The terrain's own filler, writing its colour where nobody looks. */
    S32 clipX0 = ClipXMin, clipY0 = ClipYMin, clipX1 = ClipXMax, clipY1 = ClipYMax;
    U32 bank = Fill_Bank;
    void *log = Log;
    Log = s_depthScratch;
    Switch_Fillers(FILL_POLY_FOG_ZBUFFER);
    ClipXMin = s_deferClip[0];
    ClipYMin = s_deferClip[1];
    ClipXMax = s_deferClip[2];
    ClipYMax = s_deferClip[3];

    for (S32 ty = ty0; ty <= ty1; ty++) {
        for (S32 tx = tx0; tx <= tx1; tx++) {
            S32 tile = ty * s_tilesX + tx;
            if (s_tileDone[tile]) {
                continue;
            }
            s_tileDone[tile] = TRUE;
            for (S32 b = s_binStart[tile]; b < s_binStart[tile + 1]; b++) {
                S32 t = s_binTris[b];
                if (s_deferredDone[t]) {
                    continue;
                }
                s_deferredDone[t] = TRUE;
                Struc_Point points[3];
                memcpy(points, s_deferred[t].P, sizeof(points));
                Fill_Poly(POLY_FLAT, 0, 3, points);
            }
        }
    }

    Switch_Fillers(bank);
    Log = log;
    ClipXMin = clipX0;
    ClipYMin = clipY0;
    ClipXMax = clipX1;
    ClipYMax = clipY1;
}

/*──────────────────────────────────────────────────────────────────────────*/

void ClsTerrainZBuf() {
    SetClearColor((U8)FogCoul);
    Cls();
    SetClearColor(0);

    memset(PtrZBuffer, 255, SIZE_Z_BUFFER); // Clear ZBuffer
    ResetDeferredDepth();
}

/*──────────────────────────────────────────────────────────────────────────*/
/* The fill the software rasterizer uses for a terrain triangle. Where the GPU
   covers the triangle, the software pixel is wanted only for its tag (which is
   what keeps sprites, text and the HUD over it) and for its depth: the
   compositor replaces its colour. A flat fill writes both, without the texture
   sampling, the Gouraud table and the fog step that nobody sees. */
static inline S32 TerrainFill(S32 type, S32 gpuCovers) {
    return (gpuCovers AND TerrainGpuFastFill) ? POLY_FLAT : type;
}

/* Fills the triangle in Tab_Points, or keeps it for its depth alone when the
   GPU draws it and the scenery pass leaves such terrain unpainted. An animated
   triangle is always filled: the water refresh wants to know it was drawn. */
static inline S32 TerrainFillPoly(S32 type, S32 color, S32 gpuCovers, S32 animated) {
    if (gpuCovers AND TerrainGpuSkipFill AND !animated AND DeferDepth()) {
        return FALSE;
    }
    return Fill_Poly(TerrainFill(type, gpuCovers), color, 3, Tab_Points);
}

static inline void DrawFeuillePolyZBuf(S32 xi, S32 zi, T_SOMMET_ROT *ptrs) {
    S32 color;
    T_HALF_POLY *ptrh;
    S32 f0, f1, f2, f3;
    S32 i0, i1, i2, i3;
    S32 x0, z0;
    S32 x1, z1;
    S32 x2, z2;
    S32 x3, z3;
    U8 type;
    U16 fog;

    ptrh = &MapPolyGround[zi * (64 * 2) + xi * 2];

    // optimisation globale affichage terrain
    if (!(ptrh->PolyFlag + ptrh->TexFlag +
          (ptrh + 1)->PolyFlag + (ptrh + 1)->TexFlag)) {
        return;
    }

    x0 = ptrs[0].X2D;
    z0 = ptrs[0].Y2D;
    x1 = ptrs[65].X2D;
    z1 = ptrs[65].Y2D;
    x3 = ptrs[1].X2D;
    z3 = ptrs[1].Y2D;
    x2 = ptrs[66].X2D;
    z2 = ptrs[66].Y2D;

    f0 = ptrs[0].Zrot;

    /*	PtrCLUTGouraud = PtrCLUTFog +
		(16*256) * BoundRegleTrois(0,15,
					ClipZFar - StartZFog,
					f0 - StartZFog ) ;
*/
    fog = (U16)(PalLevel + 16 * BoundRegleTrois(0, 15,
                                                ClipZFar - StartZFog,
                                                f0 - StartZFog));
    SetCLUT(fog);

    f0 = GET_ZO(f0);
    f1 = GET_ZO(ptrs[65].Zrot);
    f3 = GET_ZO(ptrs[1].Zrot);
    f2 = GET_ZO(ptrs[66].Zrot);

    {
        U8 *i = &MapIntensity[zi * 65 + xi];

        i0 = (i[0] & 15) * 256 - 255;
        i1 = (i[65] & 15) * 256 - 255;
        i3 = (i[1] & 15) * 256 - 255;
        i2 = (i[65 + 1] & 15) * 256 - 255;

        if (i0 < 0)
            i0 = 0; // groumpf
        if (i1 < 0)
            i1 = 0; // groumpf
        if (i2 < 0)
            i2 = 0; // groumpf
        if (i3 < 0)
            i3 = 0; // groumpf

/*		i0 = i[0]*256 		;
		i1 = i[65]*256		;
		i3 = i[1]*256 		;
		i2 = i[65+1]*256 	;
*/	}

if (ptrh->Sens == 0) {
    if (ptrh->PolyFlag + ptrh->TexFlag) {
        // type 0 triangle 0----------------------------

        Tab_Points[0].Pt_XE = (S16)x0;
        Tab_Points[0].Pt_YE = (S16)z0;
        Tab_Points[1].Pt_XE = (S16)x1;
        Tab_Points[1].Pt_YE = (S16)z1;
        Tab_Points[2].Pt_XE = (S16)x2;
        Tab_Points[2].Pt_YE = (S16)z2;

        if (TestVuePoly(Tab_Points)) {
            color = (ptrh->Bank << 4) + 11;

            Tab_Points[0].Pt_Light = (S16)i0;
            Tab_Points[1].Pt_Light = (S16)i1;
            Tab_Points[2].Pt_Light = (S16)i2;

            Tab_Points[0].Pt_ZO = (U16)f0;
            Tab_Points[1].Pt_ZO = (U16)f1;
            Tab_Points[2].Pt_ZO = (U16)f2;

            if (ptrh->PolyFlag) {
                if (ptrh->PolyFlag == 1) {
                    color += ((i0 + i1 + i2) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {0, 1, 2};
                    const S32 gpuLights[3] = {i0, i1, i2};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, ListTypePolygone[ptrh->PolyFlag],
                                               color, NULL, FALSE, ptrh->SampleStep);
                }
                TerrainFillPoly(ListTypePolygone[ptrh->PolyFlag], color, gpuCovers, FALSE);
            }

            if (ptrh->TexFlag) {
                T_HALF_TEX *ptrt;

                ptrt = &ListTexDef[ptrh->IndexTex];

                Tab_Points[0].Pt_MapU = ptrt->Tx0;
                Tab_Points[0].Pt_MapV = ptrt->Ty0;
                Tab_Points[1].Pt_MapU = ptrt->Tx1;
                Tab_Points[1].Pt_MapV = ptrt->Ty1;
                Tab_Points[2].Pt_MapU = ptrt->Tx2;
                Tab_Points[2].Pt_MapV = ptrt->Ty2;

                if (ptrh->TexFlag == 2) {
                    color = ((i0 + i1 + i2) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                type = (U8)ListTypePTexture[ptrh->TexFlag + (ptrh->PolyFlag << 2)];

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {0, 1, 2};
                    const S32 gpuLights[3] = {i0, i1, i2};
                    const U16 gpuUv[6] = {ptrt->Tx0, ptrt->Ty0, ptrt->Tx1, ptrt->Ty1, ptrt->Tx2, ptrt->Ty2};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, type, color, gpuUv,
                                               ptrh->CodeJeu == CJ_FOOT_WATER || ptrh->CodeJeu == CJ_ANIMATED_WATER,
                                               ptrh->SampleStep);
                }
                if (TerrainFillPoly(type, color, gpuCovers, (ptrh->CodeJeu & 12) == 12)) {
                    if (MaxPolySea AND(ptrh->CodeJeu & 12) == 12) { // hihi je suis un polygone animé...

                        if (NbAniPoly < MaxPolySea - 1) {
                            PtrAniPoly->X0 = (S16)Min3(x0, x1, x2);
                            PtrAniPoly->Y0 = (S16)Min3(z0, z1, z2);
                            PtrAniPoly->X1 = (S16)Max3(x0, x1, x2);
                            PtrAniPoly->Y1 = (S16)Max3(z0, z1, z2);

                            if (((PtrAniPoly->Y1 - PtrAniPoly->Y0) > ANI_POLY_STEP_Y)
                                    AND((PtrAniPoly->X1 - PtrAniPoly->X0) > ANI_POLY_STEP_X)) {
                                PtrAniPoly->Type = type;
                                PtrAniPoly->Color = (U8)color;

                                PtrAniPoly->Fog = fog;

                                PtrAniPoly->Tab_Points[0] = Tab_Points[0];
                                PtrAniPoly->Tab_Points[1] = Tab_Points[1];
                                PtrAniPoly->Tab_Points[2] = Tab_Points[2];

                                NbAniPoly++;
                                PtrAniPoly++;
                            }
                        }
#ifdef DEBUG_TOOLS
                        else {
                            Message("MaxPolySea Atteint !", TRUE);
                        }
#endif
                    }
                }
            }
        }
    }
    // type 0 triangle 1----------------------------------

    ptrh++;

    if (ptrh->PolyFlag + ptrh->TexFlag) {
        Tab_Points[0].Pt_XE = (S16)x2;
        Tab_Points[0].Pt_YE = (S16)z2;
        Tab_Points[1].Pt_XE = (S16)x3;
        Tab_Points[1].Pt_YE = (S16)z3;
        Tab_Points[2].Pt_XE = (S16)x0;
        Tab_Points[2].Pt_YE = (S16)z0;

        if (TestVuePoly(Tab_Points)) {
            color = (ptrh->Bank << 4) + 11;

            Tab_Points[0].Pt_Light = (S16)i2;
            Tab_Points[1].Pt_Light = (S16)i3;
            Tab_Points[2].Pt_Light = (S16)i0;

            Tab_Points[0].Pt_ZO = (U16)f2;
            Tab_Points[1].Pt_ZO = (U16)f3;
            Tab_Points[2].Pt_ZO = (U16)f0;

            if (ptrh->PolyFlag) {
                if (ptrh->PolyFlag == 1) {
                    color += ((i0 + i2 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }
                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {2, 3, 0};
                    const S32 gpuLights[3] = {i2, i3, i0};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, ListTypePolygone[ptrh->PolyFlag],
                                               color, NULL, FALSE, ptrh->SampleStep);
                }
                TerrainFillPoly(ListTypePolygone[ptrh->PolyFlag], color, gpuCovers, FALSE);
            }

            if (ptrh->TexFlag) {
                T_HALF_TEX *ptrt;

                ptrt = &ListTexDef[ptrh->IndexTex];

                Tab_Points[0].Pt_MapU = ptrt->Tx0;
                Tab_Points[0].Pt_MapV = ptrt->Ty0;
                Tab_Points[1].Pt_MapU = ptrt->Tx1;
                Tab_Points[1].Pt_MapV = ptrt->Ty1;
                Tab_Points[2].Pt_MapU = ptrt->Tx2;
                Tab_Points[2].Pt_MapV = ptrt->Ty2;

                if (ptrh->TexFlag == 2) {
                    color = ((i0 + i2 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                type = (U8)ListTypePTexture[ptrh->TexFlag + (ptrh->PolyFlag << 2)];

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {2, 3, 0};
                    const S32 gpuLights[3] = {i2, i3, i0};
                    const U16 gpuUv[6] = {ptrt->Tx0, ptrt->Ty0, ptrt->Tx1, ptrt->Ty1, ptrt->Tx2, ptrt->Ty2};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, type, color, gpuUv,
                                               ptrh->CodeJeu == CJ_FOOT_WATER || ptrh->CodeJeu == CJ_ANIMATED_WATER,
                                               ptrh->SampleStep);
                }
                if (TerrainFillPoly(type, color, gpuCovers, (ptrh->CodeJeu & 12) == 12)) {
                    if (MaxPolySea AND(ptrh->CodeJeu & 12) == 12) { // hihi je suis un polygone animé...
                        if (NbAniPoly < MaxPolySea - 1) {
                            PtrAniPoly->X0 = (S16)Min3(x0, x2, x3);
                            PtrAniPoly->Y0 = (S16)Min3(z0, z2, z3);
                            PtrAniPoly->X1 = (S16)Max3(x0, x2, x3);
                            PtrAniPoly->Y1 = (S16)Max3(z0, z2, z3);

                            if (((PtrAniPoly->Y1 - PtrAniPoly->Y0) > ANI_POLY_STEP_Y)
                                    AND((PtrAniPoly->X1 - PtrAniPoly->X0) > ANI_POLY_STEP_X)) {
                                PtrAniPoly->Type = type;
                                PtrAniPoly->Color = (U8)color;

                                PtrAniPoly->Fog = fog;

                                PtrAniPoly->Tab_Points[0] = Tab_Points[0];
                                PtrAniPoly->Tab_Points[1] = Tab_Points[1];
                                PtrAniPoly->Tab_Points[2] = Tab_Points[2];

                                NbAniPoly++;
                                PtrAniPoly++;
                            }
                        }
#ifdef DEBUG_TOOLS
                        else {
                            Message("MaxPolySea Atteint !", TRUE);
                        }
#endif
                    }
                }
            }
        }
    }
} else // type == 1---------------------------------------------------
{
    // type 1 triangle 0

    if (ptrh->PolyFlag + ptrh->TexFlag) {
        Tab_Points[0].Pt_XE = (S16)x3;
        Tab_Points[0].Pt_YE = (S16)z3;
        Tab_Points[1].Pt_XE = (S16)x0;
        Tab_Points[1].Pt_YE = (S16)z0;
        Tab_Points[2].Pt_XE = (S16)x1;
        Tab_Points[2].Pt_YE = (S16)z1;

        if (TestVuePoly(Tab_Points)) {
            color = (ptrh->Bank << 4) + 11;

            Tab_Points[0].Pt_Light = (S16)i3;
            Tab_Points[1].Pt_Light = (S16)i0;
            Tab_Points[2].Pt_Light = (S16)i1;

            Tab_Points[0].Pt_ZO = (U16)f3;
            Tab_Points[1].Pt_ZO = (U16)f0;
            Tab_Points[2].Pt_ZO = (U16)f1;

            if (ptrh->PolyFlag) {
                if (ptrh->PolyFlag == 1) {
                    color += ((i0 + i1 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }
                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {3, 0, 1};
                    const S32 gpuLights[3] = {i3, i0, i1};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, ListTypePolygone[ptrh->PolyFlag],
                                               color, NULL, FALSE, ptrh->SampleStep);
                }
                TerrainFillPoly(ListTypePolygone[ptrh->PolyFlag], color, gpuCovers, FALSE);
            }

            if (ptrh->TexFlag) {
                T_HALF_TEX *ptrt;

                ptrt = &ListTexDef[ptrh->IndexTex];

                Tab_Points[0].Pt_MapU = ptrt->Tx0;
                Tab_Points[0].Pt_MapV = ptrt->Ty0;
                Tab_Points[1].Pt_MapU = ptrt->Tx1;
                Tab_Points[1].Pt_MapV = ptrt->Ty1;
                Tab_Points[2].Pt_MapU = ptrt->Tx2;
                Tab_Points[2].Pt_MapV = ptrt->Ty2;

                if (ptrh->TexFlag == 2) {
                    color = ((i0 + i1 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                type = (U8)ListTypePTexture[ptrh->TexFlag + (ptrh->PolyFlag << 2)];

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {3, 0, 1};
                    const S32 gpuLights[3] = {i3, i0, i1};
                    const U16 gpuUv[6] = {ptrt->Tx0, ptrt->Ty0, ptrt->Tx1, ptrt->Ty1, ptrt->Tx2, ptrt->Ty2};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, type, color, gpuUv,
                                               ptrh->CodeJeu == CJ_FOOT_WATER || ptrh->CodeJeu == CJ_ANIMATED_WATER,
                                               ptrh->SampleStep);
                }
                if (TerrainFillPoly(type, color, gpuCovers, (ptrh->CodeJeu & 12) == 12)) {
                    if (MaxPolySea AND(ptrh->CodeJeu & 12) == 12) { // hihi je suis un polygone animé...
                        if (NbAniPoly < MaxPolySea - 1) {
                            PtrAniPoly->X0 = (S16)Min3(x0, x1, x3);
                            PtrAniPoly->Y0 = (S16)Min3(z0, z1, z3);
                            PtrAniPoly->X1 = (S16)Max3(x0, x1, x3);
                            PtrAniPoly->Y1 = (S16)Max3(z0, z1, z3);

                            if (((PtrAniPoly->Y1 - PtrAniPoly->Y0) > ANI_POLY_STEP_Y)
                                    AND((PtrAniPoly->X1 - PtrAniPoly->X0) > ANI_POLY_STEP_X)) {
                                PtrAniPoly->Type = type;
                                PtrAniPoly->Color = (U8)color;

                                PtrAniPoly->Fog = fog;

                                PtrAniPoly->Tab_Points[0] = Tab_Points[0];
                                PtrAniPoly->Tab_Points[1] = Tab_Points[1];
                                PtrAniPoly->Tab_Points[2] = Tab_Points[2];

                                NbAniPoly++;
                                PtrAniPoly++;
                            }
                        }
#ifdef DEBUG_TOOLS
                        else {
                            Message("MaxPolySea Atteint !", TRUE);
                        }
#endif
                    }
                }
            }
        }
    }
    // type 1 triangle 1

    ptrh++;

    if (ptrh->PolyFlag + ptrh->TexFlag) {
        Tab_Points[0].Pt_XE = (S16)x1;
        Tab_Points[0].Pt_YE = (S16)z1;
        Tab_Points[1].Pt_XE = (S16)x2;
        Tab_Points[1].Pt_YE = (S16)z2;
        Tab_Points[2].Pt_XE = (S16)x3;
        Tab_Points[2].Pt_YE = (S16)z3;

        if (TestVuePoly(Tab_Points)) {
            color = (ptrh->Bank << 4) + 11;

            Tab_Points[0].Pt_Light = (S16)i1;
            Tab_Points[1].Pt_Light = (S16)i2;
            Tab_Points[2].Pt_Light = (S16)i3;

            Tab_Points[0].Pt_ZO = (U16)f1;
            Tab_Points[1].Pt_ZO = (U16)f2;
            Tab_Points[2].Pt_ZO = (U16)f3;

            if (ptrh->PolyFlag) {
                if (ptrh->PolyFlag == 1) {
                    color += ((i1 + i2 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {1, 2, 3};
                    const S32 gpuLights[3] = {i1, i2, i3};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, ListTypePolygone[ptrh->PolyFlag],
                                               color, NULL, FALSE, ptrh->SampleStep);
                }
                TerrainFillPoly(ListTypePolygone[ptrh->PolyFlag], color, gpuCovers, FALSE);
            }

            if (ptrh->TexFlag) {
                T_HALF_TEX *ptrt;

                ptrt = &ListTexDef[ptrh->IndexTex];

                Tab_Points[0].Pt_MapU = ptrt->Tx0;
                Tab_Points[0].Pt_MapV = ptrt->Ty0;
                Tab_Points[1].Pt_MapU = ptrt->Tx1;
                Tab_Points[1].Pt_MapV = ptrt->Ty1;
                Tab_Points[2].Pt_MapU = ptrt->Tx2;
                Tab_Points[2].Pt_MapV = ptrt->Ty2;

                if (ptrh->TexFlag == 2) {
                    color = ((i1 + i2 + i3) * 21845) >> 24; // *21845 >> 16 EQU / 3
                }

                type = (U8)ListTypePTexture[ptrh->TexFlag + (ptrh->PolyFlag << 2)];

                S32 gpuCovers;
                {
                    static const U8 gpuCorners[3] = {1, 2, 3};
                    const S32 gpuLights[3] = {i1, i2, i3};
                    const U16 gpuUv[6] = {ptrt->Tx0, ptrt->Ty0, ptrt->Tx1, ptrt->Ty1, ptrt->Tx2, ptrt->Ty2};
                    gpuCovers = TerrainGpu_Tri(xi, zi, gpuCorners, gpuLights, type, color, gpuUv,
                                               ptrh->CodeJeu == CJ_FOOT_WATER || ptrh->CodeJeu == CJ_ANIMATED_WATER,
                                               ptrh->SampleStep);
                }
                if (TerrainFillPoly(type, color, gpuCovers, (ptrh->CodeJeu & 12) == 12)) {
                    if (MaxPolySea AND(ptrh->CodeJeu & 12) == 12) { // hihi je suis un polygone animé...
                        if (NbAniPoly < MaxPolySea - 1) {
                            PtrAniPoly->X0 = (S16)Min3(x1, x2, x3);
                            PtrAniPoly->Y0 = (S16)Min3(z1, z2, z3);
                            PtrAniPoly->X1 = (S16)Max3(x1, x2, x3);
                            PtrAniPoly->Y1 = (S16)Max3(z1, z2, z3);

                            if (((PtrAniPoly->Y1 - PtrAniPoly->Y0) > ANI_POLY_STEP_Y)
                                    AND((PtrAniPoly->X1 - PtrAniPoly->X0) > ANI_POLY_STEP_X)) {
                                PtrAniPoly->Type = type;
                                PtrAniPoly->Color = (U8)color;

                                PtrAniPoly->Fog = fog;

                                PtrAniPoly->Tab_Points[0] = Tab_Points[0];
                                PtrAniPoly->Tab_Points[1] = Tab_Points[1];
                                PtrAniPoly->Tab_Points[2] = Tab_Points[2];

                                NbAniPoly++;
                                PtrAniPoly++;
                            }
                        }
#ifdef DEBUG_TOOLS
                        else {
                            Message("MaxPolySea Atteint !", TRUE);
                        }
#endif
                    }
                }
            }
        }
    }
}

#if !(defined(DEMO) || defined(LBA_GAME))
if (FillFeuille & 4) {
    DrawColFeuille3D(x0, z0,
                     x1, z1,
                     x2, z2,
                     x3, z3,
                     MapPolyGround[zi * 64 * 2 + xi * 2].Sens,
                     CYAN,
                     MapPolyGround[zi * 64 * 2 + xi * 2].Col,
                     MapPolyGround[zi * 64 * 2 + xi * 2 + 1].Col);
}

if (FillFeuille & 2) {
    DrawFeuille3D(x0, z0,
                  x1, z1,
                  x2, z2,
                  x3, z3,
                  MapPolyGround[zi * 64 * 2 + xi * 2].Sens,
                  GREEN,
                  FALSE);
}
#endif
}

/*──────────────────────────────────────────────────────────────────────────*/
static inline void FillBlackPolyZBuf(S32 xi, S32 zi, T_SOMMET_ROT *ptrs, U8 *ptra) {
    T_HALF_POLY *ptrh;
    //	S32 		f0, f1, f2, f3 	;
    //	S32 		f00, f10, f20, f30 ;
    S32 x0, z0;
    S32 x1, z1;
    S32 x2, z2;
    S32 x3, z3;
    U8 b0, b1, b2, b3;

    ptrh = &MapPolyGround[zi * (64 * 2) + xi * 2];

    x0 = ptrs[0].X2D;
    z0 = ptrs[0].Y2D;
    b0 = (U8)(ptra[0] & 32);
    x1 = ptrs[65].X2D;
    z1 = ptrs[65].Y2D;
    b1 = (U8)(ptra[65] & 32);
    x3 = ptrs[1].X2D;
    z3 = ptrs[1].Y2D;
    b3 = (U8)(ptra[1] & 32);
    x2 = ptrs[66].X2D;
    z2 = ptrs[66].Y2D;
    b2 = (U8)(ptra[66] & 32);

    /*
	xi *= 512 ;
	zi *= 512 ;

	f0 = GET_ZO(ptrs[0].Zrot)	;
	f1 = GET_ZO(ptrs[65].Zrot)	;
	f3 = GET_ZO(ptrs[1].Zrot)	;
	f2 = GET_ZO(ptrs[66].Zrot)	;

	LongWorldRotatePoint(xi, 0, zi)	;
	f00 = GET_ZO(CameraZr - Z0)	;
	LongWorldRotatePoint(xi, 0, zi+512);
	f10 = GET_ZO(CameraZr - Z0)	;
	LongWorldRotatePoint(xi+512, 0, zi);
	f30 = GET_ZO(CameraZr - Z0)	;
	LongWorldRotatePoint(xi+512, 0, zi+512);
	f20 = GET_ZO(CameraZr - Z0)	;
*/

    if (!(b0 | b1)) {
        Tab_Points[0].Pt_XE = (S16)x0;
        Tab_Points[0].Pt_YE = (S16)z0;
        Tab_Points[1].Pt_XE = (S16)x1;
        Tab_Points[1].Pt_YE = (S16)z1;
        Tab_Points[2].Pt_XE = (S16)x1;
        Tab_Points[2].Pt_YE = (S16)((S32)ModeDesiredY - 1);
        Tab_Points[3].Pt_XE = (S16)x0;
        Tab_Points[3].Pt_YE = (S16)((S32)ModeDesiredY - 1);

        Tab_Points[0].Pt_Light = (S16)0;
        Tab_Points[1].Pt_Light = (S16)0;
        Tab_Points[2].Pt_Light = (S16)0;
        Tab_Points[3].Pt_Light = (S16)0;

        Tab_Points[0].Pt_ZO = (U16)0;
        Tab_Points[1].Pt_ZO = (U16)0;
        Tab_Points[2].Pt_ZO = (U16)0;
        Tab_Points[3].Pt_ZO = (U16)0;

        Fill_Poly(0, 0, 4, Tab_Points);
    }

    if (!(b1 | b2)) {
        Tab_Points[0].Pt_XE = (S16)x1;
        Tab_Points[0].Pt_YE = (S16)z1;
        Tab_Points[1].Pt_XE = (S16)x2;
        Tab_Points[1].Pt_YE = (S16)z2;
        Tab_Points[2].Pt_XE = (S16)x2;
        Tab_Points[2].Pt_YE = (S16)((S32)ModeDesiredY - 1);
        Tab_Points[3].Pt_XE = (S16)x1;
        Tab_Points[3].Pt_YE = (S16)((S32)ModeDesiredY - 1);

        Tab_Points[0].Pt_Light = (S16)0;
        Tab_Points[1].Pt_Light = (S16)0;
        Tab_Points[2].Pt_Light = (S16)0;
        Tab_Points[3].Pt_Light = (S16)0;

        Tab_Points[0].Pt_ZO = (U16)0;
        Tab_Points[1].Pt_ZO = (U16)0;
        Tab_Points[2].Pt_ZO = (U16)0;
        Tab_Points[3].Pt_ZO = (U16)0;

        Fill_Poly(0, 0, 4, Tab_Points);
    }

    if (!(b2 | b3)) {
        Tab_Points[0].Pt_XE = (S16)x2;
        Tab_Points[0].Pt_YE = (S16)z2;
        Tab_Points[1].Pt_XE = (S16)x3;
        Tab_Points[1].Pt_YE = (S16)z3;
        Tab_Points[2].Pt_XE = (S16)x3;
        Tab_Points[2].Pt_YE = (S16)((S32)ModeDesiredY - 1);
        Tab_Points[3].Pt_XE = (S16)x2;
        Tab_Points[3].Pt_YE = (S16)((S32)ModeDesiredY - 1);

        Tab_Points[0].Pt_Light = (S16)0;
        Tab_Points[1].Pt_Light = (S16)0;
        Tab_Points[2].Pt_Light = (S16)0;
        Tab_Points[3].Pt_Light = (S16)0;

        Tab_Points[0].Pt_ZO = (U16)0;
        Tab_Points[1].Pt_ZO = (U16)0;
        Tab_Points[2].Pt_ZO = (U16)0;
        Tab_Points[3].Pt_ZO = (U16)0;

        Fill_Poly(0, 0, 4, Tab_Points);
    }

    if (!(b3 | b0)) {
        Tab_Points[0].Pt_XE = (S16)x3;
        Tab_Points[0].Pt_YE = (S16)z3;
        Tab_Points[1].Pt_XE = (S16)x0;
        Tab_Points[1].Pt_YE = (S16)z0;
        Tab_Points[2].Pt_XE = (S16)x0;
        Tab_Points[2].Pt_YE = (S16)((S32)ModeDesiredY - 1);
        Tab_Points[3].Pt_XE = (S16)x3;
        Tab_Points[3].Pt_YE = (S16)((S32)ModeDesiredY - 1);

        Tab_Points[0].Pt_Light = (S16)0;
        Tab_Points[1].Pt_Light = (S16)0;
        Tab_Points[2].Pt_Light = (S16)0;
        Tab_Points[3].Pt_Light = (S16)0;

        Tab_Points[0].Pt_ZO = (U16)0;
        Tab_Points[1].Pt_ZO = (U16)0;
        Tab_Points[2].Pt_ZO = (U16)0;
        Tab_Points[3].Pt_ZO = (U16)0;

        Fill_Poly(0, 0, 4, Tab_Points);
    }
}
/*──────────────────────────────────────────────────────────────────────────*/
struct S_Terrain {
    S8 x;
    S8 y;
};

struct S_Terrain TerrNO[9] = {
    {-1, -1}, {0, -1}, {1, -1}, {-1, 0}, {0, 0}, {1, 0}, {-1, 1}, {0, 1}, {1, 1}};

struct S_Terrain TerrNE[9] = {
    {1, -1}, {0, -1}, {-1, -1}, {1, 0}, {0, 0}, {-1, 0}, {1, 1}, {0, 1}, {-1, 1}};

struct S_Terrain TerrSO[9] = {
    {-1, 1}, {0, 1}, {1, 1}, {-1, 0}, {0, 0}, {1, 0}, {-1, -1}, {0, -1}, {1, -1}};

struct S_Terrain TerrSE[9] = {
    {1, 1}, {0, 1}, {-1, 1}, {1, 0}, {0, 0}, {-1, 0}, {1, -1}, {0, -1}, {-1, -1}};

struct S_Terrain *TerrainTri;

S32 OffPosCamera[3] = {
    // offsets camera precalculés
    -(512 * 64), // -1
    0,           //  0
    (512 * 64)   //  1
};

extern S32 PtrProjectPoint(S32 xw, S32 yw, S32 zw);

/* Whether the cube the camera has just been placed in front of can be left out
   entirely. The original had this test here and left it commented out (below),
   which the classic 3 x 3 ring could afford: eight neighbours, drawn in a fixed
   order. The GPU renderer's wider ring is twenty-five cubes or forty-nine, most
   of them behind the camera, and each one is loaded, has its 65 x 65 vertices
   projected and its cells walked before anything decides they are off screen.

   Conservative by construction: a cube is skipped only when every one of its
   corners, at ground level and above the highest terrain, projects past the same
   edge of the clip window. A cube with any corner behind the near plane is drawn,
   because a corner behind the camera projects nowhere meaningful and the cube may
   still cover the screen. So the frame is the same frame; it is reached without
   drawing what cannot be seen. */
#define CUBE_TOP_Y 12000 /* above any terrain in a cube */

static S32 CubeOutOfView(void) {
    static const S32 cornerX[4] = {0, 32767, 32767, 0};
    static const S32 cornerZ[4] = {0, 0, 32767, 32767};
    /* Each stays true only while every corner is outside that side of the view.
       The tests are the frustum's own planes, all through the eye, written
       without the divide: a corner behind the camera has no screen position to
       compare, but it still sits on one side of a plane. */
    S32 outNear = TRUE, outLeft = TRUE, outRight = TRUE, outAbove = TRUE, outBelow = TRUE;

    for (S32 i = 0; i < 8; i++) {
        LongWorldRotatePoint(cornerX[i & 3], (i < 4) ? 0 : CUBE_TOP_Y, cornerZ[i & 3]);
        const double zr = (double)(CameraZr - Z0);
        const double px = (double)(X0 - CameraXr) * (double)FRatioX;
        const double py = (double)(Y0 - CameraYr) * (double)FRatioX * (double)FRatioY;

        if (zr > (double)NearClip) {
            outNear = FALSE;
        }
        if (px - (double)(ClipXMin - XCentre) * zr >= 0.0) {
            outLeft = FALSE;
        }
        if (px - (double)(ClipXMax - XCentre) * zr <= 0.0) {
            outRight = FALSE;
        }
        if (py - (double)(ClipYMin - YCentre) * zr >= 0.0) {
            outAbove = FALSE;
        }
        if (py - (double)(ClipYMax - YCentre) * zr <= 0.0) {
            outBelow = FALSE;
        }
    }
    return outNear OR outLeft OR outRight OR outAbove OR outBelow;
}

void DrawHorizon2ZBuf() {
    S32 flag = FALSE;
    S32 cx = CameraX;
    S32 cy = CameraY;
    S32 cz = CameraZ;

    if (BetaCam >= 0 AND BetaCam < 1024) {
        TerrainTri = TerrSO;
    } else if (BetaCam >= 1024 AND BetaCam < 2048) {
        TerrainTri = TerrNO;
    } else if (BetaCam >= 2048 AND BetaCam < 3072) {
        TerrainTri = TerrNE;
    } else if (BetaCam >= 3072 AND BetaCam < 4096) {
        TerrainTri = TerrSE;
    }
    /*
	if( BetaCam>=0 AND BetaCam<1024 )
	{
		TerrainTri = TerrNE ;
	}
	else if( BetaCam>=1024 AND BetaCam<2048 )
	{
		TerrainTri = TerrSE ;
	}
	else if( BetaCam>=2048 AND BetaCam<3072 )
	{
		TerrainTri = TerrSO ;
	}
	else if( BetaCam>=3072 AND BetaCam<4096 )
	{
		TerrainTri = TerrNO ;
	}
*/
    /* The GPU renderer's farther view: a wider ring of cubes, farthest first. */
    const S32 radius = TerrainGpu_HorizonRadius();
    static struct S_Terrain ring[49];
    if (radius > 1) {
        S32 nb = 0;
        for (S32 dy = -radius; dy <= radius; dy++) {
            for (S32 dx = -radius; dx <= radius; dx++) {
                ring[nb].x = (S8)dx;
                ring[nb].y = (S8)dy;
                nb++;
            }
        }
        /* Distance from the camera within its cube to each cube's centre. */
        const float camX = (float)(cx & 32767) / 32768.0f - 0.5f;
        const float camZ = (float)(cz & 32767) / 32768.0f - 0.5f;
        for (S32 i = 1; i < nb; i++) {
            const struct S_Terrain t = ring[i];
            const float d = ((float)t.x - camX) * ((float)t.x - camX) + ((float)t.y - camZ) * ((float)t.y - camZ);
            S32 j = i - 1;
            while (j >= 0) {
                const float dj = ((float)ring[j].x - camX) * ((float)ring[j].x - camX) +
                                 ((float)ring[j].y - camZ) * ((float)ring[j].y - camZ);
                if (dj >= d) {
                    break;
                }
                ring[j + 1] = ring[j];
                j--;
            }
            ring[j + 1] = t;
        }
        TerrainTri = ring;
    }
    const S32 nbCubes = (2 * radius + 1) * (2 * radius + 1);
    for (S32 n = nbCubes; n > 0; n--, TerrainTri++) {
        S32 x, y;

        x = CurrentCubeX + TerrainTri->x;
        y = CurrentCubeY + TerrainTri->y;

        if (x >= 0 AND x <= 15 AND y >= 0 AND y <= 15) {
            if (TerrainTri->x OR TerrainTri->y) {
                // PreClipping du Cube
                /*				if( PtrProjectPoint( 0, 0, 0 )
				OR  PtrProjectPoint( 32767, 0, 0 )
				OR  PtrProjectPoint( 32767, 0, 32767 )
				OR  PtrProjectPoint( 0, 0, 32767 ) )
*/
                {
#ifdef LBA_GAME
                    SetPosCamera(cx - TerrainTri->x * 512 * 64 + TerrainTri->x, cy, cz - TerrainTri->y * 512 * 64 + TerrainTri->y);
#else
                    SetPosCamera(cx - OffPosCamera[TerrainTri->x + 1] + TerrainTri->x * Reglage, cy, cz - OffPosCamera[TerrainTri->y + 1] + TerrainTri->y * Reglage);
#endif
                    /* Only for the GPU renderer's wider ring: the classic
                       3 x 3 keeps the path it was verified with. */
                    if (radius > 1 AND CubeOutOfView()) {
                        continue;
                    }
                    TerrainGpu_SetWaterCube(x, y);
                    if (LoadCube(x, y)) {
                        AffichageTerrainZBuf();
                        flag = TRUE;
                    } else
                        DrawOneSea(Y_SEA);
                }
            }
        }
    }

    SetPosCamera(cx, cy, cz);
    TerrainGpu_SetWaterCube(CurrentCubeX, CurrentCubeY);

    if (flag) {
        if (!LoadCube(CurrentCubeX, CurrentCubeY)) {
            // on est dans un cube phantom !!!!!!!
            InitSeaCube();
            DrawOneSea(Y_SEA);
        }
    }
}

/*──────────────────────────────────────────────────────────────────────────*/
U32 MaskVisible(U8 *in_ptr) {
    // This code conversion could be improved...

    U8 *firstRow = in_ptr;
    U8 *secondRow = in_ptr + 65;

    U8 maskH = *firstRow;
    U8 maskL = *firstRow;

    maskH |= *secondRow;
    maskL &= *secondRow;

    maskH |= *(firstRow + 1);
    maskL &= *(firstRow + 1);

    maskH |= *(secondRow + 1);
    maskL &= *(secondRow + 1);

    U32 finalMask = 0;
    finalMask |= (maskH << 8);
    finalMask |= maskL;
    finalMask &= 0x20FF;

    return finalMask;
}

/*──────────────────────────────────────────────────────────────────────────*/
/* The 65 x 65 grid of a cube placed and projected in one pass: the same
   answers as LongWorldRotatePoint + LongProjectPoint give, without the
   long-double round trip per point. See the note at its one call site. */
static void FastGrid(U8 *ptra, const S16 *ptrm, T_SOMMET_ROT *ptrs) {
    const TYPE_MAT *m = &MatriceWorld;
    const S32 side = NB_COTE + 1;
    /* Each column's share of the rotation, and each row's. */
    static double colX[NB_COTE + 1], colY[NB_COTE + 1], colZ[NB_COTE + 1];
    static double rowX[NB_COTE + 1], rowY[NB_COTE + 1], rowZ[NB_COTE + 1];
    for (S32 i = 0; i < side; i++) {
        const double at = (double)(i * 512);
        colX[i] = at * (double)m->F.M11;
        colY[i] = at * (double)m->F.M21;
        colZ[i] = at * (double)m->F.M31;
        rowX[i] = at * (double)m->F.M13;
        rowY[i] = at * (double)m->F.M23;
        rowZ[i] = at * (double)m->F.M33;
    }
    const double m12 = (double)m->F.M12, m22 = (double)m->F.M22, m32 = (double)m->F.M32;
    const double camXr = (double)CameraXr, camYr = (double)CameraYr;
    const double ratioX = (double)FRatioX, ratioY = (double)FRatioY;
    const S32 guard16 = FollowCamera;

    for (S32 iz = 0; iz < side; iz++) {
        for (S32 ix = 0; ix < side; ix++, ptra++, ptrm++, ptrs++) {
            const double y = (double)*ptrm;
            const S32 X0f = (S32)lrint(colX[ix] + y * m12 + rowX[iz]);
            const S32 Y0f = (S32)lrint(colY[ix] + y * m22 + rowY[iz]);
            const S32 Z0f = (S32)lrint(colZ[ix] + y * m32 + rowZ[iz]);
            const S32 zr = CameraZr - Z0f;

            if (zr <= NearClip) {
                *ptra = 32;
                continue;
            }
            if (zr >= ClipZFar) {
                *ptra = 0;
                continue;
            }
            const double factor = ratioX / (double)zr;
            const S32 xp = XCentre + (S32)lrint(((double)X0f - camXr) * factor);
            const S32 yp = YCentre + (S32)lrint(((double)Y0f - camYr) * factor * ratioY);

            /* The 16-bit truncation guard the free camera needs, as above. */
            if (guard16 AND (xp != (S32)(S16)xp OR yp != (S32)(S16)yp)) {
                *ptra = 32;
                continue;
            }

            U8 flag = 16;
            if (xp < ClipXMin)
                flag |= 1;
            if (xp > ClipXMax)
                flag |= 2;
            if (yp < ClipYMin)
                flag |= 4;
            if (yp > ClipYMax)
                flag |= 8;
            *ptra = flag;
            ptrs->Zrot = zr;
            ptrs->X2D = (S16)xp;
            ptrs->Y2D = (S16)yp;
        }
    }
}

void AffichageTerrainZBuf(void) {
    S16 *ptrm;
    U8 *ptra;
    T_SOMMET_ROT *ptrs;

    // affichage des objets du decors
    AffichageObjetDecorsZBuf();

    // calcul les coordonnées 2D de chaque points
    ptra = &SommetAffichable[0][0];
    ptrm = MapSommetY;
    ptrs = &SommetRot[0][0];

    /* The grid's own placement, when the GPU renderer is the one drawing.
       Sixty-five by sixty-five points a cube, a dozen cubes a frame, each one
       rotated and projected through the engine's long-double path: 8 ms of an
       orbiting frame at 720p, more than the capture that rides along with it.
       The rotation of a regular lattice is affine, so the columns' and rows'
       shares are worth computing once, and the rest is a multiply-add and one
       divide a point in double precision -- which rounds to the same integers
       everywhere the classic path's 80 bits do, bar the exact halfway cases
       that do not survive a frame either way. The classic renderer keeps the
       path it was verified with. */
    if (GpuObj_Active()) {
        FastGrid(ptra, ptrm, ptrs);
    } else
    for (S32 z = 0; z <= NB_COTE * 512; z += 512) {
        for (S32 x = 0; x <= NB_COTE * 512; x += 512) {
            S32 zr;

            LongWorldRotatePoint(x, *ptrm, z);

            zr = CameraZr - Z0;

            // cone clipping

            if (zr <= NearClip) {
                *ptra = 32;
            } else if ((zr < ClipZFar)
                           AND LongProjectPoint(X0, Y0, Z0)) {
                U8 flag;

                /* The screen coordinates below are truncated to 16 bits. A
                   vertex just past the near plane projects far beyond that and
                   wraps, smearing its cell across the frame. The free camera's
                   near plane is close enough to reach those, so it is guarded
                   here, in the shape DRAWSKY.CPP has always used; the classic
                   camera never projects one and keeps its untested path. */
                if (FollowCamera AND (Xp != (S32)(S16)Xp OR Yp != (S32)(S16)Yp)) {
                    *ptra = 32;
                    ptra++;
                    ptrm++;
                    ptrs++;
                    continue;
                }

                flag = 16;

                if (Xp < ClipXMin)
                    flag |= 1;
                if (Xp > ClipXMax)
                    flag |= 2;
                if (Yp < ClipYMin)
                    flag |= 4;
                if (Yp > ClipYMax)
                    flag |= 8;

                *ptra = flag;

                ptrs->Zrot = zr;

                ptrs->X2D = (S16)Xp;
                ptrs->Y2D = (S16)Yp;

            } else {
                *ptra = 0;
            }

            ptra++;
            ptrm++;
            ptrs++;
        }
    }

    PtrMap = GroundTexture;
    RepMask = 0xFFFF;

    /* The GPU draws the cube unfogged (its CLUT at the normal level) and fogs
       by distance itself. */
    TerrainGpuPalette = PtrPal;
    TerrainGpu_BeginCube(MapSommetY, GroundTexture, PtrCLUTFog + ((PalLevel << 8) & 0xF000), StartZFog, ClipZFar,
                         FogCoul);

    ptra = &SommetAffichable[0][0];
    ptrs = &SommetRot[0][0];

    for (S32 z = 0; z < NB_COTE; z++) {
        for (S32 x = 0; x < NB_COTE; x++) {
            S32 mask;

            mask = MaskVisible(ptra);

            switch (mask & 0xFF) {
            case 32:
                break;
            case 16:
                DrawFeuillePolyZBuf(x, z, ptrs);
                break;
            case 0:
                if (mask) {
                    FillBlackPolyZBuf(x, z, ptrs, ptra);
                }
                break;
            }

            ptra++;
            ptrs++;
        }

        ptra++;
        ptrs++;
    }

    TerrainGpu_End();

    DrawOneSea(Y_SEA);

#ifdef LBA_EDITOR
    if (FlagInfos & INFO_DECORS) {
        AffichageObjetDecorsZV();
    }
#endif

    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);
}

/*

void	AffichageTerrainZBuf( void )
{
	S16		*ptrm	;
	U8		*ptra	;
	T_SOMMET_ROT	*ptrs	;

	// affichage des objets du decors
	AffichageObjetDecorsZBuf() ;

	// calcul les coordonnées 2D de chaque points
	ptra = &SommetAffichable[0][0];
	ptrm = MapSommetY ;
	ptrs = &SommetRot[0][0] ;

	for (S32 z=0; z<=NB_COTE*512; z+=512)
	{
		for (S32 x=0; x<=NB_COTE*512; x+=512)
		{
			S32	zr ;

			LongWorldRotatePoint(x, *ptrm, z);

			zr = CameraZr - Z0 ;// l'ecran

			// cone clipping

			if( (zr<ClipZFar)
			AND (zr>NearClip)
			AND LongProjectPoint( X0,Y0,Z0 ) )
			{
				U8	flag	;

				flag = 16	;

				if( Xp < ClipXMin )	flag |= 1 ;
				if( Xp > ClipXMax )	flag |= 2 ;
				if( Yp < ClipYMin )	flag |= 4 ;
				if( Yp > ClipYMax )	flag |= 8 ;

				*ptra		= flag 	;

				ptrs->Zrot 	= zr;

				// Add 0.5 bias before truncation to reduce edge artifacts from coordinate precision loss
				ptrs->X2D	= (S16)(Xp + 0.5);
				ptrs->Y2D	= (S16)(Yp + 0.5);

			}
			else
			{
				*ptra = 0	;
			}

			ptra++;
			ptrm++;
			ptrs++;
		}
	}

	PtrMap = GroundTexture ;
	RepMask = 0xFFFF ;

	ptra  = &SommetAffichable[0][0] ;
	ptrs  = &SommetRot[0][0] ;

	for(z=0; z<NB_COTE; z++)
	{
		for(S32 x=0; x<NB_COTE; x++)
		{
			if (MaskVisible(ptra) == 16)
			{
				DrawFeuillePolyZBuf( x, z, ptrs) ;
			}

			ptra++	;
			ptrs++	;
		}

		ptra++	;
		ptrs++	;
	}

	BoxStaticAdd( 0, 0, ModeDesiredX-1, ModeDesiredY-1 ) ;
}

*/
/*──────────────────────────────────────────────────────────────────────────*/
//****************************************************************************

void AffichageTerrainRapide(void) {
    for (S32 xi = 0; xi <= NB_COTE; xi++) {
        for (S32 zi = 0; zi <= NB_COTE; zi++) {
            LongWorldRotatePoint(xi * 512,
                                 (S32)MapSommetY[zi * 65 + xi],
                                 zi * 512);

            if (LongProjectPoint(X0, Y0, Z0)) {
                Plot(Xp, Yp, WHITE);
            }
        }
    }
}

/*──────────────────────────────────────────────────────────────────────────*/
/*──────────────────────────────────────────────────────────────────────────*/

void CopyBlock256(U8 *sour, U8 *dest) {
    S32 y;

    for (y = 0; y < 32; y++) {
        memcpy(dest, sour, 32);
        dest += 256;
        sour += 256;
    }
}

/*──────────────────────────────────────────────────────────────────────────*/
S32 RefreshAniPoly = TRUE;
S32 NbToRefresh = 0;

/*──────────────────────────────────────────────────────────────────────────*/
void ClearAniPoly(void) {
    NbAniPoly = 0;
    PtrAniPoly = PtrPolySea;
    RefreshAniPoly = NbAniPoly;
    NbToRefresh = NbAniPoly / 4;
}

/*──────────────────────────────────────────────────────────────────────────*/
void DoAnimatedPolys() {
    if (!NbAniPoly OR !MaxPolySea)
        return;

    if (TimerRefHR > TimerAniPoly) {
        CntAniPoly8++;
        CntAniPoly8 &= 7; // 0-8
        CntAniPoly6++;
        CntAniPoly6 %= 6; // 0-5

        TimerAniPoly = TimerRefHR + 1000 / 4; // voir pour nb frame/sec

        if (OffsetAniPoly1 != -1) {
            CopyBlock256(SkySeaTexture + 128 * 256 + CntAniPoly8 * 32,
                         GroundTexture + OffsetAniPoly1);
        }

        if (OffsetAniPoly2 != -1) {
            CopyBlock256(SkySeaTexture + (128 + 32) * 256 + CntAniPoly8 * 32,
                         GroundTexture + OffsetAniPoly2);
        }

        if (OffsetAniPoly3 != -1) {
            CopyBlock256(SkySeaTexture + (128 + 32 + 32) * 256 + 32 * 2 + CntAniPoly6 * 32,
                         GroundTexture + OffsetAniPoly3);
        }

        if (OffsetAniPoly4 != -1) {
            CopyBlock256(SkySeaTexture + (128 + 32 + 32 + 32) * 256 + 32 * 2 + CntAniPoly6 * 32,
                         GroundTexture + OffsetAniPoly4);
        }

        RefreshAniPoly = NbAniPoly;
        NbToRefresh = NbAniPoly / 4;
    }
}

/*──────────────────────────────────────────────────────────────────────────*/

void DrawAnimatedPolys() {
    S32 n;
    S_ANI_POLY *ptr;

    if (!NbAniPoly OR !MaxPolySea OR !RefreshAniPoly)
        return;

    PtrMap = GroundTexture;
    RepMask = 0xFFFF;

    Switch_Fillers(FILL_POLY_FOG_NZW);

    if (NbToRefresh > RefreshAniPoly)
        NbToRefresh = RefreshAniPoly;

    ptr = PtrAniPoly = (S_ANI_POLY *)&PtrPolySea[RefreshAniPoly - 1];
    for (n = 0; n < NbToRefresh; n++, ptr--) {
        UnsetClip();

        SetClip(ptr->X0, ptr->Y0, ptr->X1, ptr->Y1);

        if (ClipXMin <= ClipXMax
                            AND ClipYMin <= ClipYMax) {
            SetCLUT(ptr->Fog);

            Fill_Poly(ptr->Type, ptr->Color, 3, ptr->Tab_Points);

            /* Same water triangle, next texture frame: the GPU reads the
               animated page itself, so the pixels keep their tags. */
            GpuObj_RefreshTags(ClipXMin, ClipYMin, ClipXMax, ClipYMax);

            BoxStaticAdd(ClipXMin, ClipYMin, ClipXMax, ClipYMax);
        }

        RefreshAniPoly--;
    }

    // protec au cas ou
    if (RefreshAniPoly < 0)
        RefreshAniPoly = 0;

    // Met a jour Screen pour recouvrements
    BoxBlitStaticListToScreen();

    Switch_Fillers(FILL_POLY_TEXTURES);
    SetCLUT(PalLevel);

    UnsetClip();
}

/*──────────────────────────────────────────────────────────────────────────*/

void FiltreAnimatedPolys() {
    S32 n;
    S_ANI_POLY *ptr;

    if (!NbAniPoly OR !MaxPolySea)
        return;

    ptr = PtrAniPoly = (S_ANI_POLY *)PtrPolySea;

    for (n = 0; n < NbAniPoly;) {
        Fill_Poly(POLY_FLAG_ZBUFFER, 0, 3, ptr->Tab_Points);

        if (IsPolygonHidden) {
            if (NbAniPoly > 1) {
                memcpy((char *)ptr, (char *)&PtrPolySea[NbAniPoly - 1], sizeof(S_ANI_POLY));
            }
            NbAniPoly--;
        } else {
            n++;
            ptr++;
        }
    }
}
