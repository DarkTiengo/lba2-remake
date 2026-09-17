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
static T_GPUOBJ_DRAW *s_draw = NULL;

/* Fire-textured polygons of the body being captured: their centres summed. */
static float s_fireSum[3];
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
    s_fireSum[0] = s_fireSum[1] = s_fireSum[2] = 0.0f;
    s_glowCount = 0;
    s_glowSum[0] = s_glowSum[1] = s_glowSum[2] = 0.0f;
    s_glowRgb[0] = s_glowRgb[1] = s_glowRgb[2] = 0.0f;

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
            for (S32 i = 0; i < nbPoints; i++) {
                T_GPU_PROJ p;
                Project(points[i], &p);
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
    }
    s_fireCount = 0;
    s_fireSum[0] = s_fireSum[1] = s_fireSum[2] = 0.0f;
    s_glowCount = 0;
    s_glowSum[0] = s_glowSum[1] = s_glowSum[2] = 0.0f;
    s_glowRgb[0] = s_glowRgb[1] = s_glowRgb[2] = 0.0f;
    s_draw = NULL;
    GpuObj_EndDraw();
}

// =============================================================================
#ifdef __cplusplus
}
#endif
