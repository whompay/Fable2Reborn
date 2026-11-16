// fable2 - raw mouse look (2026-11-15)
//
// Replaces the SDK MnkInputDriver's mouse -> right-stick mapping
// (stick = mouse_delta_since_last_poll * mnk_sensitivity * 200, i.e. a
// per-poll value, so camera speed depended on poll rate / framerate and was
// shaped by the game's own stick deadzone + response curve + max turn rate)
// with a PC-style mapping: rotation is linear in mouse counts and
// framerate-independent.
//
// Model (native/mouse_camera_findings.md): the guest normalises the stick
// with a RADIAL 0.3 deadzone + linear rescale; 3rd-person yaw takes the
// latest stick value once per camera update (assumed 15 Hz) and turns
// 8 deg * (0.5 + game sens) * x. We keep a sliding window of mouse counts one
// camera update long, so the held stick value at each camera tick encodes
// exactly the counts since the previous tick (1:1, framerate-independent,
// every count used once), and invert the radial deadzone on the vector
// magnitude. Demand above full deflection is carried (capped ~100 ms). When
// the mouse stops, output returns to exactly 0 within one camera update.
// Vertical is a decaying camera-height offset in the game, so it can't be
// 1:1; it is made proportional and kept above the game's 0.1 threshold.
//
// Cvars (Fable II category): fable2_raw_mouse, fable2_mouse_sensitivity,
// fable2_mouse_invert_y, fable2_mouse_y_scale, fable2_mouse_game_sens.
//
// The SDK mouse capture (mnk_mouse = true) is still required: cursor_lock and
// SDL relative mode depend on it; this module only overwrites the guest's
// sThumbRX/sThumbRY after the SDK has filled them in.

#pragma once

#include <cstdint>

namespace fable2::raw_mouse {

// UI thread: called for every mouse move event (event.dx()/dy(): SDL's raw
// relative deltas while the SDK holds relative mode).
void OnMouseMove(float dx, float dy);

// Guest thread: called from glyph::OnInputPolled after the SDK and the
// button override have written the guest XINPUT_STATE. kbm_active = KBM is
// the active input device. Writes sThumbRX (+12) / sThumbRY (+14) in place
// when raw mouse look applies to this poll. Returns true if it wrote them.
bool ApplyToGuestState(uint32_t user_index, uint32_t state_guest_ptr,
                       uint32_t xam_status, uint8_t* base, bool kbm_active);

// Guest thread: when raw mouse did NOT write this poll (controller active,
// raw mouse off/inactive), divide the right stick's game-normalised vector by
// fable2_cam_rate_scale (the K baked into the patched camera Lua) and
// re-encode it, so pads (and the SDK mouse path) turn as in the stock game.
// No-op when the scale is 1. Right stick only.
void CompensateRightStick(uint32_t state_guest_ptr, uint8_t* base);

}  // namespace fable2::raw_mouse
