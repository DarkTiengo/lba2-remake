# Moving the camera should not rebuild the world

The free camera made one thing plain: a frame in which the camera moves costs five times a frame
in which it does not. With the GPU renderer at 1280x720 on the development laptop, the scene is
**12.6 ms** standing still and **63 ms** orbiting. This is the plan for the second half of that,
the half the camera work deliberately left alone.

Everything below was measured on Desert Island, orbiting continuously (`camnudge 5 0 560`), ray
tracing off, smooth terrain on, view distance far, vsync off. Times are **thread CPU time**, which
is what this machine can be asked twice and answer the same; wall clock here moves with whatever
else is running.

## Where an orbiting frame goes

| | scenery | present | frame |
| --- | --- | --- | --- |
| classic renderer, 720p | 14.8 ms | 10.5 ms | 26.0 ms |
| **GPU renderer, 720p** | **31.7 ms** | **35.1 ms** | **68.0 ms** |
| GPU, smooth terrain off (122k verts against 318k) | 22.5 ms | 28.6 ms | 52.4 ms |
| GPU, every composite effect off | 27.2 ms | 33.9 ms | 62.3 ms |
| GPU, 640x480 | 15.4 ms | 16.5 ms | 32.6 ms |
| GPU, camera still | — | — | 11.2 ms |

`scenery` is `RefreshGrille`: the software terrain fill and, with the GPU renderer, the capture
that rides along with it. `present` is `BoxBlit`, which ends in the GPU submit; of it, the
presenter's own bookkeeping is 3.8 ms (tags 1.0, resolve and upload 2.7, submit 0.1) and the
palette conversion 0.8, so the rest is the driver working or spinning on the frame's own render.
Everything else in the frame — the clear, the copy to Screen, every body, the shadows, the
incrusts — comes to **2.3 ms together**. There is nothing else to find.

Two numbers fall out of that table:

- **The capture costs about as much as the software fill it rides along with.** The GPU
  renderer's scenery is 31.7 ms against the classic's 14.8, and the classic is doing the same
  terrain walk and the same fills. The difference is building 318 000 vertices of 96 bytes: 30 MB
  written per frame, at about 53 ns a vertex.
- **Geometry costs on both sides.** Turning off the smooth terrain removes 196 000 vertices and
  takes 9 ms off the scenery and 6.5 ms off the present: **roughly 8 ms per 100 000 vertices a
  frame**, half in the capture, half in the upload and the render.

And the effects, which is where one would look first, are worth 6 ms of the 68.

## What to do about it, in order

### 1. Retain the scenery between frames

The scenery is captured again every frame **only because its vertices are in camera space**
(`AffGpu_ViewVertex` writes clip and `vpos` from the camera that captured them). Nothing else
about it changes while the hero stands in the same cube: the same hills, the same sea, the same
grass.

So: record the camera each scenery group was captured with, and give the vertex shader a per-draw
transform, `viewNow · viewCaptured⁻¹`, to apply to `vpos` and the normal before it computes clip
with the projection's own formula. The capture itself does not change. Then the frame's group can
be **rebound** rather than rebuilt: `GpuObj_BeginGroup` takes a fresh id every frame and the old
draws die with their tags, so what is needed beside it is a `GpuObj_RebindGroup(id)` that keeps
the draws and lets the software fill re-tag the same pixels with the same id.

The predicate for "only the camera moved" exists already — `FollowCamNeedsUpdate` separates it
from "the hero moved" — and the cube ring the capture covers has to be made camera-independent
with it (capture the whole ring, centre the smooth-terrain and grass radius on the hero rather
than on the eye), or a turn would walk into geometry that was never captured.

**Worth:** the capture, about 17 ms at 720p, and most of the 2.7 ms upload. Nothing of the render.

**Risks:** every effect reads `vpos` and the normal (fog, lights, occlusion, the ray-traced
scene's own space, the water's world phase); the clip recomputed in float in the shader is not
bit-for-bit the clip the CPU computed in long double, so silhouettes against the software frame
move by a fraction of a pixel; and the retained draw has to survive `Compact()`.

### 2. Stop drawing the terrain in software (exteriors, GPU renderer)

With the geometry retained, the software fill is still there — 14.8 ms of it — because its pixels
are what carry the tags, and the tags are what the composite matches. The way out is the one the
GPU renderer has been heading toward since the scenery got real depth: **in an exterior, let the
composite decide by depth instead of by tag**, and the terrain need not be drawn in software at
all.

What still needs the software frame, and what it needs from it, has to be settled first: sprites
(their colour and the pixel-art filter, though their depth is the GPU's since the sprite work),
`DrawRecover`'s verdict on the hero being covered, the dirty-box bookkeeping, text and the HUD
over everything, and the near-plane hole the terrain's own fill hides today.

**Worth:** most of the remaining 14.8 ms. **Risk:** the highest of the three — it changes what the
composite is.

### 3. A compact vertex for the scenery

96 bytes a vertex is position, normal, light, view position, material and uv, all in float4s. The
scenery uses a third of it. Packing it to about 40 cuts the 30 MB the capture writes and the
upload to match.

**Worth:** 3 to 5 ms at 720p, and the same fraction of the vertex budget, which is what decides
how much world can be alive at once. **Risk:** mechanical but wide — every shader that reads a
vertex.

## Order and why

1 first because it is self-contained, reversible behind a setting, and pays the most per unit of
risk. 2 second because until 1 lands the software fill is not the top cost, and because it wants
the composite's attention on its own. 3 last because it is a refactor whose value is real but
whose risk is spread over every shader, and because after 1 and 2 the remaining geometry cost is
in the render rather than in the upload.

Nothing here trades detail for speed: the effects are 6 ms of the 68 and the player already has
switches for them. Quality-for-speed knobs (a coarser smooth terrain, a shorter grass radius) stay
where they are, in the auto-adjust.

## How each step is verified

- **Frames.** `LBA2_GPU_CAPTURE` before and after, in four camera orientations, on Desert Island
  (hills, sea, town) and Zeelich (low ceiling, gas). A retained frame and a rebuilt one must
  differ by no more than the noise two runs of the same build produce.
- **Time.** `perftrace dump` reports `scene`, `scenery` and `present` per frame; the scenery phase
  was added for this work. Each step states its number the way the table above does, measured the
  same way, and a step that does not move its number is a step that did not work.
- **The classic renderer.** Untouched and proven so: same frames as upstream on the corpus cubes.
- **The fixtures.** `make test`, `make arch-check`, and the camera and image fixtures, every step.
