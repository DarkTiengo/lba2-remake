#include <OBJECT/AFF_GPU.H>

#include <OBJECT/AFF_OBJ.H>
#include <3D/LROT3D.H>
#include <3D/CAMERA.H>
#include <3D/DATAMAT.H>
#include <3D/LIGHT.H>
#include <3D/PROJ.H>
#include <POLYGON/POLY.H>
#include <SVGA/CLIP.H>
#include <SVGA/GPUOBJ.H>
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
#define PERSPECTIVE_DEPTH_RANGE 131072.0f
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
static T_GPUOBJ_DRAW *s_draw = NULL;

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
    s_pending = GpuObj_Active();
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
    if (draw->HasGlow && s_nbLamps < MAX_FRAME_LAMPS) {
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
        return;
    }
    UpdateSceneLight();
    /* Exteriors under a risen sun: the presenter traces sun rays through every
       body, so the sun needs no silhouettes. */
    GpuObjRayScene =
        GpuObjRayTrace && GpuObjSunlight && TypeProj == TYPE_3D && Dot3(GpuObjSunDir, GpuObjUpDir) > 0.05f;
    /* Lights that can shadow: the scene's own and the lamps and fires on bodies. */
    T_FRAME_LIGHT lights[GPUOBJ_MAX_LIGHTS + MAX_FRAME_LAMPS];
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
        for (S32 k = 0; k < 2; k++) {
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
        KeepCaster(s_draw);
    }
    s_draw = NULL;
    GpuObj_EndDraw();
}

// =============================================================================
#ifdef __cplusplus
}
#endif
