#include "TEXTURE_PACK.H"

#include "DIRECTORIES.H"

#include <SDL3/SDL.h>
#include <SVGA/GPUOBJ.H>
#include <SYSTEM/ADELINE.H>
#include <SYSTEM/GPURENDER.H>
#include <SYSTEM/LIMITS.H>
#include <SYSTEM/LOG.H>

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern S32 OffsetAniPoly1;
extern S32 OffsetAniPoly2;
extern S32 OffsetAniPoly3;
extern S32 OffsetAniPoly4;

static char s_island[ADELINE_MAX_PATH] = "";
static U8 *s_ground = NULL;
static U8 *s_objects = NULL;
static U8 *s_skySea = NULL;

static bool SafeIslandName(const char *name, char *out, U32 outSize) {
    if (name == NULL || name[0] == '\0' || out == NULL || outSize < 2) {
        return false;
    }
    U32 n = 0;
    while (name[n] != '\0') {
        const unsigned char c = (unsigned char)name[n];
        if (!(isalnum(c) || c == '_' || c == '-')) {
            return false;
        }
        if (n + 1 >= outSize) {
            return false;
        }
        out[n] = (char)tolower(c);
        n++;
    }
    out[n] = '\0';
    return true;
}

static bool ManifestValid(void) {
    char path[ADELINE_MAX_PATH];
    GetTexturePackPath(path, ADELINE_MAX_PATH, "pack.ini");
    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        Log_Warn("[textures] no 4x pack at %s", path);
        return false;
    }

    S32 format = -1;
    S32 scale = -1;
    char line[256];
    while (fgets(line, sizeof(line), file) != NULL) {
        char key[64];
        S32 value;
        if (sscanf(line, " %63[^=]=%d", key, &value) != 2) {
            continue;
        }
        size_t end = strlen(key);
        while (end > 0 && isspace((unsigned char)key[end - 1])) {
            key[--end] = '\0';
        }
        if (SDL_strcasecmp(key, "Format") == 0) {
            format = value;
        } else if (SDL_strcasecmp(key, "Scale") == 0) {
            scale = value;
        }
    }
    fclose(file);

    if (format != 1 || scale != 4) {
        Log_Warn("[textures] pack.ini requires Format=1 and Scale=4");
        return false;
    }
    return true;
}

static U8 *LoadRgba(const char *leaf) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    char relative[ADELINE_MAX_PATH];
    if (snprintf(relative, sizeof(relative), "islands%s%s%s%s", ADELINE_PATH_SEP, s_island,
                 ADELINE_PATH_SEP, leaf) >= (S32)sizeof(relative)) {
        return NULL;
    }
    char path[ADELINE_MAX_PATH];
    GetTexturePackPath(path, ADELINE_MAX_PATH, relative);
    SDL_Surface *loaded = SDL_LoadPNG(path);
    if (loaded == NULL) {
        return NULL; /* individual pages are optional */
    }
    if (loaded->w != GPURENDER_REPLACEMENT_SIDE || loaded->h != GPURENDER_REPLACEMENT_SIDE) {
        Log_Warn("[textures] %s is %dx%d; expected %dx%d", path, loaded->w, loaded->h,
                 GPURENDER_REPLACEMENT_SIDE, GPURENDER_REPLACEMENT_SIDE);
        SDL_DestroySurface(loaded);
        return NULL;
    }
    SDL_Surface *rgba = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
    SDL_DestroySurface(loaded);
    if (rgba == NULL || !SDL_LockSurface(rgba)) {
        Log_Warn("[textures] unable to decode %s: %s", path, SDL_GetError());
        if (rgba != NULL) {
            SDL_DestroySurface(rgba);
        }
        return NULL;
    }

    const U32 rowBytes = GPURENDER_REPLACEMENT_SIDE * 4;
    U8 *pixels = (U8 *)malloc(rowBytes * GPURENDER_REPLACEMENT_SIDE);
    if (pixels != NULL) {
        for (S32 y = 0; y < GPURENDER_REPLACEMENT_SIDE; y++) {
            memcpy(pixels + y * rowBytes, (const U8 *)rgba->pixels + y * rgba->pitch, rowBytes);
        }
    }
    SDL_UnlockSurface(rgba);
    SDL_DestroySurface(rgba);
    if (pixels == NULL) {
        Log_Warn("[textures] unable to allocate %s", path);
    }
    return pixels;
#else
    (void)leaf;
    return NULL;
#endif
}

static S32 RegisterPage(const char *leaf, U8 *page) {
    if (page == NULL) {
        return 0;
    }
    U8 *pixels = LoadRgba(leaf);
    if (pixels == NULL) {
        return 0;
    }
    const bool ok = GpuRender_SetTextureReplacement(page, pixels, GPURENDER_REPLACEMENT_SIDE,
                                                    GPURENDER_REPLACEMENT_SIDE);
    free(pixels);
    if (!ok) {
        Log_Warn("[textures] could not register %s for %s", leaf, s_island);
        return 0;
    }
    return 1;
}

static S32 RegisterTerrainMaterials(void) {
    U8 *pixels = LoadRgba("terrain.png");
    if (pixels == NULL) {
        return 0;
    }
    const bool ok = GpuRender_SetTerrainMaterials(pixels, GPURENDER_REPLACEMENT_SIDE,
                                                   GPURENDER_REPLACEMENT_SIDE);
    free(pixels);
    if (!ok) {
        Log_Warn("[textures] could not register terrain.png for %s", s_island);
    }
    return ok ? 1 : 0;
}

static void Reload(void) {
    GpuRender_ClearTextureReplacements();
    if (GpuObjTextureDetail != GPUOBJ_TEXTURE_DETAIL_PACK || s_ground == NULL || s_island[0] == '\0') {
        return;
    }
    if (!ManifestValid()) {
        return;
    }
    S32 loaded = 0;
    loaded += RegisterPage("ground.png", s_ground);
    loaded += RegisterPage("objects.png", s_objects);
    loaded += RegisterPage("skysea.png", s_skySea);
    if (strcmp(s_island, "citadel") == 0) {
        loaded += RegisterTerrainMaterials();
    }
    if (loaded > 0) {
        Log_Info("[textures] %s: %d replacement asset%s", s_island, loaded, loaded == 1 ? "" : "s");
        TexturePack_UpdateGroundAnimationMask();
    } else {
        Log_Warn("[textures] pack has no usable pages for %s; using enhanced originals", s_island);
    }
}

void TexturePack_ApplyMode(S32 value) {
    GpuObjTextureDetail = value;
    Reload();
}

void TexturePack_SetIsland(const char *islandName, U8 *ground, U8 *objects, U8 *skySea) {
    char safe[ADELINE_MAX_PATH];
    if (!SafeIslandName(islandName, safe, sizeof(safe))) {
        s_island[0] = '\0';
        s_ground = s_objects = s_skySea = NULL;
        GpuRender_ClearTextureReplacements();
        return;
    }
    strncpy(s_island, safe, sizeof(s_island));
    s_island[sizeof(s_island) - 1] = '\0';
    s_ground = ground;
    s_objects = objects;
    s_skySea = skySea;
    Reload();
}

static void MaskRect(U8 *mask, S32 offset) {
    if (offset < 0 || offset >= 256 * 256) {
        return;
    }
    const S32 x = offset & 255;
    const S32 y = offset >> 8;
    for (S32 row = 0; row < 32 && y + row < 256; row++) {
        const S32 width = x + 32 <= 256 ? 32 : 256 - x;
        memset(mask + (y + row) * 256 + x, 0xFF, width);
    }
}

void TexturePack_UpdateGroundAnimationMask(void) {
    if (GpuObjTextureDetail != GPUOBJ_TEXTURE_DETAIL_PACK || s_ground == NULL) {
        return;
    }
    U8 mask[256 * 256];
    memset(mask, 0, sizeof(mask));
    MaskRect(mask, OffsetAniPoly1);
    MaskRect(mask, OffsetAniPoly2);
    MaskRect(mask, OffsetAniPoly3);
    MaskRect(mask, OffsetAniPoly4);
    GpuRender_SetTextureReplacementMask(s_ground, mask, sizeof(mask));
}
