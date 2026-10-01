#include <OBJECT/AFF_GPU.H>

#include <OBJECT/AFF_OBJ.H>
#include <3D/LROT3D.H>
#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <3D/ROTMAT.H>
#include <POLYGON/POLY.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>
#include <SVGA/GPUTREE.H>
#include <SVGA/SCREEN.H>
#include <SVGA/SCREENXY.H>

#include <math.h>
#include <string.h>

// -----------------------------------------------------------------------------
#ifdef __cplusplus
extern "C" {
#endif

#define MAX_NB_POINTS 550
#define MAX_NB_POLYS 550
#define MAX_LIGHTS (MAX_NB_POINTS + MAX_NB_POLYS)

/* Pixels a sphere or a line may reach past the projected points' box. */
#define SNAPSHOT_MARGIN 48

/* Depth span of one body; beyond it depth only orders within the body. */
/* Bodies and exterior scenery share one depth scale, since z-buffered decors
   and terrain depth-test each other; the sky plane reaches about 70000 units. */
#define PERSPECTIVE_DEPTH_RANGE GPUOBJ_SCENE_DEPTH_RANGE
#define SCENERY_DEPTH_RANGE PERSPECTIVE_DEPTH_RANGE
#define ISO_DEPTH_RANGE 524288.0f

typedef struct {
    U16 OrgGroupe;
    U16 OrgPoint;
    U16 NbPts;
    U16 NbNorm;
} T_GPU_GROUPE;

typedef struct {
    float Sx, Sy; ///< screen position
    float W;      ///< clip w
    float Depth;  ///< normalized depth inside the body's slice
    float Vx, Vy, Vz;
} T_GPU_PROJ;

// --- Engine state read from AFF_OBJ.CPP --------------------------------------
extern T_BODY_HEADER *lBody;
extern TYPE_MAT TabMat[];
extern S32 PosXWr, PosYWr, PosZWr;

// --- Private state -----------------------------------------------------------
static S32 s_pending = FALSE;
static S32 s_capturing = FALSE;
static S32 s_perGroup = FALSE;
static S32 s_nbPoints = 0;
static T_OBJ_POINT s_rot[MAX_NB_POINTS];

static float s_normal[MAX_LIGHTS][4]; ///< xyz, shade scale
static float s_light[3];
static void UpdateSceneLight(void);
static S32 s_inScene = FALSE; ///< between AffGpu_BeginScene and AffGpu_BuildShadows

/* The frame's bodies, kept for their shadows: triangles in scene space (x, y, z,
   then 1 and the disc coordinates for sphere impostors, zeros otherwise). */
#define MAX_CASTERS 1024
#define MAX_CASTER_VERTS 393216
typedef struct {
    float Origin[3];
    U32 First;
    U32 Count;
} T_CASTER;
static T_CASTER s_casters[MAX_CASTERS];
static U32 s_nbCasters = 0;
static float s_casterVerts[MAX_CASTER_VERTS][6];
static U32 s_nbCasterVerts = 0;
static float s_origin[3];

/* Shadows asked for this frame: the body's origin and the ground under it. */
typedef struct {
    float Origin[3];
    float Ground[3];
    float Radius;
    float Tall;
    float Strength; ///< fainter the higher the body jumps
} T_SHADOW_REQUEST;
static T_SHADOW_REQUEST s_requests[GPUOBJ_MAX_SHADOWS];
static U32 s_nbRequests = 0;

/* Lamps and fires of the frame's bodies, which also cast shadows. */
typedef struct {
    float Pos[3];
    float Radius;
    float Weight;
} T_FRAME_LIGHT;
#define MAX_FRAME_LAMPS 32
static T_FRAME_LIGHT s_lamps[MAX_FRAME_LAMPS];
static U32 s_nbLamps = 0;
/* The scenery's lamps (the street lamps among its decor), kept from the frame
   the scenery was last captured in: it is captured again only when the camera
   moves, and the bodies standing under them lost their lamps' shadows on
   every frame it did not. */
static T_FRAME_LIGHT s_sceneryLamps[MAX_FRAME_LAMPS];
static U32 s_nbSceneryLamps = 0;
static U32 s_sceneryGroup = 0;
static T_GPUOBJ_DRAW *s_draw = NULL;

/* The occluder the body being displayed stops light with (AffGpu_DecorOccluder). */
static S32 s_occPending = FALSE;
static U32 s_occFirst = 0, s_occCount = 0;
static float s_occRows[12];

/* Fire-textured polygons of the body being captured: their centres summed. */
static float s_fireSum[3];
static float s_fireRect[4];
static float s_fireMin[3], s_fireMax[3]; ///< scene-space bounds of the fire polygons
static S32 s_fireSlender = 0;            ///< fire polygons far longer than wide (laser bars, beams)
static S32 s_fireCount = 0;

/* Lamp globes of the body being captured: bright, coloured, unshaded solid polygons. */
static float s_glowSum[3];
static float s_glowRgb[3];
static S32 s_glowCount = 0;

// --- Private functions -------------------------------------------------------
static void RotateNormals(const TYPE_MAT *mat, const TYPE_VT16 *src, S32 n, S32 dst, float lightLen) {
    for (S32 i = 0; i < n && dst + i < MAX_LIGHTS; i++) {
        float x = (float)src[i].X;
        float y = (float)src[i].Y;
        float z = (float)src[i].Z;
        float *out = s_normal[dst + i];
        out[0] = x * mat->F.M11 + y * mat->F.M12 + z * mat->F.M13;
        out[1] = x * mat->F.M21 + y * mat->F.M22 + z * mat->F.M23;
        out[2] = x * mat->F.M31 + y * mat->F.M32 + z * mat->F.M33;
        /* LightList: shade = dot(light * FactorLight, normal) / 256 */
        float len = (float)sqrt((double)(x * x + y * y + z * z));
        out[3] = len * lightLen * FactorLight / 256.0f;
    }
}

static void Project(U16 index, T_GPU_PROJ *p) {
    const T_OBJ_POINT *pt = &s_rot[index < s_nbPoints ? index : 0];
    float x = (float)((S32)pt->X + PosXWr);
    float y = (float)((S32)pt->Y + PosYWr);
    float z = (float)((S32)pt->Z + PosZWr);
    float d;

    p->Vx = x;
    p->Vy = y;
    p->Vz = z;

    if (TypeProj == TYPE_3D) {
        float depth = -z;
        if (depth < 1.0f) {
            depth = 1.0f;
        }
        p->Sx = (float)XCentre + x * FRatioX / depth;
        p->Sy = (float)YCentre + y * FRatioX * FRatioY / depth;
        p->W = depth;
        d = (depth - (float)NearClip) / PERSPECTIVE_DEPTH_RANGE;
    } else {
        p->Sx = (float)XCentre + (x - z) * 3.0f / 64.0f;
        p->Sy = (float)YCentre + 1.0f + ((x + z) * 6.0f - y * 15.0f) / 256.0f;
        p->W = 1.0f;
        /* Moving along (1, 0.8, 1) keeps the screen position: that is the view axis. */
        d = 0.5f - (x + 0.8f * y + z) / ISO_DEPTH_RANGE;
    }

    if (d < 0.0f)
        d = 0.0f;
    if (d > 1.0f)
        d = 1.0f;
    p->Depth = d;
}

/* Depth stays local to the body: the presenter places each draw in its slice. */
static void SetClipPos(T_GPUOBJ_VERTEX *v, float sx, float sy, float w, float d) {
    float depth = d * 0.999f;
    float nx = sx * 2.0f / (float)ModeDesiredX - 1.0f;
    float ny = 1.0f - sy * 2.0f / (float)ModeDesiredY;
    v->clip[0] = nx * w;
    v->clip[1] = ny * w;
    v->clip[2] = depth * w;
    v->clip[3] = w;
}

typedef struct {
    float Mode;
    float Color;
    float TexOffset;
    float TexMask;
    float Flags;
} T_GPU_MATERIAL;

static void FillVertex(T_GPUOBJ_VERTEX *v, const T_GPU_PROJ *p, const float *normal, const T_GPU_MATERIAL *m,
                       float u, float t) {
    SetClipPos(v, p->Sx, p->Sy, p->W, p->Depth);
    v->normal[0] = normal ? normal[0] : 0.0f;
    v->normal[1] = normal ? normal[1] : 0.0f;
    v->normal[2] = normal ? normal[2] : 1.0f;
    v->normal[3] = normal ? normal[3] : 0.0f;
    v->light[0] = s_light[0];
    v->light[1] = s_light[1];
    v->light[2] = s_light[2];
    v->light[3] = m->Mode;
    v->vpos[0] = p->Vx;
    v->vpos[1] = p->Vy;
    v->vpos[2] = p->Vz;
    v->vpos[3] = m->Flags;
    v->mat[0] = m->Color;
    v->mat[1] = m->TexOffset;
    v->mat[2] = m->TexMask;
    v->mat[3] = 0.0f;
    v->uv[0] = u;
    v->uv[1] = t;
    v->uv[2] = 0.0f;
    v->uv[3] = 0.0f;
}

static void EmitPolygon(const U16 *points, S32 nbPoints, const float *faceNormal, S32 smooth,
                        const T_GPU_MATERIAL *m, const float uvs[4][2]) {
    S32 nbTris = nbPoints - 2;
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts((U32)(nbTris * 3));
    if (v == NULL) {
        return;
    }

    T_GPU_PROJ proj[4];
    for (S32 i = 0; i < nbPoints; i++) {
        Project(points[i], &proj[i]);
    }

    for (S32 t = 0; t < nbTris; t++) {
        const S32 corners[3] = {0, t + 1, t + 2};
        for (S32 c = 0; c < 3; c++) {
            S32 k = corners[c];
            const float *normal = faceNormal;
            if (smooth) {
                normal = points[k] < MAX_LIGHTS ? s_normal[points[k]] : NULL;
            }
            FillVertex(v++, &proj[k], normal, m, uvs[k][0], uvs[k][1]);
        }
    }
}

/* Screen-aligned quad: spheres as discs, lines as one-pixel ribbons. */
static void EmitQuad(const float sx[4], const float sy[4], const T_GPU_PROJ *depthFrom, const T_GPU_MATERIAL *m,
                     S32 disc) {
    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v == NULL) {
        return;
    }
    static const S32 order[6] = {0, 1, 2, 0, 2, 3};
    static const float localX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
    static const float localY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};

    for (S32 i = 0; i < 6; i++) {
        S32 k = order[i];
        T_GPU_PROJ p = *depthFrom;
        p.Sx = sx[k];
        p.Sy = sy[k];
        FillVertex(v, &p, NULL, m, 0.0f, 0.0f);
        if (disc) {
            v->uv[2] = localX[k];
            v->uv[3] = localY[k];
        }
        v++;
    }
}

/* A bright, coloured palette entry on an unshaded surface is a lamp's globe or
   a lit window; dark or grey ones are ordinary paint. */
static S32 IsGlowColor(U32 color, S32 *rgb) {
    if (!GpuObjGlow || !GpuObjLampModel) {
        return FALSE;
    }
    /* The scene's own palette: the video palette may still be fading in while
       decors are captured. */
    if (GpuObjLampPalette == NULL) {
        return FALSE;
    }
    const U8 *c = GpuObjLampPalette + (color & 255) * 3;
    const S32 r = c[0], g = c[1], b = c[2];
    const S32 mx = r > g ? (r > b ? r : b) : (g > b ? g : b);
    const S32 mn = r < g ? (r < b ? r : b) : (g < b ? g : b);
    const S32 luma = (r * 77 + g * 150 + b * 29) >> 8;
    rgb[0] = r;
    rgb[1] = g;
    rgb[2] = b;
    /* A lamp model's pale, bright, warm glass (its grey metal stays unlit). */
    return luma >= 150 && mx - mn >= 40 && r >= b;
}

static void AddGlow(U32 color, const float *pos) {
    S32 rgb[3];
    if (!IsGlowColor(color, rgb)) {
        return;
    }
    s_glowSum[0] += pos[0];
    s_glowSum[1] += pos[1];
    s_glowSum[2] += pos[2];
    s_glowRgb[0] += (float)rgb[0] / 255.0f;
    s_glowRgb[1] += (float)rgb[1] / 255.0f;
    s_glowRgb[2] += (float)rgb[2] / 255.0f;
    s_glowCount++;
}

/* The leaves of the tree being drawn (GpuObjTreeFoliage), in model space. */
static const T_BODY_HEADER *s_leafBody = NULL;
static float s_leafMin[3], s_leafMax[3];
static S32 s_leafCount = 0;

static void LeafPolygon(U32 type, void *poly) {
    const U16 *w = (const U16 *)poly;
    if (type >= 96 || (type & 2) != 0) {
        return;
    }
    if (!GpuTree_IsFoliage(GpuObjTreeFoliage, (type >> 2) > POLY_DITHER_TABLE, w[4])) {
        return;
    }
    const TYPE_VT16 *points = (const TYPE_VT16 *)((const U8 *)s_leafBody + s_leafBody->OffPoints);
    const S32 nbPoints = (type & 1) != 0 ? 4 : 3;
    for (S32 i = 0; i < nbPoints; i++) {
        if (w[i] >= s_leafBody->NbPoints) {
            continue;
        }
        const float p[3] = {(float)points[w[i]].X, (float)points[w[i]].Y, (float)points[w[i]].Z};
        for (S32 a = 0; a < 3; a++) {
            s_leafMin[a] = s_leafCount == 0 || p[a] < s_leafMin[a] ? p[a] : s_leafMin[a];
            s_leafMax[a] = s_leafCount == 0 || p[a] > s_leafMax[a] ? p[a] : s_leafMax[a];
        }
        s_leafCount++;
    }
}

/* A tree's placement for its draw (GPUTREE.H), in the space its vertices are
   written in: `rot` takes the model's axes there (rows), `origin` is where its
   base lands, `wind` is the island's wind there. */
static void SetTree(T_GPUOBJ_DRAW *draw, T_BODY_HEADER *body, const float rot[9], const float origin[3],
                    const float wind[3]) {
    if (draw == NULL || !GpuTreeEnabled || !GpuObjTreeModel || TypeProj != TYPE_3D) {
        return;
    }
    const S32 map = RepMask;
    PTR_U8 const page = PtrMap;
    s_leafBody = body;
    s_leafCount = 0;
    BodyWalkPolys(body, LeafPolygon);
    PtrMap = page;
    RepMask = map;

    T_GPUTREE tree;
    memset(&tree, 0, sizeof(tree));
    float upLen = 0.0f, windLen = 0.0f;
    for (S32 a = 0; a < 3; a++) {
        tree.Base[a] = origin[a];
        tree.Up[a] = rot[a * 3 + 1];
        tree.Wind[a] = wind[a];
        upLen += tree.Up[a] * tree.Up[a];
        windLen += wind[a] * wind[a];
    }
    if (upLen <= 0.0f || windLen <= 0.0f) {
        return;
    }
    upLen = sqrtf(upLen);
    windLen = sqrtf(windLen);
    for (S32 a = 0; a < 3; a++) {
        tree.Up[a] /= upLen;
        tree.Wind[a] /= windLen;
    }
    if (s_leafCount > 0) {
        float c[3], reach = 0.0f;
        for (S32 a = 0; a < 3; a++) {
            c[a] = 0.5f * (s_leafMin[a] + s_leafMax[a]);
            const float e = 0.5f * (s_leafMax[a] - s_leafMin[a]);
            reach += e * e;
        }
        for (S32 r = 0; r < 3; r++) {
            tree.Centre[r] = origin[r] + rot[r * 3] * c[0] + rot[r * 3 + 1] * c[1] + rot[r * 3 + 2] * c[2];
        }
        tree.Radius = sqrtf(reach);
    }
    tree.Phase = GpuObjTreePhase;
    draw->Tree = TRUE;
    GpuTree_Uniform(&tree, draw->TreeUniform);
}

/* The island's wind in view space: the world's direction through the camera. */
static void ViewWind(float out[3]) {
    const TYPE_MAT *m = &MatriceWorld;
    out[0] = m->F.M11 * GPUTREE_WIND_X + m->F.M13 * GPUTREE_WIND_Z;
    out[1] = m->F.M21 * GPUTREE_WIND_X + m->F.M23 * GPUTREE_WIND_Z;
    out[2] = m->F.M31 * GPUTREE_WIND_X + m->F.M33 * GPUTREE_WIND_Z;
}

static void EmitSphere(const U16 *w, S32 pass) {
    T_GPU_PROJ center;
    Project(w[2], &center);
    if (!pass) {
        const float pos[3] = {center.Vx, center.Vy, center.Vz};
        AddGlow(w[1] & 255, pos);
    }

    float radius;
    if (TypeProj == TYPE_3D) {
        float depth = center.W;
        radius = (float)w[3] * (float)LFactorX / depth;
    } else {
        radius = (float)((S32)w[3] * 34) / 512.0f;
    }
    /* The software disc includes its rim. */
    radius += 0.5f;

    T_GPU_MATERIAL m;
    memset(&m, 0, sizeof(m));
    m.Mode = (float)(pass ? GPUOBJ_MODE_PASS : GPUOBJ_MODE_DISC);
    m.Color = (float)(w[1] & 0xFF);
    S32 rgb[3];
    if (!pass && IsGlowColor(w[1] & 255, rgb)) {
        m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_EMISSIVE);
    }

    const float sx[4] = {center.Sx - radius, center.Sx + radius, center.Sx + radius, center.Sx - radius};
    const float sy[4] = {center.Sy - radius, center.Sy - radius, center.Sy + radius, center.Sy + radius};
    EmitQuad(sx, sy, &center, &m, TRUE);
}

static void EmitLine(const U16 *w) {
    T_GPU_PROJ a, b;
    Project(w[2], &a);
    Project(w[3], &b);

    float dx = b.Sx - a.Sx;
    float dy = b.Sy - a.Sy;
    float len = (float)sqrt((double)(dx * dx + dy * dy));
    if (len < 0.001f) {
        dx = 1.0f;
        dy = 0.0f;
        len = 1.0f;
    }
    float ox = -dy / len;
    float oy = dx / len;

    T_GPU_MATERIAL m;
    memset(&m, 0, sizeof(m));
    m.Mode = (float)GPUOBJ_MODE_PASS;

    const float sx[4] = {a.Sx + ox, b.Sx + ox, b.Sx - ox, a.Sx - ox};
    const float sy[4] = {a.Sy + oy, b.Sy + oy, b.Sy - oy, a.Sy - oy};
    EmitQuad(sx, sy, a.Depth < b.Depth ? &a : &b, &m, FALSE);
}

/* Same mapping as AFF_OBJ.CPP BuildEnvMappedUv: the rotated vertex normal indexes the page. */
static void EnvMappedUv(U16 point, U16 scale, float uv[2]) {
    const TYPE_VT16 *normal = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffNormales) + point;
    const TYPE_MAT *mat = &TabMat[(U16)normal->Grp < 30 ? (U16)normal->Grp : 0];
    float x = (float)normal->X;
    float y = (float)normal->Y;
    float z = (float)normal->Z;
    S32 rx = (S32)lrint(x * mat->F.M11 + y * mat->F.M12 + z * mat->F.M13);
    S32 ry = (S32)lrint(x * mat->F.M21 + y * mat->F.M22 + z * mat->F.M23);
    S32 scaleX = scale & 0xFF;
    S32 scaleY = scale >> 8;
    uv[0] = (float)((((S32)(U16)(rx + 0x4000) * scaleX) >> 4) & 0xFFFF) / 256.0f;
    uv[1] = (float)((((U32)((S32)(U16)(ry + 0x4000) * scaleY)) >> 4) & 0xFFFF) / 256.0f;
}

// --- Interface ---------------------------------------------------------------
void AffGpu_ViewVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth) {
    memset(v, 0, sizeof(*v));
    /* Homogeneous form of the engine projection, so a polygon crossing the
       camera plane (the sky, the sea around the camera) is clipped by the GPU
       instead of folding over: w = depth, x/w and y/w land where
       LongProjectPoint would put them. */
    const float w2 = 2.0f / (float)ModeDesiredX;
    const float h2 = 2.0f / (float)ModeDesiredY;
    v->clip[0] = ((float)XCentre * w2 - 1.0f) * depth + x * FRatioX * w2;
    v->clip[1] = (1.0f - (float)YCentre * h2) * depth - y * FRatioX * FRatioY * h2;
    v->clip[2] = (depth - (float)NearClip) / SCENERY_DEPTH_RANGE * 0.999f * depth;
    v->clip[3] = depth;
    v->vpos[0] = x;
    v->vpos[1] = y;
    v->vpos[2] = -depth;
    v->normal[2] = 1.0f;
}

void AffGpu_SetFocus(S32 x, S32 y, S32 z) {
    if (!GpuObj_Active()) {
        return;
    }
    if (TypeProj == TYPE_3D) {
        const S32 savedX = X0;
        const S32 savedY = Y0;
        const S32 savedZ = Z0;
        LongRotatePoint(&MatriceWorld, x, y, z);
        x = X0;
        y = Y0;
        z = Z0;
        X0 = savedX;
        Y0 = savedY;
        Z0 = savedZ;
    }
    GpuObjFocus[0] = (float)(x - CameraXr);
    GpuObjFocus[1] = (float)(y - CameraYr);
    GpuObjFocus[2] = (float)(z - CameraZr);
    GpuObjFocusValid = TRUE;
}

void AffGpu_AddWorldLight(S32 x, S32 y, S32 z, float radius, float r, float g, float b, float intensity) {
    if (!GpuObj_Active()) {
        return;
    }
    /* Same placement as a body's origin in ObjectDisplay. */
    if (TypeProj == TYPE_3D) {
        /* The rotation's outputs are globals the game may still read. */
        const S32 savedX = X0;
        const S32 savedY = Y0;
        const S32 savedZ = Z0;
        LongRotatePoint(&MatriceWorld, x, y, z);
        x = X0;
        y = Y0;
        z = Z0;
        X0 = savedX;
        Y0 = savedY;
        Z0 = savedZ;
    }
    GpuObj_AddLight((float)(x - CameraXr), (float)(y - CameraYr), (float)(z - CameraZr), radius, r, g, b, intensity);
}

void AffGpu_SaveRotated(S32 nbPoints, S32 perGroup) {
    s_pending = GpuObj_Active() && !AffGpuSuspended;
    if (!s_pending) {
        return;
    }
    if (nbPoints > MAX_NB_POINTS) {
        nbPoints = MAX_NB_POINTS;
    }
    memcpy(s_rot, Obj_ListRotatedPoints, (size_t)nbPoints * sizeof(T_OBJ_POINT));
    s_nbPoints = nbPoints;
    s_perGroup = perGroup;
}

S32 AffGpu_Begin(void) {
    s_capturing = FALSE;
    if (!s_pending || lBody == NULL) {
        return FALSE;
    }
    s_pending = FALSE;
    s_fireCount = 0;
    s_fireSlender = 0;
    s_fireSum[0] = s_fireSum[1] = s_fireSum[2] = 0.0f;
    s_glowCount = 0;
    s_glowSum[0] = s_glowSum[1] = s_glowSum[2] = 0.0f;
    s_glowRgb[0] = s_glowRgb[1] = s_glowRgb[2] = 0.0f;

    UpdateSceneLight();
    s_light[0] = (float)CameraXLight;
    s_light[1] = (float)CameraYLight;
    s_light[2] = (float)CameraZLight;
    float lightLen = (float)sqrt((double)(s_light[0] * s_light[0] + s_light[1] * s_light[1] + s_light[2] * s_light[2]));

    const TYPE_VT16 *normals = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffNormales);
    const TYPE_VT16 *faces = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffNormFaces);

    if (s_perGroup) {
        /* Groups own consecutive runs of points (one normal each) and of face normals. */
        const T_GPU_GROUPE *groups = (const T_GPU_GROUPE *)((U8 *)lBody + lBody->OffGroupes);
        S32 point = 0;
        S32 face = lBody->NbPoints;
        for (S32 g = 0; g < lBody->NbGroupes && g < 30; g++) {
            RotateNormals(&TabMat[g], normals + point, groups[g].NbPts, point, lightLen);
            RotateNormals(&TabMat[g], faces + (face - lBody->NbPoints), groups[g].NbNorm, face, lightLen);
            point += groups[g].NbPts;
            face += groups[g].NbNorm;
        }
    } else {
        RotateNormals(&TabMat[0], normals, lBody->NbNormales, 0, lightLen);
        RotateNormals(&TabMat[0], faces, lBody->NbNormFaces, lBody->NbNormales, lightLen);
    }

    s_origin[0] = (float)PosXWr;
    s_origin[1] = (float)PosYWr;
    s_origin[2] = (float)PosZWr;
    s_draw = GpuObj_BeginDraw();
    if (s_draw != NULL) {
        s_draw->X0 = ClipXMin;
        s_draw->Y0 = ClipYMin;
        s_draw->X1 = ClipXMax;
        s_draw->Y1 = ClipYMax;
        s_draw->ZBuffer = Fill_Flag_ZBuffer;
        s_draw->HasLogical = Fill_Flag_Fog;
        if (Fill_Flag_Fog) {
            memcpy(s_draw->Logical, Fill_Logical_Palette, sizeof(s_draw->Logical));
        }
        if (GpuObjTreeModel) {
            /* A decor stands still: its root group's rotation is the model's. */
            const TYPE_MAT *r = &TabMat[0];
            const float rot[9] = {r->F.M11, r->F.M12, r->F.M13, r->F.M21, r->F.M22,
                                  r->F.M23, r->F.M31, r->F.M32, r->F.M33};
            float wind[3];
            ViewWind(wind);
            SetTree(s_draw, lBody, rot, s_origin, wind);
        }
    }

    GpuObj_SnapshotBegin(ScreenXMin - SNAPSHOT_MARGIN, ScreenYMin - SNAPSHOT_MARGIN,
                         ScreenXMax + SNAPSHOT_MARGIN + 1, ScreenYMax + SNAPSHOT_MARGIN + 1);
    s_capturing = TRUE;
    return TRUE;
}

void AffGpu_Entity(U32 type, void *poly) {
    if (!s_capturing || s_draw == NULL) {
        return;
    }
    const U16 *w = (const U16 *)poly;

    if (type >= 96) {
        if (type == 96) {
            EmitLine(w);
        } else {
            /* Fill_Sphere type 2 blends with the frame: leave it to software. */
            EmitSphere(w, type == 98 || (w[0] & 0xFF) == 2);
        }
        return;
    }

    U32 polyType = type >> 2;
    S32 quad = (type & 1) != 0;
    const U16 points[4] = {w[0], w[1], w[2], w[3]};
    S32 nbPoints = quad ? 4 : 3;
    U16 color = w[4];
    U16 normal = w[5];
    const float *faceNormal = normal < MAX_LIGHTS ? s_normal[normal] : NULL;

    T_GPU_MATERIAL m;
    memset(&m, 0, sizeof(m));
    m.Color = (float)(color & 0xFF);
    m.Mode = (float)GPUOBJ_MODE_PASS;
    if (TypeProj != TYPE_3D) {
        m.Flags = (float)GPUOBJ_FLAG_ISO;
    }
    S32 smooth = FALSE;

    if (polyType <= POLY_DITHER_TABLE) {
        switch (polyType) {
        case POLY_SOLID:
            m.Mode = (float)GPUOBJ_MODE_SOLID;
            {
                S32 rgb[3];
                if (IsGlowColor(color & 255, rgb)) {
                    m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_EMISSIVE);
                }
            }
            break;
        case POLY_FLAT:
        case POLY_GOURAUD:
        case POLY_DITHER: {
            m.Mode = (float)GPUOBJ_MODE_SHADED;
            smooth = polyType != POLY_FLAT;
            /* A lamp model's glass: the ramp's brightest entry tells. */
            S32 rgb[3];
            if (IsGlowColor((color & 255) | 15, rgb)) {
                m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_EMISSIVE);
            }
            break;
        }
        default:
            /* Transparent, pattern and table fills keep the software colour. */
            break;
        }
    } else if (polyType < POLY_TEXTURE_Z_FOG && ObjPtrMap != NULL) {
        U32 sub = (polyType - POLY_TEXTURE) & 7;
        U32 shading = sub & 3;

        /* One page and one CLUT per draw: a body switching either mid-way
           keeps the software colour for the rest. */
        if (s_draw->Page == NULL) {
            s_draw->Page = ObjPtrMap;
        }
        if (shading != 0 && s_draw->Clut == NULL) {
            s_draw->Clut = PtrCLUTGouraud;
        }

        S32 usable = s_draw->Page == ObjPtrMap && (shading == 0 || (PtrCLUTGouraud != NULL && s_draw->Clut == PtrCLUTGouraud));
        if (usable) {
            m.Mode = (float)(shading == 0 ? GPUOBJ_MODE_TEX : GPUOBJ_MODE_TEXSHADED);
            smooth = shading >= 2;
            m.TexOffset = (float)(PtrMap - ObjPtrMap);
            m.TexMask = (float)(RepMask & 0xFFFF);
            if (sub >= 4) {
                m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_CHROMAKEY);
            }
        }
    }

    if (GpuObjTreeModel && m.Mode != (float)GPUOBJ_MODE_PASS &&
        GpuTree_IsFoliage(GpuObjTreeFoliage, polyType > POLY_DITHER_TABLE, color)) {
        m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_FOLIAGE);
    }

    /* UVs come from the body data: Fill_Poly reuses the point list it is given. */
    float uvs[4][2];
    memset(uvs, 0, sizeof(uvs));
    if (m.Mode == (float)GPUOBJ_MODE_TEX || m.Mode == (float)GPUOBJ_MODE_TEXSHADED) {
        if (type & 2) {
            U16 scale = w[6];
            for (S32 i = 0; i < nbPoints; i++) {
                EnvMappedUv(points[i], scale, uvs[i]);
            }
        } else {
            for (S32 i = 0; i < nbPoints; i++) {
                uvs[i][0] = (float)w[6 + i * 2] / 256.0f;
                uvs[i][1] = (float)w[7 + i * 2] / 256.0f;
            }
        }
    }
    EmitPolygon(points, nbPoints, faceNormal, smooth, &m, uvs);

    /* A lamp model's glowing glass: its centre and colour make the light. */
    if (((S32)m.Flags & GPUOBJ_FLAG_EMISSIVE) != 0) {
        float c[3] = {0.0f, 0.0f, 0.0f};
        for (S32 i = 0; i < nbPoints; i++) {
            T_GPU_PROJ p;
            Project(points[i], &p);
            c[0] += p.Vx / (float)nbPoints;
            c[1] += p.Vy / (float)nbPoints;
            c[2] += p.Vz / (float)nbPoints;
        }
        AddGlow(m.Mode == (float)GPUOBJ_MODE_SHADED ? ((color & 255) | 15) : (color & 255), c);
    }

    /* A polygon showing the animated fire texture: remember where it is, so the
       body casts firelight. */
    if ((m.Mode == (float)GPUOBJ_MODE_TEX || m.Mode == (float)GPUOBJ_MODE_TEXSHADED) && GpuObjNbFires > 0 &&
        s_draw->Page == GpuObjFirePage) {
        float cu = 0.0f, cv = 0.0f;
        for (S32 i = 0; i < nbPoints; i++) {
            cu += uvs[i][0];
            cv += uvs[i][1];
        }
        const S32 u = (S32)(cu / (float)nbPoints);
        const S32 v = (S32)(cv / (float)nbPoints);
        const S32 index = ((S32)m.TexOffset + ((((v & 255) << 8) | (u & 255)) & (S32)m.TexMask)) & 65535;
        if (GpuObj_FireAt(index & 255, index >> 8) >= 0) {
            /* Longest side against shortest: a beam's polygons are slivers. */
            float longest = 0.0f, shortest = 1e30f;
            T_GPU_PROJ corner[4];
            for (S32 i = 0; i < nbPoints; i++) {
                Project(points[i], &corner[i]);
            }
            for (S32 i = 0; i < nbPoints; i++) {
                const T_GPU_PROJ *a = &corner[i];
                const T_GPU_PROJ *b = &corner[(i + 1) % nbPoints];
                const float ex = a->Vx - b->Vx, ey = a->Vy - b->Vy, ez = a->Vz - b->Vz;
                const float len = ex * ex + ey * ey + ez * ez;
                longest = len > longest ? len : longest;
                shortest = len < shortest ? len : shortest;
            }
            if (longest > shortest * 16.0f) {
                s_fireSlender++;
            }
            for (S32 i = 0; i < nbPoints; i++) {
                T_GPU_PROJ p;
                Project(points[i], &p);
                if (s_fireCount == 0 && i == 0) {
                    s_fireRect[0] = s_fireRect[2] = p.Sx;
                    s_fireRect[1] = s_fireRect[3] = p.Sy;
                }
                s_fireRect[0] = p.Sx < s_fireRect[0] ? p.Sx : s_fireRect[0];
                s_fireRect[1] = p.Sy < s_fireRect[1] ? p.Sy : s_fireRect[1];
                s_fireRect[2] = p.Sx > s_fireRect[2] ? p.Sx : s_fireRect[2];
                s_fireRect[3] = p.Sy > s_fireRect[3] ? p.Sy : s_fireRect[3];
                const float vp[3] = {p.Vx, p.Vy, p.Vz};
                for (S32 a = 0; a < 3; a++) {
                    if (s_fireCount == 0 && i == 0) {
                        s_fireMin[a] = s_fireMax[a] = vp[a];
                    }
                    s_fireMin[a] = vp[a] < s_fireMin[a] ? vp[a] : s_fireMin[a];
                    s_fireMax[a] = vp[a] > s_fireMax[a] ? vp[a] : s_fireMax[a];
                }
                s_fireSum[0] += p.Vx / (float)nbPoints;
                s_fireSum[1] += p.Vy / (float)nbPoints;
                s_fireSum[2] += p.Vz / (float)nbPoints;
            }
            s_fireCount++;
        }
    }
}

static float MaxF(float a, float b) {
    return a > b ? a : b;
}

static S32 s_orbOpen = FALSE;

S32 AffGpu_OrbBegin(S32 cx, S32 cy, S32 halfBox) {
    s_orbOpen = FALSE;
    if (!GpuObj_Active()) {
        return FALSE;
    }
    T_GPUOBJ_DRAW *draw = GpuObj_BeginDraw();
    if (draw == NULL) {
        return FALSE;
    }
    draw->X0 = ClipXMin;
    draw->Y0 = ClipYMin;
    draw->X1 = ClipXMax;
    draw->Y1 = ClipYMax;
    /* Exact id matching: the sprite's halo pixels, outside the sphere, keep the
       software colour instead of showing the scene surface behind. */
    draw->Scene = FALSE;
    GpuObj_SnapshotBegin(cx - halfBox, cy - halfBox, cx + halfBox, cy + halfBox);
    s_orbOpen = TRUE;
    return TRUE;
}

void AffGpu_OrbEnd(S32 cx, S32 cy, const U8 *palette) {
    if (!s_orbOpen) {
        return;
    }
    s_orbOpen = FALSE;

    /* The sprite's extent gives the sphere's size; its core the ball proper. */
    float radius = 6.0f;
    if (ScreenXMax >= ScreenXMin && ScreenYMax >= ScreenYMin) {
        const S32 w = ScreenXMax - ScreenXMin + 1;
        const S32 h = ScreenYMax - ScreenYMin + 1;
        /* Centred on what was drawn: the sprite's hot spot is not its middle. */
        cx = (ScreenXMin + ScreenXMax) / 2;
        cy = (ScreenYMin + ScreenYMax) / 2;
        radius = (float)(w < h ? w : h) * 0.5f * 0.72f;
        if (radius < 2.0f) {
            radius = 2.0f;
        }
    }

    /* Colour: the sprite's saturated pixels within the sphere, bright core ignored. */
    float sum[3] = {0.0f, 0.0f, 0.0f};
    float weight = 0.0f;
    const S32 r = (S32)radius;
    const U8 *log = (const U8 *)Log;
    for (S32 y = cy - r; y <= cy + r; y++) {
        if (y < ClipYMin || y > ClipYMax) {
            continue;
        }
        for (S32 x = cx - r; x <= cx + r; x++) {
            if (x < ClipXMin || x > ClipXMax || (x - cx) * (x - cx) + (y - cy) * (y - cy) > r * r) {
                continue;
            }
            const U8 *c = palette + log[y * (S32)ModeDesiredX + x] * 3;
            const float mx = (float)(c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]));
            const float mn = (float)(c[0] < c[1] ? (c[0] < c[2] ? c[0] : c[2]) : (c[1] < c[2] ? c[1] : c[2]));
            const float w = (mx - mn) + 1.0f;
            sum[0] += c[0] * w;
            sum[1] += c[1] * w;
            sum[2] += c[2] * w;
            weight += w;
        }
    }
    float rgb[3] = {1.0f, 0.85f, 0.3f};
    if (weight > 0.0f) {
        const float peak = MaxF(sum[0], MaxF(sum[1], sum[2]));
        if (peak > 0.0f) {
            for (S32 i = 0; i < 3; i++) {
                rgb[i] = sum[i] / peak;
            }
        }
    }

    T_GPUOBJ_VERTEX *v = GpuObj_AllocVerts(6);
    if (v != NULL) {
        static const S32 order[6] = {0, 1, 2, 0, 2, 3};
        static const float cornerX[4] = {-1.0f, 1.0f, 1.0f, -1.0f};
        static const float cornerY[4] = {-1.0f, -1.0f, 1.0f, 1.0f};
        const float sw = 2.0f / (float)ModeDesiredX;
        const float sh = 2.0f / (float)ModeDesiredY;
        for (S32 i = 0; i < 6; i++) {
            const S32 k = order[i];
            const float px = (float)cx + 0.5f + cornerX[k] * radius;
            const float py = (float)cy + 0.5f + cornerY[k] * radius;
            memset(&v[i], 0, sizeof(v[i]));
            v[i].clip[0] = px * sw - 1.0f;
            v[i].clip[1] = 1.0f - py * sh;
            v[i].clip[2] = 0.5f;
            v[i].clip[3] = 1.0f;
            v[i].normal[2] = 1.0f;
            v[i].light[3] = (float)GPUOBJ_MODE_ORB;
            v[i].vpos[3] = (float)GPUOBJ_FLAG_BAKED;
            v[i].mat[1] = rgb[0];
            v[i].mat[2] = rgb[1];
            v[i].mat[3] = rgb[2];
            v[i].uv[2] = cornerX[k];
            v[i].uv[3] = cornerY[k];
        }
    }
    GpuObj_EndDraw();
}

/* The global light of the scene being captured: toward the light that shades
   bodies, and the world's up, both in scene space. */
static void UpdateSceneLight(void) {
    /* Bodies drawn outside the scene (the behaviour menu's Twinsens, the
       inventory's objects) have their own projection and light: they must not
       change the scene's, which the presenter keeps using behind them. */
    if (!s_inScene) {
        return;
    }
    const float lx = (float)CameraXLight, ly = (float)CameraYLight, lz = (float)CameraZLight;
    const float len = sqrtf(lx * lx + ly * ly + lz * lz);
    if (len > 0.0f) {
        GpuObjSunDir[0] = lx / len;
        GpuObjSunDir[1] = ly / len;
        GpuObjSunDir[2] = lz / len;
    }
    GpuObjSunExterior = TypeProj == TYPE_3D;
    /* Iso: (x - z) * 3 / 64 across, so a unit is about 0.066 pixels. */
    GpuObjFocal = TypeProj == TYPE_3D ? FRatioX : 3.0f / 64.0f * 1.4142f;
    GpuObjProj[0] = (float)XCentre;
    GpuObjProj[1] = (float)YCentre;
    GpuObjProj[2] = FRatioX;
    GpuObjProj[3] = FRatioY;
    if (TypeProj == TYPE_3D) {
        const S32 savedX = X0;
        const S32 savedY = Y0;
        const S32 savedZ = Z0;
        /* The world's axes too: the sky is anchored to them, and the rain's
           ground grid. Where the camera stands goes with them, taken at the
           same moment, so a scene point can be put back in the world. */
        GpuObjCameraPos[0] = (float)CameraX;
        GpuObjCameraPos[1] = (float)CameraY;
        GpuObjCameraPos[2] = (float)CameraZ;
        /* And the placement itself, the way a vertex is placed: rows of the
           world matrix and the rotated eye, so a draw captured now can be drawn
           later through the difference. */
        GpuObjCapMat[0] = MatriceWorld.F.M11;
        GpuObjCapMat[1] = MatriceWorld.F.M12;
        GpuObjCapMat[2] = MatriceWorld.F.M13;
        GpuObjCapMat[3] = MatriceWorld.F.M21;
        GpuObjCapMat[4] = MatriceWorld.F.M22;
        GpuObjCapMat[5] = MatriceWorld.F.M23;
        GpuObjCapMat[6] = MatriceWorld.F.M31;
        GpuObjCapMat[7] = MatriceWorld.F.M32;
        GpuObjCapMat[8] = MatriceWorld.F.M33;
        GpuObjCapCam[0] = (float)CameraXr;
        GpuObjCapCam[1] = (float)CameraYr;
        GpuObjCapCam[2] = (float)CameraZr;
        GpuObjCapValid = TRUE;
        LongRotatePoint(&MatriceWorld, 4096, 0, 0);
        GpuObjWorldX[0] = (float)X0 / 4096.0f;
        GpuObjWorldX[1] = (float)Y0 / 4096.0f;
        GpuObjWorldX[2] = (float)Z0 / 4096.0f;
        LongRotatePoint(&MatriceWorld, 0, 0, 4096);
        GpuObjWorldZ[0] = (float)X0 / 4096.0f;
        GpuObjWorldZ[1] = (float)Y0 / 4096.0f;
        GpuObjWorldZ[2] = (float)Z0 / 4096.0f;
        LongRotatePoint(&MatriceWorld, 0, 4096, 0);
        const float ux = (float)X0, uy = (float)Y0, uz = (float)Z0;
        X0 = savedX;
        Y0 = savedY;
        Z0 = savedZ;
        const float ul = sqrtf(ux * ux + uy * uy + uz * uz);
        if (ul > 0.0f) {
            GpuObjUpDir[0] = ux / ul;
            GpuObjUpDir[1] = uy / ul;
            GpuObjUpDir[2] = uz / ul;
        }
    } else {
        GpuObjUpDir[0] = 0.0f;
        GpuObjUpDir[1] = 1.0f;
        GpuObjUpDir[2] = 0.0f;
        GpuObjCapValid = FALSE; /* isometric: no camera to be drawn from twice */
    }
}

/* A world point in the scene space lights and vertex positions use. */
static void SceneSpace(float wx, float wy, float wz, float out[3]) {
    if (TypeProj == TYPE_3D) {
        const S32 savedX = X0;
        const S32 savedY = Y0;
        const S32 savedZ = Z0;
        LongWorldRotatePoint((S32)wx, (S32)wy, (S32)wz);
        out[0] = (float)(X0 - CameraXr);
        out[1] = (float)(Y0 - CameraYr);
        out[2] = (float)(Z0 - CameraZr);
        X0 = savedX;
        Y0 = savedY;
        Z0 = savedZ;
    } else {
        out[0] = wx - (float)CameraXr;
        out[1] = wy - (float)CameraYr;
        out[2] = wz - (float)CameraZr;
    }
}

S32 AffGpu_Shadow(const S32 corners[4][3], S32 ground, S32 height, S32 tall) {
    if (!GpuObj_Active() || !GpuObjShadows) {
        return FALSE;
    }
    T_GPUOBJ_VERTEX *v = GpuObj_AllocShadow();
    if (v == NULL) {
        return TRUE;
    }
    UpdateSceneLight();
    /* The corners are the ellipse's axis ends: centre and half axes. */
    float c[3], a[3], b[3];
    for (S32 i = 0; i < 3; i++) {
        c[i] = ((float)corners[0][i] + (float)corners[2][i]) * 0.5f;
        a[i] = ((float)corners[3][i] - (float)corners[1][i]) * 0.5f;
        b[i] = ((float)corners[2][i] - (float)corners[0][i]) * 0.5f;
    }
    /* The quad reaches far past the corners, so it lies on the ground under
       the caster and follows a slope only when both ends of an axis agree with
       it: a corner resting on a nearby object would tilt it into the ground. */
    const float g = (float)ground;
    c[1] = g;
    if (fabsf((float)corners[1][1] - g) > 160.0f || fabsf((float)corners[3][1] - g) > 160.0f) {
        a[1] = 0.0f;
    }
    if (fabsf((float)corners[0][1] - g) > 160.0f || fabsf((float)corners[2][1] - g) > 160.0f) {
        b[1] = 0.0f;
    }
    /* Higher above the ground: fainter and wider. */
    const float h = height > 0 ? (float)height : 0.0f;
    const float soft = h / 1200.0f > 1.0f ? 1.0f : h / 1200.0f;
    const float strength = 0.8f * (1.0f - 0.75f * soft);
    const float spread = 1.25f + 0.5f * soft;
    /* Room for the contact penumbra and for shadows thrown by nearby lights. */
    const float extent = 4.0f;

    /* The caster, a vertical capsule standing on the shadow's centre. */
    const float la = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    const float lb = sqrtf(b[0] * b[0] + b[1] * b[1] + b[2] * b[2]);
    float radius = 0.4f * (la + lb);
    float body = tall > 0 ? (float)tall : 2.0f * radius;
    if (radius > body * 0.5f) {
        radius = body * 0.5f;
    }
    if (s_nbRequests < GPUOBJ_MAX_SHADOWS) {
        T_SHADOW_REQUEST *req = &s_requests[s_nbRequests++];
        SceneSpace(c[0], c[1] + h, c[2], req->Origin);
        SceneSpace(c[0], c[1], c[2], req->Ground);
        req->Radius = radius;
        req->Tall = body;
        req->Strength = 1.0f - 0.75f * soft;
    }
    float bottom[3], top[3];
    SceneSpace(c[0], c[1] + h + radius, c[2], bottom);
    SceneSpace(c[0], c[1] + h + body - radius, c[2], top);

    static const S32 cornerU[6] = {0, 1, 1, 0, 1, 0};
    static const S32 cornerV[6] = {0, 0, 1, 0, 1, 1};
    const float cell = 2.0f * extent / (float)GPUOBJ_SHADOW_CELLS;
    for (S32 n = 0; n < GPUOBJ_SHADOW_VERTS; n++) {
        const S32 quad = n / 6;
        const S32 k = n % 6;
        const float u = -extent + (float)(quad % GPUOBJ_SHADOW_CELLS + cornerU[k]) * cell;
        const float t = -extent + (float)(quad / GPUOBJ_SHADOW_CELLS + cornerV[k]) * cell;
        float wx = c[0] + a[0] * u + b[0] * t;
        float wy = c[1] + a[1] * u + b[1] * t;
        float wz = c[2] + a[2] * u + b[2] * t;
        memset(&v[n], 0, sizeof(v[n]));
        if (TypeProj == TYPE_3D) {
            /* Lifted a little toward the camera so the ground does not hide it. */
            LongWorldRotatePoint((S32)wx, (S32)wy, (S32)wz);
            const float depth = (float)(CameraZr - Z0) - 24.0f;
            AffGpu_ViewVertex(&v[n], (float)(X0 - CameraXr), (float)(Y0 - CameraYr), depth > 1.0f ? depth : 1.0f);
        } else {
            wy += 8.0f;
            const float x = wx - (float)CameraXr, y = wy - (float)CameraYr, z = wz - (float)CameraZr;
            const float sx = (float)XCentre + (x - z) * 3.0f / 64.0f;
            const float sy = (float)YCentre + 1.0f + ((x + z) * 6.0f - y * 15.0f) / 256.0f;
            float d = 0.5f - (x + 0.8f * y + z) / ISO_DEPTH_RANGE;
            d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
            SetClipPos(&v[n], sx, sy, 1.0f, d);
            v[n].vpos[0] = x;
            v[n].vpos[1] = y;
            v[n].vpos[2] = z;
        }
        v[n].vpos[3] = 0.0f;
        v[n].light[0] = bottom[0];
        v[n].light[1] = bottom[1];
        v[n].light[2] = bottom[2];
        v[n].light[3] = (float)GPUOBJ_MODE_SHADOW;
        v[n].normal[0] = top[0];
        v[n].normal[1] = top[1];
        v[n].normal[2] = top[2];
        v[n].normal[3] = radius;
        v[n].mat[1] = strength;
        v[n].mat[2] = spread;
        v[n].mat[3] = extent;
        v[n].uv[2] = u;
        v[n].uv[3] = t;
    }
    return TRUE;
}

/* A captured body, kept for the shadows; its lamp or fire as a light. */
static void KeepCaster(const T_GPUOBJ_DRAW *draw) {
    const U32 group = GpuObj_GroupId();
    if (group != 0 && group != s_sceneryGroup) {
        s_sceneryGroup = group;
        s_nbSceneryLamps = 0;
    }
    if (draw->GlowListed) {
        /* Its light is the listed one (GpuObjSceneryLamps). */
    } else if (draw->HasGlow && group != 0 && s_nbSceneryLamps < MAX_FRAME_LAMPS) {
        T_FRAME_LIGHT *l = &s_sceneryLamps[s_nbSceneryLamps++];
        memcpy(l->Pos, draw->GlowPos, sizeof(l->Pos));
        l->Radius = draw->HasGlow == 2 ? 9000.0f : 2400.0f;
        l->Weight = 0.9f * 0.6f;
    } else if (draw->HasGlow && s_nbLamps < MAX_FRAME_LAMPS) {
        T_FRAME_LIGHT *l = &s_lamps[s_nbLamps++];
        memcpy(l->Pos, draw->GlowPos, sizeof(l->Pos));
        l->Radius = draw->HasGlow == 2 ? 9000.0f : 2400.0f;
        l->Weight = 0.9f * 0.6f;
    }
    if (draw->HasFire && s_nbLamps < MAX_FRAME_LAMPS) {
        T_FRAME_LIGHT *l = &s_lamps[s_nbLamps++];
        memcpy(l->Pos, draw->FirePos, sizeof(l->Pos));
        l->Radius = 2400.0f;
        l->Weight = 0.6f * 0.65f;
    }
    if (!GpuObjShadows || s_nbCasters >= MAX_CASTERS || draw->Count == 0 ||
        s_nbCasterVerts + draw->Count > MAX_CASTER_VERTS) {
        return;
    }
    T_CASTER *c = &s_casters[s_nbCasters++];
    memcpy(c->Origin, s_origin, sizeof(c->Origin));
    c->First = s_nbCasterVerts;
    c->Count = draw->Count;
    for (U32 i = 0; i < draw->Count; i++) {
        const T_GPUOBJ_VERTEX *v = &GpuObjVerts[draw->First + i];
        float *o = s_casterVerts[s_nbCasterVerts++];
        o[0] = v->vpos[0];
        o[1] = v->vpos[1];
        o[2] = v->vpos[2];
        const S32 disc = (S32)v->light[3] == GPUOBJ_MODE_DISC;
        o[3] = disc ? 1.0f : 0.0f;
        o[4] = disc ? v->uv[2] : 0.0f;
        o[5] = disc ? v->uv[3] : 0.0f;
    }
}

void AffGpu_BeginScene(void) {
    s_inScene = TRUE;
    s_nbCasters = 0;
    s_nbCasterVerts = 0;
    s_nbRequests = 0;
    s_nbLamps = 0;
}

/* A scene-space point as a shadow vertex: lifted a little toward the camera so
   the ground it lies on does not hide it. */
static void ShadowVertex(T_GPUOBJ_VERTEX *v, float x, float y, float z) {
    if (TypeProj == TYPE_3D) {
        const float depth = -z - 24.0f;
        AffGpu_ViewVertex(v, x, y, depth > 1.0f ? depth : 1.0f);
    } else {
        memset(v, 0, sizeof(*v));
        const float ly = y + 8.0f;
        const float sx = (float)XCentre + (x - z) * 3.0f / 64.0f;
        const float sy = (float)YCentre + 1.0f + ((x + z) * 6.0f - ly * 15.0f) / 256.0f;
        float d = 0.5f - (x + 0.8f * ly + z) / ISO_DEPTH_RANGE;
        d = d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d);
        SetClipPos(v, sx, sy, 1.0f, d);
    }
    v->vpos[0] = x;
    v->vpos[1] = y;
    v->vpos[2] = z;
    v->vpos[3] = 0.0f;
}

static float Dot3(const float *a, const float *b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}

/* The caster's triangles flattened on the ground plane through the request's
   ground point, along the sun's direction (light NULL) or away from a light. */
static void EmitSilhouette(const T_CASTER *c, const T_SHADOW_REQUEST *r, const T_FRAME_LIGHT *light) {
    T_GPUOBJ_VERTEX *out = GpuObj_AllocShadowVerts(c->Count);
    if (out == NULL) {
        return;
    }
    const float *up = GpuObjUpDir;
    float sun[3] = {GpuObjSunDir[0], GpuObjSunDir[1], GpuObjSunDir[2]};
    /* A low sun is raised a little: its shadows would reach across the scene. */
    float su = Dot3(sun, up);
    if (su < 0.3f) {
        for (S32 i = 0; i < 3; i++) {
            sun[i] += up[i] * (0.3f - su);
        }
        su = 0.3f;
    }
    const float reach = r->Tall * 3.0f;
    for (U32 i = 0; i < c->Count; i++) {
        const float *p = s_casterVerts[c->First + i];
        const float rel[3] = {p[0] - r->Ground[0], p[1] - r->Ground[1], p[2] - r->Ground[2]};
        float h = Dot3(rel, up);
        if (h < 0.0f) {
            h = 0.0f;
        }
        float q[3];
        if (light == NULL) {
            for (S32 k = 0; k < 3; k++) {
                q[k] = p[k] - sun[k] * (h / su);
            }
        } else {
            const float dir[3] = {p[0] - light->Pos[0], p[1] - light->Pos[1], p[2] - light->Pos[2]};
            float du = Dot3(dir, up);
            const float len = sqrtf(Dot3(dir, dir));
            if (du > -0.15f * len) {
                du = -0.15f * len;
            }
            for (S32 k = 0; k < 3; k++) {
                q[k] = p[k] + dir[k] * (h / -du);
            }
        }
        /* Onto the plane exactly, and no farther than a long shadow reaches. */
        float off[3] = {q[0] - r->Ground[0], q[1] - r->Ground[1], q[2] - r->Ground[2]};
        const float lift = Dot3(off, up);
        for (S32 k = 0; k < 3; k++) {
            off[k] -= up[k] * lift;
        }
        const float dist = sqrtf(Dot3(off, off));
        if (dist > reach) {
            for (S32 k = 0; k < 3; k++) {
                off[k] *= reach / dist;
            }
        }
        T_GPUOBJ_VERTEX *v = &out[i];
        ShadowVertex(v, r->Ground[0] + off[0], r->Ground[1] + off[1], r->Ground[2] + off[2]);
        v->light[3] = (float)GPUOBJ_MODE_SILHOUETTE;
        v->normal[0] = r->Ground[0];
        v->normal[1] = r->Ground[1];
        v->normal[2] = r->Ground[2];
        v->normal[3] = reach;
        v->mat[1] = r->Strength;
        v->mat[3] = p[3];
        v->uv[2] = p[4];
        v->uv[3] = p[5];
        if (light != NULL) {
            v->light[0] = light->Pos[0];
            v->light[1] = light->Pos[1];
            v->light[2] = light->Pos[2];
            v->mat[0] = light->Radius;
            v->mat[2] = light->Weight;
        }
    }
}

void AffGpu_BuildShadows(void) {
    GpuObjRayScene = FALSE;
    if (!GpuObj_Active() || !GpuObjShadows) {
        s_inScene = FALSE;
        return;
    }
    UpdateSceneLight();
    s_inScene = FALSE;
    /* Exteriors: the presenter traces rays toward the sun and the lights through
       every body and the terrain, so no silhouettes are needed. */
    GpuObjRayScene = GpuObjRayTrace && TypeProj == TYPE_3D;
    /* Lights that can shadow: the scene's own and the lamps and fires on bodies. */
    T_FRAME_LIGHT lights[GPUOBJ_MAX_LIGHTS + MAX_FRAME_LAMPS * 2 + GPUOBJ_MAX_SCENERY_LAMPS];
    U32 nbLights = 0;
    if (GpuObjLightsEnabled) {
        for (U32 i = 0; i < GpuObjNbLights; i++) {
            T_FRAME_LIGHT *l = &lights[nbLights++];
            memcpy(l->Pos, GpuObjLights[i].Pos, sizeof(l->Pos));
            l->Radius = GpuObjLights[i].Pos[3];
            l->Weight = GpuObjLights[i].Color[3] * (0.3f * GpuObjLights[i].Color[0] +
                                                    0.59f * GpuObjLights[i].Color[1] +
                                                    0.11f * GpuObjLights[i].Color[2]);
        }
        for (U32 i = 0; i < s_nbLamps; i++) {
            lights[nbLights++] = s_lamps[i];
        }
        /* Only the scenery on screen: the last one captured. */
        for (U32 i = 0; i < s_nbSceneryLamps && s_sceneryGroup == GpuObj_LastGroupId(); i++) {
            lights[nbLights++] = s_sceneryLamps[i];
        }
        for (U32 i = 0; i < GpuObjNbSceneryLamps && GpuObjSceneryLampGroup == GpuObj_LastGroupId(); i++) {
            T_FRAME_LIGHT *l = &lights[nbLights++];
            memcpy(l->Pos, GpuObjSceneryLamps[i].Pos, sizeof(l->Pos));
            l->Radius = 9000.0f;
            l->Weight = 0.9f * 0.6f;
        }
    }

    for (U32 n = 0; n < s_nbRequests; n++) {
        const T_SHADOW_REQUEST *r = &s_requests[n];
        /* The body standing where the shadow's caster is. */
        const T_CASTER *best = NULL;
        float bestDist = 96.0f * 96.0f;
        for (U32 k = 0; k < s_nbCasters; k++) {
            const float dx = s_casters[k].Origin[0] - r->Origin[0];
            const float dy = s_casters[k].Origin[1] - r->Origin[1];
            const float dz = s_casters[k].Origin[2] - r->Origin[2];
            const float d = dx * dx + dy * dy + dz * dz;
            if (d < bestDist) {
                bestDist = d;
                best = &s_casters[k];
            }
        }
        if (best == NULL) {
            continue;
        }
        if (GpuObjSunlight && !GpuObjRayScene) {
            EmitSilhouette(best, r, NULL);
        }
        /* The two lights reaching the body the most, unless it carries them. */
        S32 pick[2] = {-1, -1};
        float pickW[2] = {0.0f, 0.0f};
        for (U32 k = 0; k < nbLights; k++) {
            const T_FRAME_LIGHT *l = &lights[k];
            const float d[3] = {l->Pos[0] - r->Ground[0], l->Pos[1] - r->Ground[1], l->Pos[2] - r->Ground[2]};
            const float height = Dot3(d, GpuObjUpDir);
            const float flat[3] = {d[0] - GpuObjUpDir[0] * height, d[1] - GpuObjUpDir[1] * height,
                                   d[2] - GpuObjUpDir[2] * height};
            if (Dot3(flat, flat) < r->Radius * r->Radius * 2.25f && height < r->Tall * 1.2f) {
                continue;
            }
            const float f = 1.0f - Dot3(d, d) / (l->Radius * l->Radius);
            if (f <= 0.0f) {
                continue;
            }
            const float w = f * f * l->Weight;
            if (w > pickW[0]) {
                pick[1] = pick[0];
                pickW[1] = pickW[0];
                pick[0] = (S32)k;
                pickW[0] = w;
            } else if (w > pickW[1]) {
                pick[1] = (S32)k;
                pickW[1] = w;
            }
        }
        for (S32 k = 0; k < 2 && !GpuObjRayScene; k++) {
            if (pick[k] >= 0 && pickW[k] > 0.02f) {
                EmitSilhouette(best, r, &lights[pick[k]]);
            }
        }
    }
}

void AffGpu_End(void) {
    if (!s_capturing) {
        return;
    }
    s_capturing = FALSE;
    if (s_glowCount >= 1 && s_draw != NULL) {
        s_draw->HasGlow = TypeProj == TYPE_3D ? 2 : 1;
        for (S32 i = 0; i < 3; i++) {
            s_draw->GlowPos[i] = s_glowSum[i] / (float)s_glowCount;
            s_draw->GlowColor[i] = s_glowRgb[i] / (float)s_glowCount;
        }
    }
    if (s_fireCount > 0 && s_draw != NULL) {
        s_draw->HasFire = TRUE;
        for (S32 i = 0; i < 3; i++) {
            s_draw->FirePos[i] = s_fireSum[i] / (float)s_fireCount;
        }
        for (S32 i = 0; i < 4; i++) {
            s_draw->FireRect[i] = s_fireRect[i];
        }
        /* Vertical extent against horizontal: a brazier's flame or a fireball has
           height, a laser bar sharing the fire texture lies flat. */
        const float dy = s_fireMax[1] - s_fireMin[1];
        const float dx = s_fireMax[0] - s_fireMin[0];
        const float dz = s_fireMax[2] - s_fireMin[2];
        const float flat = dx > dz ? dx : dz;
        s_draw->FireRises = dy >= flat * 0.45f && s_fireSlender * 2 < s_fireCount;
    }
    s_fireCount = 0;
    s_fireSlender = 0;
    s_fireSum[0] = s_fireSum[1] = s_fireSum[2] = 0.0f;
    s_glowCount = 0;
    s_glowSum[0] = s_glowSum[1] = s_glowSum[2] = 0.0f;
    s_glowRgb[0] = s_glowRgb[1] = s_glowRgb[2] = 0.0f;
    if (s_draw != NULL) {
        s_draw->GlowListed = s_draw->HasGlow && GpuObjLampListed;
        if (s_occPending) {
            s_draw->OccFirst = s_occFirst;
            s_draw->OccCount = s_occCount;
            memcpy(s_draw->OccRows, s_occRows, sizeof(s_occRows));
        }
        KeepCaster(s_draw);
    }
    s_draw = NULL;
    GpuObj_EndDraw();
}

// --- Decor meshes ------------------------------------------------------------
S32 AffGpuSuspended = FALSE;

/* A decor model as the GPU keeps it: every polygon in model space. */
#define MAX_DECOR_MESHES 1024
#define MAX_MESH_BUILD 8192
typedef struct {
    S32 Island; ///< -1: free
    S32 State;  ///< 1 kept, -1 not fit to be kept (drawn by the body display)
    U32 First;
    U32 Count;
    U32 Epoch; ///< GpuObjMeshEpoch it was written in
    float Min[3], Max[3];
    S32 Textured; ///< shows the object page
    S32 Shaded;   ///< some of its textures take the Gouraud CLUT
} T_DECOR_MESH;
static T_DECOR_MESH s_meshes[MAX_DECOR_MESHES];
static S32 s_meshesReady = FALSE;
static S32 s_meshIsland = -1;

static T_GPUOBJ_VERTEX s_build[MAX_MESH_BUILD];
static U32 s_nbBuild = 0;
static S32 s_buildFail = FALSE;
static T_DECOR_MESH *s_buildMesh = NULL;

/* A polygon of the body being kept, as AffGpu_Entity would take it, with the
   points and normals in model space. What the capture leaves to the software
   colour (a pass polygon), a page or CLUT switch, a glowing or burning surface,
   a view-dependent texture: any of them and the body is not kept. */
static void MeshPolygon(U32 type, void *poly) {
    if (s_buildFail) {
        return;
    }
    const U16 *w = (const U16 *)poly;
    if (type >= 96 || (type & 2) != 0) {
        s_buildFail = TRUE;
        return;
    }
    const U32 polyType = type >> 2;
    const S32 quad = (type & 1) != 0;
    const S32 nbPoints = quad ? 4 : 3;
    const U16 color = w[4];
    const U16 normal = w[5];

    T_GPU_MATERIAL m;
    memset(&m, 0, sizeof(m));
    m.Color = (float)(color & 0xFF);
    m.Mode = (float)GPUOBJ_MODE_PASS;
    S32 smooth = FALSE;
    if (polyType <= POLY_DITHER_TABLE) {
        switch (polyType) {
        case POLY_SOLID:
            m.Mode = (float)GPUOBJ_MODE_SOLID;
            break;
        case POLY_FLAT:
        case POLY_GOURAUD:
        case POLY_DITHER:
            m.Mode = (float)GPUOBJ_MODE_SHADED;
            smooth = polyType != POLY_FLAT;
            break;
        default:
            break;
        }
    } else if (polyType < POLY_TEXTURE_Z_FOG && ObjPtrMap != NULL) {
        const U32 sub = (polyType - POLY_TEXTURE) & 7;
        const U32 shading = sub & 3;
        m.Mode = (float)(shading == 0 ? GPUOBJ_MODE_TEX : GPUOBJ_MODE_TEXSHADED);
        smooth = shading >= 2;
        m.TexOffset = (float)(PtrMap - ObjPtrMap);
        m.TexMask = (float)(RepMask & 0xFFFF);
        if (sub >= 4) {
            m.Flags = (float)GPUOBJ_FLAG_CHROMAKEY;
        }
        s_buildMesh->Textured = TRUE;
        s_buildMesh->Shaded |= shading != 0;
    }
    if (m.Mode == (float)GPUOBJ_MODE_PASS) {
        s_buildFail = TRUE;
        return;
    }
    if (GpuObjTreeModel && GpuTree_IsFoliage(GpuObjTreeFoliage, polyType > POLY_DITHER_TABLE, color)) {
        m.Flags = (float)((S32)m.Flags | GPUOBJ_FLAG_FOLIAGE);
    }

    float uvs[4][2];
    memset(uvs, 0, sizeof(uvs));
    if (m.Mode == (float)GPUOBJ_MODE_TEX || m.Mode == (float)GPUOBJ_MODE_TEXSHADED) {
        for (S32 i = 0; i < nbPoints; i++) {
            uvs[i][0] = (float)w[6 + i * 2] / 256.0f;
            uvs[i][1] = (float)w[7 + i * 2] / 256.0f;
        }
        /* The animated fire texture lights its surroundings: the capture's work. */
        if (GpuObjNbFires > 0 && ObjPtrMap == GpuObjFirePage) {
            float cu = 0.0f, cv = 0.0f;
            for (S32 i = 0; i < nbPoints; i++) {
                cu += uvs[i][0];
                cv += uvs[i][1];
            }
            const S32 u = (S32)(cu / (float)nbPoints);
            const S32 v = (S32)(cv / (float)nbPoints);
            const S32 index = ((S32)m.TexOffset + ((((v & 255) << 8) | (u & 255)) & (S32)m.TexMask)) & 65535;
            if (GpuObj_FireAt(index & 255, index >> 8) >= 0) {
                s_buildFail = TRUE;
                return;
            }
        }
    }

    const U32 nbVerts = (U32)(nbPoints - 2) * 3;
    if (s_nbBuild + nbVerts > MAX_MESH_BUILD) {
        s_buildFail = TRUE;
        return;
    }
    const TYPE_VT16 *points = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffPoints);
    const TYPE_VT16 *normals = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffNormales);
    const TYPE_VT16 *faces = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffNormFaces);
    const TYPE_VT16 *faceNormal = NULL;
    if (normal < lBody->NbNormales) {
        faceNormal = &normals[normal];
    } else if (normal < lBody->NbNormales + lBody->NbNormFaces && normal < MAX_LIGHTS) {
        faceNormal = &faces[normal - lBody->NbNormales];
    }
    for (U32 t = 0; t + 2 < (U32)nbPoints; t++) {
        const S32 corners[3] = {0, (S32)t + 1, (S32)t + 2};
        for (S32 c = 0; c < 3; c++) {
            const S32 k = corners[c];
            const U16 point = w[k];
            if (point >= lBody->NbPoints) {
                s_buildFail = TRUE;
                return;
            }
            const TYPE_VT16 *n = smooth ? (point < lBody->NbNormales ? &normals[point] : NULL) : faceNormal;
            T_GPUOBJ_VERTEX *v = &s_build[s_nbBuild++];
            memset(v, 0, sizeof(*v));
            const float p[3] = {(float)points[point].X, (float)points[point].Y, (float)points[point].Z};
            v->vpos[0] = p[0];
            v->vpos[1] = p[1];
            v->vpos[2] = p[2];
            v->vpos[3] = m.Flags;
            for (S32 a = 0; a < 3; a++) {
                s_buildMesh->Min[a] = p[a] < s_buildMesh->Min[a] ? p[a] : s_buildMesh->Min[a];
                s_buildMesh->Max[a] = p[a] > s_buildMesh->Max[a] ? p[a] : s_buildMesh->Max[a];
            }
            if (n != NULL) {
                const float x = (float)n->X, y = (float)n->Y, z = (float)n->Z;
                v->normal[0] = x;
                v->normal[1] = y;
                v->normal[2] = z;
                /* Its length: the light's share is the draw's (MeshLight). */
                v->normal[3] = (float)sqrt((double)(x * x + y * y + z * z));
            } else {
                v->normal[2] = 1.0f;
            }
            v->light[3] = m.Mode;
            v->mat[0] = m.Color;
            v->mat[1] = m.TexOffset;
            v->mat[2] = m.TexMask;
            v->uv[0] = uvs[k][0];
            v->uv[1] = uvs[k][1];
        }
    }
}

static T_DECOR_MESH *DecorMesh(S32 island, S32 index, T_BODY_HEADER *body) {
    if (!s_meshesReady || island != s_meshIsland) {
        for (S32 i = 0; i < MAX_DECOR_MESHES; i++) {
            s_meshes[i].Island = -1;
        }
        if (s_meshesReady) {
            GpuObj_MeshReset();
        }
        s_meshesReady = TRUE;
        s_meshIsland = island;
    }
    if (index < 0 || index >= MAX_DECOR_MESHES) {
        return NULL;
    }
    T_DECOR_MESH *mesh = &s_meshes[index];
    if (mesh->Island == island && (mesh->State < 0 || mesh->Epoch == GpuObjMeshEpoch)) {
        return mesh->State > 0 ? mesh : NULL;
    }
    mesh->Island = island;
    mesh->State = -1;
    mesh->Textured = FALSE;
    mesh->Shaded = FALSE;
    for (S32 a = 0; a < 3; a++) {
        mesh->Min[a] = 1e30f;
        mesh->Max[a] = -1e30f;
    }
    /* Lines and spheres are drawn facing the screen. */
    if (body->NbLines != 0 || body->NbSpheres != 0 || body->NbPoints > MAX_NB_POINTS) {
        return NULL;
    }
    T_BODY_HEADER *saved = lBody;
    lBody = body;
    s_nbBuild = 0;
    s_buildFail = FALSE;
    s_buildMesh = mesh;
    BodyWalkPolys(body, MeshPolygon);
    lBody = saved;
    if (s_buildFail || s_nbBuild == 0) {
        return NULL;
    }
    U32 first = 0;
    T_GPUOBJ_VERTEX *dst = GpuObj_MeshAlloc(s_nbBuild, &first);
    if (dst == NULL) {
        /* The store is full: tried again once the island changes. */
        return NULL;
    }
    memcpy(dst, s_build, s_nbBuild * sizeof(T_GPUOBJ_VERTEX));
    mesh->First = first;
    mesh->Count = s_nbBuild;
    mesh->Epoch = GpuObjMeshEpoch;
    mesh->State = 1;
    return mesh;
}

// --- Lamp lights ---------------------------------------------------------------
/* A lamp model's glass, as the capture finds it (AffGpu_Entity, EmitSphere),
   over the whole model rather than the polygons a view shows: the centre of
   the glowing polygons and spheres in model space, and their colour. */
typedef struct {
    S32 Island; ///< -1: unknown
    S32 Glows;  ///< a glass was found
    float Centre[3];
    float Color[3];
    float Reach; ///< the farthest glass from the centre, and some: the lamp's head
} T_LAMP_GLASS;
static T_LAMP_GLASS s_lampGlass[MAX_DECOR_MESHES];
static S32 s_lampGlassReady = FALSE;
static float s_glassSum[3], s_glassRgb[3];
static S32 s_glassCount = 0;
#define MAX_GLASS_PARTS 256
static float s_glassPart[MAX_GLASS_PARTS][3];

static void GlassAdd(U32 color, const float centre[3]) {
    S32 rgb[3];
    if (!IsGlowColor(color, rgb)) {
        return;
    }
    for (S32 a = 0; a < 3; a++) {
        s_glassSum[a] += centre[a];
        s_glassRgb[a] += (float)rgb[a] / 255.0f;
    }
    if (s_glassCount < MAX_GLASS_PARTS) {
        memcpy(s_glassPart[s_glassCount], centre, sizeof(s_glassPart[0]));
    }
    s_glassCount++;
}

static void GlassPolygon(U32 type, void *poly) {
    const U16 *w = (const U16 *)poly;
    const U32 polyType = type >> 2;
    U32 color;
    if (polyType == POLY_SOLID) {
        color = w[4] & 255;
    } else if (polyType == POLY_FLAT || polyType == POLY_GOURAUD || polyType == POLY_DITHER) {
        color = (w[4] & 255) | 15;
    } else {
        return;
    }
    const S32 nbPoints = (type & 1) ? 4 : 3;
    const TYPE_VT16 *points = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffPoints);
    float c[3] = {0.0f, 0.0f, 0.0f};
    for (S32 i = 0; i < nbPoints; i++) {
        const U16 p = w[i] < lBody->NbPoints ? w[i] : 0;
        c[0] += (float)points[p].X / (float)nbPoints;
        c[1] += (float)points[p].Y / (float)nbPoints;
        c[2] += (float)points[p].Z / (float)nbPoints;
    }
    GlassAdd(color, c);
}

static const T_LAMP_GLASS *LampGlass(S32 island, S32 index, T_BODY_HEADER *body) {
    if (!s_lampGlassReady) {
        for (S32 i = 0; i < MAX_DECOR_MESHES; i++) {
            s_lampGlass[i].Island = -1;
        }
        s_lampGlassReady = TRUE;
    }
    if (index < 0 || index >= MAX_DECOR_MESHES) {
        return NULL;
    }
    T_LAMP_GLASS *g = &s_lampGlass[index];
    if (g->Island != island) {
        g->Island = island;
        s_glassCount = 0;
        for (S32 a = 0; a < 3; a++) {
            s_glassSum[a] = s_glassRgb[a] = 0.0f;
        }
        T_BODY_HEADER *saved = lBody;
        lBody = body;
        BodyWalkPolys(body, GlassPolygon);
        /* Spheres glow unless they are drawn as pass-through (EmitSphere). */
        typedef struct {
            U16 Type, Coul, P1, Rayon;
        } T_GPU_SPHERE;
        const T_GPU_SPHERE *spheres = (const T_GPU_SPHERE *)((U8 *)body + body->OffSpheres);
        const TYPE_VT16 *points = (const TYPE_VT16 *)((U8 *)body + body->OffPoints);
        for (S32 i = 0; i < body->NbSpheres; i++) {
            const U32 kind = spheres[i].Type & 0xFF;
            if (kind == 1 || kind == 2 || spheres[i].P1 >= body->NbPoints) {
                continue;
            }
            const TYPE_VT16 *p = &points[spheres[i].P1];
            const float c[3] = {(float)p->X, (float)p->Y, (float)p->Z};
            GlassAdd(spheres[i].Coul & 255, c);
        }
        lBody = saved;
        g->Glows = s_glassCount > 0;
        for (S32 a = 0; a < 3 && g->Glows; a++) {
            g->Centre[a] = s_glassSum[a] / (float)s_glassCount;
            g->Color[a] = s_glassRgb[a] / (float)s_glassCount;
        }
        g->Reach = 0.0f;
        for (S32 i = 0; i < s_glassCount && i < MAX_GLASS_PARTS; i++) {
            const float dx = s_glassPart[i][0] - g->Centre[0], dy = s_glassPart[i][1] - g->Centre[1],
                        dz = s_glassPart[i][2] - g->Centre[2];
            const float d = (float)sqrt((double)(dx * dx + dy * dy + dz * dz));
            g->Reach = d > g->Reach ? d : g->Reach;
        }
        /* The arms and the cap around the globes. */
        g->Reach += 250.0f;
    }
    return g->Glows ? g : NULL;
}

S32 AffGpu_LampLight(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta) {
    if (!GpuObj_Active() || AffGpuSuspended || TypeProj != TYPE_3D || !GpuObjGlow || !GpuObjLampModel) {
        return FALSE;
    }
    const T_LAMP_GLASS *g = LampGlass(island, index, (T_BODY_HEADER *)obj);
    if (g == NULL) {
        return FALSE;
    }
    /* Placed as BodyDisplay places the model; the rotation's outputs are
       globals the caller still reads. */
    const S32 savedX = X0, savedY = Y0, savedZ = Z0;
    LongRotatePoint(&MatriceWorld, x, y, z);
    const float pos[3] = {(float)(X0 - CameraXr), (float)(Y0 - CameraYr), (float)(Z0 - CameraZr)};
    X0 = savedX;
    Y0 = savedY;
    Z0 = savedZ;
    TYPE_MAT m;
    RotateMatrix(&m, &MatriceWorld, 0, beta, 0);
    const float *c = g->Centre;
    const float glass[3] = {m.F.M11 * c[0] + m.F.M12 * c[1] + m.F.M13 * c[2] + m.F.TX + pos[0],
                            m.F.M21 * c[0] + m.F.M22 * c[1] + m.F.M23 * c[2] + m.F.TY + pos[1],
                            m.F.M31 * c[0] + m.F.M32 * c[1] + m.F.M33 * c[2] + m.F.TZ + pos[2]};
    /* A light whose reach (9000) cannot touch the view is no use. */
    const float depth = -glass[2];
    const float reach = 9000.0f;
    if (depth < -reach) {
        return FALSE;
    }
    const float slack = depth > 1.0f ? depth : 1.0f;
    const float halfW = (float)ModeDesiredX / FRatioX, halfH = (float)ModeDesiredY / (FRatioX * fabsf(FRatioY));
    if (fabsf(glass[0]) > halfW * slack + reach || fabsf(glass[1]) > halfH * slack + reach) {
        return FALSE;
    }
    return GpuObj_AddSceneryLamp(glass, g->Color);
}

// --- Decor occluders ------------------------------------------------------------
typedef struct {
    S32 Island;       ///< -1: unknown
    U32 First, Count; ///< in GpuObjOccTris; Count 0: none
} T_DECOR_OCC;
static T_DECOR_OCC s_occ[MAX_DECOR_MESHES];
static S32 s_occReady = FALSE;
static S32 s_occIsland = -1;
static float s_occBuild[MAX_MESH_BUILD * 3];
static U32 s_nbOccBuild = 0;

/* A polygon of the model as an occluder: what the shadow rays would take from
   its capture (not pass polygons, not lamp glass), its back faces too. */
static void OccPolygon(U32 type, void *poly) {
    const U16 *w = (const U16 *)poly;
    const U32 polyType = type >> 2;
    if (polyType <= POLY_DITHER_TABLE) {
        U32 glass;
        if (polyType == POLY_SOLID) {
            glass = w[4] & 255;
        } else if (polyType == POLY_FLAT || polyType == POLY_GOURAUD || polyType == POLY_DITHER) {
            glass = (w[4] & 255) | 15;
        } else {
            return;
        }
        S32 rgb[3];
        if (IsGlowColor(glass, rgb)) {
            return;
        }
    } else if (polyType >= POLY_TEXTURE_Z_FOG) {
        return;
    }
    const S32 nbPoints = (type & 1) ? 4 : 3;
    if (s_nbOccBuild + (U32)(nbPoints - 2) > MAX_MESH_BUILD / 3) {
        return;
    }
    const TYPE_VT16 *points = (const TYPE_VT16 *)((U8 *)lBody + lBody->OffPoints);
    for (S32 t = 0; t + 2 < nbPoints; t++) {
        const S32 corners[3] = {0, t + 1, t + 2};
        float *o = s_occBuild + (size_t)s_nbOccBuild * 9;
        for (S32 c = 0; c < 3; c++) {
            const U16 p = w[corners[c]] < lBody->NbPoints ? w[corners[c]] : 0;
            o[c * 3 + 0] = (float)points[p].X;
            o[c * 3 + 1] = (float)points[p].Y;
            o[c * 3 + 2] = (float)points[p].Z;
        }
        s_nbOccBuild++;
    }
}

static const T_DECOR_OCC *DecorOcc(S32 island, S32 index, T_BODY_HEADER *body) {
    if (!s_occReady || island != s_occIsland) {
        for (S32 i = 0; i < MAX_DECOR_MESHES; i++) {
            s_occ[i].Island = -1;
        }
        if (s_occReady) {
            GpuObj_OccReset();
        }
        s_occReady = TRUE;
        s_occIsland = island;
    }
    if (index < 0 || index >= MAX_DECOR_MESHES) {
        return NULL;
    }
    T_DECOR_OCC *occ = &s_occ[index];
    if (occ->Island != island) {
        occ->Island = island;
        occ->Count = 0;
        T_BODY_HEADER *saved = lBody;
        lBody = body;
        s_nbOccBuild = 0;
        BodyWalkPolys(body, OccPolygon);
        lBody = saved;
        /* A lamp's light is placed amid its globes, inside its head: the head
           would put the ground around the post in its own shade. The post
           stays. */
        const T_LAMP_GLASS *glass = GpuObjLampModel ? LampGlass(island, index, body) : NULL;
        if (glass != NULL) {
            U32 kept = 0;
            for (U32 t = 0; t < s_nbOccBuild; t++) {
                const float *v = s_occBuild + (size_t)t * 9;
                float c[3];
                for (S32 a = 0; a < 3; a++) {
                    c[a] = (v[a] + v[3 + a] + v[6 + a]) / 3.0f - glass->Centre[a];
                }
                if (c[0] * c[0] + c[1] * c[1] + c[2] * c[2] <= glass->Reach * glass->Reach) {
                    continue;
                }
                memmove(s_occBuild + (size_t)kept * 9, v, 9 * sizeof(float));
                kept++;
            }
            s_nbOccBuild = kept;
        }
        U32 first = 0;
        float *dst = s_nbOccBuild > 0 ? GpuObj_OccAlloc(s_nbOccBuild, &first) : NULL;
        if (dst != NULL) {
            memcpy(dst, s_occBuild, (size_t)s_nbOccBuild * 9 * sizeof(float));
            occ->First = first;
            occ->Count = s_nbOccBuild;
        }
    }
    return occ->Count > 0 ? occ : NULL;
}

void AffGpu_DecorOccluder(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta) {
    s_occPending = FALSE;
    if (!GpuObj_Active() || AffGpuSuspended || TypeProj != TYPE_3D) {
        return;
    }
    const T_DECOR_OCC *occ = DecorOcc(island, index, (T_BODY_HEADER *)obj);
    if (occ == NULL) {
        return;
    }
    /* BodyDisplay's placement, as AffGpu_DecorDraw takes it. */
    const S32 savedX = X0, savedY = Y0, savedZ = Z0;
    LongRotatePoint(&MatriceWorld, x, y, z);
    const float pos[3] = {(float)(X0 - CameraXr), (float)(Y0 - CameraYr), (float)(Z0 - CameraZr)};
    X0 = savedX;
    Y0 = savedY;
    Z0 = savedZ;
    TYPE_MAT m;
    RotateMatrix(&m, &MatriceWorld, 0, beta, 0);
    const float rows[12] = {m.F.M11, m.F.M12, m.F.M13, m.F.TX + pos[0], m.F.M21, m.F.M22, m.F.M23, m.F.TY + pos[1],
                            m.F.M31, m.F.M32, m.F.M33, m.F.TZ + pos[2]};
    memcpy(s_occRows, rows, sizeof(rows));
    s_occFirst = occ->First;
    s_occCount = occ->Count;
    s_occPending = TRUE;
}

void AffGpu_DecorOccluderEnd(void) {
    s_occPending = FALSE;
}

S32 AffGpu_DecorDraw(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta, S32 box[4]) {
    if (!GpuObj_Active() || AffGpuSuspended || TypeProj != TYPE_3D) {
        return -1;
    }
    T_BODY_HEADER *body = (T_BODY_HEADER *)obj;
    T_DECOR_MESH *mesh = DecorMesh(island, index, body);
    if (mesh == NULL) {
        return -1;
    }
    if (mesh->Shaded && PtrCLUTGouraud == NULL) {
        return -1;
    }

    /* BodyDisplay's placement. */
    LongRotatePoint(&MatriceWorld, x, y, z);
    const float pos[3] = {(float)(X0 - CameraXr), (float)(Y0 - CameraYr), (float)(Z0 - CameraZr)};
    TYPE_MAT m;
    RotateMatrix(&m, &MatriceWorld, 0, beta, 0);
    const float rows[12] = {m.F.M11, m.F.M12, m.F.M13, m.F.TX + pos[0], m.F.M21, m.F.M22, m.F.M23, m.F.TY + pos[1],
                            m.F.M31, m.F.M32, m.F.M33, m.F.TZ + pos[2]};

    /* The model's box on screen: none of it may reach the near plane, where the
       software drops polygons the GPU would clip. */
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (S32 k = 0; k < 8; k++) {
        const float c[3] = {(k & 1) ? mesh->Max[0] : mesh->Min[0], (k & 2) ? mesh->Max[1] : mesh->Min[1],
                            (k & 4) ? mesh->Max[2] : mesh->Min[2]};
        float v[3];
        for (S32 r = 0; r < 3; r++) {
            v[r] = rows[r * 4] * c[0] + rows[r * 4 + 1] * c[1] + rows[r * 4 + 2] * c[2] + rows[r * 4 + 3];
        }
        const float depth = -v[2];
        if (depth <= (float)NearClip + 64.0f) {
            return -1;
        }
        const float sx = (float)XCentre + v[0] * FRatioX / depth;
        const float sy = (float)YCentre + v[1] * FRatioX * FRatioY / depth;
        x0 = sx < x0 ? sx : x0;
        x1 = sx > x1 ? sx : x1;
        y0 = sy < y0 ? sy : y0;
        y1 = sy > y1 ? sy : y1;
    }
    /* Rounded outward, and a pixel more for the rasterizer's own rounding. */
    box[0] = (S32)floorf(x0) - 2;
    box[1] = (S32)floorf(y0) - 2;
    box[2] = (S32)ceilf(x1) + 2;
    box[3] = (S32)ceilf(y1) + 2;
    if (box[0] > ClipXMax || box[2] < ClipXMin || box[1] > ClipYMax || box[3] < ClipYMin) {
        return 0;
    }

    T_GPUOBJ_DRAW *draw = GpuObj_BeginDraw();
    if (draw == NULL) {
        return -1;
    }
    draw->X0 = ClipXMin;
    draw->Y0 = ClipYMin;
    draw->X1 = ClipXMax;
    draw->Y1 = ClipYMax;
    draw->ZBuffer = Fill_Flag_ZBuffer;
    draw->HasLogical = Fill_Flag_Fog;
    if (Fill_Flag_Fog) {
        memcpy(draw->Logical, Fill_Logical_Palette, sizeof(draw->Logical));
    }
    draw->Page = mesh->Textured ? ObjPtrMap : NULL;
    draw->Clut = mesh->Shaded ? PtrCLUTGouraud : NULL;
    draw->Mesh = TRUE;
    draw->First = mesh->First;
    draw->Count = mesh->Count;
    memcpy(draw->MeshRows, rows, sizeof(rows));
    UpdateSceneLight();
    s_light[0] = (float)CameraXLight;
    s_light[1] = (float)CameraYLight;
    s_light[2] = (float)CameraZLight;
    const float lightLen =
        (float)sqrt((double)(s_light[0] * s_light[0] + s_light[1] * s_light[1] + s_light[2] * s_light[2]));
    draw->MeshLight[0] = s_light[0];
    draw->MeshLight[1] = s_light[1];
    draw->MeshLight[2] = s_light[2];
    draw->MeshLight[3] = lightLen * FactorLight / 256.0f;
    {
        /* The model's own space: its axes, its base at the origin, the wind
           carried back from view space through the placement's rotation. */
        static const float axes[9] = {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
        static const float base[3] = {0.0f, 0.0f, 0.0f};
        float view[3], wind[3];
        ViewWind(view);
        for (S32 c = 0; c < 3; c++) {
            wind[c] = rows[c] * view[0] + rows[4 + c] * view[1] + rows[8 + c] * view[2];
        }
        SetTree(draw, body, axes, base, wind);
    }
    GpuObj_EndDraw();
    return 1;
}

// =============================================================================
#ifdef __cplusplus
}
#endif
