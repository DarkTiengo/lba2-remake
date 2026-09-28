# Modern lava texture

`lava-flow-source.png` is the original generated top-down lava artwork for this project. `lava-flow.png` is its 512 × 512 game-ready export; the build embeds this PNG for the optional GPU lava material. Neither image is extracted from the retail game data.

The art brief was: dark charcoal and oxblood cooling crust in irregular, flattened flow bands; connected but restrained ember-red and orange fissures; even overhead lighting; no perspective, objects, smoke, flames, text or watermark. The shader preserves the game's indexed texture as a base, and `gfx_lava 0` restores that base without the new image.
