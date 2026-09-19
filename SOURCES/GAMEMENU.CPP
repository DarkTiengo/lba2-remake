#include "C_EXTERN.H"
#include "CONFIG.H"
#include "CONTROL.H" /* Control_KeyHold (AskChoice_RequestDrive) */
#include "DIRECTORIES.H"
#include "DISTRIB.H"
#include "FOLLOWCAM.H" /* FollowCamera (Auto/Classic camera menu label) */
#include "INPUT.H"
#include "JOYSTICK.H"
#include "PERSO.H" /* WriteConfigFile (persist on resolution change) */
#include "RES_MENU.H"
#include "RES_SWITCH.H" /* Res_Switch (apply on cycle) */
#include "MENU_KEYNAV.H"
#include "SAVENAME_VALIDATION.H"
#include "CONSOLE/CONSOLE.H"
#include "MESSAGE.H"
#include "IMAGE_LOAD.H"
#include "MENU_LABELS.H"
#include "3DEXT/TERRAIN_GPU.H"
#include <SVGA/GPUSKY.H>
#include <SVGA/GPUSTORM.H>
#include <SVGA/GPUWATER.H>

#include <FILEIO/SAVEPNG.H>
#include <SVGA/SCREEN.H>
#include <SVGA/VIDEO.H>
#include <SYSTEM/ANDROID.H>
#include <SYSTEM/KEYBOARD.H>
#include <SYSTEM/KEYBOARD_KEYS.H>
#include <SYSTEM/LOG.H> /* Log_Warn (askchoice drive hook) */
#include <SYSTEM/STRING.H>
#include <SYSTEM/WINDOW.H>
#include <SVGA/GPUOBJ.H> /* GpuObjEnabled, the Renderer row */
#include <SYSTEM/MOUSE.H>
#include <SYSTEM/UIOVERLAY.H>

#include "GAMEMENU_MOUSE.H"
#include "CONFIG_FILE.H" /* ConfigFile_MarkVSyncSetThisRun */
#include "UI_LAYOUT.H"   /* UI_CENTER_X, UI_VCENTER_OFS */

#include <dirent.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include "AMBIANCE.H"
#include "BOOT_EXIT.H"
#include "GPUTUNE.H"

/*══════════════════════════════════════════════════════════════════════════*
 *══════════════════════════════════════════════════════════════════════════*/

#define HAUTEUR_STANDARD 50
#define MENU_SPACE 6

#define COUL_TEXT_MENU LBAWHITE // trouver blanc palette jeu
#define COUL_LETTER_INPUT LBAWHITE

#ifdef DEMO
#define COUL_TEXT_MENU_DISABLE (3 * 16 + 11) // trouver blanc palette jeu
#endif

#define SCREEN_SAVE_WIDTH 160
#define SCREEN_SAVE_HEIGHT 120
/* Top-left X of the SCREEN_SAVE_WIDTH-wide save thumbnail when it should be
   horizontally centred on the framebuffer. At 640 this resolves to 240 (the
   historical literal); at wider widths it tracks ModeDesiredX/2. */
#define SAVE_THUMB_X (UI_CENTER_X - SCREEN_SAVE_WIDTH / 2)
/* Top of the save thumbnail on the save/load list screen (ChoosePlayerName).
   The historical literal is 50, on the 480-tall canvas; UI_VCENTER_OFS floats
   it with the rest of that screen on a taller framebuffer (0 at 480). The
   in-game save (y=10) and load-confirm (y=130) thumbnails are other screens
   and keep their own literals. */
#define SAVE_THUMB_Y (50 + UI_VCENTER_OFS)

U8 CoulTextMenu = COUL_TEXT_MENU;
S32 LargeurMenu = 550;

#ifdef DEMO
S32 FlagShowEndDemo = FALSE;
#endif

#define MAX_MAIN_MENUS 6

// Menu IDs are placed outside the DEBUG_TOOLS return code range (2000/2001)
#define MENU_ID_SOUND_VOLUME 2100
#define MENU_ID_CHOOSE_LANGUAGE 2101
#define MENU_ID_ADVANCED_OPTIONS 2102
#define MENU_ID_TEXT_LANGUAGE 2103
#define MENU_ID_VOICE_LANGUAGE 2104
#define MENU_ID_STEREO_CHANNELS 2105
#define MENU_ID_MOVIE_CAMERA 2106
#define MENU_ID_VIDEO_SIZE 2107
#define MENU_ID_DISPLAY_FULLSCREEN 2108
#define MENU_ID_DISPLAY_TEXT 2109
#define MENU_ID_GENERAL_VOLUME 2110
#define MENU_ID_FOLLOW_CAMERA 2111
#define MENU_ID_CONTROLLER 2112
#define MENU_ID_GAMEPAD_CONFIG 2113
#define MENU_ID_DISPLAY 2114
#define MENU_ID_RESOLUTION 2115
#define MENU_ID_DISPLAY_VSYNC 2116
#define MENU_ID_DISPLAY_GPU 2117
#define MENU_ID_GPU_EFFECTS 2118
#define MENU_ID_GPU_LIGHTS 2119
#define MENU_ID_GPU_PIXEL_FILTER 2120
#define MENU_ID_GPU_DEBAND 2121
#define MENU_ID_GPU_SPECULAR 2122
#define MENU_ID_GPU_UI_SCALE 2124
#define MENU_ID_DISPLAY_MENU_STYLE 2125
#define MENU_ID_GPU_FIRE 2126
#define MENU_ID_GPU_LAMPS 2127
#define MENU_ID_GPU_WATER 2128
#define MENU_ID_GPU_SHADOWS 2129
#define MENU_ID_GPU_SUNLIGHT 2130
#define MENU_ID_GPU_RAYTRACE 2131
#define MENU_ID_GPU_LIGHTING 2132
#define MENU_ID_GPU_STORM 2133
#define MENU_ID_GPU_SKY 2134
#define MENU_ID_GPU_SMOOTH_TERRAIN 2135
#define MENU_ID_GPU_VIEW_DISTANCE 2137
#define MENU_ID_GPU_AUTOTUNE 2138
// Returned by DoGameMenu when a selector changed which rows its page has
// (the Renderer row shows or hides GPU effects): the caller rebuilds the page.
#define MENU_ID_MENU_REBUILD 2123

// Resolution sub-page rows occupy a reserved id range: row i carries id
// MENU_ID_RESOLUTION_BASE + i, which BuildCustomMenuText and Resolution_MainCapture
// map back to s_resMenuEntries[i]. DISPLAY_MENU_MAX_ENTRIES ids reserved.
#define MENU_ID_RESOLUTION_BASE 2200

#define MENU_TYPE_TEXT_LANGUAGE 8
#define MENU_TYPE_VOICE_LANGUAGE 9
// A Display option shown as "Label:  <  value  >": left/right (or the mouse
// wheel) change the value, Enter applies it. See Display_CycleOption.
#define MENU_TYPE_DISPLAY_SELECTOR 10

char NewGameTxt[MULTI_TEXT_MAX];

U16 GameMainMenu[4 + MAX_MAIN_MENUS * 2];

// Menu Principal Complet permettant de construire GameMainMenu
U16 RealGameMainMenu[] = {
    0,   // selected
    6,   // nb entries
    335, // y center
    0,   // .dia num
    0,
    70, // reprendre partie
    0,
    71, // nouvelle partie
    0,
    72, // charger partie
    0,
    73, // sauver partie
    0,
    74, // Options
    0,
    75, // Quitter
};

U16 GameOptionMenu[] = {
    0,   // selected
    6,   // nb entries
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    0,
    MENU_ID_DISPLAY,
    0,
    MENU_ID_SOUND_VOLUME,
    0,
    MENU_ID_CHOOSE_LANGUAGE,
    0,
    MENU_ID_ADVANCED_OPTIONS,
    0,
    MENU_ID_CONTROLLER,
};

// Display submenu: one page of selector rows (Resolution, Vsync, Renderer,
// GPU effects, Display mode). Left/right change a row's value; toggles apply at
// once, the resolution applies on Enter because a switch rebuilds every frame
// buffer. Plain "Back" returns to Options. Built by Display_BuildMenu: the GPU
// effects button only exists while the GPU renderer is on, and Display mode is
// dropped on always-fullscreen devices.
U16 DisplayMenu[4 + 8 * 2];

S32 MenuStyle = MENU_STYLE_AUTO;

// GPU effects submenu, opened from the Display page while the GPU renderer is
// on: the image and the effects, and the lighting page.
U16 GpuEffectsMenu[] = {
    0,   // selected
    9,   // nb entries
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_UI_SCALE,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_PIXEL_FILTER,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_DEBAND,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_WATER,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_FIRE,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_VIEW_DISTANCE,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_SMOOTH_TERRAIN,
    0,
    MENU_ID_GPU_LIGHTING,
};

// Lighting and shadows, opened from the GPU effects page.
U16 GpuLightingMenu[] = {
    0,   // selected
    9,   // nb entries
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_SUNLIGHT,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_RAYTRACE,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_SHADOWS,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_LIGHTS,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_LAMPS,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_SPECULAR,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_SKY,
    MENU_TYPE_DISPLAY_SELECTOR,
    MENU_ID_GPU_STORM,
};

// Set when a selector changed the rows of its page; DoGameMenu then returns
// MENU_ID_MENU_REBUILD.
static S32 s_menuRebuild = FALSE;

// Resolution sub-page, opened from the Display submenu's Resolution row.
// Built at runtime from the shared catalog (like GameAskChoice builds
// GameChoiceMenu): a "Back" row plus one plain-button row per resolution.
// s_resMenuEntries / s_resMenuCount back the range-id rows; the U16 buffer
// holds 4 header words + (Back + up to DISPLAY_MENU_MAX_ENTRIES) (type, id)
// pairs. See Resolution_MainCapture + BuildCustomMenuText.
static DisplayEntry s_resMenuEntries[DISPLAY_MENU_MAX_ENTRIES];
static S32 s_resMenuCount;
static U16 ResolutionMenu[4 + (1 + DISPLAY_MENU_MAX_ENTRIES) * 2];

// Menu row index DrawGameMenu paints with the green "active value" plasma
// marker (distinct from the blue cursor). -1 = no marker.
//
// A file-static rather than a per-row flag in the menu array: the U16 (type,id)
// menu format has no spare per-row slot, and growing it for a single marker
// isn't worth touching every menu. The contract instead: only the resolution
// sub-page sets this (BuildResolutionMenu), and it is cleared back to -1 on
// every exit path (Resolution_MainCapture), paired with
// s_resMenuCount = 0, so no other menu can render a marker. See DrawOneChoice's
// select bitmask.
static S32 s_menuMarkRow = -1;

// Controller submenu: split keyboard remap and gamepad remap under one parent
// so adding the pad doesn't clutter the top-level Options screen.
U16 ControllerMenu[] = {
    0,   // selected
    3,   // nb entries
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    0,
    14, // Configuration Clavier (existing keyboard remap screen)
    0,
    MENU_ID_GAMEPAD_CONFIG,
};

U16 LanguageMenu[] = {
    0, // selected
#ifdef CDROM
    3, // nb entries
#else
    2, // nb entries
#endif
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    MENU_TYPE_TEXT_LANGUAGE,
    MENU_ID_TEXT_LANGUAGE,
#ifdef CDROM
    MENU_TYPE_VOICE_LANGUAGE,
    MENU_ID_VOICE_LANGUAGE,
#endif
};

U16 AdvancedOptionMenu[] = {
    0,   // selected
    7,   // nb entries
    260, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    0,
    MENU_ID_STEREO_CHANNELS,
    7,
    45, // Niveau des détails
    0,
    MENU_ID_MOVIE_CAMERA,
    0,
    MENU_ID_FOLLOW_CAMERA,
    0,
    MENU_ID_VIDEO_SIZE,
    0,
    MENU_ID_DISPLAY_TEXT,
};

U16 SavedConfirmMenu[] = {
    0,   // selected
    2,   // nb entries
    350, // y center.
    0,   // .dia num

    0, 24, // annuler
    0, 48  // détruire une sauvegarde

};

U16 GameChoiceMenu[] = {
    0, // selected
    7, // nb entries
    0, // y from top. byte 8(
    0, // .dia num

    0, 0,
    0, 1,
    0, 2,
    0, 3,
    0, 4,
    0, 5,
    0, 6};

#ifdef DEMO
U16 VolumeMenu[] = {
    0,   // selected
    4,   // nb entries
    240, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    2,
    19, // reglage volume samples
    4,
    21, // reglage volume musiques (Jingles)
    6,
    MENU_ID_GENERAL_VOLUME,
};
#else
U16 *VolumeMenu;

U16 VolumeMenuVoice[] = {
    // integere volume des voix
    0,   // selected
    5,   // nb entries
    240, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    2,
    19, // reglage volume samples
    3,
    20, // reglage volume voix
    4,
    21, // reglage volume musiques (Jingles)
    6,
    MENU_ID_GENERAL_VOLUME,
};

U16 VolumeMenuNoVoice[] = {
    // integere volume des voix
    0,   // selected
    4,   // nb entries
    240, // y center
    0,   // .dia num

    0,
    26, // menu precedent
    2,
    19, // reglage volume samples
    4,
    21, // reglage volume musiques (Jingles)
    6,
    MENU_ID_GENERAL_VOLUME,
};
#endif

U32 TimerSample;
S32 CursorActif = FALSE;
S32 CursorX, CursorY;

extern S32 EscTimer;

// save players games

#define NB_CAR_H 14
#define NB_CAR_V 5

char GamePathname[ADELINE_MAX_PATH];
char OldGamePathname[ADELINE_MAX_PATH];
char PlayerName[MAX_SIZE_PLAYER_NAME + 1];

const char *MenuIncrustText = NULL;
S32 MenuIncrustTimer = 0;
S32 MenuIncrustSprite = -1;

/*──────────────────────────────────────────────────────────────────────────*/

S32 SelectPlayer = 0;

/* enum MenuLocalizedLabel and LocalizedMenuLabels live in MENU_LABELS.{H,CPP}
   so the host completeness test (tests/menu_labels) links the real table.

   MENU_LABELS.H is deliberately free of engine headers (the host test links it
   with nothing else), so it declares its own width. Bind the two together here,
   where both headers are in scope: add a language to MESSAGE.H without adding a
   translation column and this fails to compile, rather than silently rendering
   the new language's menus in English. */
typedef char MenuLabelTableCoversEveryLanguage[(MENU_LANGUAGE_COUNT == NB_LANGUAGES) ? 1 : -1];

/*──────────────────────────────────────────────────────────────────────────*/

#define SIZE_FOUND_OBJ 130

static S32 GetMenuLanguageIndex(void) {
    S32 language = Language;

    if (language < 0 OR language >= GetLanguageCount()) {
        language = 1;
    }

    return language;
}

const char *GetLocalizedMenuLabel(enum MenuLocalizedLabel label) {
    /* In range by construction: GetMenuLanguageIndex() bounds against
       GetLanguageCount() == NB_LANGUAGES, which the typedef above pins equal to
       the table's own width. */
    return LocalizedMenuLabels[label].lang[GetMenuLanguageIndex()];
}

const char *GetLocalizedResDialogLabel(int which) {
    static const enum MenuLocalizedLabel kMap[RES_DIALOG_LABEL_COUNT] = {
        MENU_LABEL_RES_TITLE,
        MENU_LABEL_RES_NOW_AT,
        MENU_LABEL_RES_KEEP_Q,
        MENU_LABEL_RES_BTN_KEEP,
        MENU_LABEL_RES_BTN_REVERT_FMT,
    };
    if (which < 0 || which >= (int)RES_DIALOG_LABEL_COUNT)
        return "";
    return GetLocalizedMenuLabel(kMap[which]);
}

// Resolution selector state for the Display page: the menu's resolution list
// and the entry the player has cycled to but not applied yet (-1: none, the row
// shows the current resolution).
static DisplayEntry s_displayResEntries[DISPLAY_MENU_MAX_ENTRIES];
static S32 s_displayResCount = 0;
static S32 s_displayResPending = -1;

static void Display_RebuildResolutionList(void) {
    s_displayResCount = Display_BuildEntries(s_displayResEntries);
    s_displayResPending = -1;
}

// "Label: value", the selector row text; DrawOneChoice adds the arrows (the
// game font has no arrow or angle-bracket glyphs).
static void FormatSelector(char *string, const char *label, const char *value) {
    FormatUtf8ToCp850(string, 256, "%s: %s", label, value);
}

// One step of a Display selector. Toggles take effect immediately; the
// resolution only moves the pending choice (Enter applies it).
static void Display_CycleOption(S32 id, S32 dir) {
    switch (id) {
    case MENU_ID_RESOLUTION: {
        if (s_displayResCount <= 0) {
            Display_RebuildResolutionList();
        }
        if (s_displayResCount <= 0) {
            break;
        }
        S32 idx = s_displayResPending;
        if (idx < 0) {
            idx = Display_FindCurrentEntry(s_displayResEntries, s_displayResCount);
        }
        if (idx < 0) {
            idx = dir > 0 ? -1 : 0;
        }
        idx += dir;
        if (idx < 0)
            idx = s_displayResCount - 1;
        if (idx >= s_displayResCount)
            idx = 0;
        s_displayResPending = idx;
        break;
    }

    case MENU_ID_DISPLAY_VSYNC:
        DisplayVSync ^= 1;
        ConfigFile_MarkVSyncSetThisRun();
        Window_SetVSync(DisplayVSync);
        break;

    case MENU_ID_DISPLAY_GPU:
        /* The window swaps renderer on its next present. Scene bodies pick up
           the new look once they are drawn again, after the menu. */
        GpuObjEnabled ^= 1;
        s_menuRebuild = TRUE; /* GPU effects row appears or goes */
        break;

    case MENU_ID_GPU_LIGHTS:
        GpuObjLightsEnabled ^= 1;
        break;

    case MENU_ID_GPU_PIXEL_FILTER:
        GpuObjPixelFilter ^= 1;
        break;

    case MENU_ID_GPU_DEBAND:
        GpuObjDeband ^= 1;
        break;

    case MENU_ID_GPU_SPECULAR:
        GpuObjSpecular ^= 1;
        break;

    case MENU_ID_GPU_FIRE:
        GpuObjFire ^= 1;
        break;

    case MENU_ID_GPU_WATER:
        GpuWaterEnabled ^= 1;
        break;

    case MENU_ID_GPU_LAMPS:
        GpuObjGlow ^= 1;
        break;

    case MENU_ID_GPU_SHADOWS:
        GpuObjShadows ^= 1;
        break;

    case MENU_ID_GPU_STORM:
        GpuStormEnabled ^= 1;
        break;

    case MENU_ID_GPU_SKY:
        GpuSkyEnabled ^= 1;
        break;

    case MENU_ID_GPU_SMOOTH_TERRAIN:
        TerrainGpuSmooth ^= 1;
        break;

    case MENU_ID_GPU_VIEW_DISTANCE:
        /* Takes effect as cubes are loaded, every frame outdoors. */
        TerrainGpuViewDistance = (TerrainGpuViewDistance + (dir < 0 ? 2 : 1)) % 3;
        break;

    case MENU_ID_GPU_SUNLIGHT:
        GpuObjSunlight ^= 1;
        break;

    case MENU_ID_GPU_RAYTRACE:
        GpuObjRayTrace ^= 1;
        break;

    case MENU_ID_GPU_UI_SCALE:
        /* Takes effect when the page is left (Display_ApplyRenderSize): the
           switch rebuilds every frame buffer. */
        GpuObjUiScale ^= 1;
        break;

    case MENU_ID_DISPLAY_MENU_STYLE:
        /* Auto, Modern, Classic. */
        MenuStyle = MenuStyle == MENU_STYLE_AUTO     ? (dir > 0 ? MENU_STYLE_MODERN : MENU_STYLE_CLASSIC)
                    : MenuStyle == MENU_STYLE_MODERN ? (dir > 0 ? MENU_STYLE_CLASSIC : MENU_STYLE_AUTO)
                                                     : (dir > 0 ? MENU_STYLE_AUTO : MENU_STYLE_MODERN);
        /* The page changes look: redraw it from its backdrop. */
        s_menuRebuild = TRUE;
        break;

    case MENU_ID_DISPLAY_FULLSCREEN:
        DisplayFullScreen ^= 1;
        SetWindowFullscreen(DisplayFullScreen != FALSE);
        break;
    }
}

static void BuildCustomMenuText(S32 num, char *string) {
    // Resolution sub-page rows: id in the reserved range maps to a catalog
    // entry's label (e.g. "1280x720 (16:9)"). s_resMenuCount is set by
    // BuildResolutionMenu before the page is drawn.
    if (num >= MENU_ID_RESOLUTION_BASE &&
        num < MENU_ID_RESOLUTION_BASE + s_resMenuCount) {
        CopyUtf8ToCp850(string, 256, s_resMenuEntries[num - MENU_ID_RESOLUTION_BASE].label);
        return;
    }

    switch (num) {
    case MENU_ID_SOUND_VOLUME:
        GetMultiText(11, string);
        break;

    case MENU_ID_CHOOSE_LANGUAGE:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_CHOOSE_LANGUAGE));
        break;

    case MENU_ID_ADVANCED_OPTIONS:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_ADVANCED_OPTIONS));
        break;

    case MENU_ID_CONTROLLER:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_CONTROLLER));
        break;

    case MENU_ID_DISPLAY:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_DISPLAY));
        break;

    case MENU_ID_RESOLUTION: {
        char value[96];
        char selector[128];
        const S32 current = Display_FindCurrentEntry(s_displayResEntries, s_displayResCount);
        // Bare "WxH": the row also carries the label and the apply hint.
        if (s_displayResPending >= 0 && s_displayResPending < s_displayResCount &&
            s_displayResPending != current) {
            snprintf(value, sizeof value, "%ux%u", (unsigned)s_displayResEntries[s_displayResPending].w,
                     (unsigned)s_displayResEntries[s_displayResPending].h);
        } else {
            U32 baseW = 0, baseH = 0;
            Res_BaseDimensions(&baseW, &baseH);
            snprintf(value, sizeof value, "%ux%u", (unsigned)baseW, (unsigned)baseH);
        }
        snprintf(selector, sizeof selector, "%s", value);
        char utf8[256];
        snprintf(utf8, sizeof utf8, GetLocalizedMenuLabel(MENU_LABEL_RESOLUTION), selector);
        if (s_displayResPending >= 0 && s_displayResPending < s_displayResCount &&
            s_displayResPending != current) {
            FormatUtf8ToCp850(string, 256, "%s  (%s)", utf8, GetLocalizedMenuLabel(MENU_LABEL_RES_APPLY_HINT));
        } else {
            CopyUtf8ToCp850(string, 256, utf8);
        }
        break;
    }

    case MENU_ID_GAMEPAD_CONFIG:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_GAME_CONTROLLER));
        break;

    case MENU_ID_TEXT_LANGUAGE:
        FormatUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_TEXT_LANGUAGE), GetLanguageName(Language));
        break;

    case MENU_ID_VOICE_LANGUAGE:
#ifdef CDROM
        FormatUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_VOICE_LANGUAGE), GetLanguageName(LanguageCD));
#else
        string[0] = 0;
#endif
        break;

    case MENU_ID_STEREO_CHANNELS:
        GetMultiText(12 + ReverseStereo, string);
        break;

    case MENU_ID_MOVIE_CAMERA:
        GetMultiText(46 + AllCameras, string);
        break;

    case MENU_ID_VIDEO_SIZE:
        CopyUtf8ToCp850(string, 256,
                        GetLocalizedMenuLabel(VideoFullScreen ? MENU_LABEL_MOVIE_FIT
                                                              : MENU_LABEL_MOVIE_ORIGINAL));
        break;

    case MENU_ID_DISPLAY_FULLSCREEN:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_DISPLAY_MODE),
                       GetLocalizedMenuLabel(DisplayFullScreen ? MENU_LABEL_VAL_FULLSCREEN : MENU_LABEL_VAL_WINDOWED));
        break;

    case MENU_ID_DISPLAY_VSYNC:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_VSYNC),
                       GetLocalizedMenuLabel(DisplayVSync ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_DISPLAY_GPU:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_RENDERER),
                       GetLocalizedMenuLabel(GpuObjEnabled ? MENU_LABEL_VAL_GPU : MENU_LABEL_VAL_CLASSIC));
        break;

    case MENU_ID_GPU_EFFECTS:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_GPU_EFFECTS));
        break;

    case MENU_ID_GPU_LIGHTING:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(MENU_LABEL_GPU_LIGHTING));
        break;

    case MENU_ID_GPU_AUTOTUNE:
        CopyUtf8ToCp850(string, 256,
                        GetLocalizedMenuLabel(GpuTune_Running() ? MENU_LABEL_GPU_AUTOTUNE_RUNNING
                                                                : MENU_LABEL_GPU_AUTOTUNE));
        break;

    case MENU_ID_DISPLAY_MENU_STYLE:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_MENU_STYLE),
                       GetLocalizedMenuLabel(MenuStyle == MENU_STYLE_MODERN    ? MENU_LABEL_VAL_MODERN
                                             : MenuStyle == MENU_STYLE_CLASSIC ? MENU_LABEL_VAL_CLASSIC
                                                                               : MENU_LABEL_VAL_AUTO));
        break;

    case MENU_ID_GPU_LIGHTS:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_LIGHTS),
                       GetLocalizedMenuLabel(GpuObjLightsEnabled ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_PIXEL_FILTER:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_PIXEL_FILTER),
                       GetLocalizedMenuLabel(GpuObjPixelFilter ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_DEBAND:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_DEBAND),
                       GetLocalizedMenuLabel(GpuObjDeband ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_SPECULAR:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_SPECULAR),
                       GetLocalizedMenuLabel(GpuObjSpecular ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_LAMPS:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_LAMPS),
                       GetLocalizedMenuLabel(GpuObjGlow ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_RAYTRACE:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_RAYTRACE),
                       GetLocalizedMenuLabel(GpuObjRayTrace ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_SUNLIGHT:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_SUNLIGHT),
                       GetLocalizedMenuLabel(GpuObjSunlight ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_VIEW_DISTANCE:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_VIEW_DISTANCE),
                       GetLocalizedMenuLabel(TerrainGpuViewDistance == 0   ? MENU_LABEL_VAL_CLASSIC
                                             : TerrainGpuViewDistance == 1 ? MENU_LABEL_VAL_FAR
                                                                           : MENU_LABEL_VAL_VERY_FAR));
        break;

    case MENU_ID_GPU_SMOOTH_TERRAIN:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_SMOOTH_TERRAIN),
                       GetLocalizedMenuLabel(TerrainGpuSmooth ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_SKY:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_SKY),
                       GetLocalizedMenuLabel(GpuSkyEnabled ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_STORM:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_STORM),
                       GetLocalizedMenuLabel(GpuStormEnabled ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_SHADOWS:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_SHADOWS),
                       GetLocalizedMenuLabel(GpuObjShadows ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_FIRE:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_FIRE),
                       GetLocalizedMenuLabel(GpuObjFire ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_WATER:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_WATER),
                       GetLocalizedMenuLabel(GpuWaterEnabled ? MENU_LABEL_VAL_ON : MENU_LABEL_VAL_OFF));
        break;

    case MENU_ID_GPU_UI_SCALE:
        FormatSelector(string, GetLocalizedMenuLabel(MENU_LABEL_OPT_UI_SCALE),
                       GetLocalizedMenuLabel(GpuObjUiScale ? MENU_LABEL_VAL_UI_CLASSIC : MENU_LABEL_VAL_UI_NATIVE));
        break;

    case MENU_ID_FOLLOW_CAMERA:
        CopyUtf8ToCp850(string, 256, GetLocalizedMenuLabel(FollowCamera ? MENU_LABEL_AUTO_CAMERA : MENU_LABEL_CLASSIC_CAMERA));
        break;

    case MENU_ID_DISPLAY_TEXT:
        GetMultiText(16 + FlagDisplayText, string);
        break;

    case MENU_ID_GENERAL_VOLUME:
        GetMultiText(23, string);
        break;

    default:
        GetMultiText(num, string);
        break;
    }
}

static void CycleTextLanguage(S32 step) {
    const S32 count = GetLanguageCount();

    if (count <= 0) {
        return;
    }

    Language += step;
    if (Language < 0)
        Language = count - 1;
    if (Language >= count)
        Language = 0;

    ReloadMultiTextFile(0);
}

// Apply a resolution chosen from the sub-page. Switches live via Res_Switch,
// persists to lba2.cfg, and repaints the menu backdrop. No keep/revert dialog:
// the catalog only offers modes that fit the current display, "resolution" here
// is the internal render-framebuffer size (always scaled to the existing
// window, never an OS display-mode change that could black out the screen), and
// the open list is itself the escape hatch: the player can immediately pick
// another mode or 640x480. The revert dialog also needs a live scene to repaint
// over, which the boot main menu doesn't have. See GereDisplayMenu.
static void RepaintMenuAfterResize(void);

static void ApplyResolutionChoice(U32 w, U32 h) {
    /* Compare against the chosen base, not the live framebuffer: inside an
       interior the two differ, and picking the size the interior happens to be
       rendering at is still a change of base. */
    U32 baseW = 0, baseH = 0;
    Res_BaseDimensions(&baseW, &baseH);
    if (w == baseW && h == baseH)
        return;

    if (Res_Switch(w, h)) {
        // Persist immediately so the next launch boots at the chosen
        // resolution. Mirrors the Phase 0 keep/revert "Keep" path.
        WriteConfigFile();
        RepaintMenuAfterResize();
    }
}

// After the frame buffers were rebuilt under an open menu: repaint its backdrop.
static void RepaintMenuAfterResize(void) {
    // Res_Switch tears down Log/Screen and the cube reload inside
    // it may have clobbered SkySeaTexture (where the plasma fire
    // bar lives), so the next DoGameMenu redraw would show the
    // row on bare black with a stale plasma highlight.
    //
    // Two repaint shapes depending on context:
    //   FlagShadeMenu TRUE  → in-game (player opened Options via
    //     ESC). Repaint the live 3D scene the same way the
    //     keep/revert dialog does (RES_SWITCH.CPP:472-482), then
    //     shade so the menu items pop over the dimmed scene.
    //   FlagShadeMenu FALSE → boot main menu (player is at the
    //     Resume / New / Load screen). The right background is
    //     the PCR_MENU stretched PCX image, not the default cube
    //     AffScene would paint (which would be Twinsen's house —
    //     wrong context for the boot menu). Reload the BG using
    //     the same Image_LoadStretched MainGameMenu uses, no
    //     shade — the boot menu doesn't darken its own backdrop.
    BoxReset();
    BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
    if (FlagShadeMenu) {
        AffScene(AFF_ALL_FLIP);
    } else {
        char tmpFilePathScreen[ADELINE_MAX_PATH];
        GetResPath(tmpFilePathScreen, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
        Image_LoadStretched(tmpFilePathScreen, Screen, PCR_MENU);
        CopyScreen(Screen, Log);
    }
    BackupScreen(TRUE);
    InitPlasmaMenu();
    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1,
                    (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }
    BoxStaticFullflip();
}

#ifdef CDROM
static void ClampVoiceLanguage(void) {
    const S32 count = GetVoiceLanguageCount();

    if (LanguageCD < 0 OR LanguageCD >= count) {
        LanguageCD = 0; // English, i.e. TabLanguage[0]
    }
}

static void CycleVoiceLanguage(S32 step) {
    const S32 count = GetVoiceLanguageCount();

    if (count <= 0) {
        return;
    }

    ClampVoiceLanguage();

    LanguageCD += step;
    if (LanguageCD < 0)
        LanguageCD = count - 1;
    if (LanguageCD >= count)
        LanguageCD = 0;

    FlagSpeak = SamplesEnable;
}
#endif

/*══════════════════════════════════════════════════════════════════════════*
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void Init3DView(void) {
    // Iso projection centre = framebuffer centre - 9px horizontal shim. The -9 is
    // an iso-grid offset that pairs with Map2Screen's -32 origin shim (see
    // GRILLE.CPP::Map2Screen) — the two iso sites must move together to keep
    // interior scene composition coherent. Routing through ModeDesiredX/Y so
    // iso scenes stay centred at any render-space size; at 640x480 this is the
    // original (311, 240). See docs/WIDESCREEN.md Phase 2.
    SetIsoProjection((S32)ModeDesiredX / 2 - 9, (S32)ModeDesiredY / 2);
    SetPosCamera(0, 0, 0);
    SetAngleCamera(0, 0, 0);
    SetLightVector(AlphaLight, BetaLight, 0);
}

/*──────────────────────────────────────────────────────────────────────────*/
void Init3DGame() {
    Init3DView();

    // Initialise les pointeurs de fonctions
    PtrReajustPos = ReajustPos;
    PtrGetShadow = GetShadow;
    PtrDoAnim = DoAnim;
    PtrInitGrille = InitGrille;
    PtrWorldColBrick = WorldColBrick;
    PtrFullWorldColBrick = FullWorldColBrick;
    PtrWorldColBrickVisible = WorldColBrickVisible;
    PtrProjectShadow = ProjectShadow;
    PtrReajustPosExtra = ReajustPosExtra;
    PtrInit3DView = Init3DView;
}

/*──────────────────────────────────────────────────────────────────────────*/
void SetDetailLevel(void) {
    S32 oldmaxpolysea = MaxPolySea;
    U8 olddrawhorizon = FlagDrawHorizon;

    switch (DetailLevel) {
    case 0:                      // machine bof !!!!!!!!
        MaxPolySea = 0;          // Pas d'eau animée
        Shadow = 1;              // Pas d'ombres sur extras
        RainEnable = FALSE;      // Pas d'affichage de pluie
        FlagDrawHorizon = FALSE; // Pas D'affichage des cubes autour
        break;

    case 1:                     // 486 ?
        MaxPolySea = 0;         // Pas d'eau animée
        Shadow = 2;             // pas d'ombres sur impacts
        RainEnable = FALSE;     // Pas d'affichage de pluie
        FlagDrawHorizon = TRUE; // Affichage des cubes autour
        break;

    case 2:                     // Pentium de base ?
        MaxPolySea = 0;         // Pas d'eau animée
        Shadow = 3;             // full shadows
        RainEnable = TRUE;      // Affichage de la pluie
        FlagDrawHorizon = TRUE; // Affichage des cubes autour
        break;

    default: // machine de folie !!!!!
        if (PtrPolySea)
            MaxPolySea = MAX_POLY_SEA;
        Shadow = 3;             // full shadows
        RainEnable = TRUE;      // Affichage de la pluie
        FlagDrawHorizon = TRUE; // Affichage des cubes autour
        break;
    }

    if (FlagDrawHorizon != olddrawhorizon OR MaxPolySea != oldmaxpolysea) {
        FirstTime = AFF_ALL_FLIP;
    }
}

//──────────────────────────────────────────────────────────────────────────
void InitMenuIncrustText(const char *text, S32 sprite, S32 time) {
    ManageTime();
    MenuIncrustText = text;
    MenuIncrustTimer = TimerRefHR + time * 1000; // end message timer in s
    MenuIncrustSprite = sprite;
}

// Plasma colour banks: SelectPlasmaBank(n) ramps the fire bar through palette
// indices n*16+0..10. 12 is the blue/cyan cursor highlight, 4 the red volume
// slider, 8 a green band used to mark the active value distinct from the cursor
// (see Resolution_MainCapture). Palette rows verified against the menu palette.
#define PLASMA_BANK_CURSOR 12
#define PLASMA_BANK_SLIDER 4
#define PLASMA_BANK_GREEN 8

//──────────────────────────────────────────────────────────────────────────
void DrawFireBar(S32 xmin, S32 ymin, S32 xmax, S32 ymax, S32 coul) {
    Struc_Point TabPoint[4];

    TabPoint[0].Pt_XE = (S16)xmin;
    TabPoint[0].Pt_YE = (S16)ymin;
    TabPoint[0].Pt_MapU = (U16)0;
    TabPoint[0].Pt_MapV = (U16)0;

    TabPoint[1].Pt_XE = (S16)xmin;
    TabPoint[1].Pt_YE = (S16)ymax;
    TabPoint[1].Pt_MapU = (U16)0; //(16<<8);
    TabPoint[1].Pt_MapV = (U16)((63 << 8) + 250);

    TabPoint[2].Pt_XE = (S16)xmax;
    TabPoint[2].Pt_YE = (S16)ymax;
    TabPoint[2].Pt_MapU = (U16)(2 * ((63 << 8) + 250)); //(16<<8);
    TabPoint[2].Pt_MapV = (U16)((63 << 8) + 250);

    TabPoint[3].Pt_XE = (S16)xmax;
    TabPoint[3].Pt_YE = (S16)ymin;
    TabPoint[3].Pt_MapU = (U16)(2 * ((63 << 8) + 250));
    TabPoint[3].Pt_MapV = (U16)0;

    RepMask = 0x3f3f; // repmask x=32 y=32

    switch (coul) {
    case FIRE_SELECT_MENU:
    case FIRE_INPUT_NAME:
        SelectPlasmaBank(PLASMA_BANK_CURSOR);
        break;

    case FIRE_SLIDER_MENU:
        SelectPlasmaBank(PLASMA_BANK_SLIDER);
        break;

    case FIRE_CURRENT_MENU:
        SelectPlasmaBank(PLASMA_BANK_GREEN);
        break;
    }

    DoTextureAnimation();

    FixePlasmaPtrMap();

    Fill_Poly(POLY_TEXTURE, 0, 4, TabPoint);
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*
             ██▀▀▀ █▀▀▀█ █   █ █▀▀▀▀       █     █▀▀▀█ █▀▀▀█ █▀▀▀▄
             ▀▀▀▀█ ██▀▀█ ██ ▄▀ ██▀▀        ██    ██  █ ██▀▀█ ██  █
             ▀▀▀▀▀ ▀▀  ▀ ▀▀▀   ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

int my_sort_function(const void *a, const void *b) {
    return (strcasecmp(*(char **)a, *(char **)b));
}

/*──────────────────────────────────────────────────────────────────────────*/

S32 PlayerGameList(char **ptrlistname, char *listname, S32 flagnew) {
    struct dirent *file;
    DIR *dir;
    S32 wlong;
    char pathname[ADELINE_MAX_PATH];
    S32 nb = 0;
    S32 handle;
    char **startptrlist = ptrlistname;
    U8 wbyte;

    GetSavePath(pathname, ADELINE_MAX_PATH, NULL);
    dir = opendir(pathname);
    if (dir != NULL) {
        while ((file = readdir(dir)) != NULL) {
            if (ade_strcasestr(file->d_name, ".LBA") != NULL) {
                GetSavePath(pathname, ADELINE_MAX_PATH, file->d_name);

                struct stat st;
                stat(pathname, &st);
                if (S_ISREG(st.st_mode)) {
                    // On ne liste pas l'autosave ni la currentsave
                    // D'ailleurs ces sauvegardes pourraient avoir une autre
                    // extension que .LBA ce qui éviterait ce test !
                    if ((ade_strcasestr(pathname, AUTOSAVE_FILENAME) == NULL)
                            AND ade_strcasestr(pathname, CURRENTSAVE_FILENAME) == NULL) {
                        // BUG WORKAROUND!
                        // Take the path again because, for some reason, on Wine the call to
                        //   stat() changes the disk letter on the path
                        //   passed (even parameter being a constant). Possibly a Bug on
                        //   WINE or/and MinGW libraries used.
                        GetSavePath(pathname, ADELINE_MAX_PATH, file->d_name);
                        handle = OpenRead(pathname);
                        if (handle) {
                            Read(handle, &wbyte, 1);             // num version
                            Read(handle, &wlong, sizeof(wlong)); // num cube

                            *ptrlistname++ = listname;
                            do {
                                Read(handle, &wbyte, 1);
                                *listname++ = wbyte;
                            } while (wbyte != 0);

                            Close(handle);

                            nb++;
                            if (nb == MAX_PLAYER)
                                return nb;
                        }
                    }
                }
            }
        }
        closedir(dir);
    }

    qsort((void *)startptrlist, nb, sizeof(char *), my_sort_function);

    GetSavePath(pathname, ADELINE_MAX_PATH, AUTOSAVE_FILENAME);
    if (!flagnew AND nb < MAX_PLAYER AND ExistsFileOrDir(pathname)) {
        strcpy(GamePathname, pathname);
        LoadGamePlayerName();
        *ptrlistname = listname;
        strcpy(listname, PlayerName);
        nb++;
    }

    return nb;
}

/*──────────────────────────────────────────────────────────────────────────*/
#ifdef DEBUG_TOOLS
S32 BugGameList(char **ptrlistname, char *listname) {
    struct dirent *file;
    DIR *dir;
    S32 wlong;
    char pathname[ADELINE_MAX_PATH];
    S32 nb = 0;
    S32 handle;
    char **startptrlist = ptrlistname;
    U8 wbyte;

    GetBugPath(pathname, ADELINE_MAX_PATH, NULL);
    dir = opendir(pathname);
    if (dir != NULL) {
        while ((file = readdir(dir)) != NULL) {
            if (ade_strcasestr(file->d_name, ".LBA") != NULL) {
                GetBugPath(pathname, ADELINE_MAX_PATH, file->d_name);

                struct stat st;
                stat(pathname, &st);
                if (S_ISREG(st.st_mode)) {
                    GetBugPath(pathname, ADELINE_MAX_PATH, file->d_name);
                    handle = OpenRead(pathname);
                    if (handle) {
                        Read(handle, &wbyte, 1);             // num version
                        Read(handle, &wlong, sizeof(wlong)); // num cube

                        *ptrlistname++ = listname;
                        do {
                            Read(handle, &wbyte, 1);
                            *listname++ = wbyte;
                        } while (wbyte != 0);

                        Close(handle);

                        nb++;
                        if (nb == MAX_PLAYER)
                            return nb;
                    }
                }
            }
        }
        closedir(dir);
    }

    qsort((void *)startptrlist, nb, sizeof(char *), my_sort_function);

    return nb;
}

S32 FindBugFile(const char *bugname) {
    struct dirent *file;
    DIR *dir;
    char pathname[ADELINE_MAX_PATH];
    char playername[MAX_SIZE_PLAYER_NAME + 1];
    U8 *ptr;
    S32 handle;
    S32 wlong;
    U8 wbyte;

    GetBugPath(pathname, ADELINE_MAX_PATH, NULL);
    dir = opendir(pathname);
    if (dir != NULL) {
        while ((file = readdir(dir)) != NULL) {
            if (ade_strcasestr(file->d_name, ".LBA") != NULL) {
                GetBugPath(pathname, ADELINE_MAX_PATH, file->d_name);

                struct stat st;
                stat(pathname, &st);
                if (S_ISREG(st.st_mode)) {
                    handle = OpenRead(pathname);
                    Read(handle, &wbyte, 1);             // num version
                    Read(handle, &wlong, sizeof(wlong)); // num cube

                    ptr = (U8 *)playername;
                    do {
                        Read(handle, &wbyte, 1);
                        *ptr++ = wbyte;
                    } while (wbyte != 0);

                    Close(handle);

                    if (!strcasecmp(playername, bugname)) {
                        strcpy(GamePathname, pathname);
                        return TRUE;
                    }
                }
            }
        }
        closedir(dir);
    }

    return FALSE;
}
#endif

/*──────────────────────────────────────────────────────────────────────────*/

S32 FindPlayerFile() {
    struct dirent *file;
    DIR *dir;
    char pathname[ADELINE_MAX_PATH];
    char playername[MAX_SIZE_PLAYER_NAME + 1];
    U8 *ptr;
    S32 handle;
    S32 wlong;
    U8 wbyte;

    GetSavePath(pathname, ADELINE_MAX_PATH, NULL);
    dir = opendir(pathname);
    if (dir != NULL) {
        while ((file = readdir(dir)) != NULL) {
            if (ade_strcasestr(file->d_name, ".LBA") != NULL) {
                GetSavePath(pathname, ADELINE_MAX_PATH, file->d_name);

                struct stat st;
                stat(pathname, &st);
                if (S_ISREG(st.st_mode)) {
                    handle = OpenRead(pathname);
                    Read(handle, &wbyte, 1);             // num version
                    Read(handle, &wlong, sizeof(wlong)); // num cube

                    ptr = (U8 *)playername;
                    do {
                        Read(handle, &wbyte, 1);
                        *ptr++ = wbyte;

                    } while (wbyte != 0);

                    Close(handle);

                    if (!strcasecmp(PlayerName, playername)) {
                        strcpy(GamePathname, pathname);
                        return TRUE;
                    }
                }
            }
        }
        closedir(dir);
    }

    GetSavePath(pathname, ADELINE_MAX_PATH, AUTOSAVE_FILENAME);
    if (ExistsFileOrDir(pathname)) {
        strcpy(playername, PlayerName);

        strcpy(GamePathname, pathname);
        LoadGamePlayerName();

        if (!strcasecmp(PlayerName, playername)) {
            strcpy(PlayerName, playername);
            return TRUE;
        }

        strcpy(PlayerName, playername);
    }

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*
           █    ██▄ █ █▀▀▀█ █   █ ▀▀█▀▀       ██▄ █ █▀▀▀█ █▄ ▄█ █▀▀▀▀
           ██   ██▀██ ██▀▀▀ ██  █   ██        ██▀██ ██▀▀█ ██▀ █ ██▀▀
           ▀▀   ▀▀  ▀ ▀▀    ▀▀▀▀▀   ▀▀  ▀▀▀▀▀ ▀▀  ▀ ▀▀  ▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

/*──────────────────────────────────────────────────────────────────────────*/

#define DrawCursor() AffGraph(0, CursorX + 4, CursorY, HQR_Get(HQRPtrSprite, 9));

// Forward refs to marquee state defined below (DrawOneChoice's neighbourhood,
// ~line 1925). Reused here so the save-slot list can scroll overflowing names.
extern S32 WayChoice;
extern S32 DeltaChoice;
extern MOVE MoveChoice;
extern void InitShakeChoice(void);

#define NB_GAME_CHOICE 5
// Save/load + name-entry slot list origin (also feeds the hit-test and the
// name-entry cursor). Authored on the 480-tall canvas; UI_VCENTER_OFS floats
// the list to the vertical middle on a taller framebuffer (no-op at 480).
#define Y_START_CHOICE (205 + UI_VCENTER_OFS)

// --- Modern save / load page --------------------------------------------------
// The slot list, its title and the scene preview recorded as they are "drawn",
// then published to the UI overlay as cards (Menu_IsModern). Only while
// ChoosePlayerName runs: the same drawing calls serve other screens too.

static S32 s_savePage = FALSE;    ///< ChoosePlayerName is up
static S32 s_saveEditing = FALSE; ///< InputPlayerName is typing into the selected row

typedef struct {
    char Text[128];
    S32 Draw;
    S32 Valid;
    S32 Caret;
} T_SAVE_ROW;

static T_SAVE_ROW s_saveRows[5];
static char s_saveTitle[128];
static S32 s_savePreview = 0; ///< 0 none, 1 scene thumbnail, 2 new-game slot
static S32 s_savePreviewY = 0;
static U8 s_savePreviewRgba[160 * 120 * 4];
static U32 s_savePreviewVersion = 0;
static float s_saveFocus[5];
static U32 s_saveTime = 0;
static S32 s_saveConfirm = FALSE; ///< the delete confirmation is up (DeleteSavedGame)
static char s_saveConfirmName[128];

static U32 LerpColor(U32 a, U32 b, float t);

static S32 SavePage_Modern(void) {
    return s_savePage && Menu_IsModern();
}

static void SavePage_Reset(void) {
    memset(s_saveRows, 0, sizeof(s_saveRows));
    memset(s_saveFocus, 0, sizeof(s_saveFocus));
    s_saveTitle[0] = 0;
    s_savePreview = 0;
    s_saveConfirm = FALSE;
}

/* The scene thumbnail in true colour, through the palette the menu shows. */
static void SavePage_SetThumbnail(const U8 *indices, S32 y) {
    for (S32 i = 0; i < 160 * 120; i++) {
        const U8 *rgb = PtrPal + indices[i] * 3;
        s_savePreviewRgba[i * 4 + 0] = rgb[0];
        s_savePreviewRgba[i * 4 + 1] = rgb[1];
        s_savePreviewRgba[i * 4 + 2] = rgb[2];
        s_savePreviewRgba[i * 4 + 3] = 255;
    }
    s_savePreview = 1;
    s_savePreviewY = y;
    s_savePreviewVersion++;
}

static void SavePage_Hint(float x, float y, const char *key, S32 label, S32 alignRight) {
    char text[64];
    CopyUtf8ToCp850(text, sizeof text, GetLocalizedMenuLabel((enum MenuLocalizedLabel)label));
    const float keyW = UiOverlay_TextWidth(key, 13.0f) + 14.0f;
    const float textW = UiOverlay_TextWidth(text, 15.0f);
    const float total = keyW + 8.0f + textW;
    const float left = alignRight ? x - total : x;
    /* A dark pill behind the hint keeps it readable over any backdrop. */
    T_UIOVERLAY_CARD pill;
    memset(&pill, 0, sizeof(pill));
    pill.Radius = 13.0f;
    pill.FillTop = 0xB0101624u;
    pill.FillBottom = 0xB00A0E18u;
    pill.Border = 0x28FFFFFFu;
    pill.BorderWidth = 1.0f;
    UiOverlay_Card(left - 4.0f, y - 13.0f, left + total + 10.0f, y + 13.0f, &pill);
    T_UIOVERLAY_CARD cap;
    memset(&cap, 0, sizeof(cap));
    cap.Radius = 6.0f;
    cap.FillTop = 0xE0404A60u;
    cap.FillBottom = 0xE02A3044u;
    cap.Border = 0x60FFFFFFu;
    cap.BorderWidth = 1.0f;
    cap.Shadow = 0x60000000u;
    cap.ShadowY = 2.0f;
    cap.ShadowBlur = 4.0f;
    UiOverlay_Card(left, y - 10.0f, left + keyW, y + 10.0f, &cap);
    UiOverlay_Text(left + keyW * 0.5f, y, key, 13.0f, 0xFFE8EEF8u, UIOVERLAY_ALIGN_CENTER, TRUE, 0.0f);
    UiOverlay_Text(left + keyW + 8.0f, y, text, 15.0f, 0xFFD0DAE8u, UIOVERLAY_ALIGN_LEFT, TRUE, 0.0f);
}

static void SavePage_Preview(float cx, float y0, S32 kind, const char *label) {
    const float x0 = cx - 80.0f, x1 = cx + 80.0f, y1 = y0 + 120.0f;
    T_UIOVERLAY_CARD frame;
    memset(&frame, 0, sizeof(frame));
    frame.Radius = 12.0f;
    frame.FillTop = 0xF0283048u;
    frame.FillBottom = 0xF0161B2Au;
    frame.Border = 0x70B8DCFFu;
    frame.BorderWidth = 1.5f;
    frame.Shadow = 0x90000000u;
    frame.ShadowY = 6.0f;
    frame.ShadowBlur = 16.0f;
    frame.Glow = 0x305AB4FFu;
    frame.GlowSize = 10.0f;
    UiOverlay_Card(x0 - 7.0f, y0 - 7.0f, x1 + 7.0f, y1 + 7.0f, &frame);
    if (kind == 1) {
        UiOverlay_Image(x0, y0, x1, y1, s_savePreviewRgba, 160, 120, s_savePreviewVersion);
        /* A thin inner edge over the picture's border. */
        T_UIOVERLAY_CARD edge;
        memset(&edge, 0, sizeof(edge));
        edge.Radius = 3.0f;
        edge.Border = 0x50FFFFFFu;
        edge.BorderWidth = 1.0f;
        UiOverlay_Card(x0, y0, x1, y1, &edge);
    } else {
        UiOverlay_Sparkle(cx, y0 + 50.0f, 46.0f, 0xFFCFEAFFu);
        UiOverlay_Sparkle(cx - 34.0f, y0 + 30.0f, 16.0f, 0xB0CFEAFFu);
        UiOverlay_Sparkle(cx + 32.0f, y0 + 72.0f, 12.0f, 0x90CFEAFFu);
        if (label != NULL) {
            UiOverlay_Text(cx, y0 + 100.0f, label, 17.0f, 0xFFD6E6F6u, UIOVERLAY_ALIGN_CENTER, TRUE, 150.0f);
        }
    }
}

static void SavePage_Row(float cx, float y, const T_SAVE_ROW *row, float focus, float pulse, S32 isNew) {
    const S32 editing = s_saveEditing && (row->Draw & 1);
    const float halfW = 232.0f + 14.0f * focus;
    const float halfH = 20.0f + 3.0f * focus;
    const float x0 = cx - halfW, x1 = cx + halfW;
    T_UIOVERLAY_CARD card;
    memset(&card, 0, sizeof(card));
    card.Radius = 14.0f;
    card.FillTop = LerpColor(0xD8262E44u, 0xF2305284u, focus);
    card.FillBottom = LerpColor(0xD8171C2Cu, 0xF21C3560u, focus);
    card.Border = LerpColor(0x38FFFFFFu, editing ? 0xFFFFD27Au : 0xFF86CCFFu, focus);
    card.BorderWidth = 1.0f + focus;
    card.Shadow = LerpColor(0x70000000u, 0x90000000u, focus);
    card.ShadowY = 4.0f + 4.0f * focus;
    card.ShadowBlur = 10.0f + 8.0f * focus;
    if (focus > 0.01f) {
        const U32 glowAlpha = (U32)(focus * (110.0f + 60.0f * pulse));
        card.Glow = (glowAlpha << 24) | (editing ? 0xFFC24Au : 0x5AB4FFu);
        card.GlowSize = 12.0f;
    }
    UiOverlay_Card(x0, y - halfH, x1, y + halfH, &card);

    U32 color = LerpColor(0xFFD6DEEAu, 0xFFFFFFFFu, focus);
    if (isNew && !editing) {
        color = LerpColor(0xFF9FD8FFu, 0xFFCFEFFFu, focus);
        UiOverlay_Sparkle(x0 + 30.0f, y, 15.0f + 3.0f * pulse * focus, (0xB0u << 24) | 0xCFEAFFu);
    }
    const float maxW = (x1 - x0) - 110.0f;
    UiOverlay_Text(cx, y, row->Text, 23.0f, color, UIOVERLAY_ALIGN_CENTER, TRUE, maxW);
    if (editing && row->Caret) {
        float w = UiOverlay_TextWidth(row->Text, 23.0f);
        if (w > maxW) {
            w = maxW;
        }
        T_UIOVERLAY_CARD caret;
        memset(&caret, 0, sizeof(caret));
        caret.Radius = 1.0f;
        caret.FillTop = 0xFFFFE2A0u;
        caret.FillBottom = 0xFFFFC24Au;
        const float x = cx + w * 0.5f + 4.0f;
        UiOverlay_Card(x, y - 11.0f, x + 2.5f, y + 11.0f, &caret);
    }
    /* More slots above or below: arrows beside the list, clear of the preview. */
    if (row->Draw & ARROW_SG) {
        UiOverlay_Chevron(cx + 266.0f, y, 12.0f, -2, 0xE0B8DCFFu);
    }
    if (row->Draw & ARROW_ID) {
        UiOverlay_Chevron(cx + 266.0f, y, 12.0f, 2, 0xE0B8DCFFu);
    }
}

/* The delete confirmation's name and picture, under the confirm menu's cards. */
static void SavePage_Veil(void) {
    /* Dim the backdrop so the page reads as one layer above the game. */
    T_UIOVERLAY_CARD veil;
    memset(&veil, 0, sizeof(veil));
    veil.FillTop = 0x78000008u;
    veil.FillBottom = 0x50000008u;
    UiOverlay_Card(-4.0f, -4.0f, (float)ModeDesiredX + 4.0f, (float)ModeDesiredY + 4.0f, &veil);
}

static void SavePage_ConfirmDecor(void) {
    const float cx = (float)UI_CENTER_X;
    SavePage_Veil();
    T_SAVE_ROW row;
    memset(&row, 0, sizeof(row));
    SDL_strlcpy(row.Text, s_saveConfirmName, sizeof(row.Text));
    SavePage_Row(cx, (float)(80 + UI_VCENTER_OFS), &row, 0.0f, 0.0f, FALSE);
    if (s_savePreview == 1) {
        SavePage_Preview(cx, (float)s_savePreviewY, 1, NULL);
    }
}

static void SavePage_Publish(void) {
    if (!SavePage_Modern()) {
        return;
    }
    const float cx = (float)UI_CENTER_X;
    const U32 now = (U32)TimerRefHR;
    float dt = (float)(now - s_saveTime) / 1000.0f;
    if (dt < 0.0f || dt > 0.25f) {
        dt = 0.016f;
    }
    s_saveTime = now;
    const float step = SDL_min(dt * 40.0f, 1.0f);
    const float pulse = 0.5f + 0.5f * (float)sin((double)now / 1000.0 * 3.0);

    UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
    SavePage_Veil();
    if (s_saveTitle[0]) {
        const float ty = (float)(25 + UI_VCENTER_OFS);
        const float tw = UiOverlay_TextWidth(s_saveTitle, 30.0f);
        UiOverlay_Text(cx + 1.0f, ty + 2.0f, s_saveTitle, 30.0f, 0x90000000u, UIOVERLAY_ALIGN_CENTER, TRUE, 520.0f);
        UiOverlay_Text(cx, ty, s_saveTitle, 30.0f, 0xFFFFFFFFu, UIOVERLAY_ALIGN_CENTER, TRUE, 520.0f);
        const float half = SDL_min(tw, 520.0f) * 0.5f + 22.0f;
        UiOverlay_Sparkle(cx - half, ty, 16.0f, 0xE0CFEAFFu);
        UiOverlay_Sparkle(cx + half, ty, 16.0f, 0xE0CFEAFFu);
    }

    S32 selected = -1;
    S32 lastValid = -1;
    for (S32 n = 0; n < 5; n++) {
        if (s_saveRows[n].Valid) {
            lastValid = n;
            if (s_saveRows[n].Draw & 1) {
                selected = n;
            }
        }
    }
    if (s_savePreview != 0) {
        char label[64];
        label[0] = 0;
        if (s_savePreview == 2) {
            SDL_strlcpy(label, NewGameTxt, sizeof label);
        }
        SavePage_Preview(cx, (float)s_savePreviewY, s_savePreview, label);
        const float hy = (float)s_savePreviewY + 30.0f;
        SavePage_Hint(cx - 112.0f, hy, "Enter", MENU_LABEL_HINT_CONFIRM, TRUE);
        SavePage_Hint(cx - 112.0f, hy + 34.0f, "Esc", MENU_LABEL_HINT_BACK, TRUE);
        if (s_savePreview == 1 && !s_saveEditing) {
            SavePage_Hint(cx + 112.0f, hy, "Del", MENU_LABEL_HINT_DELETE, FALSE);
        }
    }

    for (S32 n = 0; n < 5; n++) {
        const T_SAVE_ROW *row = &s_saveRows[n];
        if (!row->Valid) {
            continue;
        }
        s_saveFocus[n] += ((n == selected ? 1.0f : 0.0f) - s_saveFocus[n]) * step;
        /* The new-game slot is always the list's last entry, shown last. */
        const S32 isNew = n == lastValid && !strcmp(row->Text, NewGameTxt);
        SavePage_Row(cx, (float)(Y_START_CHOICE + 60 * n), row, s_saveFocus[n], pulse, isNew);
    }
    /* A present only happens for a dirty frame. */
    BoxStaticAdd(0, 0, 1, 1);
}

void DrawOneString(S32 x, S32 y, char *playername, S32 draw) {
    S32 x0, y0, x1, y1;
    S32 dx;

    if (SavePage_Modern()) {
        const S32 n = (y - Y_START_CHOICE + 30) / 60;
        if (y >= Y_START_CHOICE - 30 && n >= 0 && n < 5) {
            T_SAVE_ROW *row = &s_saveRows[n];
            SDL_strlcpy(row->Text, playername, sizeof(row->Text));
            row->Draw = draw;
            row->Valid = TRUE;
            row->Caret = CursorActif;
            SavePage_Publish();
            return;
        }
    }

    x0 = x - LargeurMenu / 2;
    x1 = x + LargeurMenu / 2;

    y0 = y - HAUTEUR_STANDARD / 2;
    y1 = y + HAUTEUR_STANDARD / 2;

    draw &= 0x0F;

    // fond

    BackupAngles(x0, y0, x1, y1);

    if (draw == 0) {
        CopyBlock(x0, y0, x1, y1, Screen, x0, y0, Log);
        ShadeBoxBlk(x0, y0, x1, y1, MENU_SHADE_LVL);
    }
    if (draw == 1) {
        DrawFire(y0, FIRE_INPUT_NAME);
    }
    if (draw == 2) {
        Box(x0, y0, x1, y1, 91);
    }

    // cadre
    DrawCadre(x0, y0, x1, y1, ALL_ANGLES);

    RestoreAngles(x0, y0, x1, y1);

    // text — clipped to the box so long names (e.g. scene-aware gamepad
    // saves "Désert de la Feuille Blanche - 2026-05-17 14-30-00") cannot
    // bleed past the frame. If the text overflows the box: selected slot
    // (draw == 1) ping-pong-scrolls via WayChoice/DeltaChoice/MoveChoice,
    // mirroring DrawOneChoice (line ~2090); unselected slot is left-aligned
    // and visually truncated. Caller is responsible for InitShakeChoice()
    // on selection change so the scroll restarts from the left edge.
    ColorFont(CoulTextMenu);
    MemoClip();
    SetClip(x0 + 1, y0, x1 - 1, y1);
    dx = SizeFont(playername);
    if (dx > LargeurMenu) {
        if (draw != 1) {
            Font(x0 + 8, y - 18, playername);
        } else {
            if (WayChoice == -1)
                DeltaChoice -= GetDeltaMove(&MoveChoice);
            else
                DeltaChoice += GetDeltaMove(&MoveChoice);
            if (DeltaChoice < LargeurMenu - dx - 8) {
                DeltaChoice = LargeurMenu - dx - 8;
                WayChoice = 1;
            }
            if (DeltaChoice > 8) {
                DeltaChoice = 8;
                WayChoice = -1;
            }
            Font(x0 + DeltaChoice, y - 18, playername);
        }
    } else {
        Font(x - dx / 2, y - 18, playername);
    }
    RestoreClip();

    if (CursorActif)
        DrawCursor();

    // flip

    BoxStaticAdd(x0, y0, x1, y1);
}

/*──────────────────────────────────────────────────────────────────────────*/

void ClearOneString(S32 x, S32 y) {
    S32 x0, y0, x1, y1;

    if (SavePage_Modern()) {
        const S32 n = (y - Y_START_CHOICE + 30) / 60;
        if (y >= Y_START_CHOICE - 30 && n >= 0 && n < 5) {
            s_saveRows[n].Valid = FALSE;
        }
        SavePage_Publish();
        BoxUpdate();
        return;
    }

    x0 = x - LargeurMenu / 2;
    x1 = x + LargeurMenu / 2;

    y0 = y - HAUTEUR_STANDARD / 2;
    y1 = y + HAUTEUR_STANDARD / 2;

    // fond
    CopyBlock(x0, y0, x1, y1, Screen, x0, y0, Log);
    if (FlagShadeMenu)
        ShadeBoxBlk(x0, y0, x1, y1, SCREEN_SHADE_LVL);
    BoxStaticAdd(x0, y0, x1, y1);
    BoxUpdate();
}

/*──────────────────────────────────────────────────────────────────────────*/

void DrawSingleString(S32 x, S32 y, char *string) {
    S32 x0, y0, x1, y1;
    S32 sf;

    if (SavePage_Modern()) {
        SDL_strlcpy(s_saveTitle, string, sizeof(s_saveTitle));
        SavePage_Publish();
        return;
    }

    sf = SizeFont(string);

    x0 = x - sf / 2;
    x1 = x + sf / 2;

    y0 = y - HAUTEUR_STANDARD / 2;
    y1 = y + HAUTEUR_STANDARD / 2;

    // text
    ColorFont(BLACK);
    Font(x0 + 4, y - 18 + 4, string);
    ColorFont(CoulTextMenu);
    Font(x0, y - 18, string);

    // No flip, like DrawOneString: a page header goes up with the rows under
    // it, and flipping here showed the header alone over a bare backdrop for a
    // frame. Both callers flip once the page is composed.
    BoxStaticAdd(x0, y0, x1, y1);
}

/*──────────────────────────────────────────────────────────────────────────*/

void DrawCadreNewGame(void) {
    if (SavePage_Modern()) {
        s_savePreview = 2;
        s_savePreviewY = SAVE_THUMB_Y;
        SavePage_Publish();
        return;
    }

    S32 num = 16; // Logo Americain
    S32 dx, dy;
    S32 x, y;
    U8 *ptr;

    switch (DistribVersion) {
    case UNKNOWN_VERSION:
    case EA_VERSION:
        num = 11;
        break;
    }

    ptr = (U8 *)HQR_Get(HQRPtrSprite, num);

    GetDxDyGraph(0, &dx, &dy, ptr);

    DrawCadreSave(SAVE_THUMB_X, SAVE_THUMB_Y);

    x = UI_CENTER_X - dx / 2;
    y = SAVE_THUMB_Y + (SCREEN_SAVE_HEIGHT) / 2 - dy / 2;

    AffGraph(0, x, y, ptr);
}

/*──────────────────────────────────────────────────────────────────────────*/
#define GM_MENU_HALF_WIDTH_SAVE 275

static S32 GameMenuSaveListHitTest(U32 mx, U32 my, S32 nb, S32 start, S32 *outSel) {
    S32 n;
    S32 x0 = UI_CENTER_X - GM_MENU_HALF_WIDTH_SAVE;
    S32 x1 = UI_CENTER_X + GM_MENU_HALF_WIDTH_SAVE;

    for (n = 0; n < NB_GAME_CHOICE; n++) {
        S32 y;
        S32 y0;
        S32 y1;

        if (n + start >= nb)
            break;

        y = Y_START_CHOICE + 60 * n;
        y0 = y - HAUTEUR_STANDARD / 2;
        y1 = y + HAUTEUR_STANDARD / 2;
        if ((S32)mx >= x0 && (S32)mx <= x1 && (S32)my >= y0 && (S32)my <= y1) {
            *outSel = n + start;
            return 1;
        }
    }
    return 0;
}

// TODO(input): a pad or a touch overlay still cannot type a name. GetAscii reads
// the polled sample, so anything that writes Key reaches this screen, and a
// replayed recording and the control harness both do. Neither device does: the
// touch overlay writes TabKeys and leaves Key alone (ApplyVirtualKeys,
// TOUCH_INPUT.CPP), and a pad button lands in the joystick tail above
// TabKeys[256] rather than becoming a scancode at all. Two injectors writing the
// same array are not two injectors writing the same field. Until one of them
// does, both are routed past this function inside ChoosePlayerName: new-game slot
// gets a datetime stem, existing slot keeps its current name (overwrite). See the
// !LastInputWasKeyboard branch there.
static S32 InputPlayerNameLoop(S32 choice, char *name);

S32 InputPlayerName(S32 choice, char *name) {
    s_saveEditing = TRUE;
    const S32 ret = InputPlayerNameLoop(choice, name);
    s_saveEditing = FALSE;
    return ret;
}

static S32 InputPlayerNameLoop(S32 choice, char *name) {
    char memoplayername[255];
    S32 flag = 1;
    S32 retval = FALSE;
    S32 akey;
    S32 memoasciimode = AsciiMode;
    S32 x = 0, y = 0;
    S32 len;
    S32 timeraff, timernoaff;

    AsciiMode = TRUE;
    while (GetAscii())
        ;

    strcpy(memoplayername, name);
    strcpy(PlayerName, name);

    FindPlayerFile();

    // pour pouvoir effacer une sauvegarde sous Windows si le joueur
    // change le nom de la sauvegarde
    if (strcasecmp(PlayerName, AUTOSAVE_NAME)        // on n'efface pas l'autosave !
        AND strcasecmp(PlayerName, CURRENTSAVE_NAME) // ni le currentsave !
        AND strcasecmp(PlayerName, "")
            AND(ade_strcasestr(GamePathname, AUTOSAVE_FILENAME) == NULL) // on n'efface pas l'autosave !
        AND(ade_strcasestr(GamePathname, CURRENTSAVE_FILENAME) == NULL)) // ni le currentsave !
    {
        strcpy(OldGamePathname, GamePathname);
    }

    len = strlen(PlayerName);

    x = UI_CENTER_X;
    y = Y_START_CHOICE + choice * 60;

    ManageTime();
    timeraff = TimerRefHR + 20 * 20;
    CursorActif = TRUE;

    while (flag != -1) {
        /* The one screen that reads characters and never reaches MyGetInput, so the
           sample it reads has to be taken here. */
        ManageKeyboard();

        ManageTime();
        if (TimerRefHR < timeraff) {
            CursorX = x - 4;
            CursorY = y + 2;

            if (!len)
                CursorX -= 16;

            CursorActif = TRUE;
            timernoaff = TimerRefHR + 10 * 20;
        } else {
            if (TimerRefHR >= timernoaff) {
                timeraff = TimerRefHR + 20 * 20;
                CursorActif = TRUE;
                CursorX = x - 4;
                CursorY = y + 2;

                if (!len)
                    CursorX -= 16;
            } else
                CursorActif = FALSE;
        }

        DrawOneString(UI_CENTER_X, y, PlayerName, 1);
        BoxUpdate();

        if (flag == 1) {
            x = UI_CENTER_X + SizeFont(PlayerName) / 2;
            flag = 0;
        }

        do {
            akey = GetAscii();

            /* The illegal-filename set is a set of characters, so only ask it
               about keycodes that are one. A keycode carrying the scancode mask
               would be tested on the unrelated letter in its low byte. */
            if (A_IS_CHARACTER(akey) AND !IsValidChar(akey))
                akey = 0;

            switch (akey) {
            case A_ESC:
                strcpy(PlayerName, memoplayername);
                retval = FALSE;
                flag = -1;
                break;

            /* Retail treats keypad Enter as a universal confirm here: it commits
               the name whether or not the player has it bound to anything. */
            case A_RETURN:
            case A_NUMPAD_ENTER:
                if (IsDatetimeReservedShape(PlayerName)) {
                    // Reserved shape: refuse and bounce back to the prior name.
                    PlayErrorSample();
                    strcpy(PlayerName, memoplayername);
                    len = strlen(PlayerName);
                    flag = 1;
                } else if (strcmp(PlayerName, "")
                               AND((FindPlayerFile() == FALSE)
                                       OR(!strcmp(PlayerName, memoplayername)))) {
                    retval = TRUE;
                    flag = -1;
                } else {
                    PlayErrorSample();
                    strcpy(PlayerName, memoplayername);
                    len = strlen(PlayerName);
                    flag = 1;
                }
                break;

            case A_LEFT:
            case A_BACKSPACE:
                if (len > 0) {
                    PlayerName[--len] = '\0';
                    flag = 1;
                }
                break;

            default:
                /* Only a character belongs in the name. A keycode standing for
                   any other key carries the scancode mask, and the low byte
                   under it is an unrelated letter (keypad Enter's is 'X', Up's
                   is 'R'), so such a key is dropped rather than truncated into
                   the field. */
                if (!A_IS_CHARACTER(akey))
                    break;

                /* Max input cursor X = original 550 = centre(320) + 230 px. Keep the
                   same 230 px half-width at any resolution so the name-entry field
                   visible area stays at its authored 460 px width. */
                if (x < UI_CENTER_X + 230 AND akey >= 32 AND akey <= 122) {
                    PlayerName[len++] = (char)akey;
                    PlayerName[len] = '\0';
                    x = UI_CENTER_X + SizeFont(PlayerName) / 2;
                }
                break;
            }
            // end_switch
        } while (akey); // Tant que Buffer Clavier non vide
    }

    AsciiMode = memoasciimode;

    WaitNoInput();

    CursorActif = FALSE;

    return retval;
}

/*──────────────────────────────────────────────────────────────────────────*/
#define TEMPO_SCREEN_SAVE 110
#define TEMPO_REPEAT_KEY 150

static S32 ChoosePlayerNameLoop(S32 mess, S32 flagflip, S32 flagnew);

S32 ChoosePlayerName(S32 mess, S32 flagflip, S32 flagnew) {
    SavePage_Reset();
    s_savePage = TRUE;
    const S32 ret = ChoosePlayerNameLoop(mess, flagflip, flagnew);
    s_savePage = FALSE;
    SavePage_Reset();
    UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
    return ret;
}

static S32 ChoosePlayerNameLoop(S32 mess, S32 flagflip, S32 flagnew) {
    S32 flag = 3;
    char *listplayername;
    char **ptrlist;
    S32 n, nb;
    S32 start = 0;
    S32 select = 0;
    S32 ys = Y_START_CHOICE;
    S32 retval = 0;
    char string[256];
    U8 *ptr;
    S32 timerzoom;
    S32 timeranotherkey;
    S32 padNavDir = 0;
    S32 timerdraw = 1;
    U32 draw = 1;
    U32 newcube = NewCube;
    S32 save_press = -1;
    S32 save_prev_click = 0;
    S32 mouse_fire = 0;
    char memoplayername[MAX_SIZE_PLAYER_NAME + 1];
#ifdef DEBUG_TOOLS
    S32 isBugMode = FALSE;
#endif

    strcpy(OldGamePathname, "");

    strcpy(memoplayername, PlayerName);

    CopyScreen(Screen, Log);

    if (FlagShadeMenu)
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);

#ifdef DEBUG_TOOLS
    if (mess & 0x80000000) // chargement d'un bug
    {
        CoulText(LBAWHITE, 0);
        GraphPrintf(FALSE, 0, 460, "Chargement d'un bug...");
        isBugMode = TRUE;
    }

    if (mess & 0x40000000) // sauvegarde d'un bug
    {
        CoulText(LBAWHITE, 0);
        GraphPrintf(FALSE, 0, 460, "Sauvegarde d'un bug...");
        isBugMode = TRUE;
    }

    mess &= 0x3FFFFFFF;
#endif

    // Backdrop only: the list draws over it below and flips then. Flipping here
    // would put the bare plate on screen for a frame, which on the waves
    // background is a white flash of open sea. Same shape as GereVolumeMenu and
    // the other sub-pages.
    BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
    if (FlagShadeMenu)
        PaletteSync(PtrPalNormal);

    //      listplayername = (char *)Malloc( MAX_PLAYER * (MAX_SIZE_PLAYER_NAME+1) ) ;
    //      ptrlist = (char**)Malloc( MAX_PLAYER * sizeof(char*) ) ;

    listplayername = (char *)BufSpeak;
    ptrlist = (char **)(BufSpeak + MAX_PLAYER * (MAX_SIZE_PLAYER_NAME + 1));

    /*      if( !listplayername OR !ptrlist )
        {
                TheEnd( NOT_ENOUGH_MEM, "Choose Player Name" ) ;
	}
*/

#ifdef DEBUG_TOOLS
    if (isBugMode)
        nb = BugGameList(ptrlist, listplayername);
    else
#endif
        nb = PlayerGameList(ptrlist, listplayername, flagnew);

    strcpy(PlayerName, memoplayername);

    if (!nb AND !flagnew)
        return FALSE;

    if (flagnew AND nb < MAX_PLAYER - 1) {
        ptrlist[nb] = NewGameTxt;
        nb++;
    } else
        flagnew = 0;

    if (strcmp(PlayerName, "")) {
        for (select = 0; select < nb - 1; select++) {
            if (!strcasecmp(PlayerName, ptrlist[select]))
                break;
        }

        while (select >= (start + NB_GAME_CHOICE)) {
            if (start < (nb - 1)) {
                start++;
            }
        }
    }

    if (flagnew AND select == nb - 1)
        timerzoom = TimerRefHR;
    else
        timerzoom = 0;

    InitDial(0);

    InitWaitNoKey();
    padNavDir = JoystickMenuNavOnly();

    MenuMouseAcquire();
    save_press = -1;
    save_prev_click = 0;
    mouse_fire = 0;

    // Marquee state: starts left-aligned each time we enter the menu; reset
    // again on every selection change via the flag==1 redraw below.
    InitShakeChoice();

    while (MyKey != K_ESC AND !(Input & I_MENUS)) {
        ManageTime();

        if (flag == 3) {
            DrawSingleString(UI_CENTER_X, 25 + UI_VCENTER_OFS, (char *)GetMultiText(mess, string));
            flag = 1;
        }

        if (flag == 1) {
            // Reset marquee so the newly-selected slot starts scrolling
            // from the left edge — matches DrawGameMenu's pattern.
            InitShakeChoice();
            if (SavePage_Modern()) {
                memset(s_saveRows, 0, sizeof(s_saveRows));
            }
            for (n = 0; n < NB_GAME_CHOICE; n++) {
                if (n + start >= nb)
                    break;

                draw = ((n + start) == select);

                if ((n + start) == select) {
                    ys = Y_START_CHOICE + 60 * n;
                }

                if (start > 0 AND n == 0)
                    draw |= ARROW_SG;
                else if ((start + NB_GAME_CHOICE < nb) AND(n == NB_GAME_CHOICE - 1))
                    draw |= ARROW_ID;

                DrawOneString(UI_CENTER_X, Y_START_CHOICE + 60 * n, ptrlist[n + start], draw);
            }

            draw = 1;

            if (start > 0 AND select == start)
                draw |= ARROW_SG;
            else if ((start + NB_GAME_CHOICE < nb) AND(select == start + NB_GAME_CHOICE - 1))
                draw |= ARROW_ID;

            /*                      while( Key )
                        {
                                DrawOneString( 320, ys, ptrlist[select], draw ) ;
                        }
*/
            timeranotherkey = TimerRefHR + TEMPO_REPEAT_KEY;
            Input = MyKey = 0;
            flag = 0;
        }

        // Per-frame: redraw the selected slot so the marquee scroll advances
        // on long names that don't fit the box (DrawOneString does the
        // overflow detection internally). The flag==1 path above only fires
        // on selection change; this keeps the animation alive in between.
        {
            S32 sn = select - start;
            if (sn >= 0 && sn < NB_GAME_CHOICE && (start + sn) < nb) {
                S32 sdraw = 1;
                if (start > 0 AND sn == 0)
                    sdraw |= ARROW_SG;
                else if ((start + NB_GAME_CHOICE < nb) AND(sn == NB_GAME_CHOICE - 1))
                    sdraw |= ARROW_ID;
                DrawOneString(UI_CENTER_X, Y_START_CHOICE + 60 * sn, ptrlist[select], sdraw);
            }
        }

        if (timerdraw /*AND TimerRefHR>=timerdraw*/) {
            strcpy(PlayerName, ptrlist[select]);

            if (!flagnew OR select < nb - 1) {
#ifdef DEBUG_TOOLS
                if (isBugMode)
                    FindBugFile(PlayerName);
                else
#endif
                    FindPlayerFile();
                ptr = LoadGameScreen();
                if (ptr)
                    DrawScreenSave(ptr, SAVE_THUMB_X, SAVE_THUMB_Y);
            } else // Nouvelle partie
            {
                //                                DrawCadreSave( 240, 50 ) ;
                //				DrawSingleString( 320, 105, "New" ) ;

                DrawCadreNewGame();
            }

            timerdraw = 0;
        }

        if (FlagMenuMouse && MenuMouse_GetRefCount() > 0) {
            MyGetInput();
        } else if (TimerRefHR > timeranotherkey) {
            MyGetInput();
        }

        /* This loop compares MyKey against the navigation cluster directly, so
           the keypad legend has to be resolved before any of those tests. Page
           Up/Down, Home, End and Delete have no Input action to arrive through,
           which would otherwise leave the list unreachable from a keyboard
           without a dedicated navigation block. */
        MyKey = MenuKey_NumpadToNav(MyKey);

        /* Suppress nav keys outside the repeat window; translate gamepad
           D-pad / stick to K_GRAY_UP/DOWN when the window is open.
           Edge-only for gamepad: fire on direction change so a deflected
           stick on menu entry does not auto-fire. */
        if (TimerRefHR <= timeranotherkey) {
            if (MyKey == K_GRAY_UP || MyKey == K_GRAY_DOWN)
                MyKey = 0;
            Input &= ~(I_UP | I_DOWN);
        } else if (!MyKey) {
            S32 jdir = JoystickMenuNavOnly();
            if (jdir != padNavDir) {
                padNavDir = jdir;
                if (jdir == K_UP)
                    MyKey = K_GRAY_UP;
                else if (jdir == K_DOWN)
                    MyKey = K_GRAY_DOWN;
            }
        }

        if (FlagMenuMouse && MenuMouse_GetRefCount() > 0) {
            S32 hitSel = -1;
            S32 cur, leftDown, prevLeft, leftDownEdge, leftUpEdge, relSel;
            S32 wy;
            bool saveMoved;

            GameMenuMousePollFocus();
            saveMoved = MouseConsumeMotionFlag();
            cur = Click;
            leftDown = (cur & 1) != 0;
            prevLeft = (save_prev_click & 1) != 0;
            leftDownEdge = leftDown && !prevLeft;
            leftUpEdge = !leftDown && prevLeft;
            save_prev_click = cur;

            if (saveMoved && GameMenuSaveListHitTest(MouseX, MouseY, nb, start, &hitSel)) {
                if (!leftDown && save_press < 0 && hitSel != select) {
                    select = hitSel;
                    flag = 1;
                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
            }

            if (leftDownEdge && GameMenuSaveListHitTest(MouseX, MouseY, nb, start, &hitSel)) {
                save_press = hitSel;
                if (select != hitSel) {
                    select = hitSel;
                    flag = 1;
                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
            }

            if (leftDown && save_press >= 0) {
                if (select != save_press) {
                    select = save_press;
                    flag = 1;
                }
            }

            if (leftUpEdge && save_press >= 0) {
                relSel = -1;
                if (GameMenuSaveListHitTest(MouseX, MouseY, nb, start, &relSel) && relSel == save_press)
                    mouse_fire = 1;
                save_press = -1;
            }

            wy = ConsumeMouseWheelY();
            while (wy > 0) {
                if (select < (nb - 1)) {
                    select++;
                    if (select >= (start + NB_GAME_CHOICE)) {
                        if (start < (nb - 1)) {
                            start++;
                        }
                    } else {
                        DrawOneString(UI_CENTER_X, ys, ptrlist[select - 1], (draw & 0xFFFFFFFE));
                    }
                    flag = 1;

                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
                wy--;
            }
            while (wy < 0) {
                if (select > 0) {
                    select--;

                    if (select < start) {
                        start--;
                    } else {
                        DrawOneString(UI_CENTER_X, ys, ptrlist[select + 1], (draw & 0xFFFFFFFE));
                    }
                    flag = 1;
                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
                wy++;
            }
        }

        DrawOneString(UI_CENTER_X, ys, ptrlist[select], draw);
        BoxUpdate();

        if (Input & I_DOWN OR MyKey == K_GRAY_DOWN) {
            if (select < (nb - 1)) {
                select++;
                if (select >= (start + NB_GAME_CHOICE)) {
                    if (start < (nb - 1)) {
                        start++;
                    }
                } else {
                    DrawOneString(UI_CENTER_X, ys, ptrlist[select - 1], (draw & 0xFFFFFFFE));
                }
                flag = 1;

                if (flagnew AND select == nb - 1)
                    timerzoom = TimerRefHR;
                else
                    timerzoom = 0;
                timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
            }
        }

        if (Input & I_UP OR MyKey == K_GRAY_UP) {
            if (select > 0) {
                select--;

                if (select < start) {
                    start--;
                } else {
                    DrawOneString(UI_CENTER_X, ys, ptrlist[select + 1], (draw & 0xFFFFFFFE));
                }
                flag = 1;
                if (flagnew AND select == nb - 1)
                    timerzoom = TimerRefHR;
                else
                    timerzoom = 0;
                timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
            }
        }

        //              if( !Input )    // on s'assure qu'aucune des touches testées
        // ci-dessous ne fait partie des Inputs
        {
            if (MyKey == K_GRAY_PAGE_DOWN) {
                if (select < (nb - 1)) {
                    S32 oldselect = select;

                    select += NB_GAME_CHOICE;
                    if (select >= nb)
                        select = nb - 1;

                    if (select >= (start + NB_GAME_CHOICE)) {
                        start += NB_GAME_CHOICE;
                        if (start >= (nb - (NB_GAME_CHOICE - 1)))
                            start = nb - NB_GAME_CHOICE;
                    } else {
                        DrawOneString(UI_CENTER_X, ys, ptrlist[oldselect], (draw & 0xFFFFFFFE));
                    }
                    flag = 1;

                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
            }

            if (MyKey == K_GRAY_PAGE_UP) {
                if (select > 0) {
                    S32 oldselect = select;

                    select -= NB_GAME_CHOICE;
                    if (select < 0)
                        select = 0;

                    if (select < start) {
                        start = select;
                    } else {
                        DrawOneString(UI_CENTER_X, ys, ptrlist[oldselect], (draw & 0xFFFFFFFE));
                    }
                    flag = 1;
                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
            }

            if (MyKey == K_GRAY_HOME) {
                if (select > 0) {
                    S32 oldselect = select;

                    select = 0;

                    if (start == 0) {
                        DrawOneString(UI_CENTER_X, ys, ptrlist[oldselect], (draw & 0xFFFFFFFE));
                    } else
                        start = 0;

                    flag = 1;
                    if (flagnew AND select == nb - 1)
                        timerzoom = TimerRefHR;
                    else
                        timerzoom = 0;
                    timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
                }
            }
        }

        if (MyKey == K_GRAY_END) {
            if (select < (nb - 1)) {
                S32 oldselect = select;

                select = nb - 1;

                if (start == (nb - NB_GAME_CHOICE)) {
                    DrawOneString(UI_CENTER_X, ys, ptrlist[oldselect], (draw & 0xFFFFFFFE));
                } else {
                    start = nb - NB_GAME_CHOICE;
                    if (start < 0)
                        start = 0;
                }

                flag = 1;
                if (flagnew AND select == nb - 1)
                    timerzoom = TimerRefHR;
                else
                    timerzoom = 0;
                timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
            }
        }

        // Effacement d'une sauvegarde
        if (MyKey == K_D OR MyKey == K_BACKSPACE OR MyKey == K_GRAY_SUPPR) {
            CopyScreen(Screen, Log);
            if (FlagShadeMenu)
                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
            BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);

            if (DeleteSavedGame()) {
                if (nb > 1 AND select < nb - 1) {
                    memcpy(&ptrlist[select], &ptrlist[select + 1], ((nb - 1) - select) * sizeof(char *));
                }

                // recharge list player
                nb--;

                if (select >= nb) {
                    select = nb - 1;
                    if (select < 0)
                        select = 0;

                    if (select < start) {
                        start = select;
                        if (start < 0)
                            start = 0;
                    }
                }

                if (!nb)
                    break; // plus de sauvegarde
            }

            CopyScreen(Screen, Log);
            if (FlagShadeMenu)
                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
            BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);

            flag = 1; // redraw menu
            timerdraw = TimerRefHR + TEMPO_SCREEN_SAVE;
        }

        if (Input & I_FIRE OR MyKey == K_ENTER OR MyKey == K_NUMPAD_ENTER OR mouse_fire) {
            if (mouse_fire)
                mouse_fire = 0;
            if (timerdraw) {
                strcpy(PlayerName, ptrlist[select]);

                if (!flagnew OR select < nb - 1) {
#ifdef DEBUG_TOOLS
                    if (isBugMode)
                        FindBugFile(PlayerName);
                    else
#endif
                        FindPlayerFile();
                    ptr = LoadGameScreen();
                    if (ptr)
                        DrawScreenSave(ptr, SAVE_THUMB_X, SAVE_THUMB_Y);
                } else // Nouvelle partie
                {
                    //					DrawCadreSave( 240, 50 ) ;
                    //					DrawSingleString( 320, 105, "New" ) ;
                    DrawCadreNewGame();
                }

                timerdraw = 0;
            }

            if (flagnew) // Nouvelle partie
            {
                char memoname[ADELINE_MAX_PATH];

                if (select == nb - 1)
                    strcpy(PlayerName, "");
                else {
#ifdef DEBUG_TOOLS
                    if (isBugMode)
                        FindBugFile(PlayerName);
                    else
#endif
                        FindPlayerFile();
                    strcpy(memoname, GamePathname);
                }

                while (Input & I_FIRE OR MenuKey_NumpadToNav(MyKey) == K_ENTER)
                    MyGetInput();

                // Non-keyboard routing: unless the last input was a physical
                // keyboard, the player has no practical way to type a name
                // (InputPlayerName is keyboard-only) — this covers gamepad,
                // touch, and mouse. Android TV is forced down this path too:
                // there is no text keyboard on a remote, and SDL's routing of
                // remote D-pad keys is device-dependent, so IsAndroidTVDevice()
                // makes the choice deterministic (it is a no-op stub off
                // Android, so desktop and phone are unaffected). For a "New
                // Game" slot we synthesise a local-time datetime stem; for an
                // existing slot we keep the slot's own name and just overwrite.
                // Same-second collision overwrites silently — matches the
                // quick-save reflex chosen during design. The menu UI (slot
                // list + New Game entry) is identical to the keyboard flow;
                // only the text-entry step is bypassed.
                if (!LastInputWasKeyboard || IsAndroidTVDevice()) {
                    S32 ok = TRUE;
                    if (select == nb - 1) {
                        char datetime[32];
                        time_t now = time(NULL);
                        struct tm *lt = localtime(&now);
                        if (lt == NULL || strftime(datetime, sizeof datetime,
                                                   "%Y-%m-%d %H-%M-%S", lt) != 19) {
                            ok = FALSE;
                        } else {
                            // Scene-aware name: "<island> - <datetime>", with
                            // bare datetime as fallback. Island names live in
                            // text file 2 (gam); MainGameMenu has file 0 (sys)
                            // loaded right now (InitDial(0) at ~line 3330), so
                            // bracket the lookup and restore sys after — the
                            // menu redraws on the next loop iteration assume
                            // sys is still loaded. TabArrow defaults to zeros
                            // before the player opens the holomap, so the
                            // Mess > 0 guard avoids querying message 0 (which
                            // is a real but unrelated entry in some banks).
                            char islandName[256];
                            islandName[0] = '\0';
                            if (Island >= 0 && Island < MAX_ISLAND && TabArrow[Island].Mess > 0) {
                                InitDial(2);
                                if (GetMultiText(TabArrow[Island].Mess,
                                                 islandName) != NULL) {
                                    // Holomap data ships with a trailing '.'
                                    // ("Citadel Island."); strip for a cleaner
                                    // save name.
                                    size_t ilen = strlen(islandName);
                                    if (ilen > 0 && islandName[ilen - 1] == '.') {
                                        islandName[ilen - 1] = '\0';
                                    }
                                }
                                InitDial(0);
                            }
                            if (islandName[0]) {
                                // %.60s caps the island portion so the format
                                // is provably bounded (60 + 3 + 19 = 82 chars,
                                // safely under MAX_SIZE_PLAYER_NAME = 100) and
                                // -Wformat-truncation stays quiet. Longest
                                // known retail island name is 29 chars.
                                snprintf(PlayerName, MAX_SIZE_PLAYER_NAME + 1,
                                         "%.60s - %.19s", islandName, datetime);
                            } else {
                                strncpy(PlayerName, datetime,
                                        MAX_SIZE_PLAYER_NAME);
                                PlayerName[MAX_SIZE_PLAYER_NAME] = '\0';
                            }
                        }
                    }
                    // Existing slot: PlayerName already holds the slot name
                    // from the strcpy earlier in this loop (around line 1660),
                    // so nothing to fill — just resolve the path.
                    if (ok) {
                        char tmpPlayerFilename[ADELINE_MAX_PATH];
                        snprintf(tmpPlayerFilename, ADELINE_MAX_PATH, "%s.LBA",
                                 PlayerName);
#ifdef DEBUG_TOOLS
                        if (isBugMode)
                            GetBugPath(GamePathname, ADELINE_MAX_PATH,
                                       tmpPlayerFilename);
                        else
#endif
                            GetSavePath(GamePathname, ADELINE_MAX_PATH,
                                        tmpPlayerFilename);
                        retval = 2;
                        break;
                    }
                    // strftime is effectively infallible on real time_t; if
                    // it does fail, fall through to the keyboard flow rather
                    // than write a garbage filename.
                }

                if (InputPlayerName(select - start, PlayerName)) {
                    char tmpPlayerFilename[ADELINE_MAX_PATH];
                    if (select == nb - 1) {
                        do {
                            snprintf(tmpPlayerFilename, ADELINE_MAX_PATH, "%s.LBA", PlayerName);
#ifdef DEBUG_TOOLS
                            if (isBugMode)
                                GetBugPath(GamePathname, ADELINE_MAX_PATH, tmpPlayerFilename);
                            else
#endif
                                GetSavePath(GamePathname, ADELINE_MAX_PATH, tmpPlayerFilename);
                        } while (FileSize(GamePathname) != 0); // TODO: Review this sorcery
                    } else {
                        // renommer une sauvegarde
                        snprintf(tmpPlayerFilename, ADELINE_MAX_PATH, "%s.LBA", PlayerName);
#ifdef DEBUG_TOOLS
                        if (isBugMode)
                            GetBugPath(GamePathname, ADELINE_MAX_PATH, tmpPlayerFilename);
                        else
#endif
                            GetSavePath(GamePathname, ADELINE_MAX_PATH, tmpPlayerFilename);
                    }

                    retval = 2;
                    break;
                } else {
                    while (Input & I_FIRE OR MenuKey_NumpadToNav(MyKey) == K_ENTER)
                        MyGetInput();
                    if (select == nb - 1)
                        strcpy(PlayerName, NewGameTxt);
                    timerdraw = 1;
                    flag = 3; // Rafraichissement ecran
                }
            } else {
                retval = 1;
                break;
            }
        }
    }

    MenuMouseRelease();

    if (Key == K_ESC OR Input & I_MENUS) {
        NewCube = newcube;
        strcpy(PlayerName, memoplayername);
    } else if (retval == 1) {
        strcpy(PlayerName, ptrlist[select]);
#ifdef DEBUG_TOOLS
        if (isBugMode)
            FindBugFile(PlayerName);
        else
#endif
            FindPlayerFile();

        for (n = 0; n < NB_GAME_CHOICE; n++) {
            if ((n + start) != select) {
                ClearOneString(UI_CENTER_X, Y_START_CHOICE + 60 * n);
            }
        }
    }

    //      Free( ptrlist ) ;
    //      Free( listplayername ) ;

    if (flagflip) {
        CopyScreen(Screen, Log);
        if (FlagShadeMenu)
            ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
        BoxStaticFullflip();
    }

    SelectPlayer = select - start;

    return retval;
}

////////************* TEMPO, devrait etre en LIB ?

S32 HQR_GiveOffset(T_HQR_HEADER *header, S32 index) {
    S32 handle;
    S32 nbbloc;

    handle = OpenRead(header->Name);
    if (!handle)
        return 0;

    index *= 4;
    Read(handle, &nbbloc, 4);

    if (index >= nbbloc) {
        Close(handle);
        return 0;
    }

    Seek(handle, index, SEEK_FROM_START);
    Read(handle, &index, 4);

    Close(handle);
    return index;
}

////////*************

/*══════════════════════════════════════════════════════════════════════════*
                            █▄ ▄█ █▀▀▀▀ ██▄ █ █   █
                            ██▀ █ ██▀▀  ██▀██ ██  █
                            ▀▀  ▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 LastSampleVolume;
S32 SampleMenu;
S32 LastVoiceVolume;
S32 VoiceMenu;
U32 PanMenu;

S32 WayChoice;
S32 DeltaChoice;
MOVE MoveChoice;

/*──────────────────────────────────────────────────────────────────────────*/
void InitShakeChoice() {
    WayChoice = -1; //     To the Left First
    DeltaChoice = 0;

    InitMove(&MoveChoice, 48);
}
/*──────────────────────────────────────────────────────────────────────────*/

void DrawOneChoice(S32 x, S32 y, S32 type, S32 num, S32 select) {
    S32 x2, x0, y0, x1, y1;
    S32 dx;
    char string[256];
    S32 value;
    S32 drawslider = FALSE;
    U8 *samplePtr;
    S32 sampleSizeBytes;

    x0 = x - LargeurMenu / 2;
    x1 = x + LargeurMenu / 2;

    y0 = y - HAUTEUR_STANDARD / 2;
    y1 = y + HAUTEUR_STANDARD / 2;

    // fond

    BackupAngles(x0, y0, x1, y1);

    // select is a bitmask: bit 0 = cursor row (blue plasma, the usual
    // highlight), bit 1 = active-value marker (green plasma, e.g. the current
    // resolution in Resolution_MainCapture). Cursor wins when a row is both.
    if (select & 1) {
        switch (type) {
        case 2: // sample volume
            value = SampleVolume;
            drawslider = TRUE;
            break;

        case 3: // voice volume
            value = VoiceVolume;
            drawslider = TRUE;
            break;

        case 4: // Jingle volume
            value = JingleVolume;
            drawslider = TRUE;
            break;

        case 5: // CD volume
#ifdef CDROM
            value = CDVolume;
#else
            value = 127;
#endif
            drawslider = TRUE;
            break;

        case 6: // master volume
            value = MasterVolume;
            drawslider = TRUE;
            break;

        case 7: // Niveau de détails
            x2 = RegleTrois(x0, x1, MAX_DETAIL_LEVEL, DetailLevel);
            DrawFire(y0, FIRE_SLIDER_MENU);
            Box(x2, y0, x1, y1, COUL_SELECT_MENU);
            break;

        default:
            DrawFire(y0, FIRE_SELECT_MENU);
        }

        if (drawslider) {
            x2 = RegleTrois(x0, x1, 127, value);
            DrawFire(y0, FIRE_SLIDER_MENU);
            Box(x2, y0, x1, y1, COUL_SELECT_MENU);

            if (SamplesEnable) {
                switch (type) {
                case 2: // sample volume
                case 6: // Master volume
                    // bruitons un peu la situation (violement)

                    if ((TimerRefHR - TimerSample) > 35 * 20 AND !IsSamplePlaying(SampleMenu)) {
                        TimerSample = TimerRefHR;

                        do {
                            SampleMenu = 0 + MyRnd(28);
                        } while (!HQR_GiveOffset(HQR_Samples, SampleMenu));

                        PanMenu = 20 + MyRnd(88);

                        HQ_MixSample(SampleMenu, 0x1000, 600, 1, PanMenu, 127);
                    } else if (SampleVolume != LastSampleVolume) {
                        HQ_ChangePanSample(SampleMenu, 127, PanMenu);
                        LastSampleVolume = SampleVolume;
                    }
                    break;

#ifndef DEMO
                case 3: // voice volume
                    // bruitons un peu la situation (violement)

                    if ((TimerRefHR - TimerSample) > 5000 AND !IsSamplePlaying(VoiceMenu)) {
                        TimerSample = TimerRefHR;

                        VoiceMenu = SAMPLE_VOICE_MENU + LanguageCD;

                        PanMenu = 20 + MyRnd(88);

                        samplePtr = (U8 *)GivePtrSample(VoiceMenu, &sampleSizeBytes);
                        PlaySample(samplePtr, sampleSizeBytes, VoiceMenu, 0x1000, 1, VoiceVolume, PanMenu);
                    } else if (VoiceVolume != LastVoiceVolume) {
                        ChangeVolumePanSample(VoiceMenu, VoiceVolume, PanMenu);
                        LastVoiceVolume = VoiceVolume;
                    }
                    break;
#endif

                case 4: // Music Volume
                    if ((TimerRefHR - TimerSample) > 1000) {
                        TimerSample = TimerRefHR;
#ifdef CDROM
                        if (IsCDPlaying())
                            StopMusic();
#endif
                        PlayMusic(JINGLE_TRACK_MENU, TRUE);
                    }
                    break;

                case 5: // CD Volume
                    if ((TimerRefHR - TimerSample) > 1000) {
                        TimerSample = TimerRefHR;
#ifdef CDROM
                        if (IsStreamPlaying())
                            StopMusic();
                        PlayMusic(CD_TRACK_MENU, TRUE);
#endif
                    }
                    break;
                }
            }
        }

    } else if (select & 2) {
        // Active-value marker (not the cursor): green plasma bar.
        DrawFire(y0, FIRE_CURRENT_MENU);
    } else {
        CopyBlock(x0, y0, x1, y1, Screen, x0, y0, Log);
        ShadeBoxBlk(x0, y0, x1, y1, MENU_SHADE_LVL);
    }

    // cadre
    DrawCadre(x0, y0, x1, y1, ALL_ANGLES);
    RestoreAngles(x0, y0, x1, y1);

    // text

    ColorFont(CoulTextMenu);
    BuildCustomMenuText(num, string);

    //----------------------------------------------------------------------------
    MemoClip();
    SetClip(x0 + 1, y0, x1 - 1, y1);

    dx = SizeFont(string);

    if (dx > LargeurMenu) //       Biggest than window
    {
        if (!select)
            Font(x0 + 8, y - 18, string); // To the Left
        else {                            //     Shake Baby, shake...
            if (WayChoice == -1)
                DeltaChoice -= GetDeltaMove(&MoveChoice);
            else
                DeltaChoice += GetDeltaMove(&MoveChoice);

            if (DeltaChoice < LargeurMenu - dx - 8) {
                DeltaChoice = LargeurMenu - dx - 8;
                WayChoice = 1; // Right
            }
            if (DeltaChoice > 8) {
                DeltaChoice = 8;
                WayChoice = -1; // Left
            }
            Font(x0 + DeltaChoice, y - 18, string);
        }
    } else
        Font(x - SizeFont(string) / 2, y - 18, string);

    if (type == MENU_TYPE_DISPLAY_SELECTOR) {
        // Left and right arrows at the row's edges: the value changes with them.
        const S32 half = 7;
        const S32 left = x0 + 18;
        const S32 right = x1 - 18;
        for (S32 i = 0; i <= half; i++) {
            Box(left + i, y - i, left + i, y + i, CoulTextMenu);
            Box(right - i, y - i, right - i, y + i, CoulTextMenu);
        }
    }

    RestoreClip();
    //----------------------------------------------------------------------------

    // flip

    BoxStaticAdd(x0, y0, x1, y1);
}

/*──────────────────────────────────────────────────────────────────────────*/

// --- Modern menu ------------------------------------------------------------
// Floating cards in true colour, drawn by the UI overlay over the frame. Rows sit
// where the classic bars do (same centres and height), so mouse hit-testing and
// every caller's layout hold; Log keeps only the page's backdrop.

S32 Menu_IsModern(void) {
    if (Control_IsHeadless()) {
        return FALSE; /* no window to draw the overlay in: harness captures stay classic */
    }
    return MenuStyle == MENU_STYLE_MODERN || (MenuStyle == MENU_STYLE_AUTO && GpuObjEnabled);
}

#define MODERN_MAX_ROWS 64
static float s_modernFocus[MODERN_MAX_ROWS]; ///< 0..1 eased highlight per row
static const U16 *s_modernMenu = NULL;
static U32 s_modernTime = 0;

static U32 LerpColor(U32 a, U32 b, float t) {
    U32 out = 0;
    for (S32 shift = 0; shift < 32; shift += 8) {
        const float ca = (float)((a >> shift) & 0xFF);
        const float cb = (float)((b >> shift) & 0xFF);
        out |= (U32)(ca + (cb - ca) * t + 0.5f) << shift;
    }
    return out;
}

// Slider value 0..1 for the volume and detail rows, -1 for other rows.
static float ModernSliderValue(S32 type) {
    switch (type) {
    case 2:
        return (float)SampleVolume / 127.0f;
    case 3:
        return (float)VoiceVolume / 127.0f;
    case 4:
        return (float)JingleVolume / 127.0f;
    case 5:
#ifdef CDROM
        return (float)CDVolume / 127.0f;
#else
        return 1.0f;
#endif
    case 6:
        return (float)MasterVolume / 127.0f;
    case 7:
        return (float)DetailLevel / (float)MAX_DETAIL_LEVEL;
    }
    return -1.0f;
}

static void DrawModernRow(S32 y, S32 type, S32 num, S32 sel, float focus, float pulse) {
    char string[256];
    BuildCustomMenuText(num, string);

    const float cx = (float)UI_CENTER_X;
    const float halfW = 232.0f + 14.0f * focus;
    const float halfH = 20.0f + 3.0f * focus;
    const float x0 = cx - halfW;
    const float x1 = cx + halfW;
    const float y0 = (float)y - halfH;
    const float y1 = (float)y + halfH;

    T_UIOVERLAY_CARD card;
    memset(&card, 0, sizeof(card));
    card.Radius = 14.0f;
    card.FillTop = LerpColor(0xD8262E44u, 0xF2305284u, focus);
    card.FillBottom = LerpColor(0xD8171C2Cu, 0xF21C3560u, focus);
    card.Border = LerpColor(0x38FFFFFFu, 0xFF86CCFFu, focus);
    card.BorderWidth = 1.0f + focus;
    card.Shadow = LerpColor(0x70000000u, 0x90000000u, focus);
    card.ShadowX = 0.0f;
    card.ShadowY = 4.0f + 4.0f * focus;
    card.ShadowBlur = 10.0f + 8.0f * focus;
    if (focus > 0.01f) {
        const U32 glowAlpha = (U32)(focus * (110.0f + 60.0f * pulse));
        card.Glow = (glowAlpha << 24) | 0x5AB4FFu;
        card.GlowSize = 12.0f;
    }
    if (sel & 2) {
        /* The active value (current resolution): a green edge. */
        card.Border = LerpColor(0xFF6ED49Au, card.Border, focus);
        card.BorderWidth = SDL_max(card.BorderWidth, 1.5f);
    }
    UiOverlay_Card(x0, y0, x1, y1, &card);

    const U32 textColor = LerpColor(0xFFD6DEEAu, 0xFFFFFFFFu, focus);
    const U32 accent = 0xFF9FD8FFu;
    const float textSize = 23.0f;
    const float slider = ModernSliderValue(type);

    if (type == MENU_TYPE_DISPLAY_SELECTOR) {
        /* "Label: value": label on the left, value on the right between arrows. */
        const char *colon = strstr(string, ": ");
        const U32 arrow = LerpColor(0x80FFFFFFu, accent, focus);
        UiOverlay_Chevron(x0 + 26.0f - 3.0f * focus * pulse, (float)y, 16.0f, -1, arrow);
        UiOverlay_Chevron(x1 - 26.0f + 3.0f * focus * pulse, (float)y, 16.0f, 1, arrow);
        if (colon != NULL) {
            char label[256];
            const size_t n = (size_t)(colon - string);
            memcpy(label, string, n);
            label[n] = 0;
            const float valueW = UiOverlay_TextWidth(colon + 2, textSize);
            const float room = (x1 - x0) - 100.0f;
            UiOverlay_Text(x0 + 50.0f, (float)y, label, textSize, textColor, UIOVERLAY_ALIGN_LEFT, TRUE,
                           SDL_max(room - valueW, room * 0.4f));
            UiOverlay_Text(x1 - 50.0f, (float)y, colon + 2, textSize, LerpColor(0xFFB8CCE0u, accent, focus),
                           UIOVERLAY_ALIGN_RIGHT, TRUE, room * 0.6f);
        } else {
            UiOverlay_Text(cx, (float)y, string, textSize, textColor, UIOVERLAY_ALIGN_CENTER, TRUE, (x1 - x0) - 100.0f);
        }
    } else if (slider >= 0.0f) {
        /* Label on the left, a bar with the level on the right. */
        const float barX0 = cx + 10.0f;
        const float barX1 = x1 - 26.0f;
        UiOverlay_Text(x0 + 26.0f, (float)y, string, textSize, textColor, UIOVERLAY_ALIGN_LEFT, TRUE,
                       barX0 - x0 - 40.0f);
        T_UIOVERLAY_CARD track;
        memset(&track, 0, sizeof(track));
        track.Radius = 4.0f;
        track.FillTop = 0x50000000u;
        track.FillBottom = 0x30FFFFFFu;
        UiOverlay_Card(barX0, (float)y - 4.0f, barX1, (float)y + 4.0f, &track);
        T_UIOVERLAY_CARD level;
        memset(&level, 0, sizeof(level));
        level.Radius = 4.0f;
        level.FillTop = 0xFFA6E2FFu;
        level.FillBottom = 0xFF4A9EF0u;
        if (focus > 0.01f) {
            level.Glow = ((U32)(focus * 120.0f) << 24) | 0x5AB4FFu;
            level.GlowSize = 6.0f;
        }
        const float fillX1 = barX0 + (barX1 - barX0) * SDL_clamp(slider, 0.0f, 1.0f);
        if (fillX1 > barX0 + 8.0f) {
            UiOverlay_Card(barX0, (float)y - 4.0f, fillX1, (float)y + 4.0f, &level);
        }
    } else {
        UiOverlay_Text(cx, (float)y, string, textSize, textColor, UIOVERLAY_ALIGN_CENTER, TRUE, (x1 - x0) - 110.0f);
        if (focus > 0.01f) {
            const U32 alpha = (U32)(focus * 255.0f);
            const float size = 17.0f + 4.0f * pulse;
            UiOverlay_Sparkle(x0 + 30.0f, (float)y, size, (alpha << 24) | 0xCFEAFFu);
            UiOverlay_Sparkle(x1 - 30.0f, (float)y, size, (alpha << 24) | 0xCFEAFFu);
        }
    }
}

static void DrawGameMenuModern(U16 *ptrmenu) {
    const S32 selected = ptrmenu[0];
    const S32 nb = ptrmenu[1];
    S32 y = ptrmenu[2];

    if (y == 0) {
        y = HAUTEUR_STANDARD / 2 + 10 + UI_VCENTER_OFS;
    } else {
        y += UI_VCENTER_OFS;
        y -= (HAUTEUR_STANDARD * nb + (nb - 1) * MENU_SPACE) / 2;
    }

    /* Ease each row's highlight toward the cursor. */
    const U32 now = (U32)TimerRefHR;
    float dt = (float)(now - s_modernTime) / 1000.0f;
    if (s_modernMenu != ptrmenu || dt < 0.0f || dt > 0.25f) {
        dt = s_modernMenu != ptrmenu ? 1.0f : 0.016f;
        memset(s_modernFocus, 0, sizeof(s_modernFocus));
        s_modernMenu = ptrmenu;
    }
    s_modernTime = now;
    /* Fast enough to never trail the cursor: settled within about 60 ms. */
    const float step = SDL_min(dt * 40.0f, 1.0f);
    const float pulse = 0.5f + 0.5f * (float)sin((double)now / 1000.0 * 3.0);

    UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
    if (s_saveConfirm) {
        SavePage_ConfirmDecor();
    }
    for (S32 n = 0; n < nb && n < MODERN_MAX_ROWS; n++) {
        const S32 type = ptrmenu[4 + n * 2];
        const S32 num = ptrmenu[4 + n * 2 + 1];
        S32 sel = (n == selected) ? 1 : 0;
        if (n == s_menuMarkRow)
            sel |= 2;
        s_modernFocus[n] += ((sel & 1 ? 1.0f : 0.0f) - s_modernFocus[n]) * step;
        DrawModernRow(y, type, num, sel, s_modernFocus[n], pulse);
        y += HAUTEUR_STANDARD + MENU_SPACE;
    }
    /* A present only happens for a dirty frame; the cards animate every pass. */
    BoxStaticAdd(0, 0, 1, 1);
    BoxUpdate();
}

void DrawGameMenu(U16 *ptrmenu, S32 justone) {
    S32 n, y;
    S32 nb;
    S32 selected, type, num;
#ifdef DEMO
    U8 mainmenu = FALSE;

    if (ptrmenu == GameMainMenu)
        mainmenu = TRUE;
#endif

    if (Menu_IsModern()) {
        DrawGameMenuModern(ptrmenu);
        return;
    }

    selected = *ptrmenu++;

    nb = *ptrmenu++;

    y = *ptrmenu++;

    ptrmenu++; // dial num

    if (y == 0) // depuis le haut
    {
        // Top-anchored list (the ask-choice menu, GameChoiceMenu, the only
        // y==0 user). Float with the canvas so the choices track the rest of
        // the UI on a taller framebuffer instead of staying stranded at the top
        // edge while the question bubble (MyDial) bottom-anchors. No-op at 480.
        y = HAUTEUR_STANDARD / 2 + 10 + UI_VCENTER_OFS;
    } else // centre sur y
    {
        // The table y-centre is authored on the 480-tall canvas; float it to
        // the vertical middle of a taller framebuffer (no-op at 480).
        y += UI_VCENTER_OFS;
        y -= (HAUTEUR_STANDARD * nb + (nb - 1) * MENU_SPACE) / 2;
    }

    for (n = 0; n < nb; n++) {
        type = *ptrmenu++;
        num = *ptrmenu++;

#ifdef DEMO
        if (mainmenu AND num == 72 OR num == 73) // save or load
        {
            CoulTextMenu = COUL_TEXT_MENU_DISABLE;
        } else {
            CoulTextMenu = COUL_TEXT_MENU;
        }
#endif

        // Bit 0: cursor row. Bit 1: the green active-value marker row (only the
        // resolution sub-page sets s_menuMarkRow; -1 elsewhere so this is a no-op).
        S32 sel = (n == selected) ? 1 : 0;
        if (n == s_menuMarkRow)
            sel |= 2;

        if (justone) {
            // Per-frame redraw animates the plasma. Refresh the cursor row and,
            // when set, the green marker row, so the marker persists and animates
            // between full redraws instead of being drawn once and cleared.
            if (n == selected || n == s_menuMarkRow) {
                DrawOneChoice(UI_CENTER_X, y, type, num, sel);
            }
        } else {
            DrawOneChoice(UI_CENTER_X, y, type, num, sel);
        }

        y += HAUTEUR_STANDARD + MENU_SPACE;
    }

#ifdef DEMO
    if (mainmenu)
        CoulTextMenu = COUL_TEXT_MENU;
#endif

    BoxUpdate();
}

/*──────────────────────────────────────────────────────────────────────────*/

#define VOLUME_TIMER_KEY 5

static void GameMenuApplyMouseWheelDoGameMenu(U16 *ptrmenu, S16 selected, S32 wy, S32 *timerkey, S32 *flag) {
    S32 t;
    S32 n;
    S32 dir;
    S32 i;
    S32 applied;

    if (wy == 0 || !FlagMenuMouse || MenuMouse_GetRefCount() <= 0)
        return;

    t = ptrmenu[4 + 0 + selected * 2];
    dir = (wy > 0) ? 1 : -1;
    n = (wy > 0) ? wy : -wy;
    if (n > 16)
        n = 16;

    for (i = 0; i < n; i++) {
        if (TimerRefHR <= *timerkey)
            break;
        *timerkey = TimerRefHR + VOLUME_TIMER_KEY;
        applied = 0;

        switch (t) {
        case 2:
            if (dir < 0 AND SampleVolume > 0) {
                SampleVolume -= 1;
                applied = 1;
            } else if (dir > 0 AND SampleVolume < 127) {
                SampleVolume += 1;
                applied = 1;
            }
            break;

        case 3:
            if (dir < 0 AND VoiceVolume > 0) {
                VoiceVolume -= 1;
                applied = 1;
            } else if (dir > 0 AND VoiceVolume < 127) {
                VoiceVolume += 1;
                applied = 1;
            }
            break;

        case 4:
            if (dir < 0 AND JingleVolume > 0) {
                JingleVolume -= 1;
                SetVolumeJingle(JingleVolume);
                applied = 1;
            } else if (dir > 0 AND JingleVolume < 127) {
                JingleVolume += 1;
                SetVolumeJingle(JingleVolume);
                applied = 1;
            }
            break;

        case 5:
#ifdef CDROM
            if (dir < 0 AND CDVolume > 0) {
                CDVolume -= 1;
                SetVolumeCD(CDVolume);
                SetVolumeJingle(CDVolume);
                applied = 1;
            } else if (dir > 0 AND CDVolume < 127) {
                CDVolume += 1;
                SetVolumeCD(CDVolume);
                SetVolumeJingle(CDVolume);
                applied = 1;
            }
#endif
            break;

        case 6:
            if (dir < 0 AND MasterVolume > 0) {
                MasterVolume -= 1;
                SetMasterVolumeSample(MasterVolume);
                SetVolumeJingle(JingleVolume);
#ifdef CDROM
                SetVolumeCD(CDVolume);
#endif
                applied = 1;
            } else if (dir > 0 AND MasterVolume < 127) {
                MasterVolume += 1;
                SetMasterVolumeSample(MasterVolume);
                SetVolumeJingle(JingleVolume);
#ifdef CDROM
                SetVolumeCD(CDVolume);
#endif
                applied = 1;
            }
            break;

        case 7:
            if (dir < 0 AND DetailLevel > 0) {
                DetailLevel--;
                InitWaitNoInput(I_LEFT);
                InitWaitNoKey();
                applied = 1;
            } else if (dir > 0 AND DetailLevel < MAX_DETAIL_LEVEL) {
                DetailLevel++;
                InitWaitNoInput(I_RIGHT);
                InitWaitNoKey();
                applied = 1;
            }
            break;

        case MENU_TYPE_DISPLAY_SELECTOR:
            Display_CycleOption(ptrmenu[4 + 1 + selected * 2], dir);
            *flag = 1;
            InitWaitNoInput(dir < 0 ? I_LEFT : I_RIGHT);
            InitWaitNoKey();
            applied = 1;
            break;

        case MENU_TYPE_TEXT_LANGUAGE:
            if (dir < 0)
                CycleTextLanguage(-1);
            else
                CycleTextLanguage(1);
            *flag = 1;
            InitWaitNoInput(dir < 0 ? I_LEFT : I_RIGHT);
            InitWaitNoKey();
            applied = 1;
            break;

#ifdef CDROM
        case MENU_TYPE_VOICE_LANGUAGE:
            if (dir < 0)
                CycleVoiceLanguage(-1);
            else
                CycleVoiceLanguage(1);
            *flag = 1;
            InitWaitNoInput(dir < 0 ? I_LEFT : I_RIGHT);
            InitWaitNoKey();
            applied = 1;
            break;
#endif

        default:
            break;
        }

        if (!applied)
            break;
    }
}

/* Forward decls -- defined further down in this file. */
void BuildGameMainMenu(S32 firstloop);
static void DrawScreenSaveNoFlip(U8 *ptr, S32 x, S32 y);

/* Harness one-shot capture of the boot-time main game menu -- the screen a
   player sees at launch with Resume / New Game / Load / Options / Quit.
   The normal entry point MainGameMenu() runs a while-loop with case-driven
   side effects (Credits, PlayAcf, save-game zoom animations) that we don't
   want under the harness; instead, replicate just the minimal setup
   MainGameMenu does before its DoGameMenu(GameMainMenu) call, arm the
   capture, and call DoGameMenu directly.  The capture site inside
   DoGameMenu (added for ui menu-options) fires and returns 1000, so this
   function returns cleanly without entering MainGameMenu's outer loop. */
void Menu_MainCapture(const char *path) {
    char tmpFilePathScreen[ADELINE_MAX_PATH];

    /* Setup mirrors MainGameMenu() lines 3373-3416 minus the audio start
       (CD music) and minus the goto-load_game / firstloop logic.  All the
       visual setup (background bitmap, plasma, palette, menu data, dial bank)
       runs so the captured frame matches what a player sees at launch. */
    HQ_StopSample();
    StopMusic();
    InitPlasmaMenu();
    FlagShadeMenu = FALSE;

    GetResPath(tmpFilePathScreen, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
    Image_LoadStretched(tmpFilePathScreen, Screen, PCR_MENU);
    CopyScreen(Screen, Log);
    BoxStaticFullflip();
    PtrPal = PtrPalNormal;
    PaletteSync(PtrPalNormal);

    BuildGameMainMenu(TRUE);
    InitDial(0);

    Menu_RequestCapture(path);
    DoGameMenu(GameMainMenu);
    Menu_RequestCapture(NULL); /* disarm if DoGameMenu somehow exited without firing it */
}

/* Harness capture state — see Menu_RequestCapture() in GAMEMENU.H.  When the
   path is non-empty, DoGameMenu's modal loop counts down per iteration;
   on reaching zero it SavePNGs and returns 1000 (the "ESC" sentinel) so the
   wrapping menu loop (OptionsMenu, MainGameMenu, ...) sees a clean exit.
   The countdown gives the menu's fade-in and any per-frame text/sprite
   animations time to settle into a steady visual state. */
static char s_menu_capture_path[ADELINE_MAX_PATH] = "";
static int s_menu_capture_countdown = 0;

void Menu_RequestCapture(const char *path) {
    if (!path || !path[0]) {
        s_menu_capture_path[0] = '\0';
        s_menu_capture_countdown = 0;
        return;
    }
    strncpy(s_menu_capture_path, path, ADELINE_MAX_PATH - 1);
    s_menu_capture_path[ADELINE_MAX_PATH - 1] = '\0';
    s_menu_capture_countdown = 60; /* iterations -- ~1 s at fixed-dt 16, past any
                                      menu fade-in and dynamic-item initial draws */
}

static S32 DoGameMenuLoop(U16 *ptrmenu);

S32 DoGameMenu(U16 *ptrmenu) {
    const S32 ret = DoGameMenuLoop(ptrmenu);
    /* The page is gone: its cards must not outlive it. */
    UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
    return ret;
}

static S32 DoGameMenuLoop(U16 *ptrmenu) {
    S32 flag = 1;
    U32 chrono;
    S32 timerkey;
    // Gamepad D-pad / stick fallback: edge-only, matching the keyboard-arrow
    // feel (one step per press, no auto-repeat). Primed below so a stick
    // already deflected on menu entry does not auto-fire.
    S32 padNavDir = 0;
    S16 nb, selected;

    selected = ptrmenu[0];
    nb = ptrmenu[1];

    ManageTime();
    chrono = TimerRefHR;

    InitWaitNoKey();
    InitWaitNoInput(I_JOY | I_FIRE | I_MENUS | I_ACTION);

    MenuIncrustText = NULL;
    MenuIncrustSprite = -1;

    timerkey = TimerRefHR + VOLUME_TIMER_KEY;
    padNavDir = JoystickMenuNavOnly();

    InitShakeChoice(); //     NEW

    AsciiMode = TRUE;

    while (TRUE) {
        ManageTime();
        MyGetInput();

        // Gamepad D-pad / left stick feed the same codes as keyboard arrows.
        // Edge-only: fire one K_GRAY_* on the direction change so a held
        // stick/dpad behaves like a held keyboard arrow (one step, stay).
        if (!MyKey) {
            S32 rawDir = JoystickMenuNavOnly();
            if (rawDir != padNavDir) {
                padNavDir = rawDir;
                if (rawDir == K_UP)
                    MyKey = K_GRAY_UP;
                else if (rawDir == K_DOWN)
                    MyKey = K_GRAY_DOWN;
                else if (rawDir == K_LEFT)
                    MyKey = K_GRAY_LEFT;
                else if (rawDir == K_RIGHT)
                    MyKey = K_GRAY_RIGHT;
            }
        }

        if (FlagMenuMouse && MenuMouse_GetRefCount() > 0) {
            S32 mret = GameMenuMouseProcessDoGameMenu(ptrmenu, &selected, &flag);

            if (mret >= 0) {
                WaitNoInput();
                AsciiMode = FALSE;
                return mret;
            }
        }

#ifdef DEBUG_TOOLS
        CheckSaveLogPcx(PtrPalNormal);
#endif

        // rustine Démo non jouable ...
        if (ptrmenu == GameMainMenu) {
            if ((TimerRefHR - chrono > 50 * 20 * 60)
                    OR(CheckKey(K_D) AND(CheckKey(K_SHIFT) OR CheckKey(K_SHIFT_LEFT) OR CheckKey(K_SHIFT_RIGHT)))) {
#ifdef DEMO
                //                                if( SlideShow() )
                //					DemoBumper() ;
                SlideShow();

                char tmpFilePathScreen[ADELINE_MAX_PATH];
                GetResPath(tmpFilePathScreen, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
                Image_LoadStretched(tmpFilePathScreen, Log, PCR_MENU);
                CopyScreen(Log, Screen);
                BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

                FlagShowEndDemo = TRUE;
#else
                BeginDemoSlide();

                InitGame(1);
                NewCube = 193;

                FlagFade = TRUE;

                if (MainLoop())
                    if (SlideShow())
                        Credits(FALSE);

                UnsetClipWindow();

                // MainLoop's render path overwrote Log with gameplay frames
                // during the attract reel; restore the menu plate before
                // CopyScreen / BoxReset / FadeToBlack below so the menu UI
                // loop has the right background. Same image as the menu's
                // own boot a few hundred lines down (BuildGameMainMenu /
                // Image_LoadStretched into Screen + CopyScreen to Log).
                // Affects every exit from MainLoop on the cold-start attract
                // path: stuck-cube watchdog timeout (#176), mid-reel ESC,
                // LM_THE_END natural reel end.
                {
                    char tmpFilePathScreen[ADELINE_MAX_PATH];
                    GetResPath(tmpFilePathScreen, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
                    Image_LoadStretched(tmpFilePathScreen, Log, PCR_MENU);
                }

                CopyScreen(Log, Screen);
                BoxReset();
#endif
                PtrPal = PtrPalNormal;
                if (!FlagBlackPal)
                    FadeToBlack(PtrPal);

                FadeMenu = TRUE;
                AsciiMode = FALSE;
                return 9999;
            }
        }

        if (Input & I_DOWN OR MyKey == K_GRAY_DOWN) {
#ifdef DEMO
            if (ptrmenu == GameMainMenu) {
                do {
                    selected++;
                } while (selected < nb
                                        AND(ptrmenu[4 + 1 + selected * 2] == 72 OR ptrmenu[4 + 1 + selected * 2] == 73));
            } else
#endif
            {
                selected++;
            }

            if (selected >= nb)
                selected = 0;
            flag = 1;
        }

        if (Input & I_UP OR MyKey == K_GRAY_UP) {
#ifdef DEMO
            if (ptrmenu == GameMainMenu) {
                do {
                    selected--;
                } while (selected >= 0 AND(ptrmenu[4 + 1 + selected * 2] == 72 OR ptrmenu[4 + 1 + selected * 2] == 73));
            } else
#endif
            {
                selected--;
            }

            if (selected < 0)
                selected = (U16)(nb - 1);
            flag = 1;
        }

        switch (ptrmenu[4 + 0 + selected * 2]) // type
        {
        case 0: // text normal
            break;

        case 2: // volume sample

            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND TimerRefHR > timerkey AND SampleVolume > 0) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                SampleVolume -= 1;
            }
            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND TimerRefHR > timerkey AND SampleVolume < 127) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                SampleVolume += 1;
            }
            break;

        case 3: // volume voix (jouer 1 voix ?)

            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND TimerRefHR > timerkey AND VoiceVolume > 0) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                VoiceVolume -= 1;
            }
            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND TimerRefHR > timerkey AND VoiceVolume < 127) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                VoiceVolume += 1;
            }
            break;

        case 4: // volume musiques

            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND TimerRefHR > timerkey AND JingleVolume > 0) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                JingleVolume -= 1;
                SetVolumeJingle(JingleVolume);
            }
            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND TimerRefHR > timerkey AND JingleVolume < 127) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                JingleVolume += 1;
                SetVolumeJingle(JingleVolume);
            }
            break;

        case 5: // volume cd
#ifdef CDROM
            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND TimerRefHR > timerkey AND CDVolume > 0) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                CDVolume -= 1;
                SetVolumeCD(CDVolume);
                SetVolumeJingle(CDVolume);
            }
            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND TimerRefHR > timerkey AND CDVolume < 127) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                CDVolume += 1;
                SetVolumeCD(CDVolume);
                SetVolumeJingle(CDVolume);
            }
#endif
            break;

        case 6: // volume master

            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND TimerRefHR > timerkey AND MasterVolume > 0) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                MasterVolume -= 1;
                SetMasterVolumeSample(MasterVolume);
                SetVolumeJingle(JingleVolume);
#ifdef CDROM
                SetVolumeCD(CDVolume);
#endif
            }
            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND TimerRefHR > timerkey AND MasterVolume < 127) {
                timerkey = TimerRefHR + VOLUME_TIMER_KEY;
                MasterVolume += 1;
                SetMasterVolumeSample(MasterVolume);
                SetVolumeJingle(JingleVolume);
#ifdef CDROM
                SetVolumeCD(CDVolume);
#endif
            }
            break;

        case 7: // Niveau détails

            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)
                    AND(DetailLevel > 0)) {
                DetailLevel--;
                InitWaitNoInput(I_LEFT);
                InitWaitNoKey();
            }

            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)
                    AND(DetailLevel < MAX_DETAIL_LEVEL)) {
                DetailLevel++;
                InitWaitNoInput(I_RIGHT);
                InitWaitNoKey();
            }
            break;

        case MENU_TYPE_DISPLAY_SELECTOR:
            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)) {
                Display_CycleOption(ptrmenu[4 + 1 + selected * 2], -1);
                flag = 1;
                InitWaitNoInput(I_LEFT);
                InitWaitNoKey();
            }

            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)) {
                Display_CycleOption(ptrmenu[4 + 1 + selected * 2], 1);
                flag = 1;
                InitWaitNoInput(I_RIGHT);
                InitWaitNoKey();
            }
            break;

        case MENU_TYPE_TEXT_LANGUAGE:
            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)) {
                CycleTextLanguage(-1);
                flag = 1;
                InitWaitNoInput(I_LEFT);
                InitWaitNoKey();
            }

            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)) {
                CycleTextLanguage(1);
                flag = 1;
                InitWaitNoInput(I_RIGHT);
                InitWaitNoKey();
            }
            break;

#ifdef CDROM
        case MENU_TYPE_VOICE_LANGUAGE:
            if ((Input & I_LEFT OR MyKey == K_GRAY_LEFT)) {
                CycleVoiceLanguage(-1);
                flag = 1;
                InitWaitNoInput(I_LEFT);
                InitWaitNoKey();
            }

            if ((Input & I_RIGHT OR MyKey == K_GRAY_RIGHT)) {
                CycleVoiceLanguage(1);
                flag = 1;
                InitWaitNoInput(I_RIGHT);
                InitWaitNoKey();
            }
            break;
#endif
        }

        if (FlagMenuMouse && MenuMouse_GetRefCount() > 0) {
            S32 wy = ConsumeMouseWheelY();

            if (wy != 0)
                GameMenuApplyMouseWheelDoGameMenu(ptrmenu, selected, wy, &timerkey, &flag);
        }

        if (s_menuRebuild) {
            /* A selector changed which rows this page has. */
            s_menuRebuild = FALSE;
            ptrmenu[0] = selected;
            AsciiMode = FALSE;
            return MENU_ID_MENU_REBUILD;
        }

        if (flag == 1) {
            InitShakeChoice(); //     NEW
            TimerSample = 0;
            ptrmenu[0] = selected;

            // Preview box first, without flipping: DrawGameMenu below flips
            // once the rows are down, so the page arrives whole. Drawn after
            // the rows it landed a frame later, and the menu showed up without
            // its preview for that frame.
            if (ptrmenu == GameMainMenu) {
                U8 memonumversion = NumVersion;
                char memoplayername[256];
                char memogamepathname[256];
                U8 *ptr;

                strcpy(memoplayername, PlayerName);
                strcpy(memogamepathname, GamePathname);

                GetSavePath(GamePathname, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);

                ptr = LoadGameScreen();
                if (ptr)
                    // Main-menu current-save preview box. Float it with the menu
                    // list (both use UI_VCENTER_OFS) so the box-to-menu gap stays
                    // as authored on a taller framebuffer. No-op at 480.
                    DrawScreenSaveNoFlip(ptr, SAVE_THUMB_X, 10 + UI_VCENTER_OFS);

                strcpy(GamePathname, memogamepathname);
                strcpy(PlayerName, memoplayername);
                NumVersion = memonumversion;
                NewCube = -1;
            }

            DrawGameMenu(ptrmenu, FALSE);

            if (FadeMenu) {
                FadeToPal(PtrPalNormal);
                FadeMenu = FALSE;
            }

            do {
                DrawGameMenu(ptrmenu, TRUE);
                MyGetInput();
            } while (MyKey OR Input);

            flag = 0;
        } else {
            DrawGameMenu(ptrmenu, TRUE);
            flag = 0;
        }

        if (MenuIncrustText) {
            if (TimerRefHR > MenuIncrustTimer) {
                CopyBlock(0, 0, 150, 19, Screen, 0, 0, Log);
                BoxStaticAdd(0, 0, 150, 19);
                if (FlagShadeMenu)
                    ShadeBoxBlk(0, 0, 150, 19, SCREEN_SHADE_LVL);
                MenuIncrustText = NULL;
                MenuIncrustSprite = -1;
            } else {
                S32 x;

                if ((TimerRefHR & 0xFF) >= 85) // aff 2/3 eff 1/3
                {
                    if (MenuIncrustSprite != -1) {
                        if (MenuIncrustSprite >= 100) {
                            SpriteX = 10 + PtrZvExtra[MenuIncrustSprite * 8 + 0];
                            SpriteY = 10 + PtrZvExtra[MenuIncrustSprite * 8 + 1];
                        } else {
                            SpriteX = 10 + PtrZvExtraRaw[MenuIncrustSprite * 8 + 0];
                            SpriteY = 10 + PtrZvExtraRaw[MenuIncrustSprite * 8 + 1];
                        }

                        ScaleFactorSprite = DEF_SCALE_FACTOR;
                        PtrAffGraph(SpriteX, SpriteY, MenuIncrustSprite);
                        BoxStaticAdd(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax);
                        x = ScreenXMax + 5;
                    } else
                        x = 5;

                    CoulText(LBAWHITE, -1);
                    GraphPrintf(FALSE, x, 0, MenuIncrustText);
                    BoxStaticAdd(TextXMin, TextYMin, TextXMax, TextYMax);
                } else {
                    CopyBlock(0, 0, 150, 19, Screen, 0, 0, Log);
                    BoxStaticAdd(0, 0, 150, 19);
                    if (FlagShadeMenu)
                        ShadeBoxBlk(0, 0, 150, 19, SCREEN_SHADE_LVL);
                }
            }
        }

        if (Input & I_RETURN OR Input & I_ACTION_M OR MyKey == K_ENTER OR MyKey == K_NUMPAD_ENTER) {
            WaitNoInput();
            AsciiMode = FALSE;
            return ptrmenu[4 + 1 + selected * 2]; // num mess
        }

        if (MyKey == K_ESC
                         OR Input &
            I_MENUS) {
            AsciiMode = FALSE;
            return 1000;
        }

        /* Harness one-shot capture: count down per iteration; on reaching
           zero SavePNG the rendered menu and return the ESC sentinel so
           the wrapping menu loop exits cleanly.  Read Log (back buffer),
           not Screen -- DrawGameMenu composites the menu into Log, and the
           BoxUpdate inside only flushes dirty regions to Screen; without a
           prior frame populating Screen, the unflushed regions stay black.
           The third surface to need this (after holomap and dialog). */
        if (s_menu_capture_path[0] && --s_menu_capture_countdown <= 0) {
            SavePNG(s_menu_capture_path, Log, (S32)ModeDesiredX,
                    (S32)ModeDesiredY, PtrPal);
            s_menu_capture_path[0] = '\0';
            AsciiMode = FALSE;
            return 1000;
        }

#ifdef DEBUG_TOOLS
        if (ptrmenu == GameMainMenu) {
            // chargement d'un bug
            if (MyKey == K_L) {
                AsciiMode = FALSE;
                return 2000;
            }

            // sauvegarde d'un bug
            if (MyKey == K_G) {
                AsciiMode = FALSE;
                return 2001;
            }
        }
#endif

        if (!DemoSlide AND MyKey)
            GereCheatCode();
    }
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 GereVolumeMenu(void) {
    S32 select;
    S32 flag = 0;

    S32 wasPaused = (GetSamplesPausedCount() > 0);
    if (wasPaused)
        ResumeSamples();

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

#ifdef CDROM
    PlayMusic(CD_TRACK_MENU, TRUE);
#endif

#ifndef DEMO
    if (FlagSpeak) {
        VolumeMenu = VolumeMenuVoice; // integre volume des voix
    } else {
        VolumeMenu = VolumeMenuNoVoice; // pas de volume des voix
    }
#endif

    while (!flag) {
        select = DoGameMenu(VolumeMenu);

        switch (select) // num mess
        {
        case 26:   // menu precedent
        case 1000: // <ESC>
            flag = 1;
            break;
        }
    }

    SetVolumeJingle(JingleVolume);

    if (wasPaused)
        PauseSamples();

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 GereLanguageMenu(void) {
    S32 select;
    S32 flag = 0;

#ifdef CDROM
    ClampVoiceLanguage();
#endif

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

    while (!flag) {
        select = DoGameMenu(LanguageMenu);

        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case MENU_ID_TEXT_LANGUAGE:
            CycleTextLanguage(1);
            break;

#ifdef CDROM
        case MENU_ID_VOICE_LANGUAGE:
            CycleVoiceLanguage(1);
            break;
#endif
        }
    }

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

static void BuildResolutionMenu(void); /* defined below, near Resolution_MainCapture */
static void Display_BuildMenu(void);   /* defined below, near GereDisplayMenu */
static void Display_ApplyRenderSize(void); /* defined below, near GereDisplayMenu */

// Harness one-shot capture of the Display submenu. Mirrors
// Menu_MainCapture's pattern: do the minimal setup the regular
// OptionsMenu → GereDisplayMenu path does (plasma init, palette, dial
// bank), arm the shared Menu_RequestCapture countdown, drive DoGameMenu
// directly on DisplayMenu so the capture site inside DoGameMenu fires
// and returns 1000. Background pixels come from whatever the caller
// (cmd_ui) put in Log/Screen — for `ui --black-bg display` that's a
// solid clear; otherwise the live scene.
void Display_MainCapture(const char *path) {
    HQ_StopSample();
    InitPlasmaMenu();

    CopyScreen(Screen, Log);
    BoxStaticFullflip();
    PtrPal = PtrPalNormal;
    PaletteSync(PtrPalNormal);
    InitDial(0);

    Display_RebuildResolutionList();
    Display_BuildMenu();
    Menu_RequestCapture(path);
    DoGameMenu(DisplayMenu);
    Menu_RequestCapture(NULL); /* disarm if DoGameMenu somehow exited otherwise */
}

// Harness one-shot capture of the GPU effects submenu, same shape as
// Display_MainCapture.
static void MenuPage_MainCapture(U16 *menu, const char *path);

void GpuEffects_MainCapture(const char *path) {
    MenuPage_MainCapture(GpuEffectsMenu, path);
}

void GpuLighting_MainCapture(const char *path) {
    MenuPage_MainCapture(GpuLightingMenu, path);
}

void Advanced_MainCapture(const char *path) {
    MenuPage_MainCapture(AdvancedOptionMenu, path);
}

void Volume_MainCapture(const char *path) {
#ifdef DEMO
    MenuPage_MainCapture(VolumeMenu, path);
#else
    MenuPage_MainCapture(FlagSpeak ? VolumeMenuVoice : VolumeMenuNoVoice, path);
#endif
}

static void MenuPage_MainCapture(U16 *menu, const char *path) {
    HQ_StopSample();
    InitPlasmaMenu();

    CopyScreen(Screen, Log);
    BoxStaticFullflip();
    PtrPal = PtrPalNormal;
    PaletteSync(PtrPalNormal);
    InitDial(0);

    Menu_RequestCapture(path);
    DoGameMenu(menu);
    Menu_RequestCapture(NULL); /* disarm if DoGameMenu somehow exited otherwise */
}

// Harness one-shot capture of the resolution sub-page. Same shape as
// Display_MainCapture but builds the runtime ResolutionMenu first (the page is
// generated, not a static array), then drives DoGameMenu(ResolutionMenu) with
// the capture armed. BuildResolutionMenu also sets s_resMenuCount so the
// range-id rows resolve in BuildCustomMenuText during the capture draw.
void Resolution_MainCapture(const char *path) {
    HQ_StopSample();
    InitPlasmaMenu();

    CopyScreen(Screen, Log);
    BoxStaticFullflip();
    PtrPal = PtrPalNormal;
    PaletteSync(PtrPalNormal);
    InitDial(0);

    BuildResolutionMenu();
    /* Park the cursor on Back so the green "current" marker on the active
       resolution row is visible in the capture (the live menu opens with the
       cursor on the current row, where the blue cursor bar takes precedence). */
    ResolutionMenu[0] = 0;

    Menu_RequestCapture(path);
    DoGameMenu(ResolutionMenu);
    Menu_RequestCapture(NULL); /* disarm if DoGameMenu somehow exited otherwise */
    /* Clear the sub-page-local statics so neither the green marker nor the
       BuildCustomMenuText range guard can affect any other menu. */
    s_menuMarkRow = -1;
    s_resMenuCount = 0;
}

// Write entry `row` (0-based) of a runtime-built menu array: the (type, id)
// pair after the 4-word header. Centralises the `4 + row*2` index math the
// menu format uses, shared by the dynamic builders (BuildResolutionMenu,
// GameAskChoice).
static void MenuSetEntry(U16 *menu, S32 row, U16 type, U16 id) {
    menu[4 + row * 2 + 0] = type;
    menu[4 + row * 2 + 1] = id;
}

// Rebuild the runtime ResolutionMenu array from the shared catalog: a "Back"
// row plus one plain-button row per resolution (id = MENU_ID_RESOLUTION_BASE +
// index), cursor landing on the current resolution if present. Refreshes
// s_resMenuEntries / s_resMenuCount, which back the range-id rows in
// BuildCustomMenuText. Used by Resolution_MainCapture.
static void BuildResolutionMenu(void) {
    s_resMenuCount = Display_BuildEntries(s_resMenuEntries);

    // Header: selected (set below), entry count (Back + rows), y center, dia.
    ResolutionMenu[1] = (U16)(1 + s_resMenuCount);
    ResolutionMenu[2] = 260;
    ResolutionMenu[3] = 0;
    // Back row, then one plain-button row per catalog entry (id = base + i).
    MenuSetEntry(ResolutionMenu, 0, 0, 26);
    for (S32 i = 0; i < s_resMenuCount; i++)
        MenuSetEntry(ResolutionMenu, 1 + i, 0, (U16)(MENU_ID_RESOLUTION_BASE + i));
    // Land the cursor on the current resolution if it's in the list, else Back;
    // mark that same row green so it stays flagged as active once the cursor
    // moves off it (cursor blue wins while they coincide). -1 = none.
    S32 cur = Display_FindCurrentEntry(s_resMenuEntries, s_resMenuCount);
    ResolutionMenu[0] = (cur >= 0) ? (U16)(1 + cur) : 0;
    s_menuMarkRow = (cur >= 0) ? (1 + cur) : -1;
}

// Resolution sub-page, opened from the Display submenu's Resolution row.
// Builds a select-and-confirm list at runtime from the shared catalog (the
// GameAskChoice pattern): a "Back" row plus one row per resolution, HD
// included. The recommended tier fits the on-screen budget without scrolling.
// Picking a row applies it immediately (ApplyResolutionChoice). Mirrors
// GereDisplayMenu's backdrop setup so the list draws over the shaded scene
// (in-game) or the boot-menu image.
// Lighting and shadows page: selector rows for the GPU renderer's lighting.
static void GereGpuLightingMenu(void) {
    S32 flag = 0;
    while (!flag) {
        const S32 select = DoGameMenu(GpuLightingMenu);
        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case MENU_ID_GPU_SUNLIGHT:
        case MENU_ID_GPU_RAYTRACE:
        case MENU_ID_GPU_SHADOWS:
        case MENU_ID_GPU_LIGHTS:
        case MENU_ID_GPU_LAMPS:
        case MENU_ID_GPU_SPECULAR:
        case MENU_ID_GPU_SKY:
        case MENU_ID_GPU_STORM:
            Display_CycleOption(select, 1);
            break;
        }
    }
    WaitNoInput();
}

// GPU effects submenu: selector rows for each effect of the GPU renderer.
static S32 GereGpuEffectsMenu(void) {
    S32 flag = 0;

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

    while (!flag) {
        const S32 select = DoGameMenu(GpuEffectsMenu);

        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case MENU_ID_GPU_LIGHTING:
            GereGpuLightingMenu();
            break;

        case MENU_ID_GPU_UI_SCALE:
        case MENU_ID_GPU_FIRE:
        case MENU_ID_GPU_STORM:
        case MENU_ID_GPU_SKY:
        case MENU_ID_GPU_SMOOTH_TERRAIN:
        case MENU_ID_GPU_VIEW_DISTANCE:
        case MENU_ID_GPU_WATER:
        case MENU_ID_GPU_PIXEL_FILTER:
        case MENU_ID_GPU_DEBAND:
            Display_CycleOption(select, 1);
            break;
        }
    }

    Display_ApplyRenderSize();

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

// The renderer or its 2D scale changed: the frame follows (Res_FrameDivisor).
static void Display_ApplyRenderSize(void) {
    const U32 w = ModeDesiredX;
    const U32 h = ModeDesiredY;
    Res_ApplySceneRenderSize();
    if (ModeDesiredX != w || ModeDesiredY != h) {
        RepaintMenuAfterResize();
    }
}

// Rows of the Display page for the current state: GPU effects and Auto-adjust
// only while the GPU renderer is on, Display mode only where windowed mode exists (not on
// Android phones/TVs). The cursor stays on the same row id when it moves.
static void Display_BuildMenu(void) {
    const S32 oldRow = DisplayMenu[1] > 0 ? DisplayMenu[0] : 0;
    const U16 oldId = DisplayMenu[1] > 0 ? DisplayMenu[4 + 1 + oldRow * 2] : 26;
    S32 n = 0;

    MenuSetEntry(DisplayMenu, n++, 0, 26);
    MenuSetEntry(DisplayMenu, n++, MENU_TYPE_DISPLAY_SELECTOR, MENU_ID_RESOLUTION);
    MenuSetEntry(DisplayMenu, n++, MENU_TYPE_DISPLAY_SELECTOR, MENU_ID_DISPLAY_VSYNC);
    MenuSetEntry(DisplayMenu, n++, MENU_TYPE_DISPLAY_SELECTOR, MENU_ID_DISPLAY_GPU);
    if (GpuObjEnabled) {
        MenuSetEntry(DisplayMenu, n++, 0, MENU_ID_GPU_EFFECTS);
        MenuSetEntry(DisplayMenu, n++, 0, MENU_ID_GPU_AUTOTUNE);
    }
    MenuSetEntry(DisplayMenu, n++, MENU_TYPE_DISPLAY_SELECTOR, MENU_ID_DISPLAY_MENU_STYLE);
    if (Window_SupportsWindowedMode()) {
        MenuSetEntry(DisplayMenu, n++, MENU_TYPE_DISPLAY_SELECTOR, MENU_ID_DISPLAY_FULLSCREEN);
    }
    DisplayMenu[0] = 0;
    DisplayMenu[1] = (U16)n;
    DisplayMenu[2] = 260;
    DisplayMenu[3] = 0;
    for (S32 i = 0; i < n; i++) {
        if (DisplayMenu[4 + 1 + i * 2] == oldId) {
            DisplayMenu[0] = (U16)i;
        }
    }
}

// Display submenu: a self-contained submenu of OptionsMenu, one page of
// selector rows (see DisplayMenu and Display_CycleOption).
S32 GereDisplayMenu(void) {
    S32 select;
    S32 flag = 0;

    Display_BuildMenu();
    Display_RebuildResolutionList();

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

    while (!flag) {
        select = DoGameMenu(DisplayMenu);

        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case MENU_ID_RESOLUTION:
            // Enter applies the resolution the selector shows.
            if (s_displayResPending >= 0 && s_displayResPending < s_displayResCount) {
                const DisplayEntry e = s_displayResEntries[s_displayResPending];
                ApplyResolutionChoice(e.w, e.h);
            }
            Display_RebuildResolutionList();
            break;

        case MENU_ID_DISPLAY_FULLSCREEN:
        case MENU_ID_DISPLAY_VSYNC:
        case MENU_ID_DISPLAY_GPU:
        case MENU_ID_DISPLAY_MENU_STYLE:
            Display_CycleOption(select, 1);
            break;

        case MENU_ID_GPU_EFFECTS:
            GereGpuEffectsMenu();
            /* Back on the Display page's own backdrop. */
            CopyScreen(Screen, Log);
            BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);
            if (FlagShadeMenu) {
                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
            }
            break;

        case MENU_ID_GPU_AUTOTUNE:
            /* Measured in play, outdoors, once the menus are closed. */
            GpuTune_Start();
            break;

        case MENU_ID_MENU_REBUILD:
            /* Rows changed: redraw the page from its backdrop. */
            Display_ApplyRenderSize();
            Display_BuildMenu();
            CopyScreen(Screen, Log);
            BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);
            if (FlagShadeMenu) {
                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
            }
            break;
        }
    }

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 GereAdvancedOptionsMenu(void) {
    S32 select;
    S32 flag = 0;

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

    while (!flag) {
        select = DoGameMenu(AdvancedOptionMenu);

        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case MENU_ID_STEREO_CHANNELS:
            Audio_SetReverseStereo(ReverseStereo ^ 1);
            break;

        case MENU_ID_MOVIE_CAMERA:
            AllCameras ^= 1;
            break;

        case MENU_ID_FOLLOW_CAMERA:
            FollowCamera ^= 1;
            break;

        case MENU_ID_VIDEO_SIZE:
            VideoFullScreen ^= 1;
            break;

        case MENU_ID_DISPLAY_TEXT:
            if (FlagSpeak) {
                FlagDisplayText ^= 1;
            } else {
                FlagDisplayText = TRUE;
            }
            break;
        }
    }

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 GereControllerMenu(void) {
    S32 select;
    S32 flag = 0;

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    }

    while (!flag) {
        select = DoGameMenu(ControllerMenu);

        switch (select) {
        case 26:
        case 1000:
            flag = 1;
            break;

        case 14: // Configuration Clavier (keyboard remap screen, source-guarded)
            MenuConfig();
            break;

        case MENU_ID_GAMEPAD_CONFIG:
            MenuGamepadConfig();
            break;
        }
    }

    CopyScreen(Screen, Log);
    BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

    WaitNoInput();

    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

S32 OptionsMenu(S32 flagflip) {
    S32 select;
    S32 flag = 0;

    /* Harness capture pre-pass.  The normal in-game ESC -> OptionsMenu path
       (PERSO.CPP:846) does explicit setup before OptionsMenu that the
       harness skips when --exec calls OptionsMenu directly:

           StopSpeak()       -- terminate any active dialogue speech so its
                                portrait / strip does not bleed through the
                                menu (visible on saves with an active dialog).
           InitDial(0)       -- switch the dialogue text bank to SYS (the menu
                                strings live there).  Without this the menu's
                                first item -- "Volume du son" -- reads junk or
                                empty from whatever bank the save had loaded.
           InitPlasmaMenu()  -- initialise the plasma/fire-bar texture in
                                SkySeaTexture; otherwise the bar at the top of
                                the menu renders whatever junk was in that
                                texture region.
           AffScene(NO_FLIP) -- render the live scene to Screen so the menu's
                                ShadeBoxBlk has the player-visible background
                                to darken.  Mirrors the inventory pre-pass.

       All gated on capture-armed; non-harness callers see no behaviour
       change. */
    if (s_menu_capture_path[0]) {
        StopSpeak();
        InitDial(0);
        InitPlasmaMenu();
        if (Console_GetCaptureBlackBg()) {
            /* Cleanroom golden — menu panel composites over solid black. */
            memset(Log, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);
            memset(Screen, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);
        } else {
            AffScene(AFF_ALL_NO_FLIP);
        }
    }

    BackupScreen(TRUE);

    /*        CopyScreen( Log, Screen ) ;

	BoxReset() ;
*/
    if (FlagShadeMenu) {
        ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
        BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
        PaletteSync(PtrPalNormal);
    }

    HQ_StopSample();
    RestartRainSample = TRUE;

#ifdef CDROM
    PlayMusic(CD_TRACK_MENU, TRUE);
#endif

    GameOptionMenu[1] = 6;

    MenuMouseAcquire();

    while (!flag) {
        select = DoGameMenu(GameOptionMenu);

        switch (select) // num mess
        {
        case 15:   // retour au jeu
        case 26:   // menu precedent
        case 1000: // <ESC>
            flag = 1;
            break;

        case MENU_ID_SOUND_VOLUME:
            GereVolumeMenu();
            break;

        case MENU_ID_CHOOSE_LANGUAGE:
            GereLanguageMenu();
            break;

        case MENU_ID_CONTROLLER:
            GereControllerMenu();
            break;

        case MENU_ID_ADVANCED_OPTIONS:
            GereAdvancedOptionsMenu();
            break;

        case MENU_ID_DISPLAY:
            GereDisplayMenu();
            break;
        }
    }

    HQ_StopSample();
    SetDetailLevel(); // prise en compte des différentes options

    if (flagflip) {
        // Backdrop only. The caller's menu loop draws over it and flips on its
        // next pass; flipping here shows the bare plate, and WaitNoInput below
        // holds it there for as long as the key is down.
        CopyScreen(Screen, Log);
        BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
    } else {
        // on vient du jeu
        RestoreClipWindow();
        RestoreScreen();
    }

    WaitNoInput();

    MenuMouseRelease();
    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
S32 DeleteSavedGame(void) {
    U8 *ptr;
    S32 ret = -1;
    S32 select;

    // Delete-save confirmation: float the name + preview with SavedConfirmMenu
    // (which floats via DrawGameMenu) so the whole screen stays coherent. No-op at 480.
    if (SavePage_Modern()) {
        /* The confirm menu's cards carry the name and picture (SavePage_ConfirmDecor). */
        SDL_strlcpy(s_saveConfirmName, PlayerName, sizeof(s_saveConfirmName));
        ptr = LoadGameScreen();
        s_savePreview = 0;
        if (ptr)
            SavePage_SetThumbnail(ptr, 130 + UI_VCENTER_OFS);
        s_saveConfirm = TRUE;
    } else {
        DrawOneString(UI_CENTER_X, 80 + UI_VCENTER_OFS, PlayerName, 2);
        ptr = LoadGameScreen();
        if (ptr)
            DrawScreenSave(ptr, SAVE_THUMB_X, 130 + UI_VCENTER_OFS);
        BoxUpdate();
    }

    SavedConfirmMenu[0] = 0;

    MenuMouseAcquire();

    while (ret == -1) {
        select = DoGameMenu(SavedConfirmMenu);

        switch (select) // num mess
        {
        case 24:   // annuler
        case 1000: // <ESC>
            ret = 0;
            break;

        case 48: // detruire
            // retourne 0 si Pb -> pas effacé
            //          1 si Ok
            ret = Delete(GamePathname);
            break;
        }
    }

    InitWaitNoKey();
    InitWaitNoInput(I_FIRE | I_ACTION | I_JOY | I_MENUS);

    MenuMouseRelease();
    s_saveConfirm = FALSE;
    s_savePreview = 0;
    return ret;
}

/*══════════════════════════════════════════════════════════════════════════*/

#define TIME_ZOOM_SAVE (30 * 20)

/*──────────────────────────────────────────────────────────────────────────*/
void ZoomSavedGame(S32 x, S32 y) {
    S32 x0 = x, y0 = y, x1 = x + SCREEN_SAVE_WIDTH - 1, y1 = y + SCREEN_SAVE_HEIGHT - 1;
    S32 time, savetimer;
    S32 flag = FALSE;

    ManageTime();
    savetimer = TimerRefHR;

    CopyScreen(ScreenAux, Log); // pour cleaner le Log

    while (!flag) {
        ManageTime();
        time = TimerRefHR - savetimer;

        x0 = RegleTrois(x, 0, TIME_ZOOM_SAVE, time);
        x1 = RegleTrois(x + SCREEN_SAVE_WIDTH - 1, (S32)ModeDesiredX - 1, TIME_ZOOM_SAVE, time);
        y0 = RegleTrois(y, 0, TIME_ZOOM_SAVE, time);
        y1 = RegleTrois(y + SCREEN_SAVE_HEIGHT - 1, (S32)ModeDesiredY - 1, TIME_ZOOM_SAVE, time);

        if (x0 <= 0 OR y0 <= 0 OR x1 >= (S32)ModeDesiredX - 1 OR y1 >= (S32)ModeDesiredY - 1) {
            flag = TRUE;
        } else {
            ScaleBox(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, Screen, x0, y0, x1, y1, Log);
            BoxStaticAdd(x0, y0, x1, y1);
            BoxUpdate();
        }
    }

    CopyScreen(Screen, Log);
    PaletteSync(PtrPal);
    BoxStaticFullflip();
}

/*──────────────────────────────────────────────────────────────────────────*/
// Preview box without the flip, for callers that draw it as part of a page.
static void DrawScreenSaveNoFlip(U8 *ptr, S32 x, S32 y) {
    S32 x1, y1;

    x1 = x + SCREEN_SAVE_WIDTH - 1;
    y1 = y + SCREEN_SAVE_HEIGHT - 1;

    BackupAngles(x - 1, y - 1, x1 + 1, y1 + 1);
    Box(x, y, x1, y1, 0);
    RestoreBlock(Log, ptr, x, y, x1, y1);
    DrawCadre(x - 1, y - 1, x1 + 1, y1 + 1, ALL_ANGLES);
    RestoreAngles(x - 1, y - 1, x1 + 1, y1 + 1);
    BoxStaticAdd(x - 1, y - 1, x1 + 1, y1 + 1);
}

/*──────────────────────────────────────────────────────────────────────────*/
void DrawScreenSave(U8 *ptr, S32 x, S32 y) {
    if (SavePage_Modern()) {
        SavePage_SetThumbnail(ptr, y);
        SavePage_Publish();
        BoxUpdate();
        return;
    }
    DrawScreenSaveNoFlip(ptr, x, y);
    BoxUpdate();
}

/*──────────────────────────────────────────────────────────────────────────*/
void DrawCadreSave(S32 x, S32 y) {
    S32 x1, y1;

    x1 = x + SCREEN_SAVE_WIDTH - 1;
    y1 = y + SCREEN_SAVE_HEIGHT - 1;

    BackupAngles(x - 1, y - 1, x1 + 1, y1 + 1);
    Box(x, y, x1, y1, 0);
    DrawCadre(x - 1, y - 1, x1 + 1, y1 + 1, ALL_ANGLES);
    RestoreAngles(x - 1, y - 1, x1 + 1, y1 + 1);
    BoxStaticAdd(x - 1, y - 1, x1 + 1, y1 + 1);
    //        BoxUpdate() ;
}

/*──────────────────────────────────────────────────────────────────────────*/
S32 IsExistSavedGame(void) {
    // a optimiser
    return PlayerGameList((char **)BufSpeak, (char *)BufSpeak + 100000L, FALSE);
}

/*──────────────────────────────────────────────────────────────────────────*/
S32 FirstGame = FALSE;

S32 IsFirstGameLaunched(void) {
    char tmpSaveFilePath[ADELINE_MAX_PATH];
    if (!FirstGame) {
        GetSavePath(tmpSaveFilePath, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
        if (!IsExistSavedGame()
                AND !ExistsFileOrDir(tmpSaveFilePath)) {
            FirstGame = TRUE;
        }
    }

    return FirstGame;
}

/*──────────────────────────────────────────────────────────────────────────*/
/*
   GameMainMenu:

        0, 70,  // reprendre partie
        0, 71,  // nouvelle partie
        0, 72,  // charger partie
        0, 73,  // sauver partie
        0, 74,  // Options
        0, 75,  // Quitter
*/
// Post-save success cue: positive chime + brief centred message overlay.
// Shared by the regular save (case 73) and debug bug-save (case 2001) paths
// in MainGameMenu. Up to ~1 second on screen — skippable by any fire / action
// / menu input. After the wait, drains those inputs (via InitWaitNoInput) so
// the skip-press doesn't immediately re-trigger menu navigation when control
// returns to the surrounding menu loop.
static void ShowSaveConfirmation(enum MenuLocalizedLabel label) {
    HQ_MixSample(SAMPLE_FOUND_OBJ, 0x1000, 0, 1, 64, 127);
    CopyScreen(Screen, Log);
    ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
    const S32 modern = Menu_IsModern();
    {
        char savedMsg[256];
        CopyUtf8ToCp850(savedMsg, sizeof savedMsg, GetLocalizedMenuLabel(label));
        if (modern) {
            /* A toast card: success-green edge, a check mark, the message. */
            UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
            const float cx = (float)ModeDesiredX * 0.5f;
            const float cy = (float)ModeDesiredY * 0.5f;
            const float textW = UiOverlay_TextWidth(savedMsg, 28.0f);
            const float halfW = textW * 0.5f + 52.0f;
            T_UIOVERLAY_CARD card;
            memset(&card, 0, sizeof(card));
            card.Radius = 18.0f;
            card.FillTop = 0xF0283A4Cu;
            card.FillBottom = 0xF0162230u;
            card.Border = 0xFF6ED49Au;
            card.BorderWidth = 2.0f;
            card.Glow = 0x806ED49Au;
            card.GlowSize = 14.0f;
            card.Shadow = 0xA0000000u;
            card.ShadowY = 8.0f;
            card.ShadowBlur = 20.0f;
            UiOverlay_Card(cx - halfW, cy - 32.0f, cx + halfW, cy + 32.0f, &card);
            /* Check mark: a green disc with a short and a long stroke. */
            T_UIOVERLAY_CARD disc;
            memset(&disc, 0, sizeof(disc));
            disc.Radius = 14.0f;
            disc.FillTop = 0xFF7EE0A8u;
            disc.FillBottom = 0xFF3FAE70u;
            const float ix = cx - halfW + 34.0f;
            UiOverlay_Card(ix - 14.0f, cy - 14.0f, ix + 14.0f, cy + 14.0f, &disc);
            UiOverlay_Chevron(ix - 1.0f, cy + 1.0f, 14.0f, 2, 0xFFFFFFFFu);
            UiOverlay_Text(cx + 18.0f, cy, savedMsg, 28.0f, 0xFFFFFFFFu, UIOVERLAY_ALIGN_CENTER, TRUE, 0.0f);
        } else {
            DrawSingleString((S32)ModeDesiredX / 2, (S32)ModeDesiredY / 2, savedMsg);
        }
    }
    BoxStaticFullflip();
    {
        S32 timer;
        ManageTime();
        timer = TimerRefHR + 1000;
        while (TimerRefHR <= timer AND !MyKey
                                 AND !(Input & (I_FIRE | I_ACTION | I_MENUS))) {
            /* A wait on the game clock that draws nothing has no clock of its own under
               --fixed-dt, where time comes from the tick advance in Control_TickHook and
               a modal loop never reaches it: ManageTime() reads the same virtual time
               forever and the wait cannot end. The polled form because this loop calls
               MyGetInput() every iteration, which is what keeps it moving under a
               recorder-held clock; the unpolled Timer_FixedDtPump() would mint a second
               step there and sleep for it. Inert in ordinary play. See docs/TIMING.md. */
            Timer_FixedDtPumpPolled();
            ManageTime();
            MyGetInput();
        }
    }
    if (modern) {
        UiOverlay_ClearLayer(UIOVERLAY_LAYER_MENU);
        BoxStaticAdd(0, 0, 1, 1);
        BoxUpdate();
    }
    InitWaitNoInput(I_FIRE | I_ACTION | I_MENUS);
}

// Non-static entry so the in-game quick-save (PERSO.CPP) can show the same
// "Game Saved" chime + overlay as the main-menu Save path. Keeps the localized
// label enum internal to this module.
void ShowGameSavedConfirmation(void) {
    ShowSaveConfirmation(MENU_LABEL_GAME_SAVED);
}

void BuildGameMainMenu(S32 firstloop) {
    char memoplayername[MAX_SIZE_PLAYER_NAME + 1];
    char memogamepathname[ADELINE_MAX_PATH];
    char tmpSavePath[ADELINE_MAX_PATH];
    U16 *ptrreal = RealGameMainMenu;
    U16 *ptrmain = GameMainMenu;
    U16 nb = 0;
    U16 ycenter = 275;

    strcpy(memoplayername, PlayerName);
    strcpy(memogamepathname, GamePathname);

    *ptrmain++ = *ptrreal++; // selected
    *ptrmain++ = *ptrreal++; // nb entries
    *ptrmain++ = *ptrreal++; // y center
    *ptrmain++ = *ptrreal++; // .dia num

    // reprendre partie

    GetSavePath(tmpSavePath, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
    if (ExistsFileOrDir(tmpSavePath)) {
        *ptrmain++ = *ptrreal;
        *ptrmain++ = *(ptrreal + 1);
        nb++;
        ycenter = 335;
    }

    ptrreal += 2; // nouvelle partie
    *ptrmain++ = *ptrreal++;
    *ptrmain++ = *ptrreal++;
    nb++;

    //#ifndef       DEMO
    // charger partie

    if (IsExistSavedGame()) {
        *ptrmain++ = *ptrreal;
        *ptrmain++ = *(ptrreal + 1);
        nb++;
    }

    ptrreal += 2; // sauver partie

    if (!firstloop
            AND SavingEnable) {
        *ptrmain++ = *ptrreal;
        *ptrmain++ = *(ptrreal + 1);
        nb++;
    }
    /*#else
        ptrreal += 2 ;  // sauver partie
#endif
*/
    ptrreal += 2; // Options

    *ptrmain++ = *ptrreal++;
    *ptrmain++ = *ptrreal++;
    nb++;

    // Quitter
    *ptrmain++ = *ptrreal++;
    *ptrmain++ = *ptrreal++;
    nb++;

    if (firstloop == 2)
        GameMainMenu[0] = (S16)(nb - 1); // selected
    GameMainMenu[1] = nb;                // nb entries
    GameMainMenu[2] = ycenter;           // y center

    strcpy(PlayerName, memoplayername);
    strcpy(GamePathname, memogamepathname);
}

/*──────────────────────────────────────────────────────────────────────────*/

S32 GameInProgress = FALSE;

/* `resumeInGame` enters with a session the caller has already loaded and started, and
   goes straight to gameplay: the loop below is what makes a menu a pause rather than the
   end of the run, because MainLoop returns when the player opens one and this re-enters
   it. The CLI harness is the only caller that wants that; it has done its own load and
   must not have the menu's boot presentation done to it. Kept as its own argument rather
   than a sentinel in `load`, because `load` is passed on to SetDemoSaveGame. */
S32 MainGameMenu(S32 load, S32 resumeInGame) {
    S32 select;
    S32 flag = 0;
    S32 firstloop = TRUE;
    S32 savenewcube;
    S32 gogame = FALSE;
    S32 ret;
    /* Cleared by the first pass through the gameplay branch, so it guards the entry and
       nothing after it. */
    S32 resumeFirstPass = resumeInGame;
    char tmpFilePathSave[ADELINE_MAX_PATH];
    char tmpFilePathScreen[ADELINE_MAX_PATH];

    /* Boot housekeeping: silences the logo. A resume arrives with a loaded scene whose
       ambience is already running, and stopping it here would be heard as the session
       going quiet the moment it starts. */
    if (!resumeInGame) {
        HQ_StopSample();
        StopMusic(); // Pour Stopper la music du Logo
    }

    /* The menu's own animated background, and the flag that tells the menu painters they
       are drawing over a live scene. Both are menu presentation, and a resume goes
       straight to gameplay without drawing one. */
    if (!resumeInGame) {
        InitPlasmaMenu();
        FlagShadeMenu = FALSE;
    }

#ifdef DEMO
    GetSavePath(tmpFilePathSave, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
    if (!ExistsFileOrDir(tmpFilePathSave)
#ifdef DEBUG_TOOLS
            OR load
#endif
    ) {
#ifndef DEBUG_TOOLS
        load = 0;
#endif
        FlagBlackPal = FALSE;
        SetDemoSaveGame(load);
        goto load_game;
    }
#else
    if (!resumeInGame AND IsFirstGameLaunched()) {
        FlagPlayAcf = TRUE;
        goto new_game;
    }
#endif

#ifndef DEMO
    if (load)
        goto load_game;
#endif

    /* Outside the guard below, and that is the point: the loop reads tmpFilePathScreen on
       its redraw paths, so this is the only write to a buffer used well after the setup
       that fills it. Skipping the setup and keeping the reads would hand an uninitialised
       stack buffer to Image_LoadStretched. Its sibling tmpFilePathSave is safe by accident
       rather than design, because the loop re-derives that one where it uses it. */
    GetResPath(tmpFilePathScreen, ADELINE_MAX_PATH, SCREEN_HQR_NAME);

    /* The boot sizes the frame before the cfg says which renderer runs; the
       menu takes the size that renderer wants. */
    if (!resumeInGame)
        Res_ApplySceneRenderSize();

    if (resumeInGame) {
        /* Enter the loop already in gameplay. Nothing below is done: it paints the menu
           background over the caller's scene and points PtrPal at the menu palette, and
           the harness load path has just installed the scene's own by hand precisely
           because it wants neither the fade nor the menu's screen. */
        gogame = TRUE;
        firstloop = FALSE;
    } else {
        Image_LoadStretched(tmpFilePathScreen, Screen, PCR_MENU);
        CopyScreen(Screen, Log);
        BoxStaticFullflip();
        FadeToPal(PtrPalNormal);

        PtrPal = PtrPalNormal;

        BackupScreen(FALSE);
        CopyScreen(Log, Screen);
    }

    /* Menu-only, and paired with the MenuMouseRelease the loop makes before MainLoop. A
       resume goes straight to gameplay and takes neither half on its first pass. */
    if (!resumeInGame)
        MenuMouseAcquire();

    while (!flag) {
        if (gogame) {
            /* Session-start state, and a resume must not have it done on the way in: the
               caller has already started the session and owns these. DemoSlide is the one
               that bites -- the harness sets it through BeginDemoSlide before calling here,
               and clearing it turns an attract-mode run into an ordinary one. Only the
               first pass is skipped; a genuine menu visit later in the run leaves through
               the same code as any other and should reset them the normal way. */
            /* A resume reaches MainLoop with the engine exactly as its caller left it.
               Everything here is the menu handing a session over to gameplay: state it
               owns because it set it up on the way in. The harness set its own up and did
               not come through that path, so on the first pass none of it applies, and
               doing it anyway perturbs the run this exists to replay. Cleared straight
               away, so a real menu visit later leaves through this code as the game does. */
            if (!resumeFirstPass) {
                DemoSlide = FALSE;
                GameInProgress = FALSE;

                ClearPlasmaMenu(); // vire plasma menu

                FlagShadeMenu = TRUE;

                /* Menu mouse is for menu UI only; do not composite it during MainLoop (gameplay). */
                MenuMouseRelease();
            }
            resumeFirstPass = FALSE;

            ret = MainLoop();

            /* A resume hands control back the moment MainLoop returns for a reason that is
               not the menu. The switch below is the game's end-of-session handling -- it
               saves, rolls credits, plays the win cinematic -- and a harness run reaching
               its tick budget or the end of a recording must reach its own reporting
               instead. Only the menu is a pause worth re-entering for. */
            if (resumeInGame AND !Control_LeftForMenu()) {
                flag = TRUE;
                continue;
            }

            BackupScreen(FALSE);

            switch (ret) {
            case 0: // <ESC>
                if (SavingEnable)
                    CurrentSaveGame(); // sauve dans current.lba
                break;

            case 1: // Gagné !!!!
#ifdef DEMO
                SlideShow();
                //                                        DemoBumper() ;
                GetSavePath(tmpFilePathSave, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
                Delete(tmpFilePathSave);
                FlagShowEndDemo = TRUE;

                Image_LoadStretched(tmpFilePathScreen, Log, PCR_MENU);
                CopyScreen(Log, Screen);
                BoxStaticAdd(0, 0, ModeDesiredX - 1, ModeDesiredY - 1);

                PtrPal = PtrPalNormal;
                if (!FlagBlackPal)
                    FadeToBlack(PtrPal);

                FadeMenu = TRUE;
#else
                Credits(TRUE);
                PlayAcf("BABY");
                Image_LoadStretched(tmpFilePathScreen, Screen, PCR_MENU);
                CopyScreen(Screen, Log);
                // PlayAcf now leaves the screen black (original contract). Reveal the
                // win screen with its palette, the same PtrPalNormal the menu is drawn
                // with above; without this the composited image would present black.
                PtrPal = PtrPalNormal;
                FadeToPal(PtrPalNormal);
                EscTimer = 0;
#endif
                break;

            case 2: // GameOver :(
                // Pour ne pouvoir ni reprendre la partie
                // ni la sauver
                firstloop = TRUE;
                EscTimer = 0;
                break;
            }

            CopyScreen(Log, Screen);
            BoxReset();

            MemoClipWindow();
            UnsetClipWindow();

            ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
            // Kept as a flip, unlike the page transitions below: the menu the
            // player just asked for is assembled behind a save-directory scan, a
            // text-bank load and a CD track start, and dimming the scene now is
            // the only sign the key registered while that runs.
            BoxStaticFullflip();

            if (!FadeMenu)
                PaletteSync(PtrPalNormal);

            HQ_PauseSamples();
            FlagShadeMenu = TRUE;
            gogame = FALSE;
            InitPlasmaMenu();

            MenuMouseAcquire();
        }

        BuildGameMainMenu(firstloop);

        InitDial(0); //     SYS

#ifdef CDROM
        PlayMusic(CD_TRACK_MENU, TRUE);
#endif

        HQ_StopSample();

        //              GetMultiText( 49, PleaseWait ) ;

        select = DoGameMenu(GameMainMenu);
        switch (select) // num mess
        {
        case 1000: // <ESC>, on reprend la partie en cours
            if (firstloop) {
                firstloop = 2;
                break;
            }

        case 70: // reprendre partie
            if (!firstloop AND !GameInProgress AND !DemoSlide) {
                ChoicePalette();
                FlagPal = TRUE;
                InitDial(START_FILE_ISLAND + Island);
                HQ_ResumeSamples();

#ifdef CDROM
                StopMusic();
#endif

                RestoreClipWindow();
                FlagFade = FALSE;
                gogame = TRUE;
                RestoreTimer();
            } else {
                GetSavePath(tmpFilePathSave, ADELINE_MAX_PATH, CURRENTSAVE_FILENAME);
                if (ExistsFileOrDir(tmpFilePathSave)) {
                    char memoplayername[256];
                    char memogamepathname[256];

                    DemoSlide = FALSE;

                    strcpy(memoplayername, PlayerName);
                    strcpy(memogamepathname, GamePathname);

                    StopMusic();

                    firstloop = FALSE;
                    strcpy(PlayerName, CURRENTSAVE_NAME);
                    strcpy(GamePathname, tmpFilePathSave);
                    InitGame(-1);
                    ChangeCube();

#ifdef DEMO
                    // Grosse rustine pour savoir dans quelle
                    // sauvegarde on se trouve
                    switch (NumCube) {
                    case 183:
                    case 184:
                        CurrentDemoSave = 1;
                        break;

                    case 125:
                        CurrentDemoSave = 2;
                        break;

                    default:
                        CurrentDemoSave = 0;
                    }

                    NewDemoSave = CurrentDemoSave;
                    FlagShowEndDemo = FALSE;
#endif

                    CopyScreen(Log, ScreenAux);

                    RestoreTimer();
                    SaveTimer();
                    AffScene(AFF_ALL_NO_FLIP);
                    FixeCinemaMode(FALSE);
                    CopyScreen(Log, Screen);

                    // au cas ou le mec meurre avant de faire <ESC>
                    CurrentSaveGame();

                    BoxReset(); // pas de flip

                    // Zoom starts from the floated preview box (must match the
                    // SAVE_THUMB_X, 10 + UI_VCENTER_OFS draw site above).
                    ZoomSavedGame(SAVE_THUMB_X, 10 + UI_VCENTER_OFS);

                    HQ_ResumeSamples();
                    RestoreTimer();

                    FlagFade = FALSE;
                    RestartRainSample = TRUE;

                    strcpy(PlayerName, memoplayername);
                    strcpy(GamePathname, memogamepathname);

                    gogame = TRUE;
                    EscTimer = 0;
                }
            }
            break;

        case 71: // newgame
#ifdef DEMO
            FlagShowEndDemo = FALSE;
            SetDemoSaveGame(0);
            goto load_game;
#else
        new_game:
            firstloop = FALSE;
            strcpy(PlayerName, "");
            PtrPal = PtrPalNormal;
            StopMusic();
            InitGame(1);
            Introduction();
            HQ_StopSample();
            HQ_ResumeSamples();
            FlagFade = TRUE;
            gogame = TRUE;
            EscTimer = 0;
            break;
#endif

        case 72: // load
            switch (ChoosePlayerName(41, FALSE, FALSE)) {
            case 0:
                CopyScreen(Screen, Log);
                if (FlagShadeMenu)
                    ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
                BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
                break;

            default: // load game
            load_game:
                firstloop = FALSE;
                DemoSlide = FALSE;
                StopMusic();
                InitGame(-1);
                ChangeCube();

#ifdef DEMO
                TakeAllDarts();
#endif

                CopyScreen(Log, ScreenAux);

                RestoreTimer();
                SaveTimer();
                AffScene(AFF_ALL_NO_FLIP);
                FixeCinemaMode(FALSE);
                CopyScreen(Log, Screen);

                // au cas ou le mec meurre avant de faire <ESC>
                CurrentSaveGame();

                BoxReset(); // pas de flip

                ZoomSavedGame(SAVE_THUMB_X, SAVE_THUMB_Y);

                HQ_ResumeSamples();
                RestoreTimer();

                FlagFade = FALSE;

                gogame = TRUE;
                EscTimer = 0;
                break;
            }
            break;

        case 73: // Save Game
            if (!firstloop) {
                savenewcube = NewCube;

                if (ChoosePlayerName(42, FALSE, TRUE)) {
                    NumVersion = NUM_VERSION | SAVE_COMPRESS;
                    CopyScreen(Screen, Log);
                    if (FileSize(OldGamePathname))
                        Delete(OldGamePathname);
                    SaveGame(TRUE);
                    HQ_ResumeSamples();
                    UnsetClipWindow();

                    // Success feedback: chime + brief overlay, skippable.
                    // See ShowSaveConfirmation. Benefits keyboard, mouse,
                    // and gamepad flows equally — pre-PR, this was silent.
                    ShowSaveConfirmation(MENU_LABEL_GAME_SAVED);
                }

                CopyScreen(Screen, Log);

                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
                NewCube = savenewcube;
                BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
                RestoreTimer();
            }
            break;

        case 74: // Options
            CopyScreen(Screen, Log);
            BoxStaticAdd(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
            OptionsMenu(TRUE);
            break;

        case 75: // quitter
            FadeToBlack(PtrPalNormal);
            Cls();
            BoxUpdate();
            flag = 1;
            break;

#ifdef DEBUG_TOOLS
        // Gestion des sauvegardes de bugs
        case 2000: // chargement d'un bug
            switch (ChoosePlayerName(41 | 0x80000000, FALSE, FALSE)) {
            case 0:
                CopyScreen(Screen, Log);
                if (FlagShadeMenu)
                    ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
                BoxStaticFullflip();
                break;

            default: // load game
                firstloop = FALSE;
                DemoSlide = FALSE;
                StopMusic();
                InitGame(-1);
                ChangeCube();

                CopyScreen(Log, ScreenAux);

                RestoreTimer();
                SaveTimer();
                AffScene(AFF_ALL_NO_FLIP);
                FixeCinemaMode(FALSE);
                CopyScreen(Log, Screen);

                // au cas ou le mec meurre avant de faire <ESC>
                CurrentSaveGame();

                BoxReset(); // pas de flip

                ZoomSavedGame(SAVE_THUMB_X, SAVE_THUMB_Y);

                HQ_ResumeSamples();
                RestoreTimer();

                FlagFade = FALSE;

                gogame = TRUE;
                EscTimer = 0;
                break;
            }
            break;

        case 2001: // sauvegarde d'un bug
            if (!firstloop) {
                savenewcube = NewCube;

                if (ChoosePlayerName(42 | 0x40000000, FALSE, TRUE)) {
                    NumVersion = NUM_VERSION | SAVE_COMPRESS;
                    CopyScreen(Screen, Log);
                    SaveGame(TRUE);
                    NewCube = savenewcube;
                    HQ_ResumeSamples();
                    ShowSaveConfirmation(MENU_LABEL_BUG_SAVED);
                }

                CopyScreen(Screen, Log);

                ShadeBoxBlk(0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL);
                NewCube = savenewcube;
                BoxStaticFullflip();
                RestoreTimer();
            }
            break;
#endif
        }
    }
    MenuMouseRelease();
    return FALSE;
}

/*══════════════════════════════════════════════════════════════════════════*
       █▀▀▀▀ █▀▀▀█ █▄ ▄█ █▀▀▀▀       █▀▀▀▀ █   █ █▀▀▀█  █    █▀▀▀▀ █▀▀▀▀
       ██ ▀█ ██▀▀█ ██▀ █ ██▀▀        ██    ██▀▀█ ██  █  ██   ██    ██▀▀
       ▀▀▀▀▀ ▀▀  ▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀  ▀▀   ▀▀▀▀▀ ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
/*──────────────────────────────────────────────────────────────────────────*/

/* Long enough to outlast any chosen line, so the press is still down when the main loop
   next polls; Control_KeyClear below ends it rather than letting it run out. */
#define ASKCHOICE_PRESS_POLLS 100000

/* Harness drive state -- see AskChoice_RequestDrive() in GAMEMENU.H. Both zero in
   normal play, which is every path that does not go through `ui askchoice`. */
static U32 s_askchoice_pick_key = 0;
static U32 s_askchoice_press_key = 0;
static U32 s_askchoice_line_polls = 0;

U32 AskChoice_LastLinePolls(void) {
    return s_askchoice_line_polls;
}

void AskChoice_RequestDrive(U32 pickScancode, U32 pressScancode) {
    s_askchoice_pick_key = pickScancode;
    s_askchoice_press_key = pressScancode;
}

void GameAskChoice(S32 nummess) {
    S32 n;

    GameChoiceMenu[0] = 0;
    GameChoiceMenu[1] = GameNbChoices;
    GameChoiceMenu[2] = 0;
    GameChoiceMenu[3] = (U16)(START_FILE_ISLAND + Island);
    for (n = 0; n < GameNbChoices; n++)
        MenuSetEntry(GameChoiceMenu, n, 0 /* plain button */, GameListChoice[n]);

    MyDial(nummess);

    InitPlasmaMenu();

    MenuMouseAcquire();

    /* Land the pick a few polls in: DoGameMenu latches whatever is held when it
       opens, so a key already down when we get here would be masked, not seen.
       A refused arm would leave DoGameMenu waiting for a selection that never comes,
       which surfaces as a hung run rather than a diagnosis, so say so here. */
    if (s_askchoice_pick_key && !Control_KeyHold(s_askchoice_pick_key, 8, 6))
        Log_Warn("askchoice: could not arm the pick key (scancode %u)",
                 (unsigned)s_askchoice_pick_key);

    if (DoGameMenu(GameChoiceMenu) != 1000)
        GameChoice = GameListChoice[GameChoiceMenu[0]]; // ret num mess
    else {
        GameChoice = GameListChoice[0]; // ESC

        // redessine curseur
        GameChoiceMenu[0] = 0;
        DrawGameMenu(GameChoiceMenu, FALSE);
    }

    ClearPlasmaMenu();

    MenuMouseRelease();

    InitWaitNoKey();
    InitWaitNoInput(I_FIRE | I_ACTION | I_MENUS);

#ifdef CDROM
    NumObjSpeak = NUM_PERSO;
    Speak(GameChoice);
    MyGetInput();
    /* A fresh press, mid-line: held from just after playback starts, past the arm
       above, and long enough to still be down when the main loop next polls. Released
       below rather than left to expire, so it does not stay down for the rest of the
       session on a run that keeps going. */
    if (s_askchoice_press_key && !Control_KeyHold(s_askchoice_press_key, 4, ASKCHOICE_PRESS_POLLS))
        Log_Warn("askchoice: could not arm the press key (scancode %u)",
                 (unsigned)s_askchoice_press_key);
    U32 linepolls = Control_KeyPollCount();
    while (TestSpeak() AND(MyKey != K_ESC AND !(Input & I_MENUS)))
        MyGetInput(); // Wait until silence
    StopSpeak();      // Security
    /* Below StopSpeak so the two aligned trailing comments above stay adjacent; it costs
       nothing, since StopSpeak does not poll input. */
    s_askchoice_line_polls = Control_KeyPollCount() - linepolls;

    // Swallow the press that ended the line. Without this the same held Menus/Esc is
    // read a second time by the main loop once the life script unwinds, so skipping the
    // line also opens the main menu. Dial() and MyDial() consume after their identical
    // skip loops; this path only armed before Speak(), and that arm cannot cover a key
    // pressed later -- NoRepeatInput masks a bit only while it stays held.
    InitWaitNoKey();
    InitWaitNoInput(I_FIRE | I_ACTION | I_MENUS);
#endif

    /* One scenario per arming, like the capture hooks. Control_KeyClear lifts the press
       as well as forgetting it: the hold is deliberately longer than the line, so without
       this the key stays down after the verb returns. */
    if (s_askchoice_pick_key || s_askchoice_press_key)
        Control_KeyClear();
    AskChoice_RequestDrive(0, 0);
}

/*══════════════════════════════════════════════════════════════════════════*
             █▀▀▀▀ █▀▀▀█ █▄ ▄█ █▀▀▀▀       █▀▀▀█ █   █ █▀▀▀▀ █▀▀▀█
             ██ ▀█ ██▀▀█ ██▀ █ ██▀▀        ██  █ ██ ▄▀ ██▀▀  ██▀█▀
             ▀▀▀▀▀ ▀▀  ▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀▀   ▀▀▀▀▀ ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

#define GAMEOVER_ZOOM_TEMPO 1000
#define GAMEOVER_TEMPO 3 * 1000

#define GAMEOVER_MINZOOM 5000
#define GAMEOVER_MAXZOOM 200

void GameOver() {
    S16 *ptr3do;
    S32 zoom;
    U32 chrono;
    char tmpFilePath[ADELINE_MAX_PATH];

    HQ_StopSample();

    if (ListVarGame[160] > 0) {
        SaveTimer();

        if (!FlagBlackPal)
            FadeToBlack(PtrPal);

        PlayAcf("DELUGE");

        if (CubeMode == CUBE_INTERIEUR) {
            InitGrille(NumCube);
        }
        RazListPartFlow();
        ChoicePalette();

        RestoreTimer();
        FlagFade = TRUE;
        AffScene(AFF_ALL_NO_FLIP);
        CopyScreen(Log, Screen);
        BoxBlit();
        BoxReset();
    } else {
        BackupScreen(FALSE);
        CopyScreen(Log, Screen);
        BoxReset();
    }

    PaletteSync(PtrPal);

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, RESS_HQR_NAME);
    ptr3do = (S16 *)LoadMalloc_HQR(tmpFilePath, RESS_GAME_OVER);
    if (!ptr3do)
        return;

    // lance un sample ou music de game over ??

    zoom = GAMEOVER_MINZOOM;

    SetProjection((S32)ModeDesiredX / 2, (S32)ModeDesiredY / 2, 128, 200, 200);
    SetFollowCamera(0, 0, 0, 0, 0, 0, zoom);
    SetLightVector(0, 0, 0);

    ManageTime();
    chrono = TimerRefHR;

    // Magouille pour precharger le sample
    if (SamplesEnable) {
        HQRGetDelFunc = DelBlocHQRSample;
        GivePtrSample(SAMPLE_GAME_OVER, NULL);
        HQRGetDelFunc = NULL;
    }

    MyGetInput();

    while ((MyKey != K_ESC AND !(Input & I_MENUS)) AND(TimerRefHR - chrono <= GAMEOVER_ZOOM_TEMPO)) {
        /* The BoxUpdate below sits inside two conditionals, so it is not a clock source
           this loop can rely on: with the body fully clipped nothing presents and the
           deadline never arrives. Mint the step here instead (docs/TIMING.md). */
        Timer_FixedDtPumpPolled();
        ManageTime();
        MyGetInput();

        zoom = BoundRegleTrois(GAMEOVER_MINZOOM, GAMEOVER_MAXZOOM,
                               GAMEOVER_ZOOM_TEMPO, TimerRefHR - chrono);

        SetFollowCamera(0, 0, 0, 0, 0, 0, zoom);

        UnsetClip();

        if (BodyDisplay(0, 0, 0, 0, 0, 0, ptr3do)) {
            // Objet pas tout clippé
            SetClip(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax);
            if (ClipXMin <= ClipXMax
                                AND ClipYMin <= ClipYMax) {
                BoxMovingAdd(ClipXMin, ClipYMin, ClipXMax, ClipYMax);
                BoxUpdate();
            }
        }
    }

    HQ_MixSample(SAMPLE_GAME_OVER, 0x1000, 2000, 1, 64, 127);

    SetFollowCamera(0, 0, 0, 0, 0, 0, zoom);

    UnsetClip();

    if (BodyDisplay(0, 0, 0, 0, 0, 0, ptr3do)) {
        // Objet pas tout clippé
        SetClip(ScreenXMin, ScreenYMin, ScreenXMax, ScreenYMax);
        if (ClipXMin <= ClipXMax
                            AND ClipYMin <= ClipYMax) {
            BoxStaticAdd(ClipXMin, ClipYMin, ClipXMax, ClipYMax);
            BoxUpdate();
        }
    }

    chrono = TimerRefHR + GAMEOVER_TEMPO;
    while ((TimerRefHR < chrono) AND(MyKey != K_ESC AND !(Input & I_MENUS))) {
        /* Non-presenting wait that polls: mint its own time (docs/TIMING.md). */
        Timer_FixedDtPumpPolled();
        ManageTime();
        MyGetInput();
    }

    UnsetClip();
    Free(ptr3do);

    PtrInit3DView();

    FadeToBlack(PtrPal);

    PtrPal = PtrPalNormal;
    FadeMenu = TRUE;

    UnsetClip();
}

/*══════════════════════════════════════════════════════════════════════════*
                            █   █ █▀▀▀▀ █     █▀▀▀█
                            ██▀▀█ ██▀▀  ██    ██▀▀▀
                            ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀▀ ▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
/*
void    Help( U8 flaggame )
{
        if( !flaggame ) // appelé depuis les menus
        {
                SaveTimer() ;

                FlagFade = FALSE ;

                EffectPcx( PCR_HELP/2, (U8*)Log, 0, flaggame ) ;

                WaitNoInput()   ;
                WaitInput()     ;
                FadeToBlack( PalettePcx ) ;
                CopyScreen( Screen, Log ) ;
                if( FlagShadeMenu )     ShadeBoxBlk( 0, 0, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1, SCREEN_SHADE_LVL ) ;
                BoxStaticFullflip() ;
                FadeMenu = TRUE ;
                RestoreTimer()  ;
        }
        else    EffectPcx( PCR_HELP/2, (U8*)Screen, 0, flaggame ) ;
}
*/

/*══════════════════════════════════════════════════════════════════════════*
                   █▀▀▀▀ █▀▀▀█ █▀▀▀▀ █▀▀▀▄  █    ▀▀█▀▀ ██▀▀▀
                   ██    ██▀█▀ ██▀▀  ██  █  ██     ██  ▀▀▀▀█
                   ▀▀▀▀▀ ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀   ▀▀     ▀▀  ▀▀▀▀▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void Credits(S32 mode) {
    char tmpFilePath[ADELINE_MAX_PATH];

    SaveTimer();

    //      FadeToBlack( PtrPalNormal ) ;

    WaitNoInput();

    SaveCamera();

    /* Endgame transition can leave audio playing and PtrPal unset (issue #65).
       Mirror the safety prologue PlayAcf uses so GamePlayCredits has valid
       prerequisites regardless of caller (endgame win path or console). */
    StopMusic();
    HQ_PauseSamples();
    if (!PtrPal)
        PtrPal = PtrPalNormal;

    /* GamePlayCredits stomps several gameplay-relevant buffers because the
       original game never returned to play after credits. To make console
       invocation safe mid-game, save/restore them around the call. (issue #65)

         * PtrZBuffer's backing buffer aliases BufCube + BufferBrick (the cube
           grid and brick library — see MEM.CPP:187). Credits memsets it and
           renders Z into it, destroying the cube state.
         * BufferAnim holds animation frame state for ALL game objects.
           InitObjects() resets it; credits writes its own animation state
           into the buffer, so any in-game T_OBJ_3D with LastOfsIsPtr=1 holds
           pointers into now-stale data.

       GameInProgress is FALSE during MainLoop (set TRUE only after this
       function), so it is not a usable gate. Always save; cost is one malloc
       + memcpy round trip per buffer, ~1 ms total. */
    U8 *zbufSave = NULL;
    U32 zbufSize = (U32)ListMem[MEM_PTR_ZBUFFER].Size;
    if (zbufSize && PtrZBuffer) {
        zbufSave = (U8 *)malloc(zbufSize);
        if (zbufSave)
            memcpy(zbufSave, PtrZBuffer, zbufSize);
    }
    extern U8 *PtrLib3DBufferAnim; /* LIB386/ANIM/LIBINIT — local extern to avoid header churn */
    U8 *animSave = NULL;
    U8 *animCursorSave = NULL;
    if (BufferAnim) {
        animSave = (U8 *)malloc(SIZE_BUFFER_ANIM);
        if (animSave)
            memcpy(animSave, BufferAnim, SIZE_BUFFER_ANIM);
        animCursorSave = PtrLib3DBufferAnim;
    }

    /* Suppress console toggle for the duration of the credits sequence:
       opening the console mid-credits side-effects palette and screen state
       Credits doesn't expect, and there's no useful interaction during a
       cinematic. Mirrors the gating already in place around SlideShow(). */
    Console_SetSlideShowActive(1);

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, CREDITS_HQR_NAME);
    GamePlayCredits(tmpFilePath, mode);

    Console_SetSlideShowActive(0);

    if (zbufSave) {
        memcpy(PtrZBuffer, zbufSave, zbufSize);
        free(zbufSave);
    }
    if (animSave) {
        memcpy(BufferAnim, animSave, SIZE_BUFFER_ANIM);
        free(animSave);
        /* end_credits already re-installed the game's body/anim callbacks via
           InitObjects(BufferAnim, …) but reset the bump cursor to the start.
           Restore the cursor too, so subsequent ObjectInitAnim allocations
           don't alias frame slots already referenced by in-game T_OBJ_3D's
           with LastOfsIsPtr=1. */
        PtrLib3DBufferAnim = animCursorSave;
    }

    /* Mirror HoloMap's gameplay-restore tail (HOLOGLOB.CPP:1186-1199): re-bind
       the per-cube 3D function-pointer table (PtrWorldColBrick, PtrDoAnim, …)
       and force a full screen flip on the next frame. Without this, returning
       to MainLoop renders against credits' projection/fog/filler globals and
       the player character does not appear. */
    PtrInit3DGame();
    FlagFade = TRUE;
    FirstTime = AFF_ALL_FLIP;

    RestoreCamera();

    GameInProgress = TRUE;

    if (!mode) {
        GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
        Image_LoadStretched(tmpFilePath, Screen, PCR_MENU);
        CopyScreen(Screen, Log);
        BoxStaticFullflip();
    }

    FadeMenu = TRUE;

    WaitNoInput();

    RestoreTimer();
}

/*──────────────────────────────────────────────────────────────────────────*/

/* Harness capture state — see SlideShow_RequestCapture() in GAMEMENU.H. When
   armed, SlideShow's main loop breaks out after the first frame's Dial returns
   (Dial itself does the SavePNG via Dial_RequestCapture, which we arm
   alongside this flag). Same compose pattern as Menu_*Capture / Inv_*Capture. */
static int s_slideshow_capture_active = 0;

void SlideShow_RequestCapture(const char *path) {
    if (!path || !path[0]) {
        s_slideshow_capture_active = 0;
        Dial_RequestCapture(NULL);
        return;
    }
    s_slideshow_capture_active = 1;
    /* The actual SavePNG happens inside Dial's typewriter loop — same hook
       used by `ui dialog`. We just need to break out of the slideshow loop
       after that fires so the harness doesn't iterate the remaining shots. */
    Dial_RequestCapture(path);
}

// SlideShow de screenshots
S32 SlideShow(void) {
    U8 pal[768 + RECOVER_AREA];
    S32 n;
    S32 endtimer;
    U32 nbshots;
    U32 handle;
    S32 ret = TRUE;
    S32 memofirsttime = FirstTime;
    U8 fadepal = FALSE;
    U8 memobulle = Bulle;
    U8 memocoul;
    char tmpScrshotFilePath[ADELINE_MAX_PATH];

    // recupere nombre de screen shot dans le .HQR directement
    GetResPath(tmpScrshotFilePath, ADELINE_MAX_PATH, SCRSHOT_HQR_NAME);
    handle = OpenRead(tmpScrshotFilePath);
    if (handle) {
        Read(handle, &nbshots, 4);
        nbshots = ((nbshots / 4) - 1) / 2; // /2 car il y a aussi les palettes
        Close(handle);
    } else
        return TRUE;

    InitDial(2); // pour les textes sur les screen shots

    /* Prevent opening console during slideshow frames (no stable gameplay buffers/callback flow there). */
    Console_SetSlideShowActive(1);
    Bulle = FALSE;

    /* The slideshow renders centred 4:3 PCX stills with story text on top.
       Setting Dial_Letterbox keeps the dialog box inside the picture's 4:3
       region instead of stretching into the black sidebars at wider widths. */
    S32 memoDialLetterbox = Dial_Letterbox;
    Dial_Letterbox = 1;

#ifdef DEMO
    FlagSlideShow = TRUE; // pour les tempos des messages
#endif

    FirstTime = AFF_OBJETS_NO_FLIP;

    memocoul = ListObjet[NUM_PERSO].CoulObj;
    ListObjet[NUM_PERSO].CoulObj = 0;

    NumObjDial = NUM_PERSO;

    Init3DView();

    for (n = 0; n < nbshots; n++) {
        InitWaitNoKey();

        FadeToBlack(PtrPal);

        // charge image — clear to black first so widescreen sidebars don't keep
        // stale pixels (FadeToBlack only fades the palette). No-op at 640x480.
        memset(Log, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);
        Image_LoadCentered(tmpScrshotFilePath, Log, (n * 2));

        // on peut pas afficher le logo LBA2 car il est dans la palette
        // du jeu
        //		AffGraph( 0, 639-105, 3, HQR_Get(HQRPtrSprite,11) ) ;

        Dial(START_SLIDESHOW_TEXT + n, FALSE);

        /* Harness: SlideShow_RequestCapture arms a one-shot SavePNG in Dial
           above. After it fires, exit cleanly without iterating remaining
           shots so the harness's --exit can finish promptly. */
        if (s_slideshow_capture_active) {
            s_slideshow_capture_active = 0;
            ret = FALSE;
            break;
        }

        // charge palette
        Load_HQR(tmpScrshotFilePath, pal, (n * 2) + 1);

        // reforce en blanc la couleur 12
        pal[12 * 3] = 255;
        pal[12 * 3 + 1] = 255;
        pal[12 * 3 + 2] = 255;

        BoxStaticFullflip();
        FadeToPal(pal);

        fadepal = TRUE;
        PtrPal = pal;

        ManageTime();
        endtimer = TimerRefHR + 4 * 1000;

        while ((TimerRefHR < endtimer) AND !MyKey) {
            /* Non-presenting wait that polls: mint its own time (docs/TIMING.md). */
            Timer_FixedDtPumpPolled();
            ManageTime();
            MyGetInput();
        }

        if (MyKey == K_ESC OR(Input & I_MENUS)) {
            ret = FALSE;
            break;
        }

        if (MyKey == K_D)
            break;
    }

    FirstTime = memofirsttime;
    Bulle = memobulle;
    ListObjet[NUM_PERSO].CoulObj = memocoul;

#ifdef DEMO
    FlagSlideShow = FALSE;
#endif

    InitDial(0);

    if (fadepal) {
        FadeToBlack(pal);
        PtrPal = PtrPalNormal;
    }

    GameInProgress = TRUE; // pour recharger la partie en cours

    /* Restore normal console availability once slideshow exits. */
    Console_SetSlideShowActive(0);
    Dial_Letterbox = memoDialLetterbox;
    return ret;
}

// Enter "canned demo" mode (SHIFT+D from the main menu, the idle attract trigger, the
// SlideDemo debug entry, and the harness --demo flag all funnel through here). The
// actual RNG-seed pinning happens in ChangeCube via Demo_RngSeed(DemoSlide, ..., NewCube)
// -- while DemoSlide is set, the cube number is the seed instead of TimerRefHR,
// so the reel reproduces on every host regardless of wall-clock pace. The earlier
// "zero TimerRefHR here" approach (PR #249) only worked under --fixed-dt because
// ManageTime() runs between this call and the first ChangeCube on the SHIFT+D /
// idle-attract path and banks (SDL_GetTicks() - LastTime) right back into TimerRefHR.
// The TimerRefHR = 0 line remains as cheap defense-in-depth for any other clock-derived
// state read during the reel (currently none we know of; cheap to keep). See issue #176.
void BeginDemoSlide(void) {
    TimerRefHR = 0;
    DemoSlide = TRUE;
}

void SlideDemo(int argc, char *argv[]) {
    S32 aff = TRUE;
    U32 cubedeb = 5;

#ifdef DEBUG_TOOLS
    if (argc > 1)
        cubedeb = atoi(argv[1]);
#else
    argc = argc; // pour virer warning
    argv = argv;
#endif

    BeginDemoSlide();

    FOREVER {
        if (!BumperLogo()) {
            aff = FALSE;
            break;
        }

        InitGame(1);
        NewCube = cubedeb;

        FlagFade = TRUE;

        if (!MainLoop())
            break;

        if (!SlideShow())
            break;

        //#ifdef        CDROM
        //              StopMusicCD() ;
        //#endif

        FadeToBlack(PtrPal);
        Cls();
        BoxUpdate();

        //              Credits(?) ;

        if (!AdelineLogo())
            break;
    }

    FadeToBlack(PtrPal);
    //#ifdef        CDROM
    //      StopMusicCD() ;
    //#endif

    if (aff)
        BumperLogo();
}

/*──────────────────────────────────────────────────────────────────────────*/

S32 AdelineLogo(void) {
    char tmpFilePath[ADELINE_MAX_PATH];
    U32 endtimer;
    S32 ret = TRUE;
    //      S32     volume ;

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
    // FillFromEdge: the Adeline logo PCX has a white background that
    // should extend across the framebuffer at widescreen, not be
    // black-cropped to the centred 640. Sampling the PCX's top-left
    // pixel picks up its white-index regardless of which palette
    // entry holds white. No-op at 640x480.
    if (!Image_LoadCenteredFillFromEdge(tmpFilePath, Log, PCR_LOGO))
        TheEndCheckFile(tmpFilePath);

    if (!Load_HQR(tmpFilePath, PalettePcx, PCR_LOGO + 1))
        TheEndCheckFile(tmpFilePath);

    PtrPal = PalettePcx;

    Palette(PalettePcx);

    //      volume = RegleTrois( 0, SampleVolume, 127, 127 ) ;
    // ATTENTION: déborde sur ScreenAux (voir MEM.CPP) !!!!!!!
    //      GetResPath(tmpFilePath, ADELINE_MAX_PATH, SAMPLES_HQR_NAME);
    //      if( Load_HQR( tmpFilePath, PtrZBuffer, SAMPLE_LOGO ) )
    {
        S32 timer;

        // pause permettant d'attendre (a peu près) la synchro ecran !
        ManageTime();
        timer = TimerRefHR + 1000;

        while (TimerRefHR <= timer) {
            ManageTime();
        }

        //              PlaySample( PtrZBuffer, SAMPLE_LOGO, 0x1000, 1, volume, 64 ) ;
    }

    InitWaitNoKey();

    PlayMusic(MUSIC_LOGO, TRUE);

    BoxStaticFullflip();

    ManageTime();
    endtimer = TimerRefHR + 3 * 1000; // 3s
    while ((TimerRefHR <= endtimer OR IsStreamPlaying()) AND !MyKey) {
        ManageTime();
        MyGetInput();
        if (FlagMenuMouse && (Click & 1))
            break;
    }
    if (MyKey == K_ESC OR(Input & I_MENUS))
        ret = FALSE;

    FlagBlackPal = FALSE;
    FadeToBlack(PalettePcx);

    ManageTime();
    endtimer = TimerRefHR + 1 * 1000;
    while (TimerRefHR <= endtimer AND !MyKey) {
        ManageTime();
        MyGetInput();
        if (FlagMenuMouse && (Click & 1))
            break;
    }
    if (MyKey == K_ESC OR(Input & I_MENUS))
        ret = FALSE;

    FadeOutVolumeMusic();
    StopMusic();

#ifdef CDROM
    SetVolumeCD(CDVolume);
#endif
    SetVolumeJingle(JingleVolume);

    //      HQ_StopSample() ;
    //      StopMusic() ;

    // ATTENTION: La music du logo est stoppée plus tard (Dans MainGameMenu())

    return ret;
}

/*──────────────────────────────────────────────────────────────────────────*/

S32 ShowLogo(S32 numscr, S32 nsec) {
    U32 endtimer;
    S32 ret = TRUE;
    char tmpFilePath[ADELINE_MAX_PATH];

    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
    // Widescreen sidebars treatment: the Adeline studio bumper PCX has
    // a white background that should extend across the framebuffer, so
    // the fade-in reads as one contiguous frame instead of a centred
    // white logo with black wings. Sampling the PCX's top-left pixel
    // via Image_LoadCenteredFillFromEdge handles this without hard-
    // coding the palette's white-index. All other PCRs (distributor
    // logos: Activision / EA / Virgin / publisher bumpers) pre-clear
    // Log to 0 so the sidebars are explicit black instead of whatever
    // leftover bytes the previous frame left behind. Both branches are
    // no-ops at 640x480 (image fully overwrites the buffer).
    U32 loaded;
    if (numscr == PCR_BUMPER) {
        loaded = Image_LoadCenteredFillFromEdge(tmpFilePath, Log, numscr);
    } else {
        memset(Log, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);
        loaded = Image_LoadCentered(tmpFilePath, Log, numscr);
    }
    if (loaded
            AND Load_HQR(tmpFilePath, PalettePcx, numscr + 1)) {
        WaitNoInput();

        PtrPal = PalettePcx;

        BoxStaticFullflip();
        FadeToPal(PalettePcx);

        ManageTime();
        endtimer = TimerRefHR + nsec * 1000; // wait nsec

        while (TimerRefHR <= endtimer AND !MyKey) {
            /* Non-presenting wait that polls: mint its own time (docs/TIMING.md). */
            Timer_FixedDtPumpPolled();
            ManageTime();
            MyGetInput();
            if (FlagMenuMouse && (Click & 1))
                break;
        }

        if (MyKey == K_ESC OR(Input & I_MENUS))
            ret = FALSE;

        FadeToBlack(PalettePcx);
    }

    return ret;
}

/*──────────────────────────────────────────────────────────────────────────*/

void DistribLogo(void) {
#ifdef DEMO
    /* Bumper 3 Distributeurs. Not gated on Distrib_ShowSplash: the demo's
       bumper is part of its own presentation and owes nothing to
       DistribVersion, and the splash default is off for anything that declares
       no release, which is every demo tree. */
    ShowLogo(3 * 2, 5);
#else
    /* Which publisher a release was licensed to show is not a fact about its
       assets, so a value the data supplied does not get to put a logo in front
       of anyone. Distrib_Resolve decides; see DISTRIB.H. */
    if (!Distrib_ShowSplash()) {
        return;
    }

    // depend de la version
    switch (DistribVersion) {
    case ACTIVISION_VERSION:
    case ACTIVISION_SUD_VERSION:
        ShowLogo(PCR_ACTIVISION, 2);
        break;

    case EA_VERSION:
        ShowLogo(PCR_EA, 2);
        break;

    case VIRGIN_VERSION:
    case VIRGIN_ASIA_VERSION:
        ShowLogo(PCR_VIRGIN, 2);
        break;
    }
#endif
}

/*──────────────────────────────────────────────────────────────────────────*/

S32 BumperLogo(void) {
    /*	U32     endtimer ;
	S32     ret = TRUE ;
	chat    tmpFilePath[ADELINE_MAX_PATH];

	GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
	if( !Load_HQR( tmpFilePath, Log, PCR_BUMPER ) )
		TheEndCheckFile( tmpFilePath ) ;

	if( !Load_HQR( tmpFilePath, PalettePcx, PCR_BUMPER+1) )
		TheEndCheckFile( tmpFilePath ) ;

	WaitNoInput() ;

	PtrPal = PalettePcx ;

	BoxStaticFullflip() ;
	FadeToPal( PalettePcx ) ;

	ManageTime() ;
	endtimer = TimerRefHR + 3*1000 ;

	while( TimerRefHR<=endtimer AND !MyKey )
	{
		ManageTime() ;
		MyGetInput() ;
	}

	if( MyKey==K_ESC OR (Input&I_MENUS) )   ret = FALSE ;

	FadeToBlack( PalettePcx ) ;

	return ret ;
*/
    return (ShowLogo(PCR_BUMPER, 5));
}

/*──────────────────────────────────────────────────────────────────────────*/

#ifdef DEMO
void DemoLogo(void) {
    char string[MULTI_TEXT_MAX];
    S32 x0, y0, x1, y1;
    S32 endtimer;

    FlagBlackPal = FALSE;

    FadeToBlackAndSamples(PtrPal);
    BoxReset();
    Cls();

    strcpy(string, "      Demo  Version      ");
    ColorFont(LBAWHITE);

    // cadre
    x0 = UI_CENTER_X - SizeFont(string) / 2 - 50;
    x1 = UI_CENTER_X + SizeFont(string) / 2 + 50;
    y0 = (S32)ModeDesiredY / 2 - 25;
    y1 = (S32)ModeDesiredY / 2 + 25;
    //      ShadeBoxBlk( 0, 0, 639, 479, SCREEN_SHADE_LVL ) ;
    BackupAngles(x0, y0, x1, y1);
    ShadeBoxBlk(x0, y0, x1, y1, MENU_SHADE_LVL);
    DrawCadre(x0, y0, x1, y1, ALL_ANGLES);
    RestoreAngles(x0, y0, x1, y1);
    Font(x0 + 50, y0 + 7, string);
    BoxStaticFullflip();

    FadeToPal(PtrPal);

    InitWaitNoKey();

    ManageTime();
    endtimer = TimerRefHR + 2 * 1000;

    // wait a key or tempo
    while (TimerRefHR <= endtimer AND !MyKey) {
        /* Non-presenting wait that polls: mint its own time (docs/TIMING.md). */
        Timer_FixedDtPumpPolled();
        ManageTime();
        MyGetInput();
    }

    FadeToBlack(PtrPal);
    Cls();
    BoxUpdate();
    FlagBlackPal = FALSE;
}

/*──────────────────────────────────────────────────────────────────────────*/

void DemoBumper(void) {
    /*
	U8      pal[768+RECOVER_AREA] ;
	char    tmpFilePath[ADELINE_MAX_PATH];

	GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
	if( Load_HQR( tmpFilePath, Log, PCR_BUMPER )
	AND Load_HQR( tmpFilePath, pal, PCR_BUMPER+1 ) )
	{
		S32     endtimer ;

		WaitNoInput() ;

		FadeToBlack( PtrPal ) ;
		BoxStaticFullflip() ;
		FadeToPal( pal ) ;

		PtrPal = PtrPalNormal ;

		ManageTime() ;
		endtimer = TimerRefHR + 2*1000 ;

		while( (TimerRefHR<endtimer) AND !MyKey )
		{
			ManageTime() ;
			MyGetInput() ;
		}

		FadeToBlack( pal ) ;

		Load_HQR( tmpFilePath, Log, PCR_MENU ) ;
	}
*/
    /* ShowLogo leaves the bumper on screen, so the menu backdrop has to come
       back from screen.hqr before the caller flips. */
    char tmpFilePath[ADELINE_MAX_PATH];
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);

    ShowLogo(PCR_BUMPER, 5);
    Load_HQR(tmpFilePath, Log, PCR_MENU);
}
#endif // DEMO

/*══════════════════════════════════════════════════════════════════════════*
                               █▀▀▀█ █▀▀▀▀ ▀▄ ▄▀
                               ██▀▀▀ ██     ▄▀▄
                               ▀▀    ▀▀▀▀▀ ▀   ▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/
void EffectPcx(U8 numpcx, U8 *screen, U8 effect, U8 flagbcl) {
    char tmpFilePath[ADELINE_MAX_PATH];

    /* PCX cinematic: any dialog drawn over the centred PCX should letterbox to
       stay inside the picture's 4:3 region. The flag is scoped to this call —
       AffScene at the end (in the flagbcl branch) re-renders the gameplay
       scene, and subsequent Dial calls during normal play will see the
       restored original value. */
    S32 memoDialLetterbox = Dial_Letterbox;
    Dial_Letterbox = 1;

    SaveTimer();

    numpcx *= 2; // car il y a les palettes intercalées

    if (!FlagFade)
        FadeToBlackAndSamples(PtrPal);

    BoxReset(); // pour virer les boites de la scene

    // charge palette qui se trouve derriere dans le .HQR
    GetResPath(tmpFilePath, ADELINE_MAX_PATH, SCREEN_HQR_NAME);
    if (!Load_HQR(tmpFilePath, PalettePcx, numpcx + 1))
        TheEndCheckFile(tmpFilePath);

    switch (effect) {
    case PCX_FADE:
        /* Clear the whole framebuffer to black first: Image_LoadCentered only
           writes the centred 640x480 region, so at widescreen the sidebars
           keep stale pixels that FadeToPal then re-indexes under the PCX
           palette (garbage margins). No-op at 640x480 — the image fills it. */
        memset(Log, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);
        if (!Image_LoadCentered(tmpFilePath, Log, numpcx))
            TheEndCheckFile(tmpFilePath);

        BoxStaticFullflip();
        FadeToPal(PalettePcx);
        break;

    case PCX_V_SHADE: {
        /* Venetian-blind reveal: load the PCX into <screen> with centering +
           proper stride, then copy bands of rows from <screen> to Log row by
           row to animate the reveal. CopyBlock src/dst coords are shifted by
           the centering offsets so the reveal lines up with the centred PCX
           content; the sidebars stay whatever was already in Log. */
        S32 x, y;
        const S32 xOff = ((S32)ModeDesiredX - IMAGE_AUTHORED_WIDTH) / 2;
        const S32 yOff = ((S32)ModeDesiredY - IMAGE_AUTHORED_HEIGHT) / 2;

        if (!Image_LoadCentered(tmpFilePath, screen, numpcx))
            TheEndCheckFile(tmpFilePath);

        PaletteSync(PalettePcx);

        /* The reveal below only copies the centred PCX region from <screen>
           into Log, so clear Log to black first or the widescreen sidebars
           show stale pixels under the new palette. At 640x480 the final frame
           is unchanged (the reveal fills the whole frame); the un-revealed
           bands now wipe in from black instead of the previous frame. */
        memset(Log, 0, (size_t)ModeDesiredX * (size_t)ModeDesiredY);

        for (y = 0; y <= IMAGE_AUTHORED_HEIGHT - 1; y++) {
            CopyBlock(xOff, yOff + y, xOff + IMAGE_AUTHORED_WIDTH - 1, yOff + y + 4,
                      screen, xOff, yOff + y, Log);
            y += 5;

            for (x = y; x <= IMAGE_AUTHORED_HEIGHT - 1; x++) {
                CopyBlock(xOff, yOff + y, xOff + IMAGE_AUTHORED_WIDTH - 1, yOff + y,
                          screen, xOff, yOff + x, Log);
            }

            BoxStaticAdd(0, yOff + y - 5, (S32)ModeDesiredX - 1, (S32)ModeDesiredY - 1);
            WaitVideoSync();
            BoxUpdate();
        }
    }; break;
    }

    if (flagbcl) {
        WaitNoInput();
        WaitInput();
        FadeToBlack(PalettePcx);
        RestoreTimer();
        AffScene(AFF_ALL_FLIP);
        FlagFade = TRUE;
    } else
        RestoreTimer();

    Dial_Letterbox = memoDialLetterbox;
}

/*──────────────────────────────────────────────────────────────────────────*/
void EffectPcxMess(U8 numpcx, U8 *screen, U8 effect, S32 mess) {
#ifdef CDROM
    S32 memoflagdisplaytext;
#endif

    /* PCX cinematic with a dialog message overlay (scripted scene-message
       opcodes, end-of-island stills, etc.). Letterbox the dialog so it stays
       inside the centred PCX picture instead of bleeding into the black
       sidebars at wider framebuffers. Encompasses EffectPcx + Dial; AffScene
       at the end clears the cinematic and the restore returns to gameplay
       dialog framing. */
    S32 memoDialLetterbox = Dial_Letterbox;
    Dial_Letterbox = 1;

    SaveTimer();

    EffectPcx(numpcx, screen, effect, FALSE);

    FirstTime = AFF_OBJETS_FLIP;
    FlagFade = FALSE;

#ifdef CDROM
    memoflagdisplaytext = FlagDisplayText;
    FlagDisplayText = TRUE;
#endif

    HQ_StopSample();

    Dial(mess, FALSE);

    RestartRainSample = TRUE;

#ifdef CDROM
    FlagDisplayText = memoflagdisplaytext;
#endif

    FadeToBlack(PalettePcx);
    RestoreTimer();
    FlagFade = FALSE;
    AffScene(AFF_ALL_FLIP);
    FlagFade = TRUE;

    Dial_Letterbox = memoDialLetterbox;
}

/*══════════════════════════════════════════════════════════════════════════*
     █    ██▄ █ ▀▀█▀▀ █▀▀▀█ █▀▀▀█ █▀▀▀▄ █   █ █▀▀▀▀ ▀▀█▀▀  █    █▀▀▀█ ██▄ █
     ██   ██▀██   ██  ██▀█▀ ██  █ ██  █ ██  █ ██      ██   ██   ██  █ ██▀██
     ▀▀   ▀▀  ▀   ▀▀  ▀▀  ▀ ▀▀▀▀▀ ▀▀▀▀  ▀▀▀▀▀ ▀▀▀▀▀   ▀▀   ▀▀   ▀▀▀▀▀ ▀▀  ▀
 *══════════════════════════════════════════════════════════════════════════*/
/*──────────────────────────────────────────────────────────────────────────*/

void Introduction(void) {
    PlayAcf("INTRO");
    FlagPlayAcf = FALSE;
}
