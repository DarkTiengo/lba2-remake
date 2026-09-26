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

   Outdoors, with the software leaving the ground to the GPU, a cube's land is
   one draw the GPU grows from records of the cube (TerrainGpu_LandDraw), and
   nothing here walks its cells. What the classic walk still left behind that
   the game reads is the terrain's depth: DrawRecover decides from it whether
   the hero stands behind a hill, and the camera acts on that. So the cube is
   kept as the walk would have seen it -- its projected grid, which points are
   visible, its half-cells -- and its depth is filled in later, a cell at a
   time, only where something asks for it (TerrainGpu_EnsureDepth), with the
   filler, the corners and the clip window the walk would have used: the
   depth is the classic one to the bit. The depth buffer keeps the nearest of
   what is written into it, so filling later does not change what ends up in it. */

S32 TerrainGpuSkipFill = FALSE;

S32 TerrainGpu_CanSkipFill(void) {
    return TerrainGpuFastFill AND GpuObj_Active() AND GpuObjShadows AND PtrZBuffer != NULL;
}

static void ResetDeferredDepth(void);

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

/* Fills the triangle in Tab_Points. */
static inline S32 TerrainFillPoly(S32 type, S32 color, S32 gpuCovers, S32 animated) {
    (void)animated;
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
                        DecorsGpuHorizon = TRUE;
                        AffichageTerrainZBuf();
                        DecorsGpuHorizon = FALSE;
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

/*──────────────────────────────────────────────────────────────────────────*/
/* The cubes whose land the GPU drew this pass, kept for their depth. */

#define DEFER_MAX_CUBES 64
#define DEFER_BLOCK 8 ///< cells a side of a block, the unit asked about first

typedef struct {
    S16 Height[65 * 65];            ///< MapSommetY
    T_HALF_POLY Poly[64 * 64 * 2];  ///< MapPolyGround
    /* The camera the cube was drawn from, as FastGrid reads it. */
    double M[9];                    ///< MatriceWorld, row after row
    S32 CamXr, CamYr, CamZr, Near, Far, CentreX, CentreY, Guard16;
    double RatioX, RatioY;
    S32 Clip[4];
    /* The grid, placed a point at a time as it is asked for. */
    T_SOMMET_ROT Rot[65 * 65];      ///< SommetRot as the walk would have read it
    U8 Vis[65 * 65];                ///< SommetAffichable
    U8 Placed[65 * 65];
    S16 Box[(64 / DEFER_BLOCK) * (64 / DEFER_BLOCK)][4]; ///< what each block's cells can fill, x0 > x1 for nothing
    U8 Done[64 * 64];               ///< the cell's depth is in
    S32 Boxed;                      ///< Box is worked out
} T_DEFER_CUBE;

static T_DEFER_CUBE *s_deferCubes = NULL;
static S32 s_nbDeferCubes = 0;
static S32 s_deferClip[4];
static U8 *s_depthScratch = NULL; /* where the depth fill's colour goes */
static size_t s_depthScratchSize = 0;

static void ResetDeferredDepth(void) {
    s_nbDeferCubes = 0;
    DecorsGpu_Reset();
}

/* FastGrid's placement of grid point (ix, iz), the same arithmetic in the same
   order, so the point lands where the walk would have had it. A point it
   leaves alone (behind the near plane, past the far one) keeps what the grid
   held; no cell the depth is filled from reads one. */
static void PlacePoint(T_DEFER_CUBE *c, S32 ix, S32 iz) {
    const S32 k = iz * 65 + ix;
    if (c->Placed[k]) {
        return;
    }
    c->Placed[k] = TRUE;
    const double x = (double)(ix * 512), z = (double)(iz * 512);
    const double y = (double)c->Height[k];
    const S32 X0f = (S32)lrint(x * c->M[0] + y * c->M[1] + z * c->M[2]);
    const S32 Y0f = (S32)lrint(x * c->M[3] + y * c->M[4] + z * c->M[5]);
    const S32 Z0f = (S32)lrint(x * c->M[6] + y * c->M[7] + z * c->M[8]);
    const S32 zr = c->CamZr - Z0f;
    if (zr <= c->Near) {
        c->Vis[k] = 32;
        return;
    }
    if (zr >= c->Far) {
        c->Vis[k] = 0;
        return;
    }
    const double factor = c->RatioX / (double)zr;
    const S32 xp = c->CentreX + (S32)lrint(((double)X0f - (double)c->CamXr) * factor);
    const S32 yp = c->CentreY + (S32)lrint(((double)Y0f - (double)c->CamYr) * factor * c->RatioY);
    if (c->Guard16 AND (xp != (S32)(S16)xp OR yp != (S32)(S16)yp)) {
        c->Vis[k] = 32;
        return;
    }
    U8 flag = 16;
    if (xp < c->Clip[0])
        flag |= 1;
    if (xp > c->Clip[2])
        flag |= 2;
    if (yp < c->Clip[1])
        flag |= 4;
    if (yp > c->Clip[3])
        flag |= 8;
    c->Vis[k] = flag;
    c->Rot[k].Zrot = zr;
    c->Rot[k].X2D = (S16)xp;
    c->Rot[k].Y2D = (S16)yp;
}

/* What the walk does with cell (xi, zi): 1 its two triangles, 2 the black
   fill under the near edge, 0 nothing. */
static S32 CellKind(T_DEFER_CUBE *c, S32 xi, S32 zi) {
    PlacePoint(c, xi, zi);
    PlacePoint(c, xi + 1, zi);
    PlacePoint(c, xi, zi + 1);
    PlacePoint(c, xi + 1, zi + 1);
    const U32 mask = MaskVisible((U8 *)&c->Vis[zi * 65 + xi]);
    if ((mask & 0xFF) == 16) {
        return 1;
    }
    return ((mask & 0xFF) == 0 AND mask) ? 2 : 0;
}

/* The screen box cell (xi, zi) fills, FALSE when it fills nothing. */
static S32 CellBox(T_DEFER_CUBE *c, S32 xi, S32 zi, S32 kind, S32 box[4]) {
    if (kind == 0) {
        return FALSE;
    }
    const T_SOMMET_ROT *r = &c->Rot[zi * 65 + xi];
    const T_SOMMET_ROT *corner[4] = {&r[0], &r[65], &r[66], &r[1]};
    box[0] = box[2] = corner[0]->X2D;
    box[1] = box[3] = corner[0]->Y2D;
    for (S32 k = 1; k < 4; k++) {
        box[0] = ade_min(box[0], (S32)corner[k]->X2D);
        box[2] = ade_max(box[2], (S32)corner[k]->X2D);
        box[1] = ade_min(box[1], (S32)corner[k]->Y2D);
        box[3] = ade_max(box[3], (S32)corner[k]->Y2D);
    }
    if (kind == 2) {
        box[3] = (S32)ModeDesiredY - 1; /* the black fill reaches the bottom of the frame */
    }
    return TRUE;
}

/* Where each block of cells can reach on screen: the projection of its box in
   the world (the cells' extent, their lowest and highest heights), the whole
   frame when some of it is behind the near plane -- which is also the only
   block that can hold a black fill. */
static void BoxCube(T_DEFER_CUBE *c) {
    const S32 blocks = 64 / DEFER_BLOCK;
    for (S32 b = 0; b < blocks * blocks; b++) {
        const S32 bx = (b % blocks) * DEFER_BLOCK, bz = (b / blocks) * DEFER_BLOCK;
        S32 lo = 32767, hi = -32768;
        for (S32 iz = bz; iz <= bz + DEFER_BLOCK; iz++) {
            for (S32 ix = bx; ix <= bx + DEFER_BLOCK; ix++) {
                lo = ade_min(lo, (S32)c->Height[iz * 65 + ix]);
                hi = ade_max(hi, (S32)c->Height[iz * 65 + ix]);
            }
        }
        S16 *bb = c->Box[b];
        double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
        S32 near = FALSE;
        for (S32 k = 0; k < 8 AND !near; k++) {
            const double x = (double)((bx + (k & 1) * DEFER_BLOCK) * 512);
            const double z = (double)((bz + (k >> 1 & 1) * DEFER_BLOCK) * 512);
            const double y = (double)((k & 4) ? hi : lo);
            const double zr = (double)c->CamZr - (x * c->M[6] + y * c->M[7] + z * c->M[8]);
            if (zr <= (double)c->Near + 1.0) {
                near = TRUE;
                break;
            }
            const double f = c->RatioX / zr;
            const double sx = (double)c->CentreX + (x * c->M[0] + y * c->M[1] + z * c->M[2] - (double)c->CamXr) * f;
            const double sy =
                (double)c->CentreY + (x * c->M[3] + y * c->M[4] + z * c->M[5] - (double)c->CamYr) * f * c->RatioY;
            x0 = sx < x0 ? sx : x0;
            x1 = sx > x1 ? sx : x1;
            y0 = sy < y0 ? sy : y0;
            y1 = sy > y1 ? sy : y1;
        }
        if (near) {
            bb[0] = bb[1] = -32768;
            bb[2] = bb[3] = 32767;
            continue;
        }
        /* A pixel of rounding on each side. */
        bb[0] = (S16)ade_max(-32768.0, x0 - 2.0);
        bb[1] = (S16)ade_max(-32768.0, y0 - 2.0);
        bb[2] = (S16)ade_min(32767.0, x1 + 2.0);
        bb[3] = (S16)ade_min(32767.0, y1 + 2.0);
    }
    c->Boxed = TRUE;
}

/* A cell's depth, DrawFeuillePolyZBuf's triangles without their colour. */
static void DepthCell(T_DEFER_CUBE *c, S32 xi, S32 zi) {
    static const U8 kCorners[2][2][3] = {{{0, 1, 2}, {2, 3, 0}}, {{3, 0, 1}, {1, 2, 3}}};
    const T_HALF_POLY *ptrh = &c->Poly[zi * (64 * 2) + xi * 2];
    if (!(ptrh->PolyFlag + ptrh->TexFlag + (ptrh + 1)->PolyFlag + (ptrh + 1)->TexFlag)) {
        return;
    }
    const T_SOMMET_ROT *ptrs = &c->Rot[zi * 65 + xi];
    const T_SOMMET_ROT *corner[4] = {&ptrs[0], &ptrs[65], &ptrs[66], &ptrs[1]};
    for (S32 half = 0; half < 2; half++) {
        const T_HALF_POLY *h = ptrh + half;
        if (!(h->PolyFlag + h->TexFlag)) {
            continue;
        }
        const U8 *k = kCorners[ptrh->Sens][half];
        for (S32 i = 0; i < 3; i++) {
            Tab_Points[i].Pt_XE = corner[k[i]]->X2D;
            Tab_Points[i].Pt_YE = corner[k[i]]->Y2D;
        }
        if (TestVuePoly(Tab_Points)) {
            for (S32 i = 0; i < 3; i++) {
                Tab_Points[i].Pt_ZO = (U16)GET_ZO(corner[k[i]]->Zrot);
            }
            Fill_Poly(POLY_FLAT, 0, 3, Tab_Points);
        }
    }
}

/* Keeps the cube just drawn by the GPU for its depth. */
static void DeferCube(void) {
    if (s_deferCubes == NULL) {
        s_deferCubes = (T_DEFER_CUBE *)malloc(DEFER_MAX_CUBES * sizeof(T_DEFER_CUBE));
        if (s_deferCubes == NULL) {
            return;
        }
    }
    if (s_nbDeferCubes >= DEFER_MAX_CUBES) {
        return;
    }
    if (s_nbDeferCubes == 0) {
        s_deferClip[0] = ClipXMin;
        s_deferClip[1] = ClipYMin;
        s_deferClip[2] = ClipXMax;
        s_deferClip[3] = ClipYMax;
    }
    T_DEFER_CUBE *c = &s_deferCubes[s_nbDeferCubes++];
    memcpy(c->Height, MapSommetY, sizeof(c->Height));
    memcpy(c->Poly, MapPolyGround, sizeof(c->Poly));
    const TYPE_MAT *m = &MatriceWorld;
    const float rows[9] = {m->F.M11, m->F.M12, m->F.M13, m->F.M21, m->F.M22, m->F.M23, m->F.M31, m->F.M32, m->F.M33};
    for (S32 k = 0; k < 9; k++) {
        c->M[k] = (double)rows[k];
    }
    c->CamXr = CameraXr;
    c->CamYr = CameraYr;
    c->CamZr = CameraZr;
    c->Near = NearClip;
    c->Far = ClipZFar;
    c->CentreX = XCentre;
    c->CentreY = YCentre;
    c->Guard16 = FollowCamera;
    c->RatioX = (double)FRatioX;
    c->RatioY = (double)FRatioY;
    c->Clip[0] = ClipXMin;
    c->Clip[1] = ClipYMin;
    c->Clip[2] = ClipXMax;
    c->Clip[3] = ClipYMax;
    memset(c->Placed, 0, sizeof(c->Placed));
    memset(c->Done, 0, sizeof(c->Done));
    c->Boxed = FALSE;
}

void TerrainGpu_EnsureDepth(S32 x0, S32 y0, S32 x1, S32 y1) {
    if ((s_nbDeferCubes == 0 AND !DecorsGpu_Pending()) OR Log == NULL) {
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

    DecorsGpu_EnsureDepth(x0, y0, x1, y1);
    ClipXMin = s_deferClip[0];
    ClipYMin = s_deferClip[1];
    ClipXMax = s_deferClip[2];
    ClipYMax = s_deferClip[3];

    const S32 blocks = 64 / DEFER_BLOCK;
    for (S32 n = 0; n < s_nbDeferCubes; n++) {
        T_DEFER_CUBE *c = &s_deferCubes[n];
        if (!c->Boxed) {
            BoxCube(c);
        }
        for (S32 b = 0; b < blocks * blocks; b++) {
            const S16 *bb = c->Box[b];
            if (bb[0] > x1 OR bb[2] < x0 OR bb[1] > y1 OR bb[3] < y0) {
                continue;
            }
            const S32 bx = (b % blocks) * DEFER_BLOCK, bz = (b / blocks) * DEFER_BLOCK;
            for (S32 zi = bz; zi < bz + DEFER_BLOCK; zi++) {
                for (S32 xi = bx; xi < bx + DEFER_BLOCK; xi++) {
                    if (c->Done[zi * 64 + xi]) {
                        continue;
                    }
                    const S32 kind = CellKind(c, xi, zi);
                    S32 box[4];
                    if (!CellBox(c, xi, zi, kind, box) OR box[0] > x1 OR box[2] < x0 OR box[1] > y1 OR box[3] < y0) {
                        continue;
                    }
                    c->Done[zi * 64 + xi] = TRUE;
                    if (kind == 1) {
                        DepthCell(c, xi, zi);
                    } else {
                        FillBlackPolyZBuf(xi, zi, &c->Rot[zi * 65 + xi], &c->Vis[zi * 65 + xi]);
                    }
                }
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

/* The cube's land, when the GPU draws it whole: its records built the first
   time the cube is seen, one draw, the grass, and the cube kept for its depth. */
static void LandCube(void) {
    static T_TERRAIN_HALF halves[64 * 64 * 2];
    TerrainGpuPalette = PtrPal;
    if (TerrainGpu_LandSlot(MapSommetY)) {
        for (S32 k = 0; k < 64 * 64 * 2; k++) {
            const T_HALF_POLY *h = &MapPolyGround[k];
            T_TERRAIN_HALF *o = &halves[k];
            o->PolyFlag = (U8)h->PolyFlag;
            o->TexFlag = (U8)h->TexFlag;
            o->Bank = (U8)h->Bank;
            o->Step = (U8)h->SampleStep;
            o->CodeJeu = (U8)h->CodeJeu;
            o->Sens = (U8)MapPolyGround[k & ~1].Sens;
            o->IndexTex = (U16)h->IndexTex;
            o->PolyType = ListTypePolygone[h->PolyFlag];
            o->TexType = ListTypePTexture[h->TexFlag + (h->PolyFlag << 2)];
        }
        TerrainGpu_LandBuild(halves, MapIntensity, (const U16 *)ListTexDef);
    }
    /* How near the cube comes: its box's nearest corner, the depth being
       linear in the point. */
    S32 lo = 32767, hi = -32768;
    for (S32 k = 0; k < 65 * 65; k++) {
        lo = ade_min(lo, (S32)MapSommetY[k]);
        hi = ade_max(hi, (S32)MapSommetY[k]);
    }
    const TYPE_MAT *m = &MatriceWorld;
    float nearestF = 1e30f;
    for (S32 k = 0; k < 8; k++) {
        const float x = (float)((k & 1) * 64 * 512), z = (float)((k >> 1 & 1) * 64 * 512);
        const float y = (float)((k & 4) ? hi : lo);
        const float zr = (float)CameraZr - (x * m->F.M31 + y * m->F.M32 + z * m->F.M33);
        nearestF = zr < nearestF ? zr : nearestF;
    }
    const S32 nearest = (S32)nearestF;
    TerrainGpu_LandDraw(GroundTexture, PtrCLUTFog + ((PalLevel << 8) & 0xF000), StartZFog, ClipZFar, FogCoul, nearest);
    TerrainGpu_End();
    DeferCube();
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
    if (TerrainGpuSkipFill) {
        /* The GPU places the land itself; the grid the game's depth needs is
           placed a point at a time when it is asked for (TerrainGpu_EnsureDepth). */
    } else if (GpuObj_Active()) {
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

    if (TerrainGpuSkipFill) {
        LandCube();
    } else {
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
    }

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
