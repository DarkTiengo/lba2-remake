#pragma once

#include <SYSTEM/ADELINE_TYPES.H>

/*
 * Bounding volume hierarchy over the frame's captured bodies, for rays traced
 * in the GPU composite (sun shadows over the whole scene). Built on the CPU
 * each scene frame in scene space; the presenter uploads it as two storage
 * buffers.
 *
 * Node n is 8 floats: min xyz, then the first child (inner node, its sibling
 * follows) or the first triangle (leaf); max xyz, then the leaf's triangle count
 * (0 for an inner node). Triangle t is 12 floats: a vertex and two edges, each
 * padded to 4. Indices are stored as floats (exact below 2^24).
 */

#ifdef __cplusplus
extern "C" {
#endif

#define GPUBVH_MAX_TRIS 131072
#define GPUBVH_MAX_NODES (GPUBVH_MAX_TRIS * 2)
#define GPUBVH_NODE_FLOATS 8
#define GPUBVH_TRI_FLOATS 12

extern float GpuBvhNodes[GPUBVH_MAX_NODES * GPUBVH_NODE_FLOATS];
extern float GpuBvhTris[GPUBVH_MAX_TRIS * GPUBVH_TRI_FLOATS];
extern U32 GpuBvhNbNodes;
extern U32 GpuBvhNbTris;

/** Builds the hierarchy over count triangles, 9 floats each (three vertices).
 *  Triangles past GPUBVH_MAX_TRIS are dropped. An empty input leaves no node. */
void GpuBvh_Build(const float *triangles, U32 count);

/** Nearest hit along origin + t * dir for t in (tMin, tMax), or a negative
 *  value: the reference the shader's traversal follows (host tests). */
float GpuBvh_Trace(const float origin[3], const float dir[3], float tMin, float tMax);

#ifdef __cplusplus
}
#endif
