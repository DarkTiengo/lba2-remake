# GPU renderer

An opt-in renderer that draws the game's 3D bodies (actors, decor objects, extras, inventory and menu models) and the exterior scenery (terrain, sea, sky) on the GPU, with smooth shading, filtered textures, per-pixel lighting on bodies and continuous distance fog. Interior rooms keep their 1997 brick art, smoothly upscaled, with actors occluded by real depth. Sprites and the 2D UI keep the original software look. Off by default: a stock run renders exactly as before.

## Using it

| Control | Where | Effect |
|---------|-------|--------|
| Renderer: GPU / Classic | Options → Display menu | Switches renderer immediately, no restart. Saved to lba2.cfg as `GpuRenderer`. |
| `gfx_gpu 0/1` | console | Same switch as the menu row. |
| GPU effects | Options → Display menu (while the renderer is GPU) | Page of on/off selectors for the effects below, applied immediately. |
| `gfx_uiscale 0/1` | console | 2D size. `1` (default) keeps the software frame at the classic height (480 to 719 lines, e.g. 960x540 for a 1920x1080 choice) so menus, text, sprites and rooms keep their authored size on screen and reach it through the pixel-art upscaler, while 3D still renders at the full resolution. `0` draws 2D natively (small UI at high resolutions). Saved as `GpuUiScale`; applied when the GPU effects page is left or at the next scene. |
| `gfx_lights 0/1` | console | Dynamic lights (room lamps, magic ball glow, soft light following the hero in rooms). Saved as `GpuLights`, on by default. |
| `gfx_pixelfilter 0/1` | console | xBR pixel-art filter on upscaled software pixels (bricks, sprites, UI). Saved as `GpuPixelFilter`, on by default. |
| `gfx_deband 0/1` | console | In-between colours on the palette's ramps for upscaled software pixels. Saved as `GpuDeband`, on by default. |
| `gfx_lamps 0/1` | console | Lamp globes on 3D models (street lamps) cast light. Saved as `GpuLamps`, on by default. |
| `gfx_fire 0/1` | console | Procedural flames and flickering firelight on the game's animated fire textures. Saved as `GpuFire`, on by default. |
| `gfx_specular 0/1` | console | Specular highlights on 3D models. Saved as `GpuSpecular`, on by default. |
| `LBA2_GPU=1` / `LBA2_GPU=0` | environment | Forces the choice for this run only, without changing the saved one. With `1` the window is created with the GPU renderer from the first frame. |
| `LBA2_GPU_DEBUG=1` | environment | Tints every pixel the GPU supplies green, to check coverage. `2` shows the GPU image alone (magenta where it drew nothing), without the software frame. |
| `LBA2_GPU_CAPTURE=<file.bmp>` | environment | Writes the last presented frame to disk (GPU composite and menu overlay included, with either renderer); add `LBA2_CAPTURE_MENU=1` to keep only frames with the modern menu up. `--screenshot` saves the software frame, so this is how to capture the GPU look. |
| `LBA2_GPU_STATS=1` | environment | Logs the CPU cost of each present (tag scan, uploads, render) every 300 presents. |

If the GPU renderer cannot be created (no Vulkan device, headless run), the window keeps the classic renderer and logs a warning; the saved choice is left as the player set it.

```bash
LBA2_GPU=1 ./lba2cc
LBA2_GPU=1 LBA2_GPU_CAPTURE=gpu.bmp ./lba2cc --exec "cube 42" --tick 200 --exit
```

It needs SDL 3.4 or newer at build time; a build against an older SDL compiles the layer out and `LBA2_GPU` does nothing. Shaders are SPIR-V only for now, so the layer runs where SDL GPU uses Vulkan (Linux, Windows, Android). Metal and Direct3D builds fall back to the software look.

## How it works

The software rasterizer still draws every body into `Log`. That keeps every visibility rule of the original intact: bricks redrawn over actors in interiors, the exterior z-buffer against the terrain, menus and text on top. The GPU only replaces the colour of pixels a body owns.

1. **Capture** ([LIB386/OBJECT/AFF_GPU.CPP](../LIB386/OBJECT/AFF_GPU.CPP), [SOURCES/3DEXT/TERRAIN_GPU.CPP](../SOURCES/3DEXT/TERRAIN_GPU.CPP)). While `ObjectDisplay` / `BodyDisplay` rasterize a body, the capture records its entities as GPU vertices: positions projected on the CPU with the engine's own perspective or isometric formulas (so silhouettes match), view-space normals, palette colour, texture coordinates. Transparent, pattern and table fills and lines are recorded as "pass" geometry, which makes their pixels keep the software colour. The exterior scenery is captured next to its software fills in `TERRAIN.CPP` and `DRAWSKY.CPP`: every terrain cell of the current and horizon cubes with its authored vertex intensities, colour bank, Gouraud table and texture coordinates, and the sea and sky planes in camera space. Interior rooms are captured in `AffGrille` ([SOURCES/GRILLE_GPU.CPP](../SOURCES/GRILLE_GPU.CPP)): every drawn brick becomes a quad over its sprite, whose coverage comes from a brick atlas and whose depth the fragment shader computes per pixel by casting the isometric view ray through the brick's grid cell (the same depth formula as isometric bodies). Bricks overwrite each other in software order, as their sprites overlap by design, and only their depth is kept for actors to test against; `DrawOverBrick`'s masked re-blits carry the bricks' tags along.
2. **Tagging** ([LIB386/SVGA/GPUOBJ.CPP](../LIB386/SVGA/GPUOBJ.CPP)). Every pixel the body changes in `Log` gets the draw's id and the palette index it wrote. The exterior scenery pass (`AffGrilleExt`) is one group: all its draws share one id and the frame is diffed once, after the pass, against the cleared frame it started from. Tag planes follow the pixels when `CopyScreen`, `CopyBlock` or the dirty-box restore copy between `Log`, `Screen` and full-frame buffers (`ScreenAux`, `BufSpeak`), so a body baked into the background, or a scene kept behind a menu, keeps its id.
3. **Present** ([LIB386/SYSTEM/GPURENDER.CPP](../LIB386/SYSTEM/GPURENDER.CPP)). The GPU targets match the frame's size on screen (up to 3840x2160), not the software frame's, so models render at the window's resolution: a 1920x1080 frame on a 4K window gets 4K models. A pixel is live when its tag is set and `Log` still holds the index the body wrote. Every present re-renders each draw that still has a live pixel, with the current palette, fog remap, CLUT and texture pages, into a colour target and an id target. Drawing order follows the software painter's order through depth slices; consecutive z-buffered bodies (exterior decors, drawn front to back) share a slice and let depth decide.
   Draws opened during the scene part of `AffScene` (exterior scenery or interior bricks, actors, extras) are scene draws: they share the back half of the depth range and depth-test each other for real, while menu and inventory models keep the draw-order slices in front of them.
4. **Composite** ([LIB386/SYSTEM/SHADERS/COMPOSITE.frag](../LIB386/SYSTEM/SHADERS/COMPOSITE.frag)). The SDL renderer draws the software frame through a custom fragment shader: where the pixel's tag equals the id the GPU wrote, or where both are scene ids (the GPU's depth then picks the surface, at the GPU's resolution), the GPU colour replaces it, except where the nearest surface is an interior brick, which shows the software frame; anywhere else the software colour stays. Wherever the frame is shown larger than its pixels, software pixels (bricks, sprites, UI) go through a pixel-art upscaler (xBR level 2 with blended edges) and a debanding pass that averages neighbours of nearly the same colour, filling the palette's 16-step ramps with in-between colours without crossing edges. Scene pixels are then multiplied by the dynamic light the GPU wrote to a third target.

Dynamic lights ([SOURCES/OBJECT.CPP](../SOURCES/OBJECT.CPP) `AffScene`, `AffGpu_AddWorldLight`) are up to 16 point lights set each scene frame in world coordinates and moved into the space the capture stores positions in (camera-relative world in rooms, view space outdoors). Bodies take them through their normals, terrain and bricks omnidirectionally at their surface point (bricks use the point their depth ray found). The original data has no light sources. Room lamps are listed by hand in [SOURCES/GRILLE_GPU.CPP](../SOURCES/GRILLE_GPU.CPP) (`kLampBricks`) by their brick number in `BKG.HQR`, found by checking every room that uses each candidate: lamp shades, wall sconces, globe lamps, glowing fixtures, crystal lamps, candles. `LoadUsedBrick` maps the room's sprites back to those numbers; each drawn lamp (stacked lamp bricks merged into one) lights with the average colour of its brightest pixels in the room's palette, nearest to the camera first. The magic ball and a soft light over the hero in rooms emit too; the ball's light stops when it bursts (the extra that flies home is an invisible seeker, `EXTRA_SEARCH_OBJ`).

**Fire.** The object texture page declares animated fire regions (`ScanTextureAnimation`, 32x32 each, rewritten by `Do_Fire` every frame); the page and regions are handed to the GPU layer (`GpuObj_SetFireRects`). With `gfx_fire` on (Display > GPU effects > Modern fire, `GpuFire`), fragments of textured polygons that sample a fire region draw procedural flames instead (layered noise rising over time, a white-yellow root through orange to red ragged tips, unshaded), and every live body with such polygons casts a warm flickering light at their centre (`T_GPUOBJ_DRAW::HasFire`, added by the presenter each present, so bodies baked into a room's background keep lighting it). Fire made of many small bodies close together (a dragon's breath) casts one light. Braziers, fire breath and the laser barriers that reuse the fire texture all take it.

**Lamps on models.** Island decor models that are lamps are listed by island and model number (`kLampModels` in [SOURCES/3DEXT/DECORS.CPP](../SOURCES/3DEXT/DECORS.CPP), found by looking: Citadel Island's street lamp is model 26). While one is drawn, `GpuObjLampModel` tells the capture, which marks its pale, bright, warm glass (judged in the scene palette, since decors are captured while the video palette still fades in) emissive: the shader lights it from inside and the composite draws a bloom halo around it, and the body casts a steady light in the glass colour from its centre, with a larger radius in exterior scenes. With `gfx_lamps` on (Display > GPU effects > Lamps on models, `GpuLamps`). Other islands' lamp models are not listed yet.

**Glow.** Emissive surfaces (fire flames, lamp glass) write their strength to the light target's alpha; the composite adds a soft halo from three rings of taps around each pixel, over GPU and software pixels alike, so flames lose their hard polygon edges and lamps read as light sources.

**Magic ball.** While the ball flies, its sprite's core is replaced by a lit glossy sphere (`GPUOBJ_MODE_ORB`, `AffGpu_OrbBegin/End` around the sprite draw in `AffOneObject`): tinted from the sprite's own saturated pixels, centred on what the sprite drew, with a hot core, a coloured rim and a sharp highlight. It uses exact id matching rather than scene depth, so the sprite's halo keeps its software pixels.

**2D scale.** With `gfx_uiscale` on, `Res_FrameDivisor` ([SOURCES/RES_SWITCH.CPP](../SOURCES/RES_SWITCH.CPP)) divides the chosen resolution down to the classic height whenever the GPU renderer is on (not headless). The GPU targets still follow the window and the chosen size, so bodies, terrain, sea and sky and the depth of bricks are rendered at full resolution, and everything drawn in 2D is magnified by the composite's xBR filter.

Colours still come from the original data. Terrain keeps its authored per-vertex intensities, interpolated per pixel through the unfogged Gouraud table rows; its 16 fog steps become a continuous fade to the fog colour by view depth. The shade index a Gouraud or flat polygon would use is computed per pixel from interpolated normals, then blended between neighbouring entries of its 16-step palette ramp (or CLUT rows for textured polygons), so banding and dithering disappear without changing the palette. A specular term is added on top. Textures are filtered bilinearly in palette space.

Re-rendering live draws each present, rather than keeping their colour, is what lets palette fades and flashes reach GPU pixels: decor objects are drawn once when a scene is entered, often while the palette is still faded to black.

## Limits and known gaps

- Pixels a body writes with the same palette index that was already there are not tagged, and keep the software colour.
- Where another sprite or text later writes exactly the index a body had written, that pixel shows the body's GPU colour.
- A body switching texture page or CLUT mid-way keeps the software colour for the rest of its polygons.
- Tags are scanned and uploaded over the box that holds them each present: cheap in interiors, about 2 ms at 1280x960 in a full exterior view.
- Where the GPU renders above the software frame's resolution, edges against software pixels (sprites, shadows, rain, interior bricks) still follow the software frame's pixels; edges between scene surfaces in exteriors do not.
- Sprites, particles, rain and shadows are still software, and so is the interior brick colour (upscaled with xBR).
- Only the lamps in `kLampBricks` emit: lamps modelled as 3D objects, torches and fires in exteriors do not. A lamp brick just off screen does not light the edge of the view.
- `gfx_gpu` or `gfx_uiscale` typed in the console resize the frame at the next scene; the menu applies them at once.
- A brick's depth is its whole grid cell, as the software occlusion rules assume: furniture thinner than a cell hides an actor inside the same cell.

## Changing the shaders

The GLSL sources live in `LIB386/SYSTEM/SHADERS/`. The build embeds the committed `*.spv.h` headers, so no shader compiler is needed to build. After editing a shader, regenerate them with [scripts/dev/build-gpu-shaders.sh](../scripts/dev/build-gpu-shaders.sh) (needs `glslc` from shaderc or the Vulkan SDK, and `xxd`).
