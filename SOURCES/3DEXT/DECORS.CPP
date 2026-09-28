#include "DEFINES.H"
#include "../C_EXTERN.H" /* Island */
#include <SVGA/GPUOBJ.H>
#include <OBJECT/AFF_GPU.H>
#include "TERRAIN_GPU.H"
#include "TREES.H"
#include <SVGA/GPUTREE.H>

/*══════════════════════════════════════════════════════════════════════════*
		      █▀▀▀▄ █▀▀▀▀ █▀▀▀▀ █▀▀▀█ █▀▀▀█ ██▀▀▀
		      ██  █ ██▀▀  ██    ██  █ ██▀█▀ ▀▀▀▀█
		      ▀▀▀▀  ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

#ifdef LBA_EDITOR // Special PERSO.EXE
#define INFO_DECORS 1024
extern S32 FlagInfos;

#define SIZE_NAME 19

typedef struct
{
    char Name[SIZE_NAME + 1];
    S16 Value;
} T_VAR;

extern T_VAR ListVarGame[];
#else
extern S16 ListVarGame[];
#endif

extern S32 FlagMasque;

#define MAX_SOUS_OBJET 20
#define SIZE_BRICK_Y 256

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
// affiche les objets de decors

#ifdef LBA_EDITOR // uniquement PERSO
extern void ClearScreenMinMax();
extern void AdjustScreenMinMax();

S32 NumDecorsDraw;

//****************************************************************************
void DrawCubeNoBox(S32 ox, S32 oy, S32 oz,
                   S32 xw0, S32 yw0, S32 zw0,
                   S32 xw1, S32 yw1, S32 zw1) {
    S32 x0, y0, x1, y1, x2, y2, x3, y3, x4, y4, x5, y5, x6, y6, x7, y7;
    S16 coul = 11;
    S16 coul1 = 13;

    ClearScreenMinMax();
    LongWorldRotatePoint(ox, oy, oz);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    Rect(Xp - 3, Yp - 3, Xp + 3, Yp + 3, coul); // point org

    CoulText(GREEN, -1);
    GraphPrintf(FALSE, Xp, Yp, "%d", NumDecorsDraw);

    LongWorldRotatePoint(xw0, yw0, zw0);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x0 = Xp;
    y0 = Yp;
    LongWorldRotatePoint(xw1, yw0, zw0);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x1 = Xp;
    y1 = Yp;
    LongWorldRotatePoint(xw1, yw0, zw1);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x2 = Xp;
    y2 = Yp;
    LongWorldRotatePoint(xw0, yw0, zw1);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x3 = Xp;
    y3 = Yp;
    LongWorldRotatePoint(xw0, yw1, zw0);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x4 = Xp;
    y4 = Yp;
    LongWorldRotatePoint(xw1, yw1, zw0);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x5 = Xp;
    y5 = Yp;
    LongWorldRotatePoint(xw1, yw1, zw1);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x6 = Xp;
    y6 = Yp;
    LongWorldRotatePoint(xw0, yw1, zw1);
    if (!LongProjectPoint(X0, Y0, Z0))
        return;
    AdjustScreenMinMax();
    x7 = Xp;
    y7 = Yp;

    Line(x0, y0, x1, y1, coul);
    Line(x1, y1, x2, y2, coul);
    Line(x2, y2, x3, y3, coul);
    Line(x3, y3, x0, y0, coul);

    Line(x0, y0, x4, y4, coul1);
    Line(x1, y1, x5, y5, coul1);
    Line(x2, y2, x6, y6, coul1);
    Line(x3, y3, x7, y7, coul1);
    Line(x4, y4, x5, y5, coul1);
    Line(x5, y5, x6, y6, coul1);
    Line(x6, y6, x7, y7, coul1);
    Line(x7, y7, x4, y4, coul1);
}

// Version ZVs absolues
void AffZVObjet(T_DECORS *ptrobj) {
    DrawCubeNoBox(ptrobj->Xworld, ptrobj->Yworld, ptrobj->Zworld,
                  ptrobj->XMin, ptrobj->YMin, ptrobj->ZMin,
                  ptrobj->XMax, ptrobj->YMax, ptrobj->ZMax);
}
#endif

/*══════════════════════════════════════════════════════════════════════════*/
void FixeObjetsDecorsInvisibles(void) {
    S32 i;
    T_DECORS *ptrobj;
    S32 numvar;

    ptrobj = ListDecors;

    for (i = 0; i < NbObjDecors; i++, ptrobj++) {
        ptrobj->Body &= ~(DEC_DRAWN | DEC_INVISIBLE);

        // Regarde si objet visible/invisible
        numvar = ptrobj->Beta >> 16;

        if (numvar) {
#ifdef LBA_EDITOR
            if (((numvar < 0) AND !ListVarGame[-numvar].Value)
                    OR((numvar >= 0) AND ListVarGame[numvar].Value))
#else
            if (((numvar < 0) AND !ListVarGame[-numvar])
                    OR((numvar >= 0) AND ListVarGame[numvar]))
#endif
                ptrobj->Body |= DEC_INVISIBLE;
        }
    }
}

/*══════════════════════════════════════════════════════════════════════════*/
// sous routine de tri temporaire

int sub_qsort_decors(const void *a, const void *b) {
    return (((T_LIST_TRI *)a)->Zrot - ((T_LIST_TRI *)b)->Zrot);
}

/*══════════════════════════════════════════════════════════════════════════*/
// sous routine de tri temporaire

int sub_qsort_decors_zbuf(const void *a, const void *b) {
    return (((T_LIST_TRI *)b)->Zrot - ((T_LIST_TRI *)a)->Zrot);
}

/*══════════════════════════════════════════════════════════════════════════*/

// Version ZVs absolues
int sub_qsort_zv_decors(const void *a, const void *b) {
    T_DECORS *ptrobja;
    T_DECORS *ptrobjb;

    S32 x0a, y0a, z0a, x1a, y1a, z1a;
    S32 x0b, y0b, z0b, x1b, y1b, z1b;

    ptrobja = ((T_LIST_TRI *)a)->PtrDec;
    ptrobjb = ((T_LIST_TRI *)b)->PtrDec;

    x0a = ptrobja->XMin;
    y0a = ptrobja->YMin;
    z0a = ptrobja->ZMin;
    x1a = ptrobja->XMax;
    y1a = ptrobja->YMax;
    z1a = ptrobja->ZMax;

    x0b = ptrobjb->XMin;
    y0b = ptrobjb->YMin;
    z0b = ptrobjb->ZMin;
    x1b = ptrobjb->XMax;
    y1b = ptrobjb->YMax;
    z1b = ptrobjb->ZMax;

    if (x0a >= x1b)
        goto pas_rec_sur_x;
    if (x1a <= x0b)
        goto pas_rec_sur_x;

    if (z0a >= z1b)
        goto pas_rec_sur_z;
    if (z1a <= z0b)
        goto pas_rec_sur_z;

    //	ordre sur y

    // affinement decision sur une des 2 faces paralelle
    if (abs(y0a - CameraY) < abs(y1a - CameraY))
        return (abs(y0b - CameraY) - abs(y0a - CameraY));
    else
        return (abs(y1b - CameraY) - abs(y1a - CameraY));

pas_rec_sur_z:

    //	ordre sur z

    if (abs(z0a - CameraZ) < abs(z1a - CameraZ))
        return (abs(z0b - CameraZ) - abs(z0a - CameraZ));
    else
        return (abs(z1b - CameraZ) - abs(z1a - CameraZ));

pas_rec_sur_x:

    if (z0a >= z1b)
        goto pas_rec_sur_z_2;
    if (z1a <= z0b)
        goto pas_rec_sur_z_2;

    if (abs(x0a - CameraZ) < abs(x1a - CameraZ))
        return (abs(x0b - CameraX) - abs(x0a - CameraX));
    else
        return (abs(x1b - CameraX) - abs(x1a - CameraX));

pas_rec_sur_z_2:

    // plus de methode dispo (obj en collision), utilise zrot

    return (((T_LIST_TRI *)b)->Zrot - ((T_LIST_TRI *)a)->Zrot);
}

/*══════════════════════════════════════════════════════════════════════════*/

#ifdef LBA_EDITOR
void AffichageObjetDecorsZV(void) {
    S32 i;
    T_LIST_TRI *ptrtri;
    T_DECORS *ptrobj;

    // affiche les zv des objets statiques
    ptrtri = ListTriExt;
    for (i = 0; i < NbObjScreen; i++, ptrtri++) {
        ptrobj = ptrtri->PtrDec;

        if (ptrobj->Body & DEC_DRAWN) {
            UnsetClip();
            NumDecorsDraw = ptrtri->Num;
            AffZVObjet(ptrobj);
        }
    }

    UnsetClip();
}
#endif

/*══════════════════════════════════════════════════════════════════════════*/

// Version masques compris

/* The island decor models that are lamps, found by looking at the islands: the
   GPU renderer makes their bright glass glow and cast light. */
typedef struct {
    S32 Island;
    S32 Body;
} T_LAMP_MODEL;

static const T_LAMP_MODEL kLampModels[] = {
    {0, 26}, /* Citadel Island street lamp */
    {2, 0},  /* Desert Island lighthouse */
    {3, 19}, /* Emerald Moon ring light */
    {3, 27}, /* Emerald Moon square light */
    {3, 29}, /* Emerald Moon lanterns */
    {6, 24}, /* dome lamp */
};

static S32 IsLampModel(S32 island, S32 body) {
    for (S32 i = 0; i < (S32)(sizeof(kLampModels) / sizeof(kLampModels[0])); i++) {
        if (kLampModels[i].Island == island && kLampModels[i].Body == body) {
            return TRUE;
        }
    }
    return FALSE;
}

/*──────────────────────────────────────────────────────────────────────────*/
/* The decor the GPU draws alone. Its bodies are kept on the GPU in model space
   and placed there; the software only needs their depth, for DrawRecover, and
   draws it the first time it is asked for, with the camera the scenery pass
   had: the depth buffer keeps the nearest of what is written into it, so the
   order does not change what it ends up holding. */
S32 DecorsGpuHorizon = FALSE;

typedef struct {
    S32 Index;
    S32 X, Y, Z, Beta;
    S32 CamXr, CamYr, CamZr;
    S32 Box[4];
    S32 Done;
} T_DEFER_DECOR;
#define DEFER_MAX_DECORS 4096
static T_DEFER_DECOR s_deferDecors[DEFER_MAX_DECORS];
static S32 s_nbDeferDecors = 0;
static TYPE_MAT s_deferMat;
static S32 s_deferProj[6]; ///< XCentre, YCentre, NearClip, LFactorX, LFactorY, TypeProj
static float s_deferRatio[2];

void DecorsGpu_Reset(void) {
    s_nbDeferDecors = 0;
}

S32 DecorsGpu_Pending(void) {
    return s_nbDeferDecors > 0;
}

void DecorsGpu_EnsureDepth(S32 x0, S32 y0, S32 x1, S32 y1) {
    if (s_nbDeferDecors == 0) {
        return;
    }
    const TYPE_MAT mat = MatriceWorld;
    const S32 cam[3] = {CameraXr, CameraYr, CameraZr};
    const S32 proj[6] = {XCentre, YCentre, NearClip, LFactorX, LFactorY, TypeProj};
    const float ratio[2] = {FRatioX, FRatioY};
    const S32 clip[4] = {ClipXMin, ClipYMin, ClipXMax, ClipYMax};
    U8 *const map = ObjPtrMap;
    if (proj[5] != s_deferProj[5]) {
        /* Another projection is in use (a menu): wait for the scene's. */
        return;
    }
    MatriceWorld = s_deferMat;
    XCentre = s_deferProj[0];
    YCentre = s_deferProj[1];
    NearClip = s_deferProj[2];
    LFactorX = s_deferProj[3];
    LFactorY = s_deferProj[4];
    FRatioX = s_deferRatio[0];
    FRatioY = s_deferRatio[1];
    ObjPtrMap = ObjTexture;
    AffGpuSuspended = TRUE;
    for (S32 n = 0; n < s_nbDeferDecors; n++) {
        T_DEFER_DECOR *d = &s_deferDecors[n];
        if (d->Done || d->Box[0] > x1 || d->Box[2] < x0 || d->Box[1] > y1 || d->Box[3] < y0) {
            continue;
        }
        d->Done = TRUE;
        CameraXr = d->CamXr;
        CameraYr = d->CamYr;
        CameraZr = d->CamZr;
        UnsetClip();
        BodyDisplay(d->X, d->Y, d->Z, 0, d->Beta, 0, HQR_Get(HQR_Isle_Obj, d->Index));
    }
    AffGpuSuspended = FALSE;
    ObjPtrMap = map;
    MatriceWorld = mat;
    CameraXr = cam[0];
    CameraYr = cam[1];
    CameraZr = cam[2];
    XCentre = proj[0];
    YCentre = proj[1];
    NearClip = proj[2];
    LFactorX = proj[3];
    LFactorY = proj[4];
    FRatioX = ratio[0];
    FRatioY = ratio[1];
    ClipXMin = clip[0];
    ClipYMin = clip[1];
    ClipXMax = clip[2];
    ClipYMax = clip[3];
}

/* The island's model file, for the GPU's model store: two files share an
   island's number (Citadel Island before and after the storm) and number their
   models differently. */
static S32 IslandFileKey(void) {
    U32 h = 2166136261u;
    for (const char *c = IslandName; *c != 0; c++) {
        h = (h ^ (U32)(*c >= 'A' && *c <= 'Z' ? *c - 'A' + 'a' : *c)) * 16777619u;
    }
    return (S32)(h & 0x7FFFFFFF);
}

/* Leaves the decor to the GPU when it can draw it alone: TRUE when nothing is
   left for BodyDisplay to do. */
static S32 DecorToGpu(T_DECORS *ptrobj) {
    if (!(TerrainGpuSkipFill && DecorsGpuHorizon) || GpuObjLampModel || s_nbDeferDecors >= DEFER_MAX_DECORS) {
        return FALSE;
    }
    const S32 index = (S32)(ptrobj->Body & 0xFFFF);
    const S32 beta = ptrobj->Beta & 0xFFFF;
    S32 box[4];
    const S32 drawn = AffGpu_DecorDraw(IslandFileKey(), index, HQR_Get(HQR_Isle_Obj, index), ptrobj->Xworld,
                                       ptrobj->Yworld, ptrobj->Zworld, beta, box);
    if (drawn < 0) {
        return FALSE;
    }
    if (drawn > 0) {
        if (s_nbDeferDecors == 0) {
            s_deferMat = MatriceWorld;
            s_deferProj[0] = XCentre;
            s_deferProj[1] = YCentre;
            s_deferProj[2] = NearClip;
            s_deferProj[3] = LFactorX;
            s_deferProj[4] = LFactorY;
            s_deferProj[5] = TypeProj;
            s_deferRatio[0] = FRatioX;
            s_deferRatio[1] = FRatioY;
        }
        T_DEFER_DECOR *d = &s_deferDecors[s_nbDeferDecors++];
        d->Index = index;
        d->X = ptrobj->Xworld;
        d->Y = ptrobj->Yworld;
        d->Z = ptrobj->Zworld;
        d->Beta = beta;
        d->CamXr = CameraXr;
        d->CamYr = CameraYr;
        d->CamZr = CameraZr;
        memcpy(d->Box, box, sizeof(d->Box));
        d->Done = FALSE;
    }
    return TRUE;
}

void AffichageObjetDecorsZBuf(void) {
    S32 i;
    T_LIST_TRI *ptrtri;
    T_DECORS *ptrobj;
    S32 clipzstart;
    S32 numvar;
    S32 zr;

    ObjPtrMap = ObjTexture; // texture obj

    ptrtri = ListTriExt;
    ptrobj = ListDecors;

    // projection des centres
    NbObjScreen = 0;
    for (i = 0; i < NbObjDecors; i++, ptrobj++) {
        ptrobj->Body &= ~(DEC_DRAWN | DEC_INVISIBLE);

        // Regarde si objet visible/invisible
        numvar = ptrobj->Beta >> 16;

        if (numvar) {
#ifdef LBA_EDITOR
            if (((numvar < 0) AND !ListVarGame[-numvar].Value)
                    OR((numvar >= 0) AND ListVarGame[numvar].Value))
#else
            if (((numvar < 0) AND !ListVarGame[-numvar])
                    OR((numvar >= 0) AND ListVarGame[numvar]))
#endif
                ptrobj->Body |= DEC_INVISIBLE;
            //			else	ptrobj->Body &= ~(DEC_INVISIBLE) ;
        }

        // SEB le 12/04/96
        if (!(ptrobj->Body & DEC_INVISIBLE)) {
            LongWorldRotatePoint(ptrobj->XMin,
                                 ptrobj->YMin,
                                 ptrobj->ZMin);
            zr = CameraZr - Z0;
            if (zr > ClipZFar)
                continue;

            LongWorldRotatePoint(ptrobj->XMin,
                                 ptrobj->YMin,
                                 ptrobj->ZMax);
            zr = CameraZr - Z0;
            if (zr > ClipZFar)
                continue;

            LongWorldRotatePoint(ptrobj->XMax,
                                 ptrobj->YMin,
                                 ptrobj->ZMin);
            zr = CameraZr - Z0;
            if (zr > ClipZFar)
                continue;

            LongWorldRotatePoint(ptrobj->XMax,
                                 ptrobj->YMin,
                                 ptrobj->ZMax);
            zr = CameraZr - Z0;
            if (zr > ClipZFar)
                continue;

            // point chaud obj pour calcul profondeur fog
            LongWorldRotatePoint(ptrobj->Xworld,
                                 ptrobj->Yworld,
                                 ptrobj->Zworld);

            if (LongProjectPoint(X0, Y0, Z0)) {
                // Test preclipping a mettre ici
                //				if( (Xp>VIEW_EXT_X0) AND (Xp<VIEW_EXT_X1)
                //				AND (Yp>VIEW_EXT_Y0) AND (Yp<VIEW_EXT_Y1) )
                {
                    // add dans listtri
                    ptrtri->Zrot = CameraZr - Z0;
                    ptrtri->Num = i;
                    ptrtri->PtrDec = ptrobj;
                    ptrtri++;
                    NbObjScreen++;
                }
            }
        }
    }

    if (!NbObjScreen)
        return;

    // trie les objet en z 'front to back'
    qsort(ListTriExt, NbObjScreen, sizeof(T_LIST_TRI), sub_qsort_decors_zbuf);

    clipzstart = ClipZFar - StartZFog;

    //	Switch_Fillers( FILL_POLY_FOG_ZBUFFER );

    // affiche les objets statiques
    ptrtri = ListTriExt;
    for (i = 0; i < NbObjScreen; i++, ptrtri++) {
        UnsetClip();
        ptrobj = ptrtri->PtrDec;

        SetCLUT(PalLevel +
                16 * BoundRegleTrois(0, 15,
                                     clipzstart,
                                     ptrtri->Zrot - StartZFog));

        /* Tell the GPU capture when this decor is a lamp: its bright glass glows. */
        GpuObjLampModel = IsLampModel(Island, (S32)(ptrobj->Body & 0xFFFF));
        GpuObjLampPalette = PtrPal;
        /* And when it is a tree: its leaves are shaded as foliage, it sways. */
        U32 foliage = 0;
        if (Trees_Find((S32)(ptrobj->Body & 0xFFFF), &foliage)) {
            GpuTree_Begin(foliage, ptrobj->Xworld, ptrobj->Zworld);
        } else {
            GpuTree_End();
        }
        if (DecorToGpu(ptrobj)) {
            continue;
        }
        if (BodyDisplay(ptrobj->Xworld,
                        ptrobj->Yworld,
                        ptrobj->Zworld,
                        0,
                        (ptrobj->Beta & 0xFFFF), 0,
                        HQR_Get(HQR_Isle_Obj, ptrobj->Body & 0xFFFF))) {
            SetClip(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax);
            if (ClipXMin <= ClipXMax
                                AND ClipYMin <= ClipYMax) {
                ptrobj->Body |= DEC_DRAWN;
            }
        }
    }
    GpuObjLampModel = FALSE;
    GpuTree_End();

    //	Switch_Fillers( FILL_TEXTURES_ON );

    UnsetClip();
}

/*══════════════════════════════════════════════════════════════════════════*
	  █▀▀▀▀ █▀▀▀█ █     █      █    ██▀▀▀  █    █▀▀▀█ ██▄ █ ██▀▀▀
	  ██    ██  █ ██    ██     ██   ▀▀▀▀█  ██   ██  █ ██▀██ ▀▀▀▀█
	  ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀  ▀▀   ▀▀▀▀▀  ▀▀   ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

// Version ZVs absolues précalculées
// on teste par rapport a un seul point
S32 TestZVDecors(S32 xw, S32 yw, S32 zw, T_DECORS *objet) {
    if (objet->Body & DEC_INVISIBLE)
        return FALSE;

    if (objet->XMin > xw)
        return FALSE;
    if (objet->XMax < xw)
        return FALSE;

    if (objet->YMin > yw)
        return FALSE;
    if (objet->YMax < yw)
        return FALSE;

    if (objet->ZMin > zw)
        return FALSE;
    if (objet->ZMax < zw)
        return FALSE;

    return TRUE;
}
