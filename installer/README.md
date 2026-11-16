# fable2-installer

Builds the Fable II (Xbox 360, title 4D5307F1) PC port from the player's **own** copy of the game.
The published port holds no game code and no game data. The installer regenerates all of it from
the player's disc, applies the port's changes, compiles it and makes a launcher.

It is the same installer as the Godfather port's, adapted to an Xbox 360 game: it reads the disc
image (XDVDFS) itself, checks `default.xex` instead of a PS3 EBOOT, runs ReXGlue's code generator
instead of the PS3 lifters, and applies the port's game-data changes with `fable2_data.py`.

It is written in Rust. Everything that touches the game files has no dependencies; only the setup
window (eframe) and the launcher dialog (windows-sys) do:

```bat
cd installer
cargo build --release
:: -> installer\target\release\fable2-installer.exe, fable2-setup.exe, fable2-launch.exe
```

## For players

You need:

* **Your Fable II disc**: a disc image (`.iso`, a full Redump image or a trimmed XISO) or a folder
  with its extracted files (`default.xex` + `data`). It must be the disc's own `default.xex`
  (media ID 716F0A0D); a title update or a Games on Demand copy has a different one.
* **Python 3.10+** from python.org, with "Add python.exe to PATH" ticked.
* **Visual Studio 2022 Build Tools** with the *Desktop development with C++* workload and the
  *C++ Clang Compiler for Windows* component (the port builds with its `clang`/`clang++`).
* A 64-bit CPU from about 2009 on (x86-64-v2: SSE4.2 + POPCNT) and a DirectX 12 GPU. With
  **AVX2/FMA** (most CPUs from 2013 on) the installer builds the fast version; without it, the
  compatible version (see "Processors" below).
* About 15 GB of free disk space (7 GB of game files, 0.3 GB of generated C++, the build); about
  4 GB more if both versions are built.

The easy way is **`Fable II Setup.exe`**: Welcome → Requirements (Python and the C++ tools, with
**Install** buttons that use winget) → Your game (pick the `.iso` or a folder) → Install location →
progress bar → **Play**. It carries the whole publishable tree inside itself, extracts it to the
chosen folder, copies the game out of the disc image into `extracted\`, runs
`fable2-installer install` there and creates `Fable II.exe` (the launcher) plus desktop and
Start-menu shortcuts. Running it again on the same folder repairs/updates and only redoes what
changed.

From a terminal:

```bat
installer\fable2-installer --iso "D:\Games\Fable II.iso"     :: copy the disc into extracted\, then build
installer\fable2-installer --game "D:\Games\Fable II"        :: an already extracted disc (linked, not copied)
installer\fable2-installer doctor --game extracted           :: check the prerequisites and the game version
installer\fable2-installer --cpu both                        :: build the AVX2 and the compatible version
installer\fable2-installer extract-iso --iso game.iso --out DIR
```

The installer runs these steps:

1. It reads the XEX2 header of `default.xex` and refuses any game that isn't title 4D5307F1, then
   checks the SHA-256 of `default.xex` against the version the port was made from.
2. It makes the game available as `extracted\` in the checkout (a disc image is copied there; a
   folder given with `--game` is linked there with a directory junction).
3. It creates a private Python environment in `.installer\venv` (the `cryptography` package).
4. It runs the generation pipeline (`[[step]]` in `fable2-installer.toml`):
   `tools/extract_pe.py` (decrypt and unpack `default.xex` to `native/default.pe`),
   `rexglue codegen` (the recompiled C++ in `native/generated/`; the long step; it is given
   `installer/codegen.partition.json` first, the maintainer's function-to-file layout, which
   ReXGlue keeps stable between runs: without it the functions land in different files),
   `tools/repair_local_branches.py`, and `fable2_data.py apply` (the port's changes to the game
   data: keyboard/mouse button prompts in the GUI texture banks, the camera and reward script
   edits in `gamescripts_r.bnk`).
5. It checks every generated file against the hash recorded by the maintainer, then applies
   `installer/patches/` (the port's hand edits to the generated code) and checks again. The result
   is byte-identical to the maintainer's tree.
6. It builds the Xenos graphics plugin (`native/gpu`) and the game (`native/`) with CMake, Ninja
   and clang in their own `native/out/<flavor>-gpu` and `native/out/build/<flavor>` folders (the
   same settings as `native/build.ps1`, PGO off), copies the plugin next to `fable2.exe`, and
   creates `native/fable2.toml` from `installer/fable2.default.toml` if there is none yet. This
   runs once per processor version it builds.
7. It writes `Play Fable II.cmd` (the build this PC runs) and `installer/launch.txt` (what
   `Fable II.exe` starts; it holds both builds when both were made).

Finished steps are skipped when you run the installer again, so an interrupted install continues
where it stopped. `--regen` starts the pipeline over.

**`Fable II.exe`** (the launcher) shows the PC settings first: display mode, VSync, 30/60/120 FPS,
render resolution, texture filtering, FXAA, the game's MSAA, keyboard & mouse, mouse sensitivity,
invert look and cursor lock. They are the cvars in `native/fable2.toml`, the same file the in-game
PC Settings menu writes; **Play** saves them and starts the game. `Fable II.exe --play` starts the
game straight away.

### Processors

The port can be built two ways (`[build] cpu_variants` in `fable2-installer.toml`):

| version | flavor folder | CMake | runs on |
|---|---|---|---|
| fast | `native/out/build/install` | `FABLE2_AVX2=ON`, `FABLE2_GPU_AVX2=ON` (AVX2, FMA, BMI1/2, LZCNT) | Haswell / Excavator and newer |
| compatible | `native/out/build/install-compat` | both `OFF` (SSE4.1 code) | any x86-64-v2 CPU (Nehalem / Bulldozer and newer) |

The ReXGlue runtime (`rexruntime.dll`) is built for x86-64-v2, so it serves both. Both versions
run the same game: the code generator's output and the port's patches are shared, only the
compiler flags differ, and the game math is kept strict (`-ffp-contract=off`; `std::fma` is exact
with or without the FMA instruction), so results do not change, only speed.

By default (`--cpu auto`) the installer builds the version this PC can run, plus any version an
earlier install built (so updates keep both up to date). `--cpu both` (the setup's "Also build
the version for ..." checkbox) builds both, for a folder that will be copied to another PC;
`--cpu avx2` / `--cpu compat` build just that one. `installer/launch.txt` names each build
(`exe=` / `cwd=` for the fast one, `exe_compat=` / `cwd_compat=` for the compatible one), and
`Fable II.exe` checks the processor at every start and runs the right one.

### Game data

`fable2_data.py apply` changes the game's own files inside `extracted\`. It keeps a copy of every
original it overwrites in `.installer\work\data-originals\`, can always start again from those,
and is safe to run any number of times. `fable2_data.py check --game extracted --patches
installer\data-patches` tells you which files are in their final state. If you pointed `--game` at
a folder of your own, that folder is the one that gets changed.

### Updating an installation

The launcher checks for updates every time it opens (in the background, through
`fable2-installer update --check`; skipped offline, in a development tree, or with
`FABLE2_NO_UPDATE_CHECK=1`). When GitHub has a newer version it shows an **Update** button, which
saves the settings, closes the launcher and runs `Update Fable II.cmd` in a console window.

```bat
installer\fable2-installer update            :: download what changed on GitHub, rebuild what that needs
installer\fable2-installer update --check    :: only report whether an update is available
```

The repository is set in `fable2-installer.toml` (`[update] repo / branch / path`):
[github.com/whompay/Fable2Reborn](https://github.com/whompay/Fable2Reborn). The updater works exactly as the
Godfather installer's does: it compares git blob hashes, downloads only the files that differ,
removes files an earlier update installed that the repository no longer has, and regenerates the
game code only when `generated.lock` changed.

## For the maintainer

Two patch sets are made once on the development tree, and again whenever what they cover changes:

```bat
:: 1. the game-data changes (reads the originals straight from the disc image, takes a few seconds)
python installer\fable2_data.py make --iso "pc\Fable II (USA, Europe) (En,Zh,Ko,Pl,Cs,Hu,Sk) (Rev 2).iso" --tree extracted --out installer\data-patches

:: 2. the hand edits to the generated code (runs the whole pipeline on a clean copy: takes a while)
installer\target\release\fable2-installer make-patches --game extracted

:: 3. the setup program players download
installer\target\release\fable2-installer package --out "dist\Fable II Setup.exe"
```

(`installer\make-patches.cmd` and `installer\build-setup.cmd` run these.)

* `fable2_data.py make` compares the files listed in its `DEFAULT_TARGETS` (the `_gui_kbm` /
  `_gui_x360` bank copies, `gui_textures*.bnk`, `gui_texture_headers.bnk`, `gamescripts_r.bnk`)
  with the disc's originals and writes `installer/data-patches/`: plain copies, byte ranges (the
  port's glyph art), and for `gamescripts_r.bnk` only the edited bytes of the *decompressed* Lua
  bytecode of the four changed scripts. It rebuilds every file from the originals as a self-check
  and refuses to write a set in which any patch carries more than half of its original file.
  As made on 2026-11-16: 17 targets, 700 KB of patch data, 378 bytes of it script edits.
  Banks are rebuilt with zlib level 9; a different zlib build may produce different compressed
  bytes, so for banks the installer verifies the decompressed content instead of the file hash.
* `make-patches` exports a clean tree to `.installer\fresh` (exactly what a player gets), runs the
  pipeline there, compares the output with your `native/generated/` (and `native/default.pe`,
  `native/local-branch-repairs.json`), and writes `installer/generated.lock` and
  `installer/patches/`. It doesn't change any of your files. It refuses to finish if publishing a
  patch would amount to publishing generated content (a binary file differs, the pipeline doesn't
  produce a file, or a patch carries more than half of a file). The codegen bookkeeping files
  (`codegen.d`, `codegen.stamp`, `codegen.build.stamp`) hold machine paths and are "loose": never
  patched or hash-checked.
* `export --out DIR` copies the publishable tree: `installer/`, the `native/` sources, the build
  helpers in `tools/`, the ReXGlue SDK (`tools/rexglue-sdk/win-amd64`, which carries the port's
  own `rexruntime.dll`), the parts of `tools/rexglue-source` the graphics plugin is built from, and
  the port's glyph art in `pc/assets/glyphs/`. Nothing in `[generated]`, nothing excluded in
  `[export]` (notes, handoffs, logs, saves, caches, the old emulator in `pc/`, the game), and
  `[export] forbid` makes it stop if a game file would get through. As of 2026-11-16: 2,643 files,
  228 MB, most of it the SDK. **Review the result before pushing**; in particular whether the
  ReXGlue SDK's and sources' licences let you redistribute them, and whether everything in
  `installer/data-patches/` is the port's own work.

Everything about the game and the port is in `fable2-installer.toml`: pipeline steps, which files
count as generated, build commands, launcher arguments and export rules. You can change it
without rebuilding the installer.

### Line endings

The pipeline's Python tools write LF; `repair_local_branches.py` writes the generated files with
LF. If your maintained generated files use CRLF throughout, the patch starts with `# eol crlf` and
the installer converts before applying the hunks. Run `make-patches` on Windows, because that is
what players run.
