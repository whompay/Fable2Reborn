<p align="center">
  <img src="docs/assets/logo.png" alt="Fable II Reborn" width="300">
</p>

<h1 align="center">Fable II Reborn</h1>

<p align="center">
  <b>A native Windows port of Fable II, rebuilt from your own disc.</b><br>
  No emulator. The game's Xbox 360 code is recompiled to C++ and runs on your PC.
</p>

<p align="center">
  <a href="https://github.com/whompay/Fable2Reborn/releases/latest"><img src="https://img.shields.io/github/v/release/whompay/Fable2Reborn?include_prereleases&label=download&style=for-the-badge&color=c9a45c" alt="Download"></a>
  <img src="https://img.shields.io/badge/platform-Windows%20x64-3d2a1e?style=for-the-badge&logo=windows&logoColor=white" alt="Windows x64">
  <img src="https://img.shields.io/badge/graphics-DirectX%2012-3d2a1e?style=for-the-badge" alt="DirectX 12">
  <a href="LICENSE"><img src="https://img.shields.io/badge/license-Apache%202.0-3d2a1e?style=for-the-badge" alt="Apache 2.0"></a>
</p>

<p align="center">
  <a href="#-getting-started">Getting started</a> •
  <a href="#-features">Features</a> •
  <a href="#-settings">Settings</a> •
  <a href="#-building-from-source">Building</a> •
  <a href="#-faq">FAQ</a> •
  <a href="#-credits">Credits</a>
</p>

---

> [!IMPORTANT]
> **This repository contains no game code and no game data.** You need your own copy of
> **Fable II** for the Xbox 360 (disc image or extracted files). The installer reads it, recompiles
> the game from it and applies the port's changes on your PC.

> [!NOTE]
> Fable II Reborn is a **work in progress**: the game is playable, but expect rough edges.
> Bug reports are very welcome: see [Reporting bugs](#-reporting-bugs).

## ✨ Features

|  |  |
|---|---|
| ⚔️ **Native PC executable** | Static recompilation with [ReXGlue](https://github.com/rexglue/rexglue-sdk): the game's own code, compiled for x64. No emulator, no JIT. |
| 🎞️ **30, 60 or 120 FPS** | Pick the frame rate in the launcher or the in-game menu. The original 30 FPS stays one click away. |
| 🖱️ **Real keyboard & mouse** | Raw mouse look with adjustable sensitivity and invert, free vertical look, rebindable keys, cursor lock, and keyboard prompts instead of Xbox buttons in the UI. A controller works too. |
| 🖥️ **PC graphics options** | Up to 3x internal resolution, anisotropic filtering, FXAA, the game's own MSAA on or off, VSync, windowed or fullscreen (F11). |
| ⚙️ **PC Settings, in game** | A *PC Settings* entry in the main menu, plus a launcher (`Fable II.exe`) with the same options before you play. |
| 🚀 **Tuned for speed** | GPU submission and readback work reshaped for PC; a fast AVX2 build and a compatible build for older CPUs, picked automatically. |
| 🔄 **Updates built in** | The installer fetches only what changed in this repository and rebuilds what that needs. |

## 🗡️ Getting started

### What you need

- **Fable II** for the Xbox 360: a disc image (`.iso`, Redump or XISO) or its extracted files
  (`default.xex` + `data`). It must be the disc's own `default.xex` (media ID `716F0A0D`); Games on
  Demand copies and title updates don't match.
- **Windows 10/11 x64** with a **DirectX 12** GPU.
- A 64-bit CPU from about 2009 on (SSE4.2). CPUs with **AVX2** (most from 2013 on) get the faster build.
- **Python 3.10+** and **Visual Studio 2022 Build Tools** (C++ workload + *C++ Clang Compiler for
  Windows*). The setup can install both for you.
- About **15 GB** of free disk space.

### Install

1. Download **`Fable II Setup.exe`** from the [latest release](https://github.com/whompay/Fable2Reborn/releases/latest).
2. Run it: **Requirements** (installs Python and the C++ tools if they're missing), then
   **Your game** (pick your `.iso` or game folder), then **Install location**.
3. Wait for the build. The first one recompiles the whole game and takes a while.
4. Press **Play**, or use the **Fable II** shortcut on your desktop.

Running the setup again on the same folder repairs or updates the install and only redoes what changed.

<details>
<summary><b>Installing from the command line</b></summary>

```bat
installer\fable2-installer --iso "D:\Games\Fable II.iso"     :: copy the disc into extracted\, then build
installer\fable2-installer --game "D:\Games\Fable II"        :: use an already extracted disc
installer\fable2-installer doctor --game extracted           :: check the prerequisites and the game version
installer\fable2-installer update                            :: fetch the latest changes and rebuild
```

The full details are in [installer/README.md](installer/README.md).
</details>

## 🎮 Controls

Keyboard and mouse are on by default; plug in a controller and the game switches to it.

| Action | Key |
|---|---|
| Move | `W` `A` `S` `D` |
| Look around | Mouse |
| Interact / confirm (A) | `F` |
| Back / cancel (B) | `C` |
| Pause menu (Start) | `Esc` |
| Sneak (left-stick click) | `Ctrl` |
| D-pad | Arrow keys |
| FPS counter | `F1` |
| Fullscreen / windowed | `F11` |

Every binding can be changed in `native/fable2.toml` (`keybind_*`).

## 🛠️ Settings

Settings live in **`native/fable2.toml`**. You can change them three ways: the launcher
(`Fable II.exe`), the **PC Settings** entry in the game's main menu, or by editing the file. Frame
rate, resolution and anti-aliasing take effect after a restart.

| Setting | Key | Options |
|---|---|---|
| Frame rate | `fable2_frame_rate` | `30` (original), `60`, `120` |
| Render resolution | `resolution_scale` | `1`, `2`, `3` |
| Texture filtering | `anisotropic_override` | `0` off … `5` 16x |
| Anti-aliasing | `swap_post_effect` | `"none"`, `"fxaa"`, `"fxaa_extreme"` |
| Game MSAA | `fable2_disable_msaa` | `true` (faster) / `false` (original) |
| VSync | `vsync` | `true` / `false` |
| Mouse sensitivity | `fable2_mouse_sensitivity` | `0.25` … `3.0` |
| Vertical sensitivity | `fable2_mouse_y_scale` | `0.5` … `2.0` |
| Invert vertical look | `fable2_mouse_invert_y` | `true` / `false` |
| Lock cursor to the game | `fable2_lock_cursor` | `true` / `false` |

Delete `native/fable2.toml` to get the defaults back. Saves are kept in `native/user-data`.

## 🏗️ Building from source

The installer is the supported way to build: it runs every step below in order and checks each
result. For development:

```bat
:: the installer, the setup program and the launcher (Rust)
cd installer
cargo build --release

:: then build the port from your disc (generates native/generated, builds native/ and native/gpu)
installer\target\release\fable2-installer --iso "D:\Games\Fable II.iso"
```

<details>
<summary><b>How the port is put together</b></summary>

1. `tools/extract_pe.py` decrypts and unpacks `default.xex` from your disc.
2. **ReXGlue** recompiles the PowerPC code into C++ (`native/generated/`, never published).
3. The port's hand edits to that code (`installer/patches/`) and its game-data changes
   (`installer/data-patches/`: keyboard prompts, camera and script tweaks) are applied and
   hash-checked, so every install matches the maintainer's tree byte for byte.
4. CMake + Ninja + clang build the Xenos graphics plugin (`native/gpu`) and the game (`native/`).

| Folder | What's in it |
|---|---|
| `native/src/` | The port's own code: PC settings, input, mouse look, frame rate, glyphs |
| `native/gpu/` | The D3D12 graphics plugin build (with Fable-specific performance work) |
| `installer/` | Installer, setup program, launcher, patches and data patches |
| `tools/` | Build helpers, the ReXGlue SDK and its sources |

</details>

## ❓ FAQ

<details>
<summary><b>Do I need the game?</b></summary>

Yes. Nothing from Fable II is in this repository or in the setup download. The installer builds
everything from your own copy.
</details>

<details>
<summary><b>Is this an emulator?</b></summary>

No. The game's code is translated to C++ ahead of time and compiled into a normal Windows program.
The ReXGlue runtime provides the Xbox 360 system services and a Xenia-derived graphics backend.
</details>

<details>
<summary><b>Does 60 or 120 FPS change the game?</b></summary>

The game's own timing is kept; only how often it draws a frame changes. Early testing at 120 FPS
showed no problems. If something looks off at a higher frame rate, please report it, and switch
back to 30 FPS to compare.
</details>

<details>
<summary><b>Busy towns run slower than open areas</b></summary>

That's expected for now. Towns like Bowerstone have thousands of draw calls per frame, and that
work is still being optimised.
</details>


## 🐞 Reporting bugs

Please [open an issue](https://github.com/whompay/Fable2Reborn/issues/new/choose) and include:

- what happened, and where in the game;
- your CPU, GPU and Windows version;
- the newest log from `native/out/build/<build>/logs/` (`fable2_###.log`);
- a screenshot or short clip for anything visual.

## 🙏 Credits

- **[ReXGlue](https://github.com/rexglue/rexglue-sdk)** by Tom Clay and contributors: the
  recompiler and runtime this port is built on.
- **[Xenia](https://github.com/xenia-project/xenia)**: the Xbox 360 research the runtime and graphics
  backend come from.
- **Margen67**: the title-specific 60 FPS and MSAA patches for media `716F0A0D`.
- **Lionhead Studios**, for making Albion.

## ⚖️ Legal

Fable II Reborn is an unofficial fan project. It is **not affiliated with, endorsed or sponsored
by Microsoft, Xbox Game Studios or Lionhead Studios**. *Fable* is a trademark of Microsoft
Corporation. This repository contains no copyrighted game code or assets; you need a legally
obtained copy of the game. The Fable II Reborn emblem is the project's own artwork.

The port's own code is licensed under the **[Apache License 2.0](LICENSE)**. Third-party
components keep their own licenses; see [NOTICE](NOTICE).
