#ifndef BEZIER_H
#define BEZIER_H

#define ALTITUDE_MAX 200

#define FTOW(x) (S16)(((x) + 32767) >> 16)
#define WTOF(x) (S32)((S32)(x) << 16)

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀
typedef S32(Func_ProjectShadow)(T_SORT *, S32);

//▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀▀

extern S32 XS0, YS0, XS1, YS1, XS2, YS2, XS3, YS3;
extern S32 ShadowTestObject;
/* The last projected shadow's four corners in world space, the ground under
   it and its caster, for the GPU renderer's soft shadows. */
extern S32 ShadowCorner[4][3];
extern S32 ShadowGround;     ///< the ground's altitude under the caster
extern S32 ShadowCasterY;    ///< the caster's feet
extern S32 ShadowCasterTall; ///< its height, 0 when unknown

/*--------------------------------------------------------------------------*/
void CalculeLigneOmbre(S32 x1, S32 y1, S32 x2, S32 y2);
/*--------------------------------------------------------------------------*/
void DrawOmbre(S32 x0, S32 y0, S32 x1, S32 y1, S32 x2, S32 y2, S32 x3, S32 y3);
/*--------------------------------------------------------------------------*/
void DrawLineShade(S32 x0, S32 z0, S32 x1, S32 z1, S32 x2, S32 z2, S32 x3, S32 z3);
/*--------------------------------------------------------------------------*/
S32 TestZVDecorsXZFull(S32 xw, S32 zw, S32 y0, S32 y1, T_DECORS *objet);
/*--------------------------------------------------------------------------*/
S32 TestZVObjectsXZFull(S32 xw, S32 zw, S32 y0, S32 y1, T_OBJET *objet);
/*--------------------------------------------------------------------------*/
S32 AdjustShadowDecors(S32 xw, S32 zw, S32 y0, S32 y1);
/*--------------------------------------------------------------------------*/
S32 AdjustShadowObjects(S32 xw, S32 zw, S32 y0, S32 y1);
/*--------------------------------------------------------------------------*/
extern Func_ProjectShadow ProjectShadowExt;
/*--------------------------------------------------------------------------*/
void DrawShadow(void);
/*--------------------------------------------------------------------------*/

#endif // BEZIER_H
