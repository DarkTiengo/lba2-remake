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

## What the present is, and is not

The present is half the frame, so it was taken apart the same way, by removing one thing at a
time from a 720p orbit and measuring what the present phase did:

| removed | present |
| --- | --- |
| nothing | 33.0 ms |
| the vertex upload (30 MB a frame) | 29.6 ms |
| every scene draw | 34.3 ms |
| the tag texture upload | 34.1 ms |
| the whole GPU renderer (classic) | ~10 ms |

So the geometry the capture builds is **not** what the present spends its time on: uploading it is
3.4 ms and drawing it is free. What is left is the composite, and it is pixel-bound — 640x480 puts
the present at 16.5 ms against 33.0 at 1280x720, near enough to the pixel count.

And the GPU it runs on is asleep. During that orbit `nvidia-smi` reports the laptop's 4060 at
**900 to 1000 MHz of its 3105, 25 % utilisation, 10 to 14 W**, on AC power with no throttling
event active. The work arrives in bursts too small and too far apart for the driver's clock
governor to answer, so the composite takes three times what it would at full clock, and the frame
waits for it.

That is the second reason the CPU side comes first. Every millisecond taken off the scenery
arrives twice: once as itself, and once as a GPU that is given enough to do to wake up.

## Where the scenery phase goes, a cube at a time

Measured per cube on Desert Island at 720p, orbiting, with the curved land already indexed:

| | per cube | per frame (15 cubes) |
| --- | --- | --- |
| the cube's decor objects (`AffichageObjetDecorsZBuf`) | 0.445 ms | 6.7 ms |
| its cells: the software fill and the capture riding along | 0.96 ms | 14.4 ms |
| its 65 x 65 grid, placed and projected | 0.086 ms | 1.3 ms |

Of the cell walk, the capture is about 5 ms a frame (measured by turning it off) and the software
fill the rest. So **the exterior's software rasterising is around 17 ms of a 52 ms frame** and the
capture that rides along with it about 5.

That reverses the order below. Retaining the scenery saves the capture; the capture is no longer
the biggest thing in the frame. **Step 2 is.**

### Tried and refused: filling the decor flat

The terrain's `gfx_fastterrain` fills a triangle flat where the GPU covers it, and the same trade
looked obvious for the bodies standing on that terrain: the decor of an exterior cube is 0.445 ms
of software rasterising, 6.7 ms a frame. It was built (a predicate beside the capture, a flat fill
at the dispatch point, a setting) and then measured:

| | scenery phase, 720p | 1080p |
| --- | --- | --- |
| the decor's polygons filled as authored | 20.90 ms | 16.14 ms |
| filled flat where the GPU covers them | 20.72 ms | 15.83 ms |
| not filled at all | 20.31 ms | — |
| filled, but not captured | 20.53 ms | — |

**The fill is 0.6 ms of the 6.7 and the capture 0.3.** Flattening bought 0.2, inside the noise of
a single run, for a second fill path through the 1997 rasteriser and a predicate that has to track
what the capture covers. Reverted.

What the decor actually costs is the engine's own per-object work: the box projections that select
it, the point transform, the lights, the sort. The classic renderer pays it too — its scenery
phase is 14.8 ms, which reconciles as 6.7 of decor, about 7 of cells and 1 of grid. There is no
modern-path saving to be had in it **short of not drawing the decor in software at all**, which is
step 2.

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

### 2. Stop drawing the terrain in software (exteriors, GPU renderer) — **done, for the ground and the sea**

Landed as the unpainted ground ([GPU_RENDERER.md](../GPU_RENDERER.md)): the scenery group claims the
scene window, covered terrain triangles and captured sea tiles are not filled, and their depth is
filled in only for the boxes `DrawRecover` asks about, with verdicts identical to the flat fill's.
Measured per cube on Desert Island at 720p (thread CPU, `LBA2_PERFTRACE_CPU=1`), before it:

| | per cube | per frame (~11 cubes) |
| --- | --- | --- |
| decor objects | 0.40 ms | 4.4 ms |
| cell walk and capture | 0.35 ms | 3.8 ms |
| the software fill of the land | 0.57 ms | 6.3 ms |
| `TerrainGpu_End` (smooth terrain, grass) | 0.57 ms | 6.2 ms |
| the sea: capture 0.31, software fill 0.17 | 0.49 ms | 5.4 ms |

The two fills are gone: the scenery phase fell from 26.8 to 19.6 ms. What was left was CPU work
done *for* the GPU, and two thirds of it has moved: the sea's swell is the vertex shader's (19.6 to
16.1 ms, and the waves move with the camera still), and the smooth terrain grows on the GPU from
the cube's height map (`GPUTERRAIN.vert`, 16.1 to 13.3 ms). The grass's blades followed (13.3 to 12.0 ms):
the CPU only decides each tuft. The scenery phase is now the engine's own work — the cell walk,
the grid, the decor — and the flat land's records.

The original analysis, kept:

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

### 3. Fewer vertices for the same curve — **done**

A land triangle near the camera becomes sixteen, emitted as 48 separate vertices where the sub-grid
has only 15 distinct points: every interior point is built, written and uploaded three times over.
An index buffer would cut the capture's arithmetic and its writes by the same three, and the
upload with them.

Landed. On Desert Island a full orbiting view fell from 242 000 vertices to 141 000, the scenery
phase from 26.5 ms to 21.3 and the present from 33.0 to 30.5 at 720p; the frame from 62 to 52.
Image-equivalent with and without ray tracing (1777 and 1790 pixels of a 2M-pixel capture, against
1829 between two runs of the same build).

**Worth:** most of the smooth terrain's share of the capture, which is two thirds of it. **Risk:**
contained to the GPU plumbing — `Compact` has to move a draw's indices with its vertices (store
them relative to the draw's base and it is a move, not a remap), the presenter has to bind them,
and the shadow hierarchy has to walk triangles through them.

### 4. A compact vertex, if the budget asks for it

96 bytes a vertex is position, normal, light, view position, material and uv, all in float4s; the
scenery uses a third of it. Packing it to about 40 would cut the memory the world costs, which is
what decides how much of it can be alive at once.

**Worth for speed: little** — the measurements above put the upload at 3.4 ms, so this is a memory
change, not a time one. It is here because the vertex budget is what the retained ring in step 1
will run into. **Risk:** mechanical but wide — every shader that reads a vertex.

## Order and why

**Step 2 is done** for the ground and the sea (above); what remains of the scenery phase is the
capture's own CPU work and the decor. **The order changed once the numbers did.** 2 first then: the software's own rasterising of the
exterior is 17 ms of the frame and the capture is 5. 1 second, and smaller than it looked when it
was written. 3 is done. 4 is not a speed change at all.

The reasoning that put 1 first, kept because it is still true of 1: it is self-contained,
reversible behind a setting, and pays the most per unit of risk. 2 second because until 1 lands the software fill is not the top cost, and because it wants
the composite's attention on its own. 3 is the one to take first if 1 turns out to want the whole
cube ring captured and the vertex budget says no: it buys a third of the same cost with a fraction
of the risk, and it helps 1 when 1 comes. 4 is not a speed change at all.

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
