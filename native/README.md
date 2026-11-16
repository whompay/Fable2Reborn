# Fable II Windows recompilation

Work in progress. Native build, animated title screen, main menu and entry into
the Bowerstone Old Town loading screen verified. The user also confirms the game
runs and works. Full-game and save/load coverage remain incomplete.

This project statically recompiles the local Fable II PowerPC executable to C++
and builds it for Windows x64 using ReXGlue 0.10.0, matching the approach used by
the local N+ project. It does not execute Xbox firmware or launch Xenia. It does
use ReXGlue runtime services and its Xenia-derived Xenos graphics backend.

## Input

- Title: Fable II, title ID 4D5307F1
- Media ID: 716F0A0D; executable version 0.0.0.26
- Source: the ISO in the parent directory, left unchanged
- Extracted files: ../extracted (451 files)
- ReXGlue SDK: ../tools/rexglue-sdk/win-amd64
- Extractor: ../tools/extract_iso.py (from the local 360tools checkout)

## Build

Run `./build.ps1 -Regenerate` from PowerShell with Visual Studio 2022 C++ Build
Tools installed. Subsequent builds can omit `-Regenerate`.

The application finds `extracted/default.xex` relative to its own location.
Saves and caches use `native/user-data` and `native/cache`.

Run `./package.ps1` after building to copy the native executable and its two
runtime DLLs to the game directory. Double-click `Fable II.exe` there to launch.
This is the actual Windows x64 game build, not a launcher for another process.

Keyboard defaults: Space selects (A), Backspace goes back (B), Enter is Start,
WASD moves, L is X, P is Y, and the mouse controls the right stick. A controller
is also supported by the runtime.

F1 toggles a small FPS/frame-time overlay. It counts the game's VdSwap submissions,
not ImGui refreshes or display refreshes, over a rolling half-second sampling
interval. This is submission rate, not a GPU completion measurement. F11 toggles
fullscreen/windowed. Holding either key down does not repeatedly toggle it.

`fable2_disable_msaa` in `fable2.toml` disables the game's MSAA when true (restart
required). This reduces antialiasing quality and GPU work. Set it to false for
the original setting. The compiled hook is at 0x8238DF3C (original instruction
39200002), based on Margen67's title-specific disable-MSAA patch for media
716F0A0D. The frame counter hook runs after the VdSwap call at 0x82B9CFCC
(original instruction 487160C1). Both are manifest hooks preserved by codegen;
the extracted game files are unchanged.

## Experimental 60 FPS and sharper rendering

`fable2_60fps = true` selects the title-specific 60 FPS presentation interval.
The manifest hook at 0x82B9C8E8 changes r11 from 2 to 1 after the original
`39600002` instruction (Margen67's patch for media 716F0A0D). It does not change
the global clock or apply the separate high-tick-rate patch. Set it to false
and restart to restore the original interval. Quest, animation and physics
behavior at 60 FPS still require gameplay validation.

`resolution_scale = 2` renders supported targets at twice their original width
and height (four times the pixels), for a sharper image. This is higher internal
resolution, not AI/FSR upscaling. The bundled runtime does not include FSR.
Set the scale to 1 and restart if GPU load is too high or an effect looks wrong;
the 60 FPS toggle can remain enabled independently. Higher resolution cannot
add detail missing from original textures or prerecorded video.

Initial testing at 2x in Bowerstone showed about 22 FPS, so the shipped default
is 1x. The initial video presentation also reported an unscaled swap source;
do not assume every pass or movie benefits from internal resolution scaling.
After restarting at 1x, the in-game counter showed 49.5 FPS while moving in
Bowerstone, confirming output above the former 30 FPS cap. This is a spot
measurement, not a stable-60 benchmark or full gameplay validation.

## Moderate visual preset

The active config uses `anisotropic_override = 4` (8x filtering, versus the
runtime's default value 3 / 4x) and `swap_post_effect = "fxaa"`. These features
are present in the bundled graphics DLL. They improve oblique texture filtering
and smooth edges without increasing internal resolution. FXAA can soften fine
detail and UI text. The 60 FPS patch remains enabled and resolution remains 1x.

To undo just this visual tuning, set `anisotropic_override = 3` and
`swap_post_effect = "none"`, then restart. The complete pre-tuning config is
also preserved as `fable2-before-visual-tuning.toml`.

The old emulator setup is isolated in ../pc and is not used by this project.

## Analysis changes

The manifest declares additional function-entry hints for direct
branches reported by validation. These are analysis hints, not runtime patches;
function extents remain inferred by ReXGlue. Initial code generation passed.

CRT setjmp/longjmp were identified at 0x83000200 and 0x82CA9260 by their complete
nonvolatile register save/restore sequence and jump-buffer layout, matching the
CRT implementation previously identified in N+.

Further startup failures were corrected at 0x82CD8650, 0x82CDBAE0, 0x82C1DC80
and 0x829FCB00. An audit added 82 reviewed .rdata table entries whose code begins
at an aligned function boundary. Candidate evidence is recorded in
vtable-candidates-reviewed.json; table slots are recorded alongside each hint.

Further scanning finds exact standalone virtual forwarding thunks and global
getters, verified against the instruction patterns of observed missing calls.
These entries and their evidence are in forwarding-thunks.json and
global-getters.json. The code-pointer scan excludes inline switch tables and
nearby basic blocks. Fourteen reviewed out-of-line address constructions were
added after a runtime failure confirmed candidate 0x822D6678. The corresponding
construction sites are recorded in code-pointers-reviewed.json and the manifest.

Upstream tools: https://github.com/rexglue/rexglue-sdk and
https://github.com/sp00nznet/360tools

## Stability validation

See `stability-pass.md` for the 2026-09-11 pass and outstanding Childhood tests.
Builds run `tools/repair_local_branches.py` after codegen to repair 17 reviewed
internal branch misclassifications, validated against `default.pe`. The repair
rejects unknown cases rather than guessing destinations. Python is required for
this build step; CMake searches the bundled runtime and then the usual PATH.
`build.ps1 -Regenerate` forces codegen, and the repair is reapplied automatically.
Explicit `--user_data_root` and `--cache_root` command-line values now work for
isolated tests; ordinary launches keep the original project-relative paths.

The current generated game build enables AVX2/FMA with strict multiply/add
contraction settings. It requires an AVX2/FMA-capable CPU. Use the CMake option
`FABLE2_AVX2=OFF` for the previous SSE4.1 target. See `performance-pass.md` for
numerical checks, benchmark limits and the preserved baseline executable.
