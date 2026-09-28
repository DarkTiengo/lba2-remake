#include "DEFINES.H"
#include "../C_EXTERN.H" /* Island, PtrPal */
#include "../CONSOLE/CONSOLE.H"
#include "TREES.H"

#include <3D/LPROJ.H>
#include <SVGA/GPUTREE.H>

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

/* By the island's model file, not its number: Citadel Island after the storm
   (citabau) is the same island with models of its own, numbered differently. */
typedef struct {
    const char *File;
    S32 Body;
    U32 Foliage; ///< palette banks that are leaves, GPUTREE_FOLIAGE_TEXTURED
} T_TREE_MODEL;

#define BANK(n) (1u << (n))
#define TEXTURED GPUTREE_FOLIAGE_TEXTURED

static const T_TREE_MODEL kTreeModels[] = {
    {"citadel", 0, BANK(8)},    /* round tree */
    {"citadel", 1, BANK(7)},    /* tall tree */
    {"citadel", 4, TEXTURED},   /* palm */
    {"citadel", 6, BANK(7)},    /* bush */
    {"citadel", 25, BANK(8)},   /* young firs */
    {"citabau", 0, BANK(8)},    /* round tree */
    {"citabau", 1, BANK(7)},    /* tall tree */
    {"citabau", 4, TEXTURED},   /* palm */
    {"citabau", 6, BANK(7)},    /* bush */
    {"citabau", 7, BANK(8)},    /* round tree */
    {"citabau", 9, BANK(7)},    /* tall tree */
    {"citabau", 12, BANK(7)},   /* bush */
    {"citabau", 33, BANK(8)},   /* young firs */
    {"desert", 2, BANK(7)},     /* bush */
    {"desert", 9, 0},           /* palm trunk, under crown 10 */
    {"desert", 10, TEXTURED},   /* palm crown */
    {"desert", 11, 0},          /* palm trunk, under crown 12 */
    {"desert", 12, TEXTURED},   /* palm crown */
    {"desert", 74, BANK(8)},    /* flowering plant */
    {"mosquibe", 45, TEXTURED}, /* reed */
    {"mosquibe", 46, TEXTURED}, /* reed */
    {"mosquibe", 49, TEXTURED}, /* bulb flowers */
    {"mosquibe", 50, TEXTURED}, /* bulb flowers */
    {"knartas", 62, TEXTURED},  /* leafy plant */
};

/* IslandName is the file's base name as the game asked for it, whatever its
   case on disk; an extension or a directory in front is not part of it. */
static S32 SameFile(const char *file, const char *islandName) {
    const char *slash = strrchr(islandName, '/');
    const char *name = slash != NULL ? slash + 1 : islandName;
    for (; *file != 0; file++, name++) {
        if (tolower((unsigned char)*file) != tolower((unsigned char)*name)) {
            return FALSE;
        }
    }
    return *name == 0 || *name == '.';
}

S32 Trees_Find(S32 body, U32 *foliage) {
    for (S32 i = 0; i < (S32)(sizeof(kTreeModels) / sizeof(kTreeModels[0])); i++) {
        if (kTreeModels[i].Body == body && SameFile(kTreeModels[i].File, IslandName)) {
            if (foliage != NULL) {
                *foliage = kTreeModels[i].Foliage;
            }
            return TRUE;
        }
    }
    return FALSE;
}

/*──────────────────────────────────────────────────────────────────────────*/
/* `decors`: what the current cube's decor is made of, to tell the trees. */

static S32 s_polyKinds[4];  ///< solid, flat, gouraud/dither, textured
static S32 s_bankPolys[16]; ///< untextured polygons by palette bank

static void CountPoly(U32 type, void *poly) {
    const U32 polyType = type >> 2;
    const U16 color = ((const U16 *)poly)[4];
    if ((type & 2) != 0 || polyType > POLY_DITHER_TABLE) {
        s_polyKinds[3]++;
        return;
    }
    s_polyKinds[polyType == POLY_SOLID ? 0 : (polyType == POLY_FLAT ? 1 : 2)]++;
    s_bankPolys[(color >> 4) & 15]++;
}

static void cmd_decors(int argc, const char *argv[]) {
    (void)argc;
    (void)argv;
    if (!CubeMode || ListDecors == NULL) {
        Console_Print("decors: exterior only");
        return;
    }
    S32 seen[1024];
    S32 nbSeen = 0;
    Console_Print("decors: island %d (%s), %d objects", (int)Island, IslandName, (int)NbObjDecors);
    for (S32 i = 0; i < NbObjDecors; i++) {
        const T_DECORS *d = &ListDecors[i];
        const S32 body = (S32)(d->Body & 0xFFFF);
        S32 sx = -1, sy = -1;
        LongWorldRotatePoint(d->Xworld, d->Yworld, d->Zworld);
        if (LongProjectPoint(X0, Y0, Z0)) {
            sx = Xp;
            sy = Yp;
        }
        Console_Print("  obj %d body %d at (%d,%d,%d) screen (%d,%d)%s", (int)i, (int)body, (int)d->Xworld,
                      (int)d->Yworld, (int)d->Zworld, (int)sx, (int)sy,
                      Trees_Find(body, NULL) ? " tree" : "");
        S32 known = FALSE;
        for (S32 k = 0; k < nbSeen; k++) {
            known |= seen[k] == body;
        }
        if (!known && nbSeen < 1024) {
            seen[nbSeen++] = body;
        }
    }
    for (S32 k = 0; k < nbSeen; k++) {
        T_BODY_HEADER *b = (T_BODY_HEADER *)HQR_Get(HQR_Isle_Obj, seen[k]);
        if (b == NULL) {
            continue;
        }
        memset(s_polyKinds, 0, sizeof(s_polyKinds));
        memset(s_bankPolys, 0, sizeof(s_bankPolys));
        U8 *const map = ObjPtrMap;
        ObjPtrMap = ObjTexture;
        BodyWalkPolys(b, CountPoly);
        ObjPtrMap = map;
        char banks[256];
        S32 len = 0;
        banks[0] = 0;
        for (S32 n = 0; n < 16; n++) {
            if (s_bankPolys[n] == 0 || len > 200) {
                continue;
            }
            const U8 *c = PtrPal + (n * 16 + 8) * 3;
            len += snprintf(banks + len, sizeof(banks) - len, " %d:%d(%02x%02x%02x)", (int)n, (int)s_bankPolys[n],
                            c[0], c[1], c[2]);
        }
        Console_Print("  body %d box x %d..%d y %d..%d z %d..%d polys solid %d flat %d gouraud %d tex %d lines %d "
                      "spheres %d banks%s",
                      (int)seen[k], (int)b->XMin, (int)b->XMax, (int)b->YMin, (int)b->YMax, (int)b->ZMin,
                      (int)b->ZMax, (int)s_polyKinds[0], (int)s_polyKinds[1], (int)s_polyKinds[2],
                      (int)s_polyKinds[3], (int)b->NbLines, (int)b->NbSpheres, banks);
    }
}

void Trees_RegisterCommands(void) {
    Console_RegisterCommandEx("decors", cmd_decors, "List the current cube's decor models",
                              "decors",
                              "Each decor object with its model, world position and screen point, then "
                              "each model's box and its polygons by kind and palette bank, the bank's "
                              "middle colour in brackets: what tells a tree's leaves from its trunk.");
}
