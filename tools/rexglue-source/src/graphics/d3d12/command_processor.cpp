/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <sstream>
#include <utility>
#include <set>
#include <tuple>
#include <deque>
#include <vector>
#include <condition_variable>  // [PERF-1116] replay thread
#include <atomic>  // [PERF-1117]
#include <bit>  // [PERF-1117] std::countr_zero
#include <memory>
#include <mutex>
#include <thread>

#include <rex/assert.h>
#include <rex/cvar.h>
#include <rex/dbg.h>
#include <rex/perf/counter.h>
#include <rex/graphics/d3d12/command_processor.h>
#include <rex/graphics/d3d12/graphics_system.h>
#include <rex/graphics/d3d12/shader.h>
#include <rex/graphics/flags.h>
#include <rex/graphics/registers.h>
#include <rex/graphics/util/draw.h>
#include <rex/graphics/xenos.h>
#include <rex/kernel/xboxkrnl/video.h>
#include <rex/logging.h>
#include <rex/memory/utils.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/d3d12/d3d12_util.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>  // Fable2PC diagnostic: GetPhysicalHeap()/
                                 // QueryRegionInfo(), see gpu_fetch_watch_address
#include "fable2_morph_readback.h"

REXCVAR_DEFINE_BOOL(fable2_morph_readback, true, "Fable II",
                    "Synchronously read back the GOTY dog/hero morph staging texture only.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_bindless, true, "GPU/D3D12", "Use bindless resources where available")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_BOOL(d3d12_readback_memexport, false, "GPU/D3D12",
                    "Read data written by memory export in shaders on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_readback_resolve, false, "GPU/D3D12",
                    "Read render-to-texture results on the CPU")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(d3d12_submit_on_primary_buffer_end, true, "GPU/D3D12",
                    "Submit command list when PM4 primary buffer ends")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Round 9 (2026-09-16, diagnostic-only): see the declaration comment in
// rex/graphics/flags.h. Default off; when off this cvar read is the only
// added cost anywhere on the draw path (a single already-established
// REXCVAR_GET check, same pattern as async_shader_compilation just above,
// which is already read unconditionally in this same hot function).
REXCVAR_DEFINE_BOOL(gpu_draw_dedup_analysis, false, "GPU/D3D12",
                    "Diagnostic only, not a rendering or performance change. When "
                    "enabled, hashes each draw's {pipeline, vertex-fetch, "
                    "texture-fetch} identity, and once per frame (in IssueSwap) logs "
                    "how many draws in that frame were exact duplicates of another "
                    "draw's state -- i.e. how many draws could theoretically be "
                    "merged/instanced. Off by default; zero cost when off.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Round 14 (2026-09-18, optimization prototype -- see
// native/performance-investigation-phase4.md, Round 14): two cvars, both
// required to change rendering, so a single flip of the first one is an
// instant, complete fallback to pre-Round-14 behavior:
//   gpu_exact_draw_dedup        -- master enable. Off by default. When
//                                  off, IssueDraw's only added cost is
//                                  this one REXCVAR_GET check, same
//                                  pattern as gpu_draw_dedup_analysis
//                                  above.
//   gpu_exact_draw_dedup_active -- when the master is ALSO on, actually
//                                  suppress a matched draw. When off (the
//                                  default) with the master on, this is
//                                  VALIDATION MODE: the full comparison
//                                  runs and is counted, but every draw is
//                                  still submitted normally -- nothing
//                                  rendered changes. Two separate opt-ins
//                                  are required to reach the
//                                  behavior-changing state, deliberately,
//                                  since this is the first Round 9-14
//                                  addition that can alter what's
//                                  submitted to the GPU rather than only
//                                  observe it.
REXCVAR_DEFINE_BOOL(gpu_exact_draw_dedup, false, "GPU/D3D12",
                    "Off by default; zero added cost when off beyond one "
                    "REXCVAR_GET check per draw. When on, computes a conservative "
                    "state+safety fingerprint per draw and compares it against only "
                    "the immediately preceding eligible draw (never a frame-wide "
                    "set). With gpu_exact_draw_dedup_active also on, a match "
                    "suppresses the repeated draw's GPU submission; otherwise this "
                    "is validation mode -- it measures and logs what WOULD be "
                    "skipped without changing anything rendered. See "
                    "native/performance-investigation-phase4.md, Round 14.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(gpu_exact_draw_dedup_active, false, "GPU/D3D12",
                    "Off by default. Has no effect unless gpu_exact_draw_dedup is "
                    "also on. When both are on, a matched draw is actually "
                    "suppressed (not just counted) -- this is the only cvar "
                    "combination in this codebase so far that changes what gets "
                    "submitted to the GPU based on a diagnostic-style comparison. "
                    "See native/performance-investigation-phase4.md, Round 14.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// Round 16 (Fable2PC, fortieth follow-up, 2026-09-17): a one-shot,
// external-diagnostic watch for a single guest-physical vertex-fetch
// address, set from OUTSIDE this DLL via rex::cvar::SetFlagByName (the
// generic cross-DLL string-based cvar setter -- see cvar.h; no new
// exported symbol needed). native/src/diagnostics/fable_draw_capture.cpp
// sets this to a constC address the moment its OWN guest-memory snapshot
// of that address fails, so this watch fires at the EXACT SAME address,
// checked instead at the one point in the real renderer proven to
// successfully resolve it: right after SharedMemory::RequestRange()
// succeeds for that vfetch constant, in the loop below. This answers a
// concrete question the Fable-side capture could not: does the guest
// memory backing a transient vertex buffer ever actually become readable
// via the SAME TranslatePhysical()/QueryRegionInfo() mechanism, just
// checked at the renderer's own authoritative moment instead of guessed
// at from outside it. Diagnostic only -- does not change what gets
// rendered. Zero cost when 0 (the default): a single already-established
// REXCVAR_GET plus one uint32 comparison per used vfetch slot, same
// pattern as gpu_draw_dedup_analysis above.
REXCVAR_DEFINE_UINT32(gpu_fetch_watch_address, 0u, "GPU/D3D12",
                      "Diagnostic only. When nonzero, the NEXT draw whose resolved "
                      "vertex-fetch guest-physical byte address equals this value "
                      "gets its bytes read (via the same TranslatePhysical() path "
                      "Fable2PC's own capture uses) and logged with full provenance "
                      "under the [GpuFetchWatch] tag, then this resets to 0 "
                      "(one-shot). Set externally via rex::cvar::SetFlagByName; not "
                      "meant to be set from the console.")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// PERF-1006 (2026-10-06): per-frame hook (build marker + PGO profile writer),
// defined in native/gpu/fable2_gpu_perf.cpp.
void Fable2GpuFrameTick();

// [PERF-1006b] (2026-10-06): CP-thread statistics (logged as [PERF-STAT] every
// 5 s from Fable2GpuFrameTick), defined in native/gpu/fable2_gpu_perf.cpp.
void Fable2PerfCountDraw();
void Fable2PerfCountSubmission(bool split);
void Fable2PerfAddFenceWait(int64_t qpc_ticks);
int64_t Fable2PerfNow();

// [PERF-1006b]: end the current submission every N guest draws so the GPU can
// start on the first part of a frame while the command-processor thread is
// still translating the rest of it. Without this a whole frame (one PM4 kick,
// because the CP thread is always behind) was recorded and only handed to the
// GPU at the end, so CPU and GPU time added up instead of overlapping (the
// "11.8 ms CPU + 8.4 ms GPU" resolution fit in the playbook, 10.1). The split
// happens right before a draw packet is processed, which is the same kind of
// boundary d3d12_submit_on_primary_buffer_end already ends submissions at.
REXCVAR_DEFINE_UINT32(fable2_submit_every_n_draws, 512u, "Fable II",
                      "End the GPU submission every N draws so CPU and GPU work "
                      "overlap within a frame (0 = old behaviour: one submission "
                      "per PM4 kick).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
static uint32_t fable2_draws_in_submission_ = 0;

// [PERF-1006c] (2026-10-06): asynchronous occlusion-query results.
// [PERF-STAT] in fable2_147 showed the command-processor thread waiting on the
// GPU fence ~270 ms of every second in Bowerstone (cp_busy ~75%, the rest
// fence waits). Every guest occlusion-query END ended the submission and then
// waited until the GPU had finished EVERYTHING queued so far, so CPU and GPU
// took turns several times per frame. Now the query result is resolved into
// the readback buffer as before but written to guest memory only once the GPU
// has reached it (polled from the CP thread, also while idle). That is how the
// real console behaves too: the result lands in memory later, and the game
// polls for the 0xFFFFFEED marker to be replaced.
REXCVAR_DEFINE_BOOL(fable2_async_occlusion_queries, true, "Fable II",
                    "Write occlusion-query results when the GPU has produced them instead "
                    "of stalling the GPU command thread until the GPU is idle (false = old "
                    "synchronous behaviour).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
void Fable2PerfCountQuery(bool waited);
void Fable2PerfCountMemexport(bool waited);

// [PERF-1006d] (2026-10-06): asynchronous memexport readback. fable2_149 with
// readback_memexport=false: fence_wait 250 -> ~0 ms/s and 42-46 -> 53-57 fps
// in Bowerstone, but the dog, the hero and distant ground glitched, so the CPU
// does need that data. Every memexporting draw used to copy its output to a
// readback buffer and then wait for the GPU to go idle (the "fast" path fell
// back to that whenever the previous copy was still in flight - always, within
// a frame). Now each copy goes into its own pooled readback buffer, the
// submission is handed to the GPU straight away, and the data is written to
// guest memory as soon as the GPU has produced it (no CP stall). A range that
// a newer pending copy also covers is skipped, so older data never lands over
// newer data.
REXCVAR_DEFINE_BOOL(fable2_async_memexport_readback, true, "Fable II",
                    "Copy memexport results to guest memory when the GPU has produced them "
                    "instead of stalling until the GPU is idle (false = old behaviour).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// Called from the CP idle loop in src/graphics/command_processor.cpp.
void Fable2CpIdlePoll();

// [BGFIX2-1116] (2026-11-16): on the host-render-target (RTV) path - the
// default on NVIDIA/AMD - pixel shader colour was written unclamped, so
// k_2_10_10_10_FLOAT (7e3) targets, emulated as R16G16B16A16_FLOAT, could hold
// negative values, values far above 31.875, +-inf and NaN. The real EDRAM (and
// the ROV path) clamp to the format's range and have no NaNs. Fable II's HDR
// scene + bloom turned such pixels at sky/foliage edges into saturated
// purple/blue, later white/cyan, glowing blobs. When true, the translated pixel
// shaders turn NaN into 0 and clamp to edram_rt_clamp, which is now also filled
// for the RTV path. false = old behaviour. Restart required (applied when
// shaders are translated).
REXCVAR_DEFINE_BOOL(fable2_rtv_clamp_color_output, true, "Fable II",
                    "[BGFIX2-1116] Clamp pixel shader colour to the guest render target format "
                    "range and replace NaN with 0 on the RTV path (false = old unclamped output).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
// [BGFIX3-1116] (2026-11-16): with fable2_rtv_clamp_color_output, a NaN
// channel became 0 on its own, so a sky pixel with NaN red but valid
// green/blue stayed cyan and the bloom blurred it into cyan flicker. When
// true, a pixel with a NaN in any channel is written as 0 in all channels.
REXCVAR_DEFINE_BOOL(fable2_rtv_nan_pixel_to_black, true, "Fable II",
                    "[BGFIX3-1116] With fable2_rtv_clamp_color_output: if any colour channel is "
                    "NaN, write the whole pixel as 0 (false = only that channel becomes 0).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

// [PERF-1116] (2026-11-16): memexport readback submission batching.
// [PERF-STAT] in fable2_157..160 (async memexport readback on): 30-106
// submissions per frame, almost all of them ended by a memexport readback
// (PERF-1006d ended the submission after every memexporting draw), and cp_busy
// follows the submission count far more than the draw count: ~2,150 draws with
// 31 subs/frame = 10 ms of CP time per frame, ~2,700 draws with 106 subs/frame
// = 17.4 ms. That is ~60-70 us of CP time per extra submission (replaying the
// deferred list, ExecuteCommandLists + Signal, then every root-signature /
// pipeline / descriptor-heap / viewport binding re-set in the next
// submission). Now the submission is ended N draws after the first pending
// readback copy instead (or earlier at the next split / primary-buffer end /
// swap), so a run of memexporting draws shares one submission. The data still
// goes to guest memory only once the GPU has produced it, exactly as before;
// it just arrives up to N draws (~0.5 ms of CP time at 128) later.
REXCVAR_DEFINE_UINT32(fable2_memexport_submit_batch_draws, 128u, "Fable II",
                      "With fable2_async_memexport_readback: end the GPU submission this "
                      "many draws after a memexport readback copy instead of right away, so "
                      "consecutive memexporting draws share one submission (0 = old "
                      "behaviour: one submission per memexporting draw).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
static bool fable2_memexport_flush_pending_ = false;
static uint32_t fable2_memexport_flush_countdown_ = 0;

// [PERF-1116]: replay thread (Saints Row approach, playbook 5.6), default ON since
// 2026-11-17 (Bowerstone 55 -> 62-64 fps, no issues). The deferred
// command list of a finished submission is replayed onto the D3D12 command
// list, closed, executed and signalled on a worker thread while the CP thread
// already translates the next submission. One submission in flight on the
// worker at a time (the CP waits for it before handing over the next one), and
// swap submissions stay fully synchronous so the presenter's own queue work is
// always ordered after the frame. See Fable2Replay* next to
// CheckSubmissionFence for the thread-safety argument.
REXCVAR_DEFINE_BOOL(fable2_replay_thread, true, "Fable II",
                    "Replay + submit GPU command lists on a separate thread so the GPU "
                    "command-processor thread can go on translating the next draws "
                    "(false = old behaviour: everything on the CP thread).")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
void Fable2PerfAddReplayWait(int64_t qpc_ticks);

// [PERF-1117] (2026-11-17): per-draw command-processor (CP) caches, ported from
// Saints Reborn's "perf round 5/5b" (same code, same verify mode). [PERF-STAT]
// shows the CP thread at 98-100% busy in heavy scenes, so the CP's per-draw
// cost is the frame-rate ceiling. Each switch is read once, at the first draw.
// None of them changes what is drawn; with fable2_cp_verify = true the cached
// result is recomputed 1 call in 16 and compared ("[PERF-1117] VERIFY" lines).
REXCVAR_DEFINE_BOOL(fable2_cp_regs_fast, true, "Fable II",
                    "[PERF-1117] Store context registers 0x2000-0x3FFF (no side effects) "
                    "with one byte-swapping copy instead of a call per register.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_translation_cache, true, "Fable II",
                    "[PERF-1117] Remember the shader translation of each (shader, "
                    "modification) instead of a map lookup per draw.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_float_runs, true, "Fable II",
                    "[PERF-1117] Copy runs of used float constants with one memcpy each.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_const_compare, true, "Fable II",
                    "[PERF-1117] Bool/loop and fetch constant writes that do not change the "
                    "value no longer mark constant buffers / textures / vertex buffers dirty.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_sampler_cache, true, "Fable II",
                    "[PERF-1117] Skip recomputing sampler parameters when the sampler layout "
                    "and all fetch constants are the same as at the last draw.")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_ib_whole_view, true, "Fable II",
                    "[PERF-1117] Guest index buffers: one index buffer view of the whole "
                    "shared memory, the offset goes into the draw (no IASetIndexBuffer per draw).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_BOOL(fable2_cp_verify, false, "Fable II",
                    "[PERF-1117] Verify mode for the fable2_cp_* caches: recompute 1 in 16 "
                    "and log how many differ (costs a little CP time; for testing).")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace rex::graphics::d3d12 {

// [PERF-1006c]: async occlusion-query helpers, defined next to
// EndGuestOcclusionQuery further down.
void Fable2PollPendingQueriesNow();
namespace {
void Fable2DropPendingQueries();
}  // namespace
// [PERF-1116]: replay thread, defined next to CheckSubmissionFence.
void Fable2ReplayWait();
void Fable2ReplayStop();
struct Fable2MemexportCopy {
  uint8_t* destination;  // guest memory (host pointer)
  uint32_t guest_address;
  uint32_t size;
  uint32_t buffer_offset;
};
// Returns the readback buffer to copy into (nullptr = use the old path).
ID3D12Resource* Fable2BeginAsyncMemexport(ID3D12Device* device, uint32_t total_size);
void Fable2EndAsyncMemexport(ID3D12Fence* fence, uint64_t submission,
                             std::vector<Fable2MemexportCopy>&& copies);
size_t Fable2PendingMemexportCount();
uint64_t Fable2OldestPendingMemexportSubmission();

// ---- [PERF-1117] per-draw CP caches (ported from Saints Reborn perf round 5/5b) ----
namespace {
struct P1117Switches {
  bool regs_fast = true, trcache = true, float_runs = true, const_cmp = true,
       sampler_cache = true, ib_view = true, verify = false;
};
const P1117Switches& P1117() {
  static const P1117Switches s = [] {
    P1117Switches r;
    r.regs_fast = REXCVAR_GET(fable2_cp_regs_fast);
    r.trcache = REXCVAR_GET(fable2_cp_translation_cache);
    r.float_runs = REXCVAR_GET(fable2_cp_float_runs);
    r.const_cmp = REXCVAR_GET(fable2_cp_const_compare);
    r.sampler_cache = REXCVAR_GET(fable2_cp_sampler_cache);
    r.ib_view = REXCVAR_GET(fable2_cp_ib_whole_view);
    r.verify = REXCVAR_GET(fable2_cp_verify);
    REXGPU_INFO("[PERF-1117] CP caches: regs_fast {} translation_cache {} float_runs {} "
                "const_compare {} sampler_cache {} ib_whole_view {} verify {}",
                r.regs_fast, r.trcache, r.float_runs, r.const_cmp, r.sampler_cache, r.ib_view,
                r.verify);
    return r;
  }();
  return s;
}
struct P1117VerifyStats {
  uint64_t checked[4] = {};
  uint64_t bad[4] = {};
  uint64_t calls = 0;
};
P1117VerifyStats g_p1117_verify;  // CP thread only
uint32_t g_p1117_sample = 0;      // CP thread only
bool P1117Sample() { return P1117().verify && (++g_p1117_sample & 15) == 0; }
void P1117VerifyNote(uint32_t kind, bool bad) {
  P1117VerifyStats& v = g_p1117_verify;
  ++v.checked[kind];
  if (bad) {
    ++v.bad[kind];
  }
  if (++v.calls >= 200000) {
    REXGPU_INFO("[PERF-1117] VERIFY: regs_fast {} checked / {} differ | translation cache {} / {} | "
                "float runs {} / {} | sampler cache {} / {}",
                v.checked[0], v.bad[0], v.checked[1], v.bad[1], v.checked[2], v.bad[2],
                v.checked[3], v.bad[3]);
    v = P1117VerifyStats();
  }
}
// Bumped when shader storage is loaded or caches are cleared (translations that
// failed while loading are destroyed then).
std::atomic<uint32_t> g_p1117_translation_epoch{1};
struct P1117TranslationEntry {
  const D3D12Shader* shader = nullptr;
  uint64_t modification = 0;
  uint32_t epoch = 0;
  D3D12Shader::D3D12Translation* translation = nullptr;
};
P1117TranslationEntry g_p1117_translations[512];  // CP thread only
D3D12Shader::D3D12Translation* P1117GetTranslation(D3D12Shader& shader, uint64_t modification) {
  if (!P1117().trcache) {
    return static_cast<D3D12Shader::D3D12Translation*>(shader.GetOrCreateTranslation(modification));
  }
  const uint64_t h = (uint64_t(reinterpret_cast<uintptr_t>(&shader)) >> 4) ^
                     (modification * 0x9E3779B97F4A7C15ull);
  P1117TranslationEntry& e = g_p1117_translations[(h ^ (h >> 29)) & 511];
  const uint32_t epoch = g_p1117_translation_epoch.load(std::memory_order_relaxed);
  if (e.shader == &shader && e.modification == modification && e.epoch == epoch) {
    if (P1117Sample()) {
      P1117VerifyNote(1, shader.GetOrCreateTranslation(modification) != e.translation);
    }
    return e.translation;
  }
  auto* translation =
      static_cast<D3D12Shader::D3D12Translation*>(shader.GetOrCreateTranslation(modification));
  e.shader = &shader;
  e.modification = modification;
  e.epoch = epoch;
  e.translation = translation;
  return translation;
}
// Copies the used float constants in bitmap order: each run of consecutive used
// constants with one memcpy (same bytes as one vec4 at a time).
uint8_t* P1117CopyFloatConstants(uint8_t* dst, const uint64_t* bitmap,
                                 const uint32_t* src_constant_000) {
  uint8_t* const dst_start = dst;
  if (!P1117().float_runs) {
    for (uint32_t i = 0; i < 4; ++i) {
      uint64_t bits = bitmap[i];
      while (bits) {
        const uint32_t index = uint32_t(std::countr_zero(bits));
        bits &= bits - 1;
        std::memcpy(dst, src_constant_000 + (((i << 6) + index) << 2), 4 * sizeof(float));
        dst += 4 * sizeof(float);
      }
    }
    return dst;
  }
  for (uint32_t i = 0; i < 4; ++i) {
    uint64_t bits = bitmap[i];
    while (bits) {
      const uint32_t first = uint32_t(std::countr_zero(bits));
      const uint64_t shifted = bits >> first;
      const uint32_t run = shifted == UINT64_MAX ? 64u : uint32_t(std::countr_zero(~shifted));
      std::memcpy(dst, src_constant_000 + (((i << 6) + first) << 2),
                  size_t(run) * 4 * sizeof(float));
      dst += size_t(run) * 4 * sizeof(float);
      bits = run == 64 ? 0 : bits & ~(((uint64_t(1) << run) - 1) << first);
    }
  }
  if (P1117Sample()) {
    const uint8_t* check = dst_start;
    bool bad = false;
    for (uint32_t i = 0; i < 4 && !bad; ++i) {
      uint64_t bits = bitmap[i];
      while (bits && !bad) {
        const uint32_t index = uint32_t(std::countr_zero(bits));
        bits &= bits - 1;
        bad = std::memcmp(check, src_constant_000 + (((i << 6) + index) << 2),
                          4 * sizeof(float)) != 0;
        check += 4 * sizeof(float);
      }
    }
    P1117VerifyNote(2, bad || check != dst);
  }
  return dst;
}
// Bumped whenever a fetch constant may have changed value, and at every swap
// (sampler parameters also depend on settings such as anisotropy). CP thread only.
uint64_t g_p1117_fetch_gen = 1;
struct P1117SamplerCache {
  size_t layout_uid = 0;
  uint64_t fetch_gen = 0;
  bool valid = false;
};
P1117SamplerCache g_p1117_sampler_cache[2];  // vertex, pixel; CP thread only
// Index buffer view currently set in the deferred command list (CP thread only;
// reset at every new submission).
struct P1117IbState {
  D3D12_GPU_VIRTUAL_ADDRESS location = 0;
  UINT size = 0;
  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
  bool valid = false;
};
P1117IbState g_p1117_ib;
// Byte-swapping copy that also says whether any value changed.
bool P1117CopySwapChanged(uint32_t* dst, const uint32_t* src, uint32_t count) {
  uint32_t diff = 0;
  for (uint32_t i = 0; i < count; ++i) {
    uint32_t raw;
    std::memcpy(&raw, src + i, sizeof(raw));
    const uint32_t v = rex::byte_swap(raw);
    diff |= v ^ dst[i];
    dst[i] = v;
  }
  return diff != 0;
}
}  // namespace

static bool NeedsFable2MorphReadback(const RegisterFile& regs, uint32_t title) {
  const auto draw = regs.Get<reg::VGT_DRAW_INITIATOR>();
  return REXCVAR_GET(fable2_morph_readback) && IsFable2MorphResolve(
      title, uint32_t(draw.prim_type), draw.num_indices,
      regs.values[XE_GPU_REG_RB_COPY_DEST_BASE],
      uint32_t(regs.Get<reg::RB_COPY_DEST_INFO>().copy_dest_format),
      regs.Get<reg::RB_COPY_CONTROL>().copy_src_select);
}

// Generated with `xb buildshaders`.
namespace shaders {
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_pwl_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_cs.h"
#include "../shaders/bytecode/d3d12_5_1/apply_gamma_table_fxaa_luma_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_cs.h"
#include "../shaders/bytecode/d3d12_5_1/fxaa_extreme_cs.h"
#include "../shaders/bytecode/d3d12_5_1/resolve_downscale_cs.h"
}  // namespace shaders

D3D12CommandProcessor::D3D12CommandProcessor(D3D12GraphicsSystem* graphics_system,
                                             system::KernelState* kernel_state)
    : CommandProcessor(graphics_system, kernel_state), deferred_command_list_(*this) {
  legacy_readback_memexport_cvar_name_ = "d3d12_readback_memexport";
}
D3D12CommandProcessor::~D3D12CommandProcessor() = default;

void D3D12CommandProcessor::UpdateDebugMarkersEnabled() {
  debug_markers_enabled_ = IsGpuDebugMarkersEnabled();
}

void D3D12CommandProcessor::PushDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.BeginDebugMarker(label);
}

void D3D12CommandProcessor::PopDebugMarker() {
  if (!debug_markers_enabled_) {
    return;
  }
  deferred_command_list_.EndDebugMarker();
}

void D3D12CommandProcessor::InsertDebugMarker(const char* format, ...) {
  if (!debug_markers_enabled_) {
    return;
  }
  char label[256];
  va_list args;
  va_start(args, format);
  vsnprintf(label, sizeof(label), format, args);
  va_end(args);
  deferred_command_list_.InsertDebugMarker(label);
}

void D3D12CommandProcessor::ClearCaches() {
  CommandProcessor::ClearCaches();
  InvalidateAllVertexBufferResidency();
  cache_clear_requested_ = true;
  g_p1117_translation_epoch.fetch_add(1, std::memory_order_relaxed);  // [PERF-1117]
  ++g_p1117_fetch_gen;                                                // [PERF-1117]
}

void D3D12CommandProcessor::InvalidateGpuMemory() {
  if (shared_memory_) {
    shared_memory_->InvalidateAllPages();
  }
}

void D3D12CommandProcessor::InvalidateAllVertexBufferResidency() {
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  for (VertexBufferState& state : vertex_buffer_states_) {
    state.address = UINT32_MAX;
    state.size = UINT32_MAX;
  }
}

void D3D12CommandProcessor::InvalidateVertexBufferResidency(uint32_t vfetch_index) {
  if (vfetch_index >= vertex_buffer_states_.size()) {
    return;
  }
  vertex_buffers_in_sync_[vfetch_index >> 6] &= ~(uint64_t(1) << (vfetch_index & 63));
}

void D3D12CommandProcessor::InvalidateVertexBufferResidencyRange(uint32_t first_vfetch,
                                                                 uint32_t last_vfetch) {
  if (first_vfetch > last_vfetch) {
    std::swap(first_vfetch, last_vfetch);
  }
  if (first_vfetch >= vertex_buffer_states_.size()) {
    return;
  }
  last_vfetch = std::min(last_vfetch, uint32_t(vertex_buffer_states_.size() - 1));
  for (uint32_t vfetch_index = first_vfetch; vfetch_index <= last_vfetch; ++vfetch_index) {
    InvalidateVertexBufferResidency(vfetch_index);
  }
}

void D3D12CommandProcessor::InitializeShaderStorage(const std::filesystem::path& cache_root,
                                                    uint32_t title_id, bool blocking) {
  CommandProcessor::InitializeShaderStorage(cache_root, title_id, blocking);
  g_p1117_translation_epoch.fetch_add(1, std::memory_order_relaxed);  // [PERF-1117]
  pipeline_cache_->InitializeShaderStorage(cache_root, title_id, blocking);
  g_p1117_translation_epoch.fetch_add(1, std::memory_order_relaxed);  // [PERF-1117]
}

bool D3D12CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(memory::RingBuffer* reader,
                                                               uint32_t packet, uint32_t count) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return CommandProcessor::ExecutePacketType3_EVENT_WRITE_ZPD(reader, packet, count);
  }

  const uint32_t kQueryFinished = rex::byte_swap(0xFFFFFEED);
  assert_true(count == 1);
  uint32_t initiator = reader->ReadAndSwap<uint32_t>();
  WriteRegister(XE_GPU_REG_VGT_EVENT_INITIATOR, initiator & 0x3F);

  uint32_t sample_count_addr = register_file_->values[XE_GPU_REG_RB_SAMPLE_COUNT_ADDR];
  auto* sample_counts =
      memory_->TranslatePhysical<xenos::xe_gpu_depth_sample_counts*>(sample_count_addr);
  if (!sample_counts) {
    DisableHostOcclusionQueries();
    return true;
  }

  auto write_fallback_result = [sample_counts, kQueryFinished]() -> bool {
    auto fake_sample_count = REXCVAR_GET(query_occlusion_fake_sample_count);
    if (fake_sample_count < 0) {
      return true;
    }
    bool is_end_via_z_pass =
        sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
    bool is_end_via_z_fail =
        sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
    std::memset(sample_counts, 0, sizeof(xenos::xe_gpu_depth_sample_counts));
    if (is_end_via_z_pass || is_end_via_z_fail) {
      sample_counts->ZPass_A = fake_sample_count;
      sample_counts->Total_A = fake_sample_count;
    }
    return true;
  };

  bool is_end_via_z_pass =
      sample_counts->ZPass_A == kQueryFinished || sample_counts->ZPass_B == kQueryFinished;
  bool is_end_via_z_fail =
      sample_counts->ZFail_A == kQueryFinished || sample_counts->ZFail_B == kQueryFinished;
  bool is_end = is_end_via_z_pass || is_end_via_z_fail;

  if (!is_end) {
    if (active_occlusion_query_.valid &&
        active_occlusion_query_.sample_count_address != sample_count_addr) {
      DisableHostOcclusionQueries();
      return write_fallback_result();
    }
    if (!BeginGuestOcclusionQuery(sample_count_addr)) {
      return write_fallback_result();
    }
    return true;
  }

  if (!active_occlusion_query_.valid ||
      active_occlusion_query_.sample_count_address != sample_count_addr) {
    DisableHostOcclusionQueries();
    return write_fallback_result();
  }

  if (!EndGuestOcclusionQuery(sample_count_addr, sample_counts)) {
    return write_fallback_result();
  }

  return true;
}

bool D3D12CommandProcessor::PushTransitionBarrier(ID3D12Resource* resource,
                                                  D3D12_RESOURCE_STATES old_state,
                                                  D3D12_RESOURCE_STATES new_state,
                                                  UINT subresource) {
  if (old_state == new_state) {
    return false;
  }
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Transition.pResource = resource;
  barrier.Transition.Subresource = subresource;
  barrier.Transition.StateBefore = old_state;
  barrier.Transition.StateAfter = new_state;
  barriers_.push_back(barrier);
  return true;
}

void D3D12CommandProcessor::PushAliasingBarrier(ID3D12Resource* old_resource,
                                                ID3D12Resource* new_resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_ALIASING;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.Aliasing.pResourceBefore = old_resource;
  barrier.Aliasing.pResourceAfter = new_resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::PushUAVBarrier(ID3D12Resource* resource) {
  D3D12_RESOURCE_BARRIER barrier;
  barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
  barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier.UAV.pResource = resource;
  barriers_.push_back(barrier);
}

void D3D12CommandProcessor::SubmitBarriers() {
  UINT barrier_count = UINT(barriers_.size());
  if (barrier_count != 0) {
    deferred_command_list_.D3DResourceBarrier(barrier_count, barriers_.data());
    barriers_.clear();
  }
}

ID3D12RootSignature* D3D12CommandProcessor::GetRootSignature(const DxbcShader* vertex_shader,
                                                             const DxbcShader* pixel_shader,
                                                             bool tessellated) {
  if (bindless_resources_used_) {
    return tessellated ? root_signature_bindless_ds_ : root_signature_bindless_vs_;
  }

  D3D12_SHADER_VISIBILITY vertex_visibility =
      tessellated ? D3D12_SHADER_VISIBILITY_DOMAIN : D3D12_SHADER_VISIBILITY_VERTEX;

  uint32_t texture_count_vertex =
      uint32_t(vertex_shader->GetTextureBindingsAfterTranslation().size());
  uint32_t sampler_count_vertex =
      uint32_t(vertex_shader->GetSamplerBindingsAfterTranslation().size());
  uint32_t texture_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetTextureBindingsAfterTranslation().size()) : 0;
  uint32_t sampler_count_pixel =
      pixel_shader ? uint32_t(pixel_shader->GetSamplerBindingsAfterTranslation().size()) : 0;

  // Better put the pixel texture/sampler in the lower bits probably because it
  // changes often.
  uint32_t index = 0;
  uint32_t index_offset = 0;
  index |= texture_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_pixel << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= texture_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxTextureBindingIndexBits;
  index |= sampler_count_vertex << index_offset;
  index_offset += D3D12Shader::kMaxSamplerBindingIndexBits;
  index |= uint32_t(vertex_visibility == D3D12_SHADER_VISIBILITY_DOMAIN) << index_offset;
  ++index_offset;
  assert_true(index_offset <= 32);

  // Try an existing root signature.
  auto it = root_signatures_bindful_.find(index);
  if (it != root_signatures_bindful_.end()) {
    return it->second;
  }

  // Create a new one.
  D3D12_ROOT_SIGNATURE_DESC desc;
  D3D12_ROOT_PARAMETER parameters[kRootParameter_Bindful_Count_Max];
  desc.NumParameters = kRootParameter_Bindful_Count_Base;
  desc.pParameters = parameters;
  desc.NumStaticSamplers = 0;
  desc.pStaticSamplers = nullptr;
  desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;

  // Base parameters.

  // Fetch constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FetchConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Vertex float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsVertex];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = vertex_visibility;
  }

  // Pixel float constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_FloatConstantsPixel];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  }

  // System constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_SystemConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Bool and loop constants.
  {
    auto& parameter = parameters[kRootParameter_Bindful_BoolLoopConstants];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    parameter.Descriptor.ShaderRegister =
        uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
    parameter.Descriptor.RegisterSpace = 0;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }

  // Shared memory and, if ROVs are used, EDRAM.
  D3D12_DESCRIPTOR_RANGE shared_memory_and_edram_ranges[3];
  {
    auto& parameter = parameters[kRootParameter_Bindful_SharedMemoryAndEdram];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 2;
    parameter.DescriptorTable.pDescriptorRanges = shared_memory_and_edram_ranges;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    shared_memory_and_edram_ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    shared_memory_and_edram_ranges[0].NumDescriptors = 1;
    shared_memory_and_edram_ranges[0].BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
    shared_memory_and_edram_ranges[0].RegisterSpace =
        uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    shared_memory_and_edram_ranges[0].OffsetInDescriptorsFromTableStart = 0;
    shared_memory_and_edram_ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    shared_memory_and_edram_ranges[1].NumDescriptors = 1;
    shared_memory_and_edram_ranges[1].BaseShaderRegister =
        UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
    shared_memory_and_edram_ranges[1].RegisterSpace = 0;
    shared_memory_and_edram_ranges[1].OffsetInDescriptorsFromTableStart = 1;
    if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
      ++parameter.DescriptorTable.NumDescriptorRanges;
      shared_memory_and_edram_ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
      shared_memory_and_edram_ranges[2].NumDescriptors = 1;
      shared_memory_and_edram_ranges[2].BaseShaderRegister =
          UINT(DxbcShaderTranslator::UAVRegister::kEdram);
      shared_memory_and_edram_ranges[2].RegisterSpace = 0;
      shared_memory_and_edram_ranges[2].OffsetInDescriptorsFromTableStart = 2;
    }
  }

  // Extra parameters.

  // Pixel textures.
  D3D12_DESCRIPTOR_RANGE range_textures_pixel;
  if (texture_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_textures_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_pixel.NumDescriptors = texture_count_pixel;
    range_textures_pixel.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_pixel.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Pixel samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_pixel;
  if (sampler_count_pixel > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_pixel;
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    range_samplers_pixel.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_pixel.NumDescriptors = sampler_count_pixel;
    range_samplers_pixel.BaseShaderRegister = 0;
    range_samplers_pixel.RegisterSpace = 0;
    range_samplers_pixel.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex textures.
  D3D12_DESCRIPTOR_RANGE range_textures_vertex;
  if (texture_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_textures_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_textures_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range_textures_vertex.NumDescriptors = texture_count_vertex;
    range_textures_vertex.BaseShaderRegister =
        uint32_t(DxbcShaderTranslator::SRVMainRegister::kBindfulTexturesStart);
    range_textures_vertex.RegisterSpace = uint32_t(DxbcShaderTranslator::SRVSpace::kMain);
    range_textures_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  // Vertex samplers.
  D3D12_DESCRIPTOR_RANGE range_samplers_vertex;
  if (sampler_count_vertex > 0) {
    auto& parameter = parameters[desc.NumParameters];
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable.NumDescriptorRanges = 1;
    parameter.DescriptorTable.pDescriptorRanges = &range_samplers_vertex;
    parameter.ShaderVisibility = vertex_visibility;
    range_samplers_vertex.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    range_samplers_vertex.NumDescriptors = sampler_count_vertex;
    range_samplers_vertex.BaseShaderRegister = 0;
    range_samplers_vertex.RegisterSpace = 0;
    range_samplers_vertex.OffsetInDescriptorsFromTableStart = 0;
    ++desc.NumParameters;
  }

  ID3D12RootSignature* root_signature =
      ui::d3d12::util::CreateRootSignature(GetD3D12Provider(), desc);
  if (root_signature == nullptr) {
    REXGPU_ERROR(
        "Failed to create a root signature with {} pixel textures, {} pixel "
        "samplers, {} vertex textures and {} vertex samplers",
        texture_count_pixel, sampler_count_pixel, texture_count_vertex, sampler_count_vertex);
    return nullptr;
  }
  root_signatures_bindful_.emplace(index, root_signature);
  return root_signature;
}

uint32_t D3D12CommandProcessor::GetRootBindfulExtraParameterIndices(
    const DxbcShader* vertex_shader, const DxbcShader* pixel_shader,
    RootBindfulExtraParameterIndices& indices_out) {
  uint32_t index = kRootParameter_Bindful_Count_Base;
  if (pixel_shader && !pixel_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_pixel = index++;
  } else {
    indices_out.textures_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (pixel_shader && !pixel_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_pixel = index++;
  } else {
    indices_out.samplers_pixel = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetTextureBindingsAfterTranslation().empty()) {
    indices_out.textures_vertex = index++;
  } else {
    indices_out.textures_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  if (!vertex_shader->GetSamplerBindingsAfterTranslation().empty()) {
    indices_out.samplers_vertex = index++;
  } else {
    indices_out.samplers_vertex = RootBindfulExtraParameterIndices::kUnavailable;
  }
  return index;
}

uint64_t D3D12CommandProcessor::RequestViewBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = view_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = view_bindful_heap_pool_->GetLastRequestHeap();
  if (view_bindful_heap_current_ != heap) {
    view_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetViewDescriptor(
      view_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

uint32_t D3D12CommandProcessor::RequestPersistentViewBindlessDescriptor() {
  assert_true(bindless_resources_used_);
  if (!view_bindless_heap_free_.empty()) {
    uint32_t descriptor_index = view_bindless_heap_free_.back();
    view_bindless_heap_free_.pop_back();
    return descriptor_index;
  }
  if (view_bindless_heap_allocated_ >= kViewBindlessHeapSize) {
    return UINT32_MAX;
  }
  return view_bindless_heap_allocated_++;
}

void D3D12CommandProcessor::ReleaseViewBindlessDescriptorImmediately(uint32_t descriptor_index) {
  assert_true(bindless_resources_used_);
  view_bindless_heap_free_.push_back(descriptor_index);
}

bool D3D12CommandProcessor::RequestOneUseSingleViewDescriptors(
    uint32_t count, ui::d3d12::util::DescriptorCpuGpuHandlePair* handles_out) {
  assert_true(submission_open_);
  if (!count) {
    return true;
  }
  assert_not_null(handles_out);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  if (bindless_resources_used_) {
    // Request separate bindless descriptors that will be freed when this
    // submission is completed by the GPU.
    if (count >
        kViewBindlessHeapSize - view_bindless_heap_allocated_ + view_bindless_heap_free_.size()) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      uint32_t descriptor_index;
      if (!view_bindless_heap_free_.empty()) {
        descriptor_index = view_bindless_heap_free_.back();
        view_bindless_heap_free_.pop_back();
      } else {
        descriptor_index = view_bindless_heap_allocated_++;
      }
      view_bindless_one_use_descriptors_.push_back(
          std::make_pair(descriptor_index, submission_current_));
      handles_out[i] = std::make_pair(
          provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, descriptor_index),
          provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, descriptor_index));
    }
  } else {
    // Request a range within the current heap for bindful resources path.
    D3D12_CPU_DESCRIPTOR_HANDLE cpu_handle_start;
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_start;
    if (RequestViewBindfulDescriptors(ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid, count,
                                      count, cpu_handle_start, gpu_handle_start) ==
        ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      return false;
    }
    for (uint32_t i = 0; i < count; ++i) {
      handles_out[i] = std::make_pair(provider.OffsetViewDescriptor(cpu_handle_start, i),
                                      provider.OffsetViewDescriptor(gpu_handle_start, i));
    }
  }
  return true;
}

ui::d3d12::util::DescriptorCpuGpuHandlePair D3D12CommandProcessor::GetSystemBindlessViewHandlePair(
    SystemBindlessView view) const {
  assert_true(bindless_resources_used_);
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  return std::make_pair(
      provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_, uint32_t(view)),
      provider.OffsetViewDescriptor(view_bindless_heap_gpu_start_, uint32_t(view)));
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetSharedMemoryUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kSharedMemoryR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kSharedMemoryR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessSRVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintSRV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintSRV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintSRV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintSRV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

ui::d3d12::util::DescriptorCpuGpuHandlePair
D3D12CommandProcessor::GetEdramUintPow2BindlessUAVHandlePair(
    uint32_t element_size_bytes_pow2) const {
  SystemBindlessView view;
  switch (element_size_bytes_pow2) {
    case 2:
      view = SystemBindlessView::kEdramR32UintUAV;
      break;
    case 3:
      view = SystemBindlessView::kEdramR32G32UintUAV;
      break;
    case 4:
      view = SystemBindlessView::kEdramR32G32B32A32UintUAV;
      break;
    default:
      assert_unhandled_case(element_size_bytes_pow2);
      view = SystemBindlessView::kEdramR32UintUAV;
  }
  return GetSystemBindlessViewHandlePair(view);
}

uint64_t D3D12CommandProcessor::RequestSamplerBindfulDescriptors(
    uint64_t previous_heap_index, uint32_t count_for_partial_update, uint32_t count_for_full_update,
    D3D12_CPU_DESCRIPTOR_HANDLE& cpu_handle_out, D3D12_GPU_DESCRIPTOR_HANDLE& gpu_handle_out) {
  assert_false(bindless_resources_used_);
  assert_true(submission_open_);
  uint32_t descriptor_index;
  uint64_t current_heap_index = sampler_bindful_heap_pool_->Request(
      frame_current_, previous_heap_index, count_for_partial_update, count_for_full_update,
      descriptor_index);
  if (current_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
    // There was an error.
    return ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
  }
  ID3D12DescriptorHeap* heap = sampler_bindful_heap_pool_->GetLastRequestHeap();
  if (sampler_bindful_heap_current_ != heap) {
    sampler_bindful_heap_current_ = heap;
    deferred_command_list_.SetDescriptorHeaps(view_bindful_heap_current_,
                                              sampler_bindful_heap_current_);
  }
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  cpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapCPUStart(), descriptor_index);
  gpu_handle_out = provider.OffsetSamplerDescriptor(
      sampler_bindful_heap_pool_->GetLastRequestHeapGPUStart(), descriptor_index);
  return current_heap_index;
}

ID3D12Resource* D3D12CommandProcessor::RequestScratchGPUBuffer(uint32_t size,
                                                               D3D12_RESOURCE_STATES state) {
  assert_true(submission_open_);
  assert_false(scratch_buffer_used_);
  if (!submission_open_ || scratch_buffer_used_ || size == 0) {
    return nullptr;
  }

  if (size <= scratch_buffer_size_) {
    PushTransitionBarrier(scratch_buffer_, scratch_buffer_state_, state);
    scratch_buffer_state_ = state;
    scratch_buffer_used_ = true;
    return scratch_buffer_;
  }

  size = rex::align(size, kScratchBufferSizeIncrement);

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size,
                                          D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
  ID3D12Resource* buffer;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             provider.GetHeapFlagCreateNotZeroed(), &buffer_desc,
                                             state, nullptr, IID_PPV_ARGS(&buffer)))) {
    REXGPU_ERROR("Failed to create a {} MB scratch GPU buffer", size >> 20);
    return nullptr;
  }
  if (scratch_buffer_ != nullptr) {
    resources_for_deletion_.emplace_back(submission_current_, scratch_buffer_);
  }
  scratch_buffer_ = buffer;
  scratch_buffer_size_ = size;
  scratch_buffer_state_ = state;
  scratch_buffer_used_ = true;
  return scratch_buffer_;
}

void D3D12CommandProcessor::ReleaseScratchGPUBuffer(ID3D12Resource* buffer,
                                                    D3D12_RESOURCE_STATES new_state) {
  assert_true(submission_open_);
  assert_true(scratch_buffer_used_);
  scratch_buffer_used_ = false;
  if (buffer == scratch_buffer_) {
    scratch_buffer_state_ = new_state;
  }
}

void D3D12CommandProcessor::SetExternalPipeline(ID3D12PipelineState* pipeline) {
  if (current_external_pipeline_ != pipeline) {
    current_external_pipeline_ = pipeline;
    current_guest_pipeline_ = nullptr;
    deferred_command_list_.D3DSetPipelineState(pipeline);
  }
}

void D3D12CommandProcessor::SetExternalGraphicsRootSignature(ID3D12RootSignature* root_signature) {
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }
  // Force-invalidate because setting a non-guest root signature.
  current_graphics_root_up_to_date_ = 0;
}

void D3D12CommandProcessor::SetViewport(const D3D12_VIEWPORT& viewport) {
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftX != viewport.TopLeftX;
  ff_viewport_update_needed_ |= ff_viewport_.TopLeftY != viewport.TopLeftY;
  ff_viewport_update_needed_ |= ff_viewport_.Width != viewport.Width;
  ff_viewport_update_needed_ |= ff_viewport_.Height != viewport.Height;
  ff_viewport_update_needed_ |= ff_viewport_.MinDepth != viewport.MinDepth;
  ff_viewport_update_needed_ |= ff_viewport_.MaxDepth != viewport.MaxDepth;
  if (ff_viewport_update_needed_) {
    ff_viewport_ = viewport;
    deferred_command_list_.RSSetViewport(ff_viewport_);
    ff_viewport_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetScissorRect(const D3D12_RECT& scissor_rect) {
  ff_scissor_update_needed_ |= ff_scissor_.left != scissor_rect.left;
  ff_scissor_update_needed_ |= ff_scissor_.top != scissor_rect.top;
  ff_scissor_update_needed_ |= ff_scissor_.right != scissor_rect.right;
  ff_scissor_update_needed_ |= ff_scissor_.bottom != scissor_rect.bottom;
  if (ff_scissor_update_needed_) {
    ff_scissor_ = scissor_rect;
    deferred_command_list_.RSSetScissorRect(ff_scissor_);
    ff_scissor_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetStencilReference(uint32_t stencil_ref) {
  ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
  if (ff_stencil_ref_update_needed_) {
    ff_stencil_ref_ = stencil_ref;
    deferred_command_list_.D3DOMSetStencilRef(stencil_ref);
    ff_stencil_ref_update_needed_ = false;
  }
}

void D3D12CommandProcessor::SetPrimitiveTopology(D3D12_PRIMITIVE_TOPOLOGY primitive_topology) {
  if (primitive_topology_ != primitive_topology) {
    primitive_topology_ = primitive_topology;
    deferred_command_list_.D3DIASetPrimitiveTopology(primitive_topology);
  }
}

std::string D3D12CommandProcessor::GetWindowTitleText() const {
  std::ostringstream title;
  title << "Direct3D 12";
  if (render_target_cache_) {
    // Rasterizer-ordered views are a feature very rarely used as of 2020 and
    // that faces adoption complications (outside of Direct3D - on Vulkan - at
    // least), but crucial to Xenia - raise awareness of its usage.
    // https://github.com/KhronosGroup/Vulkan-Ecosystem/issues/27#issuecomment-455712319
    // "In Xenia's title bar "D3D12 ROV" can be seen, which was a surprise, as I
    //  wasn't aware that Xenia D3D12 backend was using Raster Order Views
    //  feature" - oscarbg in that issue.
    switch (render_target_cache_->GetPath()) {
      case RenderTargetCache::Path::kHostRenderTargets:
        title << " - RTV/DSV";
        break;
      case RenderTargetCache::Path::kPixelShaderInterlock:
        title << " - ROV";
        break;
      default:
        break;
    }
    uint32_t draw_resolution_scale_x =
        texture_cache_ ? texture_cache_->draw_resolution_scale_x() : 1;
    uint32_t draw_resolution_scale_y =
        texture_cache_ ? texture_cache_->draw_resolution_scale_y() : 1;
    if (draw_resolution_scale_x > 1 || draw_resolution_scale_y > 1) {
      title << ' ' << draw_resolution_scale_x << 'x' << draw_resolution_scale_y;
    }
  }
  return title.str();
}

bool D3D12CommandProcessor::SetupContext() {
  if (!CommandProcessor::SetupContext()) {
    REXGPU_ERROR("Failed to initialize base command processor context");
    return false;
  }
  InvalidateAllVertexBufferResidency();
  UpdateDebugMarkersEnabled();

  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

  fence_completion_event_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
  if (fence_completion_event_ == nullptr) {
    REXGPU_ERROR("Failed to create the fence completion event");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&submission_fence_)))) {
    REXGPU_ERROR("Failed to create the submission fence");
    return false;
  }
  if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                 IID_PPV_ARGS(&queue_operations_since_submission_fence_)))) {
    REXGPU_ERROR(
        "Failed to create the fence for awaiting queue operations done since "
        "the latest submission");
    return false;
  }

  // Create the command list and one allocator because it's needed for a command
  // list.
  ID3D12CommandAllocator* command_allocator;
  if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                            IID_PPV_ARGS(&command_allocator)))) {
    REXGPU_ERROR("Failed to create a command allocator");
    return false;
  }
  command_allocator_writable_first_ = new CommandAllocator;
  command_allocator_writable_first_->command_allocator = command_allocator;
  command_allocator_writable_first_->last_usage_submission = 0;
  command_allocator_writable_first_->next = nullptr;
  command_allocator_writable_last_ = command_allocator_writable_first_;
  if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, command_allocator,
                                       nullptr, IID_PPV_ARGS(&command_list_)))) {
    REXGPU_ERROR("Failed to create the graphics command list");
    return false;
  }
  // Initially in open state, wait until a deferred command list submission.
  command_list_->Close();
  // Optional - added in Creators Update (SDK 10.0.15063.0).
  command_list_->QueryInterface(IID_PPV_ARGS(&command_list_1_));

  bindless_resources_used_ = REXCVAR_GET(d3d12_bindless) &&
                             provider.GetResourceBindingTier() >= D3D12_RESOURCE_BINDING_TIER_2;

  // Get the draw resolution scale for the render target cache and the texture
  // cache.
  uint32_t draw_resolution_scale_x, draw_resolution_scale_y;
  bool draw_resolution_scale_not_clamped =
      TextureCache::GetConfigDrawResolutionScale(draw_resolution_scale_x, draw_resolution_scale_y);
  if (!D3D12TextureCache::ClampDrawResolutionScaleToMaxSupported(
          draw_resolution_scale_x, draw_resolution_scale_y, provider)) {
    draw_resolution_scale_not_clamped = false;
  }
  if (!draw_resolution_scale_not_clamped) {
    REXGPU_WARN(
        "The requested draw resolution scale is not supported by the device or "
        "the emulator, reducing to {}x{}",
        draw_resolution_scale_x, draw_resolution_scale_y);
  }

  shared_memory_ = std::make_unique<D3D12SharedMemory>(*this, *memory_);
  if (!shared_memory_->Initialize()) {
    REXGPU_ERROR("Failed to initialize shared memory");
    return false;
  }

  // Initialize the render target cache before configuring binding - need to
  // know if using rasterizer-ordered views for the bindless root signature.
  render_target_cache_ = std::make_unique<D3D12RenderTargetCache>(
      *register_file_, *memory_, draw_resolution_scale_x, draw_resolution_scale_y, *this,
      bindless_resources_used_);
  if (!render_target_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the render target cache");
    return false;
  }

  // Initialize resource binding.
  constant_buffer_pool_ = std::make_unique<ui::d3d12::D3D12UploadBufferPool>(
      provider, std::max(ui::d3d12::D3D12UploadBufferPool::kDefaultPageSize,
                         sizeof(float) * 4 * D3D12_REQ_CONSTANT_BUFFER_ELEMENT_COUNT));
  if (bindless_resources_used_) {
    D3D12_DESCRIPTOR_HEAP_DESC view_bindless_heap_desc;
    view_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    view_bindless_heap_desc.NumDescriptors = kViewBindlessHeapSize;
    view_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    view_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&view_bindless_heap_desc,
                                            IID_PPV_ARGS(&view_bindless_heap_)))) {
      REXGPU_ERROR("Failed to create the bindless CBV/SRV/UAV descriptor heap");
      return false;
    }
    view_bindless_heap_cpu_start_ = view_bindless_heap_->GetCPUDescriptorHandleForHeapStart();
    view_bindless_heap_gpu_start_ = view_bindless_heap_->GetGPUDescriptorHandleForHeapStart();
    view_bindless_heap_allocated_ = uint32_t(SystemBindlessView::kCount);

    D3D12_DESCRIPTOR_HEAP_DESC sampler_bindless_heap_desc;
    sampler_bindless_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
    sampler_bindless_heap_desc.NumDescriptors = kSamplerHeapSize;
    sampler_bindless_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    sampler_bindless_heap_desc.NodeMask = 0;
    if (FAILED(device->CreateDescriptorHeap(&sampler_bindless_heap_desc,
                                            IID_PPV_ARGS(&sampler_bindless_heap_current_)))) {
      REXGPU_ERROR("Failed to create the bindless sampler descriptor heap");
      return false;
    }
    sampler_bindless_heap_cpu_start_ =
        sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_gpu_start_ =
        sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
    sampler_bindless_heap_allocated_ = 0;
  } else {
    view_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kViewBindfulHeapSize);
    sampler_bindful_heap_pool_ = std::make_unique<ui::d3d12::D3D12DescriptorHeapPool>(
        device, D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerHeapSize);
  }

  if (bindless_resources_used_) {
    // Global bindless resource root signatures.
    // No CBV or UAV descriptor ranges with any descriptors to be allocated
    // dynamically (via RequestPersistentViewBindlessDescriptor or
    // RequestOneUseSingleViewDescriptors) should be here, because they would
    // overlap the unbounded SRV range, which is not allowed on Nvidia Fermi!
    D3D12_ROOT_SIGNATURE_DESC root_signature_bindless_desc;
    D3D12_ROOT_PARAMETER
    root_parameters_bindless[kRootParameter_Bindless_Count];
    root_signature_bindless_desc.NumParameters = kRootParameter_Bindless_Count;
    root_signature_bindless_desc.pParameters = root_parameters_bindless;
    root_signature_bindless_desc.NumStaticSamplers = 0;
    root_signature_bindless_desc.pStaticSamplers = nullptr;
    root_signature_bindless_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
    // Fetch constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FetchConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFetchConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Vertex float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // Pixel float constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_FloatConstantsPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kFloatConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Pixel shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesPixel];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    // Vertex shader descriptor indices.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kDescriptorIndices);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_VERTEX;
    }
    // System constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SystemConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kSystemConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Bool and loop constants.
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_BoolLoopConstants];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
      parameter.Descriptor.ShaderRegister =
          uint32_t(DxbcShaderTranslator::CbufferRegister::kBoolLoopConstants);
      parameter.Descriptor.RegisterSpace = 0;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
    }
    // Shared memory SRV and UAV.
    D3D12_DESCRIPTOR_RANGE root_shared_memory_view_ranges[2];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SharedMemory];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      parameter.DescriptorTable.NumDescriptorRanges =
          uint32_t(rex::countof(root_shared_memory_view_ranges));
      parameter.DescriptorTable.pDescriptorRanges = root_shared_memory_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      {
        auto& range = root_shared_memory_view_ranges[0];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::SRVMainRegister::kSharedMemory);
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kMain);
        range.OffsetInDescriptorsFromTableStart = 0;
      }
      {
        auto& range = root_shared_memory_view_ranges[1];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kSharedMemory);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = 1;
      }
    }
    // Sampler heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_sampler_range;
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_SamplerHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 1;
      parameter.DescriptorTable.pDescriptorRanges = &root_bindless_sampler_range;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      root_bindless_sampler_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
      root_bindless_sampler_range.NumDescriptors = UINT_MAX;
      root_bindless_sampler_range.BaseShaderRegister = 0;
      root_bindless_sampler_range.RegisterSpace = 0;
      root_bindless_sampler_range.OffsetInDescriptorsFromTableStart = 0;
    }
    // View heap.
    D3D12_DESCRIPTOR_RANGE root_bindless_view_ranges[4];
    {
      auto& parameter = root_parameters_bindless[kRootParameter_Bindless_ViewHeap];
      parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
      // Will be appending.
      parameter.DescriptorTable.NumDescriptorRanges = 0;
      parameter.DescriptorTable.pDescriptorRanges = root_bindless_view_ranges;
      parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
      // EDRAM.
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock) {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
        range.NumDescriptors = 1;
        range.BaseShaderRegister = UINT(DxbcShaderTranslator::UAVRegister::kEdram);
        range.RegisterSpace = 0;
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kEdramR32UintUAV);
      }
      // Used UAV and SRV ranges must not overlap on Nvidia Fermi, so textures
      // have OffsetInDescriptorsFromTableStart after all static descriptors of
      // other types.
      // 2D array textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures2DArray);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // 3D textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTextures3D);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
      // Cube textures.
      {
        assert_true(parameter.DescriptorTable.NumDescriptorRanges <
                    rex::countof(root_bindless_view_ranges));
        auto& range = root_bindless_view_ranges[parameter.DescriptorTable.NumDescriptorRanges++];
        range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
        range.NumDescriptors = UINT_MAX;
        range.BaseShaderRegister = 0;
        range.RegisterSpace = UINT(DxbcShaderTranslator::SRVSpace::kBindlessTexturesCube);
        range.OffsetInDescriptorsFromTableStart = UINT(SystemBindlessView::kUnboundedSRVsStart);
      }
    }
    root_signature_bindless_vs_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_vs_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use without tessellation");
      return false;
    }
    root_parameters_bindless[kRootParameter_Bindless_FloatConstantsVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_parameters_bindless[kRootParameter_Bindless_DescriptorIndicesVertex].ShaderVisibility =
        D3D12_SHADER_VISIBILITY_DOMAIN;
    root_signature_bindless_ds_ =
        ui::d3d12::util::CreateRootSignature(provider, root_signature_bindless_desc);
    if (!root_signature_bindless_ds_) {
      REXGPU_ERROR(
          "Failed to create the global root signature for bindless resources, "
          "the version for use with tessellation");
      return false;
    }
  }

  primitive_processor_ =
      std::make_unique<D3D12PrimitiveProcessor>(*register_file_, *memory_, *shared_memory_, *this);
  if (!primitive_processor_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the geometric primitive processor");
    return false;
  }

  texture_cache_ =
      D3D12TextureCache::Create(*register_file_, *shared_memory_, draw_resolution_scale_x,
                                draw_resolution_scale_y, *this, bindless_resources_used_);
  if (!texture_cache_) {
    REXGPU_ERROR("Failed to initialize the texture cache");
    return false;
  }

  pipeline_cache_ = std::make_unique<PipelineCache>(
      *this, *register_file_, *render_target_cache_.get(), bindless_resources_used_);
  if (!pipeline_cache_->Initialize()) {
    REXGPU_ERROR("Failed to initialize the graphics pipeline cache");
    return false;
  }

  D3D12_HEAP_FLAGS heap_flag_create_not_zeroed = provider.GetHeapFlagCreateNotZeroed();

  // Create gamma ramp resources.
  gamma_ramp_256_entry_table_up_to_date_ = false;
  gamma_ramp_pwl_up_to_date_ = false;
  D3D12_RESOURCE_DESC gamma_ramp_buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(gamma_ramp_buffer_desc, (256 + 128 * 3) * 4,
                                          D3D12_RESOURCE_FLAG_NONE);
  // The first action will be uploading.
  gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesDefault,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             gamma_ramp_buffer_state_, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp buffer");
    return false;
  }
  // The upload buffer is frame-buffered.
  gamma_ramp_buffer_desc.Width *= kQueueFrames;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesUpload,
                                             heap_flag_create_not_zeroed, &gamma_ramp_buffer_desc,
                                             D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&gamma_ramp_upload_buffer_)))) {
    REXGPU_ERROR("Failed to create the gamma ramp upload buffer");
    return false;
  }
  if (FAILED(gamma_ramp_upload_buffer_->Map(
          0, nullptr, reinterpret_cast<void**>(&gamma_ramp_upload_buffer_mapping_)))) {
    REXGPU_ERROR("Failed to map the gamma ramp upload buffer");
    gamma_ramp_upload_buffer_mapping_ = nullptr;
    return false;
  }

  // Initialize compute pipelines for output with gamma ramp.
  D3D12_ROOT_PARAMETER
  apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_constants =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    apply_gamma_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    apply_gamma_root_parameter_constants.Constants.ShaderRegister = 0;
    apply_gamma_root_parameter_constants.Constants.RegisterSpace = 0;
    apply_gamma_root_parameter_constants.Constants.Num32BitValues =
        sizeof(ApplyGammaConstants) / sizeof(uint32_t);
    apply_gamma_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_dest;
  apply_gamma_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  apply_gamma_root_descriptor_range_dest.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_dest.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_dest.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_dest =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kDestination)];
    apply_gamma_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_dest.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_dest;
    apply_gamma_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_source;
  apply_gamma_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_source.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_source.BaseShaderRegister = 1;
  apply_gamma_root_descriptor_range_source.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_source =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kSource)];
    apply_gamma_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_source;
    apply_gamma_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE apply_gamma_root_descriptor_range_ramp;
  apply_gamma_root_descriptor_range_ramp.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  apply_gamma_root_descriptor_range_ramp.NumDescriptors = 1;
  apply_gamma_root_descriptor_range_ramp.BaseShaderRegister = 0;
  apply_gamma_root_descriptor_range_ramp.RegisterSpace = 0;
  apply_gamma_root_descriptor_range_ramp.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& apply_gamma_root_parameter_gamma_ramp =
        apply_gamma_root_parameters[UINT(ApplyGammaRootParameter::kRamp)];
    apply_gamma_root_parameter_gamma_ramp.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.NumDescriptorRanges = 1;
    apply_gamma_root_parameter_gamma_ramp.DescriptorTable.pDescriptorRanges =
        &apply_gamma_root_descriptor_range_ramp;
    apply_gamma_root_parameter_gamma_ramp.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC apply_gamma_root_signature_desc;
  apply_gamma_root_signature_desc.NumParameters = UINT(ApplyGammaRootParameter::kCount);
  apply_gamma_root_signature_desc.pParameters = apply_gamma_root_parameters;
  apply_gamma_root_signature_desc.NumStaticSamplers = 0;
  apply_gamma_root_signature_desc.pStaticSamplers = nullptr;
  apply_gamma_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(apply_gamma_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, apply_gamma_root_signature_desc);
  if (!apply_gamma_root_signature_) {
    REXGPU_ERROR("Failed to create the gamma ramp application root signature");
    return false;
  }
  *(apply_gamma_table_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_table_cs, sizeof(shaders::apply_gamma_table_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline");
    return false;
  }
  *(apply_gamma_table_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_table_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_table_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_table_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the 256-entry table gamma ramp application compute "
        "pipeline with perceptual luma output");
    return false;
  }
  *(apply_gamma_pwl_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::apply_gamma_pwl_cs, sizeof(shaders::apply_gamma_pwl_cs),
      apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_pipeline_) {
    REXGPU_ERROR("Failed to create the PWL gamma ramp application compute pipeline");
    return false;
  }
  *(apply_gamma_pwl_fxaa_luma_pipeline_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateComputePipeline(device, shaders::apply_gamma_pwl_fxaa_luma_cs,
                                             sizeof(shaders::apply_gamma_pwl_fxaa_luma_cs),
                                             apply_gamma_root_signature_.Get());
  if (!apply_gamma_pwl_fxaa_luma_pipeline_) {
    REXGPU_ERROR(
        "Failed to create the PWL gamma ramp application compute pipeline with "
        "perceptual luma output");
    return false;
  }

  // Initialize compute pipelines for post-processing anti-aliasing.
  D3D12_ROOT_PARAMETER fxaa_root_parameters[UINT(FxaaRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_constants =
        fxaa_root_parameters[UINT(ApplyGammaRootParameter::kConstants)];
    fxaa_root_parameter_constants.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    fxaa_root_parameter_constants.Constants.ShaderRegister = 0;
    fxaa_root_parameter_constants.Constants.RegisterSpace = 0;
    fxaa_root_parameter_constants.Constants.Num32BitValues =
        sizeof(FxaaConstants) / sizeof(uint32_t);
    fxaa_root_parameter_constants.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_dest;
  fxaa_root_descriptor_range_dest.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  fxaa_root_descriptor_range_dest.NumDescriptors = 1;
  fxaa_root_descriptor_range_dest.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_dest.RegisterSpace = 0;
  fxaa_root_descriptor_range_dest.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_dest =
        fxaa_root_parameters[UINT(FxaaRootParameter::kDestination)];
    fxaa_root_parameter_dest.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_dest.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_dest.DescriptorTable.pDescriptorRanges = &fxaa_root_descriptor_range_dest;
    fxaa_root_parameter_dest.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE fxaa_root_descriptor_range_source;
  fxaa_root_descriptor_range_source.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  fxaa_root_descriptor_range_source.NumDescriptors = 1;
  fxaa_root_descriptor_range_source.BaseShaderRegister = 0;
  fxaa_root_descriptor_range_source.RegisterSpace = 0;
  fxaa_root_descriptor_range_source.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& fxaa_root_parameter_source =
        fxaa_root_parameters[UINT(FxaaRootParameter::kSource)];
    fxaa_root_parameter_source.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    fxaa_root_parameter_source.DescriptorTable.NumDescriptorRanges = 1;
    fxaa_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &fxaa_root_descriptor_range_source;
    fxaa_root_parameter_source.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_STATIC_SAMPLER_DESC fxaa_root_sampler;
  fxaa_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  fxaa_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  fxaa_root_sampler.MipLODBias = 0.0f;
  fxaa_root_sampler.MaxAnisotropy = 1;
  fxaa_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  fxaa_root_sampler.BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  fxaa_root_sampler.MinLOD = 0.0f;
  fxaa_root_sampler.MaxLOD = 0.0f;
  fxaa_root_sampler.ShaderRegister = 0;
  fxaa_root_sampler.RegisterSpace = 0;
  fxaa_root_sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_ROOT_SIGNATURE_DESC fxaa_root_signature_desc;
  fxaa_root_signature_desc.NumParameters = UINT(FxaaRootParameter::kCount);
  fxaa_root_signature_desc.pParameters = fxaa_root_parameters;
  fxaa_root_signature_desc.NumStaticSamplers = 1;
  fxaa_root_signature_desc.pStaticSamplers = &fxaa_root_sampler;
  fxaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(fxaa_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, fxaa_root_signature_desc);
  if (!fxaa_root_signature_) {
    REXGPU_ERROR("Failed to create the FXAA root signature");
    return false;
  }
  *(fxaa_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_cs, sizeof(shaders::fxaa_cs), fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the FXAA compute pipeline");
    return false;
  }
  *(fxaa_extreme_pipeline_.ReleaseAndGetAddressOf()) = ui::d3d12::util::CreateComputePipeline(
      device, shaders::fxaa_extreme_cs, sizeof(shaders::fxaa_extreme_cs),
      fxaa_root_signature_.Get());
  if (!fxaa_pipeline_) {
    REXGPU_ERROR("Failed to create the extreme-quality FXAA compute pipeline");
    return false;
  }

  // Resolve downscale compute pipeline for scaled readback resolve.
  D3D12_ROOT_PARAMETER
  resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kCount)];
  {
    D3D12_ROOT_PARAMETER& constants_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kConstants)];
    constants_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    constants_parameter.Constants.ShaderRegister = 0;
    constants_parameter.Constants.RegisterSpace = 0;
    constants_parameter.Constants.Num32BitValues =
        sizeof(ResolveDownscaleConstants) / sizeof(uint32_t);
    constants_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_source_range;
  resolve_downscale_source_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  resolve_downscale_source_range.NumDescriptors = 1;
  resolve_downscale_source_range.BaseShaderRegister = 0;
  resolve_downscale_source_range.RegisterSpace = 0;
  resolve_downscale_source_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& source_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kSource)];
    source_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    source_parameter.DescriptorTable.NumDescriptorRanges = 1;
    source_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_source_range;
    source_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_DESCRIPTOR_RANGE resolve_downscale_destination_range;
  resolve_downscale_destination_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
  resolve_downscale_destination_range.NumDescriptors = 1;
  resolve_downscale_destination_range.BaseShaderRegister = 0;
  resolve_downscale_destination_range.RegisterSpace = 0;
  resolve_downscale_destination_range.OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& destination_parameter =
        resolve_downscale_root_parameters[UINT(ResolveDownscaleRootParameter::kDestination)];
    destination_parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    destination_parameter.DescriptorTable.NumDescriptorRanges = 1;
    destination_parameter.DescriptorTable.pDescriptorRanges = &resolve_downscale_destination_range;
    destination_parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  }
  D3D12_ROOT_SIGNATURE_DESC resolve_downscale_root_signature_desc;
  resolve_downscale_root_signature_desc.NumParameters = UINT(ResolveDownscaleRootParameter::kCount);
  resolve_downscale_root_signature_desc.pParameters = resolve_downscale_root_parameters;
  resolve_downscale_root_signature_desc.NumStaticSamplers = 0;
  resolve_downscale_root_signature_desc.pStaticSamplers = nullptr;
  resolve_downscale_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  *(resolve_downscale_root_signature_.ReleaseAndGetAddressOf()) =
      ui::d3d12::util::CreateRootSignature(provider, resolve_downscale_root_signature_desc);
  if (resolve_downscale_root_signature_) {
    *(resolve_downscale_pipeline_.ReleaseAndGetAddressOf()) =
        ui::d3d12::util::CreateComputePipeline(device, shaders::resolve_downscale_cs,
                                               sizeof(shaders::resolve_downscale_cs),
                                               resolve_downscale_root_signature_.Get());
  }
  if (!resolve_downscale_root_signature_ || !resolve_downscale_pipeline_) {
    resolve_downscale_pipeline_.Reset();
    resolve_downscale_root_signature_.Reset();
    REXGPU_WARN("Failed to initialize D3D12 resolve-downscale readback pipeline");
  }

  if (bindless_resources_used_) {
    // Create the system bindless descriptors once all resources are
    // initialized.
    // kNullRawSRV.
    ui::d3d12::util::CreateBufferRawSRV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawSRV)),
        nullptr, 0);
    // kNullRawUAV.
    ui::d3d12::util::CreateBufferRawUAV(
        device,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullRawUAV)),
        nullptr, 0);
    // kNullTexture2DArray.
    D3D12_SHADER_RESOURCE_VIEW_DESC null_srv_desc;
    null_srv_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    null_srv_desc.Shader4ComponentMapping = D3D12_ENCODE_SHADER_4_COMPONENT_MAPPING(
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0,
        D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0, D3D12_SHADER_COMPONENT_MAPPING_FORCE_VALUE_0);
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
    null_srv_desc.Texture2DArray.MostDetailedMip = 0;
    null_srv_desc.Texture2DArray.MipLevels = 1;
    null_srv_desc.Texture2DArray.FirstArraySlice = 0;
    null_srv_desc.Texture2DArray.ArraySize = 1;
    null_srv_desc.Texture2DArray.PlaneSlice = 0;
    null_srv_desc.Texture2DArray.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture2DArray)));
    // kNullTexture3D.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
    null_srv_desc.Texture3D.MostDetailedMip = 0;
    null_srv_desc.Texture3D.MipLevels = 1;
    null_srv_desc.Texture3D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTexture3D)));
    // kNullTextureCube.
    null_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
    null_srv_desc.TextureCube.MostDetailedMip = 0;
    null_srv_desc.TextureCube.MipLevels = 1;
    null_srv_desc.TextureCube.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        nullptr, &null_srv_desc,
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kNullTextureCube)));
    // kSharedMemoryRawSRV.
    shared_memory_->WriteRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawSRV)));
    // kSharedMemoryR32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintSRV)),
        2);
    // kSharedMemoryR32G32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintSRV)),
        3);
    // kSharedMemoryR32G32B32A32UintSRV.
    shared_memory_->WriteUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintSRV)),
        4);
    // kSharedMemoryRawUAV.
    shared_memory_->WriteRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kSharedMemoryRawUAV)));
    // kSharedMemoryR32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32UintUAV)),
        2);
    // kSharedMemoryR32G32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kSharedMemoryR32G32UintUAV)),
        3);
    // kSharedMemoryR32G32B32A32UintUAV.
    shared_memory_->WriteUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(
            view_bindless_heap_cpu_start_,
            uint32_t(SystemBindlessView::kSharedMemoryR32G32B32A32UintUAV)),
        4);
    // kEdramRawSRV.
    render_target_cache_->WriteEdramRawSRVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawSRV)));
    // kEdramR32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintSRV)),
        2);
    // kEdramR32G32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintSRV)),
        3);
    // kEdramR32G32B32A32UintSRV.
    render_target_cache_->WriteEdramUintPow2SRVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintSRV)),
        4);
    // kEdramRawUAV.
    render_target_cache_->WriteEdramRawUAVDescriptor(provider.OffsetViewDescriptor(
        view_bindless_heap_cpu_start_, uint32_t(SystemBindlessView::kEdramRawUAV)));
    // kEdramR32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32UintUAV)),
        2);
    // kEdramR32G32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32UintUAV)),
        3);
    // kEdramR32G32B32A32UintUAV.
    render_target_cache_->WriteEdramUintPow2UAVDescriptor(
        provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                      uint32_t(SystemBindlessView::kEdramR32G32B32A32UintUAV)),
        4);
    // kGammaRampTableSRV.
    WriteGammaRampSRV(
        false, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                             uint32_t(SystemBindlessView::kGammaRampTableSRV)));
    // kGammaRampPWLSRV.
    WriteGammaRampSRV(
        true, provider.OffsetViewDescriptor(view_bindless_heap_cpu_start_,
                                            uint32_t(SystemBindlessView::kGammaRampPWLSRV)));
  }

  occlusion_query_resources_available_ = InitializeOcclusionQueryResources();

  // Just not to expose uninitialized memory.
  std::memset(&system_constants_, 0, sizeof(system_constants_));

  return true;
}

void D3D12CommandProcessor::ShutdownContext() {
  AwaitAllQueueOperationsCompletion();
  Fable2ReplayStop();  // [PERF-1116]
  InvalidateAllVertexBufferResidency();
  ShutdownOcclusionQueryResources();

  ui::d3d12::util::ReleaseAndNull(readback_buffer_);
  readback_buffer_size_ = 0;
  for (auto& resolve_readback_pair : readback_buffers_) {
    auto& readback = resolve_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  readback_buffers_.clear();
  for (auto& memexport_readback_pair : memexport_readback_buffers_) {
    auto& readback = memexport_readback_pair.second;
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
          readback.mapped_data[i] = nullptr;
        }
        readback.buffers[i]->Release();
        readback.buffers[i] = nullptr;
      }
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
  }
  memexport_readback_buffers_.clear();

  ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
  scratch_buffer_size_ = 0;
  resolve_downscale_buffer_size_ = 0;
  resolve_downscale_buffer_.Reset();

  for (const std::pair<uint64_t, ID3D12Resource*>& resource_for_deletion :
       resources_for_deletion_) {
    resource_for_deletion.second->Release();
  }
  resources_for_deletion_.clear();

  fxaa_source_texture_submission_ = 0;
  fxaa_source_texture_.Reset();

  fxaa_extreme_pipeline_.Reset();
  fxaa_pipeline_.Reset();
  fxaa_root_signature_.Reset();
  resolve_downscale_pipeline_.Reset();
  resolve_downscale_root_signature_.Reset();

  apply_gamma_pwl_fxaa_luma_pipeline_.Reset();
  apply_gamma_pwl_pipeline_.Reset();
  apply_gamma_table_fxaa_luma_pipeline_.Reset();
  apply_gamma_table_pipeline_.Reset();
  apply_gamma_root_signature_.Reset();

  // Unmapping will be done implicitly by the destruction.
  gamma_ramp_upload_buffer_mapping_ = nullptr;
  gamma_ramp_upload_buffer_.Reset();
  gamma_ramp_buffer_.Reset();

  texture_cache_.reset();

  pipeline_cache_.reset();

  primitive_processor_.reset();

  // Shut down binding - bindless descriptors may be owned by subsystems like
  // the texture cache.

  // Root signatures are used by pipelines, thus freed after the pipelines.
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_ds_);
  ui::d3d12::util::ReleaseAndNull(root_signature_bindless_vs_);
  for (auto it : root_signatures_bindful_) {
    it.second->Release();
  }
  root_signatures_bindful_.clear();

  if (bindless_resources_used_) {
    texture_cache_bindless_sampler_map_.clear();
    for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
      sampler_bindless_heap_overflowed.first->Release();
    }
    sampler_bindless_heaps_overflowed_.clear();
    sampler_bindless_heap_allocated_ = 0;
    ui::d3d12::util::ReleaseAndNull(sampler_bindless_heap_current_);
    view_bindless_one_use_descriptors_.clear();
    view_bindless_heap_free_.clear();
    ui::d3d12::util::ReleaseAndNull(view_bindless_heap_);
  } else {
    sampler_bindful_heap_pool_.reset();
    view_bindful_heap_pool_.reset();
  }
  constant_buffer_pool_.reset();

  render_target_cache_.reset();

  shared_memory_.reset();

  deferred_command_list_.Reset();
  ui::d3d12::util::ReleaseAndNull(command_list_1_);
  ui::d3d12::util::ReleaseAndNull(command_list_);
  ClearCommandAllocatorCache();

  frame_open_ = false;
  frame_current_ = 1;
  frame_completed_ = 0;
  std::memset(closed_frame_submissions_, 0, sizeof(closed_frame_submissions_));

  // First release the fences since they may reference fence_completion_event_.

  queue_operations_done_since_submission_signal_ = false;
  queue_operations_since_submission_fence_last_ = 0;
  ui::d3d12::util::ReleaseAndNull(queue_operations_since_submission_fence_);

  ui::d3d12::util::ReleaseAndNull(submission_fence_);
  submission_open_ = false;
  submission_current_ = 1;
  submission_completed_ = 0;

  if (fence_completion_event_) {
    CloseHandle(fence_completion_event_);
    fence_completion_event_ = nullptr;
  }

  device_removed_ = false;

  CommandProcessor::ShutdownContext();
}

void D3D12CommandProcessor::WriteRegister(uint32_t index, uint32_t value) {
  CommandProcessor::WriteRegister(index, value);

  if (index >= XE_GPU_REG_SHADER_CONSTANT_000_X && index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    if (frame_open_) {
      uint32_t float_constant_index = (index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (float_constant_index >= 256) {
        float_constant_index -= 256;
        if (current_float_constant_map_pixel_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      } else {
        if (current_float_constant_map_vertex_[float_constant_index >> 6] &
            (1ull << (float_constant_index & 63))) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
    }
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    cbuffer_binding_bool_loop_.up_to_date = false;
  } else if (index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
             index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    ++g_p1117_fetch_gen;  // [PERF-1117]
    cbuffer_binding_fetch_.up_to_date = false;
    if (texture_cache_ != nullptr) {
      texture_cache_->TextureFetchConstantWritten((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) /
                                                  6);
    }
    InvalidateVertexBufferResidency((index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0) / 2);
  }
}

void D3D12CommandProcessor::WriteRegistersFromMem(uint32_t start_index, uint32_t* base,
                                                  uint32_t num_registers) {
  if (!num_registers) {
    return;
  }
  uint32_t end_index = start_index + num_registers - 1;

  auto range_has_any_constant_usage = [](const uint64_t* usage_map, uint32_t first_constant,
                                         uint32_t last_constant) -> bool {
    if (first_constant > last_constant) {
      return false;
    }
    uint32_t first_word = first_constant >> 6;
    uint32_t last_word = last_constant >> 6;
    uint32_t first_bit = first_constant & 63;
    uint32_t last_bit = last_constant & 63;
    if (first_word == last_word) {
      uint32_t bit_count = last_bit - first_bit + 1;
      uint64_t mask = bit_count == 64 ? UINT64_MAX : ((UINT64_C(1) << bit_count) - 1) << first_bit;
      return (usage_map[first_word] & mask) != 0;
    }
    if (usage_map[first_word] & (UINT64_MAX << first_bit)) {
      return true;
    }
    for (uint32_t word = first_word + 1; word < last_word; ++word) {
      if (usage_map[word]) {
        return true;
      }
    }
    uint64_t last_mask = last_bit == 63 ? UINT64_MAX : ((UINT64_C(1) << (last_bit + 1)) - 1);
    return (usage_map[last_word] & last_mask) != 0;
  };

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_000_X &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_511_W) {
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    if (frame_open_) {
      uint32_t first_float_constant = (start_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      uint32_t last_float_constant = (end_index - XE_GPU_REG_SHADER_CONSTANT_000_X) >> 2;
      if (first_float_constant < 256) {
        uint32_t last_vertex_constant = std::min(last_float_constant, 255u);
        if (range_has_any_constant_usage(current_float_constant_map_vertex_, first_float_constant,
                                         last_vertex_constant)) {
          cbuffer_binding_float_vertex_.up_to_date = false;
        }
      }
      if (last_float_constant >= 256) {
        uint32_t first_pixel_constant =
            first_float_constant >= 256 ? first_float_constant - 256 : 0;
        uint32_t last_pixel_constant = last_float_constant - 256;
        if (range_has_any_constant_usage(current_float_constant_map_pixel_, first_pixel_constant,
                                         last_pixel_constant)) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_LOOP_31) {
    // [PERF-1117]: unchanged values leave the constant buffer as it is.
    if (P1117().const_cmp) {
      if (P1117CopySwapChanged(register_file_->values + start_index, base, num_registers)) {
        cbuffer_binding_bool_loop_.up_to_date = false;
      }
      return;
    }
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_bool_loop_.up_to_date = false;
    return;
  }

  if (start_index >= XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 &&
      end_index <= XE_GPU_REG_SHADER_CONSTANT_FETCH_31_5) {
    if (P1117().const_cmp) {
      // [PERF-1117]: per dword, only the texture slots (6 dwords) and vertex
      // fetch constants (2 dwords) whose value changed are invalidated.
      // (Vertex buffers with an unchanged fetch constant would be found "in
      // sync" by address/size in IssueDraw anyway.)
      uint32_t* dst = register_file_->values + start_index;
      const uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
      uint32_t changed_textures = 0;
      uint64_t changed_vfetch[2] = {};
      for (uint32_t i = 0; i < num_registers; ++i) {
        uint32_t raw;
        std::memcpy(&raw, base + i, sizeof(raw));
        const uint32_t v = rex::byte_swap(raw);
        if (v != dst[i]) {
          dst[i] = v;
          const uint32_t d = first_fetch_dword + i;
          changed_textures |= uint32_t(1) << (d / 6);
          changed_vfetch[(d / 2) >> 6] |= uint64_t(1) << ((d / 2) & 63);
        }
      }
      if (changed_textures) {
        ++g_p1117_fetch_gen;
        cbuffer_binding_fetch_.up_to_date = false;
        if (texture_cache_) {
          uint32_t slots = changed_textures;
          while (slots) {
            const uint32_t slot = uint32_t(std::countr_zero(slots));
            slots &= slots - 1;
            texture_cache_->TextureFetchConstantWritten(slot);
          }
        }
        for (uint32_t w = 0; w < 2; ++w) {
          uint64_t bits = changed_vfetch[w];
          while (bits) {
            const uint32_t b = uint32_t(std::countr_zero(bits));
            bits &= bits - 1;
            InvalidateVertexBufferResidency(w * 64 + b);
          }
        }
      }
      return;
    }
    ++g_p1117_fetch_gen;
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    cbuffer_binding_fetch_.up_to_date = false;
    uint32_t first_fetch_dword = start_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    uint32_t last_fetch_dword = end_index - XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0;
    if (texture_cache_) {
      texture_cache_->TextureFetchConstantsWritten(first_fetch_dword / 6, last_fetch_dword / 6);
    }
    InvalidateVertexBufferResidencyRange(first_fetch_dword / 2, last_fetch_dword / 2);
    return;
  }

  // [PERF-1117]: context registers 0x2000-0x3FFF have no side effects in
  // WriteRegister (only scratch, COHER_STATUS_HOST and the DC_LUT gamma ramp
  // registers below 0x2000 and the constants from 0x4000 have) - store them with
  // one byte-swapping copy instead of two virtual calls per register.
  if (start_index >= 0x2000 && end_index < XE_GPU_REG_SHADER_CONSTANT_000_X &&
      P1117().regs_fast) {
    static_assert(XE_GPU_REG_SHADER_CONSTANT_000_X == 0x4000, "context register range");
    if (P1117Sample()) {
      uint32_t before[64];
      const uint32_t n = std::min(num_registers, uint32_t(64));
      std::memcpy(before, register_file_->values + start_index, n * sizeof(uint32_t));
      CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
      uint32_t slow[64];
      std::memcpy(slow, register_file_->values + start_index, n * sizeof(uint32_t));
      std::memcpy(register_file_->values + start_index, before, n * sizeof(uint32_t));
      memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
      P1117VerifyNote(0, std::memcmp(slow, register_file_->values + start_index,
                                     n * sizeof(uint32_t)) != 0);
      return;
    }
    memory::copy_and_swap(register_file_->values + start_index, base, num_registers);
    return;
  }

  CommandProcessor::WriteRegistersFromMem(start_index, base, num_registers);
}

void D3D12CommandProcessor::OnGammaRamp256EntryTableValueWritten() {
  gamma_ramp_256_entry_table_up_to_date_ = false;
}

void D3D12CommandProcessor::OnGammaRampPWLValueWritten() {
  gamma_ramp_pwl_up_to_date_ = false;
}

// Round 9 (2026-09-16, diagnostic-only): completes the declaration in
// d3d12/command_processor.h -- logs, once per frame, how many of this
// frame's draws (accumulated into draw_dedup_signatures_ by the
// gpu_draw_dedup_analysis block in IssueDraw) shared an exact {pipeline,
// vertex-fetch, texture-fetch} signature with another draw earlier in the
// same frame, then clears the vector for the next frame. Purely
// informational: does not change what is drawn or how, and the vector is
// only ever non-empty when gpu_draw_dedup_analysis is on. See
// native/performance-investigation-phase1.md, Optimization Round 9 /
// architectural investigation candidate A.
namespace {
// Round 10 (2026-09-18): shared helper -- sorts a signature vector in
// place and returns {total, unique, duplicate} counts. Used three times
// (combined/pipeline-only/texture-only) by LogDrawDedupSummary below.
struct DedupCounts {
  size_t total = 0;
  size_t unique_count = 0;
  size_t duplicate_count = 0;
  double duplicate_percent = 0.0;
};
DedupCounts SummarizeDedupVector(std::vector<uint64_t>& sigs) {
  DedupCounts result;
  result.total = sigs.size();
  if (result.total == 0) {
    return result;
  }
  std::sort(sigs.begin(), sigs.end());
  result.unique_count =
      static_cast<size_t>(std::unique(sigs.begin(), sigs.end()) - sigs.begin());
  result.duplicate_count = result.total - result.unique_count;
  result.duplicate_percent =
      100.0 * double(result.duplicate_count) / double(result.total);
  return result;
}
}  // namespace

void D3D12CommandProcessor::LogDrawDedupSummary() {
  if (draw_dedup_signatures_.empty()) {
    // draw_dedup_pipeline_signatures_/draw_dedup_texture_signatures_ are
    // always the same size as draw_dedup_signatures_ (pushed together,
    // every iteration, in IssueDraw) -- checking the one is sufficient.
    return;
  }
  const DedupCounts combined = SummarizeDedupVector(draw_dedup_signatures_);
  const DedupCounts pipeline_only = SummarizeDedupVector(draw_dedup_pipeline_signatures_);
  const DedupCounts texture_only = SummarizeDedupVector(draw_dedup_texture_signatures_);
  // Round 13 (2026-09-18): literal_dup is a SUBSET of combined's duplicates
  // by construction (literal_sig extends combined's sig, never diverges
  // from it independently), so literal_dup.duplicate_count <=
  // combined.duplicate_count always -- the gap between the two is the
  // "same mesh/material, different position" instancing-candidate
  // population. See native/performance-investigation-phase4.md, Round 13.
  const DedupCounts literal_dup = SummarizeDedupVector(draw_dedup_literal_signatures_);
  const double literal_of_full_state_percent =
      combined.duplicate_count > 0
          ? 100.0 * double(literal_dup.duplicate_count) / double(combined.duplicate_count)
          : 0.0;
  REXGPU_INFO(
      "Round 9/10 draw-dedup analysis: {} draws this frame -- full-state "
      "(pipeline+vertex-fetch+texture+index): {} unique, {} ({:.1f}%) exact "
      "duplicates | pipeline-only (same PSO): {} unique, {} ({:.1f}%) "
      "duplicates | texture-only (same bound texture set): {} unique, {} "
      "({:.1f}%) duplicates -- see native/performance-investigation-phase4.md "
      "for what each number implies about which fast-path, if any, is worth "
      "building",
      combined.total, combined.unique_count, combined.duplicate_count,
      combined.duplicate_percent, pipeline_only.unique_count, pipeline_only.duplicate_count,
      pipeline_only.duplicate_percent, texture_only.unique_count, texture_only.duplicate_count,
      texture_only.duplicate_percent);
  REXGPU_INFO(
      "Round 13 literal-duplicate analysis: {} unique, {} ({:.1f}%) draws "
      "byte-identical including shader float constants (world transform "
      "etc.) -- of this frame's {} full-state duplicates, {:.1f}% are also "
      "literal duplicates (same object drawn twice, a pure waste -- "
      "skippable with no instancing needed) and the remaining {:.1f}% share "
      "mesh/material but NOT float constants (same object at a different "
      "position -- a real GPU-instancing candidate, not a free repeat)",
      literal_dup.unique_count, literal_dup.duplicate_count, literal_dup.duplicate_percent,
      combined.duplicate_count, literal_of_full_state_percent,
      100.0 - literal_of_full_state_percent);
  // Round 11a (2026-09-18, correctness fix -- see
  // native/performance-investigation-phase4.md, "Round 11a"): this used to
  // publish through rex::perf::SetCounter/IncrementCounter, the same
  // cross-DLL registry kFps/kFrameTimeUs use. That was unsafe for any
  // CounterId this project adds -- rex::perf's actual storage
  // (rexcore/counter.cpp) is only ever the prebuilt SDK rexruntime(rd).dll,
  // never rebuilt locally, so a new enum value indexes past that binary's
  // already-compiled, unchecked g_counters array. Reverted to plain local
  // members (real_pipeline_switches_ etc., command_processor.h) that never
  // leave this DLL; REXGPU_INFO output below is unchanged.
  const int64_t real_pipeline_switches = real_pipeline_switches_;
  const int64_t real_texture_barriers = real_texture_barriers_pushed_;
  const int64_t texture_slots_considered = texture_slots_considered_;
  real_pipeline_switches_ = 0;
  real_texture_barriers_pushed_ = 0;
  texture_slots_considered_ = 0;
  const double real_pipeline_switch_percent =
      combined.total > 0
          ? 100.0 * double(real_pipeline_switches) / double(combined.total)
          : 0.0;
  const double real_texture_barrier_percent =
      texture_slots_considered > 0
          ? 100.0 * double(real_texture_barriers) / double(texture_slots_considered)
          : 0.0;
  REXGPU_INFO(
      "Round 11 real-switch analysis: pipeline: {} real SetPipelineStateHandle "
      "calls / {} draws ({:.1f}%) -- compare to pipeline-only duplication "
      "above to see how much of that duplication is already free | texture: "
      "{} real transition barriers / {} slots considered ({:.1f}%) -- compare "
      "to texture-only duplication above the same way",
      real_pipeline_switches, combined.total, real_pipeline_switch_percent,
      real_texture_barriers, texture_slots_considered, real_texture_barrier_percent);

  draw_dedup_signatures_.clear();
  draw_dedup_pipeline_signatures_.clear();
  draw_dedup_texture_signatures_.clear();
  draw_dedup_literal_signatures_.clear();
}

// Round 14 (2026-09-18, optimization prototype -- see
// native/performance-investigation-phase4.md, Round 14): sibling to
// LogDrawDedupSummary above, but for the exact-repeat draw-skip
// predicate's counters. Deliberately NOT published through rex::perf's
// CounterId registry -- Round 11a (phase4.md) found that registry's
// storage lives only in the prebuilt, never-locally-rebuilt
// rexruntime(rd).dll, so any CounterId this project adds indexes past
// that binary's already-compiled g_counters array: a real, confirmed,
// unchecked out-of-bounds write. These four counters are plain local
// members that never leave this DLL, same fix pattern Round 11a applied
// to real_pipeline_switches_ etc.
void D3D12CommandProcessor::LogExactDrawDedupSummary() {
  if (exact_dedup_draws_considered_ == 0) {
    // Cheap no-op path: cvar off, or a frame with zero real draws.
    exact_dedup_draws_skipped_ = 0;
    exact_dedup_rejected_state_differed_ = 0;
    exact_dedup_rejected_unsafe_ = 0;
    return;
  }
  bool active = REXCVAR_GET(gpu_exact_draw_dedup_active);
  double skip_percent =
      100.0 * double(exact_dedup_draws_skipped_) / double(exact_dedup_draws_considered_);
  REXGPU_INFO(
      "Round 14 exact-draw-dedup analysis ({}): {} draws considered, {} {} "
      "({:.1f}% of considered) | rejected: {} state differed from the last "
      "candidate, {} unsafe/unknown side-effect state (memexport, active "
      "occlusion query, stencil test, non-opaque blend, non-host-render-target "
      "path, a render-target transfer, or tessellation)",
      active ? "ACTIVE -- suppressing matched draws"
             : "validation -- measuring only, nothing suppressed",
      exact_dedup_draws_considered_, exact_dedup_draws_skipped_,
      active ? "skipped" : "would skip", skip_percent, exact_dedup_rejected_state_differed_,
      exact_dedup_rejected_unsafe_);
  exact_dedup_draws_considered_ = 0;
  exact_dedup_draws_skipped_ = 0;
  exact_dedup_rejected_state_differed_ = 0;
  exact_dedup_rejected_unsafe_ = 0;
}

void D3D12CommandProcessor::IssueSwap(uint32_t frontbuffer_ptr, uint32_t frontbuffer_width,
                                      uint32_t frontbuffer_height) {
  SCOPE_profile_cpu_f("gpu");
  Fable2GpuFrameTick();
  vertex_buffers_in_sync_[0] = 0;
  vertex_buffers_in_sync_[1] = 0;
  ++g_p1117_fetch_gen;  // [PERF-1117] sampler parameters also depend on settings

  // Round 9 (2026-09-16, diagnostic-only): report + clear last frame's
  // draw-dedup signatures before this frame accumulates new ones. See
  // LogDrawDedupSummary() and the gpu_draw_dedup_analysis cvar declaration
  // in rex/graphics/flags.h.
  if (REXCVAR_GET(gpu_draw_dedup_analysis)) {
    LogDrawDedupSummary();
  }

  // Round 14 (2026-09-18): report + reset the exact-draw-dedup counters,
  // and invalidate the last-candidate slot -- a frame boundary is exactly
  // the kind of state/event boundary the predicate must not carry a
  // candidate across (new frame, new constant-buffer-pool allocations,
  // PM4 stream state not traced by this predicate). Called unconditionally
  // (not gated on the cvar) so a mid-session cvar toggle-off still flushes
  // and resets cleanly; the function itself no-ops cheaply when nothing
  // was considered. See native/performance-investigation-phase4.md,
  // Round 14.
  LogExactDrawDedupSummary();
  exact_draw_dedup_last_candidate_.valid = false;

  if (!graphics_system_)
    return;
  ui::Presenter* presenter = graphics_system_->presenter();
  if (!presenter) {
    REXGPU_ERROR("IssueSwap: presenter is null");
    return;
  }

  // In case the swap command is the only one in the frame.
  if (!BeginSubmission(true)) {
    REXGPU_ERROR("IssueSwap: BeginSubmission failed");
    return;
  }

  // Obtain the actual swap source texture size (resolution-scaled if it's a
  // resolve destination, or not otherwise).
  D3D12_SHADER_RESOURCE_VIEW_DESC swap_texture_srv_desc;
  xenos::TextureFormat frontbuffer_format;
  uint32_t frontbuffer_width_unscaled = 0, frontbuffer_height_unscaled = 0;
  ID3D12Resource* swap_texture_resource =
      texture_cache_->RequestSwapTexture(swap_texture_srv_desc, frontbuffer_format,
                                         &frontbuffer_width_unscaled, &frontbuffer_height_unscaled);
  if (!swap_texture_resource) {
    // Dump texture fetch constant 0 for debugging
    const auto& regs = *register_file_;
    auto fetch = regs.GetTextureFetch(0);
    REXGPU_ERROR(
        "IssueSwap: RequestSwapTexture failed - fetch0: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        fetch.dword_0, fetch.dword_1, fetch.dword_2, fetch.dword_3, fetch.dword_4, fetch.dword_5);
    return;
  }
  D3D12_RESOURCE_DESC swap_texture_desc = swap_texture_resource->GetDesc();
  // The swap gamma / FXAA pass samples source texels by pixel index, but swap
  // textures may be allocation-padded. Prefer the active frontbuffer region
  // from the swap packet, scaled proportionally to the actual source texture.
  uint32_t source_width_scaled = uint32_t(swap_texture_desc.Width);
  uint32_t source_height_scaled = uint32_t(swap_texture_desc.Height);
  auto get_active_swap_dimension = [](uint32_t packet_unscaled, uint32_t source_unscaled,
                                      uint32_t source_scaled) -> uint32_t {
    if (!source_scaled) {
      return 0;
    }
    uint32_t active_unscaled = packet_unscaled ? packet_unscaled : source_unscaled;
    if (!active_unscaled) {
      return source_scaled;
    }
    if (source_unscaled) {
      active_unscaled = std::min(active_unscaled, source_unscaled);
      uint64_t active_scaled =
          (uint64_t(active_unscaled) * source_scaled + (source_unscaled >> 1)) / source_unscaled;
      return uint32_t(std::clamp<uint64_t>(active_scaled, 1, source_scaled));
    }
    return std::min(active_unscaled, source_scaled);
  };
  uint32_t guest_output_width =
      get_active_swap_dimension(frontbuffer_width, frontbuffer_width_unscaled, source_width_scaled);
  uint32_t guest_output_height = get_active_swap_dimension(
      frontbuffer_height, frontbuffer_height_unscaled, source_height_scaled);
  if (!guest_output_width) {
    guest_output_width = source_width_scaled
                             ? source_width_scaled
                             : (frontbuffer_width ? frontbuffer_width : frontbuffer_width_unscaled);
  }
  if (!guest_output_height) {
    guest_output_height = source_height_scaled ? source_height_scaled
                                               : (frontbuffer_height ? frontbuffer_height
                                                                     : frontbuffer_height_unscaled);
  }
  bool swap_source_scaled = frontbuffer_width_unscaled && frontbuffer_height_unscaled &&
                            (source_width_scaled != frontbuffer_width_unscaled ||
                             source_height_scaled != frontbuffer_height_unscaled);
  if (texture_cache_->IsDrawResolutionScaled() && !swap_source_scaled) {
    static bool draw_scale_swap_unscaled_logged = false;
    if (!draw_scale_swap_unscaled_logged) {
      draw_scale_swap_unscaled_logged = true;
      REXGPU_WARN(
          "D3D12 draw resolution scaling is enabled, but the swap source is "
          "unscaled ({}x{}). This title may be presenting from an unscaled "
          "resolve path.",
          guest_output_width, guest_output_height);
    }
  }

  system::X_VIDEO_MODE video_mode;
  kernel::xboxkrnl::VdQueryVideoMode(&video_mode);
  uint32_t display_width = std::max(uint32_t(1), uint32_t(video_mode.display_width));
  uint32_t display_height = std::max(uint32_t(1), uint32_t(video_mode.display_height));

  presenter->RefreshGuestOutput(
      guest_output_width, guest_output_height, display_width, display_height,
      [this, &swap_texture_srv_desc, frontbuffer_format, swap_texture_resource, guest_output_width,
       guest_output_height](ui::Presenter::GuestOutputRefreshContext& context) -> bool {
        const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
        ID3D12Device* device = provider.GetDevice();

        SwapPostEffect swap_post_effect = GetActualSwapPostEffect();
        bool use_fxaa = swap_post_effect == SwapPostEffect::kFxaa ||
                        swap_post_effect == SwapPostEffect::kFxaaExtreme;
        if (use_fxaa) {
          // Make sure the texture of the correct size is available for FXAA.
          if (fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc = fxaa_source_texture_->GetDesc();
            if (fxaa_source_texture_desc.Width != guest_output_width ||
                fxaa_source_texture_desc.Height != guest_output_height) {
              if (submission_completed_ < fxaa_source_texture_submission_) {
                fxaa_source_texture_->AddRef();
                resources_for_deletion_.emplace_back(fxaa_source_texture_submission_,
                                                     fxaa_source_texture_.Get());
              }
              fxaa_source_texture_.Reset();
              fxaa_source_texture_submission_ = 0;
            }
          }
          if (!fxaa_source_texture_) {
            D3D12_RESOURCE_DESC fxaa_source_texture_desc;
            fxaa_source_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            fxaa_source_texture_desc.Alignment = 0;
            fxaa_source_texture_desc.Width = guest_output_width;
            fxaa_source_texture_desc.Height = guest_output_height;
            fxaa_source_texture_desc.DepthOrArraySize = 1;
            fxaa_source_texture_desc.MipLevels = 1;
            fxaa_source_texture_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_texture_desc.SampleDesc.Count = 1;
            fxaa_source_texture_desc.SampleDesc.Quality = 0;
            fxaa_source_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            fxaa_source_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            if (FAILED(device->CreateCommittedResource(
                    &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
                    &fxaa_source_texture_desc, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                    nullptr, IID_PPV_ARGS(&fxaa_source_texture_)))) {
              REXGPU_ERROR("Failed to create the FXAA input texture");
              swap_post_effect = SwapPostEffect::kNone;
              use_fxaa = false;
            }
          }
        }

        // This is according to D3D::InitializePresentationParameters from a
        // game executable, which initializes the 256-entry table gamma ramp for
        // 8_8_8_8 output and the PWL gamma ramp for 2_10_10_10.
        // TODO(Triang3l): Choose between the table and PWL based on
        // DC_LUTA_CONTROL, support both for all formats (and also different
        // increments for PWL).
        bool use_pwl_gamma_ramp =
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10 ||
            frontbuffer_format == xenos::TextureFormat::k_2_10_10_10_AS_16_16_16_16;

        context.SetIs8bpc(!use_pwl_gamma_ramp && !use_fxaa);

        // Upload the new gamma ramp, using the upload buffer for the current
        // frame (will close the frame after this anyway, so can't write
        // multiple times per frame).
        if (!(use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                                 : gamma_ramp_256_entry_table_up_to_date_)) {
          uint32_t gamma_ramp_offset_bytes = use_pwl_gamma_ramp ? 256 * 4 : 0;
          uint32_t gamma_ramp_upload_offset_bytes =
              uint32_t(frame_current_ % kQueueFrames) * ((256 + 128 * 3) * 4) +
              gamma_ramp_offset_bytes;
          uint32_t gamma_ramp_size_bytes = (use_pwl_gamma_ramp ? 128 * 3 : 256) * 4;
          if (std::endian::native != std::endian::little && use_pwl_gamma_ramp) {
            // R16G16 is first R16, where the shader expects the base, and
            // second G16, where the delta should be, but gamma_ramp_pwl_rgb()
            // is an array of 32-bit DC_LUT_PWL_DATA registers - swap 16 bits in
            // each 32.
            auto gamma_ramp_pwl_upload_buffer = reinterpret_cast<reg::DC_LUT_PWL_DATA*>(
                gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes);
            const reg::DC_LUT_PWL_DATA* gamma_ramp_pwl = gamma_ramp_pwl_rgb();
            for (size_t i = 0; i < 128 * 3; ++i) {
              reg::DC_LUT_PWL_DATA& gamma_ramp_pwl_upload_buffer_entry =
                  gamma_ramp_pwl_upload_buffer[i];
              reg::DC_LUT_PWL_DATA gamma_ramp_pwl_entry = gamma_ramp_pwl[i];
              gamma_ramp_pwl_upload_buffer_entry.base = gamma_ramp_pwl_entry.delta;
              gamma_ramp_pwl_upload_buffer_entry.delta = gamma_ramp_pwl_entry.base;
            }
          } else {
            std::memcpy(gamma_ramp_upload_buffer_mapping_ + gamma_ramp_upload_offset_bytes,
                        use_pwl_gamma_ramp ? static_cast<const void*>(gamma_ramp_pwl_rgb())
                                           : static_cast<const void*>(gamma_ramp_256_entry_table()),
                        gamma_ramp_size_bytes);
          }
          PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                                D3D12_RESOURCE_STATE_COPY_DEST);
          gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              gamma_ramp_buffer_.Get(), gamma_ramp_offset_bytes, gamma_ramp_upload_buffer_.Get(),
              gamma_ramp_upload_offset_bytes, gamma_ramp_size_bytes);
          (use_pwl_gamma_ramp ? gamma_ramp_pwl_up_to_date_
                              : gamma_ramp_256_entry_table_up_to_date_) = true;
        }

        // Destination, source, and if bindful, gamma ramp.
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptors[3];
        ui::d3d12::util::DescriptorCpuGpuHandlePair apply_gamma_descriptor_gamma_ramp;
        if (!RequestOneUseSingleViewDescriptors(bindless_resources_used_ ? 2 : 3,
                                                apply_gamma_descriptors)) {
          return false;
        }
        // Must not call anything that can change the descriptor heap from now
        // on!
        if (bindless_resources_used_) {
          apply_gamma_descriptor_gamma_ramp = GetSystemBindlessViewHandlePair(
              use_pwl_gamma_ramp ? SystemBindlessView::kGammaRampPWLSRV
                                 : SystemBindlessView::kGammaRampTableSRV);
        } else {
          apply_gamma_descriptor_gamma_ramp = apply_gamma_descriptors[2];
          WriteGammaRampSRV(use_pwl_gamma_ramp, apply_gamma_descriptor_gamma_ramp.first);
        }

        ID3D12Resource* guest_output_resource =
            static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
                .resource_uav_capable();

        if (use_fxaa) {
          fxaa_source_texture_submission_ = submission_current_;
        }

        ID3D12Resource* apply_gamma_dest =
            use_fxaa ? fxaa_source_texture_.Get() : guest_output_resource;
        D3D12_RESOURCE_STATES apply_gamma_dest_initial_state =
            use_fxaa ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                     : ui::d3d12::D3D12Presenter::kGuestOutputInternalState;
        static_cast<ui::d3d12::D3D12Presenter::D3D12GuestOutputRefreshContext&>(context)
            .resource_uav_capable();
        PushTransitionBarrier(apply_gamma_dest, apply_gamma_dest_initial_state,
                              D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        // From now on, even in case of failure, apply_gamma_dest must be
        // transitioned back to apply_gamma_dest_initial_state!
        D3D12_UNORDERED_ACCESS_VIEW_DESC apply_gamma_dest_uav_desc;
        apply_gamma_dest_uav_desc.Format =
            use_fxaa ? kFxaaSourceTextureFormat : ui::d3d12::D3D12Presenter::kGuestOutputFormat;
        apply_gamma_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        apply_gamma_dest_uav_desc.Texture2D.MipSlice = 0;
        apply_gamma_dest_uav_desc.Texture2D.PlaneSlice = 0;
        device->CreateUnorderedAccessView(apply_gamma_dest, nullptr, &apply_gamma_dest_uav_desc,
                                          apply_gamma_descriptors[0].first);

        device->CreateShaderResourceView(swap_texture_resource, &swap_texture_srv_desc,
                                         apply_gamma_descriptors[1].first);

        PushTransitionBarrier(gamma_ramp_buffer_.Get(), gamma_ramp_buffer_state_,
                              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        gamma_ramp_buffer_state_ = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;

        deferred_command_list_.D3DSetComputeRootSignature(apply_gamma_root_signature_.Get());
        ApplyGammaConstants apply_gamma_constants;
        apply_gamma_constants.size[0] = guest_output_width;
        apply_gamma_constants.size[1] = guest_output_height;
        deferred_command_list_.D3DSetComputeRoot32BitConstants(
            UINT(ApplyGammaRootParameter::kConstants),
            sizeof(apply_gamma_constants) / sizeof(uint32_t), &apply_gamma_constants, 0);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kDestination), apply_gamma_descriptors[0].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kSource), apply_gamma_descriptors[1].second);
        deferred_command_list_.D3DSetComputeRootDescriptorTable(
            UINT(ApplyGammaRootParameter::kRamp), apply_gamma_descriptor_gamma_ramp.second);
        ID3D12PipelineState* apply_gamma_pipeline;
        if (use_pwl_gamma_ramp) {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_pwl_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_pwl_pipeline_.Get();
        } else {
          apply_gamma_pipeline = use_fxaa ? apply_gamma_table_fxaa_luma_pipeline_.Get()
                                          : apply_gamma_table_pipeline_.Get();
        }
        SetExternalPipeline(apply_gamma_pipeline);
        SubmitBarriers();
        uint32_t group_count_x = (guest_output_width + 15) / 16;
        uint32_t group_count_y = (guest_output_height + 7) / 8;
        deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);

        // Apply FXAA.
        if (use_fxaa) {
          // Destination and source.
          ui::d3d12::util::DescriptorCpuGpuHandlePair fxaa_descriptors[2];
          if (!RequestOneUseSingleViewDescriptors(uint32_t(rex::countof(fxaa_descriptors)),
                                                  fxaa_descriptors)) {
            // Failed to obtain descriptors for FXAA - just copy after gamma
            // ramp application without applying FXAA.
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  D3D12_RESOURCE_STATE_COPY_SOURCE);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_COPY_DEST);
            SubmitBarriers();
            deferred_command_list_.D3DCopyResource(guest_output_resource, apply_gamma_dest);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_COPY_SOURCE,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_COPY_DEST,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
            return false;
          } else {
            assert_true(apply_gamma_dest_initial_state ==
                        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  apply_gamma_dest_initial_state);
            PushTransitionBarrier(guest_output_resource,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState,
                                  D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            // From now on, even in case of failure, guest_output_resource must
            // be transitioned back to kGuestOutputInternalState!
            deferred_command_list_.D3DSetComputeRootSignature(fxaa_root_signature_.Get());
            FxaaConstants fxaa_constants;
            fxaa_constants.size[0] = guest_output_width;
            fxaa_constants.size[1] = guest_output_height;
            fxaa_constants.size_inv[0] = 1.0f / float(fxaa_constants.size[0]);
            fxaa_constants.size_inv[1] = 1.0f / float(fxaa_constants.size[1]);
            deferred_command_list_.D3DSetComputeRoot32BitConstants(
                UINT(FxaaRootParameter::kConstants), sizeof(fxaa_constants) / sizeof(uint32_t),
                &fxaa_constants, 0);
            D3D12_UNORDERED_ACCESS_VIEW_DESC fxaa_dest_uav_desc;
            fxaa_dest_uav_desc.Format = ui::d3d12::D3D12Presenter::kGuestOutputFormat;
            fxaa_dest_uav_desc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            fxaa_dest_uav_desc.Texture2D.MipSlice = 0;
            fxaa_dest_uav_desc.Texture2D.PlaneSlice = 0;
            device->CreateUnorderedAccessView(guest_output_resource, nullptr, &fxaa_dest_uav_desc,
                                              fxaa_descriptors[0].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kDestination), fxaa_descriptors[0].second);
            D3D12_SHADER_RESOURCE_VIEW_DESC fxaa_source_srv_desc;
            fxaa_source_srv_desc.Format = kFxaaSourceTextureFormat;
            fxaa_source_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            fxaa_source_srv_desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            fxaa_source_srv_desc.Texture2D.MostDetailedMip = 0;
            fxaa_source_srv_desc.Texture2D.MipLevels = 1;
            fxaa_source_srv_desc.Texture2D.PlaneSlice = 0;
            fxaa_source_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(fxaa_source_texture_.Get(), &fxaa_source_srv_desc,
                                             fxaa_descriptors[1].first);
            deferred_command_list_.D3DSetComputeRootDescriptorTable(
                UINT(FxaaRootParameter::kSource), fxaa_descriptors[1].second);
            SetExternalPipeline(swap_post_effect == SwapPostEffect::kFxaaExtreme
                                    ? fxaa_extreme_pipeline_.Get()
                                    : fxaa_pipeline_.Get());
            SubmitBarriers();
            deferred_command_list_.D3DDispatch(group_count_x, group_count_y, 1);
            PushTransitionBarrier(guest_output_resource, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                  ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          }
        } else {
          assert_true(apply_gamma_dest_initial_state ==
                      ui::d3d12::D3D12Presenter::kGuestOutputInternalState);
          PushTransitionBarrier(apply_gamma_dest, D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                                apply_gamma_dest_initial_state);
        }

        // Need to submit all the commands before giving the image back to the
        // presenter so it can submit its own commands for displaying it to the
        // queue.
        SubmitBarriers();
        EndSubmission(true);
        return true;
      });

  // End the frame even if did not present for any reason (the image refresher
  // was not called), to prevent leaking per-frame resources.
  EndSubmission(true);
}

void D3D12CommandProcessor::OnPrimaryBufferEnd() {
  if (REXCVAR_GET(d3d12_submit_on_primary_buffer_end) && submission_open_ &&
      CanEndSubmissionImmediately()) {
    EndSubmission(false);
  }
}

Shader* D3D12CommandProcessor::LoadShader(xenos::ShaderType shader_type, uint32_t guest_address,
                                          const uint32_t* host_address, uint32_t dword_count) {
  return pipeline_cache_->LoadShader(shader_type, host_address, dword_count);
}

// Round 16 (Fable2PC, fortieth follow-up): logs the guest-physical bytes
// backing ONE vertex-fetch constant, at the exact point the real renderer
// has just proven them resolvable (SharedMemory::RequestRange() having
// just succeeded, or the buffer already being in sync from an earlier
// draw -- see both call sites below). Uses the SAME TranslatePhysical()/
// GetPhysicalHeap()/QueryRegionInfo() mechanism Fable2PC's own diagnostic
// (native/src/diagnostics/fable_draw_capture.cpp's DiagSnapshotGuestPhysical
// Bytes) already uses, deliberately, so a success or failure here is a
// true apples-to-apples comparison against that file's own "0 bytes via
// both paths" result for the identical address -- not a different
// mechanism that might just happen to work for unrelated reasons.
static void LogWatchedVertexFetchCapture(uint32_t vfetch_index,
                                         const xenos::xe_gpu_vertex_fetch_t& vfetch_constant,
                                         Shader& vertex_shader, memory::Memory* mem,
                                         const char* path_label) {
  // NOTE: parameter is named "mem", not "memory" -- "memory" is also this
  // TU's namespace name (rex::memory), and shadowing it with a local of
  // the same name would break every unqualified memory::Foo reference
  // below (same reason DiagIsPhysicalChunkReadable in fable_draw_capture.cpp
  // uses "mem" too).
  const uint32_t const_index = vfetch_index / 3u;
  const uint32_t const_index_sel = vfetch_index % 3u;
  const uint32_t address_bytes = vfetch_constant.address << 2;
  const uint32_t size_bytes = vfetch_constant.size << 2;
  constexpr uint32_t kCaptureBytesCap = 224u;
  const uint32_t want_bytes = (size_bytes < kCaptureBytesCap) ? size_bytes : kCaptureBytesCap;

  // Round 17 (Fable2PC, forty-second follow-up): the forty-first
  // follow-up's result -- QueryRegionInfo/committed says NOT readable even
  // inside the renderer's own IssueDraw, right after RequestRange()
  // resolved this exact address -- turned out not to be evidence the data
  // is unreachable. Reading D3D12SharedMemory::UploadRanges() (the actual
  // function RequestRange()/RequestRanges() calls to fill shared_memory_'s
  // host buffer) shows it never calls QueryRegionInfo at all: it just does
  // memory().TranslatePhysical(addr) and memcpy's from it, unconditionally.
  // And Memory::TranslatePhysical() itself (rex/system/xmemory.h) is bare
  // pointer arithmetic -- physical_membase_ + (guest_address & 0x1FFFFFFF)
  // -- into a single bulk host allocation reserved for the WHOLE guest
  // physical address space at emulator startup (Memory::Initialize(),
  // rex/system/xmemory.cpp: physical_membase_ = mapping_base_ +
  // 0x100000000ull). So any in-range guest-physical address is always a
  // valid, dereferenceable host pointer, regardless of QueryRegionInfo's
  // answer. QueryRegionInfo's "committed" bit is a separate, higher-level
  // simulation of the GUEST's own allocator bookkeeping (mirroring what
  // the Xbox 360 kernel's VirtualQuery-equivalent would report to guest
  // code), not a reflection of host memory validity -- transient/streaming
  // vertex data written outside that tracked allocation path legitimately
  // shows as "not committed" while still being perfectly safe, meaningful
  // bytes to read the exact same way UploadRanges() already does, every
  // frame, to render correctly. So: stop gating the read on it. Still
  // query it, but only to log what it says for correlation -- never to
  // decide whether to attempt the read.
  bool queryable = false;
  bool committed = false;
  bool prot_readable = false;
  if (mem) {
    memory::VirtualHeap* heap = mem->GetPhysicalHeap();
    if (heap) {
      memory::HeapAllocationInfo info{};
      if (heap->QueryRegionInfo(address_bytes, &info)) {
        queryable = true;
        committed = (info.state & memory::kMemoryAllocationCommit) != 0;
        prot_readable = (info.protect & memory::kMemoryProtectRead) != 0;
      }
    }
  }

  if (!mem) {
    REXGPU_WARN(
        "[GpuFetchWatch] vfetch_index={} (const_index={} sel={}) address={:#010x} "
        "size_bytes={} ucode_hash={:#018x} via={}: no memory::Memory* available -- "
        "cannot even attempt TranslatePhysical() here",
        vfetch_index, const_index, const_index_sel, address_bytes, size_bytes,
        vertex_shader.ucode_data_hash(), path_label);
    REXCVAR_SET(gpu_fetch_watch_address, 0u);
    return;
  }

  // Unconditional read, matching D3D12SharedMemory::UploadRanges() exactly
  // -- no QueryRegionInfo gate. queryable/committed/prot_readable are
  // logged alongside the captured bytes purely as correlation data (does
  // the guest allocator bookkeeping ever catch up for this address), not
  // as a precondition for the read.
  uint8_t* bytes = mem->TranslatePhysical(address_bytes);
  std::string hex;
  for (uint32_t off = 0; off + 4 <= want_bytes; off += 4) {
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x ",
                  (uint32_t(bytes[off]) << 24) | (uint32_t(bytes[off + 1]) << 16) |
                      (uint32_t(bytes[off + 2]) << 8) | uint32_t(bytes[off + 3]));
    hex += buf;
  }
  REXGPU_INFO(
      "[GpuFetchWatch] vfetch_index={} (const_index={} sel={}) address={:#010x} "
      "size_bytes={} ucode_hash={:#018x} via={} queryable={} committed={} "
      "prot_readable={}: read (unconditional, matching UploadRanges) -- {} bytes "
      "captured at the renderer's own resolution point: {}",
      vfetch_index, const_index, const_index_sel, address_bytes, size_bytes,
      vertex_shader.ucode_data_hash(), path_label, queryable, committed, prot_readable,
      want_bytes, hex);
  REXCVAR_SET(gpu_fetch_watch_address, 0u);
}

bool D3D12CommandProcessor::IssueDraw(xenos::PrimitiveType primitive_type, uint32_t index_count,
                                      IndexBufferInfo* index_buffer_info,
                                      bool major_mode_explicit) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // [PERF-1006b]: mid-frame submission split, see fable2_submit_every_n_draws.
  // Nothing of this draw has been recorded yet, so this is equivalent to the
  // primary buffer having ended right before this packet.
  Fable2PerfCountDraw();
  {
    static uint32_t fable2_poll_counter = 0;
    if ((++fable2_poll_counter & 63) == 0) {
      Fable2PollPendingQueriesNow();  // [PERF-1006d] deliver finished readbacks
    }

    const uint32_t split_every = REXCVAR_GET(fable2_submit_every_n_draws);
    if (split_every && submission_open_ && ++fable2_draws_in_submission_ >= split_every &&
        !scratch_buffer_used_ && CanEndSubmissionImmediately()) {
      exact_draw_dedup_last_candidate_.valid = false;
      if (EndSubmission(false)) {
        Fable2PerfCountSubmission(true);
      }
    }

    // [PERF-1116]: hand batched memexport readback copies to the GPU, see
    // fable2_memexport_submit_batch_draws. Same boundary as the split above.
    if (fable2_memexport_flush_pending_ && submission_open_) {
      if (fable2_memexport_flush_countdown_) {
        --fable2_memexport_flush_countdown_;
      }
      if (!fable2_memexport_flush_countdown_ && !scratch_buffer_used_ &&
          CanEndSubmissionImmediately()) {
        exact_draw_dedup_last_candidate_.valid = false;
        EndSubmission(false);
      }
    }
  }

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  const RegisterFile& regs = *register_file_;

  xenos::EdramMode edram_mode = regs.Get<reg::RB_MODECONTROL>().edram_mode;
  if (edram_mode == xenos::EdramMode::kCopy) {
    // Special copy handling.
    return IssueCopy();
  }

  bool surface_pitch_is_zero = regs.Get<reg::RB_SURFACE_INFO>().surface_pitch == 0;

  // Vertex shader analysis.
  auto vertex_shader = static_cast<D3D12Shader*>(active_vertex_shader());
  if (!vertex_shader) {
    // Always need a vertex shader.
    return false;
  }
  pipeline_cache_->AnalyzeShaderUcode(*vertex_shader);
  bool memexport_used_vertex = vertex_shader->memexport_eM_written() != 0;

  // Pixel shader analysis.
  bool primitive_polygonal = draw_util::IsPrimitivePolygonal(regs);
  bool is_rasterization_done = draw_util::IsRasterizationPotentiallyDone(regs, primitive_polygonal);
  if (surface_pitch_is_zero && is_rasterization_done) {
    // Doesn't actually draw.
    // Unlikely that zero would even really be legal though.
    return true;
  }
  D3D12Shader* pixel_shader = nullptr;
  if (is_rasterization_done) {
    // See xenos::EdramMode for explanation why the pixel shader is only used
    // when it's kColorDepth here.
    if (edram_mode == xenos::EdramMode::kColorDepth) {
      pixel_shader = static_cast<D3D12Shader*>(active_pixel_shader());
      if (pixel_shader) {
        pipeline_cache_->AnalyzeShaderUcode(*pixel_shader);
        if (!draw_util::IsPixelShaderNeededWithRasterization(*pixel_shader, regs)) {
          pixel_shader = nullptr;
        }
      }
    }
  } else {
    // Disabling pixel shader for this case is also required by the pipeline
    // cache.
    if (!memexport_used_vertex) {
      // This draw has no effect.
      return true;
    }
  }
  bool memexport_used_pixel = pixel_shader && (pixel_shader->memexport_eM_written() != 0);
  bool memexport_used = memexport_used_vertex || memexport_used_pixel;

  if (!BeginSubmission(true)) {
    return false;
  }

  // Process primitives.
  PrimitiveProcessor::ProcessingResult primitive_processing_result;
  {
    // Round 21 (2026-09-19, IssueDraw per-draw CPU cost breakdown -- see
    // native/performance-investigation-phase4.md): this and the zones
    // below it are purely additive Tracy instrumentation (same
    // RelWithDebInfo-only, zero-cost-when-disabled mechanism as Round
    // 12's PipelineBind zone), measuring where IssueDraw's per-draw CPU
    // cost actually concentrates, to decide WHERE a future draw-skip
    // predicate could safely be hoisted earlier to capture more of it
    // (Round 14's predicate currently only runs right before dispatch,
    // per its own documented "what this does NOT save" caveat).
    SCOPE_profile_cpu_f("DrawPrimitiveProcess");
    if (!primitive_processor_->Process(primitive_processing_result)) {
      return false;
    }
  }
  if (!primitive_processing_result.host_draw_vertex_count) {
    // Nothing to draw.
    return true;
  }

  reg::RB_DEPTHCONTROL normalized_depth_control = draw_util::GetNormalizedDepthControl(regs);

  // Shader modifications.
  uint32_t ps_param_gen_pos = UINT32_MAX;
  uint32_t interpolator_mask =
      pixel_shader ? (vertex_shader->writes_interpolators() &
                      pixel_shader->GetInterpolatorInputMask(regs.Get<reg::SQ_PROGRAM_CNTL>(),
                                                             regs.Get<reg::SQ_CONTEXT_MISC>(),
                                                             ps_param_gen_pos))
                   : 0;
  DxbcShaderTranslator::Modification vertex_shader_modification =
      pipeline_cache_->GetCurrentVertexShaderModification(
          *vertex_shader, primitive_processing_result.host_vertex_shader_type, interpolator_mask);
  DxbcShaderTranslator::Modification pixel_shader_modification =
      pixel_shader
          ? pipeline_cache_->GetCurrentPixelShaderModification(
                *pixel_shader, interpolator_mask, ps_param_gen_pos, normalized_depth_control)
          : DxbcShaderTranslator::Modification(0);

  // Set up the render targets - this may perform dispatches and draws.
  uint32_t normalized_color_mask =
      pixel_shader ? draw_util::GetNormalizedColorMask(regs, pixel_shader->writes_color_targets())
                   : 0;
  {
    SCOPE_profile_cpu_f("DrawRenderTargetUpdate");
    if (!render_target_cache_->Update(is_rasterization_done, normalized_depth_control,
                                      normalized_color_mask, *vertex_shader)) {
      return false;
    }
  }

  // Create the pipeline (for this, need the actually used render target formats
  // from the render target cache), translating the shaders - doing this now to
  // obtain the used textures.
  void* pipeline_handle;
  ID3D12RootSignature* root_signature;
  bool host_render_targets_used;
  {
    SCOPE_profile_cpu_f("DrawPipelineConfigure");
    // [PERF-1117]: translation lookup cache.
    D3D12Shader::D3D12Translation* vertex_shader_translation =
        P1117GetTranslation(*vertex_shader, vertex_shader_modification.value);
    D3D12Shader::D3D12Translation* pixel_shader_translation =
        pixel_shader ? P1117GetTranslation(*pixel_shader, pixel_shader_modification.value)
                     : nullptr;
    uint32_t bound_depth_and_color_render_target_bits;
    uint32_t bound_depth_and_color_render_target_formats[1 + xenos::kMaxColorRenderTargets];
    host_render_targets_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets;
    if (host_render_targets_used) {
      bound_depth_and_color_render_target_bits =
          render_target_cache_->GetLastUpdateBoundRenderTargets(
              bound_depth_and_color_render_target_formats);
    } else {
      bound_depth_and_color_render_target_bits = 0;
    }
    if (!pipeline_cache_->ConfigurePipeline(
            vertex_shader_translation, pixel_shader_translation, primitive_processing_result,
            normalized_depth_control, normalized_color_mask, bound_depth_and_color_render_target_bits,
            bound_depth_and_color_render_target_formats, &pipeline_handle, &root_signature)) {
      return false;
    }
    if (REXCVAR_GET(async_shader_compilation) &&
        pipeline_cache_->GetD3D12PipelineByHandle(pipeline_handle) == nullptr) {
      return true;
    }
  }

  // Update the textures - this may bind pipelines.
  uint32_t used_texture_mask =
      vertex_shader->GetUsedTextureMaskAfterTranslation() |
      (pixel_shader != nullptr ? pixel_shader->GetUsedTextureMaskAfterTranslation() : 0);
  {
    SCOPE_profile_cpu_f("DrawTextureRequest");
    texture_cache_->RequestTextures(used_texture_mask);
  }

  // Round 9 (2026-09-16, diagnostic-only, not a rendering change): when
  // gpu_draw_dedup_analysis is enabled, hash this draw's {pipeline,
  // vertex-fetch, texture-fetch} identity and record it, so IssueSwap can
  // report how many draws per frame are exact duplicates of another draw's
  // state (a prerequisite measurement for whether cross-draw batching is
  // worth building -- see native/performance-investigation-phase1.md,
  // architectural investigation candidate A). Deliberately excludes
  // per-instance-varying state (shader float constants like world
  // transform, viewport/scissor, blend factor) -- those are exactly what
  // a real instanced-draw implementation would still carry per-instance,
  // so including them here would undercount duplication. Off by default;
  // the single REXCVAR_GET check below is the only cost when disabled,
  // same pattern as the existing async_shader_compilation check just above.
  if (REXCVAR_GET(gpu_draw_dedup_analysis)) {
    if (draw_dedup_signatures_.capacity() == 0) {
      draw_dedup_signatures_.reserve(8192);
      draw_dedup_pipeline_signatures_.reserve(8192);
      draw_dedup_texture_signatures_.reserve(8192);
      draw_dedup_literal_signatures_.reserve(8192);
    }
    uint64_t sig = 0xcbf29ce484222325ULL;  // FNV-1a offset basis
    auto hash_combine = [](uint64_t& s, uint64_t v) {
      s ^= v;
      s *= 0x100000001B3ULL;  // FNV-1a prime
    };
    // Round 10 (2026-09-18): pipeline-only and texture-only sub-signatures,
    // hashed independently of the combined `sig` above (own FNV-1a chains,
    // not derived from `sig`) so each is a clean identity for its own
    // narrower question -- "same PSO as some other draw this frame?" /
    // "same bound texture set as some other draw this frame?" -- rather
    // than being entangled with vertex-fetch/index-buffer state. See
    // command_processor.h's comment on the two new vectors and
    // native/performance-investigation-phase4.md.
    uint64_t pipeline_sig = 0xcbf29ce484222325ULL;
    uint64_t texture_sig = 0xcbf29ce484222325ULL;
    hash_combine(sig, reinterpret_cast<uintptr_t>(pipeline_handle));
    hash_combine(pipeline_sig, reinterpret_cast<uintptr_t>(pipeline_handle));
    auto hash_fetch_slot = [&](uint64_t& s, uint32_t fetch_index) {
      uint32_t base = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 6;
      for (uint32_t d = 0; d < 6; ++d) {
        hash_combine(s, regs.values[base + d]);
      }
    };
    const Shader::ConstantRegisterMap& dedup_vfetch_map = vertex_shader->constant_register_map();
    for (uint32_t i = 0; i < rex::countof(dedup_vfetch_map.vertex_fetch_bitmap); ++i) {
      uint32_t bits_remaining = dedup_vfetch_map.vertex_fetch_bitmap[i];
      uint32_t j;
      while (rex::bit_scan_forward(bits_remaining, &j)) {
        bits_remaining &= ~(uint32_t(1) << j);
        hash_fetch_slot(sig, i * 32 + j);
      }
    }
    uint32_t textures_remaining_dedup = used_texture_mask;
    uint32_t tex_index;
    while (rex::bit_scan_forward(textures_remaining_dedup, &tex_index)) {
      textures_remaining_dedup &= ~(uint32_t(1) << tex_index);
      hash_fetch_slot(sig, tex_index);
      hash_fetch_slot(texture_sig, tex_index);
    }
    if (index_buffer_info != nullptr) {
      hash_combine(sig, index_buffer_info->guest_base);
      hash_combine(sig, uint64_t(index_buffer_info->length));
      hash_combine(sig, uint32_t(index_buffer_info->format));
    }
    draw_dedup_signatures_.push_back(sig);
    draw_dedup_pipeline_signatures_.push_back(pipeline_sig);
    // Empty texture set (used_texture_mask == 0) still gets a real, valid
    // signature here (the FNV-1a offset basis, untouched) -- draws with no
    // bound textures at all are a real, meaningful category (e.g. pure
    // depth/shadow passes) and should dedup against each other, not be
    // silently skipped.
    draw_dedup_texture_signatures_.push_back(texture_sig);

    // Round 13 (2026-09-18): literal_sig starts from the already-computed
    // full-state `sig` and extends it with every float constant register
    // the vertex AND pixel shader actually read (world transform lives
    // among the vertex shader's -- see Shader::ConstantRegisterMap's
    // float_bitmap comment). Two draws sharing literal_sig are
    // byte-for-byte identical inputs, not just "same mesh/material." Base
    // register offsets verified against the existing float-constant
    // upload path just below in this file (cbuffer_binding_float_vertex_/
    // cbuffer_binding_float_pixel_): vertex constants live at
    // XE_GPU_REG_SHADER_CONSTANT_000_X, pixel at
    // XE_GPU_REG_SHADER_CONSTANT_256_X, each float_bitmap chunk `i`
    // covering registers [i*256, i*256+255] i.e. `(i << 8) +
    // (local_bit << 2)` -- the same indexing that path already uses, not
    // reinvented here. See native/performance-investigation-phase4.md,
    // Round 13.
    uint64_t literal_sig = sig;
    auto hash_float_constants = [&](uint64_t& s, const Shader::ConstantRegisterMap& map,
                                     uint32_t base_reg) {
      for (uint32_t i = 0; i < rex::countof(map.float_bitmap); ++i) {
        uint64_t bits_remaining_f = map.float_bitmap[i];
        uint32_t bit;
        while (rex::bit_scan_forward(bits_remaining_f, &bit)) {
          bits_remaining_f &= ~(uint64_t(1) << bit);
          uint32_t reg = base_reg + (i << 8) + (bit << 2);
          for (uint32_t d = 0; d < 4; ++d) {
            hash_combine(s, regs.values[reg + d]);
          }
        }
      }
    };
    hash_float_constants(literal_sig, vertex_shader->constant_register_map(),
                          XE_GPU_REG_SHADER_CONSTANT_000_X);
    if (pixel_shader != nullptr) {
      hash_float_constants(literal_sig, pixel_shader->constant_register_map(),
                            XE_GPU_REG_SHADER_CONSTANT_256_X);
    }
    draw_dedup_literal_signatures_.push_back(literal_sig);
  }

  // Bind the pipeline after configuring it and doing everything that may bind
  // other pipelines.
  //
  // Round 11 (2026-09-18): this is the REAL dirty check that already exists
  // for pipeline binding -- it only emits SetPipelineStateHandle when the
  // new PSO differs from whatever is CURRENTLY bound (i.e. what the
  // previous draw left behind), not from anywhere else in the frame. The
  // kRealPipelineSwitches counter below records how often this branch
  // actually fires, so it can be compared against Round 10's frame-wide
  // kDrawDedupPipelineDuplicateDraws: if real switches are already rare
  // (draws using the same PSO are mostly submitted back-to-back), this
  // existing check is already capturing most of the available savings and
  // a same-PSO-skip fast path would have little left to gain; if real
  // switches track close to (total draws - 0), duplicate-PSO draws are
  // scattered across the frame and only a draw-reordering/batching change
  // (not a simple skip-if-unchanged check) could realize the Round 10
  // headroom. Gated by the same cvar as Round 9/10; zero cost when off.
  if (current_guest_pipeline_ != pipeline_handle) {
    if (REXCVAR_GET(gpu_draw_dedup_analysis)) {
      // Counted only when this branch is actually entered -- i.e. a real
      // SetPipelineStateHandle is about to be emitted. Compare against the
      // per-frame draw count (Round 10) to get the real switch RATE.
      // Round 11a: local member, not rex::perf::IncrementCounter -- see
      // command_processor.h and LogDrawDedupSummary for why.
      ++real_pipeline_switches_;
    }
    deferred_command_list_.SetPipelineStateHandle(reinterpret_cast<void*>(pipeline_handle));
    current_guest_pipeline_ = pipeline_handle;
    current_external_pipeline_ = nullptr;
  }

  // Get dynamic rasterizer state.
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  bool convert_z_to_float24 =
      host_render_targets_used && render_target_cache_->depth_float24_convert_in_pixel_shader();
  bool ps_writes_depth = pixel_shader && pixel_shader->writes_depth();

  // Build a cache key from all viewport-affecting state to skip redundant
  // recalculation when the viewport registers haven't changed between draws.
  ViewportCacheKey viewport_key;
  viewport_key.pa_cl_clip_cntl = regs[XE_GPU_REG_PA_CL_CLIP_CNTL];
  viewport_key.pa_cl_vte_cntl = regs[XE_GPU_REG_PA_CL_VTE_CNTL];
  viewport_key.pa_su_sc_mode_cntl = regs[XE_GPU_REG_PA_SU_SC_MODE_CNTL];
  viewport_key.pa_su_vtx_cntl = regs[XE_GPU_REG_PA_SU_VTX_CNTL];
  viewport_key.pa_sc_window_offset = regs[XE_GPU_REG_PA_SC_WINDOW_OFFSET];
  viewport_key.normalized_depth_control = normalized_depth_control.value;
  std::memcpy(viewport_key.vport_regs, &regs[XE_GPU_REG_PA_CL_VPORT_XSCALE],
              sizeof(viewport_key.vport_regs));
  viewport_key.flags = (uint32_t(convert_z_to_float24) << 0) |
                       (uint32_t(host_render_targets_used) << 1) | (uint32_t(ps_writes_depth) << 2);

  draw_util::ViewportInfo viewport_info;
  if (viewport_cache_valid_ && viewport_key == previous_viewport_key_) {
    viewport_info = previous_viewport_info_;
  } else {
    draw_util::GetHostViewportInfo(regs, draw_resolution_scale_x, draw_resolution_scale_y, true,
                                   D3D12_VIEWPORT_BOUNDS_MAX, D3D12_VIEWPORT_BOUNDS_MAX, false,
                                   normalized_depth_control, convert_z_to_float24,
                                   host_render_targets_used, ps_writes_depth, viewport_info);
    previous_viewport_key_ = viewport_key;
    previous_viewport_info_ = viewport_info;
    viewport_cache_valid_ = true;
  }

  draw_util::Scissor scissor;
  draw_util::GetScissor(regs, scissor);
  scissor.offset[0] *= draw_resolution_scale_x;
  scissor.offset[1] *= draw_resolution_scale_y;
  scissor.extent[0] *= draw_resolution_scale_x;
  scissor.extent[1] *= draw_resolution_scale_y;

  // Update viewport, scissor, blend factor and stencil reference.
  {
    SCOPE_profile_cpu_f("DrawFixedFunctionState");
    UpdateFixedFunctionState(viewport_info, scissor, primitive_polygonal, normalized_depth_control);
  }

  // Update system constants before uploading them.
  // TODO(Triang3l): With ROV, pass the disabled render target mask for safety.
  {
    SCOPE_profile_cpu_f("DrawSystemConstants");
    UpdateSystemConstantValues(memexport_used, primitive_polygonal,
                               primitive_processing_result.line_loop_closing_index,
                               primitive_processing_result.host_shader_index_endian, viewport_info,
                               used_texture_mask, normalized_depth_control, normalized_color_mask);
  }

  // Update constant buffers, descriptors and root parameters.
  {
    SCOPE_profile_cpu_f("DrawBindingsUpdate");
    if (!UpdateBindings(vertex_shader, pixel_shader, root_signature, memexport_used)) {
      return false;
    }
  }
  // Must not call anything that can change the descriptor heap from now on!

  {
    // Round 21: vertex-buffer-residency check/upload -- one of the
    // costs a hoisted-earlier draw-skip predicate would need to
    // either still pay or prove unnecessary; see the DrawPrimitiveProcess
    // zone's comment above for context.
    SCOPE_profile_cpu_f("DrawVertexBufferResidency");
  // Ensure vertex buffers are resident.
  const Shader::ConstantRegisterMap& constant_map_vertex = vertex_shader->constant_register_map();
  for (uint32_t i = 0; i < rex::countof(constant_map_vertex.vertex_fetch_bitmap); ++i) {
    uint32_t vfetch_bits_remaining = constant_map_vertex.vertex_fetch_bitmap[i];
    uint32_t j;
    while (rex::bit_scan_forward(vfetch_bits_remaining, &j)) {
      vfetch_bits_remaining &= ~(uint32_t(1) << j);
      uint32_t vfetch_index = i * 32 + j;
      uint64_t vfetch_bit = uint64_t(1) << (vfetch_index & 63);
      if (vertex_buffers_in_sync_[vfetch_index >> 6] & vfetch_bit) {
        continue;
      }
      xenos::xe_gpu_vertex_fetch_t vfetch_constant = regs.GetVertexFetch(vfetch_index);
      switch (vfetch_constant.type) {
        case xenos::FetchConstantType::kVertex:
          break;
        case xenos::FetchConstantType::kInvalidVertex:
          if (REXCVAR_GET(gpu_allow_invalid_fetch_constants)) {
            break;
          }
          REXGPU_WARN(
              "Vertex fetch constant {} ({:08X} {:08X}) has \"invalid\" type! "
              "This is incorrect behavior, but you can try bypassing this by "
              "launching Xenia with --gpu_allow_invalid_fetch_constants=true.",
              vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
        default:
          REXGPU_WARN("Vertex fetch constant {} ({:08X} {:08X}) is completely invalid!",
                      vfetch_index, vfetch_constant.dword_0, vfetch_constant.dword_1);
          return false;
      }
      // Round 16 (Fable2PC, fortieth follow-up): see gpu_fetch_watch_address's
      // own comment. Checked once per vfetch_index actually used by this
      // draw's vertex shader, BEFORE the two paths below diverge, because
      // the "already in sync" fast path jumps straight to its own continue
      // statement, skipping anything placed after it -- a transient
      // buffer's address should differ every draw (that's what makes it
      // transient), so in practice this almost always takes the
      // RequestRange path, but checking both keeps this correct rather
      // than assuming which one fires for a given capture.
      const uint32_t watch_address_bytes = REXCVAR_GET(gpu_fetch_watch_address);
      const bool is_watched_vfetch =
          watch_address_bytes != 0 && (vfetch_constant.address << 2) == watch_address_bytes;

      VertexBufferState& state = vertex_buffer_states_[vfetch_index];
      if (state.address == vfetch_constant.address && state.size == vfetch_constant.size) {
        vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
        if (is_watched_vfetch) {
          LogWatchedVertexFetchCapture(vfetch_index, vfetch_constant, *vertex_shader, memory_,
                                       "already-in-sync");
        }
        continue;
      }
      if (!shared_memory_->RequestRange(vfetch_constant.address << 2, vfetch_constant.size << 2)) {
        REXGPU_ERROR(
            "Failed to request vertex buffer at 0x{:08X} (size {}) in the "
            "shared memory",
            vfetch_constant.address << 2, vfetch_constant.size << 2);
        return false;
      }
      state.address = vfetch_constant.address;
      state.size = vfetch_constant.size;
      vertex_buffers_in_sync_[vfetch_index >> 6] |= vfetch_bit;
      if (is_watched_vfetch) {
        LogWatchedVertexFetchCapture(vfetch_index, vfetch_constant, *vertex_shader, memory_,
                                     "request-range");
      }
    }
  }
  }

  // Gather memexport ranges and ensure the heaps for them are resident, and
  // also load the data surrounding the export and to fill the regions that
  // won't be modified by the shaders.
  memexport_ranges_.clear();
  if (memexport_used_vertex) {
    draw_util::AddMemExportRanges(regs, *vertex_shader, memexport_ranges_);
  }
  if (memexport_used_pixel) {
    draw_util::AddMemExportRanges(regs, *pixel_shader, memexport_ranges_);
  }
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    if (!shared_memory_->RequestRange(memexport_range.base_address_dwords << 2,
                                      memexport_range.size_bytes)) {
      REXGPU_ERROR(
          "Failed to request memexport stream at 0x{:08X} (size {}) in the "
          "shared memory",
          memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
      return false;
    }
  }
  if (memexport_used && memexport_ranges_.empty()) {
    if (!shared_memory_->RequestRange(0, SharedMemory::kBufferSize)) {
      REXGPU_ERROR(
          "Failed to request full shared memory residency for unresolved "
          "memexport destinations");
      return false;
    }
  }

  // Primitive topology.
  D3D_PRIMITIVE_TOPOLOGY primitive_topology;
  if (primitive_processing_result.IsTessellated()) {
    switch (primitive_processing_result.host_primitive_type) {
      // TODO(Triang3l): Support all primitive types.
      case xenos::PrimitiveType::kTriangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kTrianglePatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_3_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      case xenos::PrimitiveType::kQuadPatch:
        primitive_topology =
            (regs.Get<reg::VGT_HOS_CNTL>().tess_mode == xenos::TessellationMode::kAdaptive)
                ? D3D_PRIMITIVE_TOPOLOGY_4_CONTROL_POINT_PATCHLIST
                : D3D_PRIMITIVE_TOPOLOGY_1_CONTROL_POINT_PATCHLIST;
        break;
      default:
        REXGPU_ERROR(
            "Host tessellated primitive type {} returned by the primitive "
            "processor is not supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  } else {
    switch (primitive_processing_result.host_primitive_type) {
      case xenos::PrimitiveType::kPointList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
        break;
      case xenos::PrimitiveType::kLineList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST;
        break;
      case xenos::PrimitiveType::kLineStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINESTRIP;
        break;
      case xenos::PrimitiveType::kTriangleList:
      case xenos::PrimitiveType::kRectangleList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
        break;
      case xenos::PrimitiveType::kTriangleStrip:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP;
        break;
      case xenos::PrimitiveType::kQuadList:
        primitive_topology = D3D_PRIMITIVE_TOPOLOGY_LINELIST_ADJ;
        break;
      default:
        REXGPU_ERROR(
            "Host primitive type {} returned by the primitive processor is not "
            "supported by the Direct3D 12 command processor",
            uint32_t(primitive_processing_result.host_primitive_type));
        assert_unhandled_case(primitive_processing_result.host_primitive_type);
        return false;
    }
  }
  SetPrimitiveTopology(primitive_topology);
  // Must not call anything that may change the primitive topology from now on!

  {
    // Round 21 (2026-09-19): everything from here through the actual
    // dispatch call(s) -- Round 14's own predicate plus both
    // D3DDraw(Indexed)Instanced branches and the post-dispatch memexport
    // writeback bookkeeping. Compared against the setup zones above
    // (DrawPrimitiveProcess through DrawVertexBufferResidency), this
    // answers the question native/performance-investigation-phase4.md's
    // Round 14 section left open: how much of IssueDraw's total per-draw
    // cost is concentrated here at the tail (what Round 14 already
    // elides on a skip) versus spread through setup (what it doesn't).
    SCOPE_profile_cpu_f("DrawDispatchTail");
  // Round 14 (2026-09-18, optimization prototype -- see
  // native/performance-investigation-phase4.md, Round 14): exact-repeat
  // draw skip. Unlike Round 9/10/13's signatures above (diagnostic only,
  // never change what's submitted), this CAN suppress a real draw when
  // gpu_exact_draw_dedup_active is also on. Two independent gates:
  //   gpu_exact_draw_dedup        -- master enable
  //   gpu_exact_draw_dedup_active -- when the master is also on, actually
  //                                  suppress a match instead of only
  //                                  counting what WOULD be skipped
  // combine2/exact_dedup_hash_a/exact_dedup_hash_b are declared here (not
  // inside the `if`) so the indexed-draw branch below can finish the hash
  // with the index-buffer-view fields, which aren't built yet at this
  // point -- see the two dispatch branches below.
  bool exact_dedup_want = REXCVAR_GET(gpu_exact_draw_dedup);
  bool exact_dedup_eligible = false;
  uint64_t exact_dedup_hash_a = 0xcbf29ce484222325ULL;
  uint64_t exact_dedup_hash_b = 0x9E3779B97F4A7C15ULL;
  // Two independent FNV-1a-family chains (different offset basis, and
  // chain B additionally salts + rotates + uses a different multiplier)
  // as a practical ~128-bit-strength fingerprint. This predicate can
  // change what's rendered, so a single 64-bit hash's collision odds --
  // already vanishingly small against an effectively 1-entry comparison
  // window, but not zero -- aren't treated as good enough alone.
  auto exact_dedup_combine2 = [](uint64_t& ha, uint64_t& hb, uint64_t v) {
    ha ^= v;
    ha *= 0x100000001B3ULL;
    hb ^= (v ^ 0xD1B54A32D192ED03ULL);
    hb = (hb << 17) | (hb >> 47);
    hb *= 0xFF51AFD7ED558CCDULL;
  };
  if (exact_dedup_want) {
    ++exact_dedup_draws_considered_;
    // Categorical exclusions: any of these means the draw's visible
    // output either isn't a pure function of state this predicate can
    // cheaply compare, or that repeating it is NOT equivalent to running
    // it once even when the state matches exactly (accumulating/
    // ordering-sensitive side effects). Conservative, not exhaustive --
    // see native/performance-investigation-phase4.md, Round 14, for the
    // reasoning behind each one, including two that a state-equality
    // hash alone cannot catch (blend accumulation, stencil-op
    // accumulation) and why pipeline_handle equality does NOT prove
    // render-target identity (it's a PSO cache key -- format/count
    // compatible, not "the same actual resource").
    bool unsafe = memexport_used ||               // side effect: memory write
                  active_occlusion_query_.valid ||  // side effect: sample count
                  normalized_depth_control.stencil_enable;  // ordering: stencil-op accumulation
    if (!unsafe) {
      if (host_render_targets_used) {
        for (uint32_t rt_i = 0; rt_i < 4 && !unsafe; ++rt_i) {
          if (!((normalized_color_mask >> (rt_i * 4)) & 0xF)) {
            continue;  // Not written by this draw -- nothing to check.
          }
          auto blendcontrol = regs.Get<reg::RB_BLENDCONTROL>(
              reg::RB_BLENDCONTROL::rt_register_indices[rt_i]);
          bool color_opaque = blendcontrol.color_srcblend == xenos::BlendFactor::kOne &&
                              blendcontrol.color_destblend == xenos::BlendFactor::kZero &&
                              blendcontrol.color_comb_fcn == xenos::BlendOp::kAdd;
          bool alpha_opaque = blendcontrol.alpha_srcblend == xenos::BlendFactor::kOne &&
                              blendcontrol.alpha_destblend == xenos::BlendFactor::kZero &&
                              blendcontrol.alpha_comb_fcn == xenos::BlendOp::kAdd;
          if (!color_opaque || !alpha_opaque) {
            // Real (non-opaque) blending accumulates: drawing the same
            // triangle twice with blending on produces a different final
            // color than drawing it once, even with byte-identical
            // inputs. State-equality alone cannot catch this.
            unsafe = true;
          }
        }
        if (!unsafe) {
          auto transfers = render_target_cache_->GetLastUpdateTransfersForDedup();
          if (transfers != nullptr && !transfers->empty()) {
            // This draw's own RenderTargetCache::Update() (already run,
            // above) performed a transfer/resolve as a side effect of
            // this render target becoming current -- an event boundary,
            // not a state to compare past.
            unsafe = true;
          }
        }
      } else {
        // Not on the host-render-targets path: last_update_accumulated_
        // render_targets() below (RT IDENTITY, not just format) isn't
        // established, so this predicate can't cheaply verify the actual
        // destination resource is the same one.
        unsafe = true;
      }
    }
    if (!unsafe && primitive_processing_result.IsTessellated()) {
      // Hull/domain-shader stages and tessellation-factor state this
      // predicate hasn't traced -- excluded rather than assumed safe.
      unsafe = true;
    }
    if (unsafe) {
      ++exact_dedup_rejected_unsafe_;
      exact_draw_dedup_last_candidate_.valid = false;
    } else {
      exact_dedup_eligible = true;
      uint64_t& a = exact_dedup_hash_a;
      uint64_t& b = exact_dedup_hash_b;
      exact_dedup_combine2(a, b, reinterpret_cast<uintptr_t>(pipeline_handle));
      exact_dedup_combine2(a, b, uint64_t(primitive_topology));
      exact_dedup_combine2(a, b, uint64_t(primitive_processing_result.host_draw_vertex_count));
      auto hash_fetch_slot_ex = [&](uint32_t fetch_index) {
        uint32_t base = XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0 + fetch_index * 6;
        for (uint32_t d = 0; d < 6; ++d) {
          exact_dedup_combine2(a, b, regs.values[base + d]);
        }
      };
      const Shader::ConstantRegisterMap& vfetch_map_ex = vertex_shader->constant_register_map();
      for (uint32_t i = 0; i < rex::countof(vfetch_map_ex.vertex_fetch_bitmap); ++i) {
        uint32_t bits = vfetch_map_ex.vertex_fetch_bitmap[i];
        uint32_t j;
        while (rex::bit_scan_forward(bits, &j)) {
          bits &= ~(uint32_t(1) << j);
          hash_fetch_slot_ex(i * 32 + j);
        }
      }
      uint32_t tex_remaining_ex = used_texture_mask;
      uint32_t tex_index_ex;
      while (rex::bit_scan_forward(tex_remaining_ex, &tex_index_ex)) {
        tex_remaining_ex &= ~(uint32_t(1) << tex_index_ex);
        hash_fetch_slot_ex(tex_index_ex);
      }
      auto hash_float_constants_ex = [&](const Shader::ConstantRegisterMap& map,
                                         uint32_t base_reg) {
        for (uint32_t i = 0; i < rex::countof(map.float_bitmap); ++i) {
          uint64_t bits_f = map.float_bitmap[i];
          uint32_t bit;
          while (rex::bit_scan_forward(bits_f, &bit)) {
            bits_f &= ~(uint64_t(1) << bit);
            uint32_t reg = base_reg + (i << 8) + (bit << 2);
            for (uint32_t d = 0; d < 4; ++d) {
              exact_dedup_combine2(a, b, regs.values[reg + d]);
            }
          }
        }
      };
      hash_float_constants_ex(vertex_shader->constant_register_map(),
                              XE_GPU_REG_SHADER_CONSTANT_000_X);
      if (pixel_shader != nullptr) {
        hash_float_constants_ex(pixel_shader->constant_register_map(),
                                XE_GPU_REG_SHADER_CONSTANT_256_X);
      }
      // Bool + loop constants: unlike float constants, the actual upload
      // path (cbuffer_binding_bool_loop_, below in this file) doesn't
      // filter these by per-shader usage bitmap -- it always uploads the
      // whole 40-dword block unconditionally. Matching that: hash the
      // whole block rather than bitmap-filtering per shader, which is
      // both simpler and strictly more conservative (never under-hashes).
      constexpr uint32_t kBoolLoopWords = 8 + 32;
      for (uint32_t i = 0; i < kBoolLoopWords; ++i) {
        exact_dedup_combine2(a, b, regs.values[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031 + i]);
      }
      // Whole system_constants_ struct: this is the project's own
      // exhaustively dirty-tracked EDRAM/alpha-test/poly-offset/sample-
      // count/vertex-index-interpretation/tessellation-range constant
      // block (see UpdateSystemConstantValues above). Hashing it
      // wholesale is deliberately preferred over hand-picking fields
      // from it -- an earlier draft of this predicate manually
      // reconstructed viewport/blend/stencil state field by field and,
      // in doing so, missed alpha_test_reference and vertex_index_
      // endian/offset/min/max entirely. See phase4.md, Round 14.
      const uint32_t* sc_words = reinterpret_cast<const uint32_t*>(&system_constants_);
      for (size_t i = 0; i < sizeof(system_constants_) / sizeof(uint32_t); ++i) {
        exact_dedup_combine2(a, b, sc_words[i]);
      }
      // Real D3D12 fixed-function state (viewport/scissor/blend-factor/
      // stencil-ref) -- separate from system_constants_, which only
      // covers the ROV/software-EDRAM-emulation path's equivalents.
      const uint32_t* vp_words = reinterpret_cast<const uint32_t*>(&ff_viewport_);
      for (uint32_t i = 0; i < sizeof(ff_viewport_) / sizeof(uint32_t); ++i) {
        exact_dedup_combine2(a, b, vp_words[i]);
      }
      const uint32_t* scissor_words = reinterpret_cast<const uint32_t*>(&ff_scissor_);
      for (uint32_t i = 0; i < sizeof(ff_scissor_) / sizeof(uint32_t); ++i) {
        exact_dedup_combine2(a, b, scissor_words[i]);
      }
      const uint32_t* blend_factor_words = reinterpret_cast<const uint32_t*>(ff_blend_factor_);
      for (uint32_t i = 0; i < 4; ++i) {
        exact_dedup_combine2(a, b, blend_factor_words[i]);
      }
      exact_dedup_combine2(a, b, uint64_t(ff_stencil_ref_));
      // Render-target IDENTITY (actual bound resource, not just format):
      // pipeline_handle alone only proves format/count compatibility.
      auto bound_rts = render_target_cache_->GetLastUpdateAccumulatedRenderTargetsForDedup();
      for (uint32_t i = 0; i < 1 + xenos::kMaxColorRenderTargets; ++i) {
        exact_dedup_combine2(a, b, reinterpret_cast<uintptr_t>(bound_rts[i]));
      }
      if (index_buffer_info != nullptr) {
        exact_dedup_combine2(a, b, index_buffer_info->guest_base);
        exact_dedup_combine2(a, b, uint64_t(index_buffer_info->length));
        exact_dedup_combine2(a, b, uint32_t(index_buffer_info->format));
      }
      // Non-indexed draws' hash is already complete here (host_draw_
      // vertex_count above fully describes the geometry range). Indexed
      // draws finish the hash with the index-buffer-VIEW fields (the
      // actual GPU VA/size/format ultimately submitted, the most
      // authoritative source, covering scratch-buffer copies too) once
      // index_buffer_view is built, in that branch below.
    }
  }

  // Draw.
  if (primitive_processing_result.index_buffer_type ==
      PrimitiveProcessor::ProcessedIndexBufferType::kNone) {
    // Round 14: non-indexed draws' hash is already complete (no index
    // buffer view to add) -- decide match/skip now. See phase4.md,
    // Round 14, and the parallel block in the indexed branch below.
    bool exact_dedup_skip = false;
    if (exact_dedup_eligible) {
      if (exact_draw_dedup_last_candidate_.valid &&
          exact_draw_dedup_last_candidate_.hash_a == exact_dedup_hash_a &&
          exact_draw_dedup_last_candidate_.hash_b == exact_dedup_hash_b) {
        ++exact_dedup_draws_skipped_;
        exact_dedup_skip = REXCVAR_GET(gpu_exact_draw_dedup_active);
      } else {
        ++exact_dedup_rejected_state_differed_;
      }
      exact_draw_dedup_last_candidate_.valid = true;
      exact_draw_dedup_last_candidate_.hash_a = exact_dedup_hash_a;
      exact_draw_dedup_last_candidate_.hash_b = exact_dedup_hash_b;
    }
    if (!exact_dedup_skip) {
      if (memexport_used) {
        shared_memory_->UseForWriting();
      } else {
        shared_memory_->UseForReading();
      }
    }
    SubmitBarriers();
    if (!exact_dedup_skip) {
      PROFILE_DRAW_CALL();
      PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
      deferred_command_list_.D3DDrawInstanced(primitive_processing_result.host_draw_vertex_count,
                                              1, 0, 0);
    }
  } else {
    D3D12_INDEX_BUFFER_VIEW index_buffer_view;
    index_buffer_view.SizeInBytes = primitive_processing_result.host_draw_vertex_count;
    if (primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16) {
      index_buffer_view.SizeInBytes *= sizeof(uint16_t);
      index_buffer_view.Format = DXGI_FORMAT_R16_UINT;
    } else {
      index_buffer_view.SizeInBytes *= sizeof(uint32_t);
      index_buffer_view.Format = DXGI_FORMAT_R32_UINT;
    }
    ID3D12Resource* scratch_index_buffer = nullptr;
    switch (primitive_processing_result.index_buffer_type) {
      case PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA: {
        if (memexport_used) {
          // If the shared memory is a UAV, it can't be used as an index buffer
          // (UAV is a read/write state, index buffer is a read-only state).
          // Need to copy the indices to a buffer in the index buffer state.
          scratch_index_buffer = RequestScratchGPUBuffer(index_buffer_view.SizeInBytes,
                                                         D3D12_RESOURCE_STATE_COPY_DEST);
          if (scratch_index_buffer == nullptr) {
            return false;
          }
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          deferred_command_list_.D3DCopyBufferRegion(
              scratch_index_buffer, 0, shared_memory_->GetBuffer(),
              primitive_processing_result.guest_index_base, index_buffer_view.SizeInBytes);
          PushTransitionBarrier(scratch_index_buffer, D3D12_RESOURCE_STATE_COPY_DEST,
                                D3D12_RESOURCE_STATE_INDEX_BUFFER);
          index_buffer_view.BufferLocation = scratch_index_buffer->GetGPUVirtualAddress();
        } else {
          index_buffer_view.BufferLocation =
              shared_memory_->GetGPUAddress() + primitive_processing_result.guest_index_base;
        }
      } break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostConverted:
        index_buffer_view.BufferLocation = primitive_processor_->GetConvertedIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForAuto:
      case PrimitiveProcessor::ProcessedIndexBufferType::kHostBuiltinForDMA:
        index_buffer_view.BufferLocation = primitive_processor_->GetBuiltinIndexBufferGpuAddress(
            primitive_processing_result.host_index_buffer_handle);
        break;
      default:
        assert_unhandled_case(primitive_processing_result.index_buffer_type);
        return false;
    }
    // Round 14: finish the hash with the index-buffer VIEW's fields --
    // the actual GPU VA/size/format ultimately submitted, the most
    // authoritative source available. This also means a memexport
    // scratch-buffer copy (RequestScratchGPUBuffer above) could never
    // accidentally produce a stable match even if reached -- moot here
    // since memexport is already categorically excluded above, but worth
    // noting as a second, independent reason it's safe.
    bool exact_dedup_skip = false;
    if (exact_dedup_eligible) {
      exact_dedup_combine2(exact_dedup_hash_a, exact_dedup_hash_b,
                           uint64_t(index_buffer_view.BufferLocation));
      exact_dedup_combine2(exact_dedup_hash_a, exact_dedup_hash_b,
                           uint64_t(index_buffer_view.SizeInBytes));
      exact_dedup_combine2(exact_dedup_hash_a, exact_dedup_hash_b,
                           uint32_t(index_buffer_view.Format));
      if (exact_draw_dedup_last_candidate_.valid &&
          exact_draw_dedup_last_candidate_.hash_a == exact_dedup_hash_a &&
          exact_draw_dedup_last_candidate_.hash_b == exact_dedup_hash_b) {
        ++exact_dedup_draws_skipped_;
        exact_dedup_skip = REXCVAR_GET(gpu_exact_draw_dedup_active);
      } else {
        ++exact_dedup_rejected_state_differed_;
      }
      exact_draw_dedup_last_candidate_.valid = true;
      exact_draw_dedup_last_candidate_.hash_a = exact_dedup_hash_a;
      exact_draw_dedup_last_candidate_.hash_b = exact_dedup_hash_b;
    }
    UINT p1117_start_index = 0;  // [PERF-1117]
    if (!exact_dedup_skip) {
      // [PERF-1117]: guest DMA indices through one view of the whole shared
      // memory; the draw's offset goes into StartIndexLocation.
      bool p1117_whole_view = false;
      if (P1117().ib_view && !memexport_used &&
          primitive_processing_result.index_buffer_type ==
              PrimitiveProcessor::ProcessedIndexBufferType::kGuestDMA) {
        const uint32_t index_size_log2 =
            primitive_processing_result.host_index_format == xenos::IndexFormat::kInt16 ? 1 : 2;
        const uint32_t index_base = primitive_processing_result.guest_index_base;
        if (!(index_base & ((uint32_t(1) << index_size_log2) - 1))) {
          p1117_whole_view = true;
          p1117_start_index = index_base >> index_size_log2;
          const D3D12_GPU_VIRTUAL_ADDRESS location = shared_memory_->GetGPUAddress();
          if (!g_p1117_ib.valid || g_p1117_ib.location != location ||
              g_p1117_ib.size != SharedMemory::kBufferSize ||
              g_p1117_ib.format != index_buffer_view.Format) {
            D3D12_INDEX_BUFFER_VIEW whole_view;
            whole_view.BufferLocation = location;
            whole_view.SizeInBytes = SharedMemory::kBufferSize;
            whole_view.Format = index_buffer_view.Format;
            deferred_command_list_.D3DIASetIndexBuffer(&whole_view);
            g_p1117_ib.location = location;
            g_p1117_ib.size = SharedMemory::kBufferSize;
            g_p1117_ib.format = whole_view.Format;
            g_p1117_ib.valid = true;
          }
        }
      }
      if (!p1117_whole_view) {
        deferred_command_list_.D3DIASetIndexBuffer(&index_buffer_view);
        g_p1117_ib.valid = false;
      }
      if (memexport_used) {
        shared_memory_->UseForWriting();
      } else {
        shared_memory_->UseForReading();
      }
    }
    SubmitBarriers();
    if (!exact_dedup_skip) {
      PROFILE_DRAW_CALL();
      PROFILE_VERTICES(primitive_processing_result.host_draw_vertex_count);
      deferred_command_list_.D3DDrawIndexedInstanced(
          primitive_processing_result.host_draw_vertex_count, 1, p1117_start_index, 0, 0);
    }
    if (scratch_index_buffer != nullptr) {
      ReleaseScratchGPUBuffer(scratch_index_buffer, D3D12_RESOURCE_STATE_INDEX_BUFFER);
    }
  }

  if (memexport_used) {
    // Make sure this memexporting draw is ordered with other work using shared
    // memory as a UAV.
    // TODO(Triang3l): Find some PM4 command that can be used for indication of
    // when memexports should be awaited?
    shared_memory_->MarkUAVWritesCommitNeeded();
    // Invalidate textures in memexported memory and watch for changes.
    if (!memexport_ranges_.empty()) {
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        shared_memory_->RangeWrittenByGpu(memexport_range.base_address_dwords << 2,
                                          memexport_range.size_bytes);
      }
    } else {
      // Stream constants can be invalid or dynamic, so exact destinations may
      // be unknown. Keep invalidation conservative in this case.
      shared_memory_->RangeWrittenByGpu(0, SharedMemory::kBufferSize);
    }
    if (IsReadbackMemexportEnabled(REXCVAR_GET(d3d12_readback_memexport)) &&
        !memexport_ranges_.empty()) {
      uint32_t memexport_total_size = 0;
      for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
        memexport_total_size += memexport_range.size_bytes;
      }
      bool fable2_async_done = false;
      if (memexport_total_size != 0 && REXCVAR_GET(fable2_async_memexport_readback)) {
        // [PERF-1006d]: asynchronous readback, see fable2_async_memexport_readback.
        ID3D12Resource* readback_buffer = Fable2BeginAsyncMemexport(
            GetD3D12Provider().GetDevice(), memexport_total_size);
        if (readback_buffer) {
          shared_memory_->UseAsCopySource();
          SubmitBarriers();
          ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
          std::vector<Fable2MemexportCopy> copies;
          copies.reserve(memexport_ranges_.size());
          uint32_t readback_offset = 0;
          for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
            uint32_t guest_address = memexport_range.base_address_dwords << 2;
            deferred_command_list_.D3DCopyBufferRegion(readback_buffer, readback_offset,
                                                       shared_memory_buffer, guest_address,
                                                       memexport_range.size_bytes);
            copies.push_back({memory_->TranslatePhysical(guest_address), guest_address,
                              memexport_range.size_bytes, readback_offset});
            readback_offset += memexport_range.size_bytes;
          }
          bool waited = false;
          if (Fable2PendingMemexportCount() >= 512) {
            // Pool safety valve: far too many in flight, wait for the oldest.
            CheckSubmissionFence(Fable2OldestPendingMemexportSubmission());
            waited = true;
          }
          Fable2EndAsyncMemexport(submission_fence_, submission_current_, std::move(copies));
          // Hand the copy to the GPU now so the data arrives as early as possible.
          // [PERF-1116]: or, with fable2_memexport_submit_batch_draws, a few
          // draws later together with the following memexport copies.
          const uint32_t fable2_batch = REXCVAR_GET(fable2_memexport_submit_batch_draws);
          if (fable2_batch == 0) {
            if (!scratch_buffer_used_ && CanEndSubmissionImmediately()) {
              EndSubmission(false);
            }
          } else if (!fable2_memexport_flush_pending_) {
            fable2_memexport_flush_pending_ = true;
            fable2_memexport_flush_countdown_ = fable2_batch;
          }
          Fable2PerfCountMemexport(waited);
          fable2_async_done = true;
        }
      }
      if (memexport_total_size != 0 && !fable2_async_done) {
        Fable2PerfCountMemexport(true);
        if (REXCVAR_GET(readback_memexport_fast)) {
          IssueDraw_MemexportReadbackFastPath(memexport_total_size);
        } else {
          IssueDraw_MemexportReadbackFullPath(memexport_total_size);
        }
      }
    }
  }

  return true;
  }
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFullPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  ID3D12Resource* readback_buffer = RequestReadbackBuffer(total_size);
  if (!readback_buffer) {
    return true;
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_buffer_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback_buffer, readback_buffer_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_buffer_offset += memexport_range.size_bytes;
  }

  if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  D3D12_RANGE readback_range = {};
  readback_range.Begin = 0;
  readback_range.End = total_size;
  void* readback_mapping = nullptr;
  if (FAILED(readback_buffer->Map(0, &readback_range, &readback_mapping))) {
    return true;
  }

  const uint8_t* readback_bytes = reinterpret_cast<const uint8_t*>(readback_mapping);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }

  D3D12_RANGE readback_write_range = {};
  readback_buffer->Unmap(0, &readback_write_range);
  return true;
}

bool D3D12CommandProcessor::IssueDraw_MemexportReadbackFastPath(uint32_t total_size) {
  if (!total_size || memexport_ranges_.empty()) {
    return true;
  }

  const uint64_t readback_key =
      MakeMemexportReadbackKey(memexport_ranges_.front().base_address_dwords, total_size);
  ReadbackBuffer& readback = memexport_readback_buffers_[readback_key];
  readback.last_used_frame = frame_current_;

  auto ensure_readback_slot = [&](uint32_t index, uint32_t size) -> bool {
    if (readback.buffers[index] && readback.mapped_data[index] && size <= readback.sizes[index]) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      return false;
    }

    D3D12_RANGE read_range = {0, size};
    void* mapped_data = nullptr;
    if (FAILED(buffer->Map(0, &read_range, &mapped_data))) {
      buffer->Release();
      return false;
    }

    if (readback.buffers[index]) {
      if (!AwaitAllQueueOperationsCompletion()) {
        buffer->Unmap(0, nullptr);
        buffer->Release();
        return false;
      }
      if (readback.mapped_data[index]) {
        readback.buffers[index]->Unmap(0, nullptr);
      }
      readback.buffers[index]->Release();
    }

    readback.buffers[index] = buffer;
    readback.mapped_data[index] = mapped_data;
    readback.sizes[index] = size;
    readback.submission_written[index] = 0;
    readback.written_size[index] = 0;
    return true;
  };

  const uint32_t write_index = readback.current_index;
  const uint32_t read_index = 1 - write_index;
  const uint32_t readback_size = AlignReadbackBufferSize(total_size);
  if (!ensure_readback_slot(write_index, readback_size)) {
    return IssueDraw_MemexportReadbackFullPath(total_size);
  }

  shared_memory_->UseAsCopySource();
  SubmitBarriers();
  ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
  uint32_t readback_offset = 0;
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    deferred_command_list_.D3DCopyBufferRegion(
        readback.buffers[write_index], readback_offset, shared_memory_buffer,
        memexport_range.base_address_dwords << 2, memexport_range.size_bytes);
    readback_offset += memexport_range.size_bytes;
  }
  readback.submission_written[write_index] = submission_current_;
  readback.written_size[write_index] = total_size;

  CheckSubmissionFence(0);
  bool previous_slot_ready = readback.buffers[read_index] && readback.mapped_data[read_index] &&
                             total_size <= readback.sizes[read_index] &&
                             total_size <= readback.written_size[read_index] &&
                             readback.submission_written[read_index] &&
                             readback.submission_written[read_index] <= submission_completed_;
  if (!previous_slot_ready) {
    IssueDraw_MemexportReadbackFullPath(total_size);
    readback.current_index = read_index;
    return true;
  }

  const uint8_t* readback_bytes = static_cast<const uint8_t*>(readback.mapped_data[read_index]);
  for (const draw_util::MemExportRange& memexport_range : memexport_ranges_) {
    std::memcpy(memory_->TranslatePhysical(memexport_range.base_address_dwords << 2),
                readback_bytes, memexport_range.size_bytes);
    readback_bytes += memexport_range.size_bytes;
  }
  readback.current_index = read_index;
  return true;
}

bool D3D12CommandProcessor::IssueCopy() {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES
  // Round 14 (2026-09-18): a resolve/copy is exactly the kind of event
  // boundary the exact-draw-dedup predicate must not carry a candidate
  // across -- it can mutate render-target/EDRAM content in ways the
  // predicate's state comparison doesn't observe. See
  // native/performance-investigation-phase4.md, Round 14.
  exact_draw_dedup_last_candidate_.valid = false;
  if (!BeginSubmission(true)) {
    return false;
  }
  // Temporary bounded discovery: record actual resolve signatures once each.
  // This identifies allocation differences from the reference implementation.
  {
    const auto draw = register_file_->Get<reg::VGT_DRAW_INITIATOR>();
    const auto format = register_file_->Get<reg::RB_COPY_DEST_INFO>().copy_dest_format;
    const auto source = register_file_->Get<reg::RB_COPY_CONTROL>().copy_src_select;
    const auto address = register_file_->values[XE_GPU_REG_RB_COPY_DEST_BASE];
    static std::set<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>> seen;
    const auto signature = std::make_tuple(address, uint32_t(format), uint32_t(source),
                                          uint32_t(draw.prim_type), uint32_t(draw.num_indices));
    if (seen.size() < 256 && seen.insert(signature).second) {
      REXGPU_INFO("Fable II resolve probe: title={:08X}, dest={:08X}, format={}, source={}, primitive={}, count={}",
                  kernel_state_->title_id(), address, uint32_t(format), uint32_t(source),
                  uint32_t(draw.prim_type), uint32_t(draw.num_indices));
    }
  }
  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  if (readback_mode == ReadbackResolveMode::kDisabled &&
      !NeedsFable2MorphReadback(*register_file_, kernel_state_->title_id())) {
    uint32_t written_address, written_length;
    return render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_,
                                         written_address, written_length);
  }
  return IssueCopy_ReadbackResolvePath();
}

bool D3D12CommandProcessor::IssueCopy_ReadbackResolvePath() {
  uint32_t written_address, written_length;
  if (!render_target_cache_->Resolve(*memory_, *shared_memory_, *texture_cache_, written_address,
                                     written_length)) {
    return false;
  }

  if (!written_length) {
    return true;
  }

  if (!memory_->TranslatePhysical(written_address)) {
    return true;
  }

  bool is_scaled = texture_cache_->IsDrawResolutionScaled();
  uint64_t resolve_key = MakeReadbackResolveKey(written_address, written_length);
  ReadbackBuffer& rb = readback_buffers_[resolve_key];
  rb.last_used_frame = frame_current_;

  uint32_t write_index = rb.current_index;
  uint32_t size = AlignReadbackBufferSize(written_length);

  if (size > rb.sizes[write_index]) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer = nullptr;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return true;
    }
    if (rb.buffers[write_index]) {
      if (rb.mapped_data[write_index]) {
        rb.buffers[write_index]->Unmap(0, nullptr);
        rb.mapped_data[write_index] = nullptr;
      }
      rb.buffers[write_index]->Release();
    }
    rb.buffers[write_index] = buffer;
    rb.sizes[write_index] = size;
    D3D12_RANGE read_range = {0, size};
    if (FAILED(buffer->Map(0, &read_range, &rb.mapped_data[write_index]))) {
      REXGPU_ERROR("Failed to persistently map resolve readback buffer");
      rb.mapped_data[write_index] = nullptr;
    }
  }

  if (!rb.buffers[write_index]) {
    return true;
  }

  if (is_scaled) {
    if (!resolve_downscale_pipeline_ || !resolve_downscale_root_signature_) {
      return true;
    }

    reg::RB_COPY_DEST_INFO copy_dest_info = register_file_->Get<reg::RB_COPY_DEST_INFO>();
    const FormatInfo* format_info = FormatInfo::Get(uint32_t(copy_dest_info.copy_dest_format));
    uint32_t bits_per_pixel = format_info->bits_per_pixel;
    if (bits_per_pixel != 8 && bits_per_pixel != 16 && bits_per_pixel != 32 &&
        bits_per_pixel != 64) {
      return true;
    }

    uint32_t pixel_size_log2;
    if (!rex::bit_scan_forward(bits_per_pixel >> 3, &pixel_size_log2)) {
      return true;
    }
    uint32_t tile_size_1x = 32 * 32 * (uint32_t(1) << pixel_size_log2);
    uint32_t tile_count = written_length / tile_size_1x;
    if (!tile_count) {
      return true;
    }

    uint32_t scaled_length = uint32_t(texture_cache_->GetCurrentScaledResolveRangeLengthScaled());
    uint64_t scaled_address = texture_cache_->GetCurrentScaledResolveRangeStartScaled();
    if (!scaled_length) {
      return true;
    }

    uint32_t downscale_buffer_size = AlignReadbackBufferSize(written_length);
    if (downscale_buffer_size > resolve_downscale_buffer_size_) {
      const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
      ID3D12Device* device = provider.GetDevice();
      D3D12_RESOURCE_DESC buffer_desc;
      ui::d3d12::util::FillBufferResourceDesc(buffer_desc, downscale_buffer_size,
                                              D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
      ID3D12Resource* buffer = nullptr;
      if (FAILED(device->CreateCommittedResource(
              &ui::d3d12::util::kHeapPropertiesDefault, provider.GetHeapFlagCreateNotZeroed(),
              &buffer_desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
              IID_PPV_ARGS(&buffer)))) {
        REXGPU_ERROR("Failed to create a {} MB resolve downscale buffer",
                     downscale_buffer_size >> 20);
        return true;
      }
      if (resolve_downscale_buffer_) {
        resources_for_deletion_.emplace_back(GetCurrentSubmission(),
                                             resolve_downscale_buffer_.Detach());
      }
      resolve_downscale_buffer_.Attach(buffer);
      resolve_downscale_buffer_size_ = downscale_buffer_size;
    }

    if (!resolve_downscale_buffer_) {
      return true;
    }

    ID3D12Resource* scaled_resolve_buffer = texture_cache_->GetCurrentScaledResolveBufferResource();
    size_t scaled_resolve_buffer_index = texture_cache_->GetCurrentScaledResolveBufferIndexPublic();
    if (!scaled_resolve_buffer) {
      return true;
    }
    uint64_t scaled_buffer_base = uint64_t(scaled_resolve_buffer_index) << 30;
    if (scaled_address < scaled_buffer_base) {
      return true;
    }
    uint64_t source_offset = scaled_address - scaled_buffer_base;

    ui::d3d12::util::DescriptorCpuGpuHandlePair downscale_descriptors[2];
    if (!RequestOneUseSingleViewDescriptors(2, downscale_descriptors)) {
      return true;
    }

    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    uint32_t aligned_scaled_length =
        rex::align(scaled_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawSRV(device, downscale_descriptors[0].first,
                                        scaled_resolve_buffer, aligned_scaled_length,
                                        source_offset);
    uint32_t aligned_written_length =
        rex::align(written_length, uint32_t(D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT));
    ui::d3d12::util::CreateBufferRawUAV(device, downscale_descriptors[1].first,
                                        resolve_downscale_buffer_.Get(), aligned_written_length, 0);

    PushUAVBarrier(scaled_resolve_buffer);
    texture_cache_->TransitionCurrentScaledResolveRange(
        D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    SubmitBarriers();

    SetExternalPipeline(resolve_downscale_pipeline_.Get());
    deferred_command_list_.D3DSetComputeRootSignature(resolve_downscale_root_signature_.Get());
    ResolveDownscaleConstants constants;
    constants.scale_x = texture_cache_->draw_resolution_scale_x();
    constants.scale_y = texture_cache_->draw_resolution_scale_y();
    constants.pixel_size_log2 = pixel_size_log2;
    constants.tile_count = tile_count;
    constants.half_pixel_offset = (REXCVAR_GET(readback_resolve_half_pixel_offset) &&
                                   (constants.scale_x > 1 || constants.scale_y > 1))
                                      ? 1u
                                      : 0u;
    deferred_command_list_.D3DSetComputeRoot32BitConstants(
        UINT(ResolveDownscaleRootParameter::kConstants), sizeof(constants) / sizeof(uint32_t),
        &constants, 0);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kSource), downscale_descriptors[0].second);
    deferred_command_list_.D3DSetComputeRootDescriptorTable(
        UINT(ResolveDownscaleRootParameter::kDestination), downscale_descriptors[1].second);
    deferred_command_list_.D3DDispatch(tile_count, 1, 1);

    PushUAVBarrier(resolve_downscale_buffer_.Get());
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_UNORDERED_ACCESS,
                          D3D12_RESOURCE_STATE_COPY_SOURCE);
    SubmitBarriers();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0,
                                               resolve_downscale_buffer_.Get(), 0, written_length);
    PushTransitionBarrier(resolve_downscale_buffer_.Get(), D3D12_RESOURCE_STATE_COPY_SOURCE,
                          D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    texture_cache_->TransitionCurrentScaledResolveRange(D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    SubmitBarriers();
  } else {
    shared_memory_->UseAsCopySource();
    SubmitBarriers();
    ID3D12Resource* shared_memory_buffer = shared_memory_->GetBuffer();
    deferred_command_list_.D3DCopyBufferRegion(rb.buffers[write_index], 0, shared_memory_buffer,
                                               written_address, written_length);
  }

  ReadbackResolveMode readback_mode = GetReadbackResolveMode(REXCVAR_GET(d3d12_readback_resolve));
  // The CPU consumes this newly generated image, so a previous-frame readback
  // is not sufficient. Keep global readback settings unchanged for other copies.
  const bool fable2_morph = NeedsFable2MorphReadback(*register_file_, kernel_state_->title_id());
  if (fable2_morph) readback_mode = ReadbackResolveMode::kFull;
  bool use_delayed_sync =
      readback_mode == ReadbackResolveMode::kFast || readback_mode == ReadbackResolveMode::kSome;
  uint32_t read_index = write_index;
  if (use_delayed_sync) {
    read_index = 1 - write_index;
  } else if (!AwaitAllQueueOperationsCompletion()) {
    return true;
  }

  bool is_cache_miss = false;
  if (use_delayed_sync && (!rb.buffers[read_index] || written_length > rb.sizes[read_index] ||
                           !rb.mapped_data[read_index])) {
    is_cache_miss = true;
    read_index = write_index;
    if (!AwaitAllQueueOperationsCompletion()) {
      return true;
    }
  }

  bool should_copy = (readback_mode == ReadbackResolveMode::kSome) ? is_cache_miss : true;
  if (should_copy && rb.buffers[read_index] && written_length <= rb.sizes[read_index] &&
      rb.mapped_data[read_index]) {
    uint8_t* destination = memory_->TranslatePhysical(written_address);
    if (destination) {
      // Bounded diagnostics verify actual GPU data differs from the CPU staging
      // bytes; no file contents or save data are logged.
      static uint32_t morph_reports = 0;
      if (fable2_morph && morph_reports < 8) {
        const auto* gpu_bytes = static_cast<const uint8_t*>(rb.mapped_data[read_index]);
        size_t changed = 0, nonzero = 0;
        for (uint32_t i = 0; i < written_length; ++i) {
          changed += destination[i] != gpu_bytes[i];
          nonzero += gpu_bytes[i] != 0;
        }
        REXGPU_INFO("Fable II morph readback: address={:08X}, bytes={}, changed={}, nonzero={}",
                    written_address, written_length, changed, nonzero);
        ++morph_reports;
      }
      std::memcpy(destination, static_cast<uint8_t*>(rb.mapped_data[read_index]), written_length);
    }
  }

  rb.current_index = 1 - rb.current_index;
  return true;
}

// [PERF-1116]: replay thread (cvar fable2_replay_thread).
// Thread-safety argument:
// * Hand-over: EndSubmission (CP thread) first waits until the worker has
//   finished the previous job (Fable2ReplayWait), then swaps the command
//   stream of deferred_command_list_ with the worker's own DeferredCommandList
//   (std::vector::swap, no copy, pointers into the stream stay valid) and
//   posts the job under the mutex. So at most one job is in flight, jobs are
//   executed in submission order, and command_list_ / command_list_1_ are only
//   ever used by one thread at a time (the CP thread touches them nowhere else
//   after initialisation).
// * Fence order: the worker does ExecuteCommandLists and then
//   Signal(submission_fence_, N) for exactly the value the CP thread assigned,
//   so every fence-based reclaim (command allocators, upload pages, one-use
//   descriptors, resources_for_deletion_, readbacks) still only happens after
//   the GPU finished the commands. Waiting for a value the worker has not
//   signalled yet just waits a bit longer; the worker never needs the CP.
// * Command allocator: taken from the writable list and put on the submitted
//   list by the CP thread as before; it only comes back once its fence value
//   (signalled by the worker after executing it) has completed.
// * Other users of the direct queue: swap submissions are synchronous (worker
//   drained first), so the presenter's work queued after RefreshGuestOutput
//   stays behind the frame. CheckSubmissionFence's "await everything" path
//   drains the worker (ShutdownContext, cache clears, AwaitAllQueue...). Tile
//   mapping updates only ever add mappings and happen before the submission
//   that uses them is posted, so their queue order is unchanged where it
//   matters.
// * Replay reads: GetD3D12PipelineByHandle is an atomic load and pipelines are
//   never destroyed; RTV/DSV CPU descriptors recorded in the stream belong to
//   render targets that only die in ClearCache (after a full drain).
namespace {
struct Fable2StreamTag {
  using type = std::vector<uintmax_t> DeferredCommandList::*;
  friend type Fable2StreamMember(Fable2StreamTag);
};
// Explicit instantiation may name a private member: lets this file swap the
// stream without changing the installed SDK header.
template <typename Tag, typename Tag::type kMember>
struct Fable2StreamAccess {
  friend typename Tag::type Fable2StreamMember(Tag) { return kMember; }
};
template struct Fable2StreamAccess<Fable2StreamTag, &DeferredCommandList::command_stream_>;

struct Fable2ReplayJob {
  ID3D12CommandAllocator* allocator = nullptr;
  ID3D12GraphicsCommandList* command_list = nullptr;
  ID3D12GraphicsCommandList1* command_list_1 = nullptr;
  ID3D12CommandQueue* queue = nullptr;
  ID3D12Fence* fence = nullptr;
  uint64_t fence_value = 0;
};
struct Fable2Replay {
  std::unique_ptr<DeferredCommandList> list;  // owned by the worker while a job runs
  std::thread thread;
  std::mutex mutex;
  std::condition_variable job_cv;
  std::condition_variable done_cv;
  Fable2ReplayJob job;
  bool job_ready = false;    // posted, not yet taken by the worker
  bool job_pending = false;  // posted, not yet finished
  bool quit = false;
  bool started = false;
};
// Heap-allocated and never destroyed: the game hard-exits, and a joinable
// std::thread destroyed at DLL unload would call std::terminate.
Fable2Replay& fable2_replay_ = *new Fable2Replay;

void Fable2ReplayRun() {
  Fable2Replay& r = fable2_replay_;
  for (;;) {
    Fable2ReplayJob job;
    {
      std::unique_lock<std::mutex> lock(r.mutex);
      r.job_cv.wait(lock, [&r] { return r.job_ready || r.quit; });
      if (!r.job_ready) {
        return;  // quit with nothing left to do
      }
      job = r.job;
      r.job_ready = false;
    }
    job.allocator->Reset();
    job.command_list->Reset(job.allocator, nullptr);
    r.list->Execute(job.command_list, job.command_list_1);
    job.command_list->Close();
    ID3D12CommandList* execute_command_lists[] = {job.command_list};
    job.queue->ExecuteCommandLists(1, execute_command_lists);
    job.queue->Signal(job.fence, job.fence_value);
    {
      std::lock_guard<std::mutex> lock(r.mutex);
      r.job_pending = false;
    }
    r.done_cv.notify_all();
  }
}

bool Fable2ReplayEnsureStarted(const D3D12CommandProcessor& command_processor) {
  Fable2Replay& r = fable2_replay_;
  if (!r.started) {
    r.list = std::make_unique<DeferredCommandList>(command_processor);
    r.quit = false;
    r.thread = std::thread(Fable2ReplayRun);
    r.started = true;
    REXGPU_INFO("[PERF-1116] replay thread started (fable2_replay_thread=true)");
  }
  return true;
}

// CP thread. Precondition: Fable2ReplayWait() was called (no job pending).
void Fable2ReplayPost(DeferredCommandList& source, const Fable2ReplayJob& job) {
  Fable2Replay& r = fable2_replay_;
  constexpr Fable2StreamTag tag{};
  (source.*Fable2StreamMember(tag)).swap((*r.list).*Fable2StreamMember(tag));
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    r.job = job;
    r.job_ready = true;
    r.job_pending = true;
  }
  r.job_cv.notify_one();
}
}  // namespace

// CP thread: returns once the worker has submitted everything posted so far.
void Fable2ReplayWait() {
  Fable2Replay& r = fable2_replay_;
  if (!r.started) {
    return;
  }
  std::unique_lock<std::mutex> lock(r.mutex);
  if (!r.job_pending) {
    return;
  }
  const int64_t wait_start = Fable2PerfNow();
  r.done_cv.wait(lock, [&r] { return !r.job_pending; });
  Fable2PerfAddReplayWait(Fable2PerfNow() - wait_start);
}

void Fable2ReplayStop() {
  Fable2Replay& r = fable2_replay_;
  if (!r.started) {
    return;
  }
  Fable2ReplayWait();
  {
    std::lock_guard<std::mutex> lock(r.mutex);
    r.quit = true;
  }
  r.job_cv.notify_one();
  r.thread.join();
  r.list.reset();
  r.started = false;
}

void D3D12CommandProcessor::CheckSubmissionFence(uint64_t await_submission) {
  if (await_submission >= submission_current_) {
    if (submission_open_) {
      EndSubmission(false);
    }
    // [PERF-1116]: everything ended so far must be on the queue before the
    // out-of-submission Signal below and before callers that await "all".
    Fable2ReplayWait();
    // Ending an open submission should result in queue operations done directly
    // (like UpdateTileMappings) to be tracked within the scope of that
    // submission, but just in case of a failure, or queue operations being done
    // outside of a submission, await explicitly.
    if (queue_operations_done_since_submission_signal_) {
      UINT64 fence_value = ++queue_operations_since_submission_fence_last_;
      ID3D12CommandQueue* direct_queue = GetD3D12Provider().GetDirectQueue();
      if (SUCCEEDED(direct_queue->Signal(queue_operations_since_submission_fence_, fence_value) &&
                    SUCCEEDED(queue_operations_since_submission_fence_->SetEventOnCompletion(
                        fence_value, fence_completion_event_)))) {
        PROFILE_CMD_BUFFER_STALL();
        const int64_t fable2_wait_start = Fable2PerfNow();  // [PERF-1006b]
        WaitForSingleObject(fence_completion_event_, INFINITE);
        Fable2PerfAddFenceWait(Fable2PerfNow() - fable2_wait_start);
        queue_operations_done_since_submission_signal_ = false;
      } else {
        REXGPU_ERROR(
            "Failed to await an out-of-submission queue operation completion "
            "Direct3D 12 fence");
      }
    }
    // A submission won't be ended if it hasn't been started, or if ending
    // has failed - clamp the index.
    await_submission = submission_current_ - 1;
  }

  uint64_t submission_completed_before = submission_completed_;
  submission_completed_ = submission_fence_->GetCompletedValue();
  Fable2PollPendingQueriesNow();  // [PERF-1006c]
  if (submission_completed_ < await_submission) {
    if (SUCCEEDED(
            submission_fence_->SetEventOnCompletion(await_submission, fence_completion_event_))) {
      PROFILE_CMD_BUFFER_STALL();
      const int64_t fable2_wait_start = Fable2PerfNow();  // [PERF-1006b]
      WaitForSingleObject(fence_completion_event_, INFINITE);
      Fable2PerfAddFenceWait(Fable2PerfNow() - fable2_wait_start);
      submission_completed_ = submission_fence_->GetCompletedValue();
    }
  }
  if (submission_completed_ < await_submission) {
    REXGPU_ERROR("Failed to await a submission completion Direct3D 12 fence");
  }
  if (submission_completed_ <= submission_completed_before) {
    // Not updated - no need to reclaim or download things.
    return;
  }

  // Reclaim command allocators.
  while (command_allocator_submitted_first_) {
    if (command_allocator_submitted_first_->last_usage_submission > submission_completed_) {
      break;
    }
    if (command_allocator_writable_last_) {
      command_allocator_writable_last_->next = command_allocator_submitted_first_;
    } else {
      command_allocator_writable_first_ = command_allocator_submitted_first_;
    }
    command_allocator_writable_last_ = command_allocator_submitted_first_;
    command_allocator_submitted_first_ = command_allocator_submitted_first_->next;
    command_allocator_writable_last_->next = nullptr;
  }
  if (!command_allocator_submitted_first_) {
    command_allocator_submitted_last_ = nullptr;
  }

  // Release single-use bindless descriptors.
  while (!view_bindless_one_use_descriptors_.empty()) {
    if (view_bindless_one_use_descriptors_.front().second > submission_completed_) {
      break;
    }
    ReleaseViewBindlessDescriptorImmediately(view_bindless_one_use_descriptors_.front().first);
    view_bindless_one_use_descriptors_.pop_front();
  }

  // Delete transient resources marked for deletion.
  while (!resources_for_deletion_.empty()) {
    if (resources_for_deletion_.front().first > submission_completed_) {
      break;
    }
    resources_for_deletion_.front().second->Release();
    resources_for_deletion_.pop_front();
  }

  shared_memory_->CompletedSubmissionUpdated();

  render_target_cache_->CompletedSubmissionUpdated();

  primitive_processor_->CompletedSubmissionUpdated();

  texture_cache_->CompletedSubmissionUpdated(submission_completed_);
}

void D3D12CommandProcessor::LogDeviceRemovalDiagnostics(ID3D12Device* device, HRESULT reason) {
  const char* reason_str = "Unknown";
  switch (reason) {
    case DXGI_ERROR_DEVICE_HUNG:
      reason_str = "DEVICE_HUNG (TDR - GPU command took too long)";
      break;
    case DXGI_ERROR_DEVICE_REMOVED:
      reason_str = "DEVICE_REMOVED (driver internal error or hot-unplug)";
      break;
    case DXGI_ERROR_DEVICE_RESET:
      reason_str = "DEVICE_RESET (bad GPU command)";
      break;
    case DXGI_ERROR_DRIVER_INTERNAL_ERROR:
      reason_str = "DRIVER_INTERNAL_ERROR";
      break;
    case DXGI_ERROR_INVALID_CALL:
      reason_str = "INVALID_CALL";
      break;
  }
  REXGPU_ERROR("D3D12 device removed: HRESULT 0x{:08X} - {}", static_cast<unsigned>(reason),
               reason_str);

  Microsoft::WRL::ComPtr<ID3D12DeviceRemovedExtendedData> dred;
  if (FAILED(device->QueryInterface(IID_PPV_ARGS(&dred)))) {
    return;
  }

  D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT breadcrumbs = {};
  if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&breadcrumbs))) {
    for (const D3D12_AUTO_BREADCRUMB_NODE* node = breadcrumbs.pHeadAutoBreadcrumbNode; node;
         node = node->pNext) {
      if (!node->pLastBreadcrumbValue || !node->pCommandHistory ||
          *node->pLastBreadcrumbValue == 0) {
        continue;
      }
      REXGPU_ERROR("DRED breadcrumb: completed {} of {} ops", *node->pLastBreadcrumbValue,
                   node->BreadcrumbCount);
      uint32_t last = std::min(*node->pLastBreadcrumbValue, node->BreadcrumbCount);
      uint32_t start = last > 3 ? last - 3 : 0;
      uint32_t end = std::min(last + 1, node->BreadcrumbCount);
      for (uint32_t i = start; i < end; i++) {
        REXGPU_ERROR("  [{}] op type {}{}", i, static_cast<int>(node->pCommandHistory[i]),
                     i == last ? " <-- FAULT" : "");
      }
    }
  }

  D3D12_DRED_PAGE_FAULT_OUTPUT page_fault = {};
  if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&page_fault)) && page_fault.PageFaultVA != 0) {
    REXGPU_ERROR("DRED page fault at VA 0x{:016X}", page_fault.PageFaultVA);
  }
}

bool D3D12CommandProcessor::BeginSubmission(bool is_guest_command) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  if (device_removed_) {
    return false;
  }

  bool is_opening_frame = is_guest_command && !frame_open_;
  if (submission_open_ && !is_opening_frame) {
    return true;
  }

  // Check if the device is still available.
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  HRESULT device_removed_reason = device->GetDeviceRemovedReason();
  if (FAILED(device_removed_reason)) {
    device_removed_ = true;
    LogDeviceRemovalDiagnostics(device, device_removed_reason);
    if (graphics_system_) {
      graphics_system_->OnHostGpuLossFromAnyThread(device_removed_reason !=
                                                   DXGI_ERROR_DEVICE_REMOVED);
    }
    return false;
  }

  // Check the fence - needed for all kinds of submissions (to reclaim transient
  // resources early) and specifically for frames (not to queue too many), and
  // await the availability of the current frame.
  CheckSubmissionFence(is_opening_frame ? closed_frame_submissions_[frame_current_ % kQueueFrames]
                                        : 0);
  // TODO(Triang3l): If failed to await (completed submission < awaited frame
  // submission), do something like dropping the draw command that wanted to
  // open the frame.
  if (is_opening_frame) {
    // Update the completed frame index, also obtaining the actual completed
    // frame number (since the CPU may be actually less than 3 frames behind)
    // before reclaiming resources tracked with the frame number.
    frame_completed_ = std::max(frame_current_, uint64_t(kQueueFrames)) - kQueueFrames;
    for (uint64_t frame = frame_completed_ + 1; frame < frame_current_; ++frame) {
      if (closed_frame_submissions_[frame % kQueueFrames] > submission_completed_) {
        break;
      }
      frame_completed_ = frame;
    }
  }

  if (!submission_open_) {
    submission_open_ = true;
    g_p1117_ib.valid = false;  // [PERF-1117] new command list, no index buffer set

    // Start a new deferred command list - will submit it to the real one in the
    // end of the submission (when async pipeline creation requests are
    // fulfilled).
    deferred_command_list_.Reset();

    // Reset cached state of the command list.
    ff_viewport_update_needed_ = true;
    ff_scissor_update_needed_ = true;
    ff_blend_factor_update_needed_ = true;
    ff_stencil_ref_update_needed_ = true;
    viewport_cache_valid_ = false;
    current_guest_pipeline_ = nullptr;
    current_external_pipeline_ = nullptr;
    current_graphics_root_signature_ = nullptr;
    current_graphics_root_up_to_date_ = 0;
    if (bindless_resources_used_) {
      deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                sampler_bindless_heap_current_);
    } else {
      view_bindful_heap_current_ = nullptr;
      sampler_bindful_heap_current_ = nullptr;
    }
    primitive_topology_ = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    render_target_cache_->BeginSubmission();

    primitive_processor_->BeginSubmission();

    texture_cache_->BeginSubmission(submission_current_);
  }

  if (is_opening_frame) {
    frame_open_ = true;

    // Reset bindings that depend on the data stored in the pools.
    std::memset(current_float_constant_map_vertex_, 0, sizeof(current_float_constant_map_vertex_));
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
    cbuffer_binding_system_.up_to_date = false;
    cbuffer_binding_float_vertex_.up_to_date = false;
    cbuffer_binding_float_pixel_.up_to_date = false;
    cbuffer_binding_bool_loop_.up_to_date = false;
    cbuffer_binding_fetch_.up_to_date = false;
    current_shared_memory_binding_is_uav_.reset();
    if (bindless_resources_used_) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    } else {
      draw_view_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      draw_sampler_bindful_heap_index_ = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Reclaim pool pages - no need to do this every small submission since some
    // may be reused.
    constant_buffer_pool_->Reclaim(frame_completed_);
    if (!bindless_resources_used_) {
      view_bindful_heap_pool_->Reclaim(frame_completed_);
      sampler_bindful_heap_pool_->Reclaim(frame_completed_);
    }
    EvictOldReadbackBuffers(readback_buffers_);
    EvictOldReadbackBuffers(memexport_readback_buffers_);

    primitive_processor_->BeginFrame();

    texture_cache_->BeginFrame();
  }

  return true;
}

bool D3D12CommandProcessor::EndSubmission(bool is_swap) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();

  // Make sure there is a command allocator to write commands to.
  if (submission_open_ && !command_allocator_writable_first_) {
    ID3D12CommandAllocator* command_allocator;
    if (FAILED(provider.GetDevice()->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                            IID_PPV_ARGS(&command_allocator)))) {
      REXGPU_ERROR("Failed to create a command allocator");
      // Try to submit later. Completely dropping the submission is not
      // permitted because resources would be left in an undefined state.
      return false;
    }
    command_allocator_writable_first_ = new CommandAllocator;
    command_allocator_writable_first_->command_allocator = command_allocator;
    command_allocator_writable_first_->last_usage_submission = 0;
    command_allocator_writable_first_->next = nullptr;
    command_allocator_writable_last_ = command_allocator_writable_first_;
  }

  bool is_closing_frame = is_swap && frame_open_;

  if (is_closing_frame) {
    texture_cache_->EndFrame();

    primitive_processor_->EndFrame();
  }

  if (submission_open_) {
    assert_false(scratch_buffer_used_);

    if (active_occlusion_query_.valid && occlusion_query_heap_) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         active_occlusion_query_.host_index);
      active_occlusion_query_ = {};
    }

    pipeline_cache_->EndSubmission();

    // Submit barriers now because resources with the queued barriers may be
    // destroyed between frames.
    SubmitBarriers();

    ID3D12CommandQueue* direct_queue = provider.GetDirectQueue();

    // Submit the deferred command list.
    // Only one deferred command list must be executed in the same
    // ExecuteCommandLists - the boundaries of ExecuteCommandLists are a full
    // UAV and aliasing barrier, and subsystems of the emulator assume it
    // happens between Xenia submissions.
    ID3D12CommandAllocator* command_allocator =
        command_allocator_writable_first_->command_allocator;
    // [PERF-1116]: the previous replay-thread job (if any) must be on the
    // queue before this one, and command_list_ must be free again.
    Fable2ReplayWait();
    const bool fable2_async_replay = !is_swap && REXCVAR_GET(fable2_replay_thread) &&
                                     Fable2ReplayEnsureStarted(*this);
    if (fable2_async_replay) {
      Fable2ReplayJob fable2_job;
      fable2_job.allocator = command_allocator;
      fable2_job.command_list = command_list_;
      fable2_job.command_list_1 = command_list_1_;
      fable2_job.queue = direct_queue;
      fable2_job.fence = submission_fence_;
      fable2_job.fence_value = submission_current_;
      Fable2ReplayPost(deferred_command_list_, fable2_job);
    } else {
    command_allocator->Reset();
    command_list_->Reset(command_allocator, nullptr);
    deferred_command_list_.Execute(command_list_, command_list_1_);
    command_list_->Close();
    ID3D12CommandList* execute_command_lists[] = {command_list_};
    direct_queue->ExecuteCommandLists(1, execute_command_lists);
    }
    command_allocator_writable_first_->last_usage_submission = submission_current_;
    if (command_allocator_submitted_last_) {
      command_allocator_submitted_last_->next = command_allocator_writable_first_;
    } else {
      command_allocator_submitted_first_ = command_allocator_writable_first_;
    }
    command_allocator_submitted_last_ = command_allocator_writable_first_;
    command_allocator_writable_first_ = command_allocator_writable_first_->next;
    command_allocator_submitted_last_->next = nullptr;
    if (!command_allocator_writable_first_) {
      command_allocator_writable_last_ = nullptr;
    }

    if (!fable2_async_replay) {  // [PERF-1116]: the replay thread signals itself
      direct_queue->Signal(submission_fence_, submission_current_);
    }
    ++submission_current_;

    submission_open_ = false;
    fable2_draws_in_submission_ = 0;  // [PERF-1006b]
    fable2_memexport_flush_pending_ = false;  // [PERF-1116]
    if (!is_swap) {
      Fable2PerfCountSubmission(false);
    }

    // Queue operations done directly (like UpdateTileMappings) will be awaited
    // alongside the last submission if needed.
    queue_operations_done_since_submission_signal_ = false;
  }

  if (is_closing_frame) {
    if (REXCVAR_GET(clear_memory_page_state) && shared_memory_) {
      shared_memory_->SetSystemPageBlocksValidWithGpuDataWritten();
    }
    frame_open_ = false;
    // Submission already closed now, so minus 1.
    closed_frame_submissions_[(frame_current_++) % kQueueFrames] = submission_current_ - 1;

    if (cache_clear_requested_ && AwaitAllQueueOperationsCompletion()) {
      cache_clear_requested_ = false;

      ClearCommandAllocatorCache();

      ui::d3d12::util::ReleaseAndNull(scratch_buffer_);
      scratch_buffer_size_ = 0;

      if (bindless_resources_used_) {
        texture_cache_bindless_sampler_map_.clear();
        for (const auto& sampler_bindless_heap_overflowed : sampler_bindless_heaps_overflowed_) {
          sampler_bindless_heap_overflowed.first->Release();
        }
        sampler_bindless_heaps_overflowed_.clear();
        sampler_bindless_heap_allocated_ = 0;
      } else {
        sampler_bindful_heap_pool_->ClearCache();
        view_bindful_heap_pool_->ClearCache();
      }
      constant_buffer_pool_->ClearCache();

      texture_cache_->ClearCache();

      // Not clearing the root signatures as they're referenced by pipelines,
      // which are not destroyed.

      primitive_processor_->ClearCache();

      render_target_cache_->ClearCache();

      shared_memory_->ClearCache();
    }
  }

  return true;
}

bool D3D12CommandProcessor::CanEndSubmissionImmediately() const {
  return !submission_open_ || !pipeline_cache_->IsCreatingPipelines();
}

void D3D12CommandProcessor::ClearCommandAllocatorCache() {
  while (command_allocator_submitted_first_) {
    auto next = command_allocator_submitted_first_->next;
    command_allocator_submitted_first_->command_allocator->Release();
    delete command_allocator_submitted_first_;
    command_allocator_submitted_first_ = next;
  }
  command_allocator_submitted_last_ = nullptr;
  while (command_allocator_writable_first_) {
    auto next = command_allocator_writable_first_->next;
    command_allocator_writable_first_->command_allocator->Release();
    delete command_allocator_writable_first_;
    command_allocator_writable_first_ = next;
  }
  command_allocator_writable_last_ = nullptr;
}

void D3D12CommandProcessor::UpdateFixedFunctionState(
    const draw_util::ViewportInfo& viewport_info, const draw_util::Scissor& scissor,
    bool primitive_polygonal, reg::RB_DEPTHCONTROL normalized_depth_control) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Viewport.
  D3D12_VIEWPORT viewport;
  viewport.TopLeftX = float(viewport_info.xy_offset[0]);
  viewport.TopLeftY = float(viewport_info.xy_offset[1]);
  viewport.Width = float(viewport_info.xy_extent[0]);
  viewport.Height = float(viewport_info.xy_extent[1]);
  viewport.MinDepth = viewport_info.z_min;
  viewport.MaxDepth = viewport_info.z_max;
  SetViewport(viewport);

  // Scissor.
  D3D12_RECT scissor_rect;
  scissor_rect.left = LONG(scissor.offset[0]);
  scissor_rect.top = LONG(scissor.offset[1]);
  scissor_rect.right = LONG(scissor.offset[0] + scissor.extent[0]);
  scissor_rect.bottom = LONG(scissor.offset[1] + scissor.extent[1]);
  SetScissorRect(scissor_rect);

  if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets) {
    const RegisterFile& regs = *register_file_;

    // Blend factor.
    float blend_factor[] = {
        regs.Get<float>(XE_GPU_REG_RB_BLEND_RED),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE),
        regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA),
    };
    // std::memcmp instead of != so in case of NaN, every draw won't be
    // invalidating it.
    ff_blend_factor_update_needed_ |=
        std::memcmp(ff_blend_factor_, blend_factor, sizeof(float) * 4) != 0;
    if (ff_blend_factor_update_needed_) {
      std::memcpy(ff_blend_factor_, blend_factor, sizeof(float) * 4);
      deferred_command_list_.D3DOMSetBlendFactor(ff_blend_factor_);
      ff_blend_factor_update_needed_ = false;
    }

    // Stencil reference value. Per-face reference not supported by Direct3D 12,
    // choose the back face one only if drawing only back faces.
    Register stencil_ref_mask_reg;
    auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
    if (primitive_polygonal && normalized_depth_control.backface_enable &&
        pa_su_sc_mode_cntl.cull_front && !pa_su_sc_mode_cntl.cull_back) {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK_BF;
    } else {
      stencil_ref_mask_reg = XE_GPU_REG_RB_STENCILREFMASK;
    }
    uint32_t stencil_ref = regs.Get<reg::RB_STENCILREFMASK>(stencil_ref_mask_reg).stencilref;
    ff_stencil_ref_update_needed_ |= ff_stencil_ref_ != stencil_ref;
    if (ff_stencil_ref_update_needed_) {
      ff_stencil_ref_ = stencil_ref;
      deferred_command_list_.D3DOMSetStencilRef(ff_stencil_ref_);
      ff_stencil_ref_update_needed_ = false;
    }
  }
}

void D3D12CommandProcessor::UpdateSystemConstantValues(
    bool shared_memory_is_uav, bool primitive_polygonal, uint32_t line_loop_closing_index,
    xenos::Endian index_endian, const draw_util::ViewportInfo& viewport_info,
    uint32_t used_texture_mask, reg::RB_DEPTHCONTROL normalized_depth_control,
    uint32_t normalized_color_mask) {
#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  const RegisterFile& regs = *register_file_;
  auto pa_cl_clip_cntl = regs.Get<reg::PA_CL_CLIP_CNTL>();
  auto pa_cl_vte_cntl = regs.Get<reg::PA_CL_VTE_CNTL>();
  auto pa_su_sc_mode_cntl = regs.Get<reg::PA_SU_SC_MODE_CNTL>();
  auto rb_alpha_ref = regs.Get<float>(XE_GPU_REG_RB_ALPHA_REF);
  auto rb_colorcontrol = regs.Get<reg::RB_COLORCONTROL>();
  auto rb_depth_info = regs.Get<reg::RB_DEPTH_INFO>();
  auto rb_stencilrefmask = regs.Get<reg::RB_STENCILREFMASK>();
  auto rb_stencilrefmask_bf = regs.Get<reg::RB_STENCILREFMASK>(XE_GPU_REG_RB_STENCILREFMASK_BF);
  auto rb_surface_info = regs.Get<reg::RB_SURFACE_INFO>();
  auto sq_context_misc = regs.Get<reg::SQ_CONTEXT_MISC>();
  auto sq_program_cntl = regs.Get<reg::SQ_PROGRAM_CNTL>();
  auto vgt_draw_initiator = regs.Get<reg::VGT_DRAW_INITIATOR>();
  uint32_t vgt_indx_offset = regs.Get<reg::VGT_INDX_OFFSET>().indx_offset;
  uint32_t vgt_max_vtx_indx = regs.Get<reg::VGT_MAX_VTX_INDX>().max_indx;
  uint32_t vgt_min_vtx_indx = regs.Get<reg::VGT_MIN_VTX_INDX>().min_indx;

  bool edram_rov_used =
      render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;
  uint32_t draw_resolution_scale_x = texture_cache_->draw_resolution_scale_x();
  uint32_t draw_resolution_scale_y = texture_cache_->draw_resolution_scale_y();

  // Get the color info register values for each render target. Also, for ROV,
  // exclude components that don't exist in the format from the write mask.
  // Don't exclude fully overlapping render targets, however - two render
  // targets with the same base address are used in the lighting pass of
  // 4D5307E6, for example, with the needed one picked with dynamic control
  // flow.
  reg::RB_COLOR_INFO color_infos[4];
  float rt_clamp[4][4];
  // Two UINT32_MAX if no components actually existing in the RT are written.
  uint32_t rt_keep_masks[4][2];
  for (uint32_t i = 0; i < 4; ++i) {
    auto color_info = regs.Get<reg::RB_COLOR_INFO>(reg::RB_COLOR_INFO::rt_register_indices[i]);
    color_infos[i] = color_info;
    if (edram_rov_used) {
      RenderTargetCache::GetPSIColorFormatInfo(
          color_info.color_format, (normalized_color_mask >> (i * 4)) & 0b1111, rt_clamp[i][0],
          rt_clamp[i][1], rt_clamp[i][2], rt_clamp[i][3], rt_keep_masks[i][0], rt_keep_masks[i][1]);
    }
  }

  // Disable depth and stencil if it aliases a color render target (for
  // instance, during the XBLA logo in 58410954, though depth writing is already
  // disabled there).
  bool depth_stencil_enabled =
      normalized_depth_control.stencil_enable || normalized_depth_control.z_enable;
  if (edram_rov_used && depth_stencil_enabled) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (rb_depth_info.depth_base == color_infos[i].color_base &&
          (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX)) {
        depth_stencil_enabled = false;
        break;
      }
    }
  }

  bool dirty = false;

  // Flags.
  uint32_t flags = 0;
  // Whether shared memory is an SRV or a UAV. Because a resource can't be in a
  // read-write (UAV) and a read-only (SRV, IBV) state at once, if any shader in
  // the pipeline uses memexport, the shared memory buffer must be a UAV.
  if (shared_memory_is_uav) {
    flags |= DxbcShaderTranslator::kSysFlag_SharedMemoryIsUAV;
  }
  // W0 division control.
  // http://www.x.org/docs/AMD/old/evergreen_3D_registers_v2.pdf
  // 8: VTX_XY_FMT = true: the incoming XY have already been multiplied by 1/W0.
  //               = false: multiply the X, Y coordinates by 1/W0.
  // 9: VTX_Z_FMT = true: the incoming Z has already been multiplied by 1/W0.
  //              = false: multiply the Z coordinate by 1/W0.
  // 10: VTX_W0_FMT = true: the incoming W0 is not 1/W0. Perform the reciprocal
  //                        to get 1/W0.
  if (pa_cl_vte_cntl.vtx_xy_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_XYDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_z_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_ZDividedByW;
  }
  if (pa_cl_vte_cntl.vtx_w0_fmt) {
    flags |= DxbcShaderTranslator::kSysFlag_WNotReciprocal;
  }
  // Whether the primitive is polygonal and SV_IsFrontFace matters.
  if (primitive_polygonal) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitivePolygonal;
  }
  // Primitive type.
  if (draw_util::IsPrimitiveLine(regs)) {
    flags |= DxbcShaderTranslator::kSysFlag_PrimitiveLine;
  }
  // Depth format.
  if (rb_depth_info.depth_format == xenos::DepthRenderTargetFormat::kD24FS8) {
    flags |= DxbcShaderTranslator::kSysFlag_DepthFloat24;
  }
  // Alpha test.
  xenos::CompareFunction alpha_test_function = rb_colorcontrol.alpha_test_enable
                                                   ? rb_colorcontrol.alpha_func
                                                   : xenos::CompareFunction::kAlways;
  flags |= uint32_t(alpha_test_function) << DxbcShaderTranslator::kSysFlag_AlphaPassIfLess_Shift;
  // Gamma writing.
  if (!render_target_cache_->gamma_render_target_as_unorm16()) {
    for (uint32_t i = 0; i < 4; ++i) {
      if (color_infos[i].color_format == xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA) {
        flags |= DxbcShaderTranslator::kSysFlag_ConvertColor0ToGamma << i;
      }
    }
  }
  if (edram_rov_used && depth_stencil_enabled) {
    flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencil;
    if (normalized_depth_control.z_enable) {
      flags |= uint32_t(normalized_depth_control.zfunc)
               << DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess_Shift;
      if (normalized_depth_control.z_write_enable) {
        flags |= DxbcShaderTranslator::kSysFlag_ROVDepthWrite;
      }
    } else {
      // In case stencil is used without depth testing - always pass, and
      // don't modify the stored depth.
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthPassIfLess |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfEqual |
               DxbcShaderTranslator::kSysFlag_ROVDepthPassIfGreater;
    }
    if (normalized_depth_control.stencil_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVStencilTest;
    }
    // Hint - if not applicable to the shader, will not have effect.
    if (alpha_test_function == xenos::CompareFunction::kAlways &&
        !rb_colorcontrol.alpha_to_mask_enable) {
      flags |= DxbcShaderTranslator::kSysFlag_ROVDepthStencilEarlyWrite;
    }
  }
  dirty |= system_constants_.flags != flags;
  system_constants_.flags = flags;

  // Tessellation factor range, plus 1.0 according to the images in
  // https://www.slideshare.net/blackdevilvikas/next-generation-graphics-programming-on-xbox-360
  float tessellation_factor_min = regs.Get<float>(XE_GPU_REG_VGT_HOS_MIN_TESS_LEVEL) + 1.0f;
  float tessellation_factor_max = regs.Get<float>(XE_GPU_REG_VGT_HOS_MAX_TESS_LEVEL) + 1.0f;
  dirty |= system_constants_.tessellation_factor_range_min != tessellation_factor_min;
  system_constants_.tessellation_factor_range_min = tessellation_factor_min;
  dirty |= system_constants_.tessellation_factor_range_max != tessellation_factor_max;
  system_constants_.tessellation_factor_range_max = tessellation_factor_max;

  // Line loop closing index (or 0 when drawing other primitives or using an
  // index buffer).
  dirty |= system_constants_.line_loop_closing_index != line_loop_closing_index;
  system_constants_.line_loop_closing_index = line_loop_closing_index;

  // Index or tessellation edge factor buffer endianness.
  dirty |= system_constants_.vertex_index_endian != index_endian;
  system_constants_.vertex_index_endian = index_endian;

  // Vertex index offset.
  dirty |= system_constants_.vertex_index_offset != vgt_indx_offset;
  system_constants_.vertex_index_offset = vgt_indx_offset;

  // Vertex index range.
  dirty |= system_constants_.vertex_index_min != vgt_min_vtx_indx;
  dirty |= system_constants_.vertex_index_max != vgt_max_vtx_indx;
  system_constants_.vertex_index_min = vgt_min_vtx_indx;
  system_constants_.vertex_index_max = vgt_max_vtx_indx;

  // User clip planes (UCP_ENA_#), when not CLIP_DISABLE.
  // The shader knows only the total count - tightly packing the user clip
  // planes that are actually used.
  if (!pa_cl_clip_cntl.clip_disable) {
    float* user_clip_plane_write_ptr = system_constants_.user_clip_planes[0];
    uint32_t user_clip_planes_remaining = pa_cl_clip_cntl.ucp_ena;
    uint32_t user_clip_plane_index;
    while (rex::bit_scan_forward(user_clip_planes_remaining, &user_clip_plane_index)) {
      user_clip_planes_remaining &= ~(UINT32_C(1) << user_clip_plane_index);
      const void* user_clip_plane_regs =
          &regs[XE_GPU_REG_PA_CL_UCP_0_X + user_clip_plane_index * 4];
      if (std::memcmp(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float))) {
        dirty = true;
        std::memcpy(user_clip_plane_write_ptr, user_clip_plane_regs, 4 * sizeof(float));
      }
      user_clip_plane_write_ptr += 4;
    }
  }

  // Conversion to Direct3D 12 normalized device coordinates.
  for (uint32_t i = 0; i < 3; ++i) {
    dirty |= system_constants_.ndc_scale[i] != viewport_info.ndc_scale[i];
    dirty |= system_constants_.ndc_offset[i] != viewport_info.ndc_offset[i];
    system_constants_.ndc_scale[i] = viewport_info.ndc_scale[i];
    system_constants_.ndc_offset[i] = viewport_info.ndc_offset[i];
  }

  // Point size.
  if (vgt_draw_initiator.prim_type == xenos::PrimitiveType::kPointList) {
    auto pa_su_point_minmax = regs.Get<reg::PA_SU_POINT_MINMAX>();
    auto pa_su_point_size = regs.Get<reg::PA_SU_POINT_SIZE>();
    float point_vertex_diameter_min = float(pa_su_point_minmax.min_size) * (2.0f / 16.0f);
    float point_vertex_diameter_max = float(pa_su_point_minmax.max_size) * (2.0f / 16.0f);
    float point_constant_diameter_x = float(pa_su_point_size.width) * (2.0f / 16.0f);
    float point_constant_diameter_y = float(pa_su_point_size.height) * (2.0f / 16.0f);
    dirty |= system_constants_.point_vertex_diameter_min != point_vertex_diameter_min;
    dirty |= system_constants_.point_vertex_diameter_max != point_vertex_diameter_max;
    dirty |= system_constants_.point_constant_diameter[0] != point_constant_diameter_x;
    dirty |= system_constants_.point_constant_diameter[1] != point_constant_diameter_y;
    system_constants_.point_vertex_diameter_min = point_vertex_diameter_min;
    system_constants_.point_vertex_diameter_max = point_vertex_diameter_max;
    system_constants_.point_constant_diameter[0] = point_constant_diameter_x;
    system_constants_.point_constant_diameter[1] = point_constant_diameter_y;
    // 2 because 1 in the NDC is half of the viewport's axis, 0.5 for diameter
    // to radius conversion to avoid multiplying the per-vertex diameter by an
    // additional constant in the shader.
    float point_screen_diameter_to_ndc_radius_x =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_x)) /
        std::max(viewport_info.xy_extent[0], uint32_t(1));
    float point_screen_diameter_to_ndc_radius_y =
        (/* 0.5f * 2.0f * */ float(draw_resolution_scale_y)) /
        std::max(viewport_info.xy_extent[1], uint32_t(1));
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[0] !=
             point_screen_diameter_to_ndc_radius_x;
    dirty |= system_constants_.point_screen_diameter_to_ndc_radius[1] !=
             point_screen_diameter_to_ndc_radius_y;
    system_constants_.point_screen_diameter_to_ndc_radius[0] =
        point_screen_diameter_to_ndc_radius_x;
    system_constants_.point_screen_diameter_to_ndc_radius[1] =
        point_screen_diameter_to_ndc_radius_y;
  }

  // Texture signedness / gamma.
  uint32_t textures_resolution_scaled = 0;
  uint32_t textures_remaining = used_texture_mask;
  uint32_t texture_index;
  while (rex::bit_scan_forward(textures_remaining, &texture_index)) {
    textures_remaining &= ~(uint32_t(1) << texture_index);
    uint32_t& texture_signs_uint = system_constants_.texture_swizzled_signs[texture_index >> 2];
    uint32_t texture_signs_shift = (texture_index & 3) * 8;
    uint8_t texture_signs = texture_cache_->GetActiveTextureSwizzledSigns(texture_index);
    uint32_t texture_signs_shifted = uint32_t(texture_signs) << texture_signs_shift;
    uint32_t texture_signs_mask = uint32_t(0b11111111) << texture_signs_shift;
    dirty |= (texture_signs_uint & texture_signs_mask) != texture_signs_shifted;
    texture_signs_uint = (texture_signs_uint & ~texture_signs_mask) | texture_signs_shifted;
    textures_resolution_scaled |=
        uint32_t(texture_cache_->IsActiveTextureResolutionScaled(texture_index)) << texture_index;
  }
  dirty |= system_constants_.textures_resolution_scaled != textures_resolution_scaled;
  system_constants_.textures_resolution_scaled = textures_resolution_scaled;

  // Log2 of sample count, for alpha to mask and with ROV, for EDRAM address
  // calculation with MSAA.
  uint32_t sample_count_log2_x = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 1 : 0;
  uint32_t sample_count_log2_y = rb_surface_info.msaa_samples >= xenos::MsaaSamples::k2X ? 1 : 0;
  dirty |= system_constants_.sample_count_log2[0] != sample_count_log2_x;
  dirty |= system_constants_.sample_count_log2[1] != sample_count_log2_y;
  system_constants_.sample_count_log2[0] = sample_count_log2_x;
  system_constants_.sample_count_log2[1] = sample_count_log2_y;

  // Alpha test and alpha to coverage.
  dirty |= system_constants_.alpha_test_reference != rb_alpha_ref;
  system_constants_.alpha_test_reference = rb_alpha_ref;
  uint32_t alpha_to_mask =
      rb_colorcontrol.alpha_to_mask_enable ? (rb_colorcontrol.value >> 24) | (1 << 8) : 0;
  dirty |= system_constants_.alpha_to_mask != alpha_to_mask;
  system_constants_.alpha_to_mask = alpha_to_mask;

  uint32_t edram_tile_dwords_scaled = xenos::kEdramTileWidthSamples *
                                      xenos::kEdramTileHeightSamples *
                                      (draw_resolution_scale_x * draw_resolution_scale_y);

  // EDRAM pitch for ROV writing.
  if (edram_rov_used) {
    // Align, then multiply by 32bpp tile size in dwords.
    uint32_t edram_32bpp_tile_pitch_dwords_scaled =
        ((rb_surface_info.surface_pitch *
          (rb_surface_info.msaa_samples >= xenos::MsaaSamples::k4X ? 2 : 1)) +
         (xenos::kEdramTileWidthSamples - 1)) /
        xenos::kEdramTileWidthSamples * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_32bpp_tile_pitch_dwords_scaled !=
             edram_32bpp_tile_pitch_dwords_scaled;
    system_constants_.edram_32bpp_tile_pitch_dwords_scaled = edram_32bpp_tile_pitch_dwords_scaled;
  }

  // Color exponent bias and ROV render target writing.
  for (uint32_t i = 0; i < 4; ++i) {
    reg::RB_COLOR_INFO color_info = color_infos[i];
    // Exponent bias is in bits 20:25 of RB_COLOR_INFO.
    int32_t color_exp_bias = color_info.color_exp_bias;
    if (color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16 ||
        color_info.color_format == xenos::ColorRenderTargetFormat::k_16_16_16_16) {
      if (render_target_cache_->GetPath() == RenderTargetCache::Path::kHostRenderTargets &&
          !render_target_cache_->IsFixed16TruncatedToMinus1To1()) {
        // Remap from -32...32 to -1...1 by dividing the output values by 32,
        // losing blending correctness, but getting the full range.
        color_exp_bias -= 5;
      }
    }
    auto color_exp_bias_scale =
        rex::memory::Reinterpret<float>(int32_t(0x3F800000 + (color_exp_bias << 23)));
    dirty |= system_constants_.color_exp_bias[i] != color_exp_bias_scale;
    system_constants_.color_exp_bias[i] = color_exp_bias_scale;
    if (!edram_rov_used && REXCVAR_GET(fable2_rtv_clamp_color_output)) {
      // [BGFIX2-1116]: clamp range for the RTV-path pixel shader output
      // (rgb low, alpha low, rgb high, alpha high), applied after the exponent
      // bias like on the EDRAM. Formats not listed are left at +-FLT_MAX (only
      // NaN and inf change).
      float rtv_clamp[4] = {-3.402823466e+38f, -3.402823466e+38f, 3.402823466e+38f,
                            3.402823466e+38f};
      switch (color_info.color_format) {
        case xenos::ColorRenderTargetFormat::k_8_8_8_8:
        case xenos::ColorRenderTargetFormat::k_8_8_8_8_GAMMA:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_AS_10_10_10_10:
          rtv_clamp[0] = rtv_clamp[1] = 0.0f;
          rtv_clamp[2] = rtv_clamp[3] = 1.0f;
          break;
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT:
        case xenos::ColorRenderTargetFormat::k_2_10_10_10_FLOAT_AS_16_16_16_16:
          rtv_clamp[0] = rtv_clamp[1] = 0.0f;
          rtv_clamp[2] = 31.875f;
          rtv_clamp[3] = 1.0f;
          break;
        case xenos::ColorRenderTargetFormat::k_16_16_FLOAT:
        case xenos::ColorRenderTargetFormat::k_16_16_16_16_FLOAT:
          rtv_clamp[0] = rtv_clamp[1] = -65504.0f;
          rtv_clamp[2] = rtv_clamp[3] = 65504.0f;
          break;
        default:
          break;
      }
      dirty |= std::memcmp(system_constants_.edram_rt_clamp[i], rtv_clamp, sizeof(rtv_clamp)) != 0;
      std::memcpy(system_constants_.edram_rt_clamp[i], rtv_clamp, sizeof(rtv_clamp));
    }
    if (edram_rov_used) {
      dirty |= system_constants_.edram_rt_keep_mask[i][0] != rt_keep_masks[i][0];
      system_constants_.edram_rt_keep_mask[i][0] = rt_keep_masks[i][0];
      dirty |= system_constants_.edram_rt_keep_mask[i][1] != rt_keep_masks[i][1];
      system_constants_.edram_rt_keep_mask[i][1] = rt_keep_masks[i][1];
      if (rt_keep_masks[i][0] != UINT32_MAX || rt_keep_masks[i][1] != UINT32_MAX) {
        uint32_t rt_base_dwords_scaled = color_info.color_base * edram_tile_dwords_scaled;
        dirty |= system_constants_.edram_rt_base_dwords_scaled[i] != rt_base_dwords_scaled;
        system_constants_.edram_rt_base_dwords_scaled[i] = rt_base_dwords_scaled;
        uint32_t format_flags = RenderTargetCache::AddPSIColorFormatFlags(color_info.color_format);
        dirty |= system_constants_.edram_rt_format_flags[i] != format_flags;
        system_constants_.edram_rt_format_flags[i] = format_flags;
        // Can't do float comparisons here because NaNs would result in always
        // setting the dirty flag.
        dirty |=
            std::memcmp(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float)) != 0;
        std::memcpy(system_constants_.edram_rt_clamp[i], rt_clamp[i], 4 * sizeof(float));
        uint32_t blend_factors_ops =
            regs[reg::RB_BLENDCONTROL::rt_register_indices[i]] & 0x1FFF1FFF;
        dirty |= system_constants_.edram_rt_blend_factors_ops[i] != blend_factors_ops;
        system_constants_.edram_rt_blend_factors_ops[i] = blend_factors_ops;
      }
    }
  }

  if (edram_rov_used) {
    uint32_t depth_base_dwords_scaled = rb_depth_info.depth_base * edram_tile_dwords_scaled;
    dirty |= system_constants_.edram_depth_base_dwords_scaled != depth_base_dwords_scaled;
    system_constants_.edram_depth_base_dwords_scaled = depth_base_dwords_scaled;

    // For non-polygons, front polygon offset is used, and it's enabled if
    // POLY_OFFSET_PARA_ENABLED is set, for polygons, separate front and back
    // are used.
    float poly_offset_front_scale = 0.0f, poly_offset_front_offset = 0.0f;
    float poly_offset_back_scale = 0.0f, poly_offset_back_offset = 0.0f;
    if (primitive_polygonal) {
      if (pa_su_sc_mode_cntl.poly_offset_front_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
      }
      if (pa_su_sc_mode_cntl.poly_offset_back_enable) {
        poly_offset_back_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_SCALE);
        poly_offset_back_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_BACK_OFFSET);
      }
    } else {
      if (pa_su_sc_mode_cntl.poly_offset_para_enable) {
        poly_offset_front_scale = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_SCALE);
        poly_offset_front_offset = regs.Get<float>(XE_GPU_REG_PA_SU_POLY_OFFSET_FRONT_OFFSET);
        poly_offset_back_scale = poly_offset_front_scale;
        poly_offset_back_offset = poly_offset_front_offset;
      }
    }
    // With non-square resolution scaling, make sure the worst-case impact is
    // reverted (slope only along the scaled axis), thus max. More bias is
    // better than less bias, because less bias means Z fighting with the
    // background is more likely.
    float poly_offset_scale_factor = xenos::kPolygonOffsetScaleSubpixelUnit *
                                     std::max(draw_resolution_scale_x, draw_resolution_scale_y);
    poly_offset_front_scale *= poly_offset_scale_factor;
    poly_offset_back_scale *= poly_offset_scale_factor;
    dirty |= system_constants_.edram_poly_offset_front_scale != poly_offset_front_scale;
    system_constants_.edram_poly_offset_front_scale = poly_offset_front_scale;
    dirty |= system_constants_.edram_poly_offset_front_offset != poly_offset_front_offset;
    system_constants_.edram_poly_offset_front_offset = poly_offset_front_offset;
    dirty |= system_constants_.edram_poly_offset_back_scale != poly_offset_back_scale;
    system_constants_.edram_poly_offset_back_scale = poly_offset_back_scale;
    dirty |= system_constants_.edram_poly_offset_back_offset != poly_offset_back_offset;
    system_constants_.edram_poly_offset_back_offset = poly_offset_back_offset;

    if (depth_stencil_enabled && normalized_depth_control.stencil_enable) {
      dirty |= system_constants_.edram_stencil_front_reference != rb_stencilrefmask.stencilref;
      system_constants_.edram_stencil_front_reference = rb_stencilrefmask.stencilref;
      dirty |= system_constants_.edram_stencil_front_read_mask != rb_stencilrefmask.stencilmask;
      system_constants_.edram_stencil_front_read_mask = rb_stencilrefmask.stencilmask;
      dirty |=
          system_constants_.edram_stencil_front_write_mask != rb_stencilrefmask.stencilwritemask;
      system_constants_.edram_stencil_front_write_mask = rb_stencilrefmask.stencilwritemask;
      uint32_t stencil_func_ops = (normalized_depth_control.value >> 8) & ((1 << 12) - 1);
      dirty |= system_constants_.edram_stencil_front_func_ops != stencil_func_ops;
      system_constants_.edram_stencil_front_func_ops = stencil_func_ops;

      if (primitive_polygonal && normalized_depth_control.backface_enable) {
        dirty |= system_constants_.edram_stencil_back_reference != rb_stencilrefmask_bf.stencilref;
        system_constants_.edram_stencil_back_reference = rb_stencilrefmask_bf.stencilref;
        dirty |= system_constants_.edram_stencil_back_read_mask != rb_stencilrefmask_bf.stencilmask;
        system_constants_.edram_stencil_back_read_mask = rb_stencilrefmask_bf.stencilmask;
        dirty |= system_constants_.edram_stencil_back_write_mask !=
                 rb_stencilrefmask_bf.stencilwritemask;
        system_constants_.edram_stencil_back_write_mask = rb_stencilrefmask_bf.stencilwritemask;
        uint32_t stencil_func_ops_bf = (normalized_depth_control.value >> 20) & ((1 << 12) - 1);
        dirty |= system_constants_.edram_stencil_back_func_ops != stencil_func_ops_bf;
        system_constants_.edram_stencil_back_func_ops = stencil_func_ops_bf;
      } else {
        dirty |= std::memcmp(system_constants_.edram_stencil_back,
                             system_constants_.edram_stencil_front, 4 * sizeof(uint32_t)) != 0;
        std::memcpy(system_constants_.edram_stencil_back, system_constants_.edram_stencil_front,
                    4 * sizeof(uint32_t));
      }
    }

    dirty |= system_constants_.edram_blend_constant[0] != regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    system_constants_.edram_blend_constant[0] = regs.Get<float>(XE_GPU_REG_RB_BLEND_RED);
    dirty |=
        system_constants_.edram_blend_constant[1] != regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    system_constants_.edram_blend_constant[1] = regs.Get<float>(XE_GPU_REG_RB_BLEND_GREEN);
    dirty |= system_constants_.edram_blend_constant[2] != regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    system_constants_.edram_blend_constant[2] = regs.Get<float>(XE_GPU_REG_RB_BLEND_BLUE);
    dirty |=
        system_constants_.edram_blend_constant[3] != regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
    system_constants_.edram_blend_constant[3] = regs.Get<float>(XE_GPU_REG_RB_BLEND_ALPHA);
  }

  cbuffer_binding_system_.up_to_date &= !dirty;
}

bool D3D12CommandProcessor::UpdateBindings(const D3D12Shader* vertex_shader,
                                           const D3D12Shader* pixel_shader,
                                           ID3D12RootSignature* root_signature,
                                           bool shared_memory_is_uav) {
  const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
  ID3D12Device* device = provider.GetDevice();
  const RegisterFile& regs = *register_file_;

#if XE_GPU_FINE_GRAINED_DRAW_SCOPES
  SCOPE_profile_cpu_f("gpu");
#endif  // XE_GPU_FINE_GRAINED_DRAW_SCOPES

  // Set the new root signature.
  if (current_graphics_root_signature_ != root_signature) {
    current_graphics_root_signature_ = root_signature;
    if (!bindless_resources_used_) {
      GetRootBindfulExtraParameterIndices(vertex_shader, pixel_shader,
                                          current_graphics_root_bindful_extras_);
    }
    // Changing the root signature invalidates all bindings.
    current_graphics_root_up_to_date_ = 0;
    deferred_command_list_.D3DSetGraphicsRootSignature(root_signature);
  }

  // Select the root parameter indices depending on the used binding model.
  uint32_t root_parameter_fetch_constants = bindless_resources_used_
                                                ? kRootParameter_Bindless_FetchConstants
                                                : kRootParameter_Bindful_FetchConstants;
  uint32_t root_parameter_float_constants_vertex =
      bindless_resources_used_ ? kRootParameter_Bindless_FloatConstantsVertex
                               : kRootParameter_Bindful_FloatConstantsVertex;
  uint32_t root_parameter_float_constants_pixel = bindless_resources_used_
                                                      ? kRootParameter_Bindless_FloatConstantsPixel
                                                      : kRootParameter_Bindful_FloatConstantsPixel;
  uint32_t root_parameter_system_constants = bindless_resources_used_
                                                 ? kRootParameter_Bindless_SystemConstants
                                                 : kRootParameter_Bindful_SystemConstants;
  uint32_t root_parameter_bool_loop_constants = bindless_resources_used_
                                                    ? kRootParameter_Bindless_BoolLoopConstants
                                                    : kRootParameter_Bindful_BoolLoopConstants;
  uint32_t root_parameter_shared_memory_and_bindful_edram =
      bindless_resources_used_ ? kRootParameter_Bindless_SharedMemory
                               : kRootParameter_Bindful_SharedMemoryAndEdram;

  //
  // Update root constant buffers that are common for bindful and bindless.
  //

  // These are the constant base addresses/ranges for shaders.
  // We have these hardcoded right now cause nothing seems to differ on the Xbox
  // 360 (however, OpenGL ES on Adreno 200 on Android has different ranges).
  assert_true(regs[XE_GPU_REG_SQ_VS_CONST] == 0x000FF000 ||
              regs[XE_GPU_REG_SQ_VS_CONST] == 0x00000000);
  assert_true(regs[XE_GPU_REG_SQ_PS_CONST] == 0x000FF100 ||
              regs[XE_GPU_REG_SQ_PS_CONST] == 0x00000000);
  // Check if the float constant layout is still the same and get the counts.
  const Shader::ConstantRegisterMap& float_constant_map_vertex =
      vertex_shader->constant_register_map();
  uint32_t float_constant_count_vertex = float_constant_map_vertex.float_count;
  for (uint32_t i = 0; i < 4; ++i) {
    if (current_float_constant_map_vertex_[i] != float_constant_map_vertex.float_bitmap[i]) {
      current_float_constant_map_vertex_[i] = float_constant_map_vertex.float_bitmap[i];
      // If no float constants at all, we can reuse any buffer for them, so not
      // invalidating.
      if (float_constant_count_vertex) {
        cbuffer_binding_float_vertex_.up_to_date = false;
      }
    }
  }
  uint32_t float_constant_count_pixel = 0;
  if (pixel_shader != nullptr) {
    const Shader::ConstantRegisterMap& float_constant_map_pixel =
        pixel_shader->constant_register_map();
    float_constant_count_pixel = float_constant_map_pixel.float_count;
    for (uint32_t i = 0; i < 4; ++i) {
      if (current_float_constant_map_pixel_[i] != float_constant_map_pixel.float_bitmap[i]) {
        current_float_constant_map_pixel_[i] = float_constant_map_pixel.float_bitmap[i];
        if (float_constant_count_pixel) {
          cbuffer_binding_float_pixel_.up_to_date = false;
        }
      }
    }
  } else {
    std::memset(current_float_constant_map_pixel_, 0, sizeof(current_float_constant_map_pixel_));
  }

  // Write the constant buffer data.
  if (!cbuffer_binding_system_.up_to_date) {
    uint8_t* system_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(system_constants_), D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_system_.address);
    if (system_constants == nullptr) {
      return false;
    }
    std::memcpy(system_constants, &system_constants_, sizeof(system_constants_));
    cbuffer_binding_system_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_system_constants);
  }
  if (!cbuffer_binding_float_vertex_.up_to_date) {
    // Even if the shader doesn't need any float constants, a valid binding must
    // still be provided, so if the first draw in the frame with the current
    // root signature doesn't have float constants at all, still allocate an
    // empty buffer.
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_vertex, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_vertex_.address);
    if (float_constants == nullptr) {
      return false;
    }
    P1117CopyFloatConstants(float_constants, float_constant_map_vertex.float_bitmap,
                            &regs[XE_GPU_REG_SHADER_CONSTANT_000_X]);  // [PERF-1117]
    cbuffer_binding_float_vertex_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_vertex);
  }
  if (!cbuffer_binding_float_pixel_.up_to_date) {
    uint8_t* float_constants = constant_buffer_pool_->Request(
        frame_current_, sizeof(float) * 4 * std::max(float_constant_count_pixel, uint32_t(1)),
        D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
        &cbuffer_binding_float_pixel_.address);
    if (float_constants == nullptr) {
      return false;
    }
    if (pixel_shader != nullptr) {
      const Shader::ConstantRegisterMap& float_constant_map_pixel =
          pixel_shader->constant_register_map();
      P1117CopyFloatConstants(float_constants, float_constant_map_pixel.float_bitmap,
                              &regs[XE_GPU_REG_SHADER_CONSTANT_256_X]);  // [PERF-1117]
    }
    cbuffer_binding_float_pixel_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_float_constants_pixel);
  }
  if (!cbuffer_binding_bool_loop_.up_to_date) {
    constexpr uint32_t kBoolLoopConstantsSize = (8 + 32) * sizeof(uint32_t);
    uint8_t* bool_loop_constants = constant_buffer_pool_->Request(
        frame_current_, kBoolLoopConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_bool_loop_.address);
    if (bool_loop_constants == nullptr) {
      return false;
    }
    std::memcpy(bool_loop_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_BOOL_000_031],
                kBoolLoopConstantsSize);
    cbuffer_binding_bool_loop_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_bool_loop_constants);
  }
  if (!cbuffer_binding_fetch_.up_to_date) {
    constexpr uint32_t kFetchConstantsSize = 32 * 6 * sizeof(uint32_t);
    uint8_t* fetch_constants = constant_buffer_pool_->Request(
        frame_current_, kFetchConstantsSize, D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT,
        nullptr, nullptr, &cbuffer_binding_fetch_.address);
    if (fetch_constants == nullptr) {
      return false;
    }
    std::memcpy(fetch_constants, &regs[XE_GPU_REG_SHADER_CONSTANT_FETCH_00_0], kFetchConstantsSize);
    cbuffer_binding_fetch_.up_to_date = true;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_fetch_constants);
  }

  //
  // Update descriptors.
  //

  if (!current_shared_memory_binding_is_uav_.has_value() ||
      current_shared_memory_binding_is_uav_.value() != shared_memory_is_uav) {
    current_shared_memory_binding_is_uav_ = shared_memory_is_uav;
    current_graphics_root_up_to_date_ &= ~(1u << root_parameter_shared_memory_and_bindful_edram);
  }

  // Get textures and samplers used by the vertex shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_vertex = vertex_shader->GetTextureBindingLayoutUserUID();
  size_t sampler_layout_uid_vertex = vertex_shader->GetSamplerBindingLayoutUserUID();
  const std::vector<D3D12Shader::TextureBinding>& textures_vertex =
      vertex_shader->GetTextureBindingsAfterTranslation();
  const std::vector<D3D12Shader::SamplerBinding>& samplers_vertex =
      vertex_shader->GetSamplerBindingsAfterTranslation();
  size_t texture_count_vertex = textures_vertex.size();
  size_t sampler_count_vertex = samplers_vertex.size();
  if (sampler_count_vertex) {
    if (current_sampler_layout_uid_vertex_ != sampler_layout_uid_vertex) {
      current_sampler_layout_uid_vertex_ = sampler_layout_uid_vertex;
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
      bindful_samplers_written_vertex_ = false;
    }
    // [PERF-1117]: sampler cache.
    P1117SamplerCache& sc = g_p1117_sampler_cache[0];
    const bool sampler_cache_hit = P1117().sampler_cache && sc.valid &&
                                   sc.layout_uid == sampler_layout_uid_vertex &&
                                   sc.fetch_gen == g_p1117_fetch_gen &&
                                   current_samplers_vertex_.size() >= sampler_count_vertex;
    sc.valid = true;
    sc.layout_uid = sampler_layout_uid_vertex;
    sc.fetch_gen = g_p1117_fetch_gen;
    current_samplers_vertex_.resize(
        std::max(current_samplers_vertex_.size(), sampler_count_vertex));
    if (sampler_cache_hit && P1117Sample()) {
      bool bad = false;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        bad |= texture_cache_->GetSamplerParameters(samplers_vertex[i]) !=
               current_samplers_vertex_[i];
      }
      P1117VerifyNote(3, bad);
    }
    for (size_t i = 0; i < (sampler_cache_hit ? size_t(0) : sampler_count_vertex); ++i) {
      D3D12TextureCache::SamplerParameters parameters =
          texture_cache_->GetSamplerParameters(samplers_vertex[i]);
      if (current_samplers_vertex_[i] != parameters) {
        cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
        bindful_samplers_written_vertex_ = false;
        current_samplers_vertex_[i] = parameters;
      }
    }
  }

  // Get textures and samplers used by the pixel shader, check if the last used
  // samplers are compatible and update them.
  size_t texture_layout_uid_pixel, sampler_layout_uid_pixel;
  const std::vector<D3D12Shader::TextureBinding>* textures_pixel;
  const std::vector<D3D12Shader::SamplerBinding>* samplers_pixel;
  size_t texture_count_pixel, sampler_count_pixel;
  if (pixel_shader != nullptr) {
    texture_layout_uid_pixel = pixel_shader->GetTextureBindingLayoutUserUID();
    sampler_layout_uid_pixel = pixel_shader->GetSamplerBindingLayoutUserUID();
    textures_pixel = &pixel_shader->GetTextureBindingsAfterTranslation();
    texture_count_pixel = textures_pixel->size();
    samplers_pixel = &pixel_shader->GetSamplerBindingsAfterTranslation();
    sampler_count_pixel = samplers_pixel->size();
    if (sampler_count_pixel) {
      if (current_sampler_layout_uid_pixel_ != sampler_layout_uid_pixel) {
        current_sampler_layout_uid_pixel_ = sampler_layout_uid_pixel;
        cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
        bindful_samplers_written_pixel_ = false;
      }
      // [PERF-1117]: sampler cache.
      P1117SamplerCache& sc = g_p1117_sampler_cache[1];
      const bool sampler_cache_hit = P1117().sampler_cache && sc.valid &&
                                     sc.layout_uid == sampler_layout_uid_pixel &&
                                     sc.fetch_gen == g_p1117_fetch_gen &&
                                     current_samplers_pixel_.size() >= sampler_count_pixel;
      sc.valid = true;
      sc.layout_uid = sampler_layout_uid_pixel;
      sc.fetch_gen = g_p1117_fetch_gen;
      current_samplers_pixel_.resize(
          std::max(current_samplers_pixel_.size(), size_t(sampler_count_pixel)));
      if (sampler_cache_hit && P1117Sample()) {
        bool bad = false;
        for (size_t i = 0; i < sampler_count_pixel; ++i) {
          bad |= texture_cache_->GetSamplerParameters((*samplers_pixel)[i]) !=
                 current_samplers_pixel_[i];
        }
        P1117VerifyNote(3, bad);
      }
      for (uint32_t i = 0; i < (sampler_cache_hit ? uint32_t(0) : uint32_t(sampler_count_pixel));
           ++i) {
        D3D12TextureCache::SamplerParameters parameters =
            texture_cache_->GetSamplerParameters((*samplers_pixel)[i]);
        if (current_samplers_pixel_[i] != parameters) {
          current_samplers_pixel_[i] = parameters;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          bindful_samplers_written_pixel_ = false;
        }
      }
    }
  } else {
    texture_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    sampler_layout_uid_pixel = PipelineCache::kLayoutUIDEmpty;
    textures_pixel = nullptr;
    texture_count_pixel = 0;
    samplers_pixel = nullptr;
    sampler_count_pixel = 0;
  }

  assert_true(sampler_count_vertex + sampler_count_pixel <= kSamplerHeapSize);

  if (bindless_resources_used_) {
    //
    // Bindless descriptors path.
    //

    // Check if need to write new descriptor indices.
    // Samplers have already been checked.
    if (texture_count_vertex && cbuffer_binding_descriptor_indices_vertex_.up_to_date &&
        (current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_vertex_.data(),
                                                          textures_vertex.data(),
                                                          texture_count_vertex))) {
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
    }
    if (texture_count_pixel && cbuffer_binding_descriptor_indices_pixel_.up_to_date &&
        (current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(current_texture_srv_keys_pixel_.data(),
                                                          textures_pixel->data(),
                                                          texture_count_pixel))) {
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
    }

    // Get sampler descriptor indices, write new samplers, and handle sampler
    // heap overflow if it happens.
    if ((sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) ||
        (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date)) {
      for (uint32_t i = 0; i < 2; ++i) {
        if (i) {
          // Overflow happened - invalidate sampler bindings because their
          // descriptor indices can't be used anymore (and even if heap creation
          // fails, because current_sampler_bindless_indices_#_ are in an
          // undefined state now) and switch to a new sampler heap.
          cbuffer_binding_descriptor_indices_vertex_.up_to_date = false;
          cbuffer_binding_descriptor_indices_pixel_.up_to_date = false;
          ID3D12DescriptorHeap* sampler_heap_new;
          if (!sampler_bindless_heaps_overflowed_.empty() &&
              sampler_bindless_heaps_overflowed_.front().second <= submission_completed_) {
            sampler_heap_new = sampler_bindless_heaps_overflowed_.front().first;
            sampler_bindless_heaps_overflowed_.pop_front();
          } else {
            D3D12_DESCRIPTOR_HEAP_DESC sampler_heap_new_desc;
            sampler_heap_new_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
            sampler_heap_new_desc.NumDescriptors = kSamplerHeapSize;
            sampler_heap_new_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
            sampler_heap_new_desc.NodeMask = 0;
            if (FAILED(device->CreateDescriptorHeap(&sampler_heap_new_desc,
                                                    IID_PPV_ARGS(&sampler_heap_new)))) {
              REXGPU_ERROR(
                  "Failed to create a new bindless sampler descriptor heap "
                  "after an overflow of the previous one");
              return false;
            }
          }
          // Only change the heap if a new heap was created successfully, not to
          // leave the values in an undefined state in case CreateDescriptorHeap
          // has failed.
          sampler_bindless_heaps_overflowed_.push_back(
              std::make_pair(sampler_bindless_heap_current_, submission_current_));
          sampler_bindless_heap_current_ = sampler_heap_new;
          sampler_bindless_heap_cpu_start_ =
              sampler_bindless_heap_current_->GetCPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_gpu_start_ =
              sampler_bindless_heap_current_->GetGPUDescriptorHandleForHeapStart();
          sampler_bindless_heap_allocated_ = 0;
          // The only thing the heap is used for now is texture cache samplers -
          // invalidate all of them.
          texture_cache_bindless_sampler_map_.clear();
          deferred_command_list_.SetDescriptorHeaps(view_bindless_heap_,
                                                    sampler_bindless_heap_current_);
          current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_SamplerHeap);
        }
        bool samplers_overflowed = false;
        if (sampler_count_vertex && !cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
          current_sampler_bindless_indices_vertex_.resize(std::max(
              current_sampler_bindless_indices_vertex_.size(), size_t(sampler_count_vertex)));
          for (uint32_t j = 0; j < sampler_count_vertex; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_vertex_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_vertex_[j] = sampler_index;
          }
        }
        if (samplers_overflowed) {
          continue;
        }
        if (sampler_count_pixel && !cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
          current_sampler_bindless_indices_pixel_.resize(std::max(
              current_sampler_bindless_indices_pixel_.size(), size_t(sampler_count_pixel)));
          for (uint32_t j = 0; j < sampler_count_pixel; ++j) {
            D3D12TextureCache::SamplerParameters sampler_parameters = current_samplers_pixel_[j];
            uint32_t sampler_index;
            auto it = texture_cache_bindless_sampler_map_.find(sampler_parameters.value);
            if (it != texture_cache_bindless_sampler_map_.end()) {
              sampler_index = it->second;
            } else {
              if (sampler_bindless_heap_allocated_ >= kSamplerHeapSize) {
                samplers_overflowed = true;
                break;
              }
              sampler_index = sampler_bindless_heap_allocated_++;
              texture_cache_->WriteSampler(sampler_parameters,
                                           provider.OffsetSamplerDescriptor(
                                               sampler_bindless_heap_cpu_start_, sampler_index));
              texture_cache_bindless_sampler_map_.emplace(sampler_parameters.value, sampler_index);
            }
            current_sampler_bindless_indices_pixel_[j] = sampler_index;
          }
        }
        if (!samplers_overflowed) {
          break;
        }
      }
    }

    if (!cbuffer_binding_descriptor_indices_vertex_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_vertex + sampler_count_vertex, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_vertex_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        const D3D12Shader::TextureBinding& texture = textures_vertex[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      if (texture_count_vertex) {
        current_texture_srv_keys_vertex_.resize(
            std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                  textures_vertex.data(), texture_count_vertex);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        descriptor_indices[samplers_vertex[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_vertex_[i];
      }
      cbuffer_binding_descriptor_indices_vertex_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesVertex);
    }

    if (!cbuffer_binding_descriptor_indices_pixel_.up_to_date) {
      uint32_t* descriptor_indices = reinterpret_cast<uint32_t*>(constant_buffer_pool_->Request(
          frame_current_,
          std::max(texture_count_pixel + sampler_count_pixel, size_t(1)) * sizeof(uint32_t),
          D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT, nullptr, nullptr,
          &cbuffer_binding_descriptor_indices_pixel_.address));
      if (!descriptor_indices) {
        return false;
      }
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        const D3D12Shader::TextureBinding& texture = (*textures_pixel)[i];
        descriptor_indices[texture.bindless_descriptor_index] =
            texture_cache_->GetActiveTextureBindlessSRVIndex(texture) -
            uint32_t(SystemBindlessView::kUnboundedSRVsStart);
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      if (texture_count_pixel) {
        current_texture_srv_keys_pixel_.resize(
            std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
        texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                  textures_pixel->data(), texture_count_pixel);
      }
      // Current samplers have already been updated.
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        descriptor_indices[(*samplers_pixel)[i].bindless_descriptor_index] =
            current_sampler_bindless_indices_pixel_[i];
      }
      cbuffer_binding_descriptor_indices_pixel_.up_to_date = true;
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindless_DescriptorIndicesPixel);
    }
  } else {
    //
    // Bindful descriptors path.
    //

    // See what descriptors need to be updated.
    // Samplers have already been checked.
    bool write_textures_vertex =
        texture_count_vertex && (!bindful_textures_written_vertex_ ||
                                 current_texture_layout_uid_vertex_ != texture_layout_uid_vertex ||
                                 !texture_cache_->AreActiveTextureSRVKeysUpToDate(
                                     current_texture_srv_keys_vertex_.data(),
                                     textures_vertex.data(), texture_count_vertex));
    bool write_textures_pixel =
        texture_count_pixel &&
        (!bindful_textures_written_pixel_ ||
         current_texture_layout_uid_pixel_ != texture_layout_uid_pixel ||
         !texture_cache_->AreActiveTextureSRVKeysUpToDate(
             current_texture_srv_keys_pixel_.data(), textures_pixel->data(), texture_count_pixel));
    bool write_samplers_vertex = sampler_count_vertex && !bindful_samplers_written_vertex_;
    bool write_samplers_pixel = sampler_count_pixel && !bindful_samplers_written_pixel_;
    bool edram_rov_used =
        render_target_cache_->GetPath() == RenderTargetCache::Path::kPixelShaderInterlock;

    // Allocate the descriptors.
    size_t view_count_partial_update = 0;
    if (write_textures_vertex) {
      view_count_partial_update += texture_count_vertex;
    }
    if (write_textures_pixel) {
      view_count_partial_update += texture_count_pixel;
    }
    // Shared memory SRV and null UAV + null SRV and shared memory UAV +
    // textures.
    size_t view_count_full_update = 4 + texture_count_vertex + texture_count_pixel;
    if (edram_rov_used) {
      // + EDRAM UAV in two tables (with the shared memory SRV and with the
      // shared memory UAV).
      view_count_full_update += 2;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE view_cpu_handle;
    D3D12_GPU_DESCRIPTOR_HANDLE view_gpu_handle;
    uint32_t descriptor_size_view = provider.GetViewDescriptorSize();
    uint64_t view_heap_index = RequestViewBindfulDescriptors(
        draw_view_bindful_heap_index_, uint32_t(view_count_partial_update),
        uint32_t(view_count_full_update), view_cpu_handle, view_gpu_handle);
    if (view_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      REXGPU_ERROR("Failed to allocate view descriptors");
      return false;
    }
    size_t sampler_count_partial_update = 0;
    if (write_samplers_vertex) {
      sampler_count_partial_update += sampler_count_vertex;
    }
    if (write_samplers_pixel) {
      sampler_count_partial_update += sampler_count_pixel;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE sampler_cpu_handle = {};
    D3D12_GPU_DESCRIPTOR_HANDLE sampler_gpu_handle = {};
    uint32_t descriptor_size_sampler = provider.GetSamplerDescriptorSize();
    uint64_t sampler_heap_index = ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid;
    if (sampler_count_vertex != 0 || sampler_count_pixel != 0) {
      sampler_heap_index = RequestSamplerBindfulDescriptors(
          draw_sampler_bindful_heap_index_, uint32_t(sampler_count_partial_update),
          uint32_t(sampler_count_vertex + sampler_count_pixel), sampler_cpu_handle,
          sampler_gpu_handle);
      if (sampler_heap_index == ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
        REXGPU_ERROR("Failed to allocate sampler descriptors");
        return false;
      }
    }
    if (draw_view_bindful_heap_index_ != view_heap_index) {
      // Need to update all view descriptors.
      write_textures_vertex = texture_count_vertex != 0;
      write_textures_pixel = texture_count_pixel != 0;
      bindful_textures_written_vertex_ = false;
      bindful_textures_written_pixel_ = false;
      // If updating fully, write the shared memory SRV and UAV descriptors and,
      // if needed, the EDRAM descriptor.
      // SRV + null UAV + EDRAM.
      gpu_handle_shared_memory_srv_and_edram_ = view_gpu_handle;
      shared_memory_->WriteRawSRVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      ui::d3d12::util::CreateBufferRawUAV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      // Null SRV + UAV + EDRAM.
      gpu_handle_shared_memory_uav_and_edram_ = view_gpu_handle;
      ui::d3d12::util::CreateBufferRawSRV(device, view_cpu_handle, nullptr, 0);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      shared_memory_->WriteRawUAVDescriptor(view_cpu_handle);
      view_cpu_handle.ptr += descriptor_size_view;
      view_gpu_handle.ptr += descriptor_size_view;
      if (edram_rov_used) {
        render_target_cache_->WriteEdramUintPow2UAVDescriptor(view_cpu_handle, 2);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_graphics_root_up_to_date_ &= ~(1u << kRootParameter_Bindful_SharedMemoryAndEdram);
    }
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid &&
        draw_sampler_bindful_heap_index_ != sampler_heap_index) {
      write_samplers_vertex = sampler_count_vertex != 0;
      write_samplers_pixel = sampler_count_pixel != 0;
      bindful_samplers_written_vertex_ = false;
      bindful_samplers_written_pixel_ = false;
    }

    // Write the descriptors.
    if (write_textures_vertex) {
      assert_true(current_graphics_root_bindful_extras_.textures_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_vertex_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_vertex; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV(textures_vertex[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_vertex_ = texture_layout_uid_vertex;
      current_texture_srv_keys_vertex_.resize(
          std::max(current_texture_srv_keys_vertex_.size(), size_t(texture_count_vertex)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_vertex_.data(),
                                                textures_vertex.data(), texture_count_vertex);
      bindful_textures_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_vertex);
    }
    if (write_textures_pixel) {
      assert_true(current_graphics_root_bindful_extras_.textures_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_textures_pixel_ = view_gpu_handle;
      for (size_t i = 0; i < texture_count_pixel; ++i) {
        texture_cache_->WriteActiveTextureBindfulSRV((*textures_pixel)[i], view_cpu_handle);
        view_cpu_handle.ptr += descriptor_size_view;
        view_gpu_handle.ptr += descriptor_size_view;
      }
      current_texture_layout_uid_pixel_ = texture_layout_uid_pixel;
      current_texture_srv_keys_pixel_.resize(
          std::max(current_texture_srv_keys_pixel_.size(), size_t(texture_count_pixel)));
      texture_cache_->WriteActiveTextureSRVKeys(current_texture_srv_keys_pixel_.data(),
                                                textures_pixel->data(), texture_count_pixel);
      bindful_textures_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.textures_pixel);
    }
    if (write_samplers_vertex) {
      assert_true(current_graphics_root_bindful_extras_.samplers_vertex !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_vertex_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_vertex; ++i) {
        texture_cache_->WriteSampler(current_samplers_vertex_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_vertex_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_vertex);
    }
    if (write_samplers_pixel) {
      assert_true(current_graphics_root_bindful_extras_.samplers_pixel !=
                  RootBindfulExtraParameterIndices::kUnavailable);
      gpu_handle_samplers_pixel_ = sampler_gpu_handle;
      for (size_t i = 0; i < sampler_count_pixel; ++i) {
        texture_cache_->WriteSampler(current_samplers_pixel_[i], sampler_cpu_handle);
        sampler_cpu_handle.ptr += descriptor_size_sampler;
        sampler_gpu_handle.ptr += descriptor_size_sampler;
      }
      // Current samplers have already been updated.
      bindful_samplers_written_pixel_ = true;
      current_graphics_root_up_to_date_ &=
          ~(1u << current_graphics_root_bindful_extras_.samplers_pixel);
    }

    // Wrote new descriptors on the current page.
    draw_view_bindful_heap_index_ = view_heap_index;
    if (sampler_heap_index != ui::d3d12::D3D12DescriptorHeapPool::kHeapIndexInvalid) {
      draw_sampler_bindful_heap_index_ = sampler_heap_index;
    }
  }

  // Update the root parameters.
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_fetch_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_fetch_constants,
                                                                cbuffer_binding_fetch_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_fetch_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_vertex))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_vertex, cbuffer_binding_float_vertex_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_vertex;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_float_constants_pixel))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
        root_parameter_float_constants_pixel, cbuffer_binding_float_pixel_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_float_constants_pixel;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_system_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_system_constants,
                                                                cbuffer_binding_system_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_system_constants;
  }
  if (!(current_graphics_root_up_to_date_ & (1u << root_parameter_bool_loop_constants))) {
    deferred_command_list_.D3DSetGraphicsRootConstantBufferView(root_parameter_bool_loop_constants,
                                                                cbuffer_binding_bool_loop_.address);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_bool_loop_constants;
  }
  if (!(current_graphics_root_up_to_date_ &
        (1u << root_parameter_shared_memory_and_bindful_edram))) {
    assert_true(current_shared_memory_binding_is_uav_.has_value());
    D3D12_GPU_DESCRIPTOR_HANDLE gpu_handle_shared_memory_and_bindful_edram;
    if (bindless_resources_used_) {
      gpu_handle_shared_memory_and_bindful_edram = provider.OffsetViewDescriptor(
          view_bindless_heap_gpu_start_,
          uint32_t(current_shared_memory_binding_is_uav_.value()
                       ? SystemBindlessView ::kNullRawSRVAndSharedMemoryRawUAVStart
                       : SystemBindlessView ::kSharedMemoryRawSRVAndNullRawUAVStart));
    } else {
      gpu_handle_shared_memory_and_bindful_edram = current_shared_memory_binding_is_uav_.value()
                                                       ? gpu_handle_shared_memory_uav_and_edram_
                                                       : gpu_handle_shared_memory_srv_and_edram_;
    }
    deferred_command_list_.D3DSetGraphicsRootDescriptorTable(
        root_parameter_shared_memory_and_bindful_edram, gpu_handle_shared_memory_and_bindful_edram);
    current_graphics_root_up_to_date_ |= 1u << root_parameter_shared_memory_and_bindful_edram;
  }
  if (bindless_resources_used_) {
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesPixel))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesPixel,
          cbuffer_binding_descriptor_indices_pixel_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesPixel;
    }
    if (!(current_graphics_root_up_to_date_ &
          (1u << kRootParameter_Bindless_DescriptorIndicesVertex))) {
      deferred_command_list_.D3DSetGraphicsRootConstantBufferView(
          kRootParameter_Bindless_DescriptorIndicesVertex,
          cbuffer_binding_descriptor_indices_vertex_.address);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_DescriptorIndicesVertex;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_SamplerHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_SamplerHeap,
                                                               sampler_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_SamplerHeap;
    }
    if (!(current_graphics_root_up_to_date_ & (1u << kRootParameter_Bindless_ViewHeap))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(kRootParameter_Bindless_ViewHeap,
                                                               view_bindless_heap_gpu_start_);
      current_graphics_root_up_to_date_ |= 1u << kRootParameter_Bindless_ViewHeap;
    }
  } else {
    uint32_t extra_index;
    extra_index = current_graphics_root_bindful_extras_.textures_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_pixel;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_pixel_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.textures_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_textures_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
    extra_index = current_graphics_root_bindful_extras_.samplers_vertex;
    if (extra_index != RootBindfulExtraParameterIndices::kUnavailable &&
        !(current_graphics_root_up_to_date_ & (1u << extra_index))) {
      deferred_command_list_.D3DSetGraphicsRootDescriptorTable(extra_index,
                                                               gpu_handle_samplers_vertex_);
      current_graphics_root_up_to_date_ |= 1u << extra_index;
    }
  }

  return true;
}

void D3D12CommandProcessor::EvictOldReadbackBuffers(
    std::unordered_map<uint64_t, ReadbackBuffer>& buffer_map) {
  if (buffer_map.empty()) {
    return;
  }
  const uint64_t eviction_frame_floor = (frame_current_ > kReadbackBufferEvictionAgeFrames)
                                            ? (frame_current_ - kReadbackBufferEvictionAgeFrames)
                                            : 0;
  for (auto it = buffer_map.begin(); it != buffer_map.end();) {
    ReadbackBuffer& readback = it->second;
    bool evict =
        buffer_map.size() > kMaxReadbackBuffers || readback.last_used_frame < eviction_frame_floor;
    if (!evict) {
      ++it;
      continue;
    }
    for (uint32_t i = 0; i < 2; ++i) {
      if (readback.buffers[i]) {
        if (readback.mapped_data[i]) {
          readback.buffers[i]->Unmap(0, nullptr);
        }
        readback.buffers[i]->Release();
      }
      readback.buffers[i] = nullptr;
      readback.mapped_data[i] = nullptr;
      readback.sizes[i] = 0;
      readback.submission_written[i] = 0;
      readback.written_size[i] = 0;
    }
    it = buffer_map.erase(it);
  }
}

ID3D12Resource* D3D12CommandProcessor::RequestReadbackBuffer(uint32_t size) {
  if (size == 0) {
    return nullptr;
  }
  size = rex::align(size, kReadbackBufferSizeIncrement);
  if (size > readback_buffer_size_) {
    const ui::d3d12::D3D12Provider& provider = GetD3D12Provider();
    ID3D12Device* device = provider.GetDevice();
    D3D12_RESOURCE_DESC buffer_desc;
    ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* buffer;
    if (FAILED(device->CreateCommittedResource(
            &ui::d3d12::util::kHeapPropertiesReadback, provider.GetHeapFlagCreateNotZeroed(),
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
      REXGPU_ERROR("Failed to create a {} MB readback buffer", size >> 20);
      return nullptr;
    }
    if (readback_buffer_ != nullptr) {
      readback_buffer_->Release();
    }
    readback_buffer_ = buffer;
    readback_buffer_size_ = size;
  }
  return readback_buffer_;
}

bool D3D12CommandProcessor::InitializeOcclusionQueryResources() {
  active_occlusion_query_ = {};
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
  occlusion_query_heap_.Reset();
  occlusion_query_readback_.Reset();
  occlusion_query_readback_mapping_ = nullptr;

  ID3D12Device* device = GetD3D12Provider().GetDevice();
  if (!device) {
    return false;
  }

  D3D12_QUERY_HEAP_DESC heap_desc;
  heap_desc.Type = D3D12_QUERY_HEAP_TYPE_OCCLUSION;
  heap_desc.Count = kMaxOcclusionQueries;
  heap_desc.NodeMask = 0;
  if (FAILED(device->CreateQueryHeap(&heap_desc, IID_PPV_ARGS(&occlusion_query_heap_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to create occlusion query heap, using fake sample counts");
    return false;
  }

  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, sizeof(uint64_t) * kMaxOcclusionQueries,
                                          D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             GetD3D12Provider().GetHeapFlagCreateNotZeroed(),
                                             &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&occlusion_query_readback_)))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to allocate occlusion query readback buffer, using fake "
        "sample counts");
    occlusion_query_heap_.Reset();
    return false;
  }

  D3D12_RANGE read_range = {0, sizeof(uint64_t) * kMaxOcclusionQueries};
  void* mapping = nullptr;
  if (FAILED(occlusion_query_readback_->Map(0, &read_range, &mapping))) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Failed to map occlusion query readback buffer, using fake sample "
        "counts");
    occlusion_query_readback_.Reset();
    occlusion_query_heap_.Reset();
    return false;
  }

  occlusion_query_readback_mapping_ = reinterpret_cast<uint64_t*>(mapping);
  occlusion_query_resources_available_ = true;
  return true;
}

void D3D12CommandProcessor::ShutdownOcclusionQueryResources() {
  DisableHostOcclusionQueries();
  Fable2DropPendingQueries();  // [PERF-1006c]

  if (occlusion_query_readback_ && occlusion_query_readback_mapping_) {
    occlusion_query_readback_->Unmap(0, nullptr);
  }
  occlusion_query_readback_mapping_ = nullptr;
  occlusion_query_readback_.Reset();
  occlusion_query_heap_.Reset();
}

bool D3D12CommandProcessor::AcquireOcclusionQueryIndex(uint32_t& host_index_out) {
  if (occlusion_query_cursor_ >= kMaxOcclusionQueries) {
    occlusion_query_cursor_ = 0;
  }
  host_index_out = occlusion_query_cursor_++;
  return true;
}

void D3D12CommandProcessor::DisableHostOcclusionQueries() {
  if (active_occlusion_query_.valid && occlusion_query_heap_) {
    uint32_t host_index = active_occlusion_query_.host_index;
    // Clear before EndSubmission to prevent the EndSubmission safety net from issuing a second
    // EndQuery for the same index.
    active_occlusion_query_ = {};
    if (BeginSubmission(true)) {
      deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                         host_index);
      EndSubmission(false);
    }
  } else {
    active_occlusion_query_ = {};
  }
  occlusion_query_cursor_ = 0;
  occlusion_query_resources_available_ = false;
}

bool D3D12CommandProcessor::BeginGuestOcclusionQuery(uint32_t sample_count_address) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_) {
    return false;
  }
  if (active_occlusion_query_.valid) {
    REXGPU_WARN(
        "D3D12CommandProcessor: Occlusion query begin issued while another query is active");
    DisableHostOcclusionQueries();
    return false;
  }

  uint32_t host_index = 0;
  if (!AcquireOcclusionQueryIndex(host_index)) {
    return false;
  }
  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DBeginQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                       host_index);
  active_occlusion_query_.sample_count_address = sample_count_address;
  active_occlusion_query_.host_index = host_index;
  active_occlusion_query_.valid = true;
  return true;
}

// [PERF-1006c]: pending asynchronous occlusion queries (CP thread only).
namespace {
struct Fable2PendingQuery {
  xenos::xe_gpu_depth_sample_counts* sample_counts;
  uint32_t sample_count_address;
  uint32_t host_index;
  uint64_t submission;
  uint64_t resolution_scale;
};
std::deque<Fable2PendingQuery> fable2_pending_queries_;
ID3D12Fence* fable2_query_fence_ = nullptr;
const uint64_t* fable2_query_mapping_ = nullptr;
constexpr size_t kFable2MaxPendingQueries = 2048;  // host index ring is 8192

void Fable2WriteQueryResult(xenos::xe_gpu_depth_sample_counts* sample_counts,
                            uint64_t samples) {
  // Same layout as D3D12CommandProcessor::WriteGuestOcclusionResult.
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void Fable2CompletePendingQueries(uint64_t completed_submission) {
  while (!fable2_pending_queries_.empty() &&
         fable2_pending_queries_.front().submission <= completed_submission) {
    const Fable2PendingQuery& q = fable2_pending_queries_.front();
    if (q.sample_counts && fable2_query_mapping_) {
      uint64_t samples = fable2_query_mapping_[q.host_index];
      if (samples && q.resolution_scale > 1) {
        samples = (samples + (q.resolution_scale >> 1)) / q.resolution_scale;
      }
      Fable2WriteQueryResult(q.sample_counts, samples);
    }
    fable2_pending_queries_.pop_front();
  }
}

// [PERF-1006d]: asynchronous memexport readback state (CP thread only).
struct Fable2ReadbackBuffer {
  ID3D12Resource* buffer = nullptr;
  uint8_t* mapping = nullptr;
  uint32_t size = 0;
};
struct Fable2PendingMemexport {
  Fable2ReadbackBuffer readback;
  uint64_t submission;
  std::vector<Fable2MemexportCopy> copies;
};
std::deque<Fable2PendingMemexport> fable2_pending_memexport_;
std::vector<Fable2ReadbackBuffer> fable2_free_readback_buffers_;
Fable2ReadbackBuffer fable2_current_readback_;  // between Begin and End
ID3D12Fence* fable2_memexport_fence_ = nullptr;

void Fable2CompletePendingMemexport(uint64_t completed_submission) {
  while (!fable2_pending_memexport_.empty() &&
         fable2_pending_memexport_.front().submission <= completed_submission) {
    Fable2PendingMemexport& done = fable2_pending_memexport_.front();
    for (const Fable2MemexportCopy& copy : done.copies) {
      if (!copy.destination) {
        continue;
      }
      // Skip if a newer pending copy covers any of this range - it carries
      // fresher GPU data and will be written when it completes.
      bool superseded = false;
      for (size_t i = 1; i < fable2_pending_memexport_.size() && !superseded; ++i) {
        for (const Fable2MemexportCopy& newer : fable2_pending_memexport_[i].copies) {
          if (newer.guest_address < copy.guest_address + copy.size &&
              copy.guest_address < newer.guest_address + newer.size) {
            superseded = true;
            break;
          }
        }
      }
      if (!superseded) {
        std::memcpy(copy.destination, done.readback.mapping + copy.buffer_offset, copy.size);
      }
    }
    fable2_free_readback_buffers_.push_back(done.readback);
    fable2_pending_memexport_.pop_front();
  }
}

void Fable2DropPendingQueries() {
  fable2_pending_queries_.clear();
  fable2_query_fence_ = nullptr;
  fable2_query_mapping_ = nullptr;
  // [PERF-1006d]: also release the memexport readback pool (shutdown only).
  for (Fable2PendingMemexport& pending : fable2_pending_memexport_) {
    fable2_free_readback_buffers_.push_back(pending.readback);
  }
  fable2_pending_memexport_.clear();
  for (Fable2ReadbackBuffer& b : fable2_free_readback_buffers_) {
    if (b.buffer) {
      b.buffer->Unmap(0, nullptr);
      b.buffer->Release();
    }
  }
  fable2_free_readback_buffers_.clear();
  fable2_memexport_fence_ = nullptr;
}
}  // namespace

ID3D12Resource* Fable2BeginAsyncMemexport(ID3D12Device* device, uint32_t total_size) {
  // Smallest free buffer that fits, else a new one (power of two, >= 64 KB).
  size_t best = SIZE_MAX;
  for (size_t i = 0; i < fable2_free_readback_buffers_.size(); ++i) {
    const Fable2ReadbackBuffer& b = fable2_free_readback_buffers_[i];
    if (b.size >= total_size &&
        (best == SIZE_MAX || b.size < fable2_free_readback_buffers_[best].size)) {
      best = i;
    }
  }
  if (best != SIZE_MAX) {
    fable2_current_readback_ = fable2_free_readback_buffers_[best];
    fable2_free_readback_buffers_[best] = fable2_free_readback_buffers_.back();
    fable2_free_readback_buffers_.pop_back();
    return fable2_current_readback_.buffer;
  }
  uint32_t size = 64 * 1024;
  while (size < total_size && size < (1u << 30)) {
    size <<= 1;
  }
  if (size < total_size) {
    return nullptr;
  }
  D3D12_RESOURCE_DESC buffer_desc;
  ui::d3d12::util::FillBufferResourceDesc(buffer_desc, size, D3D12_RESOURCE_FLAG_NONE);
  ID3D12Resource* buffer = nullptr;
  if (FAILED(device->CreateCommittedResource(&ui::d3d12::util::kHeapPropertiesReadback,
                                             D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&buffer)))) {
    return nullptr;
  }
  D3D12_RANGE read_range = {0, size};
  void* mapping = nullptr;
  if (FAILED(buffer->Map(0, &read_range, &mapping))) {
    buffer->Release();
    return nullptr;
  }
  fable2_current_readback_.buffer = buffer;
  fable2_current_readback_.mapping = static_cast<uint8_t*>(mapping);
  fable2_current_readback_.size = size;
  return buffer;
}

void Fable2EndAsyncMemexport(ID3D12Fence* fence, uint64_t submission,
                             std::vector<Fable2MemexportCopy>&& copies) {
  fable2_memexport_fence_ = fence;
  Fable2PendingMemexport pending;
  pending.readback = fable2_current_readback_;
  pending.submission = submission;
  pending.copies = std::move(copies);
  fable2_pending_memexport_.push_back(std::move(pending));
  fable2_current_readback_ = {};
}

size_t Fable2PendingMemexportCount() { return fable2_pending_memexport_.size(); }

uint64_t Fable2OldestPendingMemexportSubmission() {
  return fable2_pending_memexport_.empty() ? 0 : fable2_pending_memexport_.front().submission;
}

void Fable2PollPendingQueriesNow() {
  if (!fable2_pending_queries_.empty() && fable2_query_fence_) {
    Fable2CompletePendingQueries(fable2_query_fence_->GetCompletedValue());
  }
  if (!fable2_pending_memexport_.empty() && fable2_memexport_fence_) {
    Fable2CompletePendingMemexport(fable2_memexport_fence_->GetCompletedValue());
  }
}

bool D3D12CommandProcessor::EndGuestOcclusionQuery(
    uint32_t sample_count_address, xenos::xe_gpu_depth_sample_counts* sample_counts) {
  if (!REXCVAR_GET(occlusion_query_enable) || !occlusion_query_resources_available_ ||
      !active_occlusion_query_.valid || !occlusion_query_heap_ || !occlusion_query_readback_) {
    return false;
  }

  uint32_t host_index = active_occlusion_query_.host_index;
  active_occlusion_query_ = {};

  if (!BeginSubmission(true)) {
    return false;
  }

  deferred_command_list_.D3DEndQuery(occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION,
                                     host_index);
  deferred_command_list_.D3DResolveQueryData(
      occlusion_query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, host_index, 1,
      occlusion_query_readback_.Get(), sizeof(uint64_t) * host_index);

  if (!EndSubmission(false)) {
    return false;
  }

  uint64_t query_submission = submission_current_ ? submission_current_ - 1 : 0;

  // [PERF-1006c]: asynchronous path - see fable2_async_occlusion_queries.
  if (REXCVAR_GET(fable2_async_occlusion_queries) && occlusion_query_readback_mapping_) {
    fable2_query_fence_ = submission_fence_;
    fable2_query_mapping_ = occlusion_query_readback_mapping_;
    // A newer query on the same guest address supersedes an older pending one
    // (its result would otherwise overwrite the newer query's markers).
    for (Fable2PendingQuery& pending : fable2_pending_queries_) {
      if (pending.sample_count_address == sample_count_address) {
        pending.sample_counts = nullptr;
      }
    }
    uint64_t scale = 1;
    if (texture_cache_) {
      scale = uint64_t(texture_cache_->draw_resolution_scale_x()) *
              uint64_t(texture_cache_->draw_resolution_scale_y());
    }
    fable2_pending_queries_.push_back(
        {sample_counts, sample_count_address, host_index, query_submission, scale});
    bool waited = false;
    if (fable2_pending_queries_.size() > kFable2MaxPendingQueries) {
      // Far too many in flight (host index ring safety) - wait for the oldest.
      CheckSubmissionFence(fable2_pending_queries_.front().submission);
      waited = true;
    }
    Fable2CompletePendingQueries(submission_fence_->GetCompletedValue());
    Fable2PerfCountQuery(waited);
    return true;
  }
  Fable2PerfCountQuery(true);

  CheckSubmissionFence(query_submission);
  if (submission_completed_ < query_submission) {
    return false;
  }
  if (!occlusion_query_readback_mapping_) {
    return false;
  }

  uint64_t samples = occlusion_query_readback_mapping_[host_index];
  samples = NormalizeOcclusionSamples(samples);
  WriteGuestOcclusionResult(sample_counts, samples);
  return true;
}

uint64_t D3D12CommandProcessor::NormalizeOcclusionSamples(uint64_t samples) const {
  if (samples == 0 || !texture_cache_) {
    return samples;
  }
  uint64_t scale_x = texture_cache_->draw_resolution_scale_x();
  uint64_t scale_y = texture_cache_->draw_resolution_scale_y();
  uint64_t scale = scale_x * scale_y;
  if (scale <= 1) {
    return samples;
  }
  return (samples + (scale >> 1)) / scale;
}

void D3D12CommandProcessor::WriteGuestOcclusionResult(
    xenos::xe_gpu_depth_sample_counts* sample_counts, uint64_t samples) {
  if (!sample_counts) {
    return;
  }
  uint32_t clamped = samples > uint64_t(UINT32_MAX) ? UINT32_MAX : uint32_t(samples);
  sample_counts->Total_A = clamped;
  sample_counts->Total_B = 0;
  sample_counts->ZPass_A = clamped;
  sample_counts->ZPass_B = 0;
  sample_counts->ZFail_A = 0;
  sample_counts->ZFail_B = 0;
  sample_counts->StencilFail_A = 0;
  sample_counts->StencilFail_B = 0;
}

void D3D12CommandProcessor::WriteGammaRampSRV(bool is_pwl,
                                              D3D12_CPU_DESCRIPTOR_HANDLE handle) const {
  ID3D12Device* device = GetD3D12Provider().GetDevice();
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  if (is_pwl) {
    desc.Format = DXGI_FORMAT_R16G16_UINT;
    desc.Buffer.FirstElement = 256 * 4 / 4;
    desc.Buffer.NumElements = 128 * 3;
  } else {
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.Buffer.FirstElement = 0;
    desc.Buffer.NumElements = 256;
  }
  device->CreateShaderResourceView(gamma_ramp_buffer_.Get(), &desc, handle);
}

}  // namespace rex::graphics::d3d12

// [PERF-1006c]: lets the CP idle loop deliver occlusion results while the game
// polls for them without submitting new commands.
void Fable2CpIdlePoll() { rex::graphics::d3d12::Fable2PollPendingQueriesNow(); }
