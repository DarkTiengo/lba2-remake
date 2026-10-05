# Graphical quality options

Variables and locations that can be changed or exposed as options to improve graphical quality, increase details and effects, etc.

## Shipped options

All three are off by default, so a stock run renders exactly as before. None of them has a menu entry yet; set them from the console or the config file.

| Option | Console | lba2.cfg | Environment | Values |
|--------|---------|----------|-------------|--------|
| Texture filtering | `gfx_texfilter` | `TextureFilter` | `LBA2_TEXFILTER` | 0 off, 1 horizontal 2-tap, 2 bilinear 4-tap |
| Dithered shading | `gfx_dither` | `DitherShading` | (none) | 0 off, 1 on |
| Interior render scaling | (none) | (none) | `LBA2_ISO_DIV` | 1 off, 2 to 4; unset = follows the frame scaling below |
| GPU renderer | `gfx_gpu` | `GpuRenderer` | `LBA2_GPU` | 0 classic, 1 GPU (Display menu: Renderer); see [GPU_RENDERER.md](GPU_RENDERER.md) |
| GPU modern water | `gfx_water` | `GpuWater` | — | 1 procedural waves, rain-driven rough seas, beach/pier breakers and contact splashes on every exterior sea; 0 original material; GPU renderer only (Display > GPU effects > Scenery) |
| Menu style | `gfx_menustyle` | `MenuStyle` | — | 0 classic, 1 modern cards, 2 modern with the GPU renderer (default); see [MENU.md](MENU.md) |
| GPU 2D scale | `gfx_uiscale` | `GpuUiScale` | — | 1 classic size upscaled, 0 native; GPU renderer only |
| GPU lamps on models | `gfx_lamps` | `GpuLamps` | — | 1 lamp globes on models cast light, 0 off; GPU renderer only |
| GPU view distance | `gfx_viewdistance` | `GpuViewDistance` | — | 0 classic, 1 far (default), 2 very far: more cubes around the camera and the fog farther out; about 2–3 ms (far) and 4–6 ms (very far) a frame; GPU renderer only (Display > GPU effects > Scenery) |
| GPU auto-adjust | `gfx_autotune` (command) | `GpuAutoTuned` | — | measures this machine outdoors and turns the costliest GPU effects off until frames hold 60 fps; runs by itself once, then on request (Display > Auto-adjust) |
| GPU smooth terrain | `gfx_smoothterrain` | `GpuSmoothTerrain` | — | 1 terrain cells near the camera curved into smooth hills (at most 220 units above the original ground, never below); 0 the original flat cells; GPU renderer only (Display > GPU effects > Scenery) |
| GPU grass | `gfx_grass` | `GpuGrass` | — | 1 short tufts of grass blades swaying in the wind where the island has grass, within the near part of the view; no measurable cost; GPU renderer only (Display > GPU effects > Scenery) |
| GPU trees | `gfx_trees` | `GpuTrees` | — | 1 trees and bushes sway in the island's wind and their leaves are shaded as foliage (light wrapping round the crown, clumps of leaves, the sun shining through); 0 the models as the palette shades them; no measurable cost; GPU renderer only (Display > GPU effects > Scenery) |
| GPU modern sky | `gfx_sky` | `GpuSky` | — | 1 procedural sky with sun, clouds and stars, distant land fading into it; 0 the original sky texture and fog colour; GPU renderer only (Display > GPU effects > Scenery) |
| GPU rain and lightning | `gfx_storm` | `GpuStorm` | — | 1 layered rain with splashes, lightning bolts and flashes lighting the scene; 0 the original rain lines and lightning palette; GPU renderer only (Display > GPU effects > Scenery) |
| GPU global light | `gfx_sunlight` | `GpuSunlight` | — | 1 sun shadows, sky fill and rim light, sun/sky grading, sun bloom, outdoor ambient occlusion; 0 the palette's lighting alone; GPU renderer only (Display > GPU effects > Lighting and shadows) |
| GPU ray tracing | `gfx_raytrace` | `GpuRayTrace` | — | 1 shadows ray traced outdoors through every body and the terrain, from the sun and the lights; 0 the characters' silhouettes only; GPU renderer only (Display > GPU effects > Lighting and shadows) |
| GPU soft shadows | `gfx_shadows` | `GpuShadows` | — | 1 soft shadows that bodies also cast away from dynamic lights, 0 the original shadow; GPU renderer only (Display > GPU effects > Lighting and shadows) |
| GPU modern fire | `gfx_fire` | `GpuFire` | — | 1 procedural flames and firelight, 0 classic fire texture; GPU renderer only |
| GPU dynamic lights | `gfx_lights` | `GpuLights` | — | 1 on, 0 off; GPU renderer only (Display menu: GPU effects) |
| GPU pixel-art filter | `gfx_pixelfilter` | `GpuPixelFilter` | — | 1 xBR on upscaled 2D art, 0 plain; GPU renderer only |
| GPU colour smoothing | `gfx_deband` | `GpuDeband` | — | 1 on, 0 off; GPU renderer only |
| GPU texture detail | `gfx_texturedetail` | `GpuTextureDetail` | — | 0 current filtered pages, 1 enhanced edge-preserving sampling, 2 optional 4x RGBA pack with enhanced fallback; GPU renderer only |
| GPU specular highlights | `gfx_specular` | `GpuSpecular` | — | 1 on, 0 off; GPU renderer only |

**Texture filtering** smooths magnified texels on terrain, sea, and sky. The rasterizer works in palette indices, where averaging two entries is meaningless, so the blend is a precomputed table of the nearest palette index to each 25/50/75% RGB mix. Costs roughly 7% of frame time on terrain and 4% on a sea-heavy view at 1728x1080 with the 4-tap setting.

**Dithered shading** applies an ordered dither to Gouraud shade rows, which softens the 16-step banding of the original palette ramps.

**GPU texture detail** affects textured terrain and 3D models, not sprites, rooms or the UI. Enhanced mode holds palette texels more firmly when enlarged and takes a wider, repeat-mask-safe footprint at distance. Pack mode looks under the active user directory for `texture-pack/pack.ini` (`Format=1`, `Scale=4`) and optional `islands/<name>/ground.png`, `objects.png` and `skysea.png` RGBA pages. For Citadel, an optional `terrain.png` adds world-projected earth and sand detail to palette-only land (brown bank 1 and ochre bank 6); its left 512×512 tiles are earth above sand. Gray cement and other colour banks keep their original appearance. RGB is replacement albedo and alpha is its strength for authored pages; the original texture still owns chromakey, illumination, fog and animated rectangles. Missing or invalid pages use enhanced sampling. Pack mode also repaints the Citadel buggy and harbour boat, trees and bushes, and identified street and bench metal with embedded original albedo materials, without requiring an external texture pack. Clean satin-metal grain is overlaid on the vehicles' and fixtures' untextured palette polygons, preserving their original colours; the harbour scene's visible boat actor is included. Its window polygons become translucent glass over a small render-only cabin backdrop. It subdivides curved Citadel decor for a smoother GPU rendering, while preserving planar architecture, the original corners and collision. Modes 0–1 and the classic renderer keep their original appearance. Use `scripts/dev/texture_pack.py` to export local sources from a legitimate installation and validate a pack. Exported retail art is not redistributable and must not be committed.

**Interior render scaling** renders isometric interiors at 1/N of the chosen resolution and lets the present stretch them back, so the room is drawn larger. Nothing in an interior scales with the framebuffer (bricks are pre-rendered sprites and the iso projectors take no focal argument), so a bigger framebuffer otherwise just reveals more empty space around the room: it covers 29% of the frame at 1728x1080 against 82% at 640x480. Exteriors, whose projection does carry a focal, always render at full resolution.

`LBA2_ISO_DIV` is read once at startup and cannot be changed mid-run. It is also ignored when the divided size would fall below the engine's 320x200 minimum, which is silent: at 640x480 a divisor of 2 works and 3 does nothing.

```
LBA2_ISO_DIV=2 ./lba2cc
```

**Frame scaling above 1080 lines.** Resolutions of 2160 lines and more (4K) render the software frame at half size (a 3840x2160 choice renders at 1920x1080, exteriors and interiors alike) and the present scales it up; the choice itself is what is shown in the Display menu and saved to lba2.cfg. The terrain packs screen coordinates in 16 bits and the 2D UI draws at its authored 640x480 size, so a native 4K software frame would break the one and shrink the other. With the GPU renderer on, 3D models still render at the window's own resolution, up to 3840x2160. 1440p and below render natively; above 1080 lines the exterior focal grows with the height so the view is framed as at 1080p rather than widening.

## Ideas not yet wired up

- DetailLevel and Shadow are stored in lba2.cfg and exposed via the Options menu; see [CONFIG.md](CONFIG.md) for the full key list.
- `RAIN_RANGE` in `SOURCES/3DEXT/LINERAIN.ASM` controls how far the rain effect is drawn in the original ASM implementation.
- In the community build, the rain line routine is compiled from `SOURCES/3DEXT/LINERAIN.CPP` by default; the ASM file is kept for historical reference. If you want a user-facing option for rain draw distance in modern builds, consider wiring it through the C++ implementation rather than only tweaking the ASM constant.
