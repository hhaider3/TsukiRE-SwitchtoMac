# TsukiRE-SwitchtoMac

> **This is a fan project. It is unofficial and has no affiliation with TYPE-MOON or any other rights holder of Tsukihime.**

<p align="center">
   <img src="./media/intro.gif" width="80%" />
</p>

A native Apple silicon Mac build of *Tsukihime -A piece of blue glass moon-*, made by statically recompiling the Nintendo Switch game's own executable. It runs directly on the Mac, without an emulator or Rosetta.

**Based on [tsukire-pc-port](https://github.com/Broomstick-Girl-Magical-Amber/tsukire-pc-port) by Broomstick-Girl-Magical-Amber.** That project did the work of recompiling this game for Windows and Linux; this repository adds the macOS support on top of it, and keeps its git history. For Windows or Linux, use the original project. It is in turn a fork of [SuperMarioWonderRecomp](https://github.com/BelmanteGu/SuperMarioWonderRecomp) by BelmanteGu.

This repository contains **no game, no dumps, no keys, no firmware and no files derived from the game** (the only exceptions are the screenshots in `media/` and the icon; see [Credits and licenses](#credits-and-licenses)). You must provide your own legally obtained copy of the game and your own keys (`prod.keys`). There are no instructions here for getting either.

**AI-assisted project.** The macOS changes were written with Claude (Anthropic), like much of the original port.

## Current status

**Works**

- The Japanese edition starts and runs at about 60 fps on an Apple M2 (8 GB) with macOS 27, from a clean build with the build script.
- Keyboard and mouse controls.
- Community patches in `.nsp` format (tested with one translation patch).
- The game reads its files directly from your dump. Nothing is extracted.

**Not tested yet**

- A full playthrough. Only startup and early play have been run on a Mac, so later scenes may have problems.
- Controller input. The support is written, but no controller has been tried.
- The global edition. The script supports it, but it has not been built on a Mac.
- Other Macs and macOS versions. The memory layout was changed for macOS 27, so older versions may not work.

**Known limitations**

- Apple silicon only.
- Only the first connected controller is used.
- On macOS the guest memory sits above the Switch's 39-bit address range, because macOS 27 occupies the addresses below it. Switch IPC messages are corrected for this (`framework/runtime/src/ipc.cpp`), but other code that assumes 39-bit addresses could still cause crashes.
- Discord Rich Presence is Windows-only.

## Requirements

- An Apple silicon Mac with [Homebrew](https://brew.sh). The script installs everything else.
- A legal dump of the game, **base version v0 (1.0.0) only**, as `.xci` or `.nsp`. An `.nsp` that bundles an update, or an already updated dump, is rejected (updates and patches go in `updates/`).
- Your own `prod.keys`. They are only needed while building: the script does not copy or print them, and the game never uses them.
- Disk space: the dump itself (about 21–23 GB; it stays, because the game reads it) plus about 3 GB per edition.

## Build

```bash
git clone https://github.com/hhaider3/TsukiRE-SwitchtoMac.git && cd TsukiRE-SwitchtoMac

# Put your dump in this folder, and any .nsp updates or patches in updates/ first.
bash tools/mac/build_mac.sh --edition japan --keys /path/to/prod.keys
```

It took about 3 minutes on an M2. Then double-click `play_tsukihime.command` (`play_tsukihime_global.command` for the global edition). **Leave the dump in the folder:** the game reads it every time it starts.

### Options

| Option | Meaning |
|---|---|
| `--edition japan\|global` | Edition to build (default `japan`). |
| `--keys <file>` | Keys file (default `data/prod.keys`). |
| `--input <file>` | Another dump. The game still looks for it in this folder when it runs, unless you set `NX_ROMFS_DUMP`. |
| `--no-updates` | Build the base game and ignore `updates/`. |
| `--jobs <n>` | Compile jobs. |

## Updates and patches

1. Copy the `.nsp` into `updates/`. A name starting with `_` is skipped.
2. Run the build script again.

To turn a patch off, rename its folder in `mods/<edition>/` to `_<name>` and build again. Two active updates with different executables cannot be combined. Loose mods (replacement files) can go in `mods/<edition>/<any name>/romfs/` without rebuilding.

Saves are in `data/tsuki/user/save` (Japanese) and `data/tsuki_global/user/save` (global). Back them up before rebuilding.

## Controls

<p align="left">
  <img src="./media/keyboard_layout.png" width="60%" />
</p>

| Action in game | Keyboard | Mouse | Xbox controller |
|---|---|---|---|
| Advance text | Space, Return or Down | Wheel down | B (right button) |
| Cancel / hide the text window | Delete | | A (bottom) |
| Dialog menu | Tab | Middle click | Y (top) |
| System menu | Esc, S, L or C | Right click | X (left) |
| Select, touch | | Left click | |
| Auto mode | A | | LB |
| Fast-forward | F | | RB |
| Rewind | R (while held) | Wheel up | LT (while held) |
| Speed up while held | Control | | RT |
| Move in menus | Arrow keys | | D-pad |
| Lists, page by page | Fn+Up, Fn+Down | | Left stick |

- **Fullscreen:** Option+Return, or F11 if macOS does not take it.
- **Window size:** Control+1, 2 or 3 (50, 75 or 100% of 1920 pixels wide).
- **Title screen background:** F5 and F6, or click the logo, once you have unlocked more than one. On many Macs the F keys need Fn.
- **Remapping:** copy `framework/games/tsukihime_common/input.ini.example` to `data/tsuki/input.ini` and edit it.

## What the macOS port changes

- `tools/mac/build_mac.sh`: the whole build on a Mac, in place of the PowerShell scripts.
- `framework/runtime/include/nx/cpu.h`, `runtime.h`, `framework/runtime/src/ipc.cpp`: the new memory layout.
- `framework/runtime/src/platform_mac.cpp`, `platform.h`, `framework/games/tsukihime_common/input_tsuki.cpp`, `hle_hid_tsuki.cpp`: keyboard, mouse and controller input.
- The `game.cmake` files and `tsukihime_common.cmake`: Windows-only build steps are skipped, and FFmpeg comes from Homebrew.

Rendering uses the framework's existing MoltenVK support. The Windows and Linux scripts are still in the repository, but only the macOS path has been run here.

## Credits and licenses

This repository is under the **MIT License** ([LICENSE](LICENSE)). [NOTICE.md](NOTICE.md) documents the third-party material included.

- **[tsukire-pc-port](https://github.com/Broomstick-Girl-Magical-Amber/tsukire-pc-port)** by Broomstick-Girl-Magical-Amber, MIT: the port this repository is based on.
- **SuperMarioWonderRecomp** by BelmanteGu, MIT: the base of `framework/`. Its license is kept in `framework/LICENSE` and must stay with any copy of that code.
- **Ryujinx / Ryubing**: only its shader translator is used, offline, to turn the game's Maxwell shaders into SPIR-V. Not included: the script downloads the pinned source from [Codeberg](https://codeberg.org/Software_arch/Ryubing). MIT-licensed.
- **MoltenVK, Vulkan loader, SDL3, FFmpeg, libopus, libjpeg-turbo, libpng**: not included. Homebrew installs them, each under its own license.
- **Noto Sans JP** (`resources/fonts`): SIL Open Font License 1.1, with `OFL.txt` next to the font.
- **Button icon packs**: credits in `resources/HUD-LICENSES.md`.
- **Screenshots and intro animation** (`media/`): captures of the game, property of **TYPE-MOON / Aniplex** and the other rights holders, included only to show the port working. If you are a rights holder and want them removed, open an issue.
- ***Tsukihime -A piece of blue glass moon-*** and its artwork belong to **TYPE-MOON / Aniplex** and the other rights holders. The icon `framework/games/tsukihime_common/tsukihime.ico` derives from artwork of the game and is included only to identify the executable; open an issue and it will be replaced.

## Legal notice and contributing

This is an independent, non-commercial fan project, provided as is, without warranty (see [LICENSE](LICENSE)). It is not affiliated with, endorsed by or sponsored by TYPE-MOON, Aniplex, Broomstick-Girl-Magical-Amber or any other rights holder. It contains no game data and no keys, and it does not help to obtain them. You are responsible for using only your own legally obtained copy of the game and your own keys, in accordance with the law where you live.

Issues and pull requests are welcome. Never attach or commit game data, dumps, keys, firmware or anything derived from the game (`gen`, `analysis`, extracted shaders, textures, audio or video). The root `.gitignore` is an allowlist for this reason: anything not named in it stays out of git.
