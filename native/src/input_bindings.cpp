#include "input_bindings.h"

#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <mutex>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/keybinds.h>

namespace fable2::glyph {

namespace {
std::string ToLowerCopy(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (char c : s) out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
  return out;
}

// --- Held-state tracking for the gameplay-input override layer ------------
//
// Real keyboard key events (the only ones NotifyKeyState ever sees -- there
// is no synthetic-pad path into it) always carry a raw VirtualKey <= 0xFE
// (see virtual_key.h; the kXInputPad* range starts at 0x5800 and is only
// used for XInputGetKeystroke-style pad emulation, never for a physical
// keyboard event), so a flat 256-entry table indexed by the raw value is
// sufficient and O(1); anything out of range is simply ignored.
constexpr size_t kKeyStateSlots = 256;
std::mutex g_input_state_mutex;
std::array<bool, kKeyStateSlots> g_key_down{};
// Index matches rex::ui::MouseEvent::Button's underlying values
// (kNone, kLeft, kRight, kMiddle, kX1, kX2).
std::array<bool, 6> g_mouse_button_down{};

// A wheel tick is a single discrete event, not a held state, but
// ApplyBindingsToGamepadState is only sampled once per guest input poll --
// so a tick is latched "active" for a short pulse to guarantee at least one
// poll observes it as pressed, then clears itself.
constexpr auto kWheelPulseDuration = std::chrono::milliseconds(150);
std::chrono::steady_clock::time_point g_wheel_up_until{};
std::chrono::steady_clock::time_point g_wheel_down_until{};

size_t MouseButtonIndex(rex::ui::MouseEvent::Button button) {
  return static_cast<size_t>(button);
}

// Splits a cvar value on ',' into trimmed, non-empty tokens. A plain
// single-token value with no comma at all (every binding before
// multi-binding support existed) yields exactly one entry equal to the
// whole (trimmed) input, so existing single-binding behavior is unchanged.
std::vector<std::string_view> SplitBindingTokens(std::string_view value) {
  std::vector<std::string_view> tokens;
  size_t start = 0;
  while (start <= value.size()) {
    size_t comma = value.find(',', start);
    size_t end = (comma == std::string_view::npos) ? value.size() : comma;
    std::string_view piece = value.substr(start, end - start);
    size_t first = piece.find_first_not_of(" \t");
    if (first != std::string_view::npos) {
      size_t last = piece.find_last_not_of(" \t");
      tokens.push_back(piece.substr(first, last - first + 1));
    }
    if (comma == std::string_view::npos) break;
    start = comma + 1;
  }
  return tokens;
}

bool IsBindingActive(const KbmBinding& binding) {
  switch (binding.kind) {
    case BindingKind::kKeyboardKey: {
      size_t idx = static_cast<size_t>(binding.key);
      if (idx >= kKeyStateSlots) return false;
      std::lock_guard<std::mutex> lock(g_input_state_mutex);
      return g_key_down[idx];
    }
    case BindingKind::kMouseButton: {
      size_t idx = MouseButtonIndex(binding.mouse_button);
      if (idx >= g_mouse_button_down.size()) return false;
      std::lock_guard<std::mutex> lock(g_input_state_mutex);
      return g_mouse_button_down[idx];
    }
    case BindingKind::kMouseWheelUp: {
      std::lock_guard<std::mutex> lock(g_input_state_mutex);
      return std::chrono::steady_clock::now() < g_wheel_up_until;
    }
    case BindingKind::kMouseWheelDown: {
      std::lock_guard<std::mutex> lock(g_input_state_mutex);
      return std::chrono::steady_clock::now() < g_wheel_down_until;
    }
    default:
      return false;
  }
}

// Thin convenience wrapper around the same g_mouse_button_down state
// IsBindingActive's kMouseButton case reads, for the combat-mode dispatch
// below -- which always means "physical LMB/RMB", not whatever a cvar
// happens to be bound to, so constructing a KbmBinding each call would just
// be ceremony.
// A cvar-backed binding list is held. Shift/Ctrl bindings also accept the
// left/right-specific virtual keys, whichever the window reports.
bool IsCvarBindingHeld(const char* cvar_name) {
  for (const KbmBinding& binding : ParseBindingList(rex::cvar::GetFlagByName(cvar_name))) {
    if (IsBindingActive(binding)) return true;
    if (binding.kind == BindingKind::kKeyboardKey) {
      KbmBinding alt = binding;
      if (binding.key == rex::ui::VirtualKey::kShift) {
        for (auto k : {rex::ui::VirtualKey::kLShift, rex::ui::VirtualKey::kRShift}) {
          alt.key = k;
          if (IsBindingActive(alt)) return true;
        }
      } else if (binding.key == rex::ui::VirtualKey::kControl) {
        for (auto k : {rex::ui::VirtualKey::kLControl, rex::ui::VirtualKey::kRControl}) {
          alt.key = k;
          if (IsBindingActive(alt)) return true;
        }
      }
    }
  }
  return false;
}

// Bare-key part of an SDK bind list ("Shift+W,Up" -> W and Up), modifiers ignored.
bool IsSdkBindHeldIgnoringMods(const char* cvar_name) {
  const std::string value = rex::cvar::GetFlagByName(cvar_name);
  size_t start = 0;
  while (start <= value.size()) {
    size_t comma = value.find(',', start);
    std::string tok = value.substr(start, comma == std::string::npos ? std::string::npos
                                                                     : comma - start);
    size_t plus = tok.rfind('+');
    if (plus != std::string::npos && plus + 1 < tok.size()) tok = tok.substr(plus + 1);
    for (const KbmBinding& b : ParseBindingList(tok)) {
      if (b.kind == BindingKind::kKeyboardKey && IsBindingActive(b)) return true;
    }
    if (comma == std::string::npos) break;
    start = comma + 1;
  }
  return false;
}

bool IsMouseButtonHeld(rex::ui::MouseEvent::Button button) {
  size_t idx = MouseButtonIndex(button);
  if (idx >= g_mouse_button_down.size()) return false;
  std::lock_guard<std::mutex> lock(g_input_state_mutex);
  return g_mouse_button_down[idx];
}

// --- Fable III-style combat-mode state (2026-09-14) ------------------------
//
// Runtime-only, never persisted: resets to kMelee on every launch. Trivially
// copyable scoped enum in a std::atomic is fine (no locking needed, same as
// the existing wheel-pulse timestamps use their own mutex rather than
// atomics purely because they're not trivially atomic-friendly
// steady_clock::time_point writes needing to pair together; a single enum
// value has no such pairing concern).
std::atomic<CombatMode> g_combat_mode{CombatMode::kMelee};

// Monotonic tick counters, separate from the existing wheel-pulse latch
// (g_wheel_up_until/g_wheel_down_until) that GameAction wheel bindings use.
// The pulse latch is level-triggered and deliberately stays "active" for
// ~150ms so a once-per-poll consumer can't miss a tick -- exactly wrong for
// mode-cycling, which must advance exactly once per physical notch even
// though many polls happen inside that 150ms window. These counters let the
// per-poll combat-mode update detect the actual edge (the count changing)
// instead of the level.
std::atomic<uint32_t> g_wheel_up_ticks{0};
std::atomic<uint32_t> g_wheel_down_ticks{0};

bool IsCombatModeKeyHeld(CombatMode mode) {
  const char* cvar_name = CombatModeCvarName(mode);
  if (!cvar_name || !*cvar_name) return false;
  for (const KbmBinding& binding : ParseBindingList(rex::cvar::GetFlagByName(cvar_name))) {
    if (IsBindingActive(binding)) return true;
  }
  return false;
}

// Per-poll combat-mode update: explicit 1/2/3-style keys are level-triggered
// (holding one keeps that mode selected; releasing it leaves the mode as-is
// rather than reverting, so a single tap latches the mode -- same idea as
// Fable III's own number keys). If more than one mode-select key is somehow
// held at once, melee wins, then ranged, then magic -- an arbitrary but
// stable tie-break, not expected to matter in practice.
//
// The mouse wheel cycles mode, edge-triggered off the tick counters above so
// one notch is exactly one step regardless of poll rate.
void UpdateCombatModeFromInput() {
  if (IsCombatModeKeyHeld(CombatMode::kMelee)) {
    g_combat_mode.store(CombatMode::kMelee, std::memory_order_relaxed);
  } else if (IsCombatModeKeyHeld(CombatMode::kRanged)) {
    g_combat_mode.store(CombatMode::kRanged, std::memory_order_relaxed);
  } else if (IsCombatModeKeyHeld(CombatMode::kMagic)) {
    g_combat_mode.store(CombatMode::kMagic, std::memory_order_relaxed);
  }

  static uint32_t s_last_wheel_up_ticks = 0;
  static uint32_t s_last_wheel_down_ticks = 0;
  uint32_t up_ticks = g_wheel_up_ticks.load(std::memory_order_relaxed);
  uint32_t down_ticks = g_wheel_down_ticks.load(std::memory_order_relaxed);
  int32_t delta = static_cast<int32_t>(up_ticks - s_last_wheel_up_ticks) -
                   static_cast<int32_t>(down_ticks - s_last_wheel_down_ticks);
  s_last_wheel_up_ticks = up_ticks;
  s_last_wheel_down_ticks = down_ticks;

  if (delta != 0) {
    constexpr CombatMode kModeOrder[] = {CombatMode::kMelee, CombatMode::kRanged, CombatMode::kMagic};
    constexpr int kModeCount = 3;
    CombatMode current = g_combat_mode.load(std::memory_order_relaxed);
    int current_index = 0;
    for (int i = 0; i < kModeCount; ++i) {
      if (kModeOrder[i] == current) current_index = i;
    }
    int new_index = ((current_index + delta) % kModeCount + kModeCount) % kModeCount;
    g_combat_mode.store(kModeOrder[new_index], std::memory_order_relaxed);
  }
}
}  // namespace

// Default layout (2026-09-15, Fable III native-scheme round): refines the
// combat-mode round below it -- same architecture, three corrections:
// (1) keybind_a's primary/glyph entry is now E (was Space) since Fable
//     III's own list leads with "E interact/select"; (2) Melee/Ranged/Magic
//     mode-select keys reordered to 1/2/3 = Melee/Magic/Ranged, matching the
//     newly-given target list exactly (previously 1/2/3 = Melee/Ranged/
//     Magic); (3) B is no longer legacy -- it's repurposed as the menu
//     Back/Cancel button (Esc), OR'd with the Magic-mode combat dispatch
//     rather than replaced by it (see the file header comment's 2026-09-15
//     note and ApplyBindingsToGamepadState below). Start is repurposed from
//     Escape to P (pause), freeing Escape for B/Back-Cancel. D-pad
//     Up/Down/Left/Right added as ordinary GameActions defaulting to
//     F1-F4 ("Context-Sensitive Shortcuts" per Fable II's own manual --
//     see input_bindings.h's file comment). Full current layout:
//   WASD = movement, mouse = camera (unrelated to this file, unchanged).
//   E, Space, Shift = A (multi-bound -- Fable II's own context-sensitive A
//     button already decides interact vs. roll vs. sprint; E is primary/
//     glyph since it's listed first).
//   P = Start/Pause. Esc = B (menu Back/Cancel; also Magic-mode attack,
//     see below). Q = LT ("contextual LT-like action"). R = RB. Tab = Back.
//   LMB = X (Melee), RMB = Y (Ranged) -- direct, unconditional, mutually
//     independent (2026-09-14, 3rd round -- see ApplyBindingsToGamepadState
//     for why an earlier "simplified" version still wasn't independent).
//   2 = hold for Magic mode: while held, LMB ALSO drives B (Magic),
//     alongside X (not instead of it -- untouched out-of-scope edge case,
//     see ApplyBindingsToGamepadState); RMB has no mapped action in Magic
//     mode. 1/3 (previously Melee/Ranged mode-select) are no longer needed
//     for ordinary play -- melee/ranged are already always on via LMB/RMB --
//     but keybind_combat_mode_melee/_ranged are left defined/rebindable
//     rather than removed.
//   F1/F2/F3/F4 = D-pad Up/Down/Left/Right ("context action" hotkeys).
//  - keybind_right_trigger, keybind_left_shoulder: still "" (unbound).
//    Deliberately left this way per explicit instruction -- their exact
//    Fable II functions (and Ctrl's likely target, LB) are to be confirmed
//    separately before assigning real keys, rather than guessed now. See
//    AI_HANDOFF.md.
//  - keybind_x, keybind_y: still LEGACY -- ApplyBindingsToGamepadState never
//    reads them; X/Y activation is entirely combat-mode-driven. Kept
//    defined (not deleted) only so nothing referencing
//    GameActionCvarName(kX/kY) breaks; their string values are meaningless.
//  - M (map) and F5 (quicksave): NOT bound to anything. Fable II has no
//    button that shows a map directly (it's a tab inside the Start/pause
//    menu) and no manual/quicksave control was found (autosave-only) -- see
//    AI_HANDOFF.md rather than forcing either onto an unrelated key.
REXCVAR_DEFINE_STRING(keybind_a, "E,Space,Shift", "Keybinds",
    "PC keys/mouse for the Xbox 360 A button (context action: interact, "
    "roll, or sprint, chosen by Fable II itself based on context -- any of "
    "these three keys presses A; the first one listed is what the A glyph "
    "shows, since we cannot tell which context a given A prompt is for).");
REXCVAR_DEFINE_STRING(keybind_b, "Escape", "Keybinds",
    "PC key/mouse for the Xbox 360 B button: menu Back/Cancel (Fable II's "
    "own universal Xbox convention). Also OR'd with the Magic-mode combat "
    "dispatch below (LMB) -- same bit, different context, exactly like A's "
    "interact/roll/sprint overload. Not legacy: read directly by "
    "ApplyBindingsToGamepadState, unlike keybind_x/keybind_y below.");
REXCVAR_DEFINE_STRING(keybind_x, "LMB", "Keybinds",
    "LEGACY -- no longer read for activation. X is now driven by the "
    "Fable III-style combat-mode system (LMB/RMB in Melee mode). Kept "
    "defined for backward compatibility only.");
REXCVAR_DEFINE_STRING(keybind_y, "RMB", "Keybinds",
    "LEGACY -- no longer read for activation. Y is now driven by the "
    "Fable III-style combat-mode system (LMB/RMB in Ranged mode). Kept "
    "defined for backward compatibility only.");
REXCVAR_DEFINE_STRING(keybind_left_trigger, "Q", "Keybinds",
    "PC key/mouse control for the Xbox 360 left trigger (contextual hold/aim).");
REXCVAR_DEFINE_STRING(keybind_right_trigger, "", "Keybinds",
    "PC key/mouse control for the Xbox 360 right trigger (interact/select). "
    "Unbound by default: its previous default (E) now belongs to the A "
    "button. Deliberately left unbound pending confirmation of RT's exact "
    "Fable II function -- open gap, see AI_HANDOFF.md.");
REXCVAR_DEFINE_STRING(keybind_left_shoulder, "", "Keybinds",
    "PC key/mouse control for the Xbox 360 left shoulder (LB). Unbound by "
    "default: its previous default (1) now means \"select Melee mode\". "
    "Deliberately left unbound pending confirmation of LB's exact Fable II "
    "function (possibly first-person/camera-reset) -- open gap, see "
    "AI_HANDOFF.md.");
REXCVAR_DEFINE_STRING(keybind_right_shoulder, "R", "Keybinds",
    "PC key/mouse control for the Xbox 360 right shoulder (RB).");
REXCVAR_DEFINE_STRING(keybind_start, "P", "Keybinds",
    "PC key/mouse control for the Xbox 360 Start button (pause menu). "
    "CHANGED from \"Escape\" to \"P\": Escape now belongs to B (menu "
    "Back/Cancel).");
REXCVAR_DEFINE_STRING(keybind_back, "Tab", "Keybinds",
    "PC key/mouse control for the Xbox 360 Back button (events/info).");
REXCVAR_DEFINE_STRING(keybind_dpad_up, "F1", "Keybinds",
    "PC key/mouse control for the Xbox 360 D-pad Up (context-sensitive shortcut).");
REXCVAR_DEFINE_STRING(keybind_dpad_down, "F2", "Keybinds",
    "PC key/mouse control for the Xbox 360 D-pad Down (context-sensitive shortcut).");
REXCVAR_DEFINE_STRING(keybind_dpad_left, "F3", "Keybinds",
    "PC key/mouse control for the Xbox 360 D-pad Left (context-sensitive shortcut).");
REXCVAR_DEFINE_STRING(keybind_dpad_right, "F4", "Keybinds",
    "PC key/mouse control for the Xbox 360 D-pad Right (context-sensitive shortcut).");

// Combat-mode select cvars (2026-09-14, reordered 2026-09-15, LMB/RMB
// simplified 2026-09-14 later round -- see ApplyBindingsToGamepadState):
// melee (X) and ranged (Y) no longer need a mode-select key at all -- LMB
// and RMB drive them directly, always. keybind_combat_mode_melee/_ranged
// are consequently vestigial for default play (kept defined/rebindable
// rather than removed, in case a future scheme wants them again). Magic
// still needs an explicit hold, since there's no third mouse button: while
// keybind_combat_mode_magic is held, LMB drives B (magic attack) instead
// of X/Y. Deliberately NOT GameAction cvars -- selecting a mode never
// presses a button itself, so it doesn't belong in
// ApplyBindingsToGamepadState's owned-bits set. Parsed the same way
// (ParseBindingList), so multi-binding/rebinding works identically to every
// other cvar in this file.
REXCVAR_DEFINE_STRING(keybind_magic, "Space", "Keybinds",
    "PC key for magic: drives the Xbox B button (tap = quick cast, hold = charge; "
    "aim with WASD like the left stick).");
REXCVAR_DEFINE_STRING(keybind_sprint, "Shift", "Keybinds",
    "PC key for sprint: holds the Xbox A button. WASD keeps working while held.");

REXCVAR_DEFINE_STRING(keybind_combat_mode_melee, "1", "Keybinds",
    "Vestigial -- Melee (X) is already always driven directly by LMB, no "
    "mode-select needed. Kept defined/rebindable only.");
REXCVAR_DEFINE_STRING(keybind_combat_mode_magic, "2", "Keybinds",
    "PC key to hold for Magic mode: while held, LMB drives B (magic attack) "
    "instead of melee/ranged; RMB has no mapped action in Magic mode.");
REXCVAR_DEFINE_STRING(keybind_combat_mode_ranged, "3", "Keybinds",
    "Vestigial -- Ranged (Y) is already always driven directly by RMB, no "
    "mode-select needed. Kept defined/rebindable only.");

const char* GameActionCvarName(GameAction action) {
  switch (action) {
    case GameAction::kA: return "keybind_a";
    case GameAction::kB: return "keybind_b";
    case GameAction::kX: return "keybind_x";
    case GameAction::kY: return "keybind_y";
    case GameAction::kLeftTrigger: return "keybind_left_trigger";
    case GameAction::kRightTrigger: return "keybind_right_trigger";
    case GameAction::kLeftShoulder: return "keybind_left_shoulder";
    case GameAction::kRightShoulder: return "keybind_right_shoulder";
    case GameAction::kStart: return "keybind_start";
    case GameAction::kBack: return "keybind_back";
    case GameAction::kDPadUp: return "keybind_dpad_up";
    case GameAction::kDPadDown: return "keybind_dpad_down";
    case GameAction::kDPadLeft: return "keybind_dpad_left";
    case GameAction::kDPadRight: return "keybind_dpad_right";
  }
  return "";
}

const char* CombatModeCvarName(CombatMode mode) {
  switch (mode) {
    case CombatMode::kMelee: return "keybind_combat_mode_melee";
    case CombatMode::kRanged: return "keybind_combat_mode_ranged";
    case CombatMode::kMagic: return "keybind_combat_mode_magic";
  }
  return "";
}

const char* CombatModeName(CombatMode mode) {
  switch (mode) {
    case CombatMode::kMelee: return "Melee";
    case CombatMode::kRanged: return "Ranged";
    case CombatMode::kMagic: return "Magic";
  }
  return "?";
}

CombatMode GetCurrentCombatMode() {
  return g_combat_mode.load(std::memory_order_relaxed);
}

const char* GameActionDescription(GameAction action) {
  switch (action) {
    case GameAction::kA: return "A button (context action: interact / roll / sprint)";
    case GameAction::kB: return "B button (menu Back/Cancel, OR'd with Magic-mode attack)";
    case GameAction::kX: return "X button (Melee-mode attack/block, combat-mode-driven)";
    case GameAction::kY: return "Y button (Ranged-mode attack/aim, combat-mode-driven)";
    case GameAction::kLeftTrigger: return "Left trigger (contextual hold / aim)";
    case GameAction::kRightTrigger: return "Right trigger (function unconfirmed -- unbound)";
    case GameAction::kLeftShoulder: return "Left shoulder (function unconfirmed -- unbound)";
    case GameAction::kRightShoulder: return "Right shoulder (RB)";
    case GameAction::kStart: return "Start button (pause menu)";
    case GameAction::kBack: return "Back button (events / info)";
    case GameAction::kDPadUp: return "D-pad Up (context-sensitive shortcut)";
    case GameAction::kDPadDown: return "D-pad Down (context-sensitive shortcut)";
    case GameAction::kDPadLeft: return "D-pad Left (context-sensitive shortcut)";
    case GameAction::kDPadRight: return "D-pad Right (context-sensitive shortcut)";
  }
  return "";
}

KbmBinding ParseBindingString(std::string_view value) {
  KbmBinding binding;
  if (value.empty()) return binding;

  std::string lower = ToLowerCopy(value);
  if (lower == "lmb" || lower == "mouse1" || lower == "mouseleft") {
    binding.kind = BindingKind::kMouseButton;
    binding.mouse_button = rex::ui::MouseEvent::Button::kLeft;
    return binding;
  }
  if (lower == "rmb" || lower == "mouse2" || lower == "mouseright") {
    binding.kind = BindingKind::kMouseButton;
    binding.mouse_button = rex::ui::MouseEvent::Button::kRight;
    return binding;
  }
  if (lower == "mmb" || lower == "mouse3" || lower == "mousemiddle") {
    binding.kind = BindingKind::kMouseButton;
    binding.mouse_button = rex::ui::MouseEvent::Button::kMiddle;
    return binding;
  }
  if (lower == "mb4" || lower == "mousex1") {
    binding.kind = BindingKind::kMouseButton;
    binding.mouse_button = rex::ui::MouseEvent::Button::kX1;
    return binding;
  }
  if (lower == "mb5" || lower == "mousex2") {
    binding.kind = BindingKind::kMouseButton;
    binding.mouse_button = rex::ui::MouseEvent::Button::kX2;
    return binding;
  }
  if (lower == "wheelup" || lower == "mousewheelup" || lower == "scrollup") {
    binding.kind = BindingKind::kMouseWheelUp;
    return binding;
  }
  if (lower == "wheeldown" || lower == "mousewheeldown" || lower == "scrolldown") {
    binding.kind = BindingKind::kMouseWheelDown;
    return binding;
  }

  // Everything else: the SDK's own exact keyboard-name parser (handles
  // "Space", "Shift", "Tab", "Escape", "E", "1", "P", ...), not a substring
  // guess. VirtualKey::kNone means genuinely unrecognized.
  rex::ui::VirtualKey vk = rex::ui::ParseVirtualKey(value);
  if (vk != rex::ui::VirtualKey::kNone) {
    binding.kind = BindingKind::kKeyboardKey;
    binding.key = vk;
  }
  return binding;
}

std::string BindingToString(const KbmBinding& binding) {
  switch (binding.kind) {
    case BindingKind::kKeyboardKey:
      return rex::ui::VirtualKeyToString(binding.key);
    case BindingKind::kMouseButton:
      switch (binding.mouse_button) {
        case rex::ui::MouseEvent::Button::kLeft: return "LMB";
        case rex::ui::MouseEvent::Button::kRight: return "RMB";
        case rex::ui::MouseEvent::Button::kMiddle: return "MMB";
        case rex::ui::MouseEvent::Button::kX1: return "MB4";
        case rex::ui::MouseEvent::Button::kX2: return "MB5";
        default: return "";
      }
    case BindingKind::kMouseWheelUp: return "WheelUp";
    case BindingKind::kMouseWheelDown: return "WheelDown";
    default: return "";
  }
}

// Splits `value` on ',' (see SplitBindingTokens) and parses each token
// individually with ParseBindingString. A plain single-token value (no
// comma) parses identically to how ParseBindingString alone used to be
// called directly, so every pre-existing single-binding cvar is unaffected.
// A token that fails to parse still occupies its position in the returned
// list as an kUnbound KbmBinding, rather than being silently dropped.
std::vector<KbmBinding> ParseBindingList(std::string_view value) {
  std::vector<KbmBinding> bindings;
  for (std::string_view token : SplitBindingTokens(value)) {
    bindings.push_back(ParseBindingString(token));
  }
  return bindings;
}

std::vector<KbmBinding> GetCurrentBindings(GameAction action) {
  const char* cvar_name = GameActionCvarName(action);
  if (!cvar_name || !*cvar_name) return {};
  return ParseBindingList(rex::cvar::GetFlagByName(cvar_name));
}

KbmBinding GetCurrentBinding(GameAction action) {
  for (const KbmBinding& binding : GetCurrentBindings(action)) {
    if (binding.is_bound()) return binding;
  }
  return {};
}

// Glyph asset coverage (2026-09-13): pc/assets/glyphs/ currently only has
// art for this specific set of keys/mouse controls (see AI_HANDOFF.md for
// the full inventory). Rebinding an action to a key outside this set is a
// content gap, not a logic bug -- GlyphNameForBinding() returns "" and every
// call site falls back to that button's known-working default glyph rather
// than showing nothing.
std::string GlyphNameForBinding(const KbmBinding& binding) {
  using VK = rex::ui::VirtualKey;
  switch (binding.kind) {
    case BindingKind::kMouseButton:
      switch (binding.mouse_button) {
        case rex::ui::MouseEvent::Button::kLeft: return "lmb";
        case rex::ui::MouseEvent::Button::kRight: return "rmb";
        case rex::ui::MouseEvent::Button::kMiddle: return "mmb";
        case rex::ui::MouseEvent::Button::kX1: return "mouse4";  // 2026-11-15 art added
        case rex::ui::MouseEvent::Button::kX2: return "mouse5";
        default: return "";
      }
    case BindingKind::kMouseWheelUp:
      return "wheelup";  // 2026-11-15 art added
    case BindingKind::kMouseWheelDown:
      return "wheeldown";
    case BindingKind::kKeyboardKey:
      switch (binding.key) {
        case VK::kSpace: return "space";
        case VK::kShift:
        case VK::kLShift:
        case VK::kRShift: return "shift";
        case VK::kTab: return "tab";
        case VK::kEscape: return "esc";
        case VK::kReturn: return "enter";
        case VK::k1: return "1";
        case VK::k2: return "2";
        case VK::k3: return "3";
        case VK::k4: return "4";
        case VK::kC: return "c";
        case VK::kE: return "e";
        case VK::kF: return "f";
        case VK::kL: return "l";
        case VK::kP: return "p";
        case VK::kQ: return "q";
        case VK::kR: return "r";
        case VK::kX: return "x";
        case VK::kZ: return "z";
        // 2026-11-15 glyph art v2: additive entries for the new glyph assets
        // (tools/glyph_generator/make_glyphs_v2.py). Existing entries unchanged.
        case VK::kA: return "a";
        case VK::kB: return "b";
        case VK::kD: return "d";
        case VK::kG: return "g";
        case VK::kH: return "h";
        case VK::kI: return "i";
        case VK::kJ: return "j";
        case VK::kK: return "k";
        case VK::kM: return "m";
        case VK::kN: return "n";
        case VK::kO: return "o";
        case VK::kS: return "s";
        case VK::kT: return "t";
        case VK::kU: return "u";
        case VK::kV: return "v";
        case VK::kW: return "w";
        case VK::kY: return "y";
        case VK::k0: return "0";
        case VK::k5: return "5";
        case VK::k6: return "6";
        case VK::k7: return "7";
        case VK::k8: return "8";
        case VK::k9: return "9";
        case VK::kF1: return "f1";
        case VK::kF2: return "f2";
        case VK::kF3: return "f3";
        case VK::kF4: return "f4";
        case VK::kF5: return "f5";
        case VK::kF6: return "f6";
        case VK::kF7: return "f7";
        case VK::kF8: return "f8";
        case VK::kF9: return "f9";
        case VK::kF10: return "f10";
        case VK::kF11: return "f11";
        case VK::kF12: return "f12";
        case VK::kControl:
        case VK::kLControl:
        case VK::kRControl: return "ctrl";
        case VK::kMenu:
        case VK::kLMenu: return "alt";
        case VK::kUp: return "arrow_up";
        case VK::kDown: return "arrow_down";
        case VK::kLeft: return "arrow_left";
        case VK::kRight: return "arrow_right";
        default: return "";  // key has no drawn glyph asset yet
      }
    default:
      return "";
  }
}

void NotifyKeyState(rex::ui::VirtualKey key, bool down) {
  size_t idx = static_cast<size_t>(key);
  if (idx >= kKeyStateSlots) return;
  std::lock_guard<std::mutex> lock(g_input_state_mutex);
  g_key_down[idx] = down;
}

void NotifyMouseButtonState(rex::ui::MouseEvent::Button button, bool down) {
  size_t idx = MouseButtonIndex(button);
  if (idx >= g_mouse_button_down.size()) return;
  std::lock_guard<std::mutex> lock(g_input_state_mutex);
  g_mouse_button_down[idx] = down;
}

void NotifyMouseWheel(int32_t scroll_y) {
  if (scroll_y == 0) return;
  {
    std::lock_guard<std::mutex> lock(g_input_state_mutex);
    auto until = std::chrono::steady_clock::now() + kWheelPulseDuration;
    if (scroll_y > 0) {
      g_wheel_up_until = until;
    } else {
      g_wheel_down_until = until;
    }
  }
  // Separate from the pulse latch above (see g_wheel_up_ticks/
  // g_wheel_down_ticks's comment): the combat-mode cycle needs to know a
  // tick HAPPENED, once, not just that we're still within its ~150ms
  // window -- these counters are what UpdateCombatModeFromInput() diffs
  // against to cycle exactly once per physical notch.
  if (scroll_y > 0) {
    g_wheel_up_ticks.fetch_add(1, std::memory_order_relaxed);
  } else {
    g_wheel_down_ticks.fetch_add(1, std::memory_order_relaxed);
  }
}

bool IsMagicKeyHeld() { return IsCvarBindingHeld("keybind_magic"); }
bool IsSprintKeyHeld() { return IsCvarBindingHeld("keybind_sprint"); }

void GetKeyboardMoveStick(int16_t& lx, int16_t& ly) {
  int x = 0, y = 0;
  if (IsSdkBindHeldIgnoringMods("keybind_lstick_left")) x -= 32767;
  if (IsSdkBindHeldIgnoringMods("keybind_lstick_right")) x += 32767;
  if (IsSdkBindHeldIgnoringMods("keybind_lstick_up")) y += 32767;
  if (IsSdkBindHeldIgnoringMods("keybind_lstick_down")) y -= 32767;
  lx = static_cast<int16_t>(x);
  ly = static_cast<int16_t>(y);
}

bool IsActionActive(GameAction action) {
  for (const KbmBinding& binding : GetCurrentBindings(action)) {
    if (IsBindingActive(binding)) return true;
  }
  return false;
}

void ApplyBindingsToGamepadState(uint16_t& buttons, uint8_t& left_trigger,
                                 uint8_t& right_trigger) {
  // Update the combat-mode state (1/2/3 + wheel) before using it below, so
  // a mode change this same poll takes effect immediately rather than one
  // poll late.
  UpdateCombatModeFromInput();

  // Full replacement of exactly these 12 button bits -- clear then
  // conditionally set -- so the bindings are the single source of truth
  // for them regardless of whatever MnkInputDriver's own compiled-in
  // default mapping already wrote into `buttons`. Every other bit
  // (thumbstick clicks, Guide, both thumbstick axes) is left completely
  // untouched.
  constexpr uint16_t kBitDpadUp = 0x0001;
  constexpr uint16_t kBitDpadDown = 0x0002;
  constexpr uint16_t kBitDpadLeft = 0x0004;
  constexpr uint16_t kBitDpadRight = 0x0008;
  constexpr uint16_t kBitStart = 0x0010;
  constexpr uint16_t kBitBack = 0x0020;
  constexpr uint16_t kBitLeftShoulder = 0x0100;
  constexpr uint16_t kBitRightShoulder = 0x0200;
  constexpr uint16_t kBitA = 0x1000;
  constexpr uint16_t kBitB = 0x2000;
  constexpr uint16_t kBitX = 0x4000;
  constexpr uint16_t kBitY = 0x8000;
  constexpr uint16_t kOwnedBits = kBitA | kBitB | kBitX | kBitY | kBitStart |
                                   kBitBack | kBitLeftShoulder | kBitRightShoulder |
                                   kBitDpadUp | kBitDpadDown | kBitDpadLeft | kBitDpadRight;

  // Clearing all 12 owned bits together, every poll, regardless of combat
  // mode, is what guarantees a mode switch while LMB/RMB is held can't
  // leave the previous mode's button (X, Y, or B) stuck: whichever of the
  // three was set last poll is unconditionally cleared here before at most
  // one of them is conditionally set back below.
  buttons &= static_cast<uint16_t>(~kOwnedBits);
  if (IsActionActive(GameAction::kA) || IsSprintKeyHeld()) buttons |= kBitA;
  if (IsActionActive(GameAction::kStart)) buttons |= kBitStart;
  if (IsActionActive(GameAction::kBack)) buttons |= kBitBack;
  if (IsActionActive(GameAction::kLeftShoulder)) buttons |= kBitLeftShoulder;
  if (IsActionActive(GameAction::kRightShoulder)) buttons |= kBitRightShoulder;
  if (IsActionActive(GameAction::kDPadUp)) buttons |= kBitDpadUp;
  if (IsActionActive(GameAction::kDPadDown)) buttons |= kBitDpadDown;
  if (IsActionActive(GameAction::kDPadLeft)) buttons |= kBitDpadLeft;
  if (IsActionActive(GameAction::kDPadRight)) buttons |= kBitDpadRight;

  // X/Y: UNCONDITIONAL, independent LMB/RMB dispatch (2026-09-14, 3rd
  // round -- see file header comment). This replaces a previous version
  // that still branched on GetCurrentCombatMode() (an if/else choosing
  // between "X or Y" before checking either mouse button) -- that shared
  // read was itself the bug class a user then reproduced: ANY code path
  // that decides X vs. Y by consulting one piece of shared state makes
  // LMB's outcome depend on something other than LMB (mode drift from a
  // stray wheel tick, a leftover "1"/"3" press from earlier testing, or
  // testing against a not-yet-rebuilt binary), which is how "both buttons
  // do melee" and, after the mode drifted, "both buttons do ranged" were
  // both observed even though the two REXCVAR-level mouse reads were
  // always correct. There is now no branch, switch, or shared variable
  // between the two: LMB is the X bit's ONLY source, RMB is the Y bit's
  // ONLY source, full stop -- each is a single independent if-statement
  // reading only its own mouse-button state. No mode/cvar/1-2-3 key is
  // consulted for either. keybind_combat_mode_melee/_ranged (default
  // "1"/"3") are consequently vestigial for melee/ranged specifically (see
  // their cvar comments) -- left defined/rebindable, not removed.
  //
  // Magic is explicitly out of scope for this round and is left exactly as
  // before: holding the Magic mode-select key (default "2") still ORs LMB
  // into B (magic attack) alongside keybind_b, via GetCurrentCombatMode()
  // -- but that read is now completely separate from, and can no longer
  // suppress or redirect, the X/Y dispatch above (previously, being in
  // Magic mode skipped X/Y entirely; now X/Y fire the same either way, and
  // holding "2"+LMB will set X and B together -- an accepted, deliberately
  // untouched edge case since fixing magic's interaction is not this
  // round's task).
  bool primary_held = IsMouseButtonHeld(rex::ui::MouseEvent::Button::kLeft);
  bool secondary_held = IsMouseButtonHeld(rex::ui::MouseEvent::Button::kRight);
  if (primary_held) buttons |= kBitX;
  if (secondary_held) buttons |= kBitY;
  bool magic_lmb_active = (GetCurrentCombatMode() == CombatMode::kMagic) && primary_held;
  if (IsActionActive(GameAction::kB) || magic_lmb_active || IsMagicKeyHeld()) buttons |= kBitB;

  // TEMPORARY DIAGNOSTIC (2026-09-14, 3rd round -- remove once the user has
  // confirmed independent LMB/RMB dispatch in-game): logs the exact
  // primary_held/secondary_held inputs and the resulting X/Y/B bits
  // together, but only when that combination actually changes, so a test
  // session produces one line per press/release rather than one per poll.
  // This directly answers "do LMB and RMB reach separate guest bits and
  // never overwrite each other" from the log, rather than by inspection
  // alone.
  {
    static bool s_diag_last_primary = false, s_diag_last_secondary = false;
    static bool s_diag_last_x = false, s_diag_last_y = false, s_diag_last_b = false;
    bool now_x = (buttons & kBitX) != 0;
    bool now_y = (buttons & kBitY) != 0;
    bool now_b = (buttons & kBitB) != 0;
    if (primary_held != s_diag_last_primary || secondary_held != s_diag_last_secondary ||
        now_x != s_diag_last_x || now_y != s_diag_last_y || now_b != s_diag_last_b) {
      REXLOG_INFO(
          "[InputBindings] [DIAG] [MouseCombatDispatch] LMB={} RMB={} -> X={} Y={} B={} "
          "(mode={})",
          primary_held, secondary_held, now_x, now_y, now_b, CombatModeName(GetCurrentCombatMode()));
      s_diag_last_primary = primary_held;
      s_diag_last_secondary = secondary_held;
      s_diag_last_x = now_x;
      s_diag_last_y = now_y;
      s_diag_last_b = now_b;
    }
  }

  left_trigger = IsActionActive(GameAction::kLeftTrigger) ? 255 : 0;
  right_trigger = IsActionActive(GameAction::kRightTrigger) ? 255 : 0;
}

namespace {
const char* BindingKindName(BindingKind kind) {
  switch (kind) {
    case BindingKind::kUnbound: return "Unbound";
    case BindingKind::kKeyboardKey: return "KeyboardKey";
    case BindingKind::kMouseButton: return "MouseButton";
    case BindingKind::kMouseWheelUp: return "MouseWheelUp";
    case BindingKind::kMouseWheelDown: return "MouseWheelDown";
  }
  return "?";
}
}  // namespace

// One-time startup diagnostic (see input_bindings.h). Deliberately does not
// touch IsActionActive/held-state at all -- this only proves out the
// cvar-read -> split -> parse -> resolved-code path, independent of any
// physical input having happened yet, so it can be checked from a log
// immediately after launch, before touching a key or button. Logs every
// comma-separated entry for an action, not just the first, so a
// multi-binding cvar's per-token resolution is fully visible (this is what
// exposed the original comma-list-parsed-as-one-token bug).
void LogCurrentBindings() {
  // BUILD PROBE (2026-09-14, deployment-verification round): unconditional,
  // unique, one-time marker in the SAME translation unit as
  // ApplyBindingsToGamepadState's LMB/RMB dispatch and the
  // [MouseCombatDispatch] diagnostic below, added specifically so a build's
  // identity can be confirmed from its own startup log before trusting any
  // gameplay test against it -- three prior rounds of this exact fix were
  // deployed via device_commit_files (source only) and separately verified
  // byte-identical on disk, but the actually-launched Fable II.exe kept
  // testing stale regardless, because native/package.ps1 (the step that
  // copies native/out/build/win-amd64-release/fable2.exe over the
  // project-root Fable II.exe) was never run after the rebuild. This string
  // has never appeared in any binary before this round (confirmed via
  // `strings` against every existing .exe in the project) -- its presence
  // in a build's own startup log is what proves that build actually
  // contains this file's current source, independent of file timestamps,
  // hashes, or which .exe happens to be sitting where.
  REXLOG_INFO("[BUILD-PROBE] MouseCombatDirectXY-v1");
  constexpr GameAction kAllActions[] = {
      GameAction::kA,           GameAction::kB,
      GameAction::kX,           GameAction::kY,
      GameAction::kLeftTrigger, GameAction::kRightTrigger,
      GameAction::kLeftShoulder, GameAction::kRightShoulder,
      GameAction::kStart,       GameAction::kBack,
      GameAction::kDPadUp,      GameAction::kDPadDown,
      GameAction::kDPadLeft,    GameAction::kDPadRight,
  };
  REXLOG_INFO("[InputBindings] [DIAG] ---- Startup binding resolution dump ({} actions) ----",
              static_cast<int>(sizeof(kAllActions) / sizeof(kAllActions[0])));
  for (GameAction action : kAllActions) {
    const char* cvar_name = GameActionCvarName(action);
    std::string raw_value = cvar_name && *cvar_name ? rex::cvar::GetFlagByName(cvar_name) : "";
    std::vector<KbmBinding> bindings = ParseBindingList(raw_value);

    // The primary binding (what GetCurrentBinding()/the glyph system uses)
    // is the first entry that actually parsed bound -- not necessarily
    // index 0, if an earlier token failed to parse. Compute it the same way
    // GetCurrentBinding does, so the log's "=primary" marker never lies.
    size_t primary_index = bindings.size();
    for (size_t i = 0; i < bindings.size(); ++i) {
      if (bindings[i].is_bound()) {
        primary_index = i;
        break;
      }
    }

    std::string entries;
    int valid_count = 0;
    for (size_t i = 0; i < bindings.size(); ++i) {
      const KbmBinding& binding = bindings[i];
      int code = 0;
      switch (binding.kind) {
        case BindingKind::kKeyboardKey:
          code = static_cast<int>(binding.key);
          break;
        case BindingKind::kMouseButton:
          code = static_cast<int>(binding.mouse_button);
          break;
        default:
          code = 0;
          break;
      }
      if (binding.is_bound()) ++valid_count;
      if (i != 0) entries += ", ";
      entries += "[" + std::to_string(i) + (i == primary_index ? "=primary" : "") + "] kind=" +
                 BindingKindName(binding.kind) + " code=" + std::to_string(code) +
                 " resolved=\"" + BindingToString(binding) + "\"";
    }
    if (bindings.empty()) entries = "(none)";

    REXLOG_INFO(
        "[InputBindings] [DIAG] {} (cvar={}, raw=\"{}\") -> {}/{} valid binding(s): {}",
        GameActionDescription(action), cvar_name ? cvar_name : "", raw_value, valid_count,
        static_cast<int>(bindings.size()), entries);
  }

  // Combat-mode select cvars (2026-09-14): logged the same way as the ten
  // GameAction cvars above, so the parser/multi-binding path can be
  // verified for these too, plus the mode this state starts in (always
  // kMelee at launch -- see CombatMode's comment in input_bindings.h).
  constexpr CombatMode kAllModes[] = {CombatMode::kMelee, CombatMode::kRanged, CombatMode::kMagic};
  for (CombatMode mode : kAllModes) {
    const char* cvar_name = CombatModeCvarName(mode);
    std::string raw_value = cvar_name && *cvar_name ? rex::cvar::GetFlagByName(cvar_name) : "";
    std::vector<KbmBinding> bindings = ParseBindingList(raw_value);
    int valid_count = 0;
    for (const KbmBinding& binding : bindings) {
      if (binding.is_bound()) ++valid_count;
    }
    REXLOG_INFO(
        "[InputBindings] [DIAG] Combat mode select: {} (cvar={}, raw=\"{}\") -> {}/{} valid binding(s)",
        CombatModeName(mode), cvar_name ? cvar_name : "", raw_value, valid_count,
        static_cast<int>(bindings.size()));
  }
  REXLOG_INFO("[InputBindings] [DIAG] Combat mode at startup: {}", CombatModeName(GetCurrentCombatMode()));
  REXLOG_INFO("[InputBindings] [DIAG] ---- End binding resolution dump ----");
}

// TEMPORARY DIAGNOSTIC (see input_bindings.h) -- thin pass-through to the
// existing, unmodified IsBindingActive() above. Added only so
// glyph_system.cpp can query held state for a raw KbmBinding it constructs
// itself (from a fixed candidate-key list), without this file needing to
// know anything about that list. Remove once the architecture-verification
// pass this round is done.
bool IsPhysicalBindingDown(const KbmBinding& binding) {
  return IsBindingActive(binding);
}

}  // namespace fable2::glyph
