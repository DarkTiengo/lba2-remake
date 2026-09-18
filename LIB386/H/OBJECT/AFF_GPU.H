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

#ifdef __cplusplus
}
#endif
