#!/usr/bin/env python3
"""Export project-authored Citadel object artwork as repeatable 512px tiles.

The inputs are project-authored images, not pages exported from game data.
Moving each input's natural centre to the tile border and feathering the new
centre seam makes repetition continuous without painting over any game art.
"""

from __future__ import annotations

from pathlib import Path

from PIL import Image, ImageChops


ROOT = Path(__file__).resolve().parents[2] / "assets" / "modern" / "citadel"
SIDE = 512
FEATHER = 96
MATERIALS = (
    "vehicle-painted-steel",
    "vehicle-yellow-painted-steel",
    "vehicle-red-painted-steel",
    "vehicle-neutral-painted-steel",
    "vehicle-panel-painted-steel",
    "utility-galvanized-steel",
    "tree-bark",
    "tree-foliage",
)
OBJECT_LAYERS = (
    "vehicle-yellow-painted-steel",
    "vehicle-red-painted-steel",
    "utility-galvanized-steel",
    "tree-bark",
    "tree-foliage",
    "vehicle-neutral-painted-steel",
    "vehicle-panel-painted-steel",
)


def weight(pos: int) -> float:
    """Keep the input centre and fade it out before the repeated border."""
    distance = abs(pos + 0.5 - SIDE / 2)
    start = SIDE / 2 - FEATHER
    return max(0.0, min(1.0, (SIDE / 2 - distance) / FEATHER)) if distance > start else 1.0


def export(name: str) -> None:
    source = Image.open(ROOT / f"{name}-source.png").convert("RGB")
    original = source.resize((SIDE, SIDE), Image.Resampling.LANCZOS)
    shifted = ImageChops.offset(original, SIDE // 2, SIDE // 2)
    mask = Image.new("L", (SIDE, SIDE))
    mask.putdata([round(255 * weight(x) * weight(y)) for y in range(SIDE) for x in range(SIDE)])
    tile = Image.composite(original, shifted, mask)
    tile.save(ROOT / f"{name}.png", optimize=True)


if __name__ == "__main__":
    for material in MATERIALS:
        export(material)
    atlas = Image.new("RGB", (SIDE, SIDE * len(OBJECT_LAYERS)))
    for layer, material in enumerate(OBJECT_LAYERS):
        with Image.open(ROOT / f"{material}.png") as tile:
            atlas.paste(tile, (0, layer * SIDE))
    atlas.save(ROOT / "vehicle-materials.png", optimize=True)
