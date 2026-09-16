# GPU renderer for 3D bodies

An opt-in renderer that draws the game's 3D bodies (actors, decor objects, extras, inventory and menu models) on the GPU with per-pixel lighting and true colour, while everything else keeps the original software look. Off by default: a stock run renders exactly as before.

## Using it

| Control | Where | Effect |
|---------|-------|--------|
| `LBA2_GPU=1` | environment, read when the window is created | Creates an SDL GPU renderer (Vulkan on Linux) and enables the layer. If the device, shaders or formats are missing, the window falls back to the default renderer and logs why. |
| `gfx_gpu 0/1` | console | Switches between the GPU look and the software look at run time (needs `LBA2_GPU=1` at launch). |
| `LBA2_GPU_DEBUG=1` | environment | Tints every pixel the GPU supplies green, to check coverage. |
| `LBA2_GPU_CAPTURE=<file.bmp>` | environment | Writes the last composited frame to disk. `--screenshot` saves the software frame, so this is how to capture the GPU look. |
| `LBA2_GPU_STATS=1` | environment | Logs the CPU cost of each present (tag scan, uploads, render) every 300 presents. |

```bash
LBA2_GPU=1 ./lba2cc
LBA2_GPU=1 LBA2_GPU_CAPTURE=gpu.bmp ./lba2cc --exec "cube 42" --tick 200 --exit
```

It needs SDL 3.4 or newer at build time; a build against an older SDL compiles the layer out and `LBA2_GPU` does nothing. Shaders are SPIR-V only for now, so the layer runs where SDL GPU uses Vulkan (Linux, Windows, Android). Metal and Direct3D builds fall back to the software look.

## How it works

The software rasterizer still draws every body into `Log`. That keeps every visibility rule of the original intact: bricks redrawn over actors in interiors, the exterior z-buffer against the terrain, menus and text on top. The GPU only replaces the colour of pixels a body owns.

1. **Capture** ([LIB386/OBJECT/AFF_GPU.CPP](../LIB386/OBJECT/AFF_GPU.CPP)). While `ObjectDisplay` / `BodyDisplay` rasterize a body, the capture records its entities as GPU vertices: positions projected on the CPU with the engine's own perspective or isometric formulas (so silhouettes match), view-space normals, palette colour, texture coordinates. Transparent, pattern and table fills and lines are recorded as "pass" geometry, which makes their pixels keep the software colour.
2. **Tagging** ([LIB386/SVGA/GPUOBJ.CPP](../LIB386/SVGA/GPUOBJ.CPP)). Every pixel the body changes in `Log` gets the draw's id and the palette index it wrote. Tag planes follow the pixels when `CopyScreen`, `CopyBlock` or the dirty-box restore copy between `Log`, `Screen` and full-frame buffers (`ScreenAux`, `BufSpeak`), so a body baked into the background, or a scene kept behind a menu, keeps its id.
3. **Present** ([LIB386/SYSTEM/GPURENDER.CPP](../LIB386/SYSTEM/GPURENDER.CPP)). A pixel is live when its tag is set and `Log` still holds the index the body wrote. Every present re-renders each draw that still has a live pixel, with the current palette, fog remap, CLUT and texture pages, into a colour target and an id target. Drawing order follows the software painter's order through depth slices; consecutive z-buffered bodies (exterior decors, drawn front to back) share a slice and let depth decide.
4. **Composite** ([LIB386/SYSTEM/SHADERS/COMPOSITE.frag](../LIB386/SYSTEM/SHADERS/COMPOSITE.frag)). The SDL renderer draws the software frame through a custom fragment shader: where the pixel's tag equals the id the GPU wrote, the GPU colour replaces it; anywhere else the software colour stays.

Colours still come from the original data. The shade index a Gouraud or flat polygon would use is computed per pixel from interpolated normals, then blended between neighbouring entries of its 16-step palette ramp (or CLUT rows for textured polygons), so banding and dithering disappear without changing the palette. A specular term is added on top. Textures are filtered bilinearly in palette space.

Re-rendering live draws each present, rather than keeping their colour, is what lets palette fades and flashes reach GPU pixels: decor objects are drawn once when a scene is entered, often while the palette is still faded to black.

## Limits and known gaps

- Pixels a body writes with the same palette index that was already there are not tagged, and keep the software colour.
- Where another sprite or text later writes exactly the index a body had written, that pixel shows the body's GPU colour.
- A body switching texture page or CLUT mid-way keeps the software colour for the rest of its polygons.
- Tags are scanned and uploaded over the box that holds them each present: cheap in interiors, about 2 ms at 1280x960 in a full exterior view.
- Terrain, sky, sea, interior bricks and sprites are still software.

## Changing the shaders

The GLSL sources live in `LIB386/SYSTEM/SHADERS/`. The build embeds the committed `*.spv.h` headers, so no shader compiler is needed to build. After editing a shader, regenerate them with [scripts/dev/build-gpu-shaders.sh](../scripts/dev/build-gpu-shaders.sh) (needs `glslc` from shaderc or the Vulkan SDK, and `xxd`).
