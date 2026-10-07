# Citadel vehicle B prototypes

Editable Blender 5.2 sources (`.blend`), portable glTF models (`.glb`), and
rendered previews (`-preview.png`) for the three selected B concepts:

The latest boat and scooter use their `-concept` versions; the buggy retains
its `-refined` version. Earlier files remain for before/after comparison.

| Vehicle | Latest preview | Latest GLB | Editable source | GLB triangles (original → refined) |
| --- | --- | --- | --- | ---: |
| Yellow buggy | `buggy-b-refined-preview.png` | `buggy-b-refined.glb` | `buggy-b-refined.blend` | 22,918 → 71,710 |
| Harbour boat | `boat-b-concept-preview.png` | `boat-b-concept.glb` | `boat-b-concept.blend` | 6,400 → 38,370 |
| Blue scooter | `scooter-b-concept-preview.png` | `scooter-b-concept.glb` | `scooter-b-concept.blend` | 12,460 → 75,940 |

The refined versions add shaped body panels, denser tires/wheels, suspension
detail, glazing, and each vehicle's characteristic accessories. They are
**still prototypes**, not exact production-quality recreations of the approved
images in `../../concepts/`. In particular, material realism and some
proportions still need art review.

The concept boat is a separate rebuild with twin pontoons and a bow tunnel,
a low cabin, wraparound windshield, hipped roof, recessed timber entry,
helm and passenger seating, stainless rails, rescue ring, and articulated
crane with an open suspended bucket. Its GLB contains 38,370 triangles and
uses `KHR_materials_transmission` for the glazing; viewers need support for
that extension to reproduce the glass. The PNGs are Cycles studio
renders; `boat-b-concept-rear-preview.png` shows the stern. Neither
is a screenshot from the game. The studio floor is not part of the GLB.
This is remote boat project revision 11, saved on 2026-10-05.

The concept boat is baked into the game at build time by
`scripts/dev/export_citadel_boat.py`. With the GPU renderer and
`gfx_texturedetail 2`, it replaces the Citadel harbour boat's GPU geometry
(cube 43, actor 10, body 165; alternative decor 56). Rebuild after changing
the GLB. Classic rendering and texture-detail modes 0–1 keep the retail model.
The buggy files remain previews only.

The scooter concept is also baked at build time (`--vehicle scooter`). With
the same GPU/pack settings it replaces the vehicle portions of Citadel actor
bodies 148–150 (entity 100), including the post-storm island. Driver and
passenger geometry remain original: only groups 2 (chassis) and 20 (steering)
are replaced. Their interpolated matrices preserve steering, lean and routes.
There is no added player-driving mechanic or change to collision or scripts.
The mesh has 75,940 triangles and plain glTF-compatible paint, rubber, leather
and metal materials. Revision 7 of the scooter project is the editable source.
The finish script is
[citadel_scooter_finish_b.py](../../../../../scripts/dev/citadel_scooter_finish_b.py),
run after the common helpers on the refined scene. Its preview is a studio
render, not a game screenshot.

For a manual in-game check, visit the scooter driver in Citadel scene 42
(actor 3, near the covered entrance), enable `gfx_texturedetail 2`, and compare
with mode 0. Confirm the rider remains visible, the bike sits on the ground,
and the original transport interaction still works. The scooter remains
static in the exported GLB; runtime steering is bound in
`LIB386/OBJECT/CITADEL_SCOOTER.CPP`.

The runtime removes the entry's opaque backing, places the door and handle
at the doorway, and swings them inward according to the interpolated retail
door group. The boat's hull follows the existing buoyancy/route transforms.
Opening, open hold, closing and closed hold use the actor's animation state;
there is no independent timer and no change to collision, scripts or saves.
The studio GLB/BLEND files remain the static authoring source; animation is
bound in `LIB386/OBJECT/CITADEL_BOAT.CPP`, not a baked GLB animation clip.

The models and previews were generated specifically for this project. They do
not contain original game model data. Open the GLB in a glTF viewer or the BLEND
file in Blender 5.2 or newer to inspect and edit them.

The Blender geometry-refinement scripts are
`../../../../../scripts/dev/citadel_vehicle_refine_common.py` and the three
`citadel_vehicle_refine_{buggy,boat,scooter}.py` files beside it. Execute the
common script followed by one vehicle script in the corresponding draft scene.

For the concept boat, use the self-contained
[scene builder](../../../../../scripts/dev/build_citadel_boat_concept_b.py)
instead. It replaces the active Blender scene, so run it in a separate draft
file, not in a scene containing other work.

Interactive previews: [buggy](https://higgsfield.ai/3d-jutsu/d393494e-10c0-4dac-bf01-002774a9dd77),
[boat](https://higgsfield.ai/3d-jutsu/8dab4e7b-adfd-4f96-a364-aedac26ff924),
[scooter](https://higgsfield.ai/3d-jutsu/3282968e-6ea5-406e-8199-76052ac163cb).
