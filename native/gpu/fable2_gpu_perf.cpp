// Fable II GPU plugin helpers (PERF-1006, 2026-10-06).
//
// Kept in its own file so the Win32 headers stay out of the big SDK sources.
// Added to rexgpu-xenos by native/gpu/CMakeLists.txt only.
//
// * Fable2ShortGpuWait(): replaces the WAIT_REG_MEM "Sleep(wait / 0x100 ms)"
//   in src/graphics/command_processor.cpp. A 1 ms Sleep() rounds up to the
//   system timer tick (~15.6 ms on Windows 10/11 unless the process raised the
//   timer resolution), so every unsatisfied wait could park the GPU command
//   thread for most of a frame. Waits ~200 us on a high-resolution waitable
//   timer instead and lets the caller re-poll (same fix as Fight Night,
//   playbook 4.26 / 5.9).
// * Fable2GpuFrameTick(): called once per IssueSwap. Logs a one-time build
//   marker (check for "[PERF-1006]" in the log) and, in the instrumented PGO
//   build only, writes the profile every 30 s because the game hard-exits.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <filesystem>
#include <string>

#include <rex/logging.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

// ---------------------------------------------------------------------------
// [PERF-1006b] (2026-10-06): command-processor (CP) thread statistics.
// Everything below is only touched from the CP thread ("GPU Commands"), so
// plain variables are enough. Timing uses QueryPerformanceCounter and is only
// taken around waits (idle, fence, WAIT_REG_MEM, contended lock), never per
// draw. One log line every 5 s:
//   [PERF-STAT] fps=.. frame_ms(avg/max)=.. draws/frame=.. subs/frame=..(split ..)
//               cp_busy=..% idle=..ms/s fence_wait=..ms/s regmem_wait=..ms/s
//               lock_wait=..ms/s (..x/s)
// cp_busy near 100% = the CP thread is the limit; high fence_wait = the GPU is.
// ---------------------------------------------------------------------------
namespace {
struct Fable2PerfStats {
  int64_t frames = 0;
  int64_t draws = 0;
  int64_t submissions = 0;
  int64_t split_submissions = 0;
  int64_t idle_ticks = 0;
  int64_t fence_ticks = 0;
  int64_t regmem_ticks = 0;
  int64_t lock_ticks = 0;
  int64_t lock_waits = 0;
  int64_t max_frame_ticks = 0;
  int64_t queries = 0;         // [PERF-1006c] guest occlusion queries ended
  int64_t queries_waited = 0;  // ... of which stalled the CP thread
  int64_t memexports = 0;         // [PERF-1006d] memexport readbacks
  int64_t memexports_waited = 0;  // ... of which stalled the CP thread
  int64_t replay_ticks = 0;  // [PERF-1116] CP thread waiting for the replay thread
};
Fable2PerfStats g_perf;
int64_t g_perf_window_start = 0;
int64_t g_perf_last_frame = 0;
int64_t g_qpc_freq = [] {
  LARGE_INTEGER f;
  QueryPerformanceFrequency(&f);
  return int64_t(f.QuadPart);
}();
}  // namespace

int64_t Fable2PerfNow() {
  LARGE_INTEGER c;
  QueryPerformanceCounter(&c);
  return int64_t(c.QuadPart);
}
void Fable2PerfCountDraw() { ++g_perf.draws; }
void Fable2PerfCountSubmission(bool split) {
  if (split) {
    ++g_perf.split_submissions;
  } else {
    ++g_perf.submissions;
  }
}
void Fable2PerfAddIdle(int64_t t) { g_perf.idle_ticks += t; }
void Fable2PerfCountMemexport(bool waited) {
  ++g_perf.memexports;
  if (waited) {
    ++g_perf.memexports_waited;
  }
}
void Fable2PerfCountQuery(bool waited) {
  ++g_perf.queries;
  if (waited) {
    ++g_perf.queries_waited;
  }
}
void Fable2PerfAddFenceWait(int64_t t) { g_perf.fence_ticks += t; }
void Fable2PerfAddReplayWait(int64_t t) { g_perf.replay_ticks += t; }  // [PERF-1116]
void Fable2PerfAddLockWait(int64_t t) {
  g_perf.lock_ticks += t;
  ++g_perf.lock_waits;
}

static void Fable2PerfOnFrame() {
  const int64_t now = Fable2PerfNow();
  if (!g_perf_window_start) {
    g_perf_window_start = now;
    g_perf_last_frame = now;
    g_perf = {};
    return;
  }
  ++g_perf.frames;
  const int64_t frame_ticks = now - g_perf_last_frame;
  g_perf_last_frame = now;
  if (frame_ticks > g_perf.max_frame_ticks) {
    g_perf.max_frame_ticks = frame_ticks;
  }
  const int64_t window = now - g_perf_window_start;
  if (window < g_qpc_freq * 5) {
    return;
  }
  const double sec = double(window) / double(g_qpc_freq);
  const double ms_per_tick = 1000.0 / double(g_qpc_freq);
  const double frames = g_perf.frames ? double(g_perf.frames) : 1.0;
  const double idle_ms = double(g_perf.idle_ticks) * ms_per_tick;
  const double fence_ms = double(g_perf.fence_ticks) * ms_per_tick;
  const double regmem_ms = double(g_perf.regmem_ticks) * ms_per_tick;
  const double lock_ms = double(g_perf.lock_ticks) * ms_per_tick;
  const double replay_ms = double(g_perf.replay_ticks) * ms_per_tick;  // [PERF-1116]
  const double busy =
      100.0 * (1.0 - (idle_ms + fence_ms + regmem_ms + replay_ms) / (sec * 1000.0));
  REXGPU_INFO(
      "[PERF-STAT] fps={:.1f} frame_ms(avg/max)={:.2f}/{:.2f} draws/frame={:.0f} "
      "subs/frame={:.1f} (split {:.1f}) cp_busy={:.0f}% idle={:.1f}ms/s fence_wait={:.1f}ms/s "
      "regmem_wait={:.1f}ms/s lock_wait={:.2f}ms/s ({:.0f}x/s) occl_queries={:.0f}/s (sync {:.0f}/s) memexport_readbacks={:.0f}/s (sync {:.0f}/s) "
      "replay_wait={:.1f}ms/s",
      double(g_perf.frames) / sec, sec * 1000.0 / frames,
      double(g_perf.max_frame_ticks) * ms_per_tick, double(g_perf.draws) / frames,
      double(g_perf.submissions + g_perf.frames) / frames,
      double(g_perf.split_submissions) / frames, busy, idle_ms / sec, fence_ms / sec,
      regmem_ms / sec, lock_ms / sec, double(g_perf.lock_waits) / sec,
      double(g_perf.queries) / sec, double(g_perf.queries_waited) / sec,
      double(g_perf.memexports) / sec, double(g_perf.memexports_waited) / sec,
      replay_ms / sec);
  g_perf = {};
  g_perf_window_start = now;
}

#ifdef FABLE2_GPU_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif

void Fable2ShortGpuWait() {
  thread_local HANDLE timer = [] {
    HANDLE h = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                      TIMER_ALL_ACCESS);
    if (!h) {
      // Older Windows without high-resolution timers: plain timer still
      // honours timeBeginPeriod(1), which fable2.exe sets at startup.
      h = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS);
    }
    return h;
  }();
  const int64_t wait_start = Fable2PerfNow();  // [PERF-1006b]
  if (timer) {
    LARGE_INTEGER due;
    due.QuadPart = -2000;  // relative, 100 ns units -> 200 us
    if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
      WaitForSingleObject(timer, INFINITE);
      g_perf.regmem_ticks += Fable2PerfNow() - wait_start;
      return;
    }
  }
  SwitchToThread();
  g_perf.regmem_ticks += Fable2PerfNow() - wait_start;
}

void Fable2GpuFrameTick() {
  static bool logged = false;
  if (!logged) {
    logged = true;
#if defined(__AVX2__)
    constexpr const char* kIsa = "avx2";
#else
    constexpr const char* kIsa = "sse4.1";
#endif
#if defined(FABLE2_GPU_PGO_GEN)
    constexpr const char* kPgo = "gen (instrumented, slower)";
#elif defined(FABLE2_GPU_PGO_USE)
    constexpr const char* kPgo = "use";
#else
    constexpr const char* kPgo = "off";
#endif
    REXGPU_INFO("[PERF-1006] GPU plugin perf pass active: isa={} pgo={} short_wait=200us "
                "glyph_diag=off",
                kIsa, kPgo);
    REXGPU_INFO("[PERF-1006b] mid-frame submission split + [PERF-STAT] every 5 s");
    REXGPU_INFO("[PERF-1006c] asynchronous occlusion-query results");
    REXGPU_INFO("[PERF-1006d] asynchronous memexport readback");
    REXGPU_INFO("[PERF-1116] memexport readback submit batching "
                "(fable2_memexport_submit_batch_draws) + optional replay thread "
                "(fable2_replay_thread)");
  }
  Fable2PerfOnFrame();
#ifdef FABLE2_GPU_PGO_GEN
  static auto next_write = std::chrono::steady_clock::now() + std::chrono::seconds(30);
  static bool named = false;
  static std::string profile_path = "pgo_rexgpu.profraw";
  if (!named) {
    named = true;
    // Absolute path next to the DLL, independent of the working directory.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&Fable2GpuFrameTick), &self);
    wchar_t path[32768] = {};
    if (GetModuleFileNameW(self, path, 32768)) {
      profile_path = (std::filesystem::path(path).parent_path() / "pgo_rexgpu.profraw").string();
    }
    REXGPU_INFO("[PERF-1006] PGO: GPU profile -> {} every 30 s", profile_path);
  }
  if (std::chrono::steady_clock::now() >= next_write) {
    next_write = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    // [PERF-1006c]: temp file + atomic replace, so a hard exit mid-write keeps
    // the last complete profile (the first session's merge failed).
    static std::string tmp_path;
    tmp_path = profile_path + ".tmp";
    __llvm_profile_set_filename(tmp_path.c_str());
    if (__llvm_profile_write_file() == 0) {
      MoveFileExA(tmp_path.c_str(), profile_path.c_str(),
                  MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    }
  }
#endif
}
