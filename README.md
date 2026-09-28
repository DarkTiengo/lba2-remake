# LBA2 Remake

Little Big Adventure 2 (aka Twinsen's Odyssey) is the sequel to Little Big Adventure (aka Relentless: Twinsen's Adventure) in 1997.

LBA2 Remake gives the 1997 game a modern look without changing how it plays or what it is. It runs the original engine and the original game data, and draws them again on the GPU. Islands, characters and objects keep their models, palettes and art. They get smooth shading, soft and ray-traced shadows, sunlight and sky, living water, grass and trees in the wind, and a far view. The game underneath is the same: the scripts, the timing and the saves are untouched, and the classic renderer is one menu option away.

Game assets aren't included; you need a legitimate copy of LBA2 to play.

## Where it comes from

This project is a fork of [LBA2 Classic Community](https://github.com/LBALab/lba2-classic-community), the community source port of Adeline's [`lba2-classic`](https://github.com/2point21/lba2-classic) release, and it keeps everything that port does (below). The community port preserves the original and makes it build and run on modern systems. This fork goes further on the look: the GPU renderer and the effects built on it are developed here, and aren't part of the community port.

## The GPU renderer

The GPU renderer draws the 3D scene on the GPU while the original software rasterizer keeps deciding what is visible, so every rule of the 1997 engine still holds. Switch it in Options → Display → Renderer. Each effect has its own switch in Display → GPU effects, and Display → Auto-adjust fits them to your machine.

- **Models and scenery** at the window's resolution, up to 4K: smooth per-pixel shading from the models' own palettes, filtered textures, continuous distance fog, a far view of the surrounding island
- **Light:** sun shadows, sky fill and rim light, soft shadows away from lamps and fires, ray-traced shadows through the terrain and every body, ambient occlusion
- **Water:** waves and swell on every sea, foam and breakers along the real coastline, splashes where anything enters it, rain rings on the surface
- **Sky and weather:** a procedural sky with the sun, drifting clouds and stars, the Emerald Moon's view of Twinsun, layered rain with lightning over Citadel Island's storm
- **Living scenery:** smoothly curved hills, grass that sways in the wind, trees and bushes that bend in the same gusts with their leaves shaded as foliage
- **Fire and lamps:** procedural flames and firelight, glowing lamp globes that light their surroundings
- **2D:** rooms, sprites and menus keep their pixel art, upscaled with a pixel-art filter

How it works and every switch: [docs/GPU_RENDERER.md](docs/GPU_RENDERER.md). The options at a glance: [docs/GFX_OPTIONS.md](docs/GFX_OPTIONS.md). The GPU renderer needs Vulkan (Linux, Windows, Android); elsewhere the game keeps the classic look.

## Features of the port

- **The complete game** — the original 1997 engine, ported to 64-bit and modern compilers; bring your own copy of LBA2
- **Native builds** — Linux, macOS, Windows, and Android / Android TV (7.0+, API 24)
- **Widescreen and HD** — render resolutions up to 1080p, offered per monitor from the Display menu; the classic 640×480 4:3 mode is still there ([docs/RUNTIME_RESOLUTION.md](docs/RUNTIME_RESOLUTION.md))
- **Reads disc images directly** — retail assets straight out of a raw ISO/BIN or `.cue` pair, so a GOG Original Edition install or a CD rip needs no extraction step
- **Retail-compatible saves** — 1997 saves load directly, and every build of this port writes the same 32-bit save bytes
- **Frame-rate-independent movement** — a fixed 60 Hz simulation step, so walk and jump distances no longer change with your frame rate
- **Gamepad support** — full controller mapping, radial deadzones, and a rebindable D-pad ([docs/CONTROLLER.md](docs/CONTROLLER.md))
- **Third-person camera** — optional follow camera in exterior scenes, orbited with the mouse or the right stick, and zoomed with the mouse wheel or the numpad ([docs/CAMERA.md](docs/CAMERA.md))
- **Render smoothing** — optional texture filtering and dithered shading for the software rasterizer, off by default ([docs/GFX_OPTIONS.md](docs/GFX_OPTIONS.md))
- **Modern audio backend** — SDL3 in place of the original Miles Sound System
- **FMV playback** — via the bundled open-source libsmacker
- **Debug console** — always-on Quake-style console ([docs/CONSOLE.md](docs/CONSOLE.md))

For a history of the community port's changes, please see the [CHANGELOG.md](CHANGELOG.md).

## About the codebase

The original LBA2 engine source is the [`lba2-classic`](https://github.com/2point21/lba2-classic) codebase: it is mostly assembly, with C++ for game logic, and is the canonical historical release. The community port evolves and modernizes it: ports of assembly to C++, SDL3 for graphics/audio/input, libsmacker for FMV, native Linux/macOS/Windows/Android builds, and player-facing additions such as widescreen support, gamepad play, and an always-on debug console. See [ASM_TO_CPP_REFERENCE.md](docs/ASM_TO_CPP_REFERENCE.md) for which modules have been ported from ASM to C++. This fork builds the GPU renderer on top of that work.

## Playing

This fork has no pre-built binaries yet: build it from source (below), then run it with your game data.

```bash
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
LBA2_GAME_DIR=/path/to/game ./build/SOURCES/lba2cc
```

Turn the GPU renderer on in Options → Display → Renderer, or start with `LBA2_GPU=1` to force it for one run.

For the community port's own builds (classic renderer only), see [its releases](https://github.com/LBALab/lba2-classic-community/releases).

### Game data

You need a legitimate copy of LBA2 ([GOG](https://www.gog.com/game/little_big_adventure_2), [Steam](https://store.steampowered.com/app/398000/Little_Big_Adventure_2/), or your retail CD) — the engine doesn't ship assets.

On first launch a folder picker opens; point it at the directory containing `LBA2.HQR` (alongside `music/`, `video/`, `vox/`, …, or under a `Common/` / `CommonClassic/` subfolder for Steam/GOG re-releases). Your choice is remembered.

To skip the picker, pass an explicit path:

```bash
./lba2cc --game-dir /path/to/game
LBA2_GAME_DIR=/path/to/game ./lba2cc
```

To re-pick later: `./lba2cc --pick-game-dir`. See [docs/GAME_DATA.md](docs/GAME_DATA.md) for the full discovery order and override precedence.

Android builds are covered in [docs/ANDROID.md](docs/ANDROID.md); Windows in [docs/WINDOWS.md](docs/WINDOWS.md).

## Building from source quick start

### Prerequisites

- CMake 3.23+
- Ninja (for `make build` and presets)
- A C/C++ compiler with C++98 support (GCC, Clang)
- SDL3 (shared library)
- GNU Make — only required for the `make` shortcuts; plain CMake works without it
- Optional: UASM — only required for `ENABLE_ASM=ON` workflows

On macOS, install with `brew install ninja sdl3`.

Run `./scripts/dev/check-tooling.sh` to see what your machine is missing. That covers the build; the tools needed for testing, linting, packaging, Android, and releasing are indexed by tier in [docs/TOOLING.md](docs/TOOLING.md).

### First clone

1. `make` or `make help` — lists convenience targets (`build`, `run`, `clean`, `test`, …).
2. `make build` — configures `build/` (Ninja, Debug) and compiles `lba2cc`. Or plain CMake: `cmake -B build && cmake --build build`.
3. Point the engine at your game data — `export LBA2_GAME_DIR=/path`, `./data/` (gitignored), `--game-dir`, or bounded automatic discovery (the `LBA2.HQR` marker is the only special-cased file). See [docs/GAME_DATA.md](docs/GAME_DATA.md).
4. `make run` or `./scripts/dev/build-and-run.sh` — build if needed, then run. `make run` sets `LBA2_GAME_DIR` automatically if `./data` or `../LBA2` contains `LBA2.HQR`; otherwise pass `--game-dir /path/to/classic/install` to the binary.
5. `make test` — host-only tests (path resolution, parsers, ABI bounds, version checks); no retail files or Docker required.

**Windows:** Use MSYS2 (recommended; see [docs/WINDOWS.md](docs/WINDOWS.md)). Discovery and the game work the same (`LBA2_GAME_DIR`, `--game-dir`, paths with `\` or `/`). The root `Makefile` and `scripts/dev/*.sh` need a Unix-like shell (MSYS2 UCRT64, Git Bash, or WSL); alternatively run `cmake` and `build/SOURCES/lba2cc.exe` from cmd.exe / PowerShell and set the env var with `set LBA2_GAME_DIR=...`.

## CMake presets

For platform-specific builds, use the presets in `CMakePresets.json` (all use the `Ninja` generator, so `ninja` must be on `PATH`):

- **Linux:** `cmake --preset linux && cmake --build --preset linux`
- **macOS:** `cmake --preset macos_arm64 && cmake --build --preset macos_arm64` (or `macos_x86_64`)
- **Windows:** `cmake --preset windows_ucrt64 && cmake --build --preset windows_ucrt64` — see [docs/WINDOWS.md](docs/WINDOWS.md)
- **Cross-compile Windows from Linux:** `cmake --preset cross_linux2win && cmake --build --preset cross_linux2win`. To skip the preset, use the toolchain file directly: `cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/mingw-w64-i686.cmake`.

## Build options

| Option | Values | Default | Description |
|--------|--------|---------|-------------|
| `SOUND_BACKEND` | `null`, `miles`, `sdl` | `sdl` | Sound backend. Use `sdl` for audio via SDL3. `miles` requires the proprietary Miles Sound System SDK. See [docs/AUDIO.md](docs/AUDIO.md). |
| `MVIDEO_BACKEND` | `null`, `smacker` | `smacker` | Motion video backend. Use `smacker` for FMV playback via the bundled open-source libsmacker. |
| `DEBUG_TOOLS` | `ON`, `OFF` | `OFF` | Enable original Adeline developer debug tools: overlay, FPS counter, screenshots, collision visualization, benchmarks, cheat codes, bug save/load, command-line scene selection. See [docs/DEBUG.md](docs/DEBUG.md). |
| `LBA2_BUILD_TESTS` | `ON`, `OFF` | `OFF` | Build CTest targets (ASM equivalence + host tests such as `test_res_discovery`). |
| `LBA2_BUILD_ASM_EQUIV_TESTS` | `ON`, `OFF` | `ON` | ASM↔CPP equivalence suite (needs `objcopy`). Set `OFF` for host-only tests (e.g. macOS CI, `make test`). |

Minimal build (no audio/video): `-DSOUND_BACKEND=null -DMVIDEO_BACKEND=null`. When `MVIDEO_BACKEND=smacker`, video audio routes through the active sound backend (SDL: real audio; NULL/MILES: silent). See `LIB386/SMACKER/README.md` and `LIB386/AIL/MILES/README.md` for details on the proprietary SDKs and their open-source replacements.

## Debug console

This source port includes a Quake-style drop-down debug console. It is always available (no build flag), designed to be minimally invasive — normal gameplay is unchanged unless you open and use it. See [docs/CONSOLE.md](docs/CONSOLE.md) for commands, usage, and integration details.

## Project structure

```text
lba2-remake/
├── CMakeLists.txt            # Root build configuration
├── CMakePresets.json         # Cross-platform preset builds (linux/macos/windows/...)
├── Makefile                  # Convenience targets (build/run/test/format)
├── cmake/                    # Toolchains and CMake helpers
├── scripts/                  # Dev and CI helper scripts
├── SOURCES/                  # Main game logic and app entrypoints
│   ├── CONSOLE/              # Always-on debug console module (core + state)
│   ├── 3DEXT/                # 3D extensions (terrain, sky, rain, decor)
│   ├── CONFIG/               # Input/config UI and bindings
│   └── *.CPP, *.H, *.ASM     # Gameplay systems (AI, physics, save/load, etc.)
├── LIB386/                   # Engine libraries
│   ├── 3D/                   # Projection, rotation, matrices
│   ├── AIL/                  # Audio abstraction (SDL/Miles/null)
│   ├── ANIM/                 # Animation system
│   ├── OBJECT/               # 3D object rendering
│   ├── pol_work/             # Polygon fillers/rasterization
│   ├── SVGA/                 # Text/sprite/dirty-box rendering paths
│   ├── SYSTEM/               # Platform/system/input/timer abstractions
│   ├── H/                    # Shared legacy headers/types
│   └── libsmacker/           # Open-source Smacker decoder (LGPL 2.1)
├── tests/                    # Host tests + ASM↔CPP equivalence test wiring
├── docs/                     # Project documentation index and subsystem docs
└── run_tests_docker.sh       # Docker wrapper for ASM↔CPP test workflows
```

## Documentation

**Engine reference** (terms, lifecycles, scene index): [GLOSSARY](docs/GLOSSARY.md), [LIFECYCLES](docs/LIFECYCLES.md), [SCENES](docs/SCENES.md).

Build, debug, preservation, and porting docs are in [docs/](docs/README.md).

## Preservation notes

This codebase is a window into 1990s game development at Adeline Software International in Lyon, France. Beyond the technical content, the source files contain original developer artifacts worth exploring. The ASCII art and French comments documented below are from the original Adeline / lba2-classic codebase (same files or content preserved when porting ASM to C++ in this fork).

- **ASCII art banners** -- The developers decorated their source files with elaborate text banners in two distinct styles. See [ASCII_ART.md](docs/ASCII_ART.md) for a full catalog.
- **French comments** -- The code is written with French comments throughout, many of which are informal, humorous, or expressive in ways that reflect the personality of the team. See [FRENCH_COMMENTS.md](docs/FRENCH_COMMENTS.md) for a curated selection with English translations.

## License

This source code is licensed under the [GNU General Public License](LICENSE), as the community port and the original release are.

Please note this license only applies to **Little Big Adventure 2** engine source code. **Little Big Adventure 2** game assets (art, models, textures, audio, etc.) are not open-source and therefore aren't redistributable.

## How can I contribute?

Read the [Contribution Guidelines](CONTRIBUTING.md) and the [Code Style](CODESTYLE.md) reference.

## Links

* **Official Website:** https://twinsenslittlebigadventure.com/
* **Discord:** https://discord.gg/jsTPWYXHsh
* **Docs:** https://lba-classic-doc.readthedocs.io/

## Buy the game

* [GOG](https://www.gog.com/game/little_big_adventure_2)  
* [Steam](https://store.steampowered.com/app/398000/Little_Big_Adventure_2/)

## Original development team

* **Direction:** Frédérick Raynal
* **Programmers:** Sébastien Viannay / Laurent Salmeron / Cédric Bermond / Frantz Cournil / Marc Bureau du Colombier
* **3D Artists & Animations:** Paul-Henri Michaud / Arnaud Lhomme
* **Artists:** Yaeël Barroz, Sabine Morlat, Didier Quentin
* **Story & Design:** Frédérick Raynal / Didier Chanfray / Yaël Barroz / Laurent Salmeron / Marc Albinet
* **Dialogs:** Marc Albinet
* **Story coding:** Frantz Cournil / Lionel Chaze / Pascal Dubois
* **Video Sequences:** Frédéric Taquet / Benoît Boucher / Ludovic Rubin / Merlin Pardot
* **Music & Sound FX:** Philippe Vachey
* **Testing:** Bruno Marion / Thomas Ferraz / Alexis Madinier / Christopher Horwood / Bertrand Fillardet
* **Quality Control:** Emmanuel Oualid

Use the `credits` command in the [console](docs/CONSOLE.md) to see the full original credits.

## Copyright

The intellectual property is currently owned by [2.21]. Copyright [2.21]
Originally developed by Adeline Software International in 1994
