# Modern lava texture

`lava-flow-source.png` is the original generated top-down lava artwork for this project. `lava-flow.png` is its 512 × 512 game-ready export; the build embeds this PNG for the optional GPU lava material. Neither image is extracted from the retail game data.

The art brief was: dark charcoal and oxblood cooling crust in irregular, flattened flow bands; connected but restrained ember-red and orange fissures; even overhead lighting; no perspective, objects, smoke, flames, text or watermark. The shader preserves the game's indexed texture as a base, and `gfx_lava 0` restores that base without the new image.

The first set of original Citadel metal surfaces is documented in
[`citadel/README.md`](citadel/README.md). Its six-layer export is embedded
in the optional GPU repaint of the original parked buggy, Citadel trees and
selected street objects.

## Style and coverage

The replacement pack uses believable, slightly weathered materials while
keeping the game's readable silhouettes and characteristic colors. Author
each material as an evenly lit, repeatable albedo tile; bind it only to the
matching surface, and preserve the original transparency, animation, fog and
lighting rules. At normal camera distance, grain and wear should support the
shape rather than turn a surface into visual noise.

The Citadel buggy and harbour boat use palette-preserving paint and metal tiles.
That does not imply that every vehicle, island, character, interior
or UI texture is replaced yet. Those need their own surface identification
and in-game checks before their replacement pages are added.
