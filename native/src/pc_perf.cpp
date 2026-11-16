// Fable II PC performance helpers (PERF-1006, 2026-10-06).
//
// * Raises the Windows timer resolution to 1 ms for this process. Without it a
//   Sleep(1) / 1 ms wait anywhere in the process (game threads via the
//   kernel's delay/wait paths, the runtime, the GPU thread) rounds up to the
//   ~15.6 ms system tick on Windows 10/11.
// * In the instrumented PGO build (FABLE2_PGO=gen), writes the profile every
//   30 s and on shutdown, because the game hard-exits on close.
// * Logs a one-time "[PERF-1006]" marker so a log proves which build ran.

#include "pc_perf.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <timeapi.h>

#include <chrono>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>

#include <rex/logging.h>

#pragma comment(lib, "winmm.lib")

#ifdef FABLE2_PGO_GEN
extern "C" int __llvm_profile_write_file(void);
extern "C" void __llvm_profile_set_filename(const char*);
#endif

namespace fable2::perf {

namespace {
#ifdef FABLE2_PGO_GEN
std::string g_profile_path = "pgo_fable2.profraw";
std::string g_profile_tmp_path = "pgo_fable2.profraw.tmp";
std::mutex g_profile_mutex;

// [PERF-1006c]: the first PGO session's merge failed. Writes could overlap (the
// 30 s thread and Shutdown()) and the game hard-exits, possibly mid-write, so
// the .profraw could be torn. Now: one writer at a time, write to a temp file,
// then atomically replace the real one - a kill mid-write keeps the last
// complete profile.
void WriteProfileAtomically() {
  std::lock_guard<std::mutex> lock(g_profile_mutex);
  __llvm_profile_set_filename(g_profile_tmp_path.c_str());
  if (__llvm_profile_write_file() == 0) {
    MoveFileExA(g_profile_tmp_path.c_str(), g_profile_path.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
  }
}
#endif
}  // namespace

void Startup() {
  const MMRESULT timer_result = timeBeginPeriod(1);

#if defined(FABLE2_PGO_GEN)
  constexpr const char* kPgo = "gen (instrumented, slower)";
#elif defined(FABLE2_PGO_USE)
  constexpr const char* kPgo = "use";
#else
  constexpr const char* kPgo = "off";
#endif
  REXLOG_INFO("[PERF-1006] exe perf pass active: timer_resolution_1ms={} pgo={}",
              timer_result == TIMERR_NOERROR, kPgo);

#ifdef FABLE2_PGO_GEN
  wchar_t path[32768] = {};
  if (GetModuleFileNameW(nullptr, path, 32768)) {
    g_profile_path =
        (std::filesystem::path(path).parent_path() / "pgo_fable2.profraw").string();
    g_profile_tmp_path = g_profile_path + ".tmp";
  }
  std::thread([] {
    for (;;) {
      std::this_thread::sleep_for(std::chrono::seconds(30));
      WriteProfileAtomically();
    }
  }).detach();
  REXLOG_INFO("[PERF-1006] PGO: exe profile -> {} every 30 s", g_profile_path);
#endif
}

void Shutdown() {
#ifdef FABLE2_PGO_GEN
  WriteProfileAtomically();
  REXLOG_INFO("[PERF-1006] PGO: exe profile written on shutdown");
#endif
}

}  // namespace fable2::perf
