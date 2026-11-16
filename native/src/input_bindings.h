#pragma once
//
// game action -> current binding -> glyph
//
// Fable II is a native Xbox 360 title: the guest game code only ever
// understands XInput controller buttons/axes (see ButtonType in
// glyph_system.cpp, which mirrors this 1:1). There is no separate
// "game action" vocabulary to invent -- rebinding an action means rebinding
// which physical key or mouse control currently drives a given controller
// button, and this file is that binding layer: a real, registered,
// hot-reloadable rex::cvar per rebindable button (not the never-registered
// "keybind_*" cvar reads glyph_system.cpp used to do, which always fell
// through to a hardcoded string because nothing ever defined them), parsed
// into a typed, device-kind-aware KbmBinding, and rendered to a glyph name.
//
// Scope note (2026-09-13, revised 2026-09-15): kLeftStick/kRightStick are NOT
// included here. Fable II's left stick is analog 360-degree movement and the
// right stick is analog camera look -- both are inherently multi-key/analog
// composites (WASD, mouse-delta), not a single rebindable key or mouse
// button, so they stay fixed composite glyphs ("wasd", "mouse_look") at the
// glyph_system.cpp call site rather than being forced through this
// single-binding abstraction. kDPad WAS also excluded for this same reason
// originally, but that was wrong -- unlike the sticks, the D-pad's four
// directions are each a single discrete digital bit, exactly like Start/
// Back/LB/RB, so as of 2026-09-15 they ARE four ordinary GameActions
// (kDPadUp/Down/Left/Right, see below) used for Fable III-style "context
// action" hotkeys (F1-F4 by default). See AI_HANDOFF.md for the full
// writeup of what each round did and did not cover.
//
// Fable III-style combat-mode system (2026-09-14; LMB/RMB simplified in a
// later 2026-09-14 round): X, Y and B are no longer each driven by their
// own fixed physical-key cvar (keybind_x/keybind_y still exist below but
// are legacy -- see their declarations -- and are not consulted for
// activation anymore; keybind_b was REPURPOSED on 2026-09-15, see below,
// rather than left legacy). Fable II itself ties weapon-select and attack
// to the SAME button (X = select melee + attack, Y = select ranged +
// attack, B = select magic + attack), so there is no native "just switch
// stance" input to bind separately.
//
// The CombatMode enum below originally gated melee/ranged behind explicit
// 1/2/3 mode-select keys (a Fable III PC-style scheme). That gate was
// removed once (a user testing it found LMB/RMB always triggered melee,
// because the mode never left its kMelee default unless a mode-select key
// was actively held) but the fix at the time still had X vs. Y decided by
// an if/else on GetCurrentCombatMode() -- i.e. still one shared read
// deciding both buttons' outcome. That was the same bug class in a
// smaller form: mode drift (a stray wheel tick, a leftover "1"/"3" press,
// or testing a not-yet-rebuilt binary) could make both LMB and RMB agree
// on the same guest button again (this time both landing on Y instead of
// both on X). Fixed for real in a third round: X and Y are now each a
// single independent if-statement reading only their own mouse button --
// see ApplyBindingsToGamepadState -- with NO shared branch, switch, or
// mode read between them at all. Magic is the one case still gated behind
// an explicit hold (default key "2", CombatMode::kMagic): while held, LMB
// ALSO drives B (magic attack), alongside (not instead of) X -- fixing
// that interaction is explicitly out of scope for now (see
// ApplyBindingsToGamepadState's comment). See AI_HANDOFF.md for the full
// writeup, including why RMB has no mapped action in Magic mode and why
// melee-block/ranged-aim can't be shown as a distinct RMB glyph.
//
// B button repurposed for menu Back/Cancel (2026-09-15): Fable II's B
// button, like A, is itself contextually overloaded on real Xbox 360 --
// it is the universal Xbox "Back/Cancel" button in menus AND (per the
// combat-mode system above) the Magic-mode attack button in combat, same
// bit, different context, exactly like A's interact/roll/sprint overload.
// keybind_b's default is now "Escape" (was "Shift", legacy/unused) and
// ApplyBindingsToGamepadState ORs its held-state into the B bit alongside
// the Magic-mode LMB dispatch, rather than replacing it -- both a menu
// Back/Cancel press and a Magic attack use the same underlying bit,
// resolved by Fable II's own context exactly as A already is.
//
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>

namespace fable2::glyph {

// What kind of physical control a binding currently resolves to. Kept
// distinct (rather than one flat "key name" string) so callers -- glyph
// resolution today, a settings UI or on-screen prompt tomorrow -- can tell
// a keyboard key, a mouse button, and a wheel direction apart, and so a
// controller-mode caller never has to guess.
enum class BindingKind {
  kUnbound,
  kKeyboardKey,
  kMouseButton,
  kMouseWheelUp,
  kMouseWheelDown,
};

struct KbmBinding {
  BindingKind kind = BindingKind::kUnbound;
  rex::ui::VirtualKey key = rex::ui::VirtualKey::kNone;
  rex::ui::MouseEvent::Button mouse_button = rex::ui::MouseEvent::Button::kNone;

  bool is_bound() const { return kind != BindingKind::kUnbound; }
};

// The rebindable Xbox 360 controller buttons -- one per single-key/single-
// mouse-control action. Matches glyph_system.cpp's ButtonType exactly for
// these fourteen; kLeftStick/kRightStick are deliberately absent (see file
// comment above). kDPadUp/Down/Left/Right added 2026-09-15 for Fable
// III-style "context action" hotkeys (F1-F4 by default) -- Fable II's own
// D-pad is "Context-Sensitive Shortcuts" per its manual, so this is a
// faithful mapping, not an invented one.
enum class GameAction {
  kA,
  kB,
  kX,
  kY,
  kLeftTrigger,
  kRightTrigger,
  kLeftShoulder,
  kRightShoulder,
  kStart,
  kBack,
  kDPadUp,
  kDPadDown,
  kDPadLeft,
  kDPadRight,
};

// The rex::cvar name backing this action's binding (e.g. "keybind_a"). These
// are the exact names glyph_system.cpp's Initialize() already registers
// RegisterChangeCallback() against (to call UpdateAllSlots() on change) --
// that wiring predates this file and was already correct, it just had
// nothing registered to watch. Defining the cvars here (see
// input_bindings.cpp) is what makes it live.
const char* GameActionCvarName(GameAction action);

// Human-readable description, for a future settings UI / config dump.
const char* GameActionDescription(GameAction action);

// Re-reads the action's cvar right now, parses it as a comma-separated list
// (see ParseBindingList/GetCurrentBindings below), and returns the PRIMARY
// binding -- the first entry in that list that actually parsed to something
// bound. Returns a default-constructed (kUnbound) KbmBinding if the cvar is
// empty or every comma-separated entry failed to parse. This is what the
// glyph system uses (a rebindable action shows one glyph, not a strip of
// several) -- gameplay activation instead uses IsActionActive(), which
// checks every entry in the list, not just this primary one. Never caches --
// a rebind (SetFlagByName, a config edit, a future settings UI) is visible on
// the very next call, no separate invalidation step required.
KbmBinding GetCurrentBinding(GameAction action);

// Re-reads the action's cvar right now and parses it as a comma-separated
// list of bindings (see ParseBindingList). Never caches, same as
// GetCurrentBinding. Used by IsActionActive() so every listed binding, not
// just the first, can hold the action active.
std::vector<KbmBinding> GetCurrentBindings(GameAction action);

// Parse a single binding token ("Space", "LMB", "Q", "WheelUp", ...) into a
// binding. Mouse tokens (LMB/RMB/MMB/MB4/MB5/WheelUp/WheelDown) are
// recognized first; everything else goes through rex::ui::ParseVirtualKey()
// (the SDK's own exact keyboard-name parser) instead of the old crude "does
// the string contain this substring" scan, so e.g. a future "LeftControl"
// binding for some other action doesn't get misread as "L". Does NOT split
// on commas -- pass it one token at a time (see ParseBindingList for a full
// cvar value).
KbmBinding ParseBindingString(std::string_view value);

// Parse a full cvar value as a comma-separated list of binding tokens (e.g.
// "Q,I", or a plain single token like "Space" with no comma at all -- that
// case yields a one-element list, identical to how every existing binding
// behaved before multi-binding support existed). Each token is trimmed of
// surrounding whitespace and passed individually to ParseBindingString;
// empty tokens (stray/doubled commas, trailing comma) are skipped. A token
// that fails to parse still occupies its position in the returned list, as
// an kUnbound KbmBinding -- IsBindingActive() on it is simply always false,
// so it cannot make the action active but doesn't silently drop other valid
// entries either.
std::vector<KbmBinding> ParseBindingList(std::string_view value);

// Inverse of ParseBindingString, for round-tripping/serializing a binding
// (e.g. if a future settings UI needs to write a chosen binding back out).
std::string BindingToString(const KbmBinding& binding);

// Glyph asset name (see pc/assets/glyphs/*.bin, loaded by
// glyph_system.cpp's GetGlyphPixels()) for a binding, or "" if this specific
// binding has no glyph asset yet (e.g. rebound to a letter outside the
// current small drawn set, or a wheel direction -- there is no wheel glyph
// art at all yet). Callers must treat "" as "fall back to the button's
// known-working default glyph", not as an error.
std::string GlyphNameForBinding(const KbmBinding& binding);

// --- Gameplay-input override layer -----------------------------------------
//
// Everything above this point answers "what glyph do I draw". Everything
// below makes the actual gameplay input agree with the same answer, so there
// is exactly one source of truth (GetCurrentBinding/the ten keybind_* cvars)
// instead of a glyph mapping and a separate, driftable gameplay mapping.
//
// glyph_system.cpp's OnKeyEvent/OnMouseEvent (already the single call site
// every real key/mouse event from fable2_app.h passes through, for
// device-switch detection) also forward here to maintain held-state. Held
// state is consulted once per guest input poll, from OnInputPolled, via
// ApplyBindingsToGamepadState -- there is no separate hook into
// rex::input::mnk::MnkInputDriver, which stays completely untouched.

// Record a keyboard key's current down/up state. `key` is the raw
// rex::ui::VirtualKey value; out-of-range values are ignored.
void NotifyKeyState(rex::ui::VirtualKey key, bool down);

// Record a mouse button's current down/up state.
void NotifyMouseButtonState(rex::ui::MouseEvent::Button button, bool down);

// Record a wheel tick. Positive = wheel up/away, negative = wheel down --
// matches rex::ui::MouseEvent::scroll_y()'s convention. A single tick is
// latched "active" for a short pulse (see input_bindings.cpp) so a
// once-per-poll consumer sees it as a momentary button press rather than
// having to catch the exact poll that raced the wheel event.
void NotifyMouseWheel(int32_t scroll_y);

// True if ANY of the action's current comma-separated bindings is, right
// now, physically held/active (key down, mouse button down, or within an
// active wheel pulse) -- e.g. a cvar of "Q,I" makes the action active if
// either Q or I is held, not only the first. An action with no bound entries
// at all is always inactive.
bool IsActionActive(GameAction action);

// Applies every rebindable action's current held-state to a synthetic
// XInput gamepad's button bits and trigger bytes, called once per
// OnInputPolled while KBM is the active input device.
//
// This CLEARS then conditionally SETS each of the 12 action-owned button
// bits (A/B/X/Y/Start/Back/LeftShoulder/RightShoulder/DPadUp/Down/Left/
// Right) and always overwrites left_trigger/right_trigger -- a full
// replacement of exactly these bits, not an OR-merge -- so the bindings are
// the single source of truth for them regardless of whatever
// MnkInputDriver's own compiled-in default mapping already wrote into the
// same state. Every other bit (thumbsticks, LeftThumb/RightThumb/Guide) is
// left completely untouched: movement/camera stay on MnkInputDriver's own
// WASD/mouse-look mapping by design (see the file comment above).
//
// A/Start/Back/LeftShoulder/RightShoulder/DPad* still come straight from
// IsActionActive() against their own keybind_* cvar, unchanged (B is a
// partial exception -- see below). X/Y are unconditional and mutually
// independent as of the 3rd 2026-09-14 round: X is set iff LMB is
// currently held, Y is set iff RMB is currently held -- two separate
// if-statements, each reading only its own mouse button, with NO shared
// branch/switch/mode-read connecting them (an earlier version still chose
// between X and Y via an if/else on GetCurrentCombatMode(), which let mode
// drift make both buttons silently agree on the same bit -- see the file
// comment above). GetCurrentCombatMode() is still called (via an internal
// per-poll step that updates it from the three keybind_combat_mode_* cvars
// and the mouse wheel) but is consulted ONLY for the Magic/B dispatch
// below, never for X/Y. Never based on keybind_x/keybind_y anymore (those
// cvars still exist for backward compatibility but are not read here). B
// is set by IsActionActive(GameAction::kB) (default: Escape, for menu
// Back/Cancel -- see the file comment's 2026-09-15 note) OR'd with the
// Magic-mode LMB dispatch -- the one owned bit that can be set by two
// independent sources, deliberately, since Fable II itself already
// overloads this bit the same way; note that holding the Magic key and LMB
// together now sets X and B simultaneously (X is no longer suppressed in
// Magic mode) -- an accepted, out-of-scope edge case for this round. A
// throttled REXLOG_INFO ([InputBindings] [DIAG] [MouseCombatDispatch])
// logs LMB/RMB state and the resulting X/Y/B bits whenever that
// combination changes, so this independence can be confirmed from a log
// rather than assumed. Clearing X/Y/B together every poll regardless of
// mode is what guarantees switching Magic on/off while a mouse button is
// held cannot leave a stale bit set: whatever was set last poll is always
// cleared this poll before the current poll's bits are conditionally set.
void ApplyBindingsToGamepadState(uint16_t& buttons, uint8_t& left_trigger,
                                 uint8_t& right_trigger);

// One-time startup diagnostic: logs, for every GameAction, its cvar name,
// the raw cvar string as currently set, and what that resolves to (the
// parsed BindingKind plus the underlying key/mouse code and the glyph-style
// binding string). Intended to be called exactly once from
// glyph_system.cpp's Initialize(), so a build's log can be inspected to
// confirm the binding cvars are actually registered and resolving as
// expected, independent of runtime gameplay behavior.
void LogCurrentBindings();

// --- Fable III-style combat-mode system --------------------------------
//
// Which of the three weapon-tied buttons (X/melee, Y/ranged, B/magic) LMB
// and RMB currently drive. Runtime-only: never persisted, never itself a
// cvar (only the three *select* bindings below are cvars) -- it resets to
// kMelee on every launch, matching Fable II's usual starting stance. See the
// file comment above for why this exists instead of a fourth keybind_*
// cvar.
enum class CombatMode {
  kMelee,
  kRanged,
  kMagic,
};

// The rex::cvar name backing a mode-select binding (e.g.
// "keybind_combat_mode_melee"), same pattern as GameActionCvarName. Not a
// GameAction itself -- selecting a mode never presses a button, so it does
// not belong in that enum/ApplyBindingsToGamepadState's owned-bits set.
const char* CombatModeCvarName(CombatMode mode);

// Human-readable name ("Melee"/"Ranged"/"Magic"), for logging.
const char* CombatModeName(CombatMode mode);

// Current combat mode, as last set by a mode-select key or wheel tick
// (see ApplyBindingsToGamepadState's per-poll mode update). Never caches
// beyond that in-memory state.
CombatMode GetCurrentCombatMode();

// TEMPORARY DIAGNOSTIC (2026-09-14, architecture-verification round only):
// exposes the existing held-state check for a raw KbmBinding directly,
// independent of any GameAction/cvar lookup. Needed so glyph_system.cpp can
// ask "is this specific physical key/mouse button held right now?" for a
// fixed, hardcoded list of candidate default keys, to check whether
// RexGlue/MnkInputDriver's pre-override guest state reacts to any of them
// in a way our override does not own (D-pad, thumbstick clicks, Guide,
// thumbstick axes) -- BEFORE those keys are made the actual defaults. Purely
// read-only: does not affect IsActionActive, ApplyBindingsToGamepadState, or
// any binding cvar, and is not itself a second binding system (it takes an
// already-parsed KbmBinding, it does not define what physical input means
// what action). Remove once this investigation is done.
bool IsPhysicalBindingDown(const KbmBinding& binding);

// 2026-11-15 dedicated KBM keys (input_bindings.cpp):
//   keybind_magic  (default "Space") -> holds B (cast magic; aim with WASD).
//   keybind_sprint (default "Shift") -> holds A (sprint).
bool IsMagicKeyHeld();
bool IsSprintKeyHeld();
// Left stick from the SDK's keybind_lstick_* keys, IGNORING modifiers: the SDK
// itself requires an exact modifier match, so W/A/S/D go dead while Shift
// (sprint) is held. Returns false if no movement key is bound/parsable.
void GetKeyboardMoveStick(int16_t& lx, int16_t& ly);

}  // namespace fable2::glyph
