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

#ifdef __cplusplus
}
#endif
