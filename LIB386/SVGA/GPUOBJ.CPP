#include <SVGA/GPUOBJ.H>

#include <SVGA/CLIP.H>
#include <SVGA/SCREEN.H>

#include <stdlib.h>
#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

// --- Public state ------------------------------------------------------------
S32 GpuObjEnabled = FALSE;
S32 GpuObjAvailable = FALSE;

T_GPUOBJ_VERTEX *GpuObjVerts = NULL;
U32 GpuObjTargetHintW = 0;
U8 *GpuObjAtlas = NULL;
T_GPUOBJ_LIGHT GpuObjLights[GPUOBJ_MAX_LIGHTS];
U32 GpuObjNbLights = 0;
S32 GpuObjLightsEnabled = TRUE;
S32 GpuObjPixelFilter = TRUE;
S32 GpuObjDeband = TRUE;
S32 GpuObjSpecular = TRUE;
S32 GpuObjUiScale = TRUE;
S32 GpuObjFire = TRUE;
S32 GpuObjGlow = TRUE;
S32 GpuObjShadows = TRUE;
S32 GpuObjSunlight = TRUE;
float GpuObjSunDir[3] = {0.0f, 1.0f, 0.0f};
float GpuObjUpDir[3] = {0.0f, 1.0f, 0.0f};
S32 GpuObjSunExterior = FALSE;
float GpuObjFocal = 1.0f;
T_GPUOBJ_VERTEX GpuObjShadowVerts[GPUOBJ_SHADOW_CAPACITY];
U32 GpuObjNbShadowVerts = 0;

void GpuObj_ClearShadows(void) {
    GpuObjNbShadowVerts = 0;
}

T_GPUOBJ_VERTEX *GpuObj_AllocShadowVerts(U32 n) {
    if (GpuObjNbShadowVerts + n > GPUOBJ_SHADOW_CAPACITY) {
        return NULL;
    }
    T_GPUOBJ_VERTEX *v = GpuObjShadowVerts + GpuObjNbShadowVerts;
    GpuObjNbShadowVerts += n;
    return v;
}

T_GPUOBJ_VERTEX *GpuObj_AllocShadow(void) {
    return GpuObj_AllocShadowVerts(GPUOBJ_SHADOW_VERTS);
}
S32 GpuObjLampModel = FALSE;
const U8 *GpuObjLampPalette = NULL;

const U8 *GpuObjFirePage = NULL;
T_GPUOBJ_RECT GpuObjFireRects[GPUOBJ_MAX_FIRES];
U32 GpuObjNbFires = 0;
S32 GpuObjAtlasDirty = FALSE;
U32 GpuObjTargetHintH = 0;

// --- Private state -----------------------------------------------------------
/* Tag planes shadow full-frame pixel buffers: Log always, Screen and any
   buffer a tagged frame was copied into (ScreenAux, BufSpeak, ...) on demand. */
#define MAX_PLANES 6
#define MAX_DRAWS 8192

typedef struct {
    const void *Buffer;
    U32 *Tag;
    U8 *Snap;
    S32 AnyTag;
    S32 X0, Y0, X1, Y1; ///< half-open box holding every tag, valid while AnyTag
} T_PLANE;

static T_PLANE s_planes[MAX_PLANES];
static U32 s_planeW = 0;
static U32 s_planeH = 0;

static U8 *s_before = NULL;
static S32 s_snapX0, s_snapY0, s_snapX1, s_snapY1;
static S32 s_snapValid = FALSE;

/* Draws in id order; ids only grow, so a binary search finds one. */
static T_GPUOBJ_DRAW s_draws[MAX_DRAWS];
static U8 s_live[MAX_DRAWS];
static const T_GPUOBJ_DRAW *s_presentList[MAX_DRAWS];
static U32 s_nbDraws = 0;
static U32 s_nbVerts = 0;
static S32 s_drawOpen = FALSE;
static U32 s_nextId = 1;
static S32 s_groupOpen = FALSE;
static U32 s_groupId = 0;
static S32 s_sceneDepth = FALSE;

// --- Private functions -------------------------------------------------------
static void FreePlane(T_PLANE *plane) {
    free(plane->Tag);
    free(plane->Snap);
    plane->Buffer = NULL;
    plane->Tag = NULL;
    plane->Snap = NULL;
    plane->AnyTag = FALSE;
}

static void FreePlanes(void) {
    for (int p = 0; p < MAX_PLANES; p++) {
        FreePlane(&s_planes[p]);
    }
    free(s_before);
    s_before = NULL;
    s_planeW = 0;
    s_planeH = 0;
}

/* The planes follow Log's size; a resolution switch drops every tag. */
static S32 EnsurePlanes(void) {
    if (ModeDesiredX == 0 || ModeDesiredY == 0) {
        return FALSE;
    }
    if (s_before != NULL && s_planeW == ModeDesiredX && s_planeH == ModeDesiredY) {
        return TRUE;
    }
    FreePlanes();
    s_before = (U8 *)malloc((size_t)ModeDesiredX * (size_t)ModeDesiredY);
    if (s_before == NULL) {
        return FALSE;
    }
    s_planeW = ModeDesiredX;
    s_planeH = ModeDesiredY;
    return TRUE;
}

/* Plane shadowing buffer; created when create is set and a slot is free. */
static T_PLANE *PlaneOf(const void *buffer, S32 create) {
    if (buffer == NULL || s_before == NULL) {
        return NULL;
    }
    T_PLANE *freeSlot = NULL;
    for (int p = 0; p < MAX_PLANES; p++) {
        if (s_planes[p].Buffer == buffer) {
            return &s_planes[p];
        }
        if (s_planes[p].Buffer == NULL && freeSlot == NULL) {
            freeSlot = &s_planes[p];
        }
    }
    if (!create || freeSlot == NULL) {
        return NULL;
    }
    size_t n = (size_t)s_planeW * (size_t)s_planeH;
    freeSlot->Tag = (U32 *)calloc(n, sizeof(U32));
    freeSlot->Snap = (U8 *)calloc(n, sizeof(U8));
    if (freeSlot->Tag == NULL || freeSlot->Snap == NULL) {
        FreePlane(freeSlot);
        return NULL;
    }
    freeSlot->Buffer = buffer;
    return freeSlot;
}

static void GrowBox(T_PLANE *plane, S32 x0, S32 y0, S32 x1, S32 y1) {
    if (!plane->AnyTag) {
        plane->X0 = x0;
        plane->Y0 = y0;
        plane->X1 = x1;
        plane->Y1 = y1;
        plane->AnyTag = TRUE;
        return;
    }
    if (x0 < plane->X0)
        plane->X0 = x0;
    if (y0 < plane->Y0)
        plane->Y0 = y0;
    if (x1 > plane->X1)
        plane->X1 = x1;
    if (y1 > plane->Y1)
        plane->Y1 = y1;
}

static S32 ClipBoxToPlanes(S32 *x0, S32 *y0, S32 *x1, S32 *y1) {
    if (*x0 < 0)
        *x0 = 0;
    if (*y0 < 0)
        *y0 = 0;
    if (*x1 > (S32)s_planeW)
        *x1 = (S32)s_planeW;
    if (*y1 > (S32)s_planeH)
        *y1 = (S32)s_planeH;
    return (*x1 > *x0) && (*y1 > *y0);
}

/* First draw carrying id (a group's draws share one), -1 if none. */
static S32 FindDraw(U32 id) {
    S32 lo = 0;
    S32 hi = (S32)s_nbDraws - 1;
    S32 found = -1;
    while (lo <= hi) {
        S32 mid = (lo + hi) / 2;
        if (s_draws[mid].Id >= id) {
            if (s_draws[mid].Id == id) {
                found = mid;
            }
            hi = mid - 1;
        } else {
            lo = mid + 1;
        }
    }
    return found;
}

static void MarkDrawsLive(S32 first) {
    const U32 id = s_draws[first].Id;
    for (U32 i = (U32)first; i < s_nbDraws && s_draws[i].Id == id; i++) {
        s_live[i] = 1;
    }
}

/* Marks in s_live every draw a tag of the plane references. */
static void MarkLive(const T_PLANE *plane) {
    if (plane->Buffer == NULL || !plane->AnyTag) {
        return;
    }
    U32 lastId = 0;
    for (S32 y = plane->Y0; y < plane->Y1; y++) {
        const U32 *row = plane->Tag + (size_t)y * s_planeW;
        for (S32 x = plane->X0; x < plane->X1; x++) {
            U32 id = row[x];
            if (id == 0 || id == lastId) {
                continue;
            }
            lastId = id;
            S32 index = FindDraw(id);
            if (index >= 0) {
                MarkDrawsLive(index);
            }
        }
    }
}

/* Drops every draw no pixel of any plane references and packs the vertices. */
static void Compact(void) {
    memset(s_live, 0, s_nbDraws);
    for (int p = 0; p < MAX_PLANES; p++) {
        MarkLive(&s_planes[p]);
    }
    if (s_drawOpen && s_nbDraws > 0) {
        s_live[s_nbDraws - 1] = 1;
    }
    if (s_groupOpen) {
        for (U32 i = 0; i < s_nbDraws; i++) {
            if (s_draws[i].Id == s_groupId) {
                s_live[i] = 1;
            }
        }
    }

    U32 keptDraws = 0;
    U32 keptVerts = 0;
    for (U32 i = 0; i < s_nbDraws; i++) {
        if (!s_live[i]) {
            continue;
        }
        T_GPUOBJ_DRAW *draw = &s_draws[i];
        if (draw->First != keptVerts && draw->Count > 0) {
            memmove(GpuObjVerts + keptVerts, GpuObjVerts + draw->First, draw->Count * sizeof(T_GPUOBJ_VERTEX));
        }
        draw->First = keptVerts;
        keptVerts += draw->Count;
        if (keptDraws != i) {
            s_draws[keptDraws] = *draw;
        }
        keptDraws++;
    }
    s_nbDraws = keptDraws;
    s_nbVerts = keptVerts;
}

/* Ids never repeat while a tag could still name the old draw. */
static void ResetAll(void) {
    for (int p = 0; p < MAX_PLANES; p++) {
        if (s_planes[p].Tag != NULL) {
            memset(s_planes[p].Tag, 0, (size_t)s_planeW * s_planeH * sizeof(U32));
        }
        s_planes[p].AnyTag = FALSE;
    }
    s_nbDraws = 0;
    s_nbVerts = 0;
    s_nextId = 1;
}

// --- Capture -----------------------------------------------------------------
S32 GpuObj_Active(void) {
    return GpuObjAvailable && GpuObjEnabled && Log != NULL;
}

static void Tag(U32 id);

T_GPUOBJ_DRAW *GpuObj_BeginDraw(void) {
    if (s_drawOpen) {
        return NULL;
    }
    if (GpuObjVerts == NULL) {
        GpuObjVerts = (T_GPUOBJ_VERTEX *)malloc(GPUOBJ_MAX_VERTS * sizeof(T_GPUOBJ_VERTEX));
        if (GpuObjVerts == NULL) {
            return NULL;
        }
    }
    if (s_nextId >= GPUOBJ_MAX_IDS) {
        ResetAll();
    }
    if (s_nbDraws >= MAX_DRAWS) {
        Compact();
        if (s_nbDraws >= MAX_DRAWS) {
            return NULL;
        }
    }

    T_GPUOBJ_DRAW *draw = &s_draws[s_nbDraws++];
    memset(draw, 0, sizeof(*draw) - sizeof(draw->Logical));
    draw->Id = s_groupOpen ? s_groupId : s_nextId++;
    draw->Scene = s_sceneDepth;
    draw->First = s_nbVerts;
    s_drawOpen = TRUE;
    return draw;
}

T_GPUOBJ_VERTEX *GpuObj_AllocVerts(U32 n) {
    if (!s_drawOpen) {
        return NULL;
    }
    if (s_nbVerts + n > GPUOBJ_MAX_VERTS) {
        Compact();
        if (s_nbVerts + n > GPUOBJ_MAX_VERTS) {
            return NULL;
        }
    }
    T_GPUOBJ_VERTEX *v = GpuObjVerts + s_nbVerts;
    s_nbVerts += n;
    s_draws[s_nbDraws - 1].Count += n;
    return v;
}

void GpuObj_SnapshotBegin(S32 x0, S32 y0, S32 x1, S32 y1) {
    if (s_groupOpen) {
        return; /* the group's own snapshot covers it */
    }
    s_snapValid = FALSE;
    if (!EnsurePlanes()) {
        return;
    }

    /* Inclusive clip window to half-open box. */
    if (x0 < ClipXMin)
        x0 = ClipXMin;
    if (y0 < ClipYMin)
        y0 = ClipYMin;
    if (x1 > ClipXMax + 1)
        x1 = ClipXMax + 1;
    if (y1 > ClipYMax + 1)
        y1 = ClipYMax + 1;
    if (!ClipBoxToPlanes(&x0, &y0, &x1, &y1)) {
        return;
    }

    const U8 *log = (const U8 *)Log;
    for (S32 y = y0; y < y1; y++) {
        memcpy(s_before + y * s_planeW + x0, log + y * s_planeW + x0, (size_t)(x1 - x0));
    }
    s_snapX0 = x0;
    s_snapY0 = y0;
    s_snapX1 = x1;
    s_snapY1 = y1;
    s_snapValid = TRUE;
}

void GpuObj_EndDraw(void) {
    U32 id = 0;
    if (s_drawOpen) {
        s_drawOpen = FALSE;
        T_GPUOBJ_DRAW *draw = &s_draws[s_nbDraws - 1];
        if (draw->Count > 0) {
            id = draw->Id;
        } else {
            s_nbDraws--;
        }
    }

    if (s_groupOpen || !s_snapValid) {
        return;
    }
    Tag(id);
}

void GpuObj_SetFireRects(const U8 *page, const T_GPUOBJ_RECT *rects, U32 count) {
    GpuObjFirePage = page;
    GpuObjNbFires = count < GPUOBJ_MAX_FIRES ? count : GPUOBJ_MAX_FIRES;
    for (U32 i = 0; i < GpuObjNbFires; i++) {
        GpuObjFireRects[i] = rects[i];
    }
}

S32 GpuObj_FireAt(S32 x, S32 y) {
    for (U32 i = 0; i < GpuObjNbFires; i++) {
        const T_GPUOBJ_RECT *r = &GpuObjFireRects[i];
        if (x >= r->X && x < r->X + r->W && y >= r->Y && y < r->Y + r->H) {
            return (S32)i;
        }
    }
    return -1;
}

void GpuObj_ClearLights(void) {
    GpuObjNbLights = 0;
}

void GpuObj_AddLight(float x, float y, float z, float radius, float r, float g, float b, float intensity) {
    if (!GpuObjLightsEnabled || GpuObjNbLights >= GPUOBJ_MAX_LIGHTS || radius <= 0.0f || intensity <= 0.0f) {
        return;
    }
    T_GPUOBJ_LIGHT *light = &GpuObjLights[GpuObjNbLights++];
    light->Pos[0] = x;
    light->Pos[1] = y;
    light->Pos[2] = z;
    light->Pos[3] = radius;
    light->Color[0] = r;
    light->Color[1] = g;
    light->Color[2] = b;
    light->Color[3] = intensity;
}

U8 *GpuObj_AtlasReset(void) {
    size_t bytes = (size_t)GPUOBJ_ATLAS_SIZE * GPUOBJ_ATLAS_SIZE * 2;
    if (GpuObjAtlas == NULL) {
        GpuObjAtlas = (U8 *)malloc(bytes);
        if (GpuObjAtlas == NULL) {
            return NULL;
        }
    }
    memset(GpuObjAtlas, 0, bytes);
    GpuObjAtlasDirty = TRUE;
    return GpuObjAtlas;
}

void GpuObj_OnCopyMask(const void *src, S32 x0, S32 y0, S32 x1, S32 y1) {
    T_PLANE *sp = PlaneOf(src, FALSE);
    if (sp == NULL || !sp->AnyTag || Log == NULL) {
        return;
    }
    T_PLANE *dp = PlaneOf(Log, TRUE);
    if (dp == NULL) {
        return;
    }
    x1++;
    y1++;
    if (!ClipBoxToPlanes(&x0, &y0, &x1, &y1)) {
        return;
    }
    const U8 *log = (const U8 *)Log;
    const U8 *from = (const U8 *)src;
    S32 any = FALSE;
    for (S32 y = y0; y < y1; y++) {
        size_t row = (size_t)y * s_planeW;
        for (S32 x = x0; x < x1; x++) {
            size_t i = row + (size_t)x;
            if (log[i] == from[i] && sp->Tag[i] != 0) {
                dp->Tag[i] = sp->Tag[i];
                dp->Snap[i] = sp->Snap[i];
                any = TRUE;
            }
        }
    }
    if (any) {
        GrowBox(dp, x0, y0, x1, y1);
    }
}

void GpuObj_SetSceneDepth(S32 scene) {
    s_sceneDepth = scene;
}

void GpuObj_BeginGroup(void) {
    if (s_groupOpen || s_drawOpen) {
        return;
    }
    if (s_nextId >= GPUOBJ_MAX_IDS) {
        ResetAll();
    }
    GpuObj_SnapshotBegin(0, 0, (S32)ModeDesiredX, (S32)ModeDesiredY);
    s_groupId = s_nextId++;
    s_groupOpen = TRUE;

    /* A group opens on a freshly cleared frame. The clear wrote Log directly,
       so drop the tags it made stale: a pixel the group leaves at the clear
       colour must not keep pointing at the previous frame's scenery. */
    if (s_snapValid) {
        T_PLANE *plane = PlaneOf(Log, FALSE);
        if (plane != NULL && plane->AnyTag) {
            for (S32 y = s_snapY0; y < s_snapY1; y++) {
                memset(plane->Tag + (size_t)y * s_planeW + s_snapX0, 0, (size_t)(s_snapX1 - s_snapX0) * sizeof(U32));
            }
        }
    }
}

void GpuObj_EndGroup(void) {
    if (!s_groupOpen) {
        return;
    }
    s_groupOpen = FALSE;

    S32 any = FALSE;
    for (U32 i = 0; i < s_nbDraws; i++) {
        if (s_draws[i].Id == s_groupId) {
            any = TRUE;
            break;
        }
    }
    if (s_snapValid) {
        Tag(any ? s_groupId : 0);
    }
}

void GpuObj_RefreshTags(S32 x0, S32 y0, S32 x1, S32 y1) {
    T_PLANE *plane = PlaneOf(Log, FALSE);
    if (plane == NULL || !plane->AnyTag) {
        return;
    }
    x1++;
    y1++;
    if (!ClipBoxToPlanes(&x0, &y0, &x1, &y1)) {
        return;
    }
    const U8 *log = (const U8 *)Log;
    for (S32 y = y0; y < y1; y++) {
        size_t row = (size_t)y * s_planeW;
        for (S32 x = x0; x < x1; x++) {
            if (plane->Tag[row + x] != 0) {
                plane->Snap[row + x] = log[row + x];
            }
        }
    }
}

/* Tags every pixel that changed since the snapshot with id, 0 for a body the
   GPU could not take. */
static void Tag(U32 id) {
    s_snapValid = FALSE;

    T_PLANE *plane = PlaneOf(Log, TRUE);
    if (plane == NULL) {
        return;
    }
    const U8 *log = (const U8 *)Log;
    U32 *tag = plane->Tag;
    U8 *snap = plane->Snap;

    for (S32 y = s_snapY0; y < s_snapY1; y++) {
        U32 row = (U32)y * s_planeW;
        for (S32 x = s_snapX0; x < s_snapX1; x++) {
            U32 i = row + (U32)x;
            if (log[i] != s_before[i]) {
                /* id 0 records a body the GPU could not take: its pixels must
                   show the software colour, not an older body's. */
                tag[i] = id;
                snap[i] = log[i];
            }
        }
    }
    if (id != 0) {
        GrowBox(plane, s_snapX0, s_snapY0, s_snapX1, s_snapY1);
    }
}

// --- Pixel copies ------------------------------------------------------------
static void CopyPlaneBlock(T_PLANE *dp, const T_PLANE *sp, S32 sx, S32 sy, S32 dx, S32 dy, S32 w, S32 h) {
    for (S32 y = 0; y < h; y++) {
        memmove(dp->Tag + (dy + y) * s_planeW + dx, sp->Tag + (sy + y) * s_planeW + sx, (size_t)w * sizeof(U32));
        memmove(dp->Snap + (dy + y) * s_planeW + dx, sp->Snap + (sy + y) * s_planeW + sx, (size_t)w);
    }
    if (!sp->AnyTag) {
        return;
    }
    /* Only the part of the block where the source holds tags. */
    S32 x0 = sp->X0 - sx + dx;
    S32 y0 = sp->Y0 - sy + dy;
    S32 x1 = sp->X1 - sx + dx;
    S32 y1 = sp->Y1 - sy + dy;
    if (x0 < dx)
        x0 = dx;
    if (y0 < dy)
        y0 = dy;
    if (x1 > dx + w)
        x1 = dx + w;
    if (y1 > dy + h)
        y1 = dy + h;
    if (x1 > x0 && y1 > y0) {
        GrowBox(dp, x0, y0, x1, y1);
    }
}

void GpuObj_OnCopyBlock(const void *dst, const void *src, S32 sx, S32 sy, S32 dx, S32 dy, S32 w, S32 h) {
    if (s_before == NULL || dst == NULL || w <= 0 || h <= 0) {
        return;
    }
    if (sx < 0 || sy < 0 || dx < 0 || dy < 0) {
        return;
    }
    if (sx + w > (S32)s_planeW)
        w = (S32)s_planeW - sx;
    if (dx + w > (S32)s_planeW)
        w = (S32)s_planeW - dx;
    if (sy + h > (S32)s_planeH)
        h = (S32)s_planeH - sy;
    if (dy + h > (S32)s_planeH)
        h = (S32)s_planeH - dy;
    if (w <= 0 || h <= 0) {
        return;
    }

    T_PLANE *sp = PlaneOf(src, FALSE);
    S32 fullFrame = (sx == 0 && sy == 0 && dx == 0 && dy == 0 && w == (S32)s_planeW && h == (S32)s_planeH);

    if (sp != NULL && sp->AnyTag) {
        /* A whole tagged frame parked in another buffer is worth following;
           partial blits only land in Screen or buffers already shadowed. */
        T_PLANE *dp = PlaneOf(dst, fullFrame || dst == Screen);
        if (dp != NULL && dp != sp) {
            if (fullFrame) {
                dp->AnyTag = FALSE;
            }
            CopyPlaneBlock(dp, sp, sx, sy, dx, dy, w, h);
        }
        return;
    }

    T_PLANE *dp = PlaneOf(dst, FALSE);
    if (dp == NULL || dp == sp || !dp->AnyTag) {
        return;
    }
    if (fullFrame) {
        dp->AnyTag = FALSE;
    }
    for (S32 y = 0; y < h; y++) {
        memset(dp->Tag + (dy + y) * s_planeW + dx, 0, (size_t)w * sizeof(U32));
    }
}

void GpuObj_OnCopyArea(const void *dst, const void *src, S32 x0, S32 y0, S32 x1, S32 y1) {
    if (s_before == NULL) {
        return;
    }
    if (!ClipBoxToPlanes(&x0, &y0, &x1, &y1)) {
        return;
    }
    GpuObj_OnCopyBlock(dst, src, x0, y0, x0, y0, x1 - x0, y1 - y0);
}

// --- Present -----------------------------------------------------------------
U32 GpuObj_BuildPresent(float *out, const T_GPUOBJ_DRAW *const **draws, S32 *box) {
    *draws = s_presentList;
    if (Log == NULL || s_planeW != ModeDesiredX || s_planeH != ModeDesiredY || s_nbDraws == 0) {
        return 0;
    }
    T_PLANE *plane = PlaneOf(Log, FALSE);
    if (plane == NULL || !plane->AnyTag) {
        return 0;
    }

    const U8 *log = (const U8 *)Log;
    U32 lastId = 0;
    S32 lastIndex = -1;
    U32 tagged = 0;

    memset(s_live, 0, s_nbDraws);
    for (S32 y = plane->Y0; y < plane->Y1; y++) {
        size_t row = (size_t)y * s_planeW;
        const U32 *tag = plane->Tag + row;
        const U8 *snap = plane->Snap + row;
        const U8 *pixels = log + row;
        float *dst = out + row;
        for (S32 x = plane->X0; x < plane->X1; x++) {
            U32 id = tag[x];
            if (id == 0 || snap[x] != pixels[x]) {
                dst[x] = 0.0f;
                continue;
            }
            tagged++;
            if (id != lastId) {
                lastId = id;
                lastIndex = FindDraw(id);
                if (lastIndex >= 0) {
                    MarkDrawsLive(lastIndex);
                }
            }
            dst[x] = lastIndex >= 0 ? (float)(s_draws[lastIndex].Scene ? (id | GPUOBJ_SCENE_ID_BIT) : id) : 0.0f;
        }
    }
    if (tagged == 0) {
        plane->AnyTag = FALSE;
        return 0;
    }

    box[0] = plane->X0;
    box[1] = plane->Y0;
    box[2] = plane->X1;
    box[3] = plane->Y1;

    U32 count = 0;
    for (U32 i = 0; i < s_nbDraws; i++) {
        if (s_live[i]) {
            s_presentList[count++] = &s_draws[i];
        }
    }
    return count;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
