#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

#ifdef __cplusplus
extern "C" {
#endif

/* Trees in the wind (gfx_trees). A decor model the game lists as a tree is
   drawn with its leaves shaded as foliage and the whole model swaying: bent
   along the island's one wind (the grass's), more the higher a point stands
   above the model's base, with gusts that travel over the island as they do
   over the grass, and leaves fluttering in them. Only the GPU draws it: the
   software pixels, the depth DrawRecover reads and the shadow rays keep the
   model standing still. GPUOBJ.vert holds the same formula (TreeSway). */

extern S32 GpuTreeEnabled; ///< gfx_trees

/* Set by the caller while it draws a tree decor (GpuTree_Begin/GpuTree_End). */
extern S32 GpuObjTreeModel;
extern U32 GpuObjTreeFoliage; ///< palette banks that are leaves, GPUTREE_FOLIAGE_TEXTURED for textured polygons
extern float GpuObjTreePhase; ///< where the tree stands in the gusts

#define GPUTREE_FOLIAGE_TEXTURED (1u << 16) ///< the model's textured polygons are leaves

#define GPUTREE_HEIGHT 3000.0f ///< model units at which a tree bends by GPUTREE_BEND
#define GPUTREE_BEND 60.0f     ///< how far the wind carries that point at its strongest
#define GPUTREE_FLUTTER 14.0f  ///< how far a leaf flutters

/* The wind the grass bends in, over the whole island (x, z in the world). */
#define GPUTREE_WIND_X 0.94f
#define GPUTREE_WIND_Z 0.34f

/* A tree's placement, in the space its draw's vertices are written in: its
   base, the world's up and the wind as unit vectors there, the middle of its
   leaves and how far they reach. */
typedef struct {
    float Base[3];
    float Up[3];
    float Wind[3];
    float Centre[3];
    float Radius; ///< 0 when the model has no leaves (a palm's trunk)
    float Phase;
} T_GPUTREE;

/** The caller draws a tree decor standing at world (x, z): foliage as in
 *  GpuObjTreeFoliage. */
void GpuTree_Begin(U32 foliage, S32 worldX, S32 worldZ);
void GpuTree_End(void);

/** Where a tree standing at world (x, z) lies in the gusts: the grass's field
 *  (along the wind), with a little of its own. */
float GpuTree_Phase(S32 worldX, S32 worldZ);

/** Whether a polygon of a tree is a leaf: its bank (colour >> 4) is listed, or
 *  it is textured and the model's textured polygons are. */
S32 GpuTree_IsFoliage(U32 foliage, S32 textured, U32 color);

/** The draw uniform's four vec4 for a tree (zeros: not a tree). */
void GpuTree_Uniform(const T_GPUTREE *tree, float out[16]);

/** How far the wind carries a point of the tree `rel` from its base (in the
 *  draw's space) at `time` seconds, with the wind's strength (1 a breeze):
 *  GPUOBJ.vert's TreeSway. */
void GpuTree_Sway(const T_GPUTREE *tree, const float rel[3], S32 foliage, float time, float strength, float out[3]);

#ifdef __cplusplus
}
#endif
