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

/** Smooth terrain (gfx_smoothterrain): land cells are cut into smaller
 *  triangles on a curved surface through the height map, finer near the camera. */
extern S32 TerrainGpuSmooth;

/** Grass (gfx_grass): blades standing on the ground the island marks as grass. */
extern S32 TerrainGpuGrass;

/** The scene's 768-byte RGB palette, to tell grass from the earth it borders. */
extern const U8 *TerrainGpuPalette;

/** View distance with the GPU renderer (gfx_viewdistance): 0 the classic one,
 *  1 far, 2 very far. The fog moves out and more cubes are drawn around the
 *  camera; the classic renderer keeps the original. */
extern S32 TerrainGpuViewDistance;

/** How many cubes around the camera's are drawn (1: the classic 3 x 3). */
S32 TerrainGpu_HorizonRadius(void);

/** The factor on the island's fog distances (1 for the classic view). */
float TerrainGpu_FogScale(void);

/** Opens the draw for one terrain cube. heights is the 65x65 vertex height map
 *  of the cube as currently loaded; clutBase the unfogged 16-row Gouraud block. */
void TerrainGpu_BeginCube(const S16 *heights, U8 *groundTexture, U8 *clutBase, S32 fogStart, S32 fogEnd,
                          S32 fogColor);

/** One terrain triangle, as filled by DrawFeuillePolyZBuf. corners index the
 *  cell's corners (0 = (x, z), 1 = (x, z+1), 2 = (x+1, z+1), 3 = (x+1, z)),
 *  lights are the 8.8 vertex intensities, fillType and color what Fill_Poly
 *  got, uv the six texel coordinates (8.8) or NULL for an untextured fill,
 *  step the footstep sound the ground makes (T_HALF_POLY.SampleStep).
 *  Returns TRUE when the GPU will draw this triangle's colour, so the software
 *  fill beside it is only needed for its tag and its depth. */
S32 TerrainGpu_Tri(S32 xi, S32 zi, const U8 corners[3], const S32 lights[3], S32 fillType, S32 color,
                   const U16 *uv, S32 water, S32 step);

/* The software terrain fill is flat where the GPU covers it (gfx_fastterrain):
 * the pixel is wanted for its tag and its depth, not for its colour. */
extern S32 TerrainGpuFastFill;

/** Captures the main cube's camera before horizon cubes shift it. */
void TerrainGpu_SetWaterScene(S32 island, S32 cubeX, S32 cubeZ);

/** Selects the island cube whose local sea coordinates are being captured. */
void TerrainGpu_SetWaterCube(S32 cubeX, S32 cubeZ);

/** Opens a sea or sky plane and marks the material for the GPU compositor. */
void TerrainGpu_BeginPlane(U8 *page, S32 texOffset, S32 texMask, S32 fogStart, S32 fogEnd, S32 fogColor,
                           S32 sea);

/** One plane quad in camera space, as handed to Draw_Poly. */
void TerrainGpu_Quad(const STRUC_CLIPVERTEX src[4]);

/** Closes the draw opened by BeginCube or BeginPlane. */
void TerrainGpu_End(void);

#ifdef __cplusplus
}
#endif
