#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <atomic>
#include <mutex>
#include <rex/ui/window_listener.h>
#include <rex/ppc/context.h>

namespace fable2::glyph {

enum class InputDevice {
  kController = 0,
  kKBM = 1,
};

// Initialize glyph system and register cvar listeners
void Initialize();

// Re-applies the VFS bank redirect for the current input device. Call this
// from Fable2App::OnPostLoadXexImage() (right after VFS exists, and well
// before the engine's "Permanent Bank Mounter" thread does its one-time,
// cached directory mount of art/gui) -- Initialize() alone runs too early
// (VFS doesn't exist yet). See glyph_system.cpp's Initialize()/
// RefreshBankRedirect() comments for the trace evidence, and the
// kGuiDirKbmTarget/kGuiDirX360Target comment for why the mirror directory
// names (_gui_kbm/_gui_x360) must never start with "gui".
void RefreshBankRedirect();

// Device state management
void SetActiveInputDevice(InputDevice device);
InputDevice GetActiveInputDevice();
bool IsKbmActive();

// Distinguishes the four mouse callbacks Fable2App forwards here, so
// OnMouseEvent can update the input_bindings.h held-state layer correctly:
// a button down/up toggles that button's held state, a wheel tick latches a
// brief pulse (see input_bindings.cpp), and plain movement does neither (it
// only participates in device-switch detection, unchanged).
enum class MouseEventKind {
  kDown,
  kUp,
  kMove,
  kWheel,
};

// Window input listeners (called by Fable2App). `is_down`/`kind` let this
// forward into input_bindings.h's held-state tracking (NotifyKeyState /
// NotifyMouseButtonState / NotifyMouseWheel) in addition to the existing
// device-switch detection, so gameplay input (via OnInputPolled ->
// ApplyBindingsToGamepadState) and displayed glyphs are driven from the
// exact same GameAction -> KbmBinding source of truth.
void OnKeyEvent(const rex::ui::KeyEvent& event, bool is_down);
void OnMouseEvent(const rex::ui::MouseEvent& event, MouseEventKind kind);
void OnPollController();

// Hook called when a GUI asset name is resolved to a resource. CORRECTED
// (2026-09-11): the actual call site is NOT inside sub_82BFD780 itself --
// sub_82BFD780 is the plain resolve-by-name function and is never patched.
// The real hook is patched into sub_82817748 (native/generated/default/
// fable2_recomp.69.cpp, ~line 13274), immediately after ITS call to
// sub_82BFD780 returns, using the resolved resource pointer (r3) and the
// name string pointer loaded from that call's argument struct. sub_82817748
// is one of the three helper functions the "central button glyph
// dispatcher" (sub_8226EA00, fable2_recomp.211.cpp) invokes 40x per
// dispatch -- confirming the menu widget-setup path does reach this hook,
// not just title-screen/world-prompt code. See glyph-vfs-redirect-status.md
// for the full menu-glyph investigation.
void OnResourceResolved(uint32_t name_guest_ptr, uint32_t resource_guest_ptr, uint8_t* base);

// Hook called when guest polls controller via sub_822B2D60 / XamInputGetState
void OnInputPolled(uint32_t user_index, uint32_t state_guest_ptr, uint32_t xam_status, uint8_t* base);

// Update all registered texture slots according to current device & keybindings
void UpdateAllSlots();

// 2026-11-15 Live glyph switching (see the block at the end of
// glyph_system.cpp and AI_HANDOFF.md "## 2026-11-15 Live glyph switching").
// Wakes the background worker that re-binds resident glyph textures and
// tracked Anark render objects to the active device. Called by
// SetActiveInputDevice() on every real device change; safe from any thread.
void NotifyLiveGlyphDeviceChanged();

// Called from sub_828152D0 (fable2_recomp.50.cpp) for EVERY render-object
// creation, regardless of device. Remembers objects whose original
// model/hash/UV match a kAnarkRenderBindings rule so they can be flipped live
// later, then applies CorrectAnarkRenderBinding() only if KBM is active (the
// previous creation-time behaviour). Returns true if hash/UVs were changed.
bool OnAnarkRenderObjectCreate(uint32_t model, uint32_t object, uint32_t& hash, double& su,
                               double& sv, double& u, double& v);

// Read-only trace of the world A texture's bank payload after completed I/O.
// DISPROVEN 2026-09-14 (runtime): a full build+run produced zero
// [InteractGlyph] log lines at all, meaning sub_82CC3640/sub_82CA2C60 (the
// generic file-read/memcpy pair these hook) are simply never on the call
// path the world Interact prompt actually takes. Left in place (harmless,
// still gated/capped) but no longer the active investigation -- see
// TraceAnarkRenderObjectCreated() below for the current lead.
void TraceWorldInteractRead(uint32_t handle, uint64_t offset, uint32_t buffer,
                           uint32_t length);
void TraceWorldInteractCopy(uint32_t destination, uint32_t source, uint32_t length,
                           uint32_t caller, uint8_t* base);

// Read-only trace, unconditional (not content-gated like the two functions
// above): logs every render-object creation that passes through
// sub_828152D0 (native/generated/default/fable2_recomp.50.cpp) while KBM is
// active -- this is the SAME, already-proven-working call site that drives
// the FAC SPACE-keycap substitution for the main-menu selected-row A prompt
// (see CorrectAnarkRenderBinding()/kAnarkRenderBindings in
// anark_render_bindings.h, and glyph-vfs-redirect-status.md's "green A"
// entries). Purpose: prove or disprove whether the WORLD Interact prompt is
// also created through this same Anark render-object path (and if so, with
// which `model` id) before attempting any substitution here. Capped per
// distinct model id so a busy scene can't flood the log.
void TraceAnarkRenderObjectCreated(uint32_t model, uint32_t hash_before);

// 2026-09-15: "Unlock" world door hold-prompt cleanup. Structurally parsed
// art/gui/gameface/abilitiesscreen.bgf (see glyph-vfs-redirect-status.md's
// "fully mapped by name" entry) and identified the exact Anark component
// nodes behind this prompt's four render-objects: `green_top`(6709)/
// `green_top_translucent`(6714) are the F-glyph fill+duplicate (already
// correctly redirected by kAnarkRenderBindings/CorrectAnarkRenderBinding --
// left untouched here), `rim_and_red`(6702) is the native, permanently
// disabled-redirect "glow" highlight badge (two prior attempts to redirect
// its texture both caused a pink/magenta regression -- see
// prepare_anark_render_bindings.py's docstring), and `KeyMarker`(6723) is a
// standalone `icon_lock` padlock badge. The user wants ONLY the F glyph
// visible for this specific KBM prompt, with `rim_and_red`/`KeyMarker` (and
// the separate, non-sprite `KeyLight` light effect, id 6722, which never
// reaches this function at all -- see below) hidden, `green_top`/
// `green_top_translucent` fully untouched, and controller mode completely
// unaffected.
//
// Per the user's explicit instruction, this suppresses the *draw*, rather
// than redirecting `rim_and_red`'s texture/UV a third time (the exact
// operation already proven, twice, to regress to solid pink/magenta
// regardless of target content -- see anark_render_bindings.h's header
// comment). Returns true only when `model` is one of the two evidenced,
// sprite-rendered (kind=1, has a SOURCE/hash) nodes in this exact
// component pairing, and only while KBM is active -- gamepad/controller
// mode is untouched by construction (the call site already only reaches
// this under IsKbmActive()). `KeyLight` (6722) is a `kind=3` node with no
// SOURCE property at all -- confirmed absent from every
// [WORLD-ANARK-TRACE] line in the evidence log, meaning it is a real-time
// light effect handled by a completely different (unresearched this
// round) code path, not a sprite this function's call site ever sees --
// so it is intentionally NOT included here; see
// glyph-vfs-redirect-status.md for why.
bool ShouldSuppressAnarkRenderObjectForKbm(uint32_t model);

// Read-only trace, unconditional, entry of sub_823FACE0
// (native/generated/default/fable2_recomp.1.cpp) -- confirmed by
// native/anark-scene-xrefs.json (static string cross-reference, not a
// guess) to be the function that loads "HUD.bgf" by name via sub_8222CF18.
// NEW LEAD (2026-09-14): offline-parsed hud.fac/hud.bgf directly with the
// project's own gameface-inspect tooling (inspect_anark_bgf.parse(), same
// logic prepare_anark_render_bindings.py uses) and found the world-HUD
// "AButton" node (path .../TopBox/Buttons/Component/RightButton/HoldButton/
// Button/AButton/unselected/Material365/abyx, render-object id 2530) is
// compiled with hash=0x0D405C87 (=222321799=ABYX, the same shared atlas
// CorrectAnarkRenderBinding() redirects the MENU's selected-row icon to)
// and uv=(0.25,0.25,0,0.75) (the same UV rect that fix's "after" target) --
// ALREADY, statically, in the FAC data itself, unconditionally, for every
// input device. That means, unlike the menu's selected-row badge, this HUD
// element needs NO runtime correction to reach that UV/hash -- it's baked
// in at FAC-compile time. This fully explains why neither
// TraceWorldInteractRead/Copy nor TraceAnarkRenderObjectCreated ever fired
// for it: there is no per-frame or per-bind runtime call to intercept for
// this specific node; the only runtime work left is however sub_823FACE0/
// sub_8222CF18 turn the compiled HUD.bgf node graph into a live render
// object once, at HUD load. This function proves whether that scene-load
// entry point is even reached during a captured session, as the first,
// necessary (not yet sufficient) step toward finding where the AButton
// node specifically becomes a render object.
void TraceHudSceneLoad();

// Read-only trace, unconditional, entry of sub_8222CF18
// (native/generated/default/fable2_recomp.109.cpp) -- the generic
// "load resource by name" primitive. Confirmed by direct disassembly
// reading (not a guess) that ALL THREE scene-name loaders found so far
// funnel a null-terminated resource-name string into this exact function
// as their final step: sub_823FACE0 (the confirmed-by-string-xref
// "HUD.bgf" loader, itself proven NOT reached during a full gameplay
// session -- see TraceHudSceneLoad), sub_8281AFB0 (the ~30 pause/menu
// screen loaders, e.g. "PauseMainMenu.bgf"/"OptionsScreen.bgf"/etc.), and
// sub_823FC460 (a generic presentation-name loader called from
// sub_8281A568, the scene node-walk function that ultimately invokes
// sub_828152D0 for "selected-row" render-object creation). Since the one
// specific named loader we tried for HUD.bgf never fired, this hooks the
// shared choke-point ALL of them funnel through instead of guessing at
// another single wrapper: it logs the actual resource-name string (r4) on
// every call, so a live gameplay session with the world HUD visible will
// show us every scene/resource name actually loaded, whether or not it
// matches any function we've already tried tracing. Capped at a fixed
// total call count (not per-name) so a busy load screen can't flood the
// log. Uses the same safe bounded-length/printable-char string read as
// OnResourceResolved().
void TraceResourceLoadByName(uint32_t name_guest_ptr, uint8_t* base);

}  // namespace fable2::glyph
