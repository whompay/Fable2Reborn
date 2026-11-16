#include "pc_features.h"
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc/context.h>

#ifdef TRACY_ENABLE
#include <tracy/TracyC.h>
#endif

REXCVAR_DEFINE_BOOL(fable2_60fps, false, "Fable II",
    "Use the title-specific 60 FPS presentation interval; restart required.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [FPS120] (2026-11-16): one frame-rate setting for the game, the in-game PC
// Settings menu and the launcher. 30/60/120; 0 = not set (older settings files:
// follow fable2_60fps). The game waits `interval` guest vblanks per frame and the
// guest vblank timer runs at video_mode_refresh_rate, so:
//   30 = interval 2 @ 60 Hz, 60 = interval 1 @ 60 Hz, 120 = interval 1 @ 120 Hz.
REXCVAR_DEFINE_INT32(fable2_frame_rate, 0, "Fable II",
    "Frame rate: 30, 60 or 120 FPS (0 = use fable2_60fps). Restart required.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

static int Fable2EffectiveFrameRate() {
  const int r = REXCVAR_GET(fable2_frame_rate);
  if (r >= 120) return 120;
  if (r >= 60) return 60;
  if (r > 0) return 30;
  return REXCVAR_GET(fable2_60fps) ? 60 : 30;
}

// Media 716F0A0D, Margen67's 60 FPS patch: li r11,2 -> li r11,1.
// Change only the selected 30 FPS presentation interval, not the game clock.
void Fable2ConfigureFrameInterval(PPCRegister& r11) {
  if (Fable2EffectiveFrameRate() >= 60) r11.s64 = 1;
}

REXCVAR_DEFINE_BOOL(fable2_disable_msaa, false, "Fable II",
    "Disable game MSAA to reduce GPU load; restart required.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [BGFIX-1116] (2026-11-16): the recompiler's vexptefp (2^x estimate) builds
// 2^floor(x) by shifting floor(x)+127 into the float exponent with no range
// check, so x < -127 gives huge/negative garbage (2^-200 -> -7.2e16), x >= 128
// gives ~0 and +-inf gives NaN. Fable II's own exp2f/expf/powf helpers
// (sub_82BA34B0, sub_82BA3598, sub_82CE24B0, sub_82D09D00, sub_82D0A168,
// sub_82D0B2C0) use it, so any decay/fog/scatter term exp(-k*d) with a large
// argument blows up instead of going to 0 -> blue/purple sky/background.
// Same bug FNC had (playbook 3.5). The 7 sites in native/generated/default are
// patched to read this flag; false = old (broken) behaviour for A/B.
REXCVAR_DEFINE_BOOL(fable2_vexptefp_fix, true, "Fable II",
    "[BGFIX-1116] Correct vexptefp (2^x) for out-of-range inputs: 0 below 2^-126, "
    "+inf at 128 and above, NaN passes through (false = old recompiler output).");

#ifdef TRACY_ENABLE
// Perf-only (Tracy-enabled configs, e.g. RelWithDebInfo): one manually-scoped
// zone per guest frame, spanning from one VdSwap call to the next. This
// measures the guest's own per-frame CPU cost (game logic + any guest-side
// waiting) on the thread that calls VdSwap, in parallel with the existing
// GPU CommandProcessor::WorkerThreadMain zones, to establish a full
// guest-thread + worker-thread baseline. Added 2026-09-16 for the
// Bowerstone Market CPU-vs-GPU investigation; see
// native/performance-investigation-phase1.md. Compiled out entirely in
// Release (TRACY_ENABLE undefined there), so it costs nothing in that
// configuration. It only records a duration for the profiler; it never
// blocks, delays, or otherwise alters guest execution or game timing.
static TracyCZoneCtx g_fable2_guest_frame_tick_zone{};
static bool g_fable2_guest_frame_tick_zone_open = false;
#endif

// Called after the game's VdSwap call, once per submitted game frame.
void Fable2FrameSubmitted() {
#ifdef TRACY_ENABLE
  if (g_fable2_guest_frame_tick_zone_open) {
    TracyCZoneEnd(g_fable2_guest_frame_tick_zone);
    g_fable2_guest_frame_tick_zone_open = false;
  }
#endif
  fable2::submitted_frames.fetch_add(1, std::memory_order_relaxed);
#ifdef TRACY_ENABLE
  TracyCZoneN(fable2_guest_frame_tick_ctx, "Fable2::GuestFrameTick", TracyCIsStarted);
  g_fable2_guest_frame_tick_zone = fable2_guest_frame_tick_ctx;
  g_fable2_guest_frame_tick_zone_open = true;
#endif
}

// 716F0A0D: li r9,2 at 0x8238DF3C initializes the game's MSAA setting.
// Matches the title-specific disable-MSAA patch; leave other registers intact.
void Fable2ConfigureMsaa(PPCRegister& r9) {
  if (REXCVAR_GET(fable2_disable_msaa)) r9.s64 = 1;
}

namespace fable2 {

// [FPS120] Called from Fable2App::OnPreSetup: after fable2.toml is loaded and
// before the GPU plugin starts the guest vblank timer / the game queries its
// video mode. Old files (only fable2_60fps) are migrated to fable2_frame_rate.
void ApplyFrameRateSetting() {
  const bool explicit_rate = REXCVAR_GET(fable2_frame_rate) > 0;
  const int fps = Fable2EffectiveFrameRate();
  REXCVAR_SET(fable2_frame_rate, fps);
  REXCVAR_SET(fable2_60fps, fps >= 60);
  // Only an explicit fable2_frame_rate drives the refresh rate, and never over a
  // --video_mode_refresh_rate given on the command line.
  if (explicit_rate &&
      rex::cvar::GetFlagSource("video_mode_refresh_rate") != rex::cvar::Source::kCommandLine) {
    rex::cvar::SetFlagByName("video_mode_refresh_rate", fps >= 120 ? "120" : "60");
  }
  REXLOG_INFO("[FPS120] frame rate {} FPS (video_mode_refresh_rate={}, setting {})", fps,
              rex::cvar::GetFlagByName("video_mode_refresh_rate"),
              explicit_rate ? "fable2_frame_rate" : "migrated from fable2_60fps");
}

// Same 0.5s sampling-window approach as FpsOverlay::OnDraw (pc_features.h),
// reading the same submitted_frames counter -- kept as a free function with
// static state (rather than reusing FpsOverlay itself) since this is called
// from the F3 debug overlay's stats provider, independent of whether the F1
// overlay is open. Read-only; does not affect submitted_frames or timing.
rex::ui::FrameStats GetGuestFrameStats() {
  static auto sampled_at = std::chrono::steady_clock::now();
  static uint64_t sampled_frames = submitted_frames.load(std::memory_order_relaxed);
  static double fps = 0.0;
  static double frame_time_ms = 0.0;

  const auto now = std::chrono::steady_clock::now();
  const double seconds = std::chrono::duration<double>(now - sampled_at).count();
  const uint64_t frames = submitted_frames.load(std::memory_order_relaxed);
  if (seconds >= 0.5) {
    fps = double(frames - sampled_frames) / seconds;
    frame_time_ms = fps > 0.0 ? 1000.0 / fps : 0.0;
    sampled_frames = frames;
    sampled_at = now;
  }

  rex::ui::FrameStats stats;
  stats.fps = fps;
  stats.frame_time_ms = frame_time_ms;
  stats.frame_count = frames;
  return stats;
}

}  // namespace fable2
