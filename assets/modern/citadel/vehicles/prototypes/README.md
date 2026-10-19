# Citadel vehicle B prototypes

Editable Blender 5.2 sources (`.blend`), portable glTF models (`.glb`), and
rendered previews (`-preview.png`) for the three selected B concepts:

The latest versions use the `-concept` filenames. Earlier files remain for
before/after comparison.

| Vehicle | Latest preview | Latest GLB | Editable source | GLB triangles (original → refined) |
| --- | --- | --- | --- | ---: |
| Yellow buggy | `buggy-b-concept-preview.png` | `buggy-b-concept.glb` | `buggy-b-concept.blend` | 22,918 → 119,760 |
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
The buggy concept is also baked at build time (`--vehicle buggy`).

The concept buggy is revision 10 of the editable buggy project. It has a
compound-curved yellow bonnet and side fairings, red cockpit shoulders, a
single transverse safety hoop, a bucket seat, curved alpha-blended glazing,
headlamps fitted to the bonnet surface, closed tire sidewalls, staggered tread
blocks, dished alloy rims and exposed suspension. Materials use constant
glTF-compatible paint, rubber, leather and metal values; there are no external
textures. Its 119,760 triangles include evaluated modifiers and curves.
The preview is a studio render, not an in-game screenshot; its floor is not
in the exported model. The authoring script
[citadel_buggy_finish_b.py](../../../../../scripts/dev/citadel_buggy_finish_b.py)
runs after the common helpers and replaces mesh/curve objects in the dedicated
buggy draft. Do not run it in a scene containing unrelated artwork.

With the GPU renderer and `gfx_texturedetail 2`, the buggy replaces selected
Citadel vehicle geometry through
[CITADEL_BUGGY.CPP](../../../../../LIB386/OBJECT/CITADEL_BUGGY.CPP).
Bodies 14/15 and 22/23 contain the rider, body 219 is parked, and Citadel decor
85 is static. Chassis group 2 supplies pitch/roll, front-wheel groups 5/8
inherit steering from groups 4/7, and groups 5/8/10/12 supply wheel spin.
The authored wheel centres are offset in chassis space to avoid orbiting
when the tires rotate. Suspension pieces follow the chassis. The original
steering wheel and hands (group 13) remain together in driven bodies;
the authored steering wheel is used only on unoccupied models. Rider groups
14/15 and armed-variant groups 16–19 are retained. Physics, collision,
animation scripts, save data and classic rendering are unchanged.

Known visual limitation: close moving views can show noisy traced shadows
on the body and tires. Disabling `gfx_shadows` isolates this from the clean
paint material; the integration does not change the player's shadow setting.
The full quest boarding/exit sequence still needs a manual playthrough.

To reproduce a driving check without changing a real save, launch the game
with `--headless --no-autosave --user-dir <temporary-directory>` and load
`tests/savegame/corpus/saves/steam_classic_2023/Race.LBA`. Use
`--exec 'cube 42; gfx_autotune off; gfx_texturedetail 2; skipmodals on'`,
`--exec-at 3 'teleport 7000 250 9000 1024'`,
`--exec-at 8 'input up+left 50'` and
`--tick 70 --fixed-dt 16 --dump-state <temporary-file> --exit`.
This is a renderer fixture, not normal quest progression. For a manual
playthrough, enable the same pack setting on Citadel and use the buggy's
normal boarding/exit interaction. Compare with `gfx_texturedetail 0`.

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
