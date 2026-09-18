/*══════════════════════════════════════════════════════════════════════════*

    █▀▀▀▀ █▀▀▀█ █▄ ▄█ █▀▀▀█ █▀▀▀█ █▀▀▀█ ▀▀█▀▀ █▀▀▀▀ █▄ ▄█ █▀▀▀▀ ██▄ █ ▀▀█▀▀
    ██    ██  █ ██▀ █ ██▀▀▀ ██  █ ██▀█▀   ██  ██▀▀  ██▀ █ ██▀▀  ██▀██   ██
    ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀    ▀▀▀▀▀ ▀▀  ▀   ▀▀  ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀  ▀   ▀▀

 *══════════════════════════════════════════════════════════════════════════*/

#include "C_EXTERN.H"

#include "GAMEMENU.H" /* Menu_IsModern */
#include "INPUT.H"
#include "UI_LAYOUT.H" /* UI_VCENTER_OFS */

#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SVGA/GPUOBJ.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/UIOVERLAY.H>
#include <math.h>
#include <string.h>

#include <stdint.h>
#include "AMBIANCE.H"
#include "BOOT_EXIT.H"

#define ULTRA -1L
#define ALWAYS ULTRA

T_MENU_COMP ListMenuComp[MAX_MENUS_COMP] = {
    {//*********** 0--> Normal,Sportif,Agressif,Discret
     ALWAYS,
     3, // Previous = CONQUE
     1, // Next     = PROTOPACK
     4,
     {C_NORMAL, C_SPORTIF, C_AGRESSIF, C_DISCRET}},

    {//*********** 1--> Protopack
     FLAG_PROTOPACK,
     0, // Previous = NORMAL,SPORTIF,AGRESSIF,DISCRET
     3, // Next     = CONQUE
     1, // 1 seul au debut du jeu
     {C_PROTOPACK, C_JETPACK, -1, -1}},

    {//*********** 2--> Double
     ALWAYS,
     -1, // Previous = Aucun
     -1, // Next	    = Aucun
     1,
     {C_DOUBLE, -1, -1, -1}},

    {//*********** 3--> Conque
     FLAG_CONQUE,
     1, // Previous = PROTOPACK
     0, // Next     = NORMAL,SPORTIF,AGRESSIF,DISCRET
     1,
     {C_CONQUE, -1, -1, -1}},

    {//*********** 4--> Scaphandre Interieur
     ALWAYS,
     -1, // Previous = Aucun
     -1, // Next     = Aucun
     2,
     {C_SCAPH_INT_NORM, C_SCAPH_INT_SPOR, -1, -1}},

    {//*********** 5--> Scaphandre Exterieur
     ALWAYS,
     -1, // Previous = Aucun
     -1, // Next     = Aucun
     2,
     {C_SCAPH_EXT_NORM, C_SCAPH_EXT_SPOR, -1, -1}},

    {//*********** 6--> Buggy
     ALWAYS,
     3, // Previous = Conque
     0, // Next     = NORMAL,SPORTIF,AGRESSIF,DISCRET
     1,
     {C_BUGGY, -1, -1, -1}},

    {//*********** 7--> Squelette
     ALWAYS,
     -1, // Previous = Aucun
     -1, // Next     = Aucun
     1,
     {C_SKELETON, -1, -1, -1}}};

#define COUL_LIFE_POINTS 74   // couleur de la barre des points de vie
#define COUL_MAGIC_POINTS 200 // couleur de la barre des points de magie

MOVE RealRot;
T_OBJ_3D InvObjTwinsen[MAX_COMPORTEMENTS];

S32 DebComportement = 110; // Espacement entre deux comportement
S32 ComportementMenu = 0;  // menu comportement en cours
S32 IndexComportement = 0;

/* Behaviour-modal frame. Original literals were 100, 100, 550, 290 — a
   450×190 box positioned (mostly) centred on the 640×480 authored canvas.
   Re-anchor X via ModeDesiredX/2 so the modal stays centred at any render
   width (768, 1024, 1280, 1920…) rather than sticking to the top-left of
   the wider framebuffer. Box width is preserved so the 4-tile layout +
   info-bar geometry inside (which all derive from CTRL_X0..CTRL_X1)
   reflows automatically. CTRL_Y0/Y1 float via UI_VCENTER_OFS so the box also
   centres vertically on a taller framebuffer; the 190px height and the tile /
   info-bar geometry (all derived from CTRL_Y0..CTRL_Y1) follow. No-op at 480. */
#define CTRL_BOX_W 450
#define CTRL_X0 (((S32)ModeDesiredX - CTRL_BOX_W) / 2)
#define CTRL_X1 (CTRL_X0 + CTRL_BOX_W)
#define CTRL_Y0 (100 + UI_VCENTER_OFS)
#define CTRL_Y1 (290 + UI_VCENTER_OFS)

/*══════════════════════════════════════════════════════════════════════════*/

//		      Gestion des menus comportements

/*══════════════════════════════════════════════════════════════════════════*/

// Recupere le menu comportement precedent disponible
S32 GetPreviousMenuComp() {
    T_MENU_COMP *ptrmcomp = &ListMenuComp[ComportementMenu];

    S32 flag = FALSE;
    S32 n = -1;

    while (!flag AND n != ComportementMenu) {
        n = ptrmcomp->Previous;

        if (n == -1)
            return (ComportementMenu);

        ptrmcomp = &ListMenuComp[n];

        if (ptrmcomp->Enable == ALWAYS OR
#ifdef LBA_EDITOR
                                    ListVarGame[ptrmcomp->Enable]
                                        .Value)
            return (n);
#else
                                    ListVarGame[ptrmcomp->Enable])
            return (n);
#endif
    }

    return (ComportementMenu);
}

// Recupere le menu comportement suivant disponible
S32 GetNextMenuComp() {
    T_MENU_COMP *ptrmcomp = &ListMenuComp[ComportementMenu];

    S32 flag = FALSE;
    S32 n = -1;

    while (!flag AND n != ComportementMenu) {
        n = ptrmcomp->Next;

        if (n == -1)
            return (ComportementMenu);

        ptrmcomp = &ListMenuComp[n];

        if (ptrmcomp->Enable == ALWAYS OR
#ifdef LBA_EDITOR
                                    ListVarGame[ptrmcomp->Enable]
                                        .Value)
            return (n);
#else
                                    ListVarGame[ptrmcomp->Enable])
            return (n);
#endif
    }

    return (ComportementMenu);
}

/*══════════════════════════════════════════════════════════════════════════*/
// --- Modern behaviour menu ---------------------------------------------------
// With the modern menu style the panel, the tiles, the behaviour's name and the
// status bar are overlay cards. The tiles stay open: the turning Twinsens are
// drawn in the frame beneath, like the status icons.

static S32 s_compModern = FALSE;

static void TileBox(S32 num, S32 *x0, S32 *y0, S32 *x1, S32 *y1) {
    *x0 = CTRL_X0 + DebComportement + num * 110 - 1;
    *y0 = CTRL_Y0 + 10 - 1;
    *x1 = *x0 + 99 + 1;
    *y1 = *y0 + 119 + 1;
}

static T_UIOVERLAY_CARD Card(float radius, U32 top, U32 bottom, U32 border, float borderWidth) {
    T_UIOVERLAY_CARD card;
    memset(&card, 0, sizeof(card));
    card.Radius = radius;
    card.FillTop = top;
    card.FillBottom = bottom;
    card.Border = border;
    card.BorderWidth = borderWidth;
    return card;
}

/* A rounded bar: its track, then the filled part in a vertical gradient. */
static void Bar(float x0, float y0, float x1, float y1, float fill, U32 top, U32 bottom) {
    T_UIOVERLAY_CARD track = Card((y1 - y0) * 0.5f, 0xB0000000u, 0x90202630u, 0x40FFFFFFu, 1.0f);
    UiOverlay_Card(x0, y0, x1, y1, &track);
    if (fill > 0.0f) {
        if (fill > 1.0f) {
            fill = 1.0f;
        }
        T_UIOVERLAY_CARD bar = Card((y1 - y0) * 0.5f - 1.0f, top, bottom, 0, 0.0f);
        bar.Glow = (top & 0x00FFFFFFu) | 0x50000000u;
        bar.GlowSize = 6.0f;
        UiOverlay_Card(x0 + 1.5f, y0 + 1.5f, x0 + 1.5f + (x1 - x0 - 3.0f) * fill, y1 - 1.5f, &bar);
    }
}

static void CompModern_Publish(void) {
    S32 n, x0, y0, x1, y1;
    const T_MENU_COMP *menu = &ListMenuComp[ComportementMenu];
    const float pulse = 0.5f + 0.5f * (float)sin((double)TimerRefHR / 1000.0 * 3.0);
    T_UIOVERLAY_CARD card;
    char text[256];

    if (!s_compModern) {
        return;
    }
    UiOverlay_ClearLayer(UIOVERLAY_LAYER_PAGE);

    /* The panel: the frame beneath is already shaded. */
    card = Card(14.0f, 0x30101828u, 0x50060A14u, 0x70A8D0FFu, 1.5f);
    card.Glow = 0x305AB4FFu;
    card.GlowSize = 12.0f;
    UiOverlay_Card((float)CTRL_X0, (float)CTRL_Y0, (float)CTRL_X1, (float)CTRL_Y1, &card);

    /* The tiles, open in the middle for the turning Twinsens. */
    for (n = 0; n < menu->NbComps; n++) {
        TileBox(n, &x0, &y0, &x1, &y1);
        card = Card(10.0f, 0x20FFFFFFu, 0x00FFFFFFu, 0x34FFFFFFu, 1.0f);
        if (menu->Comps[n] == Comportement) {
            card.Border = 0xFF86CCFFu;
            card.BorderWidth = 2.5f;
            card.Glow = ((U32)(120.0f + 80.0f * pulse) << 24) | 0x5AB4FFu;
            card.GlowSize = 14.0f;
            card.FillTop = 0x405AB4FFu;
            card.FillBottom = 0x105AB4FFu;
        }
        UiOverlay_Card((float)x0, (float)y0, (float)x1, (float)y1, &card);
    }

    /* The behaviour's name, with chevrons when Up/Down reach other sets. */
    x0 = CTRL_X0 + 10;
    y0 = CTRL_Y0 + 139;
    x1 = CTRL_X1 - 10;
    y1 = CTRL_Y0 + 139 + 40;
    card = Card(10.0f, 0xD0182236u, 0xE00C1220u, 0x70A8D0FFu, 1.5f);
    card.Shadow = 0x80000000u;
    card.ShadowY = 4.0f;
    card.ShadowBlur = 12.0f;
    UiOverlay_Card((float)x0, (float)y0, (float)x1, (float)y1, &card);
    GetMultiText(Comportement + START_TEXT_COMPORTEMENT, text);
    {
        const float cx = (float)(x0 + x1) * 0.5f;
        const float cy = (float)(y0 + y1) * 0.5f;
        const float maxW = (float)(x1 - x0) - 70.0f;
        UiOverlay_Text(cx + 1.0f, cy + 2.0f, text, 26.0f, 0x90000000u, UIOVERLAY_ALIGN_CENTER, TRUE, maxW);
        UiOverlay_Text(cx, cy, text, 26.0f, 0xFFFFF0D0u, UIOVERLAY_ALIGN_CENTER, TRUE, maxW);
        if (GetPreviousMenuComp() != ComportementMenu) {
            UiOverlay_Chevron((float)x1 - 22.0f, cy - 7.0f, 10.0f, -2, 0xD0A8D0FFu);
        }
        if (GetNextMenuComp() != ComportementMenu) {
            UiOverlay_Chevron((float)x1 - 22.0f, cy + 7.0f, 10.0f, 2, 0xD0A8D0FFu);
        }
    }

    /* The status bar: life, magic, coins and keys over the frame's icons. */
    x0 = CTRL_X0;
    y0 = CTRL_Y1 + 10;
    card = Card(12.0f, 0x28182236u, 0x400C1220u, 0x70A8D0FFu, 1.5f);
    card.Glow = 0x205AB4FFu;
    card.GlowSize = 10.0f;
    UiOverlay_Card((float)x0, (float)y0, (float)(x0 + 450), (float)(y0 + 80), &card);
    {
        S32 life = ListObjet[NUM_PERSO].LifePoint;
        if (life > MAX_LIFE_POINTS) {
            life = MAX_LIFE_POINTS;
        }
        Bar((float)(x0 + 25), (float)(y0 + 10), (float)(x0 + 325), (float)(y0 + 25),
            (float)life / (float)MAX_LIFE_POINTS, 0xFFFF7A6Au, 0xFFC0282Au);
        if (MagicLevel) {
            const float cap = (float)(x0 + 25) + 300.0f * (float)(MagicLevel * 10) / 40.0f;
            Bar((float)(x0 + 25), (float)(y0 + 35), cap, (float)(y0 + 50), (float)MagicPoint / (float)(MagicLevel * 20),
                0xFF7AC8FFu, 0xFF2A6AD8u);
        }
        UiOverlay_Text((float)(x0 + 372), (float)(y0 + 20), Itoa(Planet >= 2 ? NbZlitosPieces : NbGoldPieces), 24.0f,
                       0xFFFFE08Au, UIOVERLAY_ALIGN_LEFT, TRUE, 70.0f);
        UiOverlay_Text((float)(x0 + 372), (float)(y0 + 57), Itoa(NbLittleKeys), 24.0f, 0xFFE8F0FFu,
                       UIOVERLAY_ALIGN_LEFT, TRUE, 70.0f);
    }
    BoxStaticAdd(0, 0, 1, 1);
}

static void CompModern_Close(void) {
    if (s_compModern) {
        UiOverlay_ClearLayer(UIOVERLAY_LAYER_PAGE);
    }
    s_compModern = FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
void DrawObjComportement(S32 x0, S32 y0, S32 x1, S32 y1,
                         S32 y3d, S32 betaflag,
                         T_OBJ_3D *ptrobj3d) {
    S32 beta;

    SetIsoProjection((x1 + x0) / 2, (y1 + y0) / 2);

    SetClip(x0, y0, x1, y1);

    if (betaflag == -1) {
        beta = ptrobj3d->Beta + GetDeltaMove(&RealRot);
        beta &= 4095;
    } else {
        beta = betaflag;
    }

    ptrobj3d->X = 0;
    ptrobj3d->Y = y3d;
    ptrobj3d->Z = 0;
    ptrobj3d->Beta = beta;

    ObjectDisplay(ptrobj3d); //	Animated
}

/*──────────────────────────────────────────────────────────────────────────*/
void DrawComportement(S32 num, S32 comportement, S32 beta, S32 copyblock) {
    S32 x0, y0, x1, y1;

    x0 = CTRL_X0 + DebComportement + num * 110 - 1;
    y0 = CTRL_Y0 + 10 - 1;
    x1 = x0 + 99 + 1;
    y1 = y0 + 119 + 1;

    ObjectSetInterAnim(&InvObjTwinsen[comportement]);

    if (!copyblock && !s_compModern) /* init */
    {
        // cadre des comportements
        DrawCadre(x0, y0, x1, y1, ALL_ANGLES | BACKUP_ANGLES);

        // cadre texte
        DrawCadre(CTRL_X0 + 10, CTRL_Y0 + 139, CTRL_X1 - 10, CTRL_Y0 + 139 + 40,
                  ALL_ANGLES | BACKUP_ANGLES);
    }

    MemoClip();
    UnsetClip();

    if (s_compModern) {
        /* Cleared for the next turn of the Twinsen; the overlay draws the rest. */
        Box(x0 + 1, y0 + 1, x1 - 1, y1 - 1, 0);
        if (comportement == Comportement) {
            Box(CTRL_X0 + 11, CTRL_Y0 + 140, CTRL_X1 - 11, CTRL_Y0 + 139 + 39, 0);
        }
    } else if (comportement != Comportement) {
        DrawCadreBox(x0, y0, x1, y1, 0);
    } else {
        U32 arrows = 0;

        // cadre des comportements
        DrawCadreBox(x0, y0, x1, y1, COUL_SELECT_MENU);

        // cadre texte
        DrawCadreBox(CTRL_X0 + 10, CTRL_Y0 + 139, CTRL_X1 - 10, CTRL_Y0 + 139 + 40, 0);

        if (GetNextMenuComp() != ComportementMenu)
            arrows |= ARROW_ID;
        if (GetPreviousMenuComp() != ComportementMenu)
            arrows |= ARROW_SG;

        if (arrows) {
            DrawAngles(CTRL_X0 + 10, CTRL_Y0 + 139, CTRL_X1 - 10, CTRL_Y0 + 139 + 40,
                       arrows);
        }

        ColorFont(LBAWHITE);

        GetMultiText(comportement + START_TEXT_COMPORTEMENT, String);

        Font((CTRL_X1 + CTRL_X0 - SizeFont(String)) / 2, CTRL_Y0 + 140, String);
    }

    DrawObjComportement(x0 + 1, y0 + 1, x1 - 1, y1 - 1,
                        -600, beta,
                        &InvObjTwinsen[comportement]);

    if (copyblock) {
        BoxStaticAdd(x0, y0, x1, y1);
        BoxStaticAdd(CTRL_X0 + 10, CTRL_Y0 + 139, CTRL_X1 - 10, CTRL_Y0 + 139 + 40);
        CompModern_Publish();
        BoxUpdate();
    }

    RestoreClip();
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/

void DrawInfoMenu(S32 x0, S32 y0) {
    S32 x, x1;
    S32 l;
    S32 lifepoint;

    UnsetClip();

    if (s_compModern) {
        /* The overlay draws the card, the bars and the numbers; the frame keeps
           the icons. */
        Box(x0, y0, x0 + 450, y0 + 80, 0);
        GpuObj_UntagBox(x0, y0, x0 + 450, y0 + 80);
        ScaleFactorSprite = DEF_SCALE_FACTOR;
        PtrAffGraph(x0 + 9, y0 + 13, SPRITE_COEUR);
        PtrAffGraph(x0 + 9, y0 + 36, SPRITE_MAGIE);
        PtrAffGraph(x0 + 340, y0 + 15, SPRITE_PIECE);
        PtrAffGraph(x0 + 340, y0 + 55, SPRITE_CLE);
#ifdef LBA_EDITOR
        for (l = 0; l < NbCloverBox; l++) {
            x1 = RegleTrois(x0 + 25, x0 + 325, MAX_CLOVER_BOX, l);
            PtrAffGraph(x1, y0 + 58, l < ListVarGame[FLAG_CLOVER].Value ? SPRITE_FULL_CLOVER_BOX : SPRITE_CLOVER_BOX);
        }
#else
        for (l = 0; l < NbCloverBox; l++) {
            x1 = RegleTrois(x0 + 25, x0 + 325, MAX_CLOVER_BOX, l);
            PtrAffGraph(x1, y0 + 58, l < ListVarGame[FLAG_CLOVER] ? SPRITE_FULL_CLOVER_BOX : SPRITE_CLOVER_BOX);
        }
#endif
        BoxStaticAdd(x0, y0, x0 + 450, y0 + 80);
        BoxUpdate();
        return;
    }

    BackupAngles(x0, y0, x0 + 450, y0 + 80);
    DrawCadre(x0, y0, x0 + 450, y0 + 80, 0);
    Box(x0 + 1, y0 + 1, x0 + 450 - 1, y0 + 80 - 1, 0);
    DrawAngles(x0, y0, x0 + 450, y0 + 80, ALL_ANGLES);
    RestoreAngles(x0, y0, x0 + 450, y0 + 80);

    // Life Points	(max 255)

    ScaleFactorSprite = DEF_SCALE_FACTOR;

    PtrAffGraph(x0 + 9, y0 + 13, SPRITE_COEUR);

    lifepoint = ListObjet[NUM_PERSO].LifePoint;

    // blindage
    if (lifepoint > MAX_LIFE_POINTS)
        lifepoint = MAX_LIFE_POINTS;

    x = RegleTrois(x0 + 25, x0 + 325, MAX_LIFE_POINTS, lifepoint);

    BackupAngles(x0 + 25, y0 + 10, x0 + 325, y0 + 10 + 15);
    Box(x0 + 25, y0 + 10, x, y0 + 10 + 15, COUL_LIFE_POINTS);
    Box(x, y0 + 10, x0 + 325, y0 + 10 + 15, 0);

    DrawCadre(x0 + 25, y0 + 10, x0 + 325, y0 + 10 + 15, ALL_ANGLES);
    RestoreAngles(x0 + 25, y0 + 10, x0 + 325, y0 + 10 + 15);

    // Magic level	(max 4)	MagicPoint (max 80)

    PtrAffGraph(x0 + 9, y0 + 36, SPRITE_MAGIE);

    if (MagicLevel) {
        x1 = RegleTrois(x0 + 25, x0 + 325, 40, MagicLevel * 10);
        x = RegleTrois(x0 + 25, x0 + 325, 80, MagicPoint);

        BackupAngles(x0 + 25, y0 + 35, x1, y0 + 35 + 15);
        Box(x0 + 25, y0 + 35, x, y0 + 35 + 15, COUL_MAGIC_POINTS);
        DrawCadre(x0 + 25, y0 + 35, x1, y0 + 35 + 15, ALL_ANGLES);
        RestoreAngles(x0 + 25, y0 + 35, x1, y0 + 35 + 15);
    }

    // Nb Gold Pieces

    PtrAffGraph(x0 + 340, y0 + 15, SPRITE_PIECE);
    ColorFont(155);

    if (Planet >= 2)
        l = NbZlitosPieces;
    else
        l = NbGoldPieces;

    Font(x0 + 370, y0 + 5, Itoa(l));

    // Nb Little Keys

    PtrAffGraph(x0 + 340, y0 + 55, SPRITE_CLE);
    ColorFont(155);
    Font(x0 + 370, y0 + 40, Itoa(NbLittleKeys));

    // Nb Four-leaf Clover

#ifdef LBA_EDITOR
    for (l = 0; l < ListVarGame[FLAG_CLOVER].Value; l++)
#else
    for (l = 0; l < ListVarGame[FLAG_CLOVER]; l++)
#endif
    {
        x1 = RegleTrois(x0 + 25, x0 + 325, MAX_CLOVER_BOX, l);
        PtrAffGraph(x1, y0 + 58, SPRITE_FULL_CLOVER_BOX);
    }

    // Nb CloverBox

#ifdef LBA_EDITOR
    for (l = ListVarGame[FLAG_CLOVER].Value; l < NbCloverBox; l++)
#else
    for (l = ListVarGame[FLAG_CLOVER]; l < NbCloverBox; l++)
#endif
    {
        x1 = RegleTrois(x0 + 25, x0 + 325, MAX_CLOVER_BOX, l);
        PtrAffGraph(x1, y0 + 58, SPRITE_CLOVER_BOX);
    }

    BoxStaticAdd(x0, y0, x0 + 450, y0 + 80);
    BoxUpdate();
}

/*══════════════════════════════════════════════════════════════════════════*/

void DrawMenuComportement(S32 beta) {
    S32 n;
    T_MENU_COMP *ptrmcomp = &ListMenuComp[ComportementMenu];

    // Initial iso projection centre — overridden per-tile by DrawObjComportement
    // (line 164) for each behaviour-wheel thumbnail, so this matters only as
    // initial state. Routed through ModeDesiredX/Y to stay consistent with the
    // other Init3DView/Map2Screen iso sites and survive any future refactor that
    // removes the per-tile override. At 640x480 this is the original (311, 240).
    SetIsoProjection((S32)ModeDesiredX / 2 - 9, (S32)ModeDesiredY / 2);
    SetPosCamera(0, 0, 0);
    SetAngleCamera(0, 0, 0);
    SetLightVector(AlphaLight, BetaLight, 0);

    BackupAngles(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1);
    if (!s_compModern) {
        DrawCadre(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1, ALL_ANGLES);
    }
    /* The modern panel is dark glass: the scene barely shows through. */
    ShadeBoxBlk(CTRL_X0 + 1, CTRL_Y0 + 1, CTRL_X1 - 1, CTRL_Y1 - 1, s_compModern ? 13 : 8);
    RestoreAngles(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1);
    /* Shading leaves the darkest pixels as they were: with the GPU renderer
       they would still show the scene's colours through the panel. */
    GpuObj_UntagBox(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1);

    for (n = 0; n < ptrmcomp->NbComps; n++) {
        if (ptrmcomp->Comps[n] == Comportement)
            IndexComportement = n;

        DrawComportement(n, ptrmcomp->Comps[n], beta, FALSE);
    }

    DrawInfoMenu(CTRL_X0, CTRL_Y1 + 10);

    BoxStaticAdd(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1);
    CompModern_Publish();
    BoxUpdate();
}

/*══════════════════════════════════════════════════════════════════════════*/

void CalculeDebComportement(T_MENU_COMP *ptrmcomp) {
    // Calcule ordonnee du premier comportement
    DebComportement = ((ptrmcomp->NbComps * 100) + ((ptrmcomp->NbComps - 1) * 10)) / 2;
    DebComportement = 10 + ((CTRL_X1 - 10) - (CTRL_X0 + 10)) / 2 - DebComportement;
}

/*══════════════════════════════════════════════════════════════════════════*/
// parametre release: TRUE si on désire laisser le menu comportement actif
// si on lache rapidement la touche <CTRL>

void MenuComportement(S32 affone) {
    T_OBJET *ptrobj;
    U8 *memoptrfile3d;
    S32 oldindex;
    S32 oldmenu;
    S32 memoisland;
    S32 bodyhero;
    S32 release = TRUE;
    S32 n;
    S32 flag = FALSE;
    S32 savetimer;
    S32 oldcomp;
    T_MENU_COMP *ptrmcomp;
    S32 flagproto = FALSE;

    SaveTimer();

    UnsetClip();

    SaveCamera();

    ptrobj = &ListObjet[NUM_PERSO];

    // au cas ou l'on soit appelé pendant un eclair
    Palette(PtrPal);

    // Blindage Buggy
    if (ptrobj->Move == MOVE_BUGGY OR ptrobj->Move == MOVE_BUGGY_MANUAL) {
        // modifie MenuComportement de la conque
        ListMenuComp[3].Next = 6; // branche sur comp Buggy
        // modifie MenuComportement Normal
        ListMenuComp[0].Previous = 6; // branche sur comp Buggy
    } else {
        // modifie MenuComportement de la conque
        ListMenuComp[3].Next = 0; // branche sur comp Normal
        // modifie MenuComportement Normal
        ListMenuComp[0].Previous = 3; // branche sur comp Conque
    }

    oldcomp = Comportement;

    ComportementMenu = ListComportement[Comportement].Menu;
    ptrmcomp = &ListMenuComp[ComportementMenu];

    CheckProtoPack();

    CalculeDebComportement(ptrmcomp);

    memoptrfile3d = ptrobj->PtrFile3D;

    bodyhero = ptrobj->Obj.Body.Num;

    // stop sample protopack
    if (Comportement == C_PROTOPACK OR Comportement == C_JETPACK) {
#ifdef LBA_EDITOR
        HQ_StopOneSample(NUM_SAMPLE(COMMON_NUM_DEF, SAMPLE_PROTOPACK));
#else
        HQ_StopOneSample(SAMPLE_PROTOPACK);
#endif
        ptrobj->SampleAlways = 0;

        // pour pouvoir restaurer anim marche protopack
#ifdef LBA_EDITOR
        if (!strcasecmp(ptrobj->StringAnim, GEN_ANIM_MARCHE))
#else
        if (ptrobj->GenAnim == GEN_ANIM_MARCHE)
#endif
        {
            flagproto = TRUE;
        }
    }

    // Init bodys
    for (n = 0; n < MAX_COMPORTEMENTS; n++) {
        // Pour être propre (c'est le Barbu qui m'a dit ca)
        ObjectClear(&InvObjTwinsen[n]);

        ptrobj->PtrFile3D = ListComportement[n].PtrFile3d;

#ifdef LBA_EDITOR
        bodyhero = SearchBody(ptrobj->StringBody, NUM_PERSO);
        if (bodyhero == -1) {
            bodyhero = SearchBody(GEN_BODY_NORMAL, NUM_PERSO);
            if (bodyhero == -1) {
                MessageF(TRUE, "Erreur: Body %s Comportement %d introuvable", GEN_BODY_NORMAL, n);
            }
        }
#else
        bodyhero = SearchBody(ptrobj->GenBody, NUM_PERSO);

        if (bodyhero == -1) {
            bodyhero = SearchBody(GEN_BODY_NORMAL, NUM_PERSO);
            if (bodyhero == -1) {
                TheEnd(NAME_NOT_FOUND, Itoa(GEN_BODY_NORMAL));
            }
        }
#endif

        ObjectInitBody(&InvObjTwinsen[n], (void *)(uintptr_t)(U32)bodyhero);
        ObjectInitAnim(&InvObjTwinsen[n], (void *)(uintptr_t)(U32)ListComportement[n].AnimRien);
    }

    ptrobj->PtrFile3D = memoptrfile3d;

    InitMove(&RealRot, -1024);

    s_compModern = Menu_IsModern();

    BackupScreen(TRUE); // Screen => ScreenAux

    memoisland = Island;
    Island = 255;
    InitDial(0);

    DrawMenuComportement(ptrobj->Obj.Beta);

    oldindex = IndexComportement;
    oldmenu = ComportementMenu;

    ManageTime();
    savetimer = TimerRefHR;

    while (Input & I_JOY) {
        ManageTime();
        MyGetInput();
        DrawComportement(IndexComportement, Comportement, -1, TRUE);
    }

    while (!flag) {
        ManageTime();
        if (affone AND(TimerRefHR > affone + savetimer))
            flag = TRUE;

#ifdef DEBUG_TOOLS
        CheckSaveLogPcx(PtrPalNormal);
#endif
        if (!affone)
            MyGetInput();

        if (release) {
            if (!(Input & I_COMPORTEMENT)) {
                if (TimerRefHR - savetimer < 2 * 20) {
                    release = FALSE;
                } else
                    flag = TRUE;
            }
        } else {
            if (Input & (I_COMPORTEMENT | I_RETURN | I_MENUS)
                                OR MyKey == K_ESC) {
                flag = TRUE;
            }
        }

        if (Input & I_UP) {
            ComportementMenu = GetPreviousMenuComp();
        }

        if (Input & I_DOWN) {
            ComportementMenu = GetNextMenuComp();
        }

        if (ComportementMenu != oldmenu) {
            oldmenu = ComportementMenu;

            ptrmcomp = &ListMenuComp[ComportementMenu];

            CalculeDebComportement(ptrmcomp);

            if (IndexComportement >= ptrmcomp->NbComps)
                IndexComportement = ptrmcomp->NbComps - 1;

            oldindex = IndexComportement;
            Comportement = (U8)ptrmcomp->Comps[IndexComportement];

            CopyBlock(CTRL_X0, CTRL_Y0, CTRL_X1, CTRL_Y1, Screen,
                      CTRL_X0, CTRL_Y0, Log);

            DrawMenuComportement(-1);

            while (Input & I_JOY) {
                ManageTime();
                MyGetInput();
                DrawComportement(IndexComportement, Comportement, -1, TRUE);
            }
        }

        if (Input & I_RIGHT) {
            IndexComportement++;
            if (IndexComportement >= ptrmcomp->NbComps)
                IndexComportement = 0;
        }

        if (Input & I_LEFT) {
            IndexComportement--;
            if (IndexComportement < 0)
                IndexComportement = ptrmcomp->NbComps - 1;
        }

        if (IndexComportement != oldindex) {
            Comportement = (U8)ptrmcomp->Comps[IndexComportement];

            DrawComportement(oldindex, ptrmcomp->Comps[oldindex], ptrobj->Obj.Beta, TRUE);

            oldindex = IndexComportement;

            InitMove(&RealRot, -1024);

            while (Input & I_JOY) {
                ManageTime();
                MyGetInput();
                DrawComportement(IndexComportement, Comportement, -1, TRUE);
            }
        }
        DrawComportement(IndexComportement, Comportement, -1, TRUE);
    }

    CompModern_Close();

    InitWaitNoInput(I_COMPORTEMENT | I_RETURN | I_MENUS); // fait dans la boucle du jeu
    InitWaitNoKey();

    if (oldcomp == C_BUGGY AND Comportement != C_BUGGY) {
        ptrobj->Move = MOVE_MANUAL;
        LeaveBuggy((U8)Comportement);
    }

    SetComportement(Comportement);

    Island = memoisland;
    InitDial(START_FILE_ISLAND + Island);

    PtrInit3DView();

    RestoreCamera();
    if (CubeMode == CUBE_INTERIEUR)
        CameraCenter(0);

    RestoreScreen();

    RestoreTimer();

    if (oldcomp != Comportement) {
        // repasse en anim rien
        // peut-etre faut-il le faire même si olcomp==Comportement
        InitAnim(GEN_ANIM_RIEN, ANIM_REPEAT, NUM_PERSO);
    } else if (flagproto) {
        // restore marche proto en ANIM_ALL_THEN pour que le
        // joueur ait le temps de se reprendre
        InitAnim(GEN_ANIM_MARCHE, ANIM_REPEAT, NUM_PERSO);
        ObjectSetFrame(&(ptrobj->Obj), 5);
        TimerProto = TimerRefHR + TEMPO_PROTO_AUTO; // 1/2 s
    }
}

/*══════════════════════════════════════════════════════════════════════════*/
void IncrustComportement(U8 comportement) {
    S32 n;
    T_INCRUST_DISP *ptrdisp = ListIncrustDisp;

    // clear old SYS_TEXT
    for (n = 0; n < MAX_INCRUST_DISP; n++, ptrdisp++) {
        if (ptrdisp->Type == INCRUST_SYS_TEXT) {
            ptrdisp->Num = -1; // gicle !
        }
    }

    InitIncrustDisp(INCRUST_SYS_TEXT,
                    (S16)comportement,
                    10, ClipWindowYMax - 40,
                    LBAWHITE, 0, 1);
}
