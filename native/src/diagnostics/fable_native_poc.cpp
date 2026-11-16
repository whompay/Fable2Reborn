// Fable2PC -- Phase 3 diagnostic instrumentation: NativeDrawPoc (implementation)
//
// See fable_native_poc.h for the module's purpose/contract and
// native/native-rendering-phase3.md for the full design writeup, including
// every assumption this file makes explicit below.
//
// Include order note (Phase 3 regression fix, native-rendering-phase3.md):
// fable2_pch.h is included first here, matching fable_draw_capture.cpp's
// proven-working order, even though the original order was never proven to
// be the cause of the startup crash -- zero-cost defensive normalization.
#include "fable2_pch.h"  // REX_LOAD_U16/U32, REX_RAW_ADDR (all require a local `base`)

#include "diagnostics/fable_native_poc.h"
#include "diagnostics/native_draw_repr.h"  // round 7: backend-neutral NativeDrawCommand/INativeRenderBackend
#include "diagnostics/fable_native_d3d12_backend.h"  // round 20: real D3D12 native 3D draw path

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // Phase 4 follow-up: kernel_memory(), see LogAddressDiagnostics
#include <rex/system/xmemory.h>       // Phase 4 follow-up: Memory/BaseHeap/HeapAllocationInfo, ditto

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>   // std::snprintf -- round 5 write-watch stack-trace formatting, see TickWriteWatch
#include <chrono>   // Round 20, twenty-third follow-up: steady_clock, live on-screen watch timestamps
#include <cstring>
#include <excpt.h>  // EXCEPTION_EXECUTE_HANDLER (Phase 3 regression fix -- see TryDecodeVertex)
#include <limits>   // Round 20, seventeenth follow-up: std::numeric_limits, AutoVerify NDC-range scan
#include <mutex>
#include <string>
#include <utility>  // std::pair -- PhysicalMemoryInvalidationCallback's return type, see below
#include <vector>   // Round 16, twenty-ninth follow-up: BuildDrawCommandFromValidatedFetchConstant

// GetCurrentThreadId/QueryPerformanceCounter/CaptureStackBackTrace, round 5 write-watch
// instrumentation below. Included unconditionally, consistent with this file's existing
// unconditional use of Windows SEH (__try/__except/EXCEPTION_EXECUTE_HANDLER) elsewhere --
// this module already assumes Windows throughout, unlike native/src/glyph_system.cpp (which
// guards its own <windows.h> include because the rest of that file is more platform-general).
#include <windows.h>

#include <imgui.h>

//=============================================================================
// CVars -- every one of these exists because this module makes an assumption
// Phase 1/2's evidence does not settle, and the user may need to correct it
// by eye once real geometry (or garbage) shows up on screen. None of these
// affect the existing ReXGlue/Xenos rendering path or FableDrawCapture.
//=============================================================================

// Master on/off switch. Off by default: this module does nothing (not even
// the cheap capture_active check in fable_draw_capture.cpp's wrapper costs
// more than one relaxed atomic load) until explicitly enabled.
REXCVAR_DEFINE_BOOL(fable2_native_poc_enable, false, "Fable II",
    "Phase 3: capture one runtime-validated sub_82217EE8 draw and render it "
    "natively via the F4 overlay (ImGui draw list, no new shader code, no "
    "effect on existing rendering). See native/native-rendering-phase3.md.");

// UNKNOWN, and stated as such in the overlay: Phase 1/2 never observed a
// vertex declaration/FVF/shader-input-signature at this call boundary, only
// the raw scratch-buffer address the vertex fetch will read from. Originally
// defaulted to 32 (position + normal + one UV, a common Xbox 360-era
// static-mesh stride) -- still a labeled guess, not a finding, but changed
// to 64 in round 15's seventh follow-up after fable2_064.log's interactive
// re-decode experiment (via the new "Re-decode now" overlay slider, see
// ReDecodeWithCurrentCvars() and native-rendering-phase3.md): of every
// stride tried against the same captured sub_8221E0F0 draw (11, 16, 18, 22,
// 32, 47, 64, 87), only 64 produced a decode with NO enormous-but-finite
// values at all (every other tried stride, including the old 32 default,
// produced at least one obviously-wrong value like 1e15-1e24 or a
// suspicious round power-of-two like -131072). Still not proof -- adjust
// and compare visually via the overlay's live slider, which overrides this
// default at runtime with no rebuild needed.
REXCVAR_DEFINE_UINT32(fable2_native_poc_vertex_stride, 64, "Fable II",
    "Phase 3 (UNVALIDATED GUESS, round 15 seventh follow-up raised the "
    "default from 32 to 64 -- see native-rendering-phase3.md): bytes per "
    "vertex record when decoding the captured draw's vertex buffer for the "
    "F4 native-draw overlay.");

// Index width. Xenos/D3D9-style 16-bit indices are by far the common case
// and the safe default, but this is not independently confirmed for this
// specific draw either -- exposed rather than hardcoded.
REXCVAR_DEFINE_BOOL(fable2_native_poc_index32, false, "Fable II",
    "Phase 3 (UNVALIDATED GUESS): true = decode the captured draw's index "
    "buffer as 32-bit indices instead of the default 16-bit.");

// Every OTHER field this whole investigation has decoded (packet headers,
// device-object offsets, the DRAW_INDX payload itself) is confirmed
// big-endian, matching REX_LOAD_U32's byteswap. Whether the vertex buffer's
// own float payload bytes follow that same convention, or were pre-swizzled
// for the GPU's native (little-endian-on-PC-Xenos, but this is an Xbox 360
// title) consumption, is genuinely unverified -- hence a runtime toggle
// instead of an assumption baked into the read.
REXCVAR_DEFINE_BOOL(fable2_native_poc_swap_geometry_bytes, true, "Fable II",
    "Phase 3 (UNVALIDATED GUESS): true = byteswap vertex-buffer floats like "
    "every other confirmed big-endian field in this investigation; false = "
    "read them raw (no swap). Toggle if the F4 overlay's geometry looks "
    "wrong/garbled.");

// Round 15, twelfth follow-up (native-rendering-phase3.md, fable2_070.log):
// ArmWriteWatch() calls into rex's own RegisterPhysicalMemoryInvalidationCallback/
// EnablePhysicalMemoryAccessCallbacks for the FIRST captured draw whenever
// resource_calls_captured is true -- in practice, always the trivial seq=0
// draw (prim_type=4, index_count=6) that fires before any real gameplay
// draw, since no source filter is active yet at that point. fable2_070.log
// shows this call sequence itself triggering "Unhandled guest access
// violation: read of guest ..." from INSIDE rex's own internals (not any
// REX_LOAD/memcpy this module performs), and this time it was fatal within
// ~50ms of arming -- before the overlay could even be opened. An earlier
// run (fable2_067.log) hit the exact same log line for the same mechanism
// and survived, running for minutes afterward -- consistent with this
// project's own established finding that rex's guest-fault recovery is not
// reliable in every case, not with anything new this round changed. Since
// this watch exists only to observe async population for a draw type this
// investigation isn't even targeting (sub_8221E0F0, this session's actual
// focus, never has resource_calls_captured=true and so never reaches this
// code path at all), it's disabled by default here rather than left as a
// crash that can kill the process before the real target draw is ever
// reached. Re-enable via cvar if the write-watch milestone itself needs
// revisiting later.
REXCVAR_DEFINE_BOOL(fable2_native_poc_enable_write_watch, false, "Fable II",
    "Phase 4 round 5 feature (DISABLED BY DEFAULT since round 15's twelfth "
    "follow-up -- see native-rendering-phase3.md): arm rex's physical-memory "
    "write watch on the first captured draw's vertex allocation. Off by "
    "default because this has been observed to crash the process (inside "
    "rex's own callback registration, not this module's own reads) for the "
    "trivial seq=0 draw that always fires before any real gameplay draw.");

namespace fable2::native_poc {

namespace {

std::atomic<bool> g_have_draw{false};
// Round 13 (native-rendering-phase3.md): 0 = accept a validated draw from
// any hooked draw-flush-family function (the original, round-11 behavior).
// Non-zero = only accept one from that specific hook_addr. Added because
// round 12's log evidence showed sub_82217EE8 firing every frame while
// sub_8221C518 only fires occasionally (e.g. once per UI widget) -- an
// unfiltered re-arm almost always lets sub_82217EE8 win the race back,
// even during real gameplay, starving sub_8221C518 of a chance to ever be
// the one captured. Set by the overlay's filter radio buttons, read by
// WantsNextValidatedDrawFrom() below.
std::atomic<uint32_t> g_source_filter{0};
std::mutex g_draw_mutex;
ValidatedDrawInfo g_draw;  // guarded by g_draw_mutex

// Round 16, twenty-ninth follow-up: second, independent hand-off slot for
// the fetch-constant-derived vertex stream (see ValidatedFetchConstantVertexInfo's
// own comment in fable_native_poc.h). Guarded by the same g_draw_mutex --
// there is no reason to add a second lock for two small, rarely-written
// PoC snapshots that are always read together on the same overlay frame.
ValidatedFetchConstantVertexInfo g_fetch_const_vertex;  // guarded by g_draw_mutex
bool g_have_fetch_const_vertex = false;                 // guarded by g_draw_mutex

// Round 20, twenty-third follow-up (view/projection reconstruction --
// native-rendering-phase3.md): after five real play sessions in a row
// never reproduced the leading c[72] perspective candidate at all (user
// feedback, verbatim: "I've literally tested it 5 times, walk around
// almost the entire map & you still don't see anything"), blind "please
// play more and upload another log" stopped being a reasonable ask. This
// is a LIVE, on-screen signal instead: the F4 overlay now shows, updated
// every frame, (a) whether/when the leading candidate's own two known
// shader hashes have been bound AT ALL this process, independent of
// whether a candidate was found in that specific capture, (b) the most
// recent kind=perspective PROJECTION candidate seen on ANY shader, and
// (c) the most recent [AutoVerify] composition result. The person
// playing can now watch this update live and deliberately linger in
// whatever they were just doing the moment something lights up, instead
// of wandering blindly and finding out afterward, from a log upload,
// that nothing new happened. Guarded by the same g_draw_mutex as the
// fetch-constant snapshot above -- same "small, rarely-written PoC
// state, always read together on the same overlay frame" reasoning.
struct ViewProjectionLiveWatch {
  bool target_shader_seen = false;
  uint64_t target_shader_hash = 0;
  std::chrono::steady_clock::time_point target_shader_time{};

  bool perspective_seen = false;
  uint64_t perspective_hash = 0;
  uint32_t perspective_base = 0;
  std::chrono::steady_clock::time_point perspective_time{};

  bool autoverify_seen = false;
  uint64_t autoverify_hash = 0;
  uint32_t autoverify_base = 0;
  bool autoverify_world_space = false;
  float autoverify_ndc_x_min = 0.0f;
  float autoverify_ndc_x_max = 0.0f;
  float autoverify_ndc_y_min = 0.0f;
  float autoverify_ndc_y_max = 0.0f;
  int autoverify_behind_or_degenerate = 0;
  int autoverify_vertex_count = 0;
  // Round 20, twenty-fourth follow-up: count of vertices whose NDC x/y
  // BOTH land within a generous [-2,2] window (not the raw min/max,
  // which fable2_125.1.log's own leading-candidate result showed can be
  // blown up arbitrarily far by a single near-zero-clip_w outlier -- see
  // this same round's writeup). The resilient statistic to actually
  // judge plausibility by.
  int autoverify_plausible_ndc_count = 0;
  std::chrono::steady_clock::time_point autoverify_time{};
};
ViewProjectionLiveWatch g_vp_watch;  // guarded by g_draw_mutex
// The leading candidate's own two known shader hashes -- see
// native-rendering-phase3.md round 20, seventeenth/eighteenth follow-ups.
// ucode_hash=0x129f7f2793da9084 and 0xd4d558da6a82bdc8 have both produced
// the SAME structural candidate (row0=(0.941176,0.0294118,0,0),
// row1=(0,1.73763,0,0), row2=(0,0,1.00002,-0.100002), row3=(0,0,1,0);
// near~0.1/far~5000/fovY~60deg) at logical base c[72].
constexpr uint64_t kLeadingCandidateHashes[] = {0x129f7f2793da9084ULL, 0xd4d558da6a82bdc8ULL};

struct DecodedVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
  // Round 15 follow-up: the raw index value LoadGuestIndex() pulled from
  // the index buffer for this vertex, before it's multiplied by stride and
  // added to the vertex base address. Surfaced so implausible-looking
  // decoded positions (e.g. fable2_061.log's screenshot -- mostly small
  // confined values but a few enormous, clearly-not-a-real-coordinate
  // floats recurring at a fixed period) can be diagnosed as "the indices
  // themselves are garbage/out of range" vs. "the indices are sane small
  // numbers but the vertex_addr/stride/format guess is what's wrong" --
  // two very different problems that would otherwise look identical from
  // the decoded x/y/z alone.
  uint32_t raw_index = 0;
  // Round 15 follow-up (fable2_062.log): true when raw_index is the
  // standard GPU strip/fan-cut sentinel (0xFFFF for a 16-bit index buffer,
  // 0xFFFFFFFF for 32-bit) -- "restart here, this slot names no real
  // vertex," not a literal index to fetch. Confirmed, not guessed:
  // fable2_062.log's new raw-index logging showed `0, 1, 2, 3, 4, 65535, 5,
  // 6, 7, 8, 9, ..., 65535` -- 65535 recurring at clean strip-length
  // intervals is exactly the multi-strip/fan restart pattern, not noise.
  // Before this fix, TryDecodeVertex treated 65535 as a literal index,
  // multiplying it by stride (65535*32 = ~2MB past vertex_addr) and
  // reading whatever unrelated memory happened to be there -- explaining
  // the screenshot's small-but-still-wrong repeating value at every 65535
  // slot. This vertex is excluded from decoding (no address computed, no
  // read attempted) rather than fed a nonsense address.
  bool is_restart = false;
};
constexpr int kMaxDecodedIndices = 64;  // this draw is 6; generous headroom, not a real limit

// Phase 4 (native-rendering-phase3.md): the vertex address used below is an
// unvalidated guess either way -- the regression this session fixed proved
// that the formula-transformed candidate produces an unreadable address for
// at least one real captured draw. Rather than pick a replacement formula
// blind, this module now decodes from BOTH plausible candidate addresses
// and reports both results side by side, without asserting which (if
// either) is correct. See DecodeCandidate() below and OnValidatedDraw().
struct DecodeAttempt {
  bool attempted = false;
  bool read_fault = false;
  int fault_at_index = -1;
  bool plausible = false;
  int count = 0;
  DecodedVertex verts[kMaxDecodedIndices];
  std::string error;
};
DecodeAttempt g_decode_raw;      // candidate A: vertex_addr_raw used AS-IS, no formula applied
DecodeAttempt g_decode_formula;  // candidate B: PredictScratchAddressField(vertex_addr_raw)

// Round 6 (maximum-forward-progress pass): rounds 4/5 proved candidate A's
// allocation is all-zero AT CAPTURE TIME and only becomes populated roughly
// three seconds later (an async write, not yet attributed to a specific
// writer -- the write-watch armed above is what's chasing that). The very
// first OnValidatedDraw() decode above therefore reads real, structurally
// valid, but NOT YET WRITTEN memory -- a guaranteed-implausible decode that
// says nothing about whether candidate A is the right address. This struct
// captures exactly the parameters that first decode used, so
// TickAddressWatch() can re-run the SAME decode, with the SAME formula, the
// moment it observes the buffer go non-zero -- turning "population
// happened" into "here is what the real geometry decodes to," without
// guessing a new address or format.
struct DeferredDecodeParams {
  bool valid = false;
  uint32_t index_addr = 0;
  bool index32 = false;
  uint32_t stride = 4;
  int index_count = 0;
  uint32_t candidate_addr_raw = 0;
  uint32_t candidate_addr_formula = 0;
};
DeferredDecodeParams g_deferred_decode_params;  // written once in OnValidatedDraw, read once (by
                                                 // TickAddressWatch, same thread that already reads
                                                 // g_watch_addr) when population is first observed

// Phase 4 follow-up (address-semantics trace, round 2): LogAddressDiagnostics
// confirmed candidate A's raw address (sub_82206820 call #1's own return
// value, self+13448/vertex_call_return_addr) resolves via Memory::LookupHeap
// to a heap_type=kGuestPhysical alias region that QueryRegionInfo reports as
// RESERVE|COMMIT (state=0x3) with Read|Write|WriteCombine protection -- i.e.
// rex's own bookkeeping says this address IS backed by committed, readable
// memory. Yet TryDecodeVertex's existing decode still reports a fault "at
// vertex 0". TryDecodeVertex's __try block does TWO guest reads before it
// ever touches vertex_addr itself: first the INDEX load at idx_addr (a
// DIFFERENT, already-Phase-2-validated address), then
// `v_addr = vertex_addr + vertex_index * stride` using whatever index value
// that first read produced -- so a single caught SEH fault, with no
// per-read tag, does not by itself say vertex_addr is unreadable; it could
// equally be the index read, or a `vertex_index` value large enough to walk
// v_addr outside the committed region QueryRegionInfo just reported. This
// struct/global captures a completely separate, unconditional raw dword
// dump AT vertex_call_return_addr itself -- no index, no stride, nothing
// derived -- to settle that ambiguity directly. See DumpRawDwords() below.
constexpr int kRawDumpDwords = 8;  // 32 bytes: enough to eyeball, not a format claim
struct RawDumpResult {
  bool attempted = false;
  bool fault = false;
  uint32_t addr = 0;
  uint32_t dwords[kRawDumpDwords] = {};
};
RawDumpResult g_raw_dump_call1;  // Round 15: now dumped at candidate_addr_raw (==
                                  // vertex_addr_raw/candidate A) rather than
                                  // info.vertex_call_return_addr specifically -- the two are the
                                  // same address whenever resource_calls_captured is true (per
                                  // round 4's own finding), and candidate_addr_raw is ALSO valid,
                                  // always-populated ground truth when it's false, which
                                  // vertex_call_return_addr is not (documented zero in that case).
                                  // Guarded by g_draw_mutex, updated both by the one-shot capture
                                  // AND, while g_watch_active, every subsequent frame by
                                  // TickAddressWatch() below, so this is always the MOST RECENT
                                  // observation, not just the capture-time snapshot.
RawDumpResult g_raw_dump_index;   // Round 15: a second, independent raw dword dump at index_addr
                                  // (also always-populated ground truth, from payload[2]/the real
                                  // scanned DRAW_INDX packet or index_call_return_addr). Added
                                  // because TryDecodeVertex's own "faulted at vertex 0" message
                                  // cannot say whether the INDEX read or the VERTEX read is what
                                  // actually faulted (both happen inside the same __try block) --
                                  // this settles that ambiguity directly, the same way the
                                  // call-1-return-value dump above already settled it for the
                                  // vertex side. One-shot only (not re-read per frame by
                                  // TickAddressWatch -- the index buffer isn't expected to be a
                                  // not-yet-populated bump allocation the way call #1's vertex
                                  // buffer was hypothesized to be, so there's no "wait for it to
                                  // get written" story here to justify polling it every frame).

// Phase 4 follow-up (round 3): the one-shot capture-time dump above proved
// call #1's raw address IS backed by committed, Read|Write-able memory
// (LogAddressDiagnostics) yet read all zero at that instant. Static tracing
// of sub_82217EE8's own body (every REX_STORE_* from immediately after the
// two sub_82206820 calls through the function's own return) found NO
// lfs/stfs/lfd/stfd and no write of vertex position data anywhere in that
// function -- only PM4 command-stream bytes (fixed literals and the two
// formula-transformed resource-descriptor fields) and the final self+13448/
// 13452/13456/13460 bookkeeping stores already known from Phase 1. The
// function returns 0 (li r3,0), so it hands nothing back to ITS caller
// either. sub_822065F0's own tail call, sub_82206538, was also traced and
// is NOT a writer -- its self+10896-based compare/spin-on-sub_82B9BF90/
// release-via-sub_82B9BEC8 pattern (plus a REX_QUERY_TIMEBASE() call) reads
// as a ring-buffer consumer-fence WAIT, not a data copy. So nothing in call
// #1's own call tree (sub_82217EE8 -> sub_82206820 -> sub_822065F0 ->
// sub_82206538) writes vertex data -- population, if any, must happen
// later, outside this call tree entirely (the next draw's own call tree
// reusing the same growing scratch region, a separate upload/copy this
// investigation has not located, or population never happens for this
// specific draw at all). Rather than keep expanding the static call graph
// (explicitly out of scope this round), this re-reads the SAME address
// every subsequent frame -- while the F4 overlay is open, which already
// ticks once per frame -- to observe empirically whether/when it changes,
// without needing to first find where.
std::atomic<bool> g_watch_active{false};  // true once OnValidatedDraw has a capture to watch
std::atomic<bool> g_watch_done{false};    // true once a fault or a non-zero read has been logged,
                                           // so that one-time log fires exactly once
std::atomic<uint32_t> g_watch_addr{0};    // the address being watched (== info.vertex_call_return_addr)

// Phase 4 follow-up (round 5): round 4's per-frame poll proved call #1's
// allocation goes from all-zero to populated a few seconds after capture,
// but a once-per-frame poll only bounds WHEN to ~16ms, and says nothing
// about WHO performs the write. This adds a real write watch, using rex's
// OWN physical-memory invalidation-callback mechanism -- the same
// mechanism it uses internally to invalidate cached GPU resources when
// guest code writes a watched physical page
// (Memory::RegisterPhysicalMemoryInvalidationCallback /
// EnablePhysicalMemoryAccessCallbacks, xmemory.h ~line 488-501; backed by
// the same AccessViolationCallback machinery this investigation already
// found and described in round 2's notes on xmemory.cpp ~line 577-604) --
// rather than a competing VirtualProtect/PAGE_GUARD of our own, which
// could conflict with that same internal dirty-tracking. Passing
// enable_invalidation_notifications=true, enable_data_providers=false
// watches WRITES only (per xmemory.h's own comments distinguishing
// "invalidation notifications" from the separate, unimplemented "data
// providers" read-interception mechanism), so this module's own periodic
// reads (TickAddressWatch, DumpRawDwords) do not self-trigger it.
constexpr int kWriteWatchMaxFrames = 16;  // CaptureStackBackTrace frame cap -- generous headroom
struct WriteWatchResult {
  bool attempted = false;
  bool fired = false;
  uint32_t physical_address_start = 0;
  uint32_t length = 0;
  bool exact_range = false;
  DWORD thread_id = 0;
  LONGLONG qpc_ticks = 0;
  USHORT frame_count = 0;
  void* frames[kWriteWatchMaxFrames] = {};
};
// g_write_watch_result is written exactly once, from inside
// OnPhysicalMemoryInvalidated() (see its own comment for why that write is
// a raw field-by-field store guarded only by g_write_watch_fired's
// compare_exchange, not g_draw_mutex -- taking a std::mutex from inside
// rex's own access-violation callback machinery is avoided as an
// unnecessary and untested risk), and is read only after observing
// g_write_watch_fired==true from a normal per-frame context, at which
// point nothing but that one writer ever touches it again -- safe to read
// without a lock at that point.
std::atomic<bool> g_write_watch_fired{false};
WriteWatchResult g_write_watch_result;
std::atomic<bool> g_write_watch_reported{false};    // latches the deferred log to fire once
void* g_write_watch_callback_handle = nullptr;      // guarded by g_draw_mutex; touched only from
                                                     // normal (non-callback) contexts
std::atomic<LONGLONG> g_write_watch_armed_qpc{0};   // QueryPerformanceCounter() at arm time, for
                                                     // an elapsed-time-to-write figure in the log

// Duplicate, WITH THIS CROSS-REFERENCE, of
// fable2::diag::PredictScratchAddressField in fable_draw_capture.cpp
// (native/performance-investigation-phase1.md's "scratch address" formula,
// runtime-confirmed byte-exact for the INDEX case via payload2/VGT_DMA_BASE
// in Phase 2 attempt 3). Duplicated rather than shared so this experimental
// Phase 3 module cannot accidentally perturb the already-validated Phase 2
// instrumentation by linking against its internals. Applied here to the
// VERTEX scratch address too, by analogy -- Phase 2 only ever validated
// this formula's INDEX-address application; see native-rendering-phase3.md.
uint32_t RotL32(uint32_t v, uint32_t n) {
  n &= 31u;
  return n == 0 ? v : (v << n) | (v >> (32u - n));
}
uint32_t PredictScratchAddressField(uint32_t raw_addr) {
  uint32_t hi = RotL32(raw_addr, 12) & 0xFFFu;
  hi += 512u;
  hi &= 0x1000u;
  return hi + (raw_addr & 0x1FFFFFFFu);
}

// Round 15 follow-up (fable2_058/059/060.log): the INVERSE of the
// alias->canonical/physical relationship this file's index_addr comment
// (see below, above the `const uint32_t index_addr = ...` computation)
// already established -- payload[2]/VGT_DMA_BASE is confirmed canonical/
// physical-space, not the alias space REX_LOAD_U32 expects, and the
// existing fallback ("use payload[2] directly when resource_calls_captured
// is false") was knowingly feeding that wrong-space value straight into an
// alias-space reader ("degraded, but still attempts something" -- see that
// comment). This computes the actual alias-space address instead, so the
// fallback path can read real bytes rather than reliably fail.
//
// Derivation, not a guess: every candidate_addr_raw this investigation has
// captured (an alias address, always in this title's one observed 0xE0
// physical-alias heap: heap_base=0xe0000000, heap_size=0x1fd00000, per
// every LogAddressDiagnostics call so far) has its own canonical
// physical_addr logged right next to it (LogAddressDiagnostics's own
// GetPhysicalAddress() call). Five independent (alias, canonical) pairs
// pulled directly from fable2_058/059/060.log --
//   (0xff52e140, 0x1f52f140) (0xff5b6590, 0x1f5b7590) (0xff6e59b0, 0x1f6e69b0)
//   (0xff878f10, 0x1f879f10) (0xff9c7700, 0x1f9c8700)
// -- all satisfy the exact same relationship: canonical == (alias &
// 0x1FFFFFFF) + 0x1000, i.e. alias == ((canonical - 0x1000) & 0x1FFFFFFF) |
// 0xE0000000, with zero deviation across any of the five. The 0xE0000000
// top bits match this heap's own heap_base exactly, and the 0x1000
// adjustment matches this codebase's own documented
// detail::PhysicalHostOffset (xmemory.h: "Compensates for host allocation
// granularity coarser than 4KB on the 0xE0 physical heap... guest accesses
// must add it back") -- so this isn't a coincidental curve fit, it lines up
// with a mechanism this project's own SDK headers already document for
// this exact heap. Only meaningful for addresses that actually belong to
// this 0xE0 physical-alias heap, which every vertex/index buffer address
// observed in this entire investigation has been -- not a general-purpose
// address-space converter.
uint32_t CanonicalPhysicalToAlias(uint32_t canonical_addr) {
  return 0xE0000000u | ((canonical_addr - 0x1000u) & 0x1FFFFFFFu);
}

// Reads one guest-memory float, honoring fable2_native_poc_swap_geometry_bytes.
// `addr` is a guest address (not yet base-relative-resolved); REX_RAW_ADDR
// and REX_LOAD_U32 both do that resolution themselves via the `base` local
// required in scope by fable2_pch.h's macros.
float LoadGuestFloat(uint8_t* base, uint32_t addr) {
  uint32_t bits;
  if (REXCVAR_GET(fable2_native_poc_swap_geometry_bytes)) {
    bits = REX_LOAD_U32(addr);
  } else {
    std::memcpy(&bits, REX_RAW_ADDR(addr), sizeof(bits));
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

uint32_t LoadGuestIndex(uint8_t* base, uint32_t addr, bool index32) {
  return index32 ? REX_LOAD_U32(addr) : static_cast<uint32_t>(REX_LOAD_U16(addr));
}

// Phase 3 regression fix (native-rendering-phase3.md, "Root cause and fix"):
// vertex_addr is derived from PredictScratchAddressField() applied to the
// vertex case BY ANALOGY ONLY -- Phase 2 runtime-validated that formula for
// the INDEX address alone (payload2/VGT_DMA_BASE, 500/500 exact matches).
// vertex_index below is read directly out of guest index-buffer bytes with
// no format/range guarantee. Combined, `vertex_addr + vertex_index * stride`
// is a genuinely unvalidated guess at a guest address, and for a draw this
// module was never designed to assume is real mesh geometry (it hands off
// whatever the FIRST draw matching its current source filter happens to be,
// which can be a UI/menu element or a batched/reused-buffer draw with a completely different
// vertex format), that guess can and does land outside any committed guest
// heap region. rex's own guest memory system (Memory::AccessViolationCallback
// in xmemory.cpp) treats a read of such an address as an unrecoverable,
// process-fatal "Unhandled guest access violation" -- confirmed to be the
// exact crash observed in fable2's own runtime logs
// (native/out/build/win-amd64-release/logs/fable2_039..041.log) immediately
// after this module's hand-off ran.
//
// Fix: treat every one of these speculative reads as fallible. __try/__except
// catches the resulting SEH access violation locally (rex's VEH returns
// EXCEPTION_CONTINUE_SEARCH for an address outside any guest-physical heap --
// see xmemory.cpp line ~547 -- so a frame-based __except further up the same
// thread's call stack, like this one, still gets a chance to handle it) and
// reports a decode failure instead of crashing the whole process. This does
// not change, validate, or guess at the address formula -- it only makes an
// already-labeled guess safe to be wrong.
//
// Isolated in its own function, with only POD locals, so the __try block
// never has to reason about unwinding a C++ object with a destructor.
bool TryDecodeVertex(uint8_t* base, uint32_t idx_addr, bool index32, uint32_t vertex_addr,
                      uint32_t stride, float* out_x, float* out_y, float* out_z,
                      uint32_t* out_raw_index, bool* out_is_restart) {
  __try {
    const uint32_t vertex_index = LoadGuestIndex(base, idx_addr, index32);
    *out_raw_index = vertex_index;
    // Round 15 follow-up: the standard strip/fan-cut sentinel -- see
    // DecodedVertex::is_restart's comment for the evidence. Recognized
    // before any address math, so a restart marker never gets multiplied
    // by stride into a wild, far-off address in the first place.
    const uint32_t restart_sentinel = index32 ? 0xFFFFFFFFu : 0xFFFFu;
    if (vertex_index == restart_sentinel) {
      *out_x = 0.0f;
      *out_y = 0.0f;
      *out_z = 0.0f;
      *out_is_restart = true;
      return true;
    }
    *out_is_restart = false;
    const uint32_t v_addr = vertex_addr + vertex_index * stride;
    *out_x = LoadGuestFloat(base, v_addr + 0);
    *out_y = LoadGuestFloat(base, v_addr + 4);
    *out_z = LoadGuestFloat(base, v_addr + 8);
    return true;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    *out_x = 0.0f;
    *out_y = 0.0f;
    *out_z = 0.0f;
    *out_raw_index = 0;
    *out_is_restart = false;
    return false;
  }
}

// Best-effort, clearly-fallible sanity check: reject a decode that is all
// exactly zero (degenerate -- almost certainly means the address/stride
// guess is wrong rather than genuinely all-zero geometry) or that produced
// a non-finite float (definitely means the format guess is wrong). This
// does not "fix" anything; it only decides whether the overlay shows
// triangles or an honest error message.
// Round 15, seventh follow-up: fable2_063.log/fable2_064.log both showed
// DecodeLooksPlausible() reporting decode_ok=true for candidates that a
// human eye immediately rejects -- values like 1.7e16 or -1.9e19 are
// std::isfinite() (not inf/NaN) but obviously not a real Fable II world
// coordinate; they are the signature of reading an unrelated bit pattern
// (an int, a pointer, packed data) and reinterpreting it as a float. This
// was a known, explicitly documented limitation (see the comment above the
// old version of this function), not a new bug -- fixed now that the
// interactive re-decode slider produced enough side-by-side evidence
// (fable2_064.log: strides 11/18/22/47 all-zero-or-faulted, 16/32/87 each
// still contained at least one 1e10+ magnitude value, only 64 had none) to
// pick a bound with confidence: 1,000,000.0 is far above any coordinate
// this engine's world could plausibly need, and far below every garbage
// value observed in any log this entire investigation. This only changes
// which candidates the overlay/log calls "plausible" -- it does not affect
// TryDecodeVertex, restart-marker handling, or any address/stride logic.
constexpr float kMaxPlausibleCoordMagnitude = 1.0e6f;

bool DecodeLooksPlausible(const DecodedVertex* verts, int count) {
  bool any_nonzero = false;
  for (int i = 0; i < count; ++i) {
    // Round 15 follow-up: a restart-marker slot is deliberately x=y=z=0 --
    // see TryDecodeVertex/DecodedVertex::is_restart -- not a real decoded
    // vertex, so it should not count toward "all zero" (degenerate,
    // implausible) any more than it would toward "has real geometry".
    if (verts[i].is_restart) continue;
    if (!std::isfinite(verts[i].x) || !std::isfinite(verts[i].y) || !std::isfinite(verts[i].z)) {
      return false;
    }
    // Round 15, seventh follow-up: catches the "huge-but-finite" garbage
    // pattern isfinite() alone lets through (see comment above).
    if (std::fabs(verts[i].x) > kMaxPlausibleCoordMagnitude ||
        std::fabs(verts[i].y) > kMaxPlausibleCoordMagnitude ||
        std::fabs(verts[i].z) > kMaxPlausibleCoordMagnitude) {
      return false;
    }
    if (verts[i].x != 0.0f || verts[i].y != 0.0f || verts[i].z != 0.0f) any_nonzero = true;
  }
  return any_nonzero;
}

// Decodes one candidate vertex address end to end (SEH-safe via
// TryDecodeVertex for every read), independent of any other candidate.
// Mirrors the single-candidate logic OnValidatedDraw used before Phase 4,
// factored out so it can run twice -- once per candidate address -- without
// duplicating the loop/fault-handling/plausibility logic.
DecodeAttempt DecodeCandidate(uint8_t* base, uint32_t index_addr, bool index32,
                               uint32_t vertex_addr, uint32_t stride, int index_count) {
  DecodeAttempt result;
  result.attempted = true;
  result.count = std::clamp<int>(index_count, 0, kMaxDecodedIndices);

  for (int i = 0; i < result.count; ++i) {
    const uint32_t index_stride = index32 ? 4u : 2u;
    const uint32_t idx_addr = index_addr + static_cast<uint32_t>(i) * index_stride;
    if (!TryDecodeVertex(base, idx_addr, index32, vertex_addr, stride, &result.verts[i].x,
                          &result.verts[i].y, &result.verts[i].z, &result.verts[i].raw_index,
                          &result.verts[i].is_restart)) {
      result.read_fault = true;
      result.fault_at_index = i;
      result.count = 0;  // don't trust a partially-filled array
      break;
    }
  }

  if (result.read_fault) {
    result.error = "guest memory read out of range while decoding vertex " +
                    std::to_string(result.fault_at_index) +
                    " for this candidate address -- skipped safely instead of crashing";
  } else {
    result.plausible = result.count >= 3 && DecodeLooksPlausible(result.verts, result.count);
    if (!result.plausible) {
      result.error = result.count < 3
          ? "index_count < 3, nothing to draw"
          : "decoded vertices are all-zero or non-finite for this candidate address -- "
            "fable2_native_poc_vertex_stride / fable2_native_poc_swap_geometry_bytes is "
            "very likely wrong for this draw; try toggling them";
    }
  }
  return result;
}

// Round 15, eighth follow-up (native-rendering-phase3.md): rounds six and
// seven turned "try a stride guess" from a full rebuild+relaunch into a
// live slider + button -- still one manual round trip (drag, click, look,
// screenshot, repeat) per guess, and fable2_064.log shows the user needed
// eight of those round trips to find stride=64. This sweeps a whole range
// of candidate strides against the SAME already-captured draw in a single
// call, using the exact same DecodeCandidate()/DecodeLooksPlausible() the
// manual path already uses (including this round's new magnitude sanity
// bound), and reports every stride that passes -- turning "try 16, then
// 64, then 32, screenshot each" into "read one sweep result." Not a new
// decode algorithm and no new claim about correctness: DecodeLooksPlausible
// is exactly as fallible here as it is for a single manual guess, this
// just runs it across many candidates automatically instead of one at a
// time by hand.
constexpr uint32_t kStrideSweepMin = 4;
constexpr uint32_t kStrideSweepMax = 128;
constexpr uint32_t kStrideSweepStep = 4;
constexpr int kMaxSweepHits = 34;  // (128-4)/4 + 1 = 32 candidates tried; generous headroom

struct StrideSweepResult {
  bool attempted = false;
  int candidates_tried = 0;
  uint32_t plausible_strides[kMaxSweepHits] = {};
  int plausible_count = 0;
};

StrideSweepResult SweepStrides(uint8_t* base, uint32_t index_addr, bool index32,
                                uint32_t vertex_addr, int index_count) {
  StrideSweepResult result;
  result.attempted = true;
  for (uint32_t stride = kStrideSweepMin; stride <= kStrideSweepMax; stride += kStrideSweepStep) {
    ++result.candidates_tried;
    DecodeAttempt attempt =
        DecodeCandidate(base, index_addr, index32, vertex_addr, stride, index_count);
    if (attempt.plausible && result.plausible_count < kMaxSweepHits) {
      result.plausible_strides[result.plausible_count++] = stride;
    }
  }
  return result;
}

StrideSweepResult g_sweep_raw;      // sweep against candidate_addr_raw, guarded by g_draw_mutex
StrideSweepResult g_sweep_formula;  // sweep against candidate_addr_formula, guarded by g_draw_mutex

// Round 7 (MAXIMUM FORWARD PROGRESS pass, native-rendering-phase3.md):
// converts one already-decoded, already-validated candidate (DecodeAttempt,
// Fable/Xenos-specific) into the backend-neutral NativeDrawCommand
// contract (native_draw_repr.h). This is the ONLY place those two types
// meet -- everything below this function is decode-agnostic, and
// everything above it (DecodeCandidate, TryDecodeVertex, LoadGuestFloat...)
// stays entirely unaware NativeDrawCommand exists.
fable2::native_render::NativeDrawCommand BuildDrawCommand(const char* label, uint32_t addr,
                                                           const DecodeAttempt& decode) {
  fable2::native_render::NativeDrawCommand cmd;
  cmd.valid = decode.plausible;
  cmd.debug_label = label;
  cmd.debug_source_addr = addr;
  if (!decode.plausible) return cmd;

  cmd.topology = fable2::native_render::PrimitiveTopology::kTriangleList;
  // Round 15 follow-up: skip restart-marker slots (DecodedVertex::
  // is_restart) rather than feeding their degenerate (0,0,0) placeholder
  // into the vertex list as if it were real geometry -- this renderer
  // already treats every 3 consecutive vertices as one triangle
  // (kTriangleList) regardless of the draw's actual prim_type, a
  // pre-existing simplification this does not change; it only keeps a
  // restart marker's placeholder zero out of that triangle grouping.
  const int max_out = std::min<int>(decode.count, fable2::native_render::kMaxDrawVertices);
  int out_count = 0;
  for (int i = 0; i < max_out; ++i) {
    if (decode.verts[i].is_restart) continue;
    cmd.vertices[out_count].x = decode.verts[i].x;
    cmd.vertices[out_count].y = decode.verts[i].y;
    cmd.vertices[out_count].z = decode.verts[i].z;
    ++out_count;
  }
  cmd.vertex_count = out_count;
  return cmd;
}

// Round 16, twenty-ninth follow-up: decodes ONE fetch-constant-validated
// vertex record's attributes into up to 4 floats each, using ReX's own
// reported per-attribute format/offset/sign/integer metadata --
// deliberately not a global stride/format guess (message 1's explicit
// instruction). `words` is the record's OWN dwords (already sliced to the
// attribute's own record, big-endian-decoded the same way this file's own
// LoadGuestFloat/REX_LOAD_U32 convention does elsewhere), `words_count` is
// how many of them are actually present (a snapshot can end mid-record).
//
// Formats implemented (xenos.h's VertexFormat): k_16_16_16_16_FLOAT (32,
// IEEE-754 half x4), k_32_32_32_32_FLOAT (38, IEEE-754 float x4),
// k_8_8_8_8 (6, UBYTE4 raw or UNORM/SNORM per is_integer/is_signed) -- the
// three every real capture through round 34 had reported -- plus, as of
// round 16's thirty-seventh follow-up, k_16_16_FLOAT (31), k_2_10_10_10
// (7), k_10_11_11 (16) and k_11_11_10 (17): round 35's 0xecd66a10092e6562
// shader reported 31 and 16 on its attr1/attr2 (not yet consumed by
// BuildDrawCommandFromValidatedFetchConstant, which only reads attribute[0]
// -- added anyway so any future capture that puts one of these on
// attribute[0] decodes correctly instead of falling through to the raw-
// float32 default). Bit widths/offsets for the three packed-integer
// formats are copied verbatim from xenia's own DXBC vertex fetch
// translator (tools/rexglue-source/src/graphics/pipeline/shader/
// dxbc_translator_fetch.cpp, the packed_widths/packed_offsets tables), not
// guessed. An unrecognized format is decoded as raw float32 and flagged in
// the log once (round 28's own finding: even a "known" format here has not
// yet produced values this investigation could call definitively coherent
// for every shader tried) -- this is a documented fallback, not an
// assertion that it is correct.
float HalfFloatBitsToFloat32(uint16_t h) {
  const uint32_t sign = static_cast<uint32_t>(h >> 15) & 0x1u;
  const uint32_t exp = static_cast<uint32_t>(h >> 10) & 0x1Fu;
  const uint32_t mant = static_cast<uint32_t>(h) & 0x3FFu;
  uint32_t bits;
  if (exp == 0) {
    if (mant == 0) {
      bits = sign << 31;
    } else {
      // Subnormal half -> normalized float32.
      uint32_t e = 0;
      uint32_t m = mant;
      while ((m & 0x400u) == 0) {
        m <<= 1;
        ++e;
      }
      m &= 0x3FFu;
      const uint32_t exp32 = 127 - 15 - e;
      bits = (sign << 31) | (exp32 << 23) | (m << 13);
    }
  } else if (exp == 0x1Fu) {
    bits = (sign << 31) | (0xFFu << 23) | (mant << 13);  // Inf/NaN
  } else {
    const uint32_t exp32 = exp - 15 + 127;
    bits = (sign << 31) | (exp32 << 23) | (mant << 13);
  }
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

// Round 16, thirty-seventh follow-up: shared bitfield extract/normalize for
// the packed-integer vertex formats below (k_2_10_10_10, k_10_11_11,
// k_11_11_10). `offset`/`width` are copied verbatim from xenia's own DXBC
// vertex fetch translator (see the comment above). Normalization follows
// the same convention this file already uses for k_8_8_8_8 (kZeroClampMinusOne):
// signed -> divide by (2^(width-1))-1, clamp to >= -1; unsigned -> divide
// by (2^width)-1. is_integer skips normalization entirely, same as every
// other format above.
float ExtractPackedField(uint32_t word, uint32_t offset, uint32_t width, bool is_signed,
                          bool is_integer) {
  const uint32_t mask = (width >= 32u) ? 0xFFFFFFFFu : ((1u << width) - 1u);
  const uint32_t raw = (word >> offset) & mask;
  if (is_signed) {
    const uint32_t sign_bit = 1u << (width - 1u);
    const int32_t signed_val = static_cast<int32_t>((raw ^ sign_bit) - sign_bit);
    if (is_integer || width <= 1u) return static_cast<float>(signed_val);
    const float scale = 1.0f / static_cast<float>((1u << (width - 1u)) - 1u);
    return std::max(-1.0f, static_cast<float>(signed_val) * scale);
  }
  if (is_integer) return static_cast<float>(raw);
  return static_cast<float>(raw) / static_cast<float>(mask);
}

void DecodeValidatedAttributeComponents(const fable2::native_poc::ValidatedFetchConstantAttribute& a,
                                         const uint32_t* words, int words_count, float out[4]) {
  out[0] = out[1] = out[2] = out[3] = 0.0f;
  switch (a.data_format) {
    case 32u: {  // k_16_16_16_16_FLOAT: two dwords, four half-floats.
      if (words_count < 2) return;
      const uint16_t h0 = static_cast<uint16_t>(words[0] >> 16);
      const uint16_t h1 = static_cast<uint16_t>(words[0] & 0xFFFFu);
      const uint16_t h2 = static_cast<uint16_t>(words[1] >> 16);
      const uint16_t h3 = static_cast<uint16_t>(words[1] & 0xFFFFu);
      out[0] = HalfFloatBitsToFloat32(h0);
      out[1] = HalfFloatBitsToFloat32(h1);
      out[2] = HalfFloatBitsToFloat32(h2);
      out[3] = HalfFloatBitsToFloat32(h3);
      return;
    }
    case 38u: {  // k_32_32_32_32_FLOAT: four dwords, four IEEE-754 floats.
      for (int i = 0; i < 4 && i < words_count; ++i) {
        std::memcpy(&out[i], &words[i], sizeof(float));
      }
      return;
    }
    case 6u: {  // k_8_8_8_8: one dword, four bytes.
      if (words_count < 1) return;
      const uint32_t w = words[0];
      for (int i = 0; i < 4; ++i) {
        const uint32_t byte = (w >> (24 - 8 * i)) & 0xFFu;
        if (a.is_integer) {
          out[i] = a.is_signed ? static_cast<float>(static_cast<int8_t>(byte))
                                : static_cast<float>(byte);
        } else {
          out[i] = a.is_signed ? std::max(-1.0f, static_cast<float>(static_cast<int8_t>(byte)) / 127.0f)
                                : static_cast<float>(byte) / 255.0f;
        }
      }
      return;
    }
    case 31u: {  // k_16_16_FLOAT: one dword, two half-floats -- same byte
                 // layout as the low/high half of k_16_16_16_16_FLOAT's
                 // first dword above (xenia's translator shares one switch
                 // case for both). Round 35's 0xecd66a10092e6562 shader's
                 // attr2 reported this format.
      if (words_count < 1) return;
      const uint16_t h0 = static_cast<uint16_t>(words[0] >> 16);
      const uint16_t h1 = static_cast<uint16_t>(words[0] & 0xFFFFu);
      out[0] = HalfFloatBitsToFloat32(h0);
      out[1] = HalfFloatBitsToFloat32(h1);
      return;
    }
    case 7u: {  // k_2_10_10_10: one dword, widths {10,10,10,2} at offsets
                // {0,10,20,30} -- copied from xenia's translator, not
                // guessed. Round 35's 0xecd66a10092e6562 shader's attr3
                // reported this format.
      if (words_count < 1) return;
      const uint32_t w = words[0];
      out[0] = ExtractPackedField(w, 0, 10, a.is_signed, a.is_integer);
      out[1] = ExtractPackedField(w, 10, 10, a.is_signed, a.is_integer);
      out[2] = ExtractPackedField(w, 20, 10, a.is_signed, a.is_integer);
      out[3] = ExtractPackedField(w, 30, 2, a.is_signed, a.is_integer);
      return;
    }
    case 16u: {  // k_10_11_11: despite the name, xenia's translator packs
                 // this as widths {11,11,10} at offsets {0,11,22}. Round
                 // 35's 0xecd66a10092e6562 shader's attr1 reported this
                 // format.
      if (words_count < 1) return;
      const uint32_t w = words[0];
      out[0] = ExtractPackedField(w, 0, 11, a.is_signed, a.is_integer);
      out[1] = ExtractPackedField(w, 11, 11, a.is_signed, a.is_integer);
      out[2] = ExtractPackedField(w, 22, 10, a.is_signed, a.is_integer);
      return;
    }
    case 17u: {  // k_11_11_10: widths {10,11,11} at offsets {0,10,21}. Not
                 // yet seen in a real capture; added alongside 7/16/31
                 // since all four share one ucode.h/translator table.
      if (words_count < 1) return;
      const uint32_t w = words[0];
      out[0] = ExtractPackedField(w, 0, 10, a.is_signed, a.is_integer);
      out[1] = ExtractPackedField(w, 10, 11, a.is_signed, a.is_integer);
      out[2] = ExtractPackedField(w, 21, 11, a.is_signed, a.is_integer);
      return;
    }
    default: {
      // Not one of the seven formats above -- decode as raw float32 (same
      // as case 38) so the value is at least visible/loggable, but do not
      // claim this is correct.
      for (int i = 0; i < 4 && i < words_count; ++i) {
        std::memcpy(&out[i], &words[i], sizeof(float));
      }
      return;
    }
  }
}

// Builds a NativeDrawCommand directly from a validated fetch-constant
// vertex stream (info.snapshot), with NO index buffer and NO known
// primitive topology of its own (this buffer's real per-draw vertex count/
// winding has never been captured -- only its per-record byte layout has).
// Decodes as many full stride_words-sized records as fit in the captured
// snapshot (capped at kMaxDrawVertices), and for each record takes
// attribute[0]'s first two used components as NativeVertex.x/y (z is left
// 0 unless attribute[0] itself reports z as used) purely so there is
// something to plot on the existing debug canvas -- see native_draw_repr.h
// and ImGuiDrawListBackend's own comments: there is still no captured
// view/projection transform anywhere in this stack, so this is exactly as
// much "this is a real position" as the existing candidate-A/B path
// already claims (i.e. none -- it is guest bytes in guest units, however
// this specific vertex format actually encodes them). Round 28's own
// coherence writeup (native-rendering-phase3.md) is the honest caveat on
// what this actually looks like for the shaders captured so far.
// Round 20 (Fable2PC, native 3D draw path): scans a capture's raw float
// constants (ValidatedFetchConstantVertexInfo::float_constant_* -- see
// that struct's own comment) for three CONSECUTIVE logical registers
// whose xyz components form an orthonormal set: each row's xyz unit
// length, and every pair of rows mutually perpendicular, within a
// generous tolerance. That is the unmistakable structural signature of a
// rigid transform (rotation + translation, no scale) packed as 3 vec4
// rows -- row i = (m_i0, m_i1, m_i2, translation_i) -- the standard
// compact way an object/world matrix is passed as vertex shader
// constants. This is a STRUCTURAL test on the raw numbers, not a name or
// offset guess: it makes no claim about what the engine calls this
// register range, only that the numbers themselves are mathematically a
// valid rigid transform. Motivated by a real capture
// (native-rendering-phase3.md round 20): logical registers 4/5/6 held a
// ~2.25-degree Z rotation plus translation (134.509, 176.728, 12.4346),
// with that exact translation independently reproduced in a companion
// vec4 at register 7 -- too clean to be coincidence, but still reported
// as a structural finding, not a confirmed "this is the world matrix"
// claim (no Fable/Xenos shader source has been read to confirm it).
bool FindRigidTransformCandidate(const fable2::native_poc::ValidatedFetchConstantVertexInfo& info,
                                  float out_rows[3][4]) {
  auto row_at = [&](uint32_t logical, float out[4]) -> bool {
    for (int i = 0; i < info.float_constant_count; ++i) {
      if (info.float_constant_logical_index[i] == logical) {
        out[0] = info.float_constant_value[i][0];
        out[1] = info.float_constant_value[i][1];
        out[2] = info.float_constant_value[i][2];
        out[3] = info.float_constant_value[i][3];
        return true;
      }
    }
    return false;
  };
  for (int i = 0; i + 2 < info.float_constant_count; ++i) {
    const uint32_t base = info.float_constant_logical_index[i];
    float r0[4], r1[4], r2[4];
    if (!row_at(base, r0) || !row_at(base + 1, r1) || !row_at(base + 2, r2)) continue;
    const float len0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
    const float len1 = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    const float len2 = std::sqrt(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
    constexpr float kLenTol = 0.05f;
    if (std::fabs(len0 - 1.0f) > kLenTol || std::fabs(len1 - 1.0f) > kLenTol ||
        std::fabs(len2 - 1.0f) > kLenTol) {
      continue;
    }
    const float dot01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
    const float dot02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
    const float dot12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    constexpr float kDotTol = 0.05f;
    if (std::fabs(dot01) > kDotTol || std::fabs(dot02) > kDotTol || std::fabs(dot12) > kDotTol) {
      continue;
    }
    std::memcpy(out_rows[0], r0, sizeof(r0));
    std::memcpy(out_rows[1], r1, sizeof(r1));
    std::memcpy(out_rows[2], r2, sizeof(r2));
    return true;
  }
  return false;
}

// Round 20, eleventh follow-up (view/projection reconstruction --
// native-rendering-phase3.md): one structurally-valid rigid transform
// (FindRigidTransformCandidate, above) is already applied as this draw's
// "world transform" -- but that function returns only the FIRST match it
// finds and stops there. A view matrix is ALSO, mathematically, a rigid
// transform (camera rotation + translation, no scale) -- there is no
// separate structural test that distinguishes "world" from "view"; only
// which register range it lives in and what its translation looks like
// gives a clue. So: same core test as FindRigidTransformCandidate
// (3 consecutive logical registers, each unit-length xyz and mutually
// perpendicular), but collects EVERY non-overlapping match instead of
// stopping at the first, so a second rigid block distinct from the
// already-known world transform can actually be seen if one exists.
// Diagnostic only -- does not change what gets applied to rendering.
struct RigidTransformCandidate {
  uint32_t base_logical_index = 0;
  float rows[3][4] = {};
};
constexpr int kMaxRigidTransformCandidates = 8;

int FindAllRigidTransformCandidates(const fable2::native_poc::ValidatedFetchConstantVertexInfo& info,
                                     RigidTransformCandidate out[], int max_out) {
  auto row_at = [&](uint32_t logical, float out_row[4]) -> bool {
    for (int i = 0; i < info.float_constant_count; ++i) {
      if (info.float_constant_logical_index[i] == logical) {
        out_row[0] = info.float_constant_value[i][0];
        out_row[1] = info.float_constant_value[i][1];
        out_row[2] = info.float_constant_value[i][2];
        out_row[3] = info.float_constant_value[i][3];
        return true;
      }
    }
    return false;
  };
  int found = 0;
  uint32_t last_consumed_end = 0;  // logical index one past the last candidate's base+2
  bool have_last = false;
  for (int i = 0; i + 2 < info.float_constant_count && found < max_out; ++i) {
    const uint32_t base = info.float_constant_logical_index[i];
    // Skip bases that would overlap a candidate already found, so one
    // real 3-row block is never double-counted as multiple overlapping
    // "candidates" starting at consecutive registers within it.
    if (have_last && base < last_consumed_end) continue;
    float r0[4], r1[4], r2[4];
    if (!row_at(base, r0) || !row_at(base + 1, r1) || !row_at(base + 2, r2)) continue;
    const float len0 = std::sqrt(r0[0] * r0[0] + r0[1] * r0[1] + r0[2] * r0[2]);
    const float len1 = std::sqrt(r1[0] * r1[0] + r1[1] * r1[1] + r1[2] * r1[2]);
    const float len2 = std::sqrt(r2[0] * r2[0] + r2[1] * r2[1] + r2[2] * r2[2]);
    constexpr float kLenTol = 0.05f;
    if (std::fabs(len0 - 1.0f) > kLenTol || std::fabs(len1 - 1.0f) > kLenTol ||
        std::fabs(len2 - 1.0f) > kLenTol) {
      continue;
    }
    const float dot01 = r0[0] * r1[0] + r0[1] * r1[1] + r0[2] * r1[2];
    const float dot02 = r0[0] * r2[0] + r0[1] * r2[1] + r0[2] * r2[2];
    const float dot12 = r1[0] * r2[0] + r1[1] * r2[1] + r1[2] * r2[2];
    constexpr float kDotTol = 0.05f;
    if (std::fabs(dot01) > kDotTol || std::fabs(dot02) > kDotTol || std::fabs(dot12) > kDotTol) {
      continue;
    }
    RigidTransformCandidate& c = out[found++];
    c.base_logical_index = base;
    std::memcpy(c.rows[0], r0, sizeof(r0));
    std::memcpy(c.rows[1], r1, sizeof(r1));
    std::memcpy(c.rows[2], r2, sizeof(r2));
    last_consumed_end = base + 3;
    have_last = true;
  }
  return found;
}

// Round 20, eleventh follow-up (view/projection reconstruction): a
// structural search for a 4-consecutive-register block shaped like a
// standard D3D-style projection matrix, expressed in the SAME per-
// register "one column of the mathematical matrix" convention this
// file's own world-transform application already uses and relies on
// (out[i] = v . row_i -- see FindRigidTransformCandidate's own comment
// and its caller in BuildDrawCommandFromValidatedFetchConstant, which
// together establish this is the correct convention for this codebase,
// not an assumption made fresh here). Under that convention, matching
// the universal "4 dp4 instructions against c0..c3" pattern D3D9/Xbox 360
// vertex shaders use to transform a position by a matrix, a standard
// PERSPECTIVE projection's 4 stored registers have this shape:
//   row0 (produces out.x): (xScale,  ~0,      ~0,   ~0)
//   row1 (produces out.y): (~0,      yScale,  ~0,   ~0)
//   row2 (produces out.z): (~0,      ~0,      A,    B)      -- A, B free (near/far-derived)
//   row3 (produces out.w): (~0,      ~0,      +/-1, ~0)
// row3's shape is the distinctive, convention-agnostic signature: three
// near-zero components and exactly one +/-1 in the z slot, which is what
// produces "w = +/-z" for the hardware perspective divide -- this holds
// for left- or right-handed and reversed-depth variants alike, since all
// of them still need SOME form of this row to get a perspective divide
// at all. Also tested, separately: an ORTHOGRAPHIC projection's row3 is
// instead (~0, ~0, ~0, 1) -- no perspective divide, w is always 1 --
// structurally distinct and tagged as such if found, not conflated with
// the perspective case. row0/row1 being near-diagonal (pure x-scale/
// y-scale, no cross terms) is the corroborating check for both kinds.
// Purely structural, like FindRigidTransformCandidate -- makes no claim
// about which register range the engine calls "projection", only that
// the raw numbers found there are mathematically shaped like one.
// NOT wired into any rendering path -- logged only. Applying an
// unconfirmed candidate to the now-working native draw path (round 20,
// tenth follow-up) risks visibly breaking a confirmed-good result for a
// guess with no ground truth to validate it against yet.
struct ProjectionCandidate {
  uint32_t base_logical_index = 0;
  float rows[4][4] = {};
  bool is_orthographic = false;  // false = perspective (w=+/-z), true = orthographic (w=1)
  // Round 20, thirteenth follow-up (view/projection reconstruction):
  // false when row0/row1 are ALSO clean diagonal x-scale/y-scale (the
  // "textbook" standalone projection matrix shape) -- true when only the
  // row3 signature matched but row0/row1 have real cross-terms. See this
  // function's own comment for why row0/row1 are no longer a hard
  // requirement: a game that pre-combines World*View*Projection into one
  // matrix on the CPU (a common D3D9-era optimization, to save vertex
  // shader instructions) would still very plausibly preserve row3's
  // simple "w = +/-z" shape (it is largely a property of the projection
  // component alone), while row0/row1 would legitimately be full of
  // rotation/translation cross-terms from the world and view components
  // folded in -- exactly what the ORIGINAL, stricter version of this
  // function would have silently rejected as "not diagonal enough",
  // never even logging it as a candidate. This flag exists so a
  // row3-only match (more likely, but less certain to actually be a
  // clean/separate projection) is never confused with a full textbook
  // match in the log.
  bool row0_row1_diagonal = false;
};
constexpr int kMaxProjectionCandidates = 8;

int FindProjectionCandidates(const fable2::native_poc::ValidatedFetchConstantVertexInfo& info,
                              ProjectionCandidate out[], int max_out) {
  auto row_at = [&](uint32_t logical, float out_row[4]) -> bool {
    for (int i = 0; i < info.float_constant_count; ++i) {
      if (info.float_constant_logical_index[i] == logical) {
        out_row[0] = info.float_constant_value[i][0];
        out_row[1] = info.float_constant_value[i][1];
        out_row[2] = info.float_constant_value[i][2];
        out_row[3] = info.float_constant_value[i][3];
        return true;
      }
    }
    return false;
  };
  constexpr float kZeroTol = 0.02f;
  constexpr float kOneTol = 0.05f;
  constexpr float kMinScale = 0.05f;  // reject a trivially-all-zero "diagonal" as a false match
  int found = 0;
  for (int i = 0; i + 3 < info.float_constant_count && found < max_out; ++i) {
    const uint32_t base = info.float_constant_logical_index[i];
    float row0[4], row1[4], row2[4], row3[4];
    if (!row_at(base, row0) || !row_at(base + 1, row1) || !row_at(base + 2, row2) ||
        !row_at(base + 3, row3)) {
      continue;
    }
    // Round 20, thirteenth follow-up: row3's own shape is now the ONLY
    // hard gate -- see ProjectionCandidate::row0_row1_diagonal's own
    // comment for why row0/row1 being clean diagonal is no longer
    // required to even log a candidate (it was silently discarding any
    // pre-combined World*View*Projection matrix, which is a real,
    // plausible shape for this engine to use, not just a stricter/safer
    // default). row0/row1/row2's diagonal-ness is still computed and
    // reported, just as an informative tag rather than a filter.
    const bool row3_perspective = std::fabs(row3[0]) < kZeroTol && std::fabs(row3[1]) < kZeroTol &&
                                   std::fabs(row3[3]) < kZeroTol &&
                                   std::fabs(std::fabs(row3[2]) - 1.0f) < kOneTol;
    const bool row3_orthographic = std::fabs(row3[0]) < kZeroTol && std::fabs(row3[1]) < kZeroTol &&
                                    std::fabs(row3[2]) < kZeroTol &&
                                    std::fabs(row3[3] - 1.0f) < kOneTol;
    if (!row3_perspective && !row3_orthographic) continue;
    // Round 20, fifteenth follow-up (view/projection reconstruction --
    // native-rendering-phase3.md): real false positives from the
    // thirteenth follow-up's own relaxation, caught by direct evidence
    // (fable2_118.2.log): several "candidates" turned up whose row0/row1/
    // row2 were EITHER all entirely (0,0,0,0), or had one row exactly
    // (0,0,0,0) sandwiched between two rows that individually looked like
    // real unit vectors (e.g. c[74]/c[76]: two DIFFERENT unit-length
    // vec4s with plausible plane-equation shapes -- unit normal + a
    // distance-like w -- not two rows of the SAME matrix at all). The
    // sliding 4-register window this function uses has no way to know
    // where one semantic group of constants ends and an unrelated one
    // begins -- engines commonly pack arbitrary, unrelated vec4 constants
    // at consecutive logical indices, and this function was, after the
    // thirteenth follow-up's relaxation, willing to call ANY 4 consecutive
    // registers a "candidate" as long as row3 alone happened to match,
    // even when row0/row1/row2 were mostly or entirely empty padding/
    // unrelated data. No real transform matrix -- combined or not --
    // has an output row that is ALWAYS exactly zero (that would mean one
    // whole output component never varies, for every vertex, which is not
    // how a functioning world/view/projection composition works). So:
    // require row0, row1, AND row2 to EACH individually have at least one
    // component above kMinScale, restoring a real content requirement
    // without reintroducing the thirteenth follow-up's overly strict
    // "must be diagonal" requirement -- a pre-combined WVP matrix's rows
    // would still easily clear this bar (real rotation/translation
    // content in every row), while a mostly-empty register block or an
    // unrelated single vec4 sitting next to real data will not.
    const auto row_has_content = [&](const float row[4]) {
      return std::fabs(row[0]) > kMinScale || std::fabs(row[1]) > kMinScale ||
             std::fabs(row[2]) > kMinScale || std::fabs(row[3]) > kMinScale;
    };
    if (!row_has_content(row0) || !row_has_content(row1) || !row_has_content(row2)) continue;
    const bool row0_diag = std::fabs(row0[1]) < kZeroTol && std::fabs(row0[2]) < kZeroTol &&
                            std::fabs(row0[0]) > kMinScale;
    const bool row1_diag = std::fabs(row1[0]) < kZeroTol && std::fabs(row1[2]) < kZeroTol &&
                            std::fabs(row1[1]) > kMinScale;
    const bool row2_no_xy = std::fabs(row2[0]) < kZeroTol && std::fabs(row2[1]) < kZeroTol;
    ProjectionCandidate& c = out[found++];
    c.base_logical_index = base;
    c.row0_row1_diagonal = row0_diag && row1_diag && row2_no_xy;
    std::memcpy(c.rows[0], row0, sizeof(row0));
    std::memcpy(c.rows[1], row1, sizeof(row1));
    std::memcpy(c.rows[2], row2, sizeof(row2));
    std::memcpy(c.rows[3], row3, sizeof(row3));
    c.is_orthographic = row3_orthographic;
  }
  return found;
}

// Round 20, twenty-fifth follow-up (view/projection reconstruction --
// native-rendering-phase3.md): fable2_126.3.log turned up a THIRD,
// previously-unseen shader (0xa5846836c90e1192) carrying the EXACT same
// c[72] row2/row3 values as the two hashes in kLeadingCandidateHashes
// (row2=(0,0,1.00002,-0.100002) row3=(0,0,1,0) -- the near~0.1/far~5000
// signature hand-solved in the seventeenth follow-up), with only row0/
// row1's scale differing slightly (this session: 0.95783/1.70281 on
// 0x129f7f2793da9084 itself, vs the long-standing 0.941176/1.73763 --
// consistent with a per-viewport aspect/FOV change, not a different
// matrix). A fixed hash list can never anticipate a new shader showing
// up with these same constants -- it would need another manual edit
// every time, which is exactly what just happened. Detect the signature
// by VALUE instead, so any future shader carrying it is recognized
// automatically: this is what makes the near/far shape distinctive
// (row2/row3, not row0/row1, since those are expected to vary with
// aspect/FOV) -- a coincidental match on these specific constants is
// very unlikely for an unrelated matrix.
bool MatchesLeadingProjectionSignature(const ProjectionCandidate& c) {
  constexpr float kTol = 0.01f;
  return !c.is_orthographic &&
         std::fabs(c.rows[2][0]) < kTol && std::fabs(c.rows[2][1]) < kTol &&
         std::fabs(c.rows[2][2] - 1.00002f) < kTol &&
         std::fabs(c.rows[2][3] - (-0.100002f)) < kTol &&
         std::fabs(c.rows[3][0]) < kTol && std::fabs(c.rows[3][1]) < kTol &&
         std::fabs(c.rows[3][2] - 1.0f) < kTol && std::fabs(c.rows[3][3]) < kTol;
}

fable2::native_render::NativeDrawCommand BuildDrawCommandFromValidatedFetchConstant(
    const fable2::native_poc::ValidatedFetchConstantVertexInfo& info) {
  fable2::native_render::NativeDrawCommand cmd;
  cmd.debug_label = "validated-fetch-constant(slot31,constC)";
  cmd.debug_source_addr = info.address;
  if (!info.valid || info.stride_words == 0 || info.attribute_count <= 0) return cmd;

  const uint32_t stride_bytes = info.stride_words * 4u;
  const uint32_t record_count_available = info.snapshot_bytes_captured / stride_bytes;
  const int record_count =
      std::min<int>(static_cast<int>(record_count_available), fable2::native_render::kMaxDrawVertices);
  if (record_count <= 0) return cmd;

  const fable2::native_poc::ValidatedFetchConstantAttribute& a0 = info.attributes[0];
  // Every real capture this investigation has ever reported has
  // offset_dwords >= 0; guard the theoretical negative case explicitly
  // (ucode.h documents the field's full range as [-4194304, 4194303])
  // rather than let it wrap when added into the unsigned record_dword0
  // math below.
  if (a0.offset_dwords < 0) return cmd;
  // Reinterpret the snapshot as big-endian dwords once, matching this
  // file's own LoadGuestFloat/REX_LOAD_U32 byte-swap convention for every
  // other guest-memory read in this module.
  std::vector<uint32_t> words(info.snapshot_bytes_captured / 4);
  for (size_t i = 0; i < words.size(); ++i) {
    const uint8_t* p = info.snapshot + i * 4;
    words[i] = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
               (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
  }

  auto decode_record = [&](int r, fable2::native_render::NativeVertex& out) {
    const uint32_t record_dword0 = static_cast<uint32_t>(r) * info.stride_words;
    const uint32_t attr_dword0 = record_dword0 + static_cast<uint32_t>(a0.offset_dwords);
    const int available = static_cast<int>(words.size()) - static_cast<int>(attr_dword0);
    float comps[4];
    DecodeValidatedAttributeComponents(
        a0, available > 0 ? &words[attr_dword0] : nullptr, std::max(0, available), comps);
    out.x = comps[0];
    out.y = comps[1];
    out.z = (a0.used_result_components & 0x4u) ? comps[2] : 0.0f;
  };

  // Round 20, fourth-build follow-up (native-rendering-phase3.md): use the
  // real captured index buffer, when available, to SELECT which decoded
  // record each output vertex comes from (and in what order), instead of
  // assuming the snapshot's records are each used exactly once, in buffer
  // order. This is the fix for the fable2_110.log outlier vertices -- see
  // ValidatedFetchConstantVertexInfo::index_data_valid's own comment
  // (fable_native_poc.h) for the full reasoning. Falls through to the
  // exact prior sequential behavior whenever index data isn't
  // available/valid for this draw (cmd.valid stays false after this
  // block), so every earlier capture's behavior is unchanged.
  if (info.index_data_valid && info.index_snapshot_bytes_captured > 0) {
    const uint32_t index_stride = info.index_is_32bit ? 4u : 2u;
    const uint32_t indices_available = info.index_snapshot_bytes_captured / index_stride;
    const uint32_t indices_to_use = std::min<uint32_t>(
        indices_available,
        std::min<uint32_t>(info.index_count,
                            static_cast<uint32_t>(fable2::native_render::kMaxDrawVertices)));
    int out_count = 0;
    for (uint32_t k = 0; k < indices_to_use && out_count < fable2::native_render::kMaxDrawVertices;
         ++k) {
      uint32_t idx;
      if (info.index_is_32bit) {
        const uint8_t* p = info.index_snapshot + k * 4;
        idx = (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) |
              (static_cast<uint32_t>(p[2]) << 8) | static_cast<uint32_t>(p[3]);
      } else {
        const uint8_t* p = info.index_snapshot + k * 2;
        idx = (static_cast<uint32_t>(p[0]) << 8) | static_cast<uint32_t>(p[1]);
      }
      // A real index pointing past what this capture's fixed-size vertex
      // snapshot actually covers means the real vertex buffer extends
      // beyond kValidatedFetchConstantSnapshotBytes -- not a decode bug.
      // Skip it rather than guess/clamp into unrelated decoded bytes.
      if (idx >= record_count_available) continue;
      decode_record(static_cast<int>(idx), cmd.vertices[out_count]);
      ++out_count;
    }
    if (out_count >= 3) {
      cmd.vertex_count = out_count;
      cmd.topology = fable2::native_render::PrimitiveTopology::kTriangleList;
      cmd.valid = true;
      cmd.used_real_indices = true;
    }
  }

  if (!cmd.valid) {
    // Fallback: exact prior behavior -- blind sequential decode, each
    // record used exactly once in buffer order.
    for (int r = 0; r < record_count; ++r) {
      decode_record(r, cmd.vertices[r]);
    }
    cmd.vertex_count = record_count;
    cmd.topology = record_count >= 3 ? fable2::native_render::PrimitiveTopology::kTriangleList
                                      : fable2::native_render::PrimitiveTopology::kUnknown;
    cmd.valid = true;
  }

  // Round 20 (Fable2PC, native 3D draw path): if this capture's float
  // constants contain a structurally-validated rigid transform (see
  // FindRigidTransformCandidate's own comment), apply it to every decoded
  // vertex in place -- row_i . (x,y,z,1), the standard 3-row compact
  // affine convention that matches this candidate's own row layout
  // exactly. Leaves vertices in local/guest space, unmarked, if no
  // candidate is found -- same as every draw before this round.
  float rows[3][4];
  if (FindRigidTransformCandidate(info, rows)) {
    // Round 20, fourth-build follow-up: iterate cmd.vertex_count, NOT
    // record_count -- the two are no longer always equal now that the
    // real-indices path above can populate a different number of
    // vertices (out_count) than record_count (the raw decodable-record
    // count used only by the sequential fallback). Using record_count
    // here after the real-indices path ran would silently skip the
    // transform on some populated vertices whenever out_count exceeded
    // record_count (a real index selecting a record beyond
    // kMaxDrawVertices-capped record_count, which decode_record can still
    // decode safely -- see its own bounds handling).
    for (int r = 0; r < cmd.vertex_count; ++r) {
      const fable2::native_render::NativeVertex local = cmd.vertices[r];
      cmd.vertices[r].x =
          local.x * rows[0][0] + local.y * rows[0][1] + local.z * rows[0][2] + rows[0][3];
      cmd.vertices[r].y =
          local.x * rows[1][0] + local.y * rows[1][1] + local.z * rows[1][2] + rows[1][3];
      cmd.vertices[r].z =
          local.x * rows[2][0] + local.y * rows[2][1] + local.z * rows[2][2] + rows[2][3];
    }
    cmd.world_transform_applied = true;
  }

  return cmd;
}

// Round 7: the first (and, for now, only) INativeRenderBackend
// implementation -- draws a NativeDrawCommand into the current ImGui
// window via ImGui's own already-compiled solid-fill draw-list shader.
// This is exactly the bounding-box-fit-to-canvas + AddTriangleFilled logic
// the single-candidate Phase 3 version had inline in DrawCandidateSection,
// moved here UNCHANGED so this refactor is a pure architecture split, not
// a behavior change -- see native_draw_repr.h's own comment for why this
// split exists now. A future real-D3D12/Vulkan backend implements the
// same INativeRenderBackend interface and does not touch this class or
// anything upstream of it.
class ImGuiDrawListBackend final : public fable2::native_render::INativeRenderBackend {
 public:
  void SubmitDrawCommand(const fable2::native_render::NativeDrawCommand& cmd) override {
    if (!cmd.valid || cmd.vertex_count < 3) return;

    // Debug-convenience "projection": a CPU-side bounding-box fit into the
    // canvas rect below, NOT a real view/projection transform (there isn't
    // one to fabricate -- Phase 1/2 never captured a transform matrix at
    // this boundary, and this PoC does not invent one). This only decides
    // where on screen the real, decoded triangle(s) land, not their shape.
    float min_x = cmd.vertices[0].x, max_x = cmd.vertices[0].x;
    float min_y = cmd.vertices[0].y, max_y = cmd.vertices[0].y;
    for (int i = 1; i < cmd.vertex_count; ++i) {
      min_x = std::min(min_x, cmd.vertices[i].x);
      max_x = std::max(max_x, cmd.vertices[i].x);
      min_y = std::min(min_y, cmd.vertices[i].y);
      max_y = std::max(max_y, cmd.vertices[i].y);
    }
    const float span_x = (max_x - min_x) > 1e-6f ? (max_x - min_x) : 1.0f;
    const float span_y = (max_y - min_y) > 1e-6f ? (max_y - min_y) : 1.0f;

    const ImVec2 canvas_pos = ImGui::GetCursorScreenPos();
    const ImVec2 canvas_size(ImGui::GetContentRegionAvail().x, 120.0f);
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddRectFilled(
        canvas_pos, ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
        IM_COL32(20, 20, 20, 255));
    draw_list->AddRect(canvas_pos,
                        ImVec2(canvas_pos.x + canvas_size.x, canvas_pos.y + canvas_size.y),
                        IM_COL32(90, 90, 90, 255));

    auto to_screen = [&](const fable2::native_render::NativeVertex& v) {
      const float nx = (v.x - min_x) / span_x;
      const float ny = (v.y - min_y) / span_y;
      // Flip Y: guest +Y is not known to match screen-down, but this is a
      // debug view, not a claim about the game's real coordinate convention.
      return ImVec2(canvas_pos.x + nx * canvas_size.x, canvas_pos.y + (1.0f - ny) * canvas_size.y);
    };

    static const ImU32 kTriColors[2] = {IM_COL32(230, 140, 30, 200), IM_COL32(60, 180, 220, 200)};
    for (int tri = 0; tri + 2 < cmd.vertex_count; tri += 3) {
      const ImVec2 p0 = to_screen(cmd.vertices[tri + 0]);
      const ImVec2 p1 = to_screen(cmd.vertices[tri + 1]);
      const ImVec2 p2 = to_screen(cmd.vertices[tri + 2]);
      draw_list->AddTriangleFilled(p0, p1, p2, kTriColors[(tri / 3) % 2]);
      draw_list->AddCircleFilled(p0, 3.0f, IM_COL32(255, 255, 255, 255));
      draw_list->AddCircleFilled(p1, 3.0f, IM_COL32(255, 255, 255, 255));
      draw_list->AddCircleFilled(p2, 3.0f, IM_COL32(255, 255, 255, 255));
    }

    ImGui::Dummy(canvas_size);
  }
};

// Renders one candidate address's evidence line plus, if its decode was
// plausible, the decoded geometry canvas. Shared by both candidate
// sections in NativePocOverlay::OnDraw() so the canvas/triangle-drawing
// logic exists exactly once instead of being duplicated per candidate.
//
// Round 7: the actual drawing now goes through BuildDrawCommand() +
// ImGuiDrawListBackend -- see native_draw_repr.h's comment for why. This
// function's own job shrank to exactly what it says: render the evidence
// text, then hand the decoded geometry to a backend instead of drawing it
// directly. Behavior is unchanged from the pre-round-7 version; only the
// architecture split is new.
void DrawCandidateSection(const char* label, uint32_t addr, const DecodeAttempt& decode) {
  ImGui::Text("%s: addr=0x%08X decode_ok=%s", label, addr, decode.plausible ? "true" : "false");
  if (!decode.plausible) {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f), "  %s", decode.error.c_str());
    return;
  }

  for (int i = 0; i < decode.count; ++i) {
    // Round 15 follow-up: raw_index (the actual index value pulled from
    // the index buffer for this vertex, before *stride+vertex_addr) is
    // shown alongside the decoded position -- a small, sequential-looking
    // set of raw_index values with implausible positions points at the
    // stride/format guess; large or erratic raw_index values point at the
    // index buffer/address itself instead. See DecodedVertex's own comment.
    if (decode.verts[i].is_restart) {
      ImGui::TextColored(ImVec4(0.6f, 0.6f, 1.0f, 1.0f), "  v[%d] idx=%u = RESTART (strip/fan cut "
                          "-- round 15 follow-up, not a real vertex, no address computed)",
                          i, decode.verts[i].raw_index);
    } else {
      ImGui::Text("  v[%d] idx=%u = (%.4f, %.4f, %.4f)", i, decode.verts[i].raw_index,
                  decode.verts[i].x, decode.verts[i].y, decode.verts[i].z);
    }
  }

  const fable2::native_render::NativeDrawCommand cmd = BuildDrawCommand(label, addr, decode);
  ImGuiDrawListBackend backend;
  backend.SubmitDrawCommand(cmd);
}

// Phase 4 follow-up (address-semantics trace, per explicit instruction: no
// more guessed formulas -- explain what a candidate address actually is by
// reusing rex's OWN trusted resolution path instead). For one candidate
// vertex-fetch address, logs how rex's guest-memory system itself resolves
// it:
//   1. Memory::LookupHeap(addr) -- which of the four aliasing views
//      (0xA0000000/0xC0000000/0xE0000000) or the canonical 0x00000000
//      physical heap the address falls into (xmemory.h LookupHeap, ~line
//      524) -- plus that heap's own QueryRegionInfo(addr, ...), the same
//      "alias view" query the Win32 MEMORY_BASIC_INFORMATION-equivalent API
//      exposes (xmemory.h HeapAllocationInfo, ~line 92).
//   2. Memory::GetPhysicalAddress(addr) -- the canonical physical-heap-
//      relative address, or 0xFFFFFFFF if this address isn't backed by
//      kGuestPhysical at all (xmemory.h GetPhysicalAddress, ~line 414) --
//      plus GetPhysicalHeap()->QueryRegionInfo() on THAT address, the
//      canonical view.
// This mirrors the internal alias-to-canonical-physical fallback pattern
// this module found in xmemory.cpp's own write-protect/fault-handling code
// (~lines 577-604): when an alias heap's own bookkeeping is stale or
// unreliable, rex itself re-resolves via GetPhysicalAddress() + the
// canonical physical heap's QueryRegionInfo(). Reusing that same pair of
// calls here, rather than re-deriving a masking/offset formula by hand, is
// the whole point of this helper.
//
// Every field logged is raw evidence (heap_type as its enum's underlying
// int, state/protect as their raw bitmask values) -- this function does not
// interpret or assert what a specific state/protect value means, only
// whether rex itself currently considers the address to belong to a
// tracked, queryable region at all.
//
// __try/__except for the same reason as TryDecodeVertex above: this may run
// on an address already proven unreadable as guest memory, and this module
// has no prior evidence that LookupHeap/QueryRegionInfo/GetPhysicalAddress
// are safe to call with such an address (they operate on rex's internal
// page-table bookkeeping, not a direct guest-memory read, but are untested
// by this module for a known-bad address either way -- fail safe).
void LogAddressDiagnostics(const char* label, uint32_t addr) {
  __try {
    rex::memory::Memory* mem = rex::system::kernel_memory();
    if (!mem) {
      REXLOG_WARN("[NativeDrawPoc] {} addr={:#010x}: kernel_memory() returned null", label, addr);
      return;
    }

    rex::memory::BaseHeap* alias_heap = mem->LookupHeap(addr);
    if (alias_heap) {
      rex::memory::HeapAllocationInfo alias_info{};
      const bool alias_ok = alias_heap->QueryRegionInfo(addr, &alias_info);
      REXLOG_INFO(
          "[NativeDrawPoc] {} addr={:#010x}: alias heap_type={} heap_base={:#010x} "
          "heap_size={:#010x} QueryRegionInfo={} state={:#x} protect={:#x} "
          "region_size={:#x} allocation_base={:#010x}",
          label, addr, static_cast<int>(alias_heap->heap_type()), alias_heap->heap_base(),
          alias_heap->heap_size(), alias_ok, alias_info.state, alias_info.protect,
          alias_info.region_size, alias_info.allocation_base);
    } else {
      REXLOG_INFO(
          "[NativeDrawPoc] {} addr={:#010x}: LookupHeap found no alias heap for this address",
          label, addr);
    }

    const uint32_t phys_addr = mem->GetPhysicalAddress(addr);
    if (phys_addr == 0xFFFFFFFFu) {
      REXLOG_INFO(
          "[NativeDrawPoc] {} addr={:#010x}: GetPhysicalAddress -> 0xFFFFFFFF (not backed by "
          "the canonical kGuestPhysical heap)",
          label, addr);
    } else {
      rex::memory::VirtualHeap* phys_heap = mem->GetPhysicalHeap();
      rex::memory::HeapAllocationInfo phys_info{};
      const bool phys_ok = phys_heap && phys_heap->QueryRegionInfo(phys_addr, &phys_info);
      REXLOG_INFO(
          "[NativeDrawPoc] {} addr={:#010x}: canonical physical_addr={:#010x} QueryRegionInfo={} "
          "state={:#x} protect={:#x} region_size={:#x} allocation_base={:#010x}",
          label, addr, phys_addr, phys_ok, phys_info.state, phys_info.protect,
          phys_info.region_size, phys_info.allocation_base);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    REXLOG_WARN(
        "[NativeDrawPoc] {} addr={:#010x}: LogAddressDiagnostics itself faulted (SEH-caught) -- "
        "rex's own heap bookkeeping could not be safely queried for this address",
        label, addr);
  }
}

// Round 15 follow-up (fable2_059.log): a THIRD "Unhandled guest access
// violation" was observed at index_addr, this time immediately after two
// EARLIER faults at that exact same address (inside TryDecodeVertex's
// single-dword index read, both of which recovered cleanly) -- and this
// third one was not recovered; the log ends right there. This confirms,
// again, this project's own long-standing finding (see the crash-history
// comment above OnValidatedDraw / native-rendering-phase3.md's original
// regression fix): rex's own guest-memory access-violation handling is NOT
// uniformly recoverable via a local __try/__except -- some faults return
// cleanly to the local handler, others are treated as unconditionally
// fatal, and there is no way to tell which in advance from inside the
// faulting read itself. __try/__except is therefore necessary but not
// sufficient here. The only additional safety available is to check rex's
// own OWN page bookkeeping (LookupHeap + QueryRegionInfo, exactly what
// LogAddressDiagnostics above already queries and has never itself faulted
// in any log so far) BEFORE attempting a raw read at all, and skip the
// read entirely when that bookkeeping does not affirmatively say the whole
// span is committed and readable. MemoryAllocationFlag::kMemoryAllocationCommit
// (state bit 1) and MemoryProtectFlag::kMemoryProtectRead (protect bit 0)
// are exactly the two bits candidate A's SUCCESSFUL raw dump in
// fable2_058.log showed (state=0x3, protect=0xb) -- this codifies that
// same signature as an explicit precondition instead of an after-the-fact
// observation, and additionally requires the full read span to fit inside
// the region rex reports (not just its first byte), in case a too-small
// committed region backing a valid-looking address is what made the wider
// (8-dword/32-byte) DumpRawDwords probe behave differently from the
// narrower (single-dword) reads that came before it at the same address.
bool IsGuestRangeSafeToRead(uint32_t addr, uint32_t num_bytes) {
  __try {
    rex::memory::Memory* mem = rex::system::kernel_memory();
    if (!mem) return false;
    rex::memory::BaseHeap* alias_heap = mem->LookupHeap(addr);
    if (!alias_heap) return false;
    rex::memory::HeapAllocationInfo info{};
    if (!alias_heap->QueryRegionInfo(addr, &info)) return false;
    const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
    const bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
    if (!committed || !readable) return false;
    // addr must be within [allocation_base, allocation_base+region_size), and
    // the whole [addr, addr+num_bytes) span must stay inside that same region
    // -- guards against a too-small committed region making a wider read walk
    // off the end into unmapped memory.
    const uint64_t region_start = info.allocation_base;
    const uint64_t region_end = region_start + info.region_size;
    const uint64_t read_start = addr;
    const uint64_t read_end = read_start + num_bytes;
    return read_start >= region_start && read_end <= region_end;
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return false;
  }
}

// Round 15, ninth follow-up (native-rendering-phase3.md): fable2_066.log
// proved candidate_addr_raw/index_addr are NOT stable memory once
// resource_calls_captured=false -- five stride sweeps run back to back
// against the SAME already-captured draw (no re-arm, no new capture in
// between -- only the "Sweep now" button pressed repeatedly) returned five
// DIFFERENT sets of "plausible" strides (13, 3, 13, 16, then 4 hits, no
// consistent winner, 64 present in some and absent in others). The only
// thing that changed between those calls was wall-clock time. Since this
// only happens when resource_calls_captured=false ("this call reused an
// already-bound buffer rather than re-resolving one" -- this file's own
// existing comment), the simplest explanation fits: this address is a
// reused/scratch region some OTHER draw overwrites every frame, so reading
// it seconds after capture (TickAddressWatch aside, which is deliberately
// live) reads whatever unrelated draw is using that memory AT CLICK TIME,
// not the bytes that were actually there for the draw we captured. No
// stride guess can be validated against a moving target. Fix: freeze the
// actual bytes ONCE, synchronously, as early as possible after the real
// draw call (in OnValidatedDraw, before anything else runs), and make
// every later re-decode/sweep -- automatic or button-triggered -- read
// from that frozen copy instead of live guest memory. Live guest memory
// is still used by TickAddressWatch's own per-frame watch (intentionally
// -- it exists specifically to observe an async write happening over
// time, not to hold data still), and by DecodeCandidate/SweepStrides
// above (kept, still correct for a one-shot synchronous read; simply no
// longer what later re-experiments use).
constexpr uint32_t kSnapshotMaxVertices = kMaxDecodedIndices;                      // 64
constexpr uint32_t kVertexSnapshotBytes = kSnapshotMaxVertices * kStrideSweepMax;  // 64*128=8192
constexpr uint32_t kIndexSnapshotBytes = kSnapshotMaxVertices * 4u;                // 256

struct MemorySnapshot {
  bool valid = false;
  uint32_t index_addr = 0;
  uint32_t vertex_addr_raw = 0;
  uint32_t vertex_addr_formula = 0;
  uint32_t index_bytes_captured = 0;         // how many of index_bytes are real (rest is zero-fill)
  uint32_t vertex_bytes_raw_captured = 0;    // ditto, vertex_bytes_raw
  uint32_t vertex_bytes_formula_captured = 0;  // ditto, vertex_bytes_formula
  uint8_t index_bytes[kIndexSnapshotBytes] = {};
  uint8_t vertex_bytes_raw[kVertexSnapshotBytes] = {};
  uint8_t vertex_bytes_formula[kVertexSnapshotBytes] = {};
};
MemorySnapshot g_snapshot;  // guarded by g_draw_mutex; written at capture time and again if/when
                            // TickAddressWatch observes post-population data (see its own code)

// Round 15, tenth follow-up (native-rendering-phase3.md, fable2_067.log):
// this function ORIGINALLY gated every attempt on IsGuestRangeSafeToRead()
// first, by analogy to DumpRawDwords/TickAddressWatch above -- and that
// was a real regression. fable2_067.log's overlay showed EVERY candidate,
// every stride, both sweeps, all reporting "snapshot did not cover this
// address" -- 0 bytes ever captured, for addresses this exact session has
// been successfully decoding (via TryDecodeVertex's plain __try/__except,
// no precondition) since round one. The log's own "raw dword dump ...
// skipped -- IsGuestRangeSafeToRead()=false" lines for these SAME
// addresses confirm it: IsGuestRangeSafeToRead() has been returning a
// false negative for this specific guest-physical alias heap this whole
// session (that's WHY DumpRawDwords/TickAddressWatch's raw-dump sections
// have shown "(not captured)" nearly every round -- an accepted,
// known limitation for those two diagnostic-only probes, never fixed
// because nothing depended on them succeeding). Reusing that same gate
// here made the snapshot depend on a check known to fail here, when the
// thing it's supposed to protect (an actual read of this address) was
// already proven safe by hundreds of successful TryDecodeVertex calls
// this session. Fix: drop the precondition, attempt the copy directly
// under __try/__except (exactly TryDecodeVertex's own safety model), and
// shrink by half only on an ACTUAL fault, not a metadata check that
// doesn't match this heap. Each shrink is a fresh, differently-sized
// attempt (not a repeated read of the identical range), so this does not
// reproduce the "same address faulted three times, third one fatal"
// pattern fable2_059.log found and IsGuestRangeSafeToRead was originally
// built to guard against -- that finding was about many IDENTICAL re-reads
// of one address across many frames (TickAddressWatch's use case), a
// different access pattern from one shrinking sequence taken once at
// capture time.
// Round 15, fourteenth follow-up (native-rendering-phase3.md, fable2_073.log):
// direct proof, via a per-chunk diagnostic log flushed before/after every
// memcpy attempt, that the thirteenth follow-up's __try/__except around
// SnapshotGuestBytes's chunked reads does NOT actually catch this fault --
// the log shows "snapshot chunk attempt: addr=0x1f5e7690" with NO matching
// "snapshot chunk result" line and no further engine activity at all
// afterward (the process is simply gone), meaning
// rex::arch::ExceptionHandlerCallback (tools/rexglue-source/src/core/
// exception_handler_win.cpp) returning EXCEPTION_CONTINUE_SEARCH from its
// vectored handler is, for whatever reason, never reaching this function's
// frame-based __except in practice for this address class. Rather than
// keep guessing why SEH isn't catching it here, this switches to PROACTIVE
// validation: query the SAME heap/region metadata
// Memory::AccessViolationCallback itself consults (LookupHeap +
// QueryRegionInfo -- tools/rexglue-source/src/system/xmemory.cpp,
// rex/system/xmemory.h) BEFORE ever attempting a read, so a genuinely
// unmapped chunk is skipped instead of touched at all. This is NOT a
// reuse of the existing IsGuestRangeSafeToRead() (still forbidden per its
// own comment/this file's history of false negatives) -- that helper's
// false negatives came from requiring an entire, possibly-large
// (up to 8192-byte) requested span to fit inside ONE queried region;
// IsChunkReadable below applies the identical committed+readable check at
// this function's own small kSnapshotChunkBytes (16-byte) granularity,
// the same granularity TryDecodeVertex has always safely read at, so a
// read that only needs a few committed pages is not penalized by a wider
// caller's requested size.
bool IsChunkReadable(uint32_t addr, uint32_t num_bytes) {
  rex::memory::Memory* mem = rex::system::kernel_memory();
  if (!mem) return false;
  rex::memory::BaseHeap* heap = mem->LookupHeap(addr);
  if (!heap) return false;
  rex::memory::HeapAllocationInfo info{};
  if (!heap->QueryRegionInfo(addr, &info)) return false;
  const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
  const bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
  if (!committed || !readable) return false;
  const uint64_t region_start = info.allocation_base;
  const uint64_t region_end = region_start + info.region_size;
  const uint64_t read_start = addr;
  const uint64_t read_end = read_start + num_bytes;
  return read_start >= region_start && read_end <= region_end;
}

uint32_t SnapshotGuestBytes(uint8_t* base, uint32_t addr, uint32_t want_bytes, uint8_t* out) {
  std::memset(out, 0, want_bytes);
  // Round 15, eleventh follow-up: guest address 0 is unconditionally fatal
  // on the first read attempt (fable2_057.log, several rounds back) --
  // vertex_addr_raw/its formula sibling are both 0 whenever that field
  // hasn't been populated for the captured draw yet (common for an early/
  // UI draw), so this guard stays even after the redesign below.
  if (addr == 0) return 0;
  // Round 15, thirteenth follow-up (native-rendering-phase3.md,
  // fable2_071.log): the tenth/eleventh follow-ups' "try the whole block,
  // halve once on fault" design was STILL wrong -- for the trivial seq=0
  // draw (resource_calls_captured=true), this address's underlying
  // allocation is known (round 6, this whole investigation's very first
  // milestone) to be genuinely uncommitted/all-zero at capture time,
  // populated only ~3 seconds later by an async write. A single 8192-byte
  // memcpy demands the ENTIRE span be mapped in one contiguous read --
  // fundamentally different from the small, scattered 4-/12-byte reads
  // TryDecodeVertex has always safely done at this same starting address
  // all session (each individually fits inside whatever small committed
  // region exists; one giant contiguous read does not). fable2_071.log
  // confirms this: with ArmWriteWatch now disabled (twelfth follow-up),
  // the SAME "Unhandled guest access violation: read of guest 0x1F5B7590"
  // still happened, immediately after capture, with no write-watch
  // anywhere in the log -- this function's own bulk read was now the one
  // causing it.
  //
  // Fixed by reading in small, independently-guarded chunks (kSnapshotChunkBytes,
  // matching TryDecodeVertex's own proven-safe granularity) advancing
  // through the requested range, and STOPPING at the first chunk that
  // faults rather than reporting 0 for the whole attempt or retrying a
  // different size at the SAME starting address (this project's other
  // established finding -- fable2_059.log -- is that repeated faults at
  // one address can become unconditionally fatal, so this only ever
  // attempts each address once). A region that's fully mapped (this
  // session's actual target, sub_8221E0F0's draws, per every successful
  // decode so far) still gets captured in full, chunk by chunk; a region
  // that's partially or entirely uncommitted (this trivial seq=0 draw)
  // stops cleanly at the real boundary instead of demanding all-or-half of
  // a much bigger span than any single vertex decode ever actually needs.
  constexpr uint32_t kSnapshotChunkBytes = 16;
  uint32_t captured = 0;
  while (captured < want_bytes) {
    const uint32_t chunk = std::min<uint32_t>(kSnapshotChunkBytes, want_bytes - captured);
    // Round 15, fourteenth follow-up (diagnostic only): fable2_072.log
    // reproduced the IDENTICAL "Unhandled guest access violation: read of
    // guest 0x1F5B7590" crash even with the thirteenth follow-up's chunked
    // __try/__except reader in place, AND with no "[NativeDrawPoc] captured
    // seq=..." log line ever appearing afterward -- meaning the process
    // died before returning from this function, i.e. the __except below is
    // not actually catching the fault. Logging + explicit FlushLogging()
    // immediately before every single chunk attempt so that if this crashes
    // again, the log conclusively shows the exact address/iteration reached
    // -- proving or disproving that this loop is even where the fatal fault
    // occurs, rather than continuing to guess.
    bool ok = false;
    // Round 15, fourteenth follow-up: validate BEFORE touching guest memory
    // at all -- see IsChunkReadable()'s own comment above for why this
    // replaced relying on __try/__except catching a fault after the fact.
    if (IsChunkReadable(addr + captured, chunk)) {
      __try {
        std::memcpy(out + captured, REX_RAW_ADDR(addr + captured), chunk);
        ok = true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        // Kept as defense-in-depth only -- IsChunkReadable() above is now
        // the real gate. A fault reaching here despite passing that check
        // would itself be new evidence worth logging, but must not be
        // allowed to crash the process either way.
        ok = false;
      }
    }
    if (!ok) break;
    captured += chunk;
  }
  return captured;
}

MemorySnapshot CaptureMemorySnapshot(uint8_t* base, uint32_t index_addr,
                                      uint32_t vertex_addr_raw, uint32_t vertex_addr_formula) {
  MemorySnapshot snap;
  snap.index_addr = index_addr;
  snap.vertex_addr_raw = vertex_addr_raw;
  snap.vertex_addr_formula = vertex_addr_formula;
  snap.index_bytes_captured =
      SnapshotGuestBytes(base, index_addr, kIndexSnapshotBytes, snap.index_bytes);
  snap.vertex_bytes_raw_captured =
      SnapshotGuestBytes(base, vertex_addr_raw, kVertexSnapshotBytes, snap.vertex_bytes_raw);
  snap.vertex_bytes_formula_captured =
      SnapshotGuestBytes(base, vertex_addr_formula, kVertexSnapshotBytes, snap.vertex_bytes_formula);
  snap.valid = true;
  return snap;
}

// Snapshot-backed mirror of TryDecodeVertex: identical byte-swap rules
// (LoadGuestFloat/LoadGuestIndex's __builtin_bswap16/32, gated the same
// way on fable2_native_poc_swap_geometry_bytes) and identical restart-
// sentinel handling, but every read is bounds-checked against the frozen
// snapshot array instead of dereferencing live guest memory -- a read
// outside the captured window is treated exactly like a live out-of-range
// read (a reported fault, not a crash), it just can never observe memory
// that changed after capture.
bool TryDecodeVertexFromSnapshot(const uint8_t* index_snapshot, uint32_t index_snapshot_addr,
                                  uint32_t index_snapshot_size, uint32_t idx_addr, bool index32,
                                  const uint8_t* vertex_snapshot, uint32_t vertex_snapshot_addr,
                                  uint32_t vertex_snapshot_size, uint32_t stride, bool swap_bytes,
                                  float* out_x, float* out_y, float* out_z,
                                  uint32_t* out_raw_index, bool* out_is_restart) {
  const uint32_t index_width = index32 ? 4u : 2u;
  if (idx_addr < index_snapshot_addr ||
      static_cast<uint64_t>(idx_addr) + index_width >
          static_cast<uint64_t>(index_snapshot_addr) + index_snapshot_size) {
    return false;
  }
  const uint32_t index_off = idx_addr - index_snapshot_addr;
  uint32_t vertex_index;
  if (index32) {
    uint32_t raw32;
    std::memcpy(&raw32, index_snapshot + index_off, 4);
    vertex_index = __builtin_bswap32(raw32);
  } else {
    uint16_t raw16;
    std::memcpy(&raw16, index_snapshot + index_off, 2);
    vertex_index = __builtin_bswap16(raw16);
  }
  *out_raw_index = vertex_index;
  const uint32_t restart_sentinel = index32 ? 0xFFFFFFFFu : 0xFFFFu;
  if (vertex_index == restart_sentinel) {
    *out_x = 0.0f;
    *out_y = 0.0f;
    *out_z = 0.0f;
    *out_is_restart = true;
    return true;
  }
  *out_is_restart = false;
  const uint32_t v_addr = vertex_snapshot_addr + vertex_index * stride;
  if (v_addr < vertex_snapshot_addr ||
      static_cast<uint64_t>(v_addr) + 12u >
          static_cast<uint64_t>(vertex_snapshot_addr) + vertex_snapshot_size) {
    return false;
  }
  const uint32_t v_off = v_addr - vertex_snapshot_addr;
  float components[3];
  for (int c = 0; c < 3; ++c) {
    uint32_t bits;
    std::memcpy(&bits, vertex_snapshot + v_off + static_cast<uint32_t>(c) * 4u, 4);
    if (swap_bytes) bits = __builtin_bswap32(bits);
    std::memcpy(&components[c], &bits, 4);
  }
  *out_x = components[0];
  *out_y = components[1];
  *out_z = components[2];
  return true;
}

// Snapshot-backed mirror of DecodeCandidate() -- same plausibility logic
// (DecodeLooksPlausible, including this round's magnitude bound), same
// DecodedVertex/DecodeAttempt result shape, but every vertex read comes
// from the frozen MemorySnapshot instead of live guest memory.
DecodeAttempt DecodeCandidateFromSnapshot(const MemorySnapshot& snap, bool use_formula_vertex,
                                           bool index32, uint32_t stride, int index_count) {
  DecodeAttempt result;
  result.attempted = true;
  result.count = std::clamp<int>(index_count, 0, kMaxDecodedIndices);
  const bool swap_bytes = REXCVAR_GET(fable2_native_poc_swap_geometry_bytes);
  const uint32_t vertex_addr = use_formula_vertex ? snap.vertex_addr_formula : snap.vertex_addr_raw;
  const uint8_t* vertex_snapshot =
      use_formula_vertex ? snap.vertex_bytes_formula : snap.vertex_bytes_raw;
  const uint32_t vertex_snapshot_size =
      use_formula_vertex ? snap.vertex_bytes_formula_captured : snap.vertex_bytes_raw_captured;

  for (int i = 0; i < result.count; ++i) {
    const uint32_t index_stride = index32 ? 4u : 2u;
    const uint32_t idx_addr = snap.index_addr + static_cast<uint32_t>(i) * index_stride;
    if (!TryDecodeVertexFromSnapshot(snap.index_bytes, snap.index_addr, snap.index_bytes_captured,
                                      idx_addr, index32, vertex_snapshot, vertex_addr,
                                      vertex_snapshot_size, stride, swap_bytes, &result.verts[i].x,
                                      &result.verts[i].y, &result.verts[i].z,
                                      &result.verts[i].raw_index, &result.verts[i].is_restart)) {
      result.read_fault = true;
      result.fault_at_index = i;
      result.count = 0;
      break;
    }
  }

  if (result.read_fault) {
    result.error =
        "snapshot did not cover this address (outside the window frozen at capture time) while "
        "decoding vertex " + std::to_string(result.fault_at_index) +
        " -- this candidate/stride combination cannot be tested against the frozen snapshot";
  } else {
    result.plausible = result.count >= 3 && DecodeLooksPlausible(result.verts, result.count);
    if (!result.plausible) {
      result.error = result.count < 3
          ? "index_count < 3, nothing to draw"
          : "decoded vertices are all-zero or non-finite for this candidate address -- "
            "fable2_native_poc_vertex_stride / fable2_native_poc_swap_geometry_bytes is "
            "very likely wrong for this draw; try toggling them";
    }
  }
  return result;
}

// Snapshot-backed mirror of SweepStrides() -- same range/step, same
// DecodeCandidateFromSnapshot() underneath, so results are fully
// reproducible against the SAME frozen bytes no matter how many times or
// how much later this is called (unlike a live sweep against a reused
// scratch buffer -- see this section's opening comment).
StrideSweepResult SweepStridesFromSnapshot(const MemorySnapshot& snap, bool use_formula_vertex,
                                            bool index32, int index_count) {
  StrideSweepResult result;
  result.attempted = true;
  for (uint32_t stride = kStrideSweepMin; stride <= kStrideSweepMax; stride += kStrideSweepStep) {
    ++result.candidates_tried;
    DecodeAttempt attempt =
        DecodeCandidateFromSnapshot(snap, use_formula_vertex, index32, stride, index_count);
    if (attempt.plausible && result.plausible_count < kMaxSweepHits) {
      result.plausible_strides[result.plausible_count++] = stride;
    }
  }
  return result;
}

// Phase 4 follow-up (round 2): the minimum possible probe -- read
// kRawDumpDwords consecutive dwords starting AT `addr` via REX_LOAD_U32,
// nothing else. Deliberately does NOT go through TryDecodeVertex's
// index/stride derivation (see g_raw_dump_call1's comment above for why
// that path's single caught fault is ambiguous about which read inside it
// actually faulted). A single, isolated __try/__except, exactly like every
// other guest-adjacent read in this module -- reports either the real bytes
// or that this exact read faulted, with nothing in between to misattribute.
// Round 15 follow-up: callers now gate this on IsGuestRangeSafeToRead()
// first (see its comment above for why __try/__except alone is not enough
// here) -- this function's own __try/__except stays as defense in depth,
// not as the sole safety mechanism.
RawDumpResult DumpRawDwords(uint8_t* base, uint32_t addr) {
  RawDumpResult result;
  result.attempted = true;
  result.addr = addr;
  __try {
    for (int i = 0; i < kRawDumpDwords; ++i) {
      result.dwords[i] = REX_LOAD_U32(addr + static_cast<uint32_t>(i) * 4u);
    }
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    result.fault = true;
  }
  return result;
}

// Phase 4 follow-up (round 3): called once per frame from
// NativePocOverlay::OnDraw() (see g_watch_active's comment above for why).
// Unlike OnValidatedDraw(), this does NOT run inside the recompiled PPC
// call graph, so it has no `base` parameter handed to it -- it obtains its
// own host pointer via rex::memory::Memory::virtual_membase(), the same
// public accessor TranslateVirtual() uses internally
// (`virtual_membase_ + guest_address`, xmemory.h ~line 387-388), which is
// structurally identical to REX_RAW_ADDR's `base + x` term. This is
// evidence, not an assumption taken on faith: OnValidatedDraw() below
// cross-checks this pointer against the REAL `base` the recompiled call
// graph hands it at capture time and logs whether they match, the first
// time a draw is captured.
void TickAddressWatch() {
  if (!g_watch_active.load(std::memory_order_acquire)) return;
  if (g_watch_done.load(std::memory_order_acquire)) return;

  rex::memory::Memory* mem = rex::system::kernel_memory();
  if (!mem) return;
  uint8_t* base = mem->virtual_membase();
  if (!base) return;

  const uint32_t addr = g_watch_addr.load(std::memory_order_relaxed);
  // Round 15 follow-up: defense in depth, same as OnValidatedDraw's probes
  // above -- never attempt a read at exactly guest address 0 (this file's
  // own arming code should no longer ever set g_watch_addr to 0, since
  // arming is now gated on resource_calls_captured, but this is a cheap,
  // independent safety net against a future regression of the same kind).
  // Also gate on IsGuestRangeSafeToRead(), same as both OnValidatedDraw
  // probes -- this specific per-frame read is what fable2_058.log's own
  // successful "watch: ... became non-zero" -> "POST-POPULATION re-decode
  // ... decode_ok=true" milestone came from, so this check is intentionally
  // NOT tightened beyond what that already-working case satisfies (a
  // committed, readable, correctly-sized region); it only adds the same
  // safety net used elsewhere, it does not change behavior for an address
  // that was already safe.
  if (addr == 0 || !IsGuestRangeSafeToRead(addr, kRawDumpDwords * 4u)) {
    g_watch_done.store(true, std::memory_order_release);
    return;
  }
  const RawDumpResult r = DumpRawDwords(base, addr);

  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_raw_dump_call1 = r;
  }

  if (r.fault) {
    g_watch_done.store(true, std::memory_order_release);
    REXLOG_WARN(
        "[NativeDrawPoc] watch: re-read of call #1's return value ({:#010x}) FAULTED on a later "
        "frame (capture-time read did not fault) -- the region's state changed between capture "
        "and this frame",
        addr);
    return;
  }

  bool any_nonzero = false;
  for (uint32_t dword : r.dwords) {
    if (dword != 0) {
      any_nonzero = true;
      break;
    }
  }
  if (any_nonzero) {
    g_watch_done.store(true, std::memory_order_release);
    REXLOG_INFO(
        "[NativeDrawPoc] watch: call #1's return value ({:#010x}) became non-zero -- first 32 "
        "bytes now: {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x}",
        addr, r.dwords[0], r.dwords[1], r.dwords[2], r.dwords[3], r.dwords[4], r.dwords[5],
        r.dwords[6], r.dwords[7]);

    // Round 6: the original OnValidatedDraw() decode ran against this same
    // address while it was still all-zero (guaranteed implausible). Now
    // that it's populated, re-run the IDENTICAL decode (same index_addr,
    // index32, stride, index_count, same address candidates) -- this is
    // the first chance for a real decode result, not another guess.
    DeferredDecodeParams params;
    {
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      params = g_deferred_decode_params;
    }
    if (params.valid) {
      DecodeAttempt redecode_raw =
          DecodeCandidate(base, params.index_addr, params.index32, params.candidate_addr_raw,
                           params.stride, params.index_count);
      DecodeAttempt redecode_formula =
          DecodeCandidate(base, params.index_addr, params.index32, params.candidate_addr_formula,
                           params.stride, params.index_count);
      // Round 15, ninth follow-up: this is exactly the case MemorySnapshot's
      // capture-time freeze can miss -- candidate A was genuinely all-zero
      // (not yet written) at capture time, so the original snapshot is
      // stale by design here. Re-freeze NOW, right after observing the
      // real population, so ReDecodeWithCurrentCvars()/
      // RunStrideSweepWithCurrentParams() (the overlay's slider/button
      // controls) test against these newly-populated bytes afterward,
      // instead of the old all-zero snapshot or (worse) whatever unrelated
      // draw has reused this address by the time the user next clicks
      // something.
      MemorySnapshot refreshed_snapshot = CaptureMemorySnapshot(
          base, params.index_addr, params.candidate_addr_raw, params.candidate_addr_formula);
      {
        std::lock_guard<std::mutex> lock(g_draw_mutex);
        g_decode_raw = redecode_raw;
        g_decode_formula = redecode_formula;
        g_snapshot = refreshed_snapshot;
      }
      REXLOG_INFO(
          "[NativeDrawPoc] POST-POPULATION re-decode -- candidate A (raw) addr={:#010x} "
          "decode_ok={}{}",
          params.candidate_addr_raw, redecode_raw.plausible,
          redecode_raw.plausible ? "" : (std::string(" (") + redecode_raw.error + ")").c_str());
      REXLOG_INFO(
          "[NativeDrawPoc] POST-POPULATION re-decode -- candidate B (formula) addr={:#010x} "
          "decode_ok={}{}",
          params.candidate_addr_formula, redecode_formula.plausible,
          redecode_formula.plausible ? ""
                                      : (std::string(" (") + redecode_formula.error + ")").c_str());
      if (redecode_raw.plausible || redecode_formula.plausible) {
        REXLOG_INFO(
            "[NativeDrawPoc] *** FIRST PLAUSIBLE DECODE -- overlay (F4) should now show a real "
            "captured triangle from Fable II guest memory ***");
      }
    } else {
      REXLOG_WARN(
          "[NativeDrawPoc] watch: population observed but no DeferredDecodeParams captured -- "
          "cannot re-decode");
    }
  }
}

// Round 15, sixth follow-up (native-rendering-phase3.md): fable2_063.log's
// screenshot proved the restart-marker fix (immediately above) is correct
// but not sufficient -- a legitimate, non-restart index (idx=6) still
// decoded to an enormous-but-finite z value even though x/y looked
// plausible/near-zero. With no ground-truth stride available for this
// draw (resource_calls_captured was false for the whole capture), the
// decode falls back to the fable2_native_poc_vertex_stride cvar's guess
// (32 bytes), and there is no evidence yet that 32 is correct for
// sub_8221E0F0's real vertex format. Previously, testing a different guess
// meant relaunching fable2.exe with a new
// --fable2_native_poc_vertex_stride=N command-line flag for every attempt
// -- a full rebuild-adjacent cycle per guess. rex/cvar.h's REXCVAR_SET
// macro (confirmed to exist and support live mutation -- see the comment
// block above REXCVAR_DEFINE_UINT32/BOOL near the top of this file) means
// the SAME already-captured draw can be re-decoded against a freshly
// changed stride/index32 value with no new game capture and no rebuild at
// all. g_deferred_decode_params already stores everything DecodeCandidate
// needs from the ORIGINAL capture (index_addr, index_count, and both
// candidate vertex addresses) -- this function is the interactive
// counterpart to TickAddressWatch's own automatic post-population
// re-decode immediately above: same DeferredDecodeParams, same
// DecodeCandidate() calls, but triggered by the user's "Re-decode now"
// button (see the ImGui block near the "Decode parameters" text) instead
// of the write-watch, and reading LIVE stride/index32 cvar values instead
// of the ones frozen at capture time. (swap_geometry_bytes needs no
// separate plumbing here: LoadGuestFloat already calls REXCVAR_GET on it
// fresh for every read, so toggling it already takes effect on the very
// next decode -- raw or re-decoded -- with no code change.)
void ReDecodeWithCurrentCvars() {
  MemorySnapshot snapshot;
  DeferredDecodeParams params;
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    params = g_deferred_decode_params;
    snapshot = g_snapshot;
  }
  // Round 15, ninth follow-up: no more live memory access here at all --
  // decoding now happens entirely against the frozen snapshot captured at
  // draw time (or, if TickAddressWatch has since observed population, the
  // refreshed post-population snapshot -- see its own comment). See
  // MemorySnapshot's comment above for why re-reading live guest memory on
  // every button press was actively wrong for a resource_calls_captured=
  // false draw.
  if (!params.valid || !snapshot.valid) {
    REXLOG_WARN(
        "[NativeDrawPoc] manual re-decode requested but no captured draw is available yet -- "
        "arm and capture a draw first (F4 overlay's own capture controls)");
    return;
  }

  const bool index32_live = REXCVAR_GET(fable2_native_poc_index32);
  const uint32_t stride_live =
      std::max<uint32_t>(REXCVAR_GET(fable2_native_poc_vertex_stride), 4u);

  DecodeAttempt redecode_raw = DecodeCandidateFromSnapshot(
      snapshot, /*use_formula_vertex=*/false, index32_live, stride_live, params.index_count);
  DecodeAttempt redecode_formula = DecodeCandidateFromSnapshot(
      snapshot, /*use_formula_vertex=*/true, index32_live, stride_live, params.index_count);
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_decode_raw = redecode_raw;
    g_decode_formula = redecode_formula;
  }
  REXLOG_INFO(
      "[NativeDrawPoc] MANUAL re-decode (interactive stride/format experiment) -- stride={} "
      "index32={} -- candidate A decode_ok={}{}",
      stride_live, index32_live, redecode_raw.plausible,
      redecode_raw.plausible ? "" : (std::string(" (") + redecode_raw.error + ")").c_str());
  REXLOG_INFO(
      "[NativeDrawPoc] MANUAL re-decode (interactive stride/format experiment) -- stride={} "
      "index32={} -- candidate B decode_ok={}{}",
      stride_live, index32_live, redecode_formula.plausible,
      redecode_formula.plausible ? ""
                                  : (std::string(" (") + redecode_formula.error + ")").c_str());
  if (redecode_raw.plausible || redecode_formula.plausible) {
    REXLOG_INFO(
        "[NativeDrawPoc] *** MANUAL RE-DECODE FOUND A PLAUSIBLE RESULT -- current stride/index32 "
        "sliders are worth recording as the real format for this draw type ***");
  }
}

// Round 15, eighth follow-up: on-demand counterpart to the automatic sweep
// OnValidatedDraw() runs once at capture time (see SweepStrides() above) --
// lets the user re-run the whole stride sweep after changing index32/
// swap_geometry_bytes via the overlay checkboxes, without needing a new
// game capture.
void RunStrideSweepWithCurrentParams() {
  MemorySnapshot snapshot;
  DeferredDecodeParams params;
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    params = g_deferred_decode_params;
    snapshot = g_snapshot;
  }
  // Round 15, ninth follow-up: sweeps the frozen snapshot, not live memory
  // -- see MemorySnapshot's comment for why fable2_066.log proved a live
  // re-sweep gives a different, unreproducible answer every time it's run
  // against a resource_calls_captured=false draw.
  if (!params.valid || !snapshot.valid) {
    REXLOG_WARN(
        "[NativeDrawPoc] stride sweep requested but no captured draw is available yet -- arm "
        "and capture a draw first (F4 overlay's own capture controls)");
    return;
  }

  const bool index32_live = REXCVAR_GET(fable2_native_poc_index32);
  StrideSweepResult sweep_raw = SweepStridesFromSnapshot(snapshot, /*use_formula_vertex=*/false,
                                                           index32_live, params.index_count);
  StrideSweepResult sweep_formula = SweepStridesFromSnapshot(
      snapshot, /*use_formula_vertex=*/true, index32_live, params.index_count);
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_sweep_raw = sweep_raw;
    g_sweep_formula = sweep_formula;
  }

  std::string hits_raw, hits_formula;
  for (int i = 0; i < sweep_raw.plausible_count; ++i) {
    hits_raw += std::to_string(sweep_raw.plausible_strides[i]) + " ";
  }
  for (int i = 0; i < sweep_formula.plausible_count; ++i) {
    hits_formula += std::to_string(sweep_formula.plausible_strides[i]) + " ";
  }
  REXLOG_INFO(
      "[NativeDrawPoc] MANUAL stride sweep (candidate A, {} strides tried [{}..{} step {}]): "
      "plausible at: {}",
      sweep_raw.candidates_tried, kStrideSweepMin, kStrideSweepMax, kStrideSweepStep,
      hits_raw.empty() ? "(none)" : hits_raw.c_str());
  REXLOG_INFO(
      "[NativeDrawPoc] MANUAL stride sweep (candidate B, {} strides tried [{}..{} step {}]): "
      "plausible at: {}",
      sweep_formula.candidates_tried, kStrideSweepMin, kStrideSweepMax, kStrideSweepStep,
      hits_formula.empty() ? "(none)" : hits_formula.c_str());
}

// Phase 4 follow-up (round 5): rex's own PhysicalMemoryInvalidationCallback
// signature (xmemory.h ~line 488). Fires from inside rex's own
// AccessViolationCallback machinery, on whichever thread just performed the
// write that triggered it -- deliberately minimal: no REXLOG (logging can
// allocate or take locks this context may not safely support), no guest-
// memory reads, no calls back into Memory's own API (registering/
// unregistering from inside your own callback is asking for reentrancy
// trouble). CaptureStackBackTrace is documented safe in essentially any
// context (no heap allocation) and is the one "expensive" thing done here.
// Actual logging and unregistering happen later, from TickAddressWatch()'s
// normal per-frame context, once it observes g_write_watch_fired==true.
std::pair<uint32_t, uint32_t> OnPhysicalMemoryInvalidated(void* context_ptr,
                                                            uint32_t physical_address_start,
                                                            uint32_t length, bool exact_range) {
  (void)context_ptr;
  bool expected = false;
  if (g_write_watch_fired.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    g_write_watch_result.attempted = true;
    g_write_watch_result.fired = true;
    g_write_watch_result.physical_address_start = physical_address_start;
    g_write_watch_result.length = length;
    g_write_watch_result.exact_range = exact_range;
    g_write_watch_result.thread_id = GetCurrentThreadId();
    LARGE_INTEGER qpc{};
    QueryPerformanceCounter(&qpc);
    g_write_watch_result.qpc_ticks = qpc.QuadPart;
    g_write_watch_result.frame_count = static_cast<USHORT>(CaptureStackBackTrace(
        0, kWriteWatchMaxFrames, g_write_watch_result.frames, nullptr));
  }
  // Per this callback type's own contract ("Returns start and length of the
  // smallest physical memory region surrounding the watched region that can
  // be safely unwatched, if it doesn't matter, return (0, UINT32_MAX)") --
  // this watch only ever wanted its first hit, so let rex unprotect however
  // it judges safe rather than us guessing a range.
  return {0u, 0xFFFFFFFFu};
}

// Registers and arms the write watch for [addr, addr+length) (rex snaps
// this to system page boundaries itself). Called once from
// OnValidatedDraw(), as early as possible (before the decode/diagnostics
// work below it), so the window between allocation and the real write is
// covered as tightly as this module can manage.
void ArmWriteWatch(uint32_t addr, uint32_t length) {
  rex::memory::Memory* mem = rex::system::kernel_memory();
  if (!mem) {
    REXLOG_WARN("[NativeDrawPoc] write watch: kernel_memory() returned null -- not armed");
    return;
  }
  const uint32_t phys_addr = mem->GetPhysicalAddress(addr);
  if (phys_addr == 0xFFFFFFFFu) {
    REXLOG_WARN(
        "[NativeDrawPoc] write watch: GetPhysicalAddress({:#010x}) -> 0xFFFFFFFF (not backed by "
        "the canonical physical heap) -- not armed",
        addr);
    return;
  }

  void* handle =
      mem->RegisterPhysicalMemoryInvalidationCallback(&OnPhysicalMemoryInvalidated, nullptr);
  mem->EnablePhysicalMemoryAccessCallbacks(phys_addr, length,
                                            /*enable_invalidation_notifications=*/true,
                                            /*enable_data_providers=*/false);
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_write_watch_callback_handle = handle;
  }
  LARGE_INTEGER qpc{};
  QueryPerformanceCounter(&qpc);
  g_write_watch_armed_qpc.store(qpc.QuadPart, std::memory_order_relaxed);
  REXLOG_INFO(
      "[NativeDrawPoc] write watch armed: raw_addr={:#010x} physical_addr={:#010x} length={} -- "
      "watching for the first write to call #1's allocation",
      addr, phys_addr, length);
}

// Deferred half of the write watch: called every frame from
// TickAddressWatch()'s caller context (safe to log/unregister here, unlike
// inside OnPhysicalMemoryInvalidated() itself). Logs the captured evidence
// exactly once and then unregisters the callback so a LATER, unrelated
// write to the same shared scratch region (reused by other draws) does not
// arm anything further -- this watch was only ever about call #1's own
// allocation.
void TickWriteWatch() {
  if (!g_write_watch_fired.load(std::memory_order_acquire)) return;
  if (g_write_watch_reported.exchange(true, std::memory_order_acq_rel)) return;

  const WriteWatchResult r = g_write_watch_result;  // single writer already finished; safe to copy

  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  const LONGLONG armed_qpc = g_write_watch_armed_qpc.load(std::memory_order_relaxed);
  const double elapsed_ms = freq.QuadPart != 0
                                 ? 1000.0 * static_cast<double>(r.qpc_ticks - armed_qpc) /
                                       static_cast<double>(freq.QuadPart)
                                 : -1.0;

  REXLOG_INFO(
      "[NativeDrawPoc] write watch FIRED: physical_address_start={:#010x} length={} "
      "exact_range={} thread_id={} elapsed_since_arm_ms={:.2f} frame_count={}",
      r.physical_address_start, r.length, r.exact_range, r.thread_id, elapsed_ms, r.frame_count);
  std::string frames_str;
  for (USHORT i = 0; i < r.frame_count; ++i) {
    char buf[24];
    std::snprintf(buf, sizeof(buf), "%p ", r.frames[i]);
    frames_str += buf;
  }
  REXLOG_INFO("[NativeDrawPoc] write watch call stack (raw return addresses, skip=0): {}",
              frames_str);

  void* handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    handle = g_write_watch_callback_handle;
    g_write_watch_callback_handle = nullptr;
  }
  if (handle) {
    rex::memory::Memory* mem = rex::system::kernel_memory();
    if (mem) mem->UnregisterPhysicalMemoryInvalidationCallback(handle);
  }
}

}  // namespace

bool WantsNextValidatedDrawFrom(uint32_t hook_addr) {
  if (!REXCVAR_GET(fable2_native_poc_enable)) return false;
  if (g_have_draw.load(std::memory_order_acquire)) return false;
  const uint32_t filter = g_source_filter.load(std::memory_order_relaxed);
  return filter == 0 || filter == hook_addr;
}

// Round 16, twenty-ninth follow-up: the fetch-constant hand-off's own
// entry point, analogous to OnValidatedDraw() below but for the completely
// separate slot-31/constC data path (see ValidatedFetchConstantVertexInfo's
// comment in fable_native_poc.h). Just stores the snapshot for the overlay
// to render -- no decode happens here, so this can never itself introduce
// a new crash/slowdown into the sub_8221E0F0 hook that calls it.
// Round 19 (Fable2PC, first-native-geometry milestone): once the plausible-
// magnitude check below is satisfied for the first time, g_fetch_const_vertex
// is locked and never overwritten again this process -- exactly
// OnValidatedDraw()'s own one-shot pattern (g_have_draw below), applied
// here for the same reason: a debug view that keeps changing every frame
// cannot be pointed at, screenshotted, or cited as "the draw that proved
// this works." Everything upstream (the plausibility check/log itself,
// BuildDrawCommandFromValidatedFetchConstant, the capture pipeline behind
// it) is unchanged and keeps running/logging every capture regardless --
// only the overlay-visible snapshot freezes.
// static: this sits after the file's anonymous namespace already closed
// (unlike g_have_draw above, which is inside it) -- internal linkage given
// explicitly here instead, matching this file's own convention that state
// like this is never meant to be referenced from another translation unit.
static std::atomic<bool> g_native_constC_locked{false};

void OnValidatedFetchConstantVertexStream(const ValidatedFetchConstantVertexInfo& info) {
  // Round 16, thirty-fourth follow-up: a cheap, purely diagnostic
  // plausibility check, logged right here so a play session's own log file
  // says directly whether THIS capture decoded into small, bounded values
  // (round 30's 28-byte find) or the mixed-magnitude noise round 28 found
  // for the 48-word shader -- instead of that assessment only being
  // possible by hand, one upload round-trip at a time.
  const fable2::native_render::NativeDrawCommand cmd = BuildDrawCommandFromValidatedFetchConstant(info);
  bool plausible = false;
  if (cmd.valid && cmd.vertex_count > 0) {
    float max_abs = 0.0f;
    float min_abs_nonzero = -1.0f;
    // Round 20, ninth follow-up (native-rendering-phase3.md): round 19's
    // own first-build-follow-up comment already predicted this gap in
    // words -- "NaN compares false to everything, so it can never become
    // the new max or min and would never show up in that aggregate" -- but
    // it was never actually triggered by a real capture until the widened
    // snapshot window (eighth follow-up, 224 -> 16384 bytes) started
    // decoding far more real records per draw: fable2_114.log's own
    // LATCHED draw (a DIFFERENT binding than fable2_112/113.log's --
    // different address/ucode_hash, size_bytes=41472 -- whichever capture
    // happens to be first-plausible after a process restart is not the
    // same draw every time) produced v[31] = (-17888, NaN, 0), which the
    // overlay's own pre-existing finite-check display (added round 19,
    // third-build follow-up, VertexDump loop below) correctly flagged in
    // red -- but the plausibility gate above it, checking only x/y max_abs/
    // min_abs_nonzero via std::fabs, let it through anyway, exactly as
    // predicted. Direct evidence, not a guess -- so now checked directly:
    // any non-finite x, y, or z in ANY decoded vertex makes the whole
    // capture implausible, full stop, matching what the overlay already
    // shows in red rather than silently disagreeing with its own display.
    bool any_non_finite = false;
    for (int i = 0; i < cmd.vertex_count; ++i) {
      const fable2::native_render::NativeVertex& v = cmd.vertices[i];
      if (!std::isfinite(v.x) || !std::isfinite(v.y) || !std::isfinite(v.z)) {
        any_non_finite = true;
        break;
      }
      const float comps[2] = {v.x, v.y};
      for (float c : comps) {
        const float a = std::fabs(c);
        if (a > max_abs) max_abs = a;
        if (a > 0.0f && (min_abs_nonzero < 0.0f || a < min_abs_nonzero)) min_abs_nonzero = a;
      }
    }
    // Thresholds are deliberately generous (not a claim about what "real"
    // guest-unit geometry must look like, just a coarse noise filter):
    // round 30's coherent 28-byte decode stayed within roughly [0, 43000];
    // round 28's incoherent 48-word decode ranged from ~1e-38 to ~1e6 in
    // the SAME record. Flags exactly that pattern -- a huge dynamic range
    // within one small vertex batch -- without asserting a single correct
    // value range for every possible vertex format.
    plausible = !any_non_finite && max_abs < 100000.0f &&
                (min_abs_nonzero < 0.0f || min_abs_nonzero > 1e-6f);
    REXLOG_INFO(
        "[NativeDrawPoc] [FetchConstantDecode] slot=31 constC addr={:#x} ucode_hash={:#018x} "
        "stride_bytes={} attribute_count={} decoded_vertex_count={} plausible_magnitude={} "
        "(x/y max_abs={:.6g} min_abs_nonzero={:.6g})",
        info.address, info.ucode_hash, info.stride_words * 4u, info.attribute_count,
        cmd.vertex_count, plausible, max_abs, min_abs_nonzero);
  }

  // Round 20, eleventh follow-up (view/projection reconstruction --
  // native-rendering-phase3.md): run the structural candidate searches on
  // EVERY capture attempt for this draw (not just whichever one
  // eventually latches), so evidence accumulates across many draws/
  // shaders in one play session instead of only ever seeing one. Logged
  // only when at least one candidate is found, to avoid spamming the log
  // on the common case of nothing structurally interesting -- this
  // mirrors this function's own existing throttling discipline (compare
  // the plausibility log above, which always fires, with this, which
  // does not, because the rigid/projection searches are expected to hit
  // far less often and a miss is not itself informative the way a
  // plausibility verdict is).
  {
    // Round 20, twenty-third follow-up: record target-shader sightings
    // unconditionally, every capture, regardless of whether a candidate
    // is found this time -- this is the signal that answers "was I even
    // near the right thing," which the person can watch for live.
    for (uint64_t target : kLeadingCandidateHashes) {
      if (info.ucode_hash == target) {
        std::lock_guard<std::mutex> vp_lock(g_draw_mutex);
        g_vp_watch.target_shader_seen = true;
        g_vp_watch.target_shader_hash = info.ucode_hash;
        g_vp_watch.target_shader_time = std::chrono::steady_clock::now();
        break;
      }
    }
    // Round 20, twenty-fifth follow-up: the fixed hash list above just
    // missed a real hit (0xa5846836c90e1192, see
    // MatchesLeadingProjectionSignature's own comment) because it isn't
    // one of the two seed hashes. This capture's own PROJECTION scan
    // (below) can recognize the same shader by its c[72] VALUE signature
    // regardless of hash, so a signature match there also marks the
    // target as seen -- see the perspective-candidate loop below, which
    // sets this same g_vp_watch.target_shader_seen flag when
    // MatchesLeadingProjectionSignature(c) is true, independent of
    // whether info.ucode_hash happens to be in the list above.
    RigidTransformCandidate rigid_candidates[kMaxRigidTransformCandidates];
    const int rigid_count =
        FindAllRigidTransformCandidates(info, rigid_candidates, kMaxRigidTransformCandidates);
    for (int i = 0; i < rigid_count; ++i) {
      const RigidTransformCandidate& c = rigid_candidates[i];
      REXLOG_INFO(
          "[NativeDrawPoc] [ViewProjectionSearch] slot=31 constC addr={:#x} ucode_hash={:#018x}: "
          "RIGID TRANSFORM candidate #{} at logical base c[{}]: row0=({:.6g},{:.6g},{:.6g},{:.6g}) "
          "row1=({:.6g},{:.6g},{:.6g},{:.6g}) row2=({:.6g},{:.6g},{:.6g},{:.6g}) -- structural "
          "match only (unit-length + mutually perpendicular xyz rows), no claim this is "
          "specifically a view matrix vs. another object/bone transform -- see this function's "
          "own comment",
          info.address, info.ucode_hash, i, c.base_logical_index, c.rows[0][0], c.rows[0][1],
          c.rows[0][2], c.rows[0][3], c.rows[1][0], c.rows[1][1], c.rows[1][2], c.rows[1][3],
          c.rows[2][0], c.rows[2][1], c.rows[2][2], c.rows[2][3]);
    }
    ProjectionCandidate proj_candidates[kMaxProjectionCandidates];
    const int proj_count =
        FindProjectionCandidates(info, proj_candidates, kMaxProjectionCandidates);
    // Round 20, twenty-fourth follow-up (view/projection reconstruction --
    // native-rendering-phase3.md): fable2_125.2.log caught a SECOND,
    // independent instance of the eighteenth follow-up's "sliding window
    // reads one register into an adjacent real rigid/identity block"
    // false positive (this time on a brand-new shader,
    // 0x475ec9f795e5edbb, row1/row2/row3 exactly the identity's own
    // three rows again) -- and this time [AutoVerify] did NOT reject it:
    // dividing real, already-world-space, camera-facing geometry by
    // roughly its own z (which is what composing through a near-identity
    // block effectively does) tends to land in a bounded-looking NDC
    // range by ordinary geometric coincidence, regardless of whether the
    // matrix has any real meaning as a camera. The eighteenth follow-up's
    // reasoning ("AutoVerify already rejects these correctly, no
    // scanner-side fix needed") is disproven by this direct counter-
    // example -- reversed here, with the evidence that reversed it. Fix:
    // detect the overlap structurally (a projection candidate's base
    // register sitting exactly one below an independently-found rigid
    // candidate's base means rows 1/2/3 of the former ARE rows 0/1/2 of
    // the latter, read from the exact same registers -- no float
    // comparison needed, just the register indices) and both annotate it
    // in the log (never silently dropped) and skip it in AutoVerify and
    // the live watch below, where it would otherwise produce a
    // misleading pass.
    bool proj_overlaps_rigid[kMaxProjectionCandidates] = {};
    for (int i = 0; i < proj_count; ++i) {
      for (int j = 0; j < rigid_count; ++j) {
        if (rigid_candidates[j].base_logical_index == proj_candidates[i].base_logical_index + 1) {
          proj_overlaps_rigid[i] = true;
          break;
        }
      }
    }
    for (int i = 0; i < proj_count; ++i) {
      const ProjectionCandidate& c = proj_candidates[i];
      REXLOG_INFO(
          "[NativeDrawPoc] [ViewProjectionSearch] slot=31 constC addr={:#x} ucode_hash={:#018x}: "
          "PROJECTION candidate #{} at logical base c[{}] kind={} row0/row1_diagonal={} "
          "overlaps_rigid_block={} (true = rows 1/2/3 are exactly an independently-found rigid "
          "transform's own 3 rows, one register over -- a known false-positive class, see round "
          "20's twenty-fourth follow-up; false = row3's shape matched but row0/row1 have real "
          "cross-terms -- consistent with a pre-combined World*View*Projection matrix rather "
          "than a standalone textbook projection; see ProjectionCandidate::row0_row1_diagonal's "
          "own comment): row0=({:.6g},{:.6g},{:.6g},{:.6g}) row1=({:.6g},{:.6g},{:.6g},{:.6g}) "
          "row2=({:.6g},{:.6g},{:.6g},{:.6g}) row3=({:.6g},{:.6g},{:.6g},{:.6g}) -- structural "
          "match only, NOT applied to any rendering path yet -- see this function's own comment",
          info.address, info.ucode_hash, i, c.base_logical_index,
          c.is_orthographic ? "orthographic" : "perspective", c.row0_row1_diagonal,
          proj_overlaps_rigid[i], c.rows[0][0], c.rows[0][1], c.rows[0][2], c.rows[0][3],
          c.rows[1][0], c.rows[1][1], c.rows[1][2], c.rows[1][3], c.rows[2][0], c.rows[2][1],
          c.rows[2][2], c.rows[2][3], c.rows[3][0], c.rows[3][1], c.rows[3][2], c.rows[3][3]);
      if (!c.is_orthographic && !proj_overlaps_rigid[i]) {
        std::lock_guard<std::mutex> vp_lock(g_draw_mutex);
        g_vp_watch.perspective_seen = true;
        g_vp_watch.perspective_hash = info.ucode_hash;
        g_vp_watch.perspective_base = c.base_logical_index;
        g_vp_watch.perspective_time = std::chrono::steady_clock::now();
        // Round 20, twenty-fifth follow-up: a c[72]-shaped perspective
        // candidate carrying the leading candidate's own near/far
        // signature counts as the target being seen, regardless of
        // whether info.ucode_hash is one of the two seed hashes in
        // kLeadingCandidateHashes -- see MatchesLeadingProjectionSignature's
        // own comment for why this shape is distinctive enough to trust.
        if (!proj_overlaps_rigid[i] && MatchesLeadingProjectionSignature(c)) {
          g_vp_watch.target_shader_seen = true;
          g_vp_watch.target_shader_hash = info.ucode_hash;
          g_vp_watch.target_shader_time = std::chrono::steady_clock::now();
        }
      }
    }

    // Round 20, seventeenth follow-up (view/projection reconstruction):
    // automate the SAME kind of hand-composition test the thirteenth
    // follow-up did manually, by hand, for the (unrelated, rejected)
    // c[72] rigid-transform lead -- but decisive and run on every future
    // capture instead of needing another manual round-trip through a
    // fresh play session's log. Composes cmd.vertices[i] through the
    // candidate projection matrix using this codebase's own
    // out[i] = v . row_i convention (same convention the thirteenth
    // follow-up's manual test used), performs the perspective divide
    // (w = row3 . v, matching this candidate's own row3=(0,0,1,0)-style
    // "w=z" shape), and logs the resulting NDC-space range actually
    // achieved across this draw's real geometry. Real, currently-visible
    // geometry should divide down to roughly [-1, 1] in x and y (this
    // engine's own PA_CL_CLIP_CNTL.dx_clip_space_def=1, logged by
    // fable_draw_capture.cpp's [Viewport] line for 3D-scene draws,
    // confirms D3D-convention clip space is what's expected here) -- a
    // range wildly outside that, or a majority of vertices ending up
    // behind the eye point (clip_w <= 0, unable to divide), is the same
    // kind of decisive "not supported" signal the thirteenth follow-up's
    // -4192 result was, just computed automatically instead of by hand.
    //
    // Round 20, eighteenth follow-up: fable2_121.log's own data showed
    // this gated on cmd.world_transform_applied too strictly -- this
    // investigation's actual leading perspective candidate (c[72],
    // ucode_hash=0x129f7f2793da9084/0xd4d558da6a82bdc8, near~0.1/
    // far~5000/fovY~60 deg, see native-rendering-phase3.md round 20,
    // seventeenth follow-up) has now been seen on THREE separate
    // sessions, but its own draw almost never also contains a
    // structurally-clean 3-row rigid block in the exact shape
    // FindRigidTransformCandidate requires (possibly because this
    // shader's own world placement isn't a plain rotation+translation,
    // or it genuinely doesn't need one) -- so the strict gate would
    // likely NEVER get to test our actual best lead. Now runs
    // unconditionally whenever cmd.valid, composing whatever cmd.vertices
    // currently holds -- world-space when cmd.world_transform_applied is
    // true, otherwise raw local/guest space -- with that distinction
    // spelled out explicitly in the log line itself (not left implicit)
    // so a local-space result is never misread as equivalent to a
    // world-space one: a local-space pass is weaker evidence (local
    // space can coincidentally sit near the origin for reasons that have
    // nothing to do with the camera), but a local-space FAIL is just as
    // informative a rejection as a world-space one, and this was
    // previously getting zero data at all for exactly the draws that
    // matter most.
    if (cmd.valid) {
      for (int i = 0; i < proj_count; ++i) {
        const ProjectionCandidate& pc = proj_candidates[i];
        if (pc.is_orthographic) continue;
        // Round 20, twenty-fourth follow-up: skip known rigid-block
        // overlap artifacts here too -- see this round's comment above
        // FindProjectionCandidates's call site for the full reasoning
        // (fable2_125.2.log's 0x475ec9f795e5edbb candidate composed to a
        // falsely "plausible"-looking NDC range purely because dividing
        // real geometry by roughly its own z tends to look bounded
        // regardless of whether the matrix means anything).
        if (proj_overlaps_rigid[i]) {
          REXLOG_INFO(
              "[NativeDrawPoc] [ViewProjectionSearch] [AutoVerify] slot=31 constC addr={:#x} "
              "ucode_hash={:#018x}: SKIPPED PROJECTION candidate at c[{}] -- overlaps_rigid_block "
              "(see round 20, twenty-fourth follow-up): composing it would test the SAME "
              "registers as an already-found rigid transform, not an independent candidate, and "
              "can produce a misleadingly plausible-looking result by ordinary geometric "
              "coincidence",
              info.address, info.ucode_hash, pc.base_logical_index);
          continue;
        }
        float min_ndc_x = std::numeric_limits<float>::infinity();
        float max_ndc_x = -std::numeric_limits<float>::infinity();
        float min_ndc_y = std::numeric_limits<float>::infinity();
        float max_ndc_y = -std::numeric_limits<float>::infinity();
        float min_w = std::numeric_limits<float>::infinity();
        float max_w = -std::numeric_limits<float>::infinity();
        int behind_or_degenerate = 0;
        // Round 20, twenty-fourth follow-up: fable2_125.1.log's own
        // leading-candidate result (ndc_x=[-8.787e+04,1583]) showed the
        // raw min/max is fragile -- a SINGLE vertex with clip_w just
        // barely above the 1e-6 epsilon (2.891e-06 there) divides down
        // to an enormous, meaningless NDC value and blows up the whole
        // aggregate range, drowning out whatever the other vertices
        // actually did. Kept (still logged, for transparency) alongside
        // a resilient count: how many vertices land with BOTH |ndc_x|
        // and |ndc_y| within a generous [-2,2] window, out of how many
        // were dividable at all -- this is what plausibility should
        // actually be judged by, not the raw range alone.
        int plausible_ndc_count = 0;
        for (int v = 0; v < cmd.vertex_count; ++v) {
          const fable2::native_render::NativeVertex& p = cmd.vertices[v];
          const float clip_x =
              p.x * pc.rows[0][0] + p.y * pc.rows[0][1] + p.z * pc.rows[0][2] + pc.rows[0][3];
          const float clip_y =
              p.x * pc.rows[1][0] + p.y * pc.rows[1][1] + p.z * pc.rows[1][2] + pc.rows[1][3];
          const float clip_w =
              p.x * pc.rows[3][0] + p.y * pc.rows[3][1] + p.z * pc.rows[3][2] + pc.rows[3][3];
          if (!(clip_w > 1e-6f)) {
            ++behind_or_degenerate;
            continue;
          }
          const float ndc_x = clip_x / clip_w;
          const float ndc_y = clip_y / clip_w;
          min_ndc_x = std::min(min_ndc_x, ndc_x);
          max_ndc_x = std::max(max_ndc_x, ndc_x);
          min_ndc_y = std::min(min_ndc_y, ndc_y);
          max_ndc_y = std::max(max_ndc_y, ndc_y);
          min_w = std::min(min_w, clip_w);
          max_w = std::max(max_w, clip_w);
          if (std::fabs(ndc_x) <= 2.0f && std::fabs(ndc_y) <= 2.0f) ++plausible_ndc_count;
        }
        {
          std::lock_guard<std::mutex> vp_lock(g_draw_mutex);
          g_vp_watch.autoverify_seen = true;
          g_vp_watch.autoverify_hash = info.ucode_hash;
          g_vp_watch.autoverify_base = pc.base_logical_index;
          g_vp_watch.autoverify_world_space = cmd.world_transform_applied;
          g_vp_watch.autoverify_ndc_x_min = min_ndc_x;
          g_vp_watch.autoverify_ndc_x_max = max_ndc_x;
          g_vp_watch.autoverify_ndc_y_min = min_ndc_y;
          g_vp_watch.autoverify_ndc_y_max = max_ndc_y;
          g_vp_watch.autoverify_behind_or_degenerate = behind_or_degenerate;
          g_vp_watch.autoverify_vertex_count = cmd.vertex_count;
          g_vp_watch.autoverify_plausible_ndc_count = plausible_ndc_count;
          g_vp_watch.autoverify_time = std::chrono::steady_clock::now();
        }
        REXLOG_INFO(
            "[NativeDrawPoc] [ViewProjectionSearch] [AutoVerify] slot=31 constC addr={:#x} "
            "ucode_hash={:#018x}: composed {} vertices (world_transform_applied={} -- true = "
            "already-world-space, false = raw local/guest space, see this block's own comment "
            "for why a false-here result is weaker positive evidence but still a fully valid "
            "rejection) through PROJECTION candidate at c[{}] (out[i]=v.row_i, perspective "
            "divide by row3.v) -> plausible_ndc_count(|x|,|y|<=2)={}/{} ndc_x=[{:.4g},{:.4g}] "
            "ndc_y=[{:.4g},{:.4g}] clip_w=[{:.4g},{:.4g}] behind_or_degenerate={}/{} -- judge by "
            "plausible_ndc_count, not the raw ndc_x/ndc_y range alone (a single near-zero-clip_w "
            "vertex can blow that up arbitrarily far -- see round 20's twenty-fourth follow-up); "
            "a low plausible_ndc_count or a majority behind_or_degenerate is the same kind of "
            "decisive rejection round 20's thirteenth follow-up found by hand for a different, "
            "unrelated c[72] lead -- see native-rendering-phase3.md round 20, "
            "seventeenth/eighteenth/twenty-fourth follow-ups",
            info.address, info.ucode_hash, cmd.vertex_count, cmd.world_transform_applied,
            pc.base_logical_index, plausible_ndc_count, cmd.vertex_count - behind_or_degenerate,
            min_ndc_x, max_ndc_x, min_ndc_y, max_ndc_y, min_w, max_w, behind_or_degenerate,
            cmd.vertex_count);
      }
    }
  }

  // Round 19: once locked, stop updating the overlay-visible snapshot --
  // see g_native_constC_locked's own comment above.
  if (g_native_constC_locked.load(std::memory_order_acquire)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_fetch_const_vertex = info;
    g_have_fetch_const_vertex = true;
  }

  // Round 19, first-build follow-up: fable2_104.1.log caught a real bug
  // here -- the first plausible_magnitude=true capture that session
  // happened to be the 192-byte/12-attribute shader (0x695413a9831d88da),
  // whose 224-byte snapshot cap only ever fits ONE full record
  // (224/192==1 by integer division), so it latched with
  // decoded_vertex_count=1 -- below ImGuiDrawListBackend::SubmitDrawCommand's
  // own `vertex_count < 3` early-out, meaning nothing ever actually drew.
  // plausible_magnitude alone only checks x/y value RANGE, not whether
  // there's enough of it to form a triangle -- add that check explicitly,
  // matching the backend's own real requirement exactly, so a latch can
  // only ever happen on a capture that will actually render. The same
  // 104.1.log had several later captures with vertex_count 6-11 that
  // would have worked (including x/y max_abs=9.46094/min_abs_nonzero=
  // 1.70801 -- the known 0xfbd39c64463e180b, 36-byte/2-attribute case
  // referenced throughout this round's writeup) -- this fix lets the
  // latch skip past the bad one and land on one of those instead.
  if (plausible && cmd.vertex_count >= 3) {
    bool expected = false;
    if (g_native_constC_locked.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      // Round 20, thirteenth follow-up (view/projection reconstruction --
      // native-rendering-phase3.md): fixed a real, evidenced bug in the
      // log line below. It previously hardcoded "no index buffer or real
      // transform is used for this stream" UNCONDITIONALLY, left over
      // verbatim from round 19 when that was still always true -- but
      // round 20's fourth/fifth-build follow-ups added real index/
      // transform support to this same code path (used_real_indices/
      // world_transform_applied on cmd, set dynamically per-draw), and
      // this specific message was never updated to match. Caught by
      // direct evidence, not inspection alone: fable2_117.3.log's own
      // LATCHED draw (round 20, thirteenth follow-up) logged this exact
      // static text while its OWN VertexDump line, two log lines later,
      // correctly showed world_transform_applied=true, and its own
      // IndexCapture line showed used_real_indices=true -- a direct,
      // in-the-same-log contradiction. Fixed to report the real,
      // dynamic cmd fields instead of a stale hardcoded claim.
      REXLOG_INFO(
          "[NativeDrawPoc] [NativeGeometryMilestone] LATCHED -- this is now the one validated "
          "constC vertex stream the F4 overlay's native-geometry section will show, permanently, "
          "until the process restarts. seq={} guest_frame={} addr={:#x} ucode_hash={:#018x} "
          "stride_bytes={} attribute_count={} decoded_vertex_count={} topology={} "
          "used_real_indices={} world_transform_applied={} -- decoded via ReX's own "
          "AnalyzeUcode()-reported attribute[0] format/offset (validated, not guessed), "
          "submitted through the existing ImGuiDrawListBackend (see native_draw_repr.h) -- see "
          "BuildDrawCommandFromValidatedFetchConstant's own comment for how used_real_indices/"
          "world_transform_applied are determined, and native-rendering-phase3.md's round "
          "19/20 writeups for the full reasoning. Cross-reference this seq/guest_frame against "
          "FableDrawCapture's own CSV/log rows to identify exactly which Fable draw this came "
          "from.",
          info.sequence, info.guest_frame, info.address, info.ucode_hash, info.stride_words * 4u,
          info.attribute_count, cmd.vertex_count,
          cmd.topology == fable2::native_render::PrimitiveTopology::kTriangleList ? "triangle_list"
                                                                                    : "unknown",
          cmd.used_real_indices, cmd.world_transform_applied);
      // Round 19, third-build follow-up (fable2_107*.log): the overlay's
      // canvas rendered empty even with a locked, vertex_count>=3,
      // plausible_magnitude=true capture -- and plausible_magnitude only
      // ever checked max_abs/min_abs_nonzero of std::fabs(x)/std::fabs(y),
      // which silently ignores a NaN component (NaN compares false to
      // everything, so it can never become the new max or min and would
      // never show up in that aggregate). Dump every decoded vertex's raw
      // x/y/z plus an explicit finite/NOT-FINITE flag right here, once, at
      // the exact moment this capture becomes the permanent one the
      // overlay shows -- ground truth for whether this is a NaN/Inf
      // problem, a degenerate-clustering problem, or something else,
      // instead of guessing from the aggregate stats alone.
      // Round 20: cmd.world_transform_applied tells us whether the numbers
      // below are still raw local/guest-space bytes (as every prior round's
      // VertexDump always was) or have had a structurally-validated rigid
      // transform applied (see FindRigidTransformCandidate) -- logged once
      // here so this is unambiguous from the log alone, not just inferred.
      REXLOG_INFO(
          "[NativeDrawPoc] [NativeGeometryMilestone] [VertexDump] world_transform_applied={} "
          "(false = still raw local/guest-space bytes, true = a structurally-validated rigid "
          "transform from this draw's own float constants has been applied -- see phase3.md "
          "round 20)",
          cmd.world_transform_applied);
      for (int vi = 0; vi < cmd.vertex_count; ++vi) {
        const fable2::native_render::NativeVertex& v = cmd.vertices[vi];
        const bool finite = std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        REXLOG_INFO(
            "[NativeDrawPoc] [NativeGeometryMilestone] [VertexDump] v[{}] = ({:.6g}, {:.6g}, "
            "{:.6g}) {}",
            vi, v.x, v.y, v.z, finite ? "finite" : "NOT FINITE (nan/inf)");
      }

      // Round 20, fourth-build follow-up (native-rendering-phase3.md):
      // cmd.used_real_indices tells us whether the vertex order above came
      // from this draw's own real, captured index buffer (fixing the
      // fable2_110.log outliers) or from the prior blind-sequential
      // fallback -- see ValidatedFetchConstantVertexInfo::index_data_valid
      // and BuildDrawCommandFromValidatedFetchConstant's own comments.
      // info.index_data_valid/index_count/index_is_32bit are the raw
      // capture-side fields even when used_real_indices ended up false
      // (e.g. every real index pointed past this capture's fixed-size
      // vertex snapshot window), so a "captured but unusable" case stays
      // visible in the log instead of looking identical to "no index data
      // captured at all".
      REXLOG_INFO(
          "[NativeDrawPoc] [NativeGeometryMilestone] [IndexCapture] used_real_indices={} "
          "index_data_valid={} index_addr={:#x} index_count={} index_is_32bit={} "
          "index_snapshot_bytes_captured={}",
          cmd.used_real_indices, info.index_data_valid, info.index_addr, info.index_count,
          info.index_is_32bit, info.index_snapshot_bytes_captured);

      // Round 20 (Fable2PC, native 3D draw path): the same shader's raw
      // float constant register values, captured alongside the geometry
      // above in fable_draw_capture.cpp's LogActiveVertexShaderBindingForSlot()
      // (this round's own addition -- see CommandProcessor::register_file(),
      // a new SDK accessor). Logged here too, grouped with this exact
      // winning draw's VertexDump, so both halves of "geometry + transform"
      // for the SAME draw are in one place. Still raw data only: no claim
      // is made about which of these (if any) is a world/view/projection
      // matrix -- see ValidatedFetchConstantVertexInfo::float_constant_*'s
      // own comment.
      REXLOG_INFO(
          "[NativeDrawPoc] [NativeGeometryMilestone] [FloatConstants] captured={} float4 "
          "registers for this shader (raw, no semantic claim -- see phase3.md round 20)",
          info.float_constant_count);
      for (int fi = 0; fi < info.float_constant_count; ++fi) {
        REXLOG_INFO(
            "[NativeDrawPoc] [NativeGeometryMilestone] [FloatConstants] c[{}] = ({:.6g}, {:.6g}, "
            "{:.6g}, {:.6g})",
            info.float_constant_logical_index[fi], info.float_constant_value[fi][0],
            info.float_constant_value[fi][1], info.float_constant_value[fi][2],
            info.float_constant_value[fi][3]);
      }
    }
  }
}

void OnValidatedDraw(uint8_t* base, const ValidatedDrawInfo& info) {
  // One-shot: the first fully-validated draw this module sees after being
  // enabled is the one the overlay shows, permanently, until the process
  // restarts. This is a proof of concept ("first milestone is visible
  // output"), not a continuous per-frame renderer.
  bool expected = false;
  if (!g_have_draw.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
    return;
  }

  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_draw = info;
  }

  // Phase 4 follow-up (round 5): armed FIRST, before any of the diagnostics
  // below, so the window between allocation and the real write is covered
  // as tightly as possible -- see ArmWriteWatch()'s own comment. Round 15,
  // twelfth follow-up: also gated on fable2_native_poc_enable_write_watch
  // (default false) -- see that cvar's own comment for why.
  if (info.resource_calls_captured && REXCVAR_GET(fable2_native_poc_enable_write_watch)) {
    ArmWriteWatch(info.vertex_call_return_addr, 64);
  }

  const bool index32 = REXCVAR_GET(fable2_native_poc_index32);
  // Round 6 fix: this was unconditionally REXCVAR_GET(fable2_native_poc_vertex_stride)
  // -- a GUESSED cvar defaulting to 32 -- even though every round since round 1 has
  // directly CAPTURED the real stride sub_82217EE8 itself passed to sub_82206820
  // (info.vertex_call_stride_arg, confirmed 16 on every observed call). Ground truth
  // over guess, exactly like payload[2] above: use the captured value whenever it's
  // available, and fall back to the cvar only when resource_calls_captured is false
  // (so this module still degrades gracefully rather than dividing by/indexing with 0).
  const uint32_t stride =
      (info.resource_calls_captured && info.vertex_call_stride_arg != 0)
          ? info.vertex_call_stride_arg
          : std::max<uint32_t>(REXCVAR_GET(fable2_native_poc_vertex_stride), 4u);

  // Round 9 fix (native-rendering-phase3.md): payload[2]/VGT_DMA_BASE is
  // the *actual* captured packet field (Phase 2 attempt 3 confirmed its
  // VALUE byte-exact, 500/500) -- but a live capture this round proved it
  // is in CANONICAL/physical guest-address space, not the ALIAS space
  // REX_LOAD_U32/REX_RAW_ADDR expect (the same alias space
  // vertex_call_return_addr/vertex_addr_raw already successfully reads
  // from). Evidence: LogAddressDiagnostics's own GetPhysicalAddress()
  // translated candidate A (vertex_call_return_addr, alias, readable) to a
  // canonical physical_addr exactly 0x50 bytes below payload[2] -- and
  // applying that SAME alias<->canonical delta to payload[2] lands exactly
  // on info.index_call_return_addr (self+13452's own raw contents, alias
  // space, captured directly from sub_82206820 call #2's return value,
  // the literal index-side counterpart of vertex_call_return_addr). So
  // payload[2] and index_call_return_addr are the SAME memory location in
  // two different address spaces, and the previous "use payload[2]
  // directly" choice was reading a physical-space number through an
  // alias-space accessor -- a likely root cause of every prior round's
  // decode faulting immediately "at vertex 0" (the INDEX read, not the
  // vertex read, per DumpRawDwords independently proving vertex_addr_raw
  // itself readable both before and after population). Prefer the
  // directly-captured, alias-space index_call_return_addr for the actual
  // guest-memory read -- exactly the same "ground truth over derived
  // value" preference already applied to stride above -- and fall back to
  // payload[2] only when resource_calls_captured is false.
  //
  // Round 15 follow-up: that fallback used to be "payload[2] directly,
  // degraded but still attempts something" -- known, at the time, to be
  // handing a canonical/physical-space number to an alias-space reader.
  // fable2_058/059/060.log's repeated "index_addr ... skipped --
  // IsGuestRangeSafeToRead()=false" results are exactly what that produces
  // (a canonical address is essentially never going to look like a valid,
  // committed region to a lookup that only knows the alias heap). Now
  // converted through CanonicalPhysicalToAlias() (see its own comment
  // above for the derivation) so this fallback actually lands in the same
  // address space as index_call_return_addr would have, instead of merely
  // attempting a read that was never going to succeed.
  const uint32_t index_addr =
      (info.resource_calls_captured && info.index_call_return_addr != 0)
          ? info.index_call_return_addr
          : CanonicalPhysicalToAlias(info.payload[2]);

  // Phase 4 (native-rendering-phase3.md): no packet field carries the
  // vertex-fetch address (DRAW_INDX doesn't encode it), and the runtime
  // regression this session fixed proved the formula-transformed candidate
  // can produce an unreadable address for a real captured draw. Per
  // explicit instruction, this module does not guess a replacement formula;
  // it tries BOTH plausible candidates and reports both as evidence:
  //   candidate A: vertex_addr_raw used AS-IS (the raw sub_82206820 return
  //                value, self+13448, before any transform)
  //   candidate B: PredictScratchAddressField(vertex_addr_raw), the formula
  //                validated only for the INDEX case, applied here by
  //                analogy (the pre-existing, now-known-fallible guess)
  const uint32_t candidate_addr_raw = info.vertex_addr_raw;
  const uint32_t candidate_addr_formula = PredictScratchAddressField(info.vertex_addr_raw);

  // Round 12 (native-rendering-phase3.md): ground truth over guess, the
  // same philosophy as the index_addr fix (round 9) and the stride fix
  // (round 6) above. info.index_count is the RAW ENTRY-ARGUMENT value from
  // whichever of the nine draw-flush-family functions produced this draw --
  // and round 12's hand-trace of a second sibling (sub_8221C518) proved the
  // index-count argument is NOT in the same register position for every
  // sibling (arg4/r7 for sub_82217EE8, arg3/r6 for sub_8221C518), so
  // trusting it blindly would silently mis-size this decode's loop bound for
  // any hooked sibling whose argument position hasn't been hand-traced.
  // info.payload[3] (VGT_DMA_SIZE) is the REAL, already-scanned DRAW_INDX
  // packet's own index-count field (bits 0-23 -- see PredictVgtDmaSize) --
  // always populated whenever OnValidatedDraw runs at all (FableDrawCapture
  // only ever hands off after its own scan.found), independent of which
  // sibling function or argument convention produced it. Falls back to
  // info.index_count only in the never-observed case payload[3]'s low bits
  // are exactly zero.
  const uint32_t index_count_from_payload = info.payload[3] & 0xFFFFFFu;
  const int index_count = std::clamp<int>(
      index_count_from_payload != 0 ? static_cast<int>(index_count_from_payload)
                                     : static_cast<int>(info.index_count),
      0, kMaxDecodedIndices);

  // Round 15, ninth follow-up (native-rendering-phase3.md): freeze the
  // actual guest bytes for both vertex candidates and the index buffer
  // RIGHT NOW, before anything else (another draw, another frame) gets a
  // chance to overwrite this address -- see MemorySnapshot's own comment
  // for the fable2_066.log evidence that later re-reads of this same
  // address are NOT trustworthy once resource_calls_captured is false.
  // Every decode below (the initial one, the automatic sweep, and every
  // later manual re-decode/sweep the user triggers from the overlay) now
  // reads from this one frozen copy instead of re-touching live memory.
  MemorySnapshot snapshot =
      CaptureMemorySnapshot(base, index_addr, candidate_addr_raw, candidate_addr_formula);
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_snapshot = snapshot;
  }

  DecodeAttempt decode_raw =
      DecodeCandidateFromSnapshot(snapshot, /*use_formula_vertex=*/false, index32, stride,
                                   index_count);
  DecodeAttempt decode_formula =
      DecodeCandidateFromSnapshot(snapshot, /*use_formula_vertex=*/true, index32, stride,
                                   index_count);

  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_decode_raw = decode_raw;
    g_decode_formula = decode_formula;
  }

  // Round 6: save these exact parameters for TickAddressWatch()'s deferred
  // re-decode once population is observed -- see DeferredDecodeParams above.
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_deferred_decode_params.valid = true;
    g_deferred_decode_params.index_addr = index_addr;
    g_deferred_decode_params.index32 = index32;
    g_deferred_decode_params.stride = stride;
    g_deferred_decode_params.index_count = index_count;
    g_deferred_decode_params.candidate_addr_raw = candidate_addr_raw;
    g_deferred_decode_params.candidate_addr_formula = candidate_addr_formula;
  }

  // Round 15, eighth follow-up: automatically sweep a range of stride
  // guesses against this same capture right now, so the overlay/log
  // already shows which stride(s) are worth trying by hand instead of
  // starting from nothing (or from a single fixed default). Round 15,
  // ninth follow-up: now reads the frozen snapshot above, not live memory
  // -- see SweepStridesFromSnapshot()'s own comment.
  StrideSweepResult sweep_raw =
      SweepStridesFromSnapshot(snapshot, /*use_formula_vertex=*/false, index32, index_count);
  StrideSweepResult sweep_formula =
      SweepStridesFromSnapshot(snapshot, /*use_formula_vertex=*/true, index32, index_count);
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_sweep_raw = sweep_raw;
    g_sweep_formula = sweep_formula;
  }
  {
    std::string hits_raw, hits_formula;
    for (int i = 0; i < sweep_raw.plausible_count; ++i) {
      hits_raw += std::to_string(sweep_raw.plausible_strides[i]) + " ";
    }
    for (int i = 0; i < sweep_formula.plausible_count; ++i) {
      hits_formula += std::to_string(sweep_formula.plausible_strides[i]) + " ";
    }
    REXLOG_INFO(
        "[NativeDrawPoc] auto stride sweep (candidate A, {} strides tried [{}..{} step {}]): "
        "plausible at: {}",
        sweep_raw.candidates_tried, kStrideSweepMin, kStrideSweepMax, kStrideSweepStep,
        hits_raw.empty() ? "(none)" : hits_raw.c_str());
    REXLOG_INFO(
        "[NativeDrawPoc] auto stride sweep (candidate B, {} strides tried [{}..{} step {}]): "
        "plausible at: {}",
        sweep_formula.candidates_tried, kStrideSweepMin, kStrideSweepMax, kStrideSweepStep,
        hits_formula.empty() ? "(none)" : hits_formula.c_str());
  }

  // Phase 4 evidence: the directly-captured sub_82206820 resource-call
  // arguments/return values for this specific draw (not another guessed
  // formula -- see fable_draw_capture.cpp's arm/disarm around
  // __imp__sub_82217EE8), plus the self+13516 branch-gate field read fresh
  // at hand-off time. Logged as raw evidence; no claim about vertex format
  // or shader identity is made here or anywhere else in this module.
  REXLOG_INFO(
      "[NativeDrawPoc] captured seq={} frame={} prim_type={} index_count={} index_addr={:#010x} "
      "(payload[2]/VGT_DMA_BASE, canonical space, for cross-check)={:#010x} "
      "stream_binding_ptr(self+13516)={:#010x} resource_calls_captured={}",
      info.sequence, info.guest_frame, info.prim_type, info.index_count, index_addr,
      info.payload[2], info.stream_binding_ptr, info.resource_calls_captured);
  if (info.resource_calls_captured) {
    REXLOG_INFO(
        "[NativeDrawPoc] sub_82206820 call #1 (feeds vertex_addr_raw/self+13448): "
        "count_arg={} stride_arg={} return_addr={:#010x}",
        info.vertex_call_count_arg, info.vertex_call_stride_arg, info.vertex_call_return_addr);
    REXLOG_INFO(
        "[NativeDrawPoc] sub_82206820 call #2 (feeds index_addr_raw/self+13452): "
        "count_arg={} stride_arg={} return_addr={:#010x}",
        info.index_call_count_arg, info.index_call_stride_arg, info.index_call_return_addr);
  } else {
    // Round 15: no longer necessarily a bug. Hand-off is now gated on
    // scan.found alone (see fable_draw_capture.cpp's CaptureAndValidate), so
    // this branch is the EXPECTED, common case for a high-frequency caller
    // that reused an already-bound vertex/index buffer without re-invoking
    // sub_82206820 for this specific call (self+13448/self+13452 still hold
    // the correct, currently-bound addresses either way). Logged at INFO,
    // not WARN, since it no longer implies the capture hook mis-armed.
    REXLOG_INFO(
        "[NativeDrawPoc] seq={} frame={}: resource_calls_captured=false (this call reused an "
        "already-bound buffer rather than re-resolving one, or the hook did not observe both "
        "sub_82206820 calls) -- falling back to payload[2]/cvar stride for decode",
        info.sequence, info.guest_frame);
  }

  REXLOG_INFO(
      "[NativeDrawPoc] candidate A (raw, vertex_addr_raw as-is)      addr={:#010x} decode_ok={}{}",
      candidate_addr_raw, decode_raw.plausible,
      decode_raw.plausible ? "" : (std::string(" (") + decode_raw.error + ")").c_str());
  REXLOG_INFO(
      "[NativeDrawPoc] candidate B (formula-transformed, by analogy) addr={:#010x} decode_ok={}{}",
      candidate_addr_formula, decode_formula.plausible,
      decode_formula.plausible ? "" : (std::string(" (") + decode_formula.error + ")").c_str());

  // Round 15 follow-up: log the first few raw index values a plausible
  // decode actually pulled from the index buffer (see DecodedVertex::
  // raw_index's comment for why -- distinguishing "the indices themselves
  // are sane, small numbers" from "the indices are already garbage" is
  // what fable2_061.log's screenshot needs to move past "decode_ok=true
  // but some vertices are absurd floats"). Only when plausible=true, since
  // an implausible decode's verts[]/raw_index[] were never even filled in
  // for a real read (read_fault) or are already flagged unreliable.
  auto LogRawIndices = [](const char* label, const DecodeAttempt& decode) {
    if (!decode.plausible) return;
    const int n = std::min(decode.count, 16);
    std::string indices;
    for (int i = 0; i < n; ++i) {
      if (i != 0) indices += ", ";
      indices += std::to_string(decode.verts[i].raw_index);
    }
    REXLOG_INFO("[NativeDrawPoc] {} raw index values [0..{}) of {}: {}", label, n, decode.count,
                indices);
  };
  LogRawIndices("candidate A", decode_raw);
  LogRawIndices("candidate B", decode_formula);

  // Phase 4 follow-up (this pass): explain what each candidate address
  // actually IS to rex's own guest-memory system, instead of guessing yet
  // another transform. See LogAddressDiagnostics() above.
  LogAddressDiagnostics("candidate A (raw)", candidate_addr_raw);
  LogAddressDiagnostics("candidate B (formula)", candidate_addr_formula);

  // Phase 4 follow-up (round 2): round 2's LogAddressDiagnostics run showed
  // candidate A's raw address IS backed by a committed, Read|Write-able
  // alias region per rex's own QueryRegionInfo -- yet the index/stride-
  // derived decode above still reports a fault. This dumps raw dwords
  // directly at sub_82206820 call #1's own return value (the same address
  // as candidate A, named here for exactly what it is -- "call #1's actual
  // allocation", no index or stride involved) to settle, independent of
  // TryDecodeVertex's own ambiguity, whether THIS address is actually
  // readable via REX_LOAD_U32 in practice.
  //
  // Round 15 fix + follow-up: info.vertex_call_return_addr is only ever
  // meaningful when info.resource_calls_captured is true (see
  // fable_native_poc.h's struct comment -- when false, this field is
  // documented as zero, and dumping it unconditionally is exactly what
  // crashed fable2_057.log's very first sub_8221E0F0 capture with
  // "Unhandled guest access violation: read of guest 0x00000000"). Fixed by
  // switching this probe to candidate_addr_raw instead: per round 4's own
  // finding, candidate_addr_raw (== vertex_addr_raw) IS the same address as
  // vertex_call_return_addr whenever resource_calls_captured is true, so
  // nothing is lost there -- and unlike vertex_call_return_addr,
  // candidate_addr_raw is always-populated ground truth (read fresh from
  // self+13448 regardless of whether this specific call refreshed it), so
  // this probe now runs unconditionally and safely, and usefully covers the
  // resource_calls_captured=false case too (fable2_058.log's sub_8221E0F0
  // capture, index_count=7729, was exactly this case, and TryDecodeVertex's
  // own "faulted at vertex 0" message alone couldn't say whether the index
  // read or the vertex read was the actual culprit).
  // Round 15 follow-up (defense in depth): fable2_057.log's crash was a
  // read of guest 0x00000000 that DumpRawDwords's own __try/__except did
  // NOT recover from -- rex's memory system appears to treat exactly-null
  // (or near-null) guest reads as a special, deliberately-fatal case (its
  // own AccessViolationCallback comment elsewhere in this file already
  // describes this class of read as "unrecoverable, process-fatal"),
  // unlike an ordinary invalid-but-nonzero address, which fable2_057.log
  // itself separately proved DOES get caught normally (two earlier
  // "Unhandled guest access violation" lines at 0x1CBB9F40 were followed by
  // a clean "skipped safely" recovery). So beyond gating on
  // resource_calls_captured/using always-valid ground truth, this also
  // never attempts the read at all when the address is exactly zero --
  // belt and suspenders, since a zero address carries no useful diagnostic
  // value anyway.
  const bool candidate_a_safe_to_read =
      candidate_addr_raw != 0 && IsGuestRangeSafeToRead(candidate_addr_raw, kRawDumpDwords * 4u);
  RawDumpResult raw_dump_candidate_a{};
  if (candidate_a_safe_to_read) {
    raw_dump_candidate_a = DumpRawDwords(base, candidate_addr_raw);
  }
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_raw_dump_call1 = raw_dump_candidate_a;
  }
  if (!candidate_a_safe_to_read) {
    REXLOG_INFO(
        "[NativeDrawPoc] raw dword dump at candidate A/vertex_addr_raw ({:#010x}): skipped -- "
        "IsGuestRangeSafeToRead()=false (not committed/readable, or the {}-byte span doesn't "
        "fit inside rex's reported region) -- see round 15 follow-up",
        candidate_addr_raw, kRawDumpDwords * 4u);
  } else if (raw_dump_candidate_a.fault) {
    REXLOG_WARN(
        "[NativeDrawPoc] raw dword dump at candidate A/vertex_addr_raw ({:#010x}) FAULTED -- "
        "this exact address is not actually readable via REX_LOAD_U32, despite "
        "QueryRegionInfo reporting the containing region as committed/readable",
        candidate_addr_raw);
  } else {
    REXLOG_INFO(
        "[NativeDrawPoc] raw dword dump at candidate A/vertex_addr_raw ({:#010x}): "
        "{:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x}",
        candidate_addr_raw, raw_dump_candidate_a.dwords[0], raw_dump_candidate_a.dwords[1],
        raw_dump_candidate_a.dwords[2], raw_dump_candidate_a.dwords[3],
        raw_dump_candidate_a.dwords[4], raw_dump_candidate_a.dwords[5],
        raw_dump_candidate_a.dwords[6], raw_dump_candidate_a.dwords[7]);
  }

  // Round 15 follow-up: a second, independent probe at index_addr (also
  // always-populated ground truth -- payload[2] or index_call_return_addr,
  // never the untrusted-when-uncaptured vertex_call_* fields) -- to
  // directly determine whether the INDEX side is readable at all,
  // independent of whatever candidate A/B's vertex-side probe above finds.
  // TryDecodeVertex's single __try block covers both the index read and
  // the vertex read, so its own fault report can't tell them apart; this
  // can.
  // Round 15 follow-up (fable2_059.log): same IsGuestRangeSafeToRead() gate
  // as candidate A's probe above, same reason -- this is exactly the probe
  // whose earlier, ungated version crashed unrecoverably on this exact kind
  // of address (the index side), immediately after two smaller, narrower
  // reads at the SAME address had already faulted and recovered cleanly.
  const bool index_addr_safe_to_read =
      index_addr != 0 && IsGuestRangeSafeToRead(index_addr, kRawDumpDwords * 4u);
  RawDumpResult raw_dump_index{};
  if (index_addr_safe_to_read) {
    raw_dump_index = DumpRawDwords(base, index_addr);
  }
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    g_raw_dump_index = raw_dump_index;
  }
  if (!index_addr_safe_to_read) {
    REXLOG_INFO(
        "[NativeDrawPoc] raw dword dump at index_addr ({:#010x}): skipped -- "
        "IsGuestRangeSafeToRead()=false (not committed/readable, or the {}-byte span doesn't "
        "fit inside rex's reported region) -- see round 15 follow-up",
        index_addr, kRawDumpDwords * 4u);
  } else if (raw_dump_index.fault) {
    REXLOG_WARN(
        "[NativeDrawPoc] raw dword dump at index_addr ({:#010x}) FAULTED -- the index buffer "
        "itself is not readable via REX_LOAD_U32 at this address",
        index_addr);
  } else {
    REXLOG_INFO(
        "[NativeDrawPoc] raw dword dump at index_addr ({:#010x}): "
        "{:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x} {:#010x}",
        index_addr, raw_dump_index.dwords[0], raw_dump_index.dwords[1],
        raw_dump_index.dwords[2], raw_dump_index.dwords[3], raw_dump_index.dwords[4],
        raw_dump_index.dwords[5], raw_dump_index.dwords[6], raw_dump_index.dwords[7]);
  }

  // Phase 4 follow-up (round 3): static tracing of call #1's own call tree
  // (sub_82217EE8 -> sub_82206820 -> sub_822065F0 -> sub_82206538) found no
  // write of vertex data anywhere in it -- see g_watch_active's comment
  // above. Rather than keep expanding the static call graph (out of scope
  // this round), arm a per-frame watch (TickAddressWatch(), driven by
  // NativePocOverlay::OnDraw() while F4 is open) that re-reads this same
  // address on every subsequent frame and reports the moment it changes.
  //
  // TickAddressWatch() cannot reuse the real `base` this function was
  // handed (it does not run inside the recompiled call graph, so no `base`
  // argument reaches it) -- it obtains its own via
  // rex::system::kernel_memory()->virtual_membase() instead. This ONE-TIME
  // check cross-validates that substitution directly, by comparing it
  // against the real `base` in scope right here, rather than assuming it:
  {
    rex::memory::Memory* mem = rex::system::kernel_memory();
    uint8_t* watch_base = mem ? mem->virtual_membase() : nullptr;
    REXLOG_INFO(
        "[NativeDrawPoc] watch: kernel_memory()->virtual_membase()={} vs this capture's real "
        "base={} -- match={}",
        static_cast<const void*>(watch_base), static_cast<const void*>(base),
        watch_base == base);
  }
  // Round 15 fix: same reasoning as the DumpRawDwords guard above -- arming
  // the watch on info.vertex_call_return_addr is only valid when
  // info.resource_calls_captured is true; otherwise that field is a
  // documented zero, and arming a per-frame TickAddressWatch() re-read of
  // guest address 0 would just move fable2_057.log's crash one frame later
  // instead of preventing it. When not captured, leave the watch inactive
  // (g_watch_active stays whatever it already was, cleared below) and
  // report why via the same g_raw_dump_call1 slot the overlay already reads.
  if (info.resource_calls_captured) {
    g_watch_addr.store(info.vertex_call_return_addr, std::memory_order_relaxed);
    g_watch_done.store(false, std::memory_order_relaxed);
    g_watch_active.store(true, std::memory_order_release);
  } else {
    g_watch_addr.store(0, std::memory_order_relaxed);
    g_watch_done.store(true, std::memory_order_relaxed);
    g_watch_active.store(false, std::memory_order_release);
  }
}

// Round 11 (native-rendering-phase3.md): the one-shot latch in
// OnValidatedDraw() above means the very first validated draw this process
// ever sees is the one this overlay shows, permanently, for the rest of the
// process's life. Round 10's milestone confirmed that first draw decodes
// cleanly (decode_ok=true) -- but round 11's own visual confirmation (the
// user's screenshot) shows x/y confined to {0, 0.5, 1} and the third
// component taking values like 720.0 and 1280.0 (HD screen-resolution
// numbers) alongside 0.5/1.0 in the very same column of the very same draw
// -- strong evidence this specific draw (guest_frame=7, essentially at
// process boot) is a 2D screen-space UI quad (a loading/splash element),
// not 3D world/character mesh data. DecodeLooksPlausible() only checks
// std::isfinite(), so it correctly reports decode_ok=true for this data
// without being able to tell 2D UI content from a 3D mesh.
//
// This lets the user manually request a FRESH single-draw capture once
// they are looking at real in-game content, without the higher-risk
// rewrite of turning this into a continuous multi-draw/per-frame capture
// (explicitly deferred in round 10). It resets every piece of one-shot
// state OnValidatedDraw()/ArmWriteWatch()/TickAddressWatch()/
// TickWriteWatch() maintain, in the order those systems themselves expect:
// unregister any live write-watch callback before clearing its handle;
// clear the deferred re-decode params before they could be re-read; flip
// g_have_draw last, since it is the single gate that lets OnValidatedDraw()
// run again.
static void RearmCapture(uint32_t source_filter) {
  // Round 13: set BEFORE g_have_draw goes false, so no hooked function can
  // observe a stale filter value on the very next call it makes.
  g_source_filter.store(source_filter, std::memory_order_relaxed);
  void* handle = nullptr;
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    handle = g_write_watch_callback_handle;
    g_write_watch_callback_handle = nullptr;
    g_deferred_decode_params.valid = false;
  }
  if (handle) {
    rex::memory::Memory* mem = rex::system::kernel_memory();
    if (mem) mem->UnregisterPhysicalMemoryInvalidationCallback(handle);
  }
  g_watch_active.store(false, std::memory_order_relaxed);
  g_watch_done.store(false, std::memory_order_relaxed);
  g_watch_addr.store(0, std::memory_order_relaxed);
  g_write_watch_reported.store(false, std::memory_order_relaxed);
  g_write_watch_fired.store(false, std::memory_order_relaxed);
  g_write_watch_armed_qpc.store(0, std::memory_order_relaxed);
  // Must be last: OnValidatedDraw()'s compare_exchange on g_have_draw is the
  // gate that lets it run again, and everything it (and the watch machinery
  // it arms) touches must already be reset before that happens.
  g_have_draw.store(false, std::memory_order_release);
  REXLOG_INFO(
      "[NativeDrawPoc] [Rearm] capture state reset by user request ""(source_filter={:#010x}) -- "
      "waiting for the next validated draw (guest_frame will be whatever "
      "the game is currently rendering, not necessarily 7 this time)",
      source_filter);
}

// Round 20 (native 3D draw path, native-rendering-phase3.md): declared
// (not defaulted) in the header so its implicit body -- which destroys
// d3d12_backend_, a unique_ptr<D3D12NativeRenderBackend> -- is emitted
// HERE, where fable_native_d3d12_backend.h (included above) makes that
// type complete, keeping the heavy D3D12 SDK header chain out of
// fable_native_poc.h itself. `= default` here still generates the same
// destructor body the implicit one would have; only its emission point
// changes.
// Also defined out-of-line, for the same reason as the destructor above:
// this trivial-looking constructor still needs
// fable2::native_render::D3D12NativeRenderBackend to be a complete type
// wherever it is instantiated, since it's implicitly responsible for
// unwinding (destroying) the d3d12_backend_ member if construction is
// ever left partway through. Fixed after a real build failure showed
// this was still broken with only the destructor moved out-of-line.
NativePocOverlay::NativePocOverlay(rex::ui::ImGuiDrawer* drawer)
    : ImGuiDialog(drawer) {}

NativePocOverlay::~NativePocOverlay() = default;

void NativePocOverlay::OnDraw(ImGuiIO& io) {
  (void)io;
  ImGui::SetNextWindowPos(ImVec2(12, 80), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(620, 660), ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowBgAlpha(0.85f);
  if (!ImGui::Begin("Fable2PC -- Phase 3/4 native draw PoC (F4)")) {
    ImGui::End();
    return;
  }

  if (!REXCVAR_GET(fable2_native_poc_enable)) {
    ImGui::TextUnformatted(
        "fable2_native_poc_enable is off. Set it to true (console or "
        "fable2.toml) and let the title render at least one frame to "
        "capture a draw.");
    ImGui::End();
    return;
  }

  // Phase 4 follow-up (round 3): this overlay already ticks once per frame
  // while open (F4), the natural place to drive the per-frame address watch
  // -- see TickAddressWatch()'s comment. A no-op until OnValidatedDraw has
  // armed it.
  TickAddressWatch();
  // Phase 4 follow-up (round 5): same per-frame vehicle, deferred half of
  // the write watch -- see TickWriteWatch()'s comment for why this, not
  // OnPhysicalMemoryInvalidated() itself, is where logging/unregistering
  // happens.
  TickWriteWatch();

  // Round 20, twenty-third follow-up (view/projection reconstruction --
  // native-rendering-phase3.md): live view/projection watch, rendered
  // first (right after the enable-check, before anything else) so it's
  // visible without scrolling and updates every frame this overlay is
  // open -- see ViewProjectionLiveWatch's own comment (near g_draw_mutex,
  // above) for why this exists: five real play sessions in a row never
  // reproduced the leading candidate, and blind "play more, upload
  // another log" stopped being a reasonable ask. This lets the person
  // watch it happen live instead.
  {
    ViewProjectionLiveWatch watch;
    {
      std::lock_guard<std::mutex> lock(g_draw_mutex);
      watch = g_vp_watch;
    }
    const auto now = std::chrono::steady_clock::now();
    ImGui::Separator();
    ImGui::TextColored(ImVec4(0.6f, 0.85f, 1.0f, 1.0f),
                        "View/projection live watch (round 20, twenty-third follow-up)");
    if (watch.target_shader_seen) {
      const float secs = std::chrono::duration<float>(now - watch.target_shader_time).count();
      const bool recent = secs < 5.0f;
      ImGui::TextColored(
          recent ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
          "  LEADING CANDIDATE'S SHADER SEEN -- ucode_hash=0x%016llX, %.1fs ago%s",
          static_cast<unsigned long long>(watch.target_shader_hash), secs,
          recent ? "  <<< HAPPENING NOW, STAY HERE" : "");
    } else {
      ImGui::TextColored(
          ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
          "  leading candidate's shader: not seen yet this process (seed hashes "
          "0x%016llX / 0x%016llX, OR any shader carrying the same c[72] near/far "
          "signature -- round 20, twenty-fifth follow-up)",
          static_cast<unsigned long long>(kLeadingCandidateHashes[0]),
          static_cast<unsigned long long>(kLeadingCandidateHashes[1]));
    }
    if (watch.perspective_seen) {
      const float secs = std::chrono::duration<float>(now - watch.perspective_time).count();
      const bool recent = secs < 5.0f;
      ImGui::TextColored(
          recent ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
          "  last kind=perspective candidate -- ucode_hash=0x%016llX c[%u], %.1fs ago%s",
          static_cast<unsigned long long>(watch.perspective_hash), watch.perspective_base, secs,
          recent ? "  <<< JUST NOW" : "");
    } else {
      ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                          "  no kind=perspective candidate seen yet this process");
    }
    if (watch.autoverify_seen) {
      const float secs = std::chrono::duration<float>(now - watch.autoverify_time).count();
      // Round 20, twenty-fourth follow-up: judge plausibility by
      // plausible_ndc_count (resilient to a single blown-up outlier
      // vertex), not the raw ndc_x/ndc_y min/max -- see that round's
      // comment above the AutoVerify loop for the fable2_125.1/.2.log
      // evidence that motivated this.
      const int dividable = watch.autoverify_vertex_count - watch.autoverify_behind_or_degenerate;
      const bool plausible = dividable > 0 &&
                              watch.autoverify_plausible_ndc_count * 2 >= dividable;
      ImGui::TextColored(
          plausible ? ImVec4(0.3f, 1.0f, 0.3f, 1.0f) : ImVec4(1.0f, 0.45f, 0.2f, 1.0f),
          "  last [AutoVerify]: ucode_hash=0x%016llX c[%u] world_space=%s "
          "plausible=%d/%d(dividable) ndc_x=[%.3g,%.3g] ndc_y=[%.3g,%.3g] behind/degenerate="
          "%d/%d, %.1fs ago -- %s",
          static_cast<unsigned long long>(watch.autoverify_hash), watch.autoverify_base,
          watch.autoverify_world_space ? "true" : "false", watch.autoverify_plausible_ndc_count,
          dividable, watch.autoverify_ndc_x_min, watch.autoverify_ndc_x_max,
          watch.autoverify_ndc_y_min, watch.autoverify_ndc_y_max,
          watch.autoverify_behind_or_degenerate, watch.autoverify_vertex_count, secs,
          plausible ? "PLAUSIBLE, most vertices near [-1,1]" : "rejected/out-of-range");
    } else {
      ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                          "  no [AutoVerify] composition result yet this process");
    }
    ImGui::Separator();
  }

  // Round 19 follow-up (visibility fix, from fable2_105.*.log): this
  // section used to sit further down this function, past the candidate-
  // A/B "if (!have_draw) { ...; return; }" early-out below -- see that
  // check's own comment. g_native_constC_locked is a completely separate,
  // independent one-shot latch from have_draw (which the "Re-arm" button
  // resets on every click), so being gated behind have_draw meant this
  // whole section -- including a fully latched, vertex_count>=3 milestone
  // triangle -- could vanish from view any time have_draw happened to be
  // false (e.g. right after a re-arm click, before the next draw lands),
  // and even when visible required scrolling past several unrelated
  // sections (filter radio buttons, round-11 text, candidate A/B dumps) to
  // reach. Moved here, right after the per-frame ticks and before any
  // have_draw-gated content, so it renders unconditionally -- subject only
  // to its own g_have_fetch_const_vertex guard, not this legacy path's
  // state -- every frame this overlay is open.
  ImGui::Separator();
  ImGui::TextWrapped(
      "Validated fetch-constant vertex stream (slot 31, const_index_sel=2 / \"constC\") -- a "
      "completely separate data source from candidates A/B above: not derived from this draw's "
      "DRAW_INDX payload at all, but from ReX's own Shader::AnalyzeUcode()-reported vertex "
      "binding for whatever shader was bound when this buffer was last snapshotted, read via "
      "the SDK's TranslatePhysical() memory path. See native-rendering-phase3.md round 28 for "
      "the honest coherence assessment: this has not yet decoded into visually coherent "
      "per-vertex records for every shader tried, so treat the plotted geometry below as raw "
      "decoded bytes, not confirmed real positions.");
  {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    if (!g_have_fetch_const_vertex || !g_fetch_const_vertex.valid) {
      ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                          "  no validated fetch-constant vertex stream captured yet");
    } else {
      const ValidatedFetchConstantVertexInfo snapshot = g_fetch_const_vertex;
      // Round 19: once g_native_constC_locked is set, this snapshot is the
      // SAME one every frame (see that flag's own comment) -- seq/guest_frame
      // identify exactly which Fable draw it came from, cross-referenceable
      // against FableDrawCapture's own log/CSV rows.
      const bool locked = g_native_constC_locked.load(std::memory_order_acquire);
      ImGui::Text(
          "  seq=%u guest_frame=%u addr=0x%08X ucode_hash=0x%016llX stride_words=%u "
          "attribute_count=%d snapshot_bytes=%u",
          snapshot.sequence, snapshot.guest_frame, snapshot.address,
          static_cast<unsigned long long>(snapshot.ucode_hash), snapshot.stride_words,
          snapshot.attribute_count, snapshot.snapshot_bytes_captured);
      if (locked) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                            "  LATCHED (round 19 milestone) -- this draw will not change again "
                            "this process; see [NativeGeometryMilestone] in the log for full "
                            "provenance");
      } else {
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                            "  not yet latched -- still updating every capture until the first "
                            "plausible_magnitude=true one arrives");
      }
      const fable2::native_render::NativeDrawCommand cmd =
          BuildDrawCommandFromValidatedFetchConstant(snapshot);
      ImGui::Text("  decoded vertex_count=%d (attribute[0] x/y, z only if attribute[0] uses it)",
                  cmd.vertex_count);
      // Round 20: whether the vertex numbers below are raw local/guest-space
      // bytes or have a structurally-validated rigid transform (rotation +
      // translation, from this draw's own float constants -- see
      // FindRigidTransformCandidate) already baked in.
      if (cmd.world_transform_applied) {
        ImGui::TextColored(
            ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "  world_transform_applied=true -- positions below are POST-transform (see "
            "phase3.md round 20; still no view/projection, so this is not full camera "
            "placement)");
      } else {
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                            "  world_transform_applied=false -- no structurally-validated rigid "
                            "transform found in this draw's captured float constants; positions "
                            "below are still raw local/guest-space bytes");
      }
      // Round 20, fourth-build follow-up (native-rendering-phase3.md):
      // whether the vertex order above came from this draw's own real,
      // captured index buffer (fixing the fable2_110.log outliers) or the
      // prior blind-sequential fallback -- see
      // ValidatedFetchConstantVertexInfo::index_data_valid's own comment.
      if (cmd.used_real_indices) {
        ImGui::TextColored(
            ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "  used_real_indices=true -- vertex order/selection from this draw's own captured "
            "index buffer (index_count=%u, %u-bit)",
            snapshot.index_count, snapshot.index_is_32bit ? 32u : 16u);
      } else if (snapshot.index_data_valid) {
        ImGui::TextColored(
            ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
            "  used_real_indices=false but index_data_valid=true -- indices were captured "
            "(index_count=%u, %u-bit) but every one pointed past this capture's fixed-size "
            "vertex snapshot window; falling back to blind sequential decode",
            snapshot.index_count, snapshot.index_is_32bit ? 32u : 16u);
      } else {
        ImGui::TextColored(ImVec4(0.75f, 0.75f, 0.75f, 1.0f),
                            "  used_real_indices=false, index_data_valid=false -- no real index "
                            "buffer captured for this draw (see [IndexCapture] in the log for "
                            "why); using blind sequential decode as before round 20's fourth "
                            "build follow-up");
      }
      // Round 19, third-build follow-up (fable2_107*.log): the canvas below
      // rendered empty even with a locked, plausible, vertex_count>=3
      // capture -- print the actual per-vertex numbers here too (not just
      // the log line -- see OnValidatedFetchConstantVertexStream's own
      // comment), matching candidate A/B's DrawCandidateSection style, so
      // this is visible directly in the overlay without needing a log
      // upload round-trip.
      for (int vi = 0; vi < cmd.vertex_count; ++vi) {
        const fable2::native_render::NativeVertex& v = cmd.vertices[vi];
        const bool finite = std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
        if (finite) {
          ImGui::Text("  v[%d] = (%.4f, %.4f, %.4f)", vi, v.x, v.y, v.z);
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f),
                              "  v[%d] = (%.4f, %.4f, %.4f) -- NOT FINITE (nan/inf)", vi, v.x, v.y,
                              v.z);
        }
      }
      // Round 20 (Fable2PC, native 3D draw path): this shader's captured
      // raw float constant register values, shown the same way as the
      // per-vertex dump above -- see
      // fable_draw_capture.cpp's LogActiveVertexShaderBindingForSlot() for
      // where these come from. Still no semantic claim about which (if
      // any) is a transform matrix.
      ImGui::Text("  float constants captured=%d (raw register data, no semantic claim yet)",
                  snapshot.float_constant_count);
      for (int fi = 0; fi < snapshot.float_constant_count; ++fi) {
        ImGui::Text("  c[%u] = (%.4f, %.4f, %.4f, %.4f)", snapshot.float_constant_logical_index[fi],
                    snapshot.float_constant_value[fi][0], snapshot.float_constant_value[fi][1],
                    snapshot.float_constant_value[fi][2], snapshot.float_constant_value[fi][3]);
      }
      ImGuiDrawListBackend backend;
      backend.SubmitDrawCommand(cmd);

      // Round 20 (Fable2PC, "native 3D draw path" -- native-rendering-
      // phase3.md): the FIRST real write into the live D3D12 pipeline
      // this investigation has made. Submits the SAME NativeDrawCommand
      // to a second, real-GPU backend -- see fable_native_d3d12_backend.h
      // for the full design and how it reaches the live command list
      // without ImDrawCmd::UserCallback or any binary hooking. The
      // ImGuiDrawListBackend call above is left completely unchanged and
      // keeps rendering unconditionally: this is additive, not a
      // replacement, per this round's own brief ("Preserve the existing
      // Xenos renderer as reference/fallback" -- extended here to also
      // mean "preserve the existing diagnostic 2D visualization").
      // Lazily constructed on first use since it needs imgui_drawer()
      // (only valid once this dialog is attached to one).
      ImGui::Checkbox("Also render via a real D3D12 draw call (round 20)",
                       &d3d12_backend_enabled_);
      if (d3d12_backend_enabled_) {
        if (!d3d12_backend_) {
          d3d12_backend_ =
              std::make_unique<fable2::native_render::D3D12NativeRenderBackend>(imgui_drawer());
        }
        d3d12_backend_->SubmitDrawCommand(cmd);
        if (d3d12_backend_->has_drawn_at_least_once()) {
          ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                              "  D3D12Backend: %s (orange triangle should be visible, real GPU "
                              "draw call, not ImGui's own fill)",
                              d3d12_backend_->last_status());
        } else {
          ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f), "  D3D12Backend: %s",
                              d3d12_backend_->last_status());
        }
      }
    }
  }

  ValidatedDrawInfo draw;
  DecodeAttempt decode_raw;
  DecodeAttempt decode_formula;
  RawDumpResult raw_dump_call1;
  RawDumpResult raw_dump_index;
  StrideSweepResult sweep_raw;
  StrideSweepResult sweep_formula;
  const bool have_draw = g_have_draw.load(std::memory_order_acquire);
  if (have_draw) {
    std::lock_guard<std::mutex> lock(g_draw_mutex);
    draw = g_draw;
    decode_raw = g_decode_raw;
    decode_formula = g_decode_formula;
    raw_dump_call1 = g_raw_dump_call1;
    raw_dump_index = g_raw_dump_index;
    sweep_raw = g_sweep_raw;
    sweep_formula = g_sweep_formula;
  }

  ImGui::Separator();
  // Round 13 (native-rendering-phase3.md): round 12's log evidence showed
  // sub_82217EE8 firing every frame (a fixed 2D UI/post-process element)
  // while sub_8221C518 only fires occasionally -- so an unfiltered re-arm
  // almost always lets sub_82217EE8 win the race back, even during real
  // gameplay (confirmed: a re-arm taken while standing in the open world
  // still captured a sub_82217EE8 NDC quad). These radio buttons let the
  // NEXT re-arm require a specific hook, so sub_8221C518 (or any future
  // hook) gets a real chance to be the one captured instead of losing every
  // time to whichever function happens to fire first.
  static uint32_t s_selected_filter = 0;
  ImGui::TextUnformatted("Round 13: filter which hook the NEXT re-arm will accept:");
  if (ImGui::RadioButton("any hook", s_selected_filter == 0)) {
    s_selected_filter = 0;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("sub_82217EE8 only", s_selected_filter == 0x82217EE8u)) {
    s_selected_filter = 0x82217EE8u;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("sub_8221C518 only", s_selected_filter == 0x8221C518u)) {
    s_selected_filter = 0x8221C518u;
  }
  ImGui::SameLine();
  if (ImGui::RadioButton("sub_8221E0F0 only", s_selected_filter == 0x8221E0F0u)) {
    s_selected_filter = 0x8221E0F0u;
  }
  if (have_draw) {
    if (ImGui::Button("Re-arm: capture the NEXT validated draw (per filter above)")) {
      RearmCapture(s_selected_filter);
      ImGui::End();
      return;
    }
  } else {
    ImGui::TextDisabled(
        "Re-arm: capture the NEXT validated draw (nothing captured yet)");
  }
  ImGui::TextWrapped(
      "Round 11: the very first validated draw (guest_frame=7, essentially at "
      "process boot) decodes cleanly but its own values (x/y in {0,0.5,1}, "
      "third component 720.0/1280.0/0.5/1.0) look like a 2D UI quad, not a 3D "
      "mesh -- see native-rendering-phase3.md. Get further into the game "
      "(past loading, into the main menu background or actual gameplay), "
      "THEN pick a filter above and click re-arm to grab a fresh draw from "
      "that moment, from the specific hook selected.");

  if (!have_draw) {
    ImGui::TextUnformatted(
        "Waiting for a validated draw. This overlay shows exactly ONE "
        "sub_82217EE8 call -- the first one FableDrawCapture's scan fully "
        "confirms (real DRAW_INDX packet found AND resource fields "
        "updated) after fable2_native_poc_enable is turned on.");
    ImGui::End();
    return;
  }

  ImGui::TextUnformatted("Runtime-validated fields (Phase 2 attempt 3, 500/500):");
  ImGui::Separator();
  ImGui::Text("capture sequence / guest frame: %u / %u", draw.sequence, draw.guest_frame);
  ImGui::Text("source hook: sub_%08X (round 12: which of the nine "
              "draw-flush-family functions produced this draw)", draw.source_hook_addr);
  ImGui::Text("prim_type=%u (D3DPT_TRIANGLELIST)  index_count=%u", draw.prim_type,
              draw.index_count);
  ImGui::Text("DRAW_INDX header: 0x%08X", draw.drawindx_header);
  ImGui::Text("payload0..3: 0x%08X 0x%08X 0x%08X 0x%08X", draw.payload[0], draw.payload[1],
              draw.payload[2], draw.payload[3]);
  ImGui::Text("  payload1=VGT_DRAW_INITIATOR  payload2=VGT_DMA_BASE (index addr, ground truth)");
  ImGui::Text("  payload3=VGT_DMA_SIZE  payload0=device-state bit-test (NOT flags, see phase2.md)");
  ImGui::Text("vertex_addr_raw=0x%08X  index_addr_raw=0x%08X", draw.vertex_addr_raw,
              draw.index_addr_raw);
  ImGui::Text("rt_slot0=0x%08X  shadow[0x2000/RB_SURFACE]=0x%08X", draw.rt_slot0,
              draw.shadow_0x2000);
  ImGui::Text("shader identity: UNKNOWN (not fabricated -- this overlay draws with ImGui's own "
              "solid-fill draw-list shader, not a decoded Fable II shader)");

  ImGui::Separator();
  ImGui::Text("Phase 4 vertex-stream evidence (raw capture, no formula -- see phase3.md):");
  ImGui::Text("  self+13516 (stream_binding_ptr, IDENTITY UNCONFIRMED) = 0x%08X",
              draw.stream_binding_ptr);
  if (draw.resource_calls_captured) {
    ImGui::Text("  sub_82206820 call #1 (feeds vertex_addr_raw): count_arg=%u stride_arg=%u "
                "return_addr=0x%08X",
                draw.vertex_call_count_arg, draw.vertex_call_stride_arg,
                draw.vertex_call_return_addr);
    ImGui::Text("  sub_82206820 call #2 (feeds index_addr_raw):  count_arg=%u stride_arg=%u "
                "return_addr=0x%08X",
                draw.index_call_count_arg, draw.index_call_stride_arg,
                draw.index_call_return_addr);
  } else {
    ImGui::TextColored(ImVec4(1.0f, 0.55f, 0.2f, 1.0f),
                        "  resource_calls_captured=false -- this draw reused an already-bound "
                        "buffer rather than re-invoking sub_82206820 (round 15: expected for a "
                        "high-frequency caller, not a bug -- see log)");
  }

  ImGui::Separator();
  ImGui::Text("Decode parameters (stride is CAPTURED ground truth when available; index32/"
              "swap_geometry_bytes are still unvalidated cvar guesses):");
  const uint32_t display_stride =
      (draw.resource_calls_captured && draw.vertex_call_stride_arg != 0)
          ? draw.vertex_call_stride_arg
          : REXCVAR_GET(fable2_native_poc_vertex_stride);
  ImGui::Text("  vertex_stride=%u bytes (%s)  index32=%s  swap_geometry_bytes=%s", display_stride,
              (draw.resource_calls_captured && draw.vertex_call_stride_arg != 0)
                  ? "captured"
                  : "cvar guess",
              REXCVAR_GET(fable2_native_poc_index32) ? "true" : "false",
              REXCVAR_GET(fable2_native_poc_swap_geometry_bytes) ? "true" : "false");

  ImGui::Separator();
  ImGui::TextWrapped(
      "Round 15, sixth follow-up: fable2_063.log showed idx=6 (NOT a restart marker) still "
      "decoding to an enormous z value -- with no ground-truth stride for this draw, the guess "
      "below may simply be wrong. Adjust it live and press the button below -- no rebuild, no "
      "new capture, re-decodes the SAME already-captured draw shown in this section.");
  {
    int stride_widget = static_cast<int>(REXCVAR_GET(fable2_native_poc_vertex_stride));
    if (ImGui::SliderInt("Vertex stride (bytes)", &stride_widget, 4, 128)) {
      REXCVAR_SET(fable2_native_poc_vertex_stride,
                  static_cast<uint32_t>(std::max(stride_widget, 4)));
    }
    bool index32_widget = REXCVAR_GET(fable2_native_poc_index32);
    if (ImGui::Checkbox("index32", &index32_widget)) {
      REXCVAR_SET(fable2_native_poc_index32, index32_widget);
    }
    ImGui::SameLine();
    bool swap_widget = REXCVAR_GET(fable2_native_poc_swap_geometry_bytes);
    if (ImGui::Checkbox("swap_geometry_bytes", &swap_widget)) {
      REXCVAR_SET(fable2_native_poc_swap_geometry_bytes, swap_widget);
    }
  }
  if (ImGui::Button("Re-decode now (apply stride/index32/swap above to the captured draw)")) {
    ReDecodeWithCurrentCvars();
  }

  ImGui::Separator();
  ImGui::TextWrapped(
      "Round 15, eighth follow-up: automatic stride sweep -- every stride from %u to %u "
      "(step %u) tried against THIS captured draw, no dragging required. Re-run after "
      "changing index32/swap_geometry_bytes above with the button below. Round 15, ninth "
      "follow-up: this and the slider above now read a FROZEN snapshot taken at capture time "
      "(refreshed once more if population is later observed), not live memory -- a resource_"
      "calls_captured=false draw's buffer is reused by other draws within milliseconds, which "
      "made repeated live re-sweeps of the same draw give a different answer every time.",
      kStrideSweepMin, kStrideSweepMax, kStrideSweepStep);
  if (sweep_raw.attempted) {
    if (sweep_raw.plausible_count > 0) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                          "  Candidate A: %d/%d strides plausible:", sweep_raw.plausible_count,
                          sweep_raw.candidates_tried);
      ImGui::SameLine();
      std::string hits;
      for (int i = 0; i < sweep_raw.plausible_count; ++i) {
        if (i) hits += ", ";
        hits += std::to_string(sweep_raw.plausible_strides[i]);
      }
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", hits.c_str());
    } else {
      ImGui::Text("  Candidate A: 0/%d strides plausible", sweep_raw.candidates_tried);
    }
  } else {
    ImGui::Text("  Candidate A: sweep not yet run");
  }
  if (sweep_formula.attempted) {
    if (sweep_formula.plausible_count > 0) {
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                          "  Candidate B: %d/%d strides plausible:", sweep_formula.plausible_count,
                          sweep_formula.candidates_tried);
      ImGui::SameLine();
      std::string hits_b;
      for (int i = 0; i < sweep_formula.plausible_count; ++i) {
        if (i) hits_b += ", ";
        hits_b += std::to_string(sweep_formula.plausible_strides[i]);
      }
      ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "%s", hits_b.c_str());
    } else {
      ImGui::Text("  Candidate B: 0/%d strides plausible", sweep_formula.candidates_tried);
    }
  } else {
    ImGui::Text("  Candidate B: sweep not yet run");
  }
  if (ImGui::Button("Sweep now (re-run the full stride sweep with index32/swap above)")) {
    RunStrideSweepWithCurrentParams();
  }

  ImGui::Separator();
  ImGui::TextWrapped(
      "The vertex address itself is NOT one of the assumptions above: no packet field carries "
      "it, and the formula previously applied to it by analogy is known (this session) to be "
      "wrong for at least one real draw. Both candidate addresses below are reported as "
      "evidence; neither is asserted correct.");

  ImGui::Separator();
  ImGui::TextUnformatted("Candidate A -- vertex_addr_raw used AS-IS (self+13448, no transform):");
  DrawCandidateSection("A", draw.vertex_addr_raw, decode_raw);

  ImGui::Separator();
  ImGui::TextUnformatted("Candidate B -- PredictScratchAddressField(vertex_addr_raw), by analogy:");
  const uint32_t formula_addr = PredictScratchAddressField(draw.vertex_addr_raw);
  DrawCandidateSection("B", formula_addr, decode_formula);

  ImGui::Separator();
  ImGui::TextWrapped(
      "If neither candidate is plausible, the real vertex stream base/offset/stride/format is "
      "not yet identified -- see native-rendering-phase3.md's Phase 4 section for the static "
      "trace of sub_82206820's own body (self+13516 branch, self+14916 bump allocator) that is "
      "the next place to look, rather than guessing a third address formula.");


  ImGui::Separator();
  ImGui::TextWrapped(
      "Raw dword dump at candidate A / vertex_addr_raw (no index/stride derivation). Round 15: "
      "now always attempted (previously only when resource_calls_captured, which crashed on "
      "guest 0x0 the first time a reused-buffer draw reached this point -- see phase3.md's "
      "round 15 follow-up). LIVE: re-read every frame this overlay is open (TickAddressWatch) "
      "until it faults or goes non-zero, to catch population happening after the one-shot "
      "capture above (only armed when resource_calls_captured is true -- see round 3 section).");
  if (!raw_dump_call1.attempted) {
    ImGui::TextUnformatted("  (not captured)");
  } else if (raw_dump_call1.fault) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  addr=0x%08X: SEH FAULT",
                        raw_dump_call1.addr);
  } else {
    ImGui::Text("  addr=0x%08X:", raw_dump_call1.addr);
    ImGui::Text("  %08X %08X %08X %08X %08X %08X %08X %08X", raw_dump_call1.dwords[0],
                raw_dump_call1.dwords[1], raw_dump_call1.dwords[2], raw_dump_call1.dwords[3],
                raw_dump_call1.dwords[4], raw_dump_call1.dwords[5], raw_dump_call1.dwords[6],
                raw_dump_call1.dwords[7]);
  }

  ImGui::Separator();
  ImGui::TextWrapped(
      "Round 15 follow-up: raw dword dump at index_addr, independent of the vertex-side probe "
      "above. TryDecodeVertex's single __try block covers both the index read and the vertex "
      "read, so a decode_ok=false above can't say which one actually faulted -- this settles "
      "it directly for the index side.");
  if (!raw_dump_index.attempted) {
    ImGui::TextUnformatted("  (not captured)");
  } else if (raw_dump_index.fault) {
    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "  addr=0x%08X: SEH FAULT",
                        raw_dump_index.addr);
  } else {
    ImGui::Text("  addr=0x%08X:", raw_dump_index.addr);
    ImGui::Text("  %08X %08X %08X %08X %08X %08X %08X %08X", raw_dump_index.dwords[0],
                raw_dump_index.dwords[1], raw_dump_index.dwords[2], raw_dump_index.dwords[3],
                raw_dump_index.dwords[4], raw_dump_index.dwords[5], raw_dump_index.dwords[6],
                raw_dump_index.dwords[7]);
  }

  ImGui::Separator();
  ImGui::TextWrapped(
      "Write watch on call #1's allocation (rex's own physical-memory invalidation-callback "
      "mechanism -- see native-rendering-phase3.md's round 5 section):");
  if (!g_write_watch_fired.load(std::memory_order_relaxed)) {
    ImGui::TextUnformatted("  armed, waiting for the first write...");
  } else {
    ImGui::Text("  FIRED: physical_address_start=0x%08X length=%u exact_range=%s",
                g_write_watch_result.physical_address_start, g_write_watch_result.length,
                g_write_watch_result.exact_range ? "true" : "false");
    ImGui::Text("  thread_id=%lu frame_count=%u",
                static_cast<unsigned long>(g_write_watch_result.thread_id),
                g_write_watch_result.frame_count);
    ImGui::TextWrapped(
        "  Raw call-stack return addresses (cross-reference against the build's own .pdb/.map "
        "-- see the full log line for the complete list):");
    for (USHORT i = 0; i < g_write_watch_result.frame_count && i < 4; ++i) {
      ImGui::Text("    [%u] %p", i, g_write_watch_result.frames[i]);
    }
  }

  ImGui::End();
}

}  // namespace fable2::native_poc
