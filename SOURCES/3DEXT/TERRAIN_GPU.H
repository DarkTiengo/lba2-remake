#pragma once

#include <SYSTEM/ADELINE_TYPES.H>
#include <POLYGON/CLIPPERZ.H>

/*
 * Capture of the exterior scenery for the GPU object layer (SVGA/GPUOBJ.H):
 * the terrain cells, the sea and the sky, recorded next to the software fills
 * in TERRAIN.CPP / DRAWSKY.CPP with the same polygon types and colours. Every
 * call is a no-op unless the layer is active. AffGrilleExt wraps the whole
 * scenery pass in one group (GpuObj_BeginGroup / GpuObj_EndGroup).
 *
 * The GPU keeps the original look (palette ramps, Gouraud tables, textures)
 * but blends shades smoothly, filters textures and replaces the 16 fog steps
 * with continuous distance fog.
 */

#ifdef __cplusplus
extern "C" {
#endif

/** Opens the draw for one terrain cube. heights is the 65x65 vertex height map
 *  of the cube as currently loaded; clutBase the unfogged 16-row Gouraud block. */
void TerrainGpu_BeginCube(const S16 *heights, U8 *groundTexture, U8 *clutBase, S32 fogStart, S32 fogEnd,
                          S32 fogColor);

/** One terrain triangle, as filled by DrawFeuillePolyZBuf. corners index the
 *  cell's corners (0 = (x, z), 1 = (x, z+1), 2 = (x+1, z+1), 3 = (x+1, z)),
 *  lights are the 8.8 vertex intensities, fillType and color what Fill_Poly
 *  got, uv the six texel coordinates (8.8) or NULL for an untextured fill. */
void TerrainGpu_Tri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], S32 fillType, S32 color,
                    const U16 *uv);

/** Opens the draw for a textured plane (sea, sky) on page + texOffset. */
void TerrainGpu_BeginPlane(U8 *page, S32 texOffset, S32 texMask, S32 fogStart, S32 fogEnd, S32 fogColor);

/** One plane quad in camera space, as handed to Draw_Poly. */
void TerrainGpu_Quad(const STRUC_CLIPVERTEX src[4]);

/** Closes the draw opened by BeginCube or BeginPlane. */
void TerrainGpu_End(void);

#ifdef __cplusplus
}
#endif
