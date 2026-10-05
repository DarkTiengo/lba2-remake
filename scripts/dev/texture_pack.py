#!/usr/bin/env python3
"""Export and validate optional 4x RGBA world-texture packs.

The export reads retail data supplied by the user. Its output is derivative game
art and must stay local; the engine never needs this script to load a finished
pack.
"""

import argparse
import struct
import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:  # Keep --help usable on a build-only Python installation.
    Image = None

_HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(_HERE))
from hqr_inspect import decompress_entry, entries  # noqa: E402

SIDE = 256
SCALE = 4
OUT_SIDE = SIDE * SCALE

ISLANDS = {
    "citadel": (0, 27, 11),
    "citabau": (0, 42, 26),
    "sendell": (1, 28, 12),
    "desert": (2, 29, 13),
    "emeraude": (3, 30, 14),
    "otringal": (4, 31, 15),
    "celebrat": (5, 32, 16),
    "celebra2": (5, 32, 16),
    "platform": (6, 33, 17),
    "mosquibe": (7, 34, 18),
    "knartas": (8, 35, 19),
    "ilotcx": (9, 36, 20),
    "ascence": (10, 37, 21),
}


def find_casefold(root: Path, name: str) -> Path:
    wanted = name.casefold()
    for path in root.iterdir():
        if path.name.casefold() == wanted:
            return path
    raise FileNotFoundError(f"{name} not found under {root}")


def hqr_entry(path: Path, index: int) -> bytes:
    catalog, raw = entries(str(path))
    if index >= len(catalog) or catalog[index][2] is None:
        raise ValueError(f"{path.name}: HQR entry {index} is absent")
    _, offset, size, compressed, method = catalog[index]
    data = decompress_entry(raw, offset, size, compressed, method)
    if len(data) != size:
        raise ValueError(f"{path.name}: entry {index} decoded to {len(data)}, expected {size}")
    return data


def xpl_palette(ress: Path, index: int) -> bytes:
    xpl = hqr_entry(ress, index)
    if len(xpl) < 8:
        raise ValueError(f"RESS.HQR entry {index}: truncated XPL")
    (offset,) = struct.unpack_from("<I", xpl, 4)
    palette = xpl[offset : offset + 256 * 3]
    if len(palette) != 256 * 3:
        raise ValueError(f"RESS.HQR entry {index}: truncated palette")
    if max(palette) <= 0x3F:
        palette = bytes(min((value << 2) | (value >> 4), 255) for value in palette)
    return palette


def save_page(indices: bytes, palette: bytes, destination: Path) -> None:
    if len(indices) != SIDE * SIDE:
        raise ValueError(f"texture page has {len(indices)} bytes, expected {SIDE * SIDE}")
    image = Image.frombytes("P", (SIDE, SIDE), indices)
    image.putpalette(palette)
    image = image.convert("RGBA").resize((OUT_SIDE, OUT_SIDE), Image.Resampling.NEAREST)
    destination.parent.mkdir(parents=True, exist_ok=True)
    image.save(destination)


def export_pack(game_dir: Path, output: Path) -> int:
    ress = find_casefold(game_dir, "RESS.HQR")
    output.mkdir(parents=True, exist_ok=True)
    (output / "pack.ini").write_text("Format=1\nScale=4\n", encoding="ascii")

    found = 0
    for path in sorted(game_dir.iterdir(), key=lambda item: item.name.casefold()):
        if path.suffix.casefold() != ".ile":
            continue
        island = path.stem.casefold()
        if island not in ISLANDS:
            print(f"skip {path.name}: no stable palette/sky mapping", file=sys.stderr)
            continue
        _, palette_entry, sky_entry = ISLANDS[island]
        palette = xpl_palette(ress, palette_entry)
        target = output / "islands" / island
        save_page(hqr_entry(path, 1), palette, target / "ground.png")
        save_page(hqr_entry(path, 2), palette, target / "objects.png")
        save_page(hqr_entry(ress, sky_entry), palette, target / "skysea.png")
        print(f"exported {island}")
        found += 1

    if found == 0:
        raise ValueError(f"no supported .ILE archives found under {game_dir}")
    print(f"wrote {found} island directories to {output}")
    return 0


def parse_manifest(root: Path) -> list[str]:
    errors = []
    manifest = root / "pack.ini"
    if not manifest.is_file():
        return ["pack.ini is missing"]
    values = {}
    for line in manifest.read_text(encoding="utf-8").splitlines():
        if "=" in line:
            key, value = line.split("=", 1)
            values[key.strip().casefold()] = value.strip()
    if values.get("format") != "1":
        errors.append("pack.ini must contain Format=1")
    if values.get("scale") != "4":
        errors.append("pack.ini must contain Scale=4")
    return errors


def validate_pack(root: Path) -> int:
    errors = parse_manifest(root)
    pages = sorted((root / "islands").glob("*/*.png"))
    if not pages:
        errors.append("no islands/<name>/*.png pages found")
    allowed = {"ground.png", "objects.png", "skysea.png", "terrain.png"}
    for path in pages:
        if path.name not in allowed:
            errors.append(f"{path.relative_to(root)}: unsupported page name")
            continue
        island = path.parent.name
        if not island or any(not (char.isalnum() or char in "_-") for char in island):
            errors.append(f"{path.relative_to(root)}: unsafe island directory name")
        if path.name == "terrain.png" and island != "citadel":
            errors.append(f"{path.relative_to(root)}: terrain materials are currently Citadel-only")
        try:
            with Image.open(path) as image:
                if image.size != (OUT_SIDE, OUT_SIDE):
                    errors.append(f"{path.relative_to(root)}: expected {OUT_SIDE}x{OUT_SIDE}")
                if image.mode != "RGBA":
                    errors.append(f"{path.relative_to(root)}: expected RGBA, got {image.mode}")
                image.verify()
        except (OSError, ValueError) as exc:
            errors.append(f"{path.relative_to(root)}: {exc}")

    if errors:
        for error in errors:
            print(f"error: {error}", file=sys.stderr)
        return 1
    print(f"valid texture pack: {len(pages)} page(s)")
    print("animated terrain rectangles are protected by the runtime's original-page mask")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)
    export_parser = subparsers.add_parser("export", help="export retail pages as editable 4x RGBA PNGs")
    export_parser.add_argument("--game-dir", required=True, type=Path)
    export_parser.add_argument("--out", required=True, type=Path)
    validate_parser = subparsers.add_parser("validate", help="validate a finished pack")
    validate_parser.add_argument("pack", type=Path)
    args = parser.parse_args()

    try:
        if Image is None:
            raise ValueError("Pillow is required (install it with: python3 -m pip install Pillow)")
        if args.command == "export":
            return export_pack(args.game_dir, args.out)
        return validate_pack(args.pack)
    except (FileNotFoundError, OSError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
