// fable2 - raw mouse look (2026-11-15). See raw_mouse.h.

#include "raw_mouse.h"

#include <algorithm>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>

#include "cursor_lock.h"

REXCVAR_DEFINE_BOOL(fable2_raw_mouse, true, "Fable II",
    "PC-style mouse look: linear, 1:1, framerate-independent camera turning "
    "(replaces the SDK's mouse->right-stick emulation). false = old SDK "
    "behaviour (mnk_sensitivity).");
REXCVAR_DEFINE_DOUBLE(fable2_mouse_sensitivity, 1.0, "Fable II",
    "Raw mouse look sensitivity. 1.0 = 0.055 degrees per mouse count "
    "(about a typical 800 DPI shooter setting, ~16 cm per 360). 2.0 = twice "
    "as fast.")
    .range(0.01, 20.0);
REXCVAR_DEFINE_BOOL(fable2_mouse_invert_y, false, "Fable II",
    "Raw mouse look: invert vertical look.");
REXCVAR_DEFINE_DOUBLE(fable2_mouse_game_sens, 0.5, "Fable II",
    "Raw mouse look: the game's own camera sensitivity value (the 'sens' in "
    "the camera script's (0.5 + sens) factor). Set it to match the in-game "
    "setting so 1:1 turning is computed correctly.")
    .range(0.0, 5.0);
REXCVAR_DEFINE_DOUBLE(fable2_cam_rate_scale, 8.0, "Fable II",
    "Camera max-rate multiplier baked into the patched camera Lua (yaw/pitch "
    "max rates x K, 3rd and 1st person). MUST match the Lua patch; 1 = stock "
    "game bank. Mouse output and the controller right stick are divided by "
    "it so mouse saturates K times less often and pads feel unchanged.")
    .range(1.0, 64.0);
REXCVAR_DEFINE_DOUBLE(fable2_cam_window_ms, 5.0, "Fable II",
    "Raw mouse look: how long mouse counts are held in the right stick (ms). "
    "The camera script reads the stick once per camera update. 5 = per poll "
    "(correct if the camera updates every frame -- lowest latency, default). "
    "66.7 = 15 Hz camera updates. If turning feels ~2x too fast/smeared use 5; "
    "if small movements get lost use 66.7.")
    .range(1.0, 200.0);
REXCVAR_DEFINE_DOUBLE(fable2_mouse_y_scale, 1.0, "Fable II",
    "Raw mouse look: vertical sensitivity multiplier relative to horizontal.")
    .range(0.05, 10.0);

namespace fable2::raw_mouse {
namespace {

// ---------------------------------------------------------------------------
// GAME STICK RESPONSE -- from native/mouse_camera_findings.md (2026-11-15).
//
// Stage 1 (HIGH confidence, sub_82189058 / sub_821895B0): raw/32767, Y negated,
// RADIAL deadzone 0.3 with linear rescale s = (|v|-0.3)/0.7, per-component
// clamp +-1, no curve. Inverse for desired processed vector v (|v| <= 1):
//   raw = (v/|v|) * (0.3 + 0.7*|v|) * 32767   (on the vector magnitude).
// The Y negation is a reflection, so we work in raw-stick space directly.
//
// Stage 2 (MEDIUM confidence, stringandcagecamera.lua, 3rd person):
//   yaw:   RotateRight = -x * (120/TickRate) * (0.5 + sens) deg per camera
//          update, TickRate = 15 -> 8 deg * (0.5+sens) per update; the value
//          is OVERWRITTEN per input event (not summed), the camera uses the
//          latest one each update (assumed 15 Hz). Smoothing S=(1.5S+R)/2.5 is
//          angle-preserving.
//   pitch: camera height M += -y*0.05*(0.5+sens) per event when |y| > 0.1,
//          clamp +-0.7 (~35 deg), decays M *= 20/21 per update -> can't be
//          1:1; we only make it proportional and always above the threshold.
//   First person (not specially handled): +-6 deg/update, pitch clamp 55 deg.
// ---------------------------------------------------------------------------
constexpr double kGameDeadzone = 0.3;              // radial, normalised
constexpr double kCameraUpdateHz = 15.0;           // TickRate (medium confidence)
constexpr double kYawDegPerUpdateBase = 120.0 / kCameraUpdateHz;  // * (0.5 + sens)
constexpr double kPitchThreshold = 0.1;            // |processed y| must exceed this
// Free pitch (2026-11-15, pairs with tools/patch_sac_free_pitch.py): the camera
// Lua no longer springs the height offset back, so it is a pure integrator:
// M += -y * kHeightAdjustSpeed * (0.5+sens) per event, M ~ tan(pitch). We
// output exactly the mouse's vertical distance as height (small-angle: 1 rad
// ~ 1 height unit), one event per output period. Sub-threshold remainders are
// carried until they add up.
// tools/patch_sac_pitch_speed.py: HeightAdjustSpeed 0.05 x K (=0.4, K =
// fable2_cam_rate_scale), HeightDamp 0, and the Lua's 0.1 pitch threshold removed.
constexpr double kHeightAdjustSpeed = 0.05;        // stock value; times cam_rate_scale
constexpr double kPitchMinOut = 1e-4;              // no game threshold any more
constexpr double kPitchMaxCarry = 0.5;             // height units (~27 deg)
constexpr double kPi = 3.14159265358979323846;

// fable2_mouse_sensitivity = 1.0 maps to this many degrees per mouse count.
// 0.055 deg/count = e.g. CS/Source sens 2.5 (yaw 0.022) at 800 DPI.
constexpr double kDegPerCountAtSens1 = 0.055;

// Polls closer together than this repeat the previous output (the guest
// polls XInput several times per frame; only the first poll of a frame
// starts a new output period). Periods longer than kMaxPeriodMs (hitches,
// pause) are treated as kMaxPeriodMs.
constexpr double kMinPeriodMs = 3.0;
constexpr double kMaxPeriodMs = 50.0;

// Demand beyond full deflection is carried, but never more than this much
// full-rate turning time (no long drift after a huge flick).
constexpr double kMaxCarrySeconds = 0.100;

// A slot counts as "present" if it returned success within this window.
constexpr int64_t kSlotPresentMs = 1000;

// Diagnostics: log every output period with movement for the first N, then
// at most one line every kLogEveryMs.
constexpr int kLogFirstN = 200;
constexpr int64_t kLogEveryMs = 2000;

// Mouse accumulator, fixed point (1/1024 count), UI thread -> guest thread.
constexpr double kFixedScale = 1024.0;
std::atomic<int64_t> g_acc_x{0};
std::atomic<int64_t> g_acc_y{0};

std::atomic<int64_t> g_slot_last_ok_ms[4] = {};

// Guest-thread state (OnInputPolled runs on the guest's input thread).
struct AxisState {
  double carry = 0.0;  // mouse counts still owed to the camera
  int16_t out = 0;     // value repeated within one output period
};
AxisState g_ax, g_ay;
std::chrono::steady_clock::time_point g_last_period{};
bool g_have_last_period = false;
bool g_was_active = false;
int g_logged = 0;
int64_t g_last_log_ms = 0;

int64_t NowMs(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(t.time_since_epoch()).count();
}

inline void StoreS16(uint8_t* base, uint32_t addr, int16_t v) {
  *reinterpret_cast<uint16_t*>(base + addr) = std::byteswap(static_cast<uint16_t>(v));
}

// Yaw sliding window: counts ACCEPTED into the stick output during the last
// camera update period. The camera samples the held value once per update,
// so sum(window) * deg_per_count is exactly the turn that update applies --
// every count is used exactly once, independent of framerate and phase.
struct WindowSample {
  std::chrono::steady_clock::time_point t;
  double counts;
};
constexpr int kWindowCap = 256;
WindowSample g_win[kWindowCap];
int g_win_head = 0, g_win_size = 0;
double g_win_sum = 0.0;

void WindowExpire(std::chrono::steady_clock::time_point now) {
  const auto window = std::chrono::duration<double>(REXCVAR_GET(fable2_cam_window_ms) / 1000.0);
  while (g_win_size > 0) {
    const WindowSample& s = g_win[g_win_head];
    if (now - s.t < window) break;
    g_win_sum -= s.counts;
    g_win_head = (g_win_head + 1) % kWindowCap;
    --g_win_size;
  }
  if (g_win_size == 0) g_win_sum = 0.0;  // kill float drift
}

void WindowPush(std::chrono::steady_clock::time_point now, double counts) {
  if (counts == 0.0) return;
  if (g_win_size == kWindowCap) {  // should not happen (>=3 ms periods)
    g_win_sum -= g_win[g_win_head].counts;
    g_win_head = (g_win_head + 1) % kWindowCap;
    --g_win_size;
  }
  g_win[(g_win_head + g_win_size) % kWindowCap] = {now, counts};
  ++g_win_size;
  g_win_sum += counts;
}

void WindowReset() {
  g_win_head = g_win_size = 0;
  g_win_sum = 0.0;
}

// Radial inverse: desired processed (vx, vy) in raw-stick orientation -> raw.
void InverseRadial(double vx, double vy, int16_t& rx, int16_t& ry) {
  const double m = std::sqrt(vx * vx + vy * vy);
  if (m <= 0.0) {
    rx = ry = 0;
    return;
  }
  const double raw_m = kGameDeadzone + (1.0 - kGameDeadzone) * std::min(m, 1.0);
  rx = static_cast<int16_t>(std::clamp(std::round(vx / m * raw_m * 32767.0), -32767.0, 32767.0));
  ry = static_cast<int16_t>(std::clamp(std::round(vy / m * raw_m * 32767.0), -32767.0, 32767.0));
}

inline int16_t LoadS16(const uint8_t* base, uint32_t addr) {
  return static_cast<int16_t>(std::byteswap(*reinterpret_cast<const uint16_t*>(base + addr)));
}

}  // namespace

void CompensateRightStick(uint32_t state_guest_ptr, uint8_t* base) {
  const double k = REXCVAR_GET(fable2_cam_rate_scale);
  if (!(k > 1.0) || state_guest_ptr == 0 || base == nullptr) return;
  const int16_t rx = LoadS16(base, state_guest_ptr + 12);
  const int16_t ry = LoadS16(base, state_guest_ptr + 14);
  if (rx == 0 && ry == 0) return;
  // Exactly the guest's sub_821895B0 normalisation.
  const double x = double(rx) / 32767.0;
  const double y = double(ry) / 32767.0;
  const double m = std::sqrt(x * x + y * y);
  if (m < kGameDeadzone) return;  // inside deadzone: guest sees 0 either way
  const double sc = (m - kGameDeadzone) / (1.0 - kGameDeadzone);
  const double px = std::clamp(sc * x / m, -1.0, 1.0);
  const double py = std::clamp(sc * y / m, -1.0, 1.0);
  int16_t ox = 0, oy = 0;
  // Yaw and pitch speed are both K-scaled in the patched Lua.
  InverseRadial(px / k, py / k, ox, oy);
  StoreS16(base, state_guest_ptr + 12, ox);
  StoreS16(base, state_guest_ptr + 14, oy);
}

void OnMouseMove(float dx, float dy) {
  if (dx != 0.0f) g_acc_x.fetch_add(static_cast<int64_t>(std::llround(double(dx) * kFixedScale)),
                                    std::memory_order_relaxed);
  if (dy != 0.0f) g_acc_y.fetch_add(static_cast<int64_t>(std::llround(double(dy) * kFixedScale)),
                                    std::memory_order_relaxed);
}

bool ApplyToGuestState(uint32_t user_index, uint32_t state_guest_ptr, uint32_t xam_status,
                       uint8_t* base, bool kbm_active) {
  const auto now = std::chrono::steady_clock::now();
  const int64_t now_ms = NowMs(now);
  if (user_index < 4 && xam_status == 0) {
    g_slot_last_ok_ms[user_index].store(now_ms, std::memory_order_relaxed);
  }
  if (xam_status != 0 || state_guest_ptr == 0 || base == nullptr || user_index >= 4) return false;

  // The player's slot: the lowest slot that is currently returning state.
  uint32_t player_slot = user_index;
  for (uint32_t i = 0; i < user_index; ++i) {
    if (now_ms - g_slot_last_ok_ms[i].load(std::memory_order_relaxed) < kSlotPresentMs) {
      player_slot = i;
      break;
    }
  }
  if (user_index != player_slot) return false;

  const bool active = REXCVAR_GET(fable2_raw_mouse) && kbm_active &&
                      rex::input::mnk::IsMouseLookActive() &&
                      fable2::cursor_lock::GameOwnsMouse();
  if (!active) {
    // Drop anything gathered while inactive so re-activation never snaps.
    g_acc_x.store(0, std::memory_order_relaxed);
    g_acc_y.store(0, std::memory_order_relaxed);
    g_ax = {};
    g_ay = {};
    WindowReset();
    g_have_last_period = false;
    if (g_was_active) {
      g_was_active = false;
      REXLOG_INFO("[RawMouse] inactive (raw={} kbm={} mouselook={} owns_mouse={})",
                  REXCVAR_GET(fable2_raw_mouse), kbm_active,
                  rex::input::mnk::IsMouseLookActive(), fable2::cursor_lock::GameOwnsMouse());
    }
    return false;
  }
  if (!g_was_active) {
    g_was_active = true;
    REXLOG_INFO("[RawMouse] active on slot {} (sens={} deg/count={} invert_y={} y_scale={} "
                "game_sens={} cam_rate_scale={} yaw_deg_per_update_full={} deadzone={} cam_hz={})",
                user_index, REXCVAR_GET(fable2_mouse_sensitivity),
                REXCVAR_GET(fable2_mouse_sensitivity) * kDegPerCountAtSens1,
                REXCVAR_GET(fable2_mouse_invert_y), REXCVAR_GET(fable2_mouse_y_scale),
                REXCVAR_GET(fable2_mouse_game_sens), REXCVAR_GET(fable2_cam_rate_scale),
                kYawDegPerUpdateBase * (0.5 + REXCVAR_GET(fable2_mouse_game_sens)) *
                    REXCVAR_GET(fable2_cam_rate_scale),
                kGameDeadzone, kCameraUpdateHz);
  }

  double dt_ms = kMaxPeriodMs;
  if (g_have_last_period) {
    dt_ms = std::chrono::duration<double, std::milli>(now - g_last_period).count();
  }
  const bool new_period = !g_have_last_period || dt_ms >= kMinPeriodMs;
  if (new_period) {
    g_last_period = now;
    g_have_last_period = true;
    const double dt_s = std::clamp(dt_ms, kMinPeriodMs, kMaxPeriodMs) / 1000.0;

    const double mx = double(g_acc_x.exchange(0, std::memory_order_relaxed)) / kFixedScale;
    const double my = double(g_acc_y.exchange(0, std::memory_order_relaxed)) / kFixedScale;

    const double deg_per_count = REXCVAR_GET(fable2_mouse_sensitivity) * kDegPerCountAtSens1;
    const double game_sens_factor = 0.5 + REXCVAR_GET(fable2_mouse_game_sens);
    // Raw RY positive = stick up; mouse dy positive = down (same as the SDK).
    const double y_sign = REXCVAR_GET(fable2_mouse_invert_y) ? 1.0 : -1.0;

    // --- Yaw: sliding window, capacity = counts for a full-deflection update.
    const double cam_k = REXCVAR_GET(fable2_cam_rate_scale);
    const double yaw_deg_per_update_full = kYawDegPerUpdateBase * game_sens_factor * cam_k;
    const double capacity = deg_per_count > 0.0 ? yaw_deg_per_update_full / deg_per_count : 0.0;
    WindowExpire(now);
    double want_x = mx + g_ax.carry;
    double room = 0.0;
    if (want_x > 0.0) room = std::max(0.0, capacity - g_win_sum);
    else if (want_x < 0.0) room = std::max(0.0, capacity + g_win_sum);
    double accept_x = std::clamp(want_x, -room, room);
    // Direction reversal: accept immediately, drop opposite old window content.
    if ((want_x > 0.0 && g_win_sum < 0.0) || (want_x < 0.0 && g_win_sum > 0.0)) {
      WindowReset();
      accept_x = std::clamp(want_x, -capacity, capacity);
    }
    WindowPush(now, accept_x);
    // Carry cap in counts stays what it was before the K scale (~100 ms of
    // unscaled max rate), i.e. at most ~100/K ms of scaled max-rate turning.
    const double max_carry = capacity * kMaxCarrySeconds * kCameraUpdateHz / cam_k;
    g_ax.carry = std::clamp(want_x - accept_x, -max_carry, max_carry);
    const double vx = capacity > 0.0 ? std::clamp(g_win_sum / capacity, -1.0, 1.0) : 0.0;

    // --- Pitch: distance-based (camera height is a free integrator now).
    (void)dt_s;
    const double height_per_unit = kHeightAdjustSpeed * cam_k * game_sens_factor;  // per event at |y|=1
    const double want_h = g_ay.carry + y_sign * my * deg_per_count *
                          REXCVAR_GET(fable2_mouse_y_scale) * (kPi / 180.0);
    double vy = want_h / height_per_unit;
    // Keep |v| <= 1 radially; yaw keeps priority but pitch is carried, not lost.
    const double max_vy = std::sqrt(std::max(0.0, 1.0 - vx * vx));
    vy = std::clamp(vy, -max_vy, max_vy);
    if (std::abs(vy) < kPitchMinOut) vy = 0.0;
    g_ay.carry = std::clamp(want_h - vy * height_per_unit, -kPitchMaxCarry, kPitchMaxCarry);
    InverseRadial(vx, vy, g_ax.out, g_ay.out);

    if (mx != 0.0 || my != 0.0 || g_ax.carry != 0.0 || g_win_size > 0) {
      if (g_logged < kLogFirstN || now_ms - g_last_log_ms >= kLogEveryMs) {
        ++g_logged;
        g_last_log_ms = now_ms;
        REXLOG_INFO("[RawMouse] dt={:.2f}ms delta=({:.1f},{:.1f}) win={:.1f}/{:.1f} v=({:.3f},{:.3f}) "
                    "stick=({},{}) carry_x={:.1f} carry_h={:.4f}",
                    dt_ms, mx, my, g_win_sum, capacity, vx, vy, g_ax.out, g_ay.out, g_ax.carry,
                    g_ay.carry);
      }
    }
  }

  StoreS16(base, state_guest_ptr + 12, g_ax.out);
  StoreS16(base, state_guest_ptr + 14, g_ay.out);
  return true;
}

}  // namespace fable2::raw_mouse
