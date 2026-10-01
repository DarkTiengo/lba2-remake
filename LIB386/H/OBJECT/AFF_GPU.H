#pragma once

#include <SYSTEM/ADELINE_TYPES.H>
#include <SVGA/GPUOBJ.H>

/*
 * Capture of 3D bodies for the GPU object layer (SVGA/GPUOBJ.H), called from
 * the body display in AFF_OBJ.CPP. Every call is a no-op unless the layer is
 * active, so the software path is unchanged when it is off.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Before projection: keeps the rotated points (the isometric projector
 *  overwrites their Z) and records how the body's normals are grouped. */
void AffGpu_SaveRotated(S32 nbPoints, S32 perGroup);

/** Before the sorted entities are rasterized. TRUE when this body is captured. */
S32 AffGpu_Begin(void);

/** After one entity was rasterized; type is the Poly_Jump index. */
void AffGpu_Entity(U32 type, void *poly);

/** After the last entity: closes the draw and tags the pixels it wrote. */
void AffGpu_End(void);

/** A vertex of perspective scenery given in camera space (x right, y down,
 *  depth forward, the units LongProjectPoint uses), for geometry drawn outside
 *  the body display: terrain, sea, sky. Light, normal and material are left to
 *  the caller. */
void AffGpu_ViewVertex(T_GPUOBJ_VERTEX *v, float x, float y, float depth);

/** A glowing sphere replacing the core of a sprite (the magic ball): call Begin
 *  with the sprite's projected centre before the sprite is drawn, End after. The
 *  sprite's pixels inside the sphere show a lit 3D ball at the GPU's resolution,
 *  its halo keeps the software colours. palette is the 768-byte RGB palette the
 *  frame shows, used to take the sphere's colour from the sprite. */
S32 AffGpu_OrbBegin(S32 cx, S32 cy, S32 halfBox);
void AffGpu_OrbEnd(S32 cx, S32 cy, const U8 *palette);

/** A soft shadow for the ground quad whose four edge midpoints are corners (world
 *  space, in ProjectShadow order) on the ground at altitude ground, height its
 *  caster above the ground and tall
 *  the caster's own height. Besides the contact shadow, the caster blocks the
 *  dynamic lights (lamps, fire, magic ball) and throws its shadow away from
 *  them. TRUE when the GPU draws it (the software shadow is then skipped). */
S32 AffGpu_Shadow(const S32 corners[4][3], S32 ground, S32 height, S32 tall);

/** A scene frame starts: forget the last frame's bodies and shadows. */
void AffGpu_BeginScene(void);

/** Every body and shadow of the frame is captured: project each shadow's body
 *  on the ground, away from the sun and from the strongest lights near it. */
void AffGpu_BuildShadows(void);

/** A point light at world position (x, y, z), moved into the space the bodies
 *  of the current projection are captured in (GpuObj_AddLight). */
void AffGpu_AddWorldLight(S32 x, S32 y, S32 z, float radius, float r, float g, float b, float intensity);

/** The scene's focus (GpuObjFocus): a world point, placed as the lights are. */
void AffGpu_SetFocus(S32 x, S32 y, S32 z);

/** TRUE while the body display draws for the software's own use (the depth
 *  a decor left to the GPU is asked for later): nothing is captured. */
extern S32 AffGpuSuspended;

/** A lamp model of the scenery, placed as BodyDisplay(x, y, z, 0, beta, 0, obj)
 *  would place it, listed as a light (GpuObjSceneryLamps) whether it is on
 *  screen or not; the caller sets GpuObjLampModel and GpuObjLampPalette first.
 *  TRUE when listed. */
S32 AffGpu_LampLight(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta);

/** The decor body BodyDisplay(x, y, z, 0, beta, 0, obj) is about to draw
 *  stops light with its whole model (GpuObjOccTris), not with the faces the
 *  capture keeps; AffGpu_DecorOccluderEnd after the display. The caller sets
 *  GpuObjLampModel first, so a lamp's glass is left out. */
void AffGpu_DecorOccluder(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta);
void AffGpu_DecorOccluderEnd(void);

/** A decor body of the scenery placed as BodyDisplay(x, y, z, 0, beta, 0, obj)
 *  would place it, drawn by the GPU from the model it keeps (index the body's
 *  number in the island's objects; island names the objects' file, and the
 *  models kept are dropped when it changes). While GpuObjTreeModel is set the
 *  model is a tree (GPUTREE.H). 1: drawn, box the screen box it may cover;
 *  0: out of the clip window, nothing to draw; -1: not for the GPU to draw
 *  alone, BodyDisplay is still needed. */
S32 AffGpu_DecorDraw(S32 island, S32 index, void *obj, S32 x, S32 y, S32 z, S32 beta, S32 box[4]);

#ifdef __cplusplus
}
#endif
