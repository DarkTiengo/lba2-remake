#ifndef DECORS_H
#define DECORS_H

/*--------------------------------------------------------------------------*/
extern void FixeObjetsDecorsInvisibles(void);
/*--------------------------------------------------------------------------*/
extern int sub_qsort_decors(const void *a, const void *b);
/*--------------------------------------------------------------------------*/
extern int sub_qsort_decors_zbuf(const void *a, const void *b);
/*--------------------------------------------------------------------------*/
extern void AffichageObjetDecorsZV(void);
/*--------------------------------------------------------------------------*/
extern void AffichageObjetDecorsZBuf(void);
/*--------------------------------------------------------------------------*/
extern S32 TestZVDecors(S32 xw, S32 yw, S32 zw, T_DECORS *objet);

/*--------------------------------------------------------------------------*/
/* The decor of the horizon's cubes, left to the GPU (AffGpu_DecorDraw) while
   the software leaves the scenery unpainted (TerrainGpuSkipFill): its depth is
   drawn in later, where it is asked for, as the land's is. */
extern S32 DecorsGpuHorizon; ///< set while a cube of the horizon is drawn
extern void DecorsGpu_Reset(void);
extern S32 DecorsGpu_Pending(void);
/** Draws the depth of the decor left to the GPU inside the box, the rest of the
 *  state (Log, fillers) set by TerrainGpu_EnsureDepth. */
extern void DecorsGpu_EnsureDepth(S32 x0, S32 y0, S32 x1, S32 y1);

#endif // DECORS_H
