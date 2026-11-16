// Fable2PC -- Phase 2 diagnostic instrumentation: FableDrawCapture
//
// Runtime validation of the pre-PM4 draw-boundary reconstruction recorded in
// native/performance-investigation-phase1.md. See
// native/performance-investigation-phase2.md for the full design writeup,
// the exact offsets/formulas used below, and how to read the results.
//
// WHAT THIS FILE DOES
// --------------------
// It adds a single, opt-in, bounded diagnostic hook around one confirmed
// member of the nine-function render-state-flush/draw family,
// `sub_82217EE8`. It does NOT implement a renderer, does NOT bypass or
// suppress any existing ReXGlue/Xenos rendering, and does NOT change the
// original guest logic in any way.
//
// HOW IT STAYS BEHAVIOR-PRESERVING
// ---------------------------------
// ReXGlue's weak-symbol mechanism (native/generated/default/fable2_pch.h,
// DEFINE_REX_FUNC) always compiles the *original* recompiled PPC logic into
// a non-weak symbol named `__imp__sub_82217EE8`; the plain name
// `sub_82217EE8` is only ever a *weak* alias/trampoline to it. A strong,
// non-weak definition of `sub_82217EE8` -- exactly what this file provides
// -- wins at link time and becomes what every caller (including other
// recompiled guest functions, e.g. sub_8220A528) actually calls. This file's
// override:
//   1. optionally reads read-only guest state from `ctx`/guest memory
//      (never writes to any guest-visible state),
//   2. calls straight through to `__imp__sub_82217EE8(ctx, base)` --
//      the ORIGINAL logic, with `ctx`/`base` completely untouched, so the
//      guest program's inputs, outputs, and side effects are bit-for-bit
//      identical to not having this file at all,
//   3. optionally reads more read-only guest state afterward,
//   4. records a capture row to a bounded, preallocated array and (once)
//      flushes it to a CSV file for offline analysis.
// When the diagnostic cvar below is at its default (0), step 1 and step 3
// do nothing beyond one cheap atomic load and a comparison -- no guest
// memory is read, no allocation happens, and the call graph shape is
// identical to the unmodified game. This satisfies "the original rendering
// path must remain behaviorally unchanged."
//
// WHAT IS CAPTURED (see phase2 doc for the exact reasoning/citations)
// ---------------------------------------------------------------------
//   - a local capture sequence number and the real guest frame number
//     (fable2::submitted_frames, already exposed by pc_features.h/.cpp)
//   - primitive type, index count, flags (the function's own entry args)
//   - the raw scratch-allocator addresses left behind by the two
//     sub_82206820 calls (self+13448 = vertex-fetch address,
//     self+13452 = index-buffer address), only valid when a draw was
//     actually emitted this call
//   - the *actual* DRAW_INDX (PM4 opcode 0x22) packet, recovered by
//     scanning the exact range of guest memory this call appended to the
//     shared record buffer (self+48), rather than assumed from statics
//   - the *predicted* DRAW_INDX payload dwords, computed from pre-call
//     state using the bit formulas documented in Phase 1, and a per-field
//     match/mismatch flag against the actual packet
//   - a coarse (first-dword) snapshot of the 8 confirmed shadow
//     render-state bank offsets, and the 5-slot render-target array
//   - shader identity: always written as the literal string "UNKNOWN" --
//     Phase 1 found no shader bytecode/microcode address observable at
//     this boundary, and this file does not invent one.
//
// This is diagnostic-only code. It is written to be safe by construction:
// every guest memory access is a read, every access is bounds-checked
// against a hard iteration cap, and a detected mid-call record-buffer reset
// (cursor decreasing) is reported as "unobserved" rather than followed
// blindly into an unrelated region of guest memory.

#include "fable2_pch.h"

#include <rex/cvar.h>
#include <rex/logging.h>

#include <rex/system/kernel_state.h>  // Round 16, ninth follow-up: kernel_memory(), see DiagIsChunkReadable
#include <rex/system/xmemory.h>       // Round 16, ninth follow-up: Memory/BaseHeap/HeapAllocationInfo, ditto

// Round 16, twentieth follow-up: reach ReX's own already-analyzed vertex
// shader (Shader::vertex_bindings(), populated by Shader::AnalyzeUcode()
// from the real vfetch_full microcode -- see native-rendering-phase3.md,
// Round 16 nineteenth follow-up for the full traced path) instead of
// re-deriving stride from raw bytes ourselves.
#include <rex/runtime.h>                          // Runtime::graphics_system()
#include <rex/graphics/graphics_system.h>         // GraphicsSystem::command_processor()
#include <rex/graphics/command_processor.h>       // CommandProcessor::active_vertex_shader()
#include <rex/graphics/pipeline/shader/shader.h>  // Shader::vertex_bindings(), VertexBinding
// Round 20 (Fable2PC, native 3D draw path): Register enum (XE_GPU_REG_*)
// and the SQ_VS_CONST register layout, both needed to resolve a shader's
// logical float-constant indices (Shader::constant_register_map(), already
// reachable via the include above) into physical RegisterFile offsets --
// see CommandProcessor::register_file()'s own comment in command_processor.h
// (this round's SDK addition) for why register VALUES needed a new
// accessor when register USAGE (constant_register_map()) already had one.
#include <rex/graphics/registers.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <excpt.h>  // EXCEPTION_EXECUTE_HANDLER -- see DiagSnapshotGuestBytes
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>  // Round 16, twenty-second follow-up: matches vector in LogActiveVertexShaderBindingForSlot

#include <windows.h>

#include "pc_features.h"  // fable2::submitted_frames
#include "diagnostics/fable_native_poc.h"  // Phase 3 hand-off (see below)

//=============================================================================
// Diagnostic toggle
//=============================================================================
// 0 (default) = fully disabled, zero behavioral/perf impact beyond one
// atomic load + compare per sub_82217EE8 call. A positive value N enables
// capture of the first N draws through sub_82217EE8, after which capture
// stops automatically and the CSV is flushed once. This is the "bounded
// first-N-draw capture / explicit diagnostic toggle" this pass calls for.
REXCVAR_DEFINE_UINT32(fable2_draw_capture_max, 0, "Fable II",
    "Phase 2 FableDrawCapture: capture the first N sub_82217EE8 draws and "
    "write fable2_draw_capture.csv, then stop. 0 disables (default).");

namespace fable2::diag {

// Hard cap on the preallocated capture array, independent of the cvar, so a
// misconfigured cvar value can never cause an unbounded allocation.
constexpr uint32_t kMaxCaptureRecords = 8192;

// Hard cap on how many dword positions the post-call packet scan will ever
// inspect, regardless of the observed cursor delta. REVISED after the first
// runtime capture run: self+48 is a *shared* record buffer used by every
// render-state-flush path in the device object, not just this draw's own
// DRAW_INDX emission (native/performance-investigation-phase1.md line
// ~1901), so a single sub_82217EE8 call routinely appends the register-bank
// and render-target flush records ahead of its own draw packet. The first
// runtime run's own cursor deltas (fable2_draw_capture.csv, attempt 1)
// ranged up to 2369 dwords (9476 bytes), not the "~2 dozen" this constant
// originally assumed -- that wrong assumption, combined with the
// then-structured walk bailing on the first non-DRAW_INDX record, is why
// drawindx_found was 0/500. This cap is now sized with headroom over the
// observed maximum, and remains defense-in-depth against a runaway/garbage
// cursor pair, not an expected code path.
constexpr uint32_t kScanSafetyCapDwords = 32768;

// sub_82217EE8's own entry arguments (r3=self is handled separately since
// it is also needed pre-call for old_cursor). Must be captured before
// __imp__sub_82217EE8 runs -- see the long comment on EntryArgs' only call
// site in the weak-symbol override below.
struct EntryArgs {
  uint32_t prim_type = 0;    // r4 at entry
  uint32_t index_count = 0;  // r7 at entry
  uint32_t flags = 0;        // r8 at entry
  uint32_t r6_raw = 0;       // r6 at entry (arg3, multiplied with r9 into the vertex dword-count basis)
  uint32_t r9_raw = 0;       // r9 at entry (arg6, multiplied with r6 into the vertex dword-count basis)
  // Round 12: which weak-symbol override populated this struct (e.g.
  // 0x82217EE8 or 0x8221C518) -- see ValidatedDrawInfo::source_hook_addr.
  uint32_t hook_addr = 0;


  // self+13448/13452 *before* the call, so CaptureAndValidate can tell
  // "this call's own success path ran" (pre != post) apart from "some
  // earlier call's success path ran and nothing has touched it since"
  // (pre == post, both stale). See kOffVertexAddr/kOffIndexAddr and the
  // attempt-2 root-cause note above ScanForDrawIndx.
  uint32_t vertex_addr_pre = 0;
  uint32_t index_addr_pre = 0;
};

struct DrawCaptureRow {
  uint64_t sequence = 0;
  uint64_t guest_frame = 0;

  uint32_t self_ptr = 0;
  uint32_t prim_type = 0;
  uint32_t index_count = 0;
  uint32_t flags = 0;
  uint32_t r6_raw = 0;  // sub_82217EE8's own arg3 (feeds the vertex dword-count basis)
  uint32_t r9_raw = 0;  // sub_82217EE8's own arg6 (feeds the vertex dword-count basis)
  uint32_t hook_addr = 0;  // round 12: which override function produced this row

  uint32_t old_cursor = 0;
  uint32_t new_cursor = 0;
  bool cursor_reset_detected = false;  // mid-call sub_821E8D20 drain observed

  uint32_t vertex_addr_pre = 0;   // self+13448 BEFORE the call
  uint32_t index_addr_pre = 0;    // self+13452 BEFORE the call
  uint32_t vertex_addr_raw = 0;   // self+13448 after the call (always read; compare to *_pre)
  uint32_t index_addr_raw = 0;    // self+13452 after the call (always read; compare to *_pre)
  uint32_t vertex_count_basis = 0;  // self+13456 post-call
  uint32_t index_count_basis = 0;   // self+13460 post-call
  // true iff vertex_addr/index_addr actually changed this call -- i.e. this
  // call's own success path (loc_822183B0) ran and wrote them, not just
  // "nonzero because some earlier call's success path wrote them and
  // nothing has touched them since." Computed independently of
  // drawindx_found (see attempt-2 root-cause note above ScanForDrawIndx) --
  // the two are expected to agree, and any disagreement is itself the
  // interesting result.
  bool resource_fields_updated = false;
  uint32_t buffer_limit = 0;   // self+56 post-call: the record buffer's capacity (see kOffLimit)
  uint32_t scan_end = 0;       // the actual upper bound ScanForDrawIndx was given this call

  bool drawindx_found = false;
  bool drawindx_bin_mask_bracket = false;  // opcode-0x60 packets immediately surrounding it
  uint32_t drawindx_header = 0;
  uint32_t payload_actual[4] = {0, 0, 0, 0};
  uint32_t payload_predicted[4] = {0, 0, 0, 0};
  bool payload_match[4] = {false, false, false, false};

  // First dword only at each of the 8 confirmed shadow render-state bank
  // destinations (see phase2 doc for the full offset table); a coarse
  // liveness/diff snapshot, not a full per-register dump.
  uint32_t shadow_bank[8] = {};

  uint32_t rt_slot[5] = {};

  static constexpr const char* kShaderIdentity = "UNKNOWN";
};

std::array<DrawCaptureRow, kMaxCaptureRecords> g_rows{};
std::atomic<uint32_t> g_next_index{0};
std::atomic<uint64_t> g_sequence{0};
std::once_flag g_flush_once;

// Phase 4 (vertex-stream tracing, native-rendering-phase3.md): captures the
// two sub_82206820 resource-resolver calls sub_82217EE8 makes for a specific
// draw (one feeds the vertex-fetch address, self+13448; the other feeds the
// index-buffer address, self+13452 -- performance-investigation-phase1.md's
// "Priority 1" section), rather than re-deriving another guessed address
// formula. Armed/disarmed tightly around one __imp__sub_82217EE8 call (see
// the weak-symbol overrides below) so no unrelated sub_82206820 call from
// elsewhere in the device object (it is a general-purpose resource
// resolver, not draw-exclusive -- see sub_82B9B340's slot=502 call in
// phase1.md) can be mistakenly attributed to this draw. Zero cost when
// capture_active is false, same as everything else in this file.
// Single-threaded assumption: like the rest of this diagnostic, this is not
// hardened against concurrent draw calls from multiple threads; if Fable
// II's render path ever turns out to be multi-threaded this would need
// thread-local state instead.
struct ResourceCallInfo {
  uint32_t count_arg = 0;           // r4: dword count passed to sub_82206820
  uint32_t stride_or_flag_arg = 0;  // r5: 16 for both calls per phase1.md
  uint32_t return_addr = 0;         // r3 after the call -- the real, observed return value
};
std::atomic<bool> g_capturing_resource_calls{false};
std::atomic<int> g_resource_call_count{0};
ResourceCallInfo g_resource_calls[2];

//=============================================================================
// Bit-exact helpers, mirroring the formulas documented in
// native/performance-investigation-phase1.md (Priority 1/2 section).
//=============================================================================

uint32_t RotL32(uint32_t v, uint32_t n) {
  n &= 31u;
  return n == 0 ? v : (v << n) | (v >> (32u - n));
}

// dword_2 of VGT_DRAW_INITIATOR-shaped payload: (primType & 0x3F) | (indexCount << 16).
uint32_t PredictVgtDrawInitiator(uint32_t prim_type, uint32_t index_count) {
  return (prim_type & 0x3Fu) | ((index_count << 16) & 0xFFFF0000u);
}

// VGT_DMA_SIZE-shaped payload: (indexCount & 0xFFFFFF) | ((flags << 30) & 0xC0000000).
uint32_t PredictVgtDmaSize(uint32_t index_count, uint32_t flags) {
  return (index_count & 0xFFFFFFu) | ((flags << 30) & 0xC0000000u);
}

// The address-packing idiom used for both the vertex-fetch and index-buffer
// scratch addresses: rounding-adjust the high bits, then combine with the
// low 29 bits of the raw address. No type tag is applied here (the index
// path never ORs one in -- see Phase 1).
uint32_t PredictScratchAddressField(uint32_t raw_addr) {
  uint32_t hi = RotL32(raw_addr, 12) & 0xFFFu;
  hi += 512u;
  hi &= 0x1000u;
  return hi + (raw_addr & 0x1FFFFFFFu);
}

struct Type3Header {
  bool valid = false;
  uint8_t opcode = 0;
  uint32_t payload_count = 0;
};

Type3Header DecodeType3(uint32_t header) {
  Type3Header h;
  if ((header >> 30) != 3u) return h;
  h.valid = true;
  h.opcode = static_cast<uint8_t>((header & 0xFF00u) >> 8);
  h.payload_count = ((header & 0x3FFF0000u) >> 16) + 1u;
  return h;
}

// Round 16, seventh follow-up (native-rendering-phase3.md): a sibling scan
// to ScanForDrawIndx, over the exact same appended byte range, but looking
// for TYPE0 register-write packets instead of the TYPE3 DRAW_INDX. This is
// the general-purpose version of the round-16-sixth-follow-up self+24/
// self+1148 read: that approach only works for functions that route fetch
// writes through the shared dirty-bitmask/shadow-table mechanism
// (sub_8221E0F0's family) and reads AFTER the call, which the fable2_078.log
// test showed is usually too late (the flush already cleared the dirty
// bits by then). This scan instead looks directly at the raw bytes this
// specific call appended to the shared record buffer (self+48) -- the same
// buffer ScanForDrawIndx already scans, already confirmed (by that
// function's own header comment) to interleave real PM4 TYPE-3 packets
// with the "(count<<16)|value" TYPE0 record format the register-bank flush
// path uses. This catches a freshly-emitted SHADER_CONSTANT_FETCH write
// (register range 0x4800-0x48BF, 32 slots * 6 dwords/slot) regardless of
// which internal mechanism wrote it -- the dynamic self+24 dirty-bit flush
// used by sub_8221E0F0's family, OR a direct/literal construction like the
// one found this round in sub_822069C0 (built inline via lis/ori immediates
// around fable2_recomp.49.cpp:~2517, not through self+24 at all -- a
// different vertex-fetch subsystem entirely, which is exactly why a single
// shared struct-offset read could never have covered both).
constexpr uint32_t kFetchRegBase = 0x4800;
constexpr uint32_t kFetchRegEnd = 0x48C0;  // exclusive; 32 slots * 6 dwords/slot

struct Type0Header {
  bool valid = false;
  uint32_t base_register = 0;
  uint32_t count = 0;
};

Type0Header DecodeType0(uint32_t header) {
  Type0Header h;
  if ((header >> 30) != 0u) return h;  // top 2 bits must be 00 for TYPE0
  h.base_register = header & 0xFFFFu;
  h.count = ((header & 0x3FFF0000u) >> 16) + 1u;
  return h;
}

struct FetchScanMatch {
  uint32_t addr = 0;
  uint32_t header = 0;
  uint32_t base_register = 0;
  uint32_t count = 0;
  uint32_t payload[6] = {0, 0, 0, 0, 0, 0};
};

// Same brute-force per-dword approach as ScanForDrawIndx, same reasoning
// (the buffer's mixed packet formats make a structured walk unsafe without
// fully characterizing the non-PM4 record format first). Reports up to
// kMaxFetchMatches hits rather than stopping at the first, since a single
// call can plausibly flush more than one fetch slot.
constexpr int kMaxFetchMatches = 4;

int ScanForFetchConstants(uint8_t* base, uint32_t old_cursor, uint32_t scan_end,
                           FetchScanMatch (&out)[kMaxFetchMatches]) {
  int found_count = 0;
  if (scan_end <= old_cursor) return found_count;
  const uint32_t first_addr = old_cursor + 4u;
  if (scan_end < first_addr + 4u) return found_count;
  const uint32_t last_header_addr = scan_end - 4u;

  uint32_t dwords_scanned = 0;
  for (uint32_t addr = first_addr;
       addr <= last_header_addr && dwords_scanned < kScanSafetyCapDwords && found_count < kMaxFetchMatches;
       addr += 4u, ++dwords_scanned) {
    uint32_t header = REX_LOAD_U32(addr);
    Type0Header decoded = DecodeType0(header);
    if (!decoded.valid) continue;
    if (decoded.base_register < kFetchRegBase || decoded.base_register >= kFetchRegEnd) continue;
    if (decoded.count == 0 || decoded.count > 6) continue;  // sane fetch-write size only
    // Make sure the full payload is inside the scanned/allowed range.
    if (addr + 4u + decoded.count * 4u > scan_end) continue;

    FetchScanMatch& m = out[found_count];
    m.addr = addr;
    m.header = header;
    m.base_register = decoded.base_register;
    m.count = decoded.count;
    for (uint32_t i = 0; i < decoded.count && i < 6; ++i) {
      m.payload[i] = REX_LOAD_U32(addr + 4u + i * 4u);
    }
    ++found_count;
  }
  return found_count;
}

// Scans the guest-memory range this call appended to the shared record
// buffer (self+48), looking for the PM4_DRAW_INDX (opcode 0x22) packet.
// Read-only; bounded by kScanSafetyCapDwords regardless of the observed
// cursor delta.
struct ScanResult {
  bool found = false;
  bool bin_mask_bracket = false;
  uint32_t header = 0;
  uint32_t payload[4] = {0, 0, 0, 0};
};

ScanResult ScanForDrawIndx(uint8_t* base, uint32_t old_cursor, uint32_t scan_end) {
  ScanResult result;
  if (scan_end <= old_cursor) return result;  // nothing appended/reachable this call

  // cursor points at the last-written dword; first new one is +4.
  const uint32_t first_addr = old_cursor + 4u;
  // A DRAW_INDX record is 5 dwords (1 header + 4 payload = 20 bytes), so the
  // header's address can be no later than scan_end - 16 for the whole
  // record to lie inside the scanned range. scan_end is the CALLER's
  // computed upper bound (new_cursor plus forward slack, clamped to the
  // buffer's capacity) -- see kForwardSlackDwords -- not necessarily the
  // "known good" new_cursor itself; see the comment above this function.
  if (scan_end < first_addr + 16u) return result;  // not even room for one
  const uint32_t last_header_addr = scan_end - 16u;

  // Deliberately a brute-force per-dword signature scan, not a structured
  // "decode header, skip its payload_count, repeat" walk: this shared
  // buffer interleaves genuine PM4 TYPE-3 packets with a *different*,
  // non-PM4 "(count<<16)|value" record format used by the register-bank
  // flush path (Phase 1), and a structured walk has no way to skip over
  // the latter without first characterizing it in full -- out of scope for
  // this bounded diagnostic pass. Instead, check every dword position for
  // the exact, already-confirmed DRAW_INDX signature (TYPE-3, opcode 0x22,
  // 4-dword payload count). False-positive risk is negligible: matching by
  // chance needs the 2 type bits + 14 count bits + 8 opcode bits to all
  // line up (~1-in-16.7-million per candidate dword), and any spurious
  // match still has to survive the predicted-vs-actual payload comparison
  // below, so it would surface as a flagged mismatch, not a silent error.
  uint32_t dwords_scanned = 0;
  for (uint32_t addr = first_addr;
       addr <= last_header_addr && dwords_scanned < kScanSafetyCapDwords;
       addr += 4u, ++dwords_scanned) {
    uint32_t header = REX_LOAD_U32(addr);
    Type3Header decoded = DecodeType3(header);
    if (!decoded.valid || decoded.opcode != 0x22 || decoded.payload_count != 4) {
      continue;  // some other record (or an unrelated flush packet) -- skip past it, don't stop
    }

    result.found = true;
    result.header = header;
    for (int i = 0; i < 4; ++i) {
      result.payload[i] = REX_LOAD_U32(addr + 4u + static_cast<uint32_t>(i) * 4u);
    }
    // Bonus (Priority 3): was this DRAW_INDX immediately preceded by a
    // PM4_SET_BIN_MASK_LO (0x60) packet, i.e. the "complex branch" bracket?
    if (addr >= old_cursor + 4u + 8u) {
      uint32_t prior_header = REX_LOAD_U32(addr - 8u);
      Type3Header prior = DecodeType3(prior_header);
      result.bin_mask_bracket = prior.valid && prior.opcode == 0x60 && prior.payload_count == 1;
    }
    return result;
  }
  return result;
}

//=============================================================================
// CSV output
//=============================================================================

void FlushCsv() {
  const uint32_t count = std::min(g_next_index.load(std::memory_order_acquire), kMaxCaptureRecords);
  if (count == 0) return;

  const auto path = std::filesystem::absolute("fable2_draw_capture.csv");
  FILE* f = std::fopen(path.string().c_str(), "w");
  if (!f) {
    REXGPU_ERROR("FableDrawCapture: failed to open '{}' for write", path.string());
    return;
  }

  std::fprintf(f,
      "sequence,guest_frame,self_ptr,prim_type,index_count,flags,r6_raw,r9_raw,"
      "old_cursor,new_cursor,buffer_limit,scan_end,cursor_reset_detected,"
      "vertex_addr_pre,index_addr_pre,vertex_addr_raw,index_addr_raw,"
      "vertex_count_basis,index_count_basis,resource_fields_updated,"
      "drawindx_found,drawindx_bin_mask_bracket,drawindx_header,"
      "payload0_flags_actual,payload0_flags_predicted,payload0_match,"
      "payload1_vgt_draw_initiator_actual,payload1_vgt_draw_initiator_predicted,payload1_match,"
      "payload2_vgt_dma_base_actual,payload2_vgt_dma_base_predicted,payload2_match,"
      "payload3_vgt_dma_size_actual,payload3_vgt_dma_size_predicted,payload3_match,"
      "shadow_0x2200,shadow_0x2180,shadow_0x2000,shadow_0x2100,shadow_0x2280,shadow_0x2300,shadow_0x2380,shadow_0x4900,"
      "rt_slot0,rt_slot1,rt_slot2,rt_slot3,rt_slot4,"
      "shader_identity\n");

  for (uint32_t i = 0; i < count; ++i) {
    const DrawCaptureRow& r = g_rows[i];
    std::fprintf(f,
        "%llu,%llu,0x%08X,%u,%u,%u,0x%08X,0x%08X,"
        "0x%08X,0x%08X,0x%08X,0x%08X,%d,"
        "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,%d,"
        "%d,%d,0x%08X,"
        "0x%08X,0x%08X,%d,"
        "0x%08X,0x%08X,%d,"
        "0x%08X,0x%08X,%d,"
        "0x%08X,0x%08X,%d,"
        "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,"
        "0x%08X,0x%08X,0x%08X,0x%08X,0x%08X,"
        "%s\n",
        static_cast<unsigned long long>(r.sequence), static_cast<unsigned long long>(r.guest_frame),
        r.self_ptr, r.prim_type, r.index_count, r.flags, r.r6_raw, r.r9_raw,
        r.old_cursor, r.new_cursor, r.buffer_limit, r.scan_end, r.cursor_reset_detected ? 1 : 0,
        r.vertex_addr_pre, r.index_addr_pre, r.vertex_addr_raw, r.index_addr_raw,
        r.vertex_count_basis, r.index_count_basis,
        r.resource_fields_updated ? 1 : 0,
        r.drawindx_found ? 1 : 0, r.drawindx_bin_mask_bracket ? 1 : 0, r.drawindx_header,
        r.payload_actual[0], r.payload_predicted[0], r.payload_match[0] ? 1 : 0,
        r.payload_actual[1], r.payload_predicted[1], r.payload_match[1] ? 1 : 0,
        r.payload_actual[2], r.payload_predicted[2], r.payload_match[2] ? 1 : 0,
        r.payload_actual[3], r.payload_predicted[3], r.payload_match[3] ? 1 : 0,
        r.shadow_bank[0], r.shadow_bank[1], r.shadow_bank[2], r.shadow_bank[3],
        r.shadow_bank[4], r.shadow_bank[5], r.shadow_bank[6], r.shadow_bank[7],
        r.rt_slot[0], r.rt_slot[1], r.rt_slot[2], r.rt_slot[3], r.rt_slot[4],
        DrawCaptureRow::kShaderIdentity);
  }
  std::fclose(f);

  uint32_t mismatches = 0, draws_found = 0;
  for (uint32_t i = 0; i < count; ++i) {
    if (!g_rows[i].drawindx_found) continue;
    ++draws_found;
    for (bool m : g_rows[i].payload_match) {
      if (!m) { ++mismatches; break; }
    }
  }
  REXGPU_INFO(
      "FableDrawCapture: wrote {} rows ({} with a located DRAW_INDX packet, {} with at least one "
      "predicted-vs-actual field mismatch) to '{}'",
      count, draws_found, mismatches, path.string());
}

//=============================================================================
// The hook itself
//=============================================================================

// Confirmed device-object offsets, all from native/performance-investigation-phase1.md.
constexpr uint32_t kOffCursor = 48;
constexpr uint32_t kOffLimit = 56;  // record buffer capacity; compared against the cursor at
                                     // function entry to decide whether to flush via sub_821E8D20.
constexpr uint32_t kOffVertexAddr = 13448;
constexpr uint32_t kOffIndexAddr = 13452;
constexpr uint32_t kOffVertexCountBasis = 13456;
constexpr uint32_t kOffIndexCountBasis = 13460;
constexpr uint32_t kOffRtSlotsBase = 12440;  // 5 consecutive u32 slots
// Phase 4: gates sub_82206820's own simple-vs-complex internal branch (see
// native/generated/default/fable2_recomp.182.cpp ~line 833): zero takes the
// "simple" sub_822065F0 path instead of the self+14916 bump-allocator path.
// New this pass; captured for diagnostic context only, not yet used in any
// address computation.
constexpr uint32_t kOffStreamBindingPtr = 13516;
// Phase 4 follow-up (address-semantics trace): static trace of
// sub_822065F0 (fable2_recomp.82.cpp, DEFINE_REX_FUNC(sub_822065F0),
// ~line 885) -- the function sub_82206820 calls on its "simple"
// (self+13516==0) path -- shows it ALSO reads self+14916 (its own,
// separate branch-gate field, distinct from self+13516) very early:
// `clrlwi. r10,r4,31` tests bit 0 of the incoming flags arg (cr0);
// `beq` on that never fires for our observed flags=1 call sites, so
// execution falls through to testing self+14916 against 0 (cr6). When
// self+14916==0 (first-ever allocation from this device object, our
// observed case), the allocation base is read from self+52 (+60) --
// NOT self+48 (kOffCursor) as the Phase 1 record-buffer fields might
// suggest. self+52 sits between kOffCursor=48 and kOffLimit=56;
// {cursor=48, base=52(?), limit=56} is a hypothesis, NOT confirmed.
// Both fields read fresh below, purely as raw evidence for the next
// runtime test -- neither is asserted to be a specific named member.
constexpr uint32_t kOffRecordBufferBase = 52;
constexpr uint32_t kOffVertexBumpCursor = 14916;
// Round 16 (native-rendering-phase3.md, "fifth follow-up"): statically
// derived from sub_8221D2E0's own body (fable2_recomp.79.cpp:1086-1284).
// self+24 is a 64-bit-loaded dirty-bit mask (only the low 32 bits are ever
// tested/used) over the 32 SHADER_CONSTANT_FETCH slots -- bit b corresponds
// to register 0x4800 + b*6. self+1148 is the persistent per-slot
// fetch-descriptor shadow table this mask indexes into: slot b's 24-byte
// record lives at self+1148+b*24. NOT runtime-confirmed yet -- the exact
// byte alignment inside each 24-byte record is ambiguous from static
// reading alone (sub_8221D2E0's own copy loop reads at a +4 byte offset
// within each record, which may or may not mean the real payload starts 4
// bytes into the slot rather than at its start). Captured raw below with
// margin on both sides so a real test settles it instead of guessing
// further.
constexpr uint32_t kOffFetchDirtyMask = 24;
constexpr uint32_t kOffFetchShadowTable = 1148;
constexpr uint32_t kFetchShadowSlotStride = 24;
constexpr uint32_t kFetchShadowSlotCount = 32;
constexpr uint32_t kShadowBankOffsets[8] = {
    10548,  // bank 0x2200
    10528,  // bank 0x2180 (SQ_PROGRAM_CNTL)
    10368,  // bank 0x2000 (RB_SURFACE)
    10444,  // bank 0x2100 (VGT index range)
    10596,  // bank 0x2280
    10680,  // bank 0x2300
    10832,  // bank 0x2380
    10112,  // bank 0x4900 (unconfirmed bank identity, offset itself confirmed by call-site trace)
};

// Attempt-2 fix: how far past new_cursor to keep scanning for a DRAW_INDX
// that self+48 never got pointed at (see the note above ScanForDrawIndx).
// The packet-emission block in sub_82217EE8 was traced by hand across both
// of its branches (fable2_recomp.69.cpp lines ~1527-1837, immediately
// following the register-bank-flush section) and tops out at 19 appended
// dwords (76 bytes) on its longest observed path. 256 dwords (1KB) is
// >13x that, comfortably covering this and any sibling flush function with
// a similar-shaped tail this pass didn't hand-trace, while staying tiny
// next to kScanSafetyCapDwords and clamped below to the buffer's own
// reported capacity so this can never read outside the allocated region.
constexpr uint32_t kForwardSlackDwords = 256;

void CaptureAndValidate(PPCContext& ctx, uint8_t* base, uint32_t self, uint32_t old_cursor,
                         const EntryArgs& entry) {
  DrawCaptureRow row;
  row.sequence = g_sequence.fetch_add(1, std::memory_order_relaxed);
  row.guest_frame = fable2::submitted_frames.load(std::memory_order_relaxed);
  row.self_ptr = self;
  row.prim_type = entry.prim_type;
  row.index_count = entry.index_count;
  row.flags = entry.flags;
  row.r6_raw = entry.r6_raw;
  row.hook_addr = entry.hook_addr;
  row.r9_raw = entry.r9_raw;
  row.old_cursor = old_cursor;
  row.vertex_addr_pre = entry.vertex_addr_pre;
  row.index_addr_pre = entry.index_addr_pre;

  for (int i = 0; i < 8; ++i) {
    row.shadow_bank[i] = REX_LOAD_U32(self + kShadowBankOffsets[i]);
  }
  for (int i = 0; i < 5; ++i) {
    row.rt_slot[i] = REX_LOAD_U32(self + kOffRtSlotsBase + static_cast<uint32_t>(i) * 4u);
  }

  // Always read these -- unconditionally, not gated on the scan below --
  // because whether the resource fields actually changed is an independent
  // ground-truth signal for "did this call's success path run" that must
  // not depend on whether ScanForDrawIndx also managed to locate the
  // packet. Gating this read on scan.found (attempt 2's behavior) meant a
  // scan miss silently hid a real, already-happened resource-field update
  // too, conflating two different questions.
  row.vertex_addr_raw = REX_LOAD_U32(self + kOffVertexAddr);
  row.index_addr_raw = REX_LOAD_U32(self + kOffIndexAddr);
  row.vertex_count_basis = REX_LOAD_U32(self + kOffVertexCountBasis);
  row.index_count_basis = REX_LOAD_U32(self + kOffIndexCountBasis);
  row.resource_fields_updated =
      (row.vertex_addr_raw != row.vertex_addr_pre) || (row.index_addr_raw != row.index_addr_pre);

  const uint32_t new_cursor = REX_LOAD_U32(self + kOffCursor);
  row.new_cursor = new_cursor;
  row.buffer_limit = REX_LOAD_U32(self + kOffLimit);

  if (new_cursor < old_cursor) {
    // sub_821E8D20 drained and reset the shared record buffer mid-call.
    // Reconstructing the appended range across a reset is out of scope for
    // this bounded pass -- report it, don't guess at it.
    row.cursor_reset_detected = true;
  } else {
    // scan_end: new_cursor plus forward slack (see kForwardSlackDwords),
    // clamped to the buffer's own reported capacity so this never reads
    // past the allocated region. new_cursor itself is NOT a safe upper
    // bound on a successful call -- see the note above ScanForDrawIndx.
    const uint32_t slack_end = new_cursor + kForwardSlackDwords * 4u;
    const uint32_t scan_end =
        (row.buffer_limit != 0 && row.buffer_limit < slack_end) ? row.buffer_limit : slack_end;
    row.scan_end = scan_end;

    ScanResult scan = ScanForDrawIndx(base, old_cursor, scan_end);
    row.drawindx_found = scan.found;
    row.drawindx_bin_mask_bracket = scan.bin_mask_bracket;
    row.drawindx_header = scan.header;
    for (int i = 0; i < 4; ++i) row.payload_actual[i] = scan.payload[i];

    // Round 16, seventh follow-up: same range, scan for freshly-appended
    // SHADER_CONSTANT_FETCH TYPE0 writes too. Runs for every capture
    // regardless of which hook (0x82217EE8/0x8221C518/0x8221E0F0/etc.)
    // produced it, so this is comparative evidence across all of them for
    // free. decode per xe_gpu_vertex_fetch_t (tools/rexglue-sdk/win-amd64/
    // include/rex/graphics/xenos.h): each vertex-fetch constant is 2
    // dwords -- dword0 low 2 bits = type, remaining 30 bits = address (in
    // dwords); dword1 low 2 bits = endian, next 24 bits = size (in words).
    // A slot's 6-dword block is 3 such constants back to back.
    FetchScanMatch fetch_matches[kMaxFetchMatches];
    int fetch_match_count = ScanForFetchConstants(base, old_cursor, scan_end, fetch_matches);
    for (int fmi = 0; fmi < fetch_match_count; ++fmi) {
      const FetchScanMatch& m = fetch_matches[fmi];
      REXLOG_INFO(
          "[FableDrawCapture] [FetchScan] seq={} hook={:#x} addr={:#x} "
          "base_reg={:#x} (slot={}) count={} header={:#010x}",
          row.sequence, entry.hook_addr, m.addr, m.base_register,
          (m.base_register - kFetchRegBase) / 6u, m.count, m.header);
      for (uint32_t ci = 0; ci + 1 < m.count; ci += 2u) {
        const uint32_t d0 = m.payload[ci];
        const uint32_t d1 = m.payload[ci + 1];
        const uint32_t type = d0 & 0x3u;
        const uint32_t address_dwords = d0 >> 2;
        const uint32_t endian = d1 & 0x3u;
        const uint32_t size_words = (d1 >> 2) & 0xFFFFFFu;
        REXLOG_INFO(
            "[FableDrawCapture] [FetchScan]   seq={} constant[{}] dword0={:#010x} dword1={:#010x} "
            "decoded: type={} address={:#x} endian={} size_bytes={}",
            row.sequence, ci / 2u, d0, d1, type, address_dwords * 4u, endian, size_words * 4u);
      }
    }

    if (scan.found) {
      // dword0/payload0: NOT a straight passthrough of `flags` -- corrected by
      // attempt 3 (native/performance-investigation-phase2.md, "Payload0
      // correction"). The generated body reassigns r24 (originally = flags at
      // entry) via a self+12187/self+12122-relative byte lookup, masked with
      // `& 0x13F`, before the packet store -- see fable2_recomp.69.cpp lines
      // 1645-1663. That lookup is intentionally NOT reproduced here: it isn't
      // needed by anything else this diagnostic captures, and attempt 3 already
      // showed the resulting value (0 for every observed draw so far) via the
      // *actual* packet, not this prediction. `row.flags` is kept as the
      // prediction so a mismatch stays visible in the CSV/REXGPU_WARN output
      // as a known, already-explained discrepancy rather than being silently
      // hidden -- do not "fix" this match by hand without also implementing the
      // real formula.
      row.payload_predicted[0] = row.flags;
      row.payload_predicted[1] = PredictVgtDrawInitiator(row.prim_type, row.index_count);
      row.payload_predicted[2] = PredictScratchAddressField(row.index_addr_raw);
      row.payload_predicted[3] = PredictVgtDmaSize(row.index_count, row.flags);
      for (int i = 0; i < 4; ++i) {
        row.payload_match[i] = (row.payload_actual[i] == row.payload_predicted[i]);
      }

      bool any_mismatch = false;
      for (bool m : row.payload_match) any_mismatch |= !m;
      if (any_mismatch) {
        REXGPU_WARN(
            "FableDrawCapture seq={} frame={}: predicted-vs-actual mismatch in DRAW_INDX payload "
            "(flags {}, initiator {}, dma_base {}, dma_size {})",
            row.sequence, row.guest_frame,
            row.payload_match[0] ? "ok" : "MISMATCH", row.payload_match[1] ? "ok" : "MISMATCH",
            row.payload_match[2] ? "ok" : "MISMATCH", row.payload_match[3] ? "ok" : "MISMATCH");
      }

      // Phase 3 hand-off (native/native-rendering-phase3.md): gated on
      // scan.found alone (a real DRAW_INDX packet was located in the command
      // buffer -- this is the actual, direct proof a draw happened; it can
      // only be true if the game's own code emitted the packet). Round 15:
      // previously this ALSO required row.resource_fields_updated, on the
      // assumption that an unchanged vertex/index address meant the call's
      // success path did not run (Phase 1's loc_822183E4 allocation-failure
      // path). fable2_056.1/.2.log (sub_8221E0F0, ~49000 real calls across
      // ~39 frames of ordinary walking) disproved that assumption for this
      // caller: scan.found was true and the predicted VGT_DRAW_INITIATOR
      // matched the real one on every single call ("initiator ok"), i.e. a
      // real draw certifiably happened each time, yet resource_fields_updated
      // was false on all of them -- so the gate silently discarded 100% of
      // this hook's traffic. The actual explanation: self+13448/self+13452
      // hold the CURRENTLY BOUND vertex/index buffer regardless of whether
      // THIS call's own sub_82206820 resolver calls ran (a high-frequency
      // caller can legitimately keep reusing an already-bound buffer without
      // re-resolving it, same as SetStreamSource/SetIndices persisting
      // across draws in any D3D-era API) -- so vertex_addr_raw/index_addr_raw
      // are still valid ground truth even when they didn't change this call.
      // row.resource_fields_updated is still computed and stored in the CSV
      // (informational: tells you whether THIS call freshly resolved its
      // buffers or reused the prior binding) but is no longer a hand-off
      // requirement. Downstream (fable_native_poc.cpp) already degrades
      // gracefully when the sub_82206820 calls weren't observed this time
      // (resource_calls_captured=false): it falls back to payload[2]/ground
      // truth for the index address and the cvar guess for stride, exactly
      // as it already did for any other resource_calls_captured=false case.
      // One-shot and a no-op unless fable2_native_poc_enable is on; see
      // fable_native_poc.h.
      if (fable2::native_poc::WantsNextValidatedDrawFrom(row.hook_addr)) {
        fable2::native_poc::ValidatedDrawInfo poc_info;
        // row.sequence/guest_frame are uint64_t (see DrawCaptureRow); the PoC
        // struct only needs them for display, so this narrowing is intentional.
        poc_info.sequence = static_cast<uint32_t>(row.sequence);
        poc_info.guest_frame = static_cast<uint32_t>(row.guest_frame);
        poc_info.prim_type = row.prim_type;
        poc_info.index_count = row.index_count;
        poc_info.index_addr_raw = row.index_addr_raw;
        poc_info.vertex_addr_raw = row.vertex_addr_raw;
        poc_info.drawindx_header = row.drawindx_header;
        for (int i = 0; i < 4; ++i) poc_info.payload[i] = row.payload_actual[i];
        poc_info.source_hook_addr = row.hook_addr;
        poc_info.rt_slot0 = row.rt_slot[0];
        poc_info.shadow_0x2000 = row.shadow_bank[2];

        // Phase 4: forward the directly-captured sub_82206820 resource-call
        // info, when it was observed. Round 15: NOT guaranteed anymore -- a
        // draw can reach this point via scan.found alone, reusing an
        // already-bound vertex/index buffer without this call re-invoking
        // sub_82206820 at all (see the round-15 note above CaptureAndValidate's
        // hand-off condition). poc_info.resource_calls_captured (set below
        // from g_resource_call_count) is what OnValidatedDraw actually checks
        // before trusting vertex_call_*/index_call_*; when false it already
        // falls back to payload[2]/the cvar stride guess, so this is a
        // graceful degradation, not an error condition. Also forward the
        // self+13516 branch-gate field, read fresh here (not stored in
        // DrawCaptureRow/the CSV schema, to avoid perturbing Phase 2's
        // output format).
        poc_info.stream_binding_ptr = REX_LOAD_U32(self + kOffStreamBindingPtr);
        // Phase 4 follow-up: self+52 and self+14916, read fresh here for the
        // same reason as stream_binding_ptr above (see kOffRecordBufferBase's
        // comment) -- raw evidence for sub_822065F0's self+14916==0 path,
        // read AFTER both sub_82206820 calls have completed, so
        // vertex_bump_cursor_raw reflects whatever state exists once both
        // calls' own success paths (if any) have already run.
        poc_info.record_buffer_base_raw = REX_LOAD_U32(self + kOffRecordBufferBase);
        poc_info.vertex_bump_cursor_raw = REX_LOAD_U32(self + kOffVertexBumpCursor);
        const int calls_observed = g_resource_call_count.load(std::memory_order_relaxed);
        poc_info.resource_calls_captured = (calls_observed >= 2);
        if (poc_info.resource_calls_captured) {
          poc_info.vertex_call_count_arg = g_resource_calls[0].count_arg;
          poc_info.vertex_call_stride_arg = g_resource_calls[0].stride_or_flag_arg;
          poc_info.vertex_call_return_addr = g_resource_calls[0].return_addr;
          poc_info.index_call_count_arg = g_resource_calls[1].count_arg;
          poc_info.index_call_stride_arg = g_resource_calls[1].stride_or_flag_arg;
          poc_info.index_call_return_addr = g_resource_calls[1].return_addr;
        }

        fable2::native_poc::OnValidatedDraw(base, poc_info);
      }
    }
    if (row.resource_fields_updated && !scan.found) {
      // This would be the genuinely unexpected case: the success path
      // demonstrably ran (resource fields changed) but the widened scan
      // still couldn't find the packet it should have written. Surfaced
      // loudly rather than silently folded into drawindx_found=false.
      REXGPU_WARN(
          "FableDrawCapture seq={} frame={}: resource_fields_updated=true but no DRAW_INDX found "
          "in [old_cursor+4, scan_end={:#010x}) -- packet may be further than kForwardSlackDwords "
          "past new_cursor, or this call's success path writes it somewhere else",
          row.sequence, row.guest_frame, scan_end);
    }
    // If !resource_fields_updated and no reset was detected, this call took
    // the allocation-failure path (Phase 1's loc_822183E4) and suppressed
    // the draw entirely -- both fields stay false, which is itself a valid,
    // informative result, not an error.
  }

  const uint32_t idx = g_next_index.fetch_add(1, std::memory_order_acq_rel);
  if (idx < kMaxCaptureRecords) {
    g_rows[idx] = row;
  }

  const uint32_t configured_max = REXCVAR_GET(fable2_draw_capture_max);
  // configured_max > 0 guard: without it, running ONLY for Phase 3's
  // native_poc hand-off (fable2_draw_capture_max left at its default 0)
  // would make "idx + 1 >= configured_max" (0) true on the very first
  // captured call and silently write out an unrequested CSV file.
  if (configured_max > 0 && (idx + 1 >= configured_max || idx + 1 >= kMaxCaptureRecords)) {
    std::call_once(g_flush_once, FlushCsv);
  }
}

}  // namespace fable2::diag

//=============================================================================
// Weak-symbol override
//=============================================================================
// The recompiled original logic; always present, never modified by this file.
REX_EXTERN(__imp__sub_82217EE8);
REX_EXTERN(__imp__sub_82206820);

// Phase 4: this override captures arguments/return value ONLY when armed by
// the sub_82217EE8 wrapper below, and always calls straight through to the
// original logic unconditionally -- identical behavior, zero risk to the
// many other callers of sub_82206820 elsewhere in the device object.
extern "C" void sub_82206820(PPCContext& __restrict ctx, uint8_t* base) {
  const bool capturing = fable2::diag::g_capturing_resource_calls.load(std::memory_order_relaxed);
  uint32_t count_arg = 0;
  uint32_t stride_arg = 0;
  if (capturing) {
    count_arg = ctx.r4.u32;
    stride_arg = ctx.r5.u32;
  }

  __imp__sub_82206820(ctx, base);

  if (capturing) {
    const int idx = fable2::diag::g_resource_call_count.fetch_add(1, std::memory_order_relaxed);
    if (idx < 2) {
      fable2::diag::g_resource_calls[idx] =
          fable2::diag::ResourceCallInfo{count_arg, stride_arg, ctx.r3.u32};
    }
  }
}

extern "C" void sub_82217EE8(PPCContext& __restrict ctx, uint8_t* base) {
  const uint32_t max_draws = REXCVAR_GET(fable2_draw_capture_max);
  const bool csv_capture_active =
      max_draws > 0 && fable2::diag::g_next_index.load(std::memory_order_relaxed) < max_draws;
  // Phase 3 (native/native-rendering-phase3.md): also stay engaged, independent
  // of the CSV bound above, purely to serve fable_native_poc.cpp's one-shot
  // hand-off. WantsNextValidatedDraw() is a cheap cvar read + one relaxed atomic
  // load, and goes false again (so this collapses back to csv_capture_active)
  // the moment either fable2_native_poc_enable is off or it already has its draw.
  const bool capture_active =
      csv_capture_active || fable2::native_poc::WantsNextValidatedDrawFrom(0x82217EE8u);

  // Round 8 diagnostic (native-rendering-phase3.md): a full Windows test run
  // with --fable2_native_poc_enable=true on the command line produced ZERO
  // [NativeDrawPoc] log lines of any kind -- including the unconditional
  // "captured seq=..." line OnValidatedDraw logs the instant it runs -- which
  // means OnValidatedDraw() never ran even once, i.e. capture_active was
  // false for this hook's entire lifetime. That could mean the cvar never
  // actually became true at runtime (command-line parsing, a stale/wrong
  // build, or a config file re-clobbering it), or that this gate's own logic
  // is wrong. Logged ONCE (not gated on capture_active -- deliberately so it
  // fires and reports the truth even when capture_active is false) so the
  // very next test settles which, without guessing further.
  static std::atomic<bool> s_logged_gate_state{false};
  bool gate_diag_expected = false;
  if (s_logged_gate_state.compare_exchange_strong(gate_diag_expected, true,
                                                   std::memory_order_relaxed)) {
    REXLOG_INFO(
        "[FableDrawCapture] [GateDiag] first sub_82217EE8 call this process: "
        "fable2_native_poc_enable={} fable2_draw_capture_max={} "
        "WantsNextValidatedDraw()={} csv_capture_active={} capture_active={}",
        REXCVAR_GET(fable2_native_poc_enable), max_draws,
        fable2::native_poc::WantsNextValidatedDrawFrom(0x82217EE8u), csv_capture_active,
        capture_active);
  }

  if (!capture_active) {
    // Diagnostic disabled or bound already reached: call straight through,
    // identical to there being no override at all.
    __imp__sub_82217EE8(ctx, base);
    return;
  }

  const uint32_t self = ctx.r3.u32;
  uint32_t old_cursor = 0;
  fable2::diag::EntryArgs entry{};
  entry.hook_addr = 0x82217EE8;
  if (self != 0) {
    old_cursor = REX_LOAD_U32(self + fable2::diag::kOffCursor);
    // Bugfix (post attempt-1 runtime capture): these MUST be read here,
    // before the original logic runs. sub_82217EE8 renames r4/r7/r8 to
    // r21/r23/r24 for its own use in its first few instructions and
    // reuses r4/r6/r7/r8/r9 as general scratch for everything after --
    // reading ctx.rN.u32 once the call below has returned (attempt 1's
    // bug) captures leftover scratch contents, not the real arguments.
    // This is what produced attempt 1's near-constant prim_type/r6_raw/
    // r9_raw and flat index_count=6 across all 500 captured draws.
    entry.prim_type = ctx.r4.u32;
    entry.index_count = ctx.r7.u32;
    entry.flags = ctx.r8.u32;
    entry.r6_raw = ctx.r6.u32;
    entry.r9_raw = ctx.r9.u32;
    // Attempt-2 addition: snapshot the resource fields pre-call too, so
    // CaptureAndValidate can tell "this call wrote them" from "an earlier
    // call wrote them and they're just still sitting there."
    entry.vertex_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffVertexAddr);
    entry.index_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffIndexAddr);
  }

  // Phase 4: arm the sub_82206820 resource-call capture for exactly this
  // call to __imp__sub_82217EE8. Cleared immediately after, regardless of
  // outcome, so no other code path's sub_82206820 calls are ever captured.
  // Zero cost when capture_active is false (this whole block is already
  // unreachable in that case -- see the early return above).
  fable2::diag::g_resource_call_count.store(0, std::memory_order_relaxed);
  fable2::diag::g_capturing_resource_calls.store(true, std::memory_order_relaxed);
  __imp__sub_82217EE8(ctx, base);
  fable2::diag::g_capturing_resource_calls.store(false, std::memory_order_relaxed);

  if (self != 0) {
    fable2::diag::CaptureAndValidate(ctx, base, self, old_cursor, entry);
  }
}

//=============================================================================
// Round 12 (native-rendering-phase3.md): a second draw-flush-family hook
//=============================================================================
// sub_82217EE8 is only one of nine structurally-identical "flush + optionally
// emit DRAW_INDX" functions (performance-investigation-phase1.md's "nine-
// function family": sub_822069C0/23 callers, sub_82207C30/1, sub_82217EE8/4,
// sub_8221C9C8/1, sub_8221E0F0/8, sub_8221C518/11, sub_822060A0/5,
// sub_82BA5448/1, sub_82BA29B8/1, sub_8222B948/1). Round 11's re-arm button
// let the user capture 12 real draws through sub_82217EE8 across ~90 seconds
// of actual play (fable2_052.log): 11 had index_count=6, one had 36, and the
// decoded vertices were consistently NDC/UV-shaped (x/y confined to
// {-1,-0.5,0,0.5,1}, per-vertex screenshots) -- strong evidence sub_82217EE8's
// own four call sites are a 2D UI/post-process path, not the world-mesh
// renderer, and that re-arming alone will never reach real 3D geometry no
// matter what the user is looking at.
//
// sub_8221C518 (11 callers, the highest of the nine after sub_822069C0's 23,
// which is a much larger 912-instruction function this round did not safely
// trace) is the next candidate. Hand-traced this round
// (fable2_recomp.171.cpp, DEFINE_REX_FUNC(sub_8221C518), lines 985-1577) to
// confirm its OWN entry-argument positions rather than assume sub_82217EE8's
// -- the two functions do NOT share the same argument layout:
//   - prim_type: CONFIRMED at the same position as sub_82217EE8 (arg1/r4,
//     masked & 0x3F immediately before the DRAW_INDX header push --
//     fable2_recomp.171.cpp line ~1391: `clrlwi r9,r25,26` where r25 == r4
//     from this function's own prologue's `mr r25,r4`).
//   - index_count: CONFIRMED to be arg3/r6 on this function's common path
//     (fable2_recomp.171.cpp lines 1327-1357: `mr r29,r24` where r24 == r6
//     from this function's own `mr r24,r6`, then r29 feeds the
//     VGT_DRAW_INITIATOR shift) -- NOT arg4/r7 like sub_82217EE8. This is
//     exactly the kind of per-function difference that made a blind copy of
//     sub_82217EE8's wrapper unsafe -- see the index_count-from-payload fix
//     in fable_native_poc.cpp's OnValidatedDraw (same round) for the
//     independent safety net this finding motivated.
//   - flags (arg5/r8 in sub_82217EE8): NOT re-traced for this function this
//     round. Read by analogy only, for display/prediction purposes -- never
//     used for the actual vertex/index decode, which reads real captured PM4
//     bytes regardless of this value.
//
// This hook is native_poc-only (WantsNextValidatedDraw() alone, no
// fable2_draw_capture_max/CSV participation -- that Phase 2 mechanism was
// designed and validated against sub_82217EE8 specifically and is left
// untouched here). Behavior-preserving by the exact same argument as
// sub_82217EE8's own override: __imp__sub_8221C518 is always called,
// unconditionally, with ctx/base completely untouched -- reading a few
// entry registers beforehand and some already-guest-owned device-object
// fields afterward has no guest-visible effect.
REX_EXTERN(__imp__sub_8221C518);

extern "C" void sub_8221C518(PPCContext& __restrict ctx, uint8_t* base) {
  // Deliberately native_poc-only (see comment block above) -- no
  // csv_capture_active component, unlike sub_82217EE8's wrapper.
  const bool capture_active = fable2::native_poc::WantsNextValidatedDrawFrom(0x8221C518u);
  if (!capture_active) {
    __imp__sub_8221C518(ctx, base);
    return;
  }

  const uint32_t self = ctx.r3.u32;
  uint32_t old_cursor = 0;
  fable2::diag::EntryArgs entry{};
  entry.hook_addr = 0x8221C518;
  if (self != 0) {
    old_cursor = REX_LOAD_U32(self + fable2::diag::kOffCursor);
    // Must be read here, before __imp__sub_8221C518 runs -- same reasoning
    // as sub_82217EE8's wrapper (its own prologue renames these into its
    // own working registers and reuses r4/r6/r7/r8/r9 as scratch afterward).
    entry.prim_type = ctx.r4.u32;
    entry.index_count = ctx.r6.u32;  // arg3 -- CONFIRMED for this function (see block comment)
    entry.flags = ctx.r8.u32;        // arg5 -- NOT re-traced; informational only
    entry.r6_raw = ctx.r6.u32;
    entry.r9_raw = ctx.r9.u32;
    entry.vertex_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffVertexAddr);
    entry.index_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffIndexAddr);
  }

  fable2::diag::g_resource_call_count.store(0, std::memory_order_relaxed);
  fable2::diag::g_capturing_resource_calls.store(true, std::memory_order_relaxed);
  __imp__sub_8221C518(ctx, base);
  fable2::diag::g_capturing_resource_calls.store(false, std::memory_order_relaxed);

  if (self != 0) {
    fable2::diag::CaptureAndValidate(ctx, base, self, old_cursor, entry);
  }
}

//=============================================================================
// Round 14 (native-rendering-phase3.md): a third draw-flush-family hook
//=============================================================================
// The user filtered the overlay to "sub_8221C518 only" and re-armed
// repeatedly across several minutes of real open-world traversal (walking
// around, an NPC and particle effects visible on screen) -- it never fired.
// Combined with round 12's evidence (11 callers, but only ever observed
// firing in a 24-call burst while a tab/inventory menu was open), this is
// itself a real result: sub_8221C518 is very likely menu/list-UI-specific
// and simply isn't called during ordinary world traversal at all.
//
// sub_8221E0F0 (8 callers, tail-confirmed as part of the nine-function
// family in performance-investigation-phase1.md's five-sibling check) is
// the next candidate. Hand-traced this round (fable2_recomp.143.cpp,
// DEFINE_REX_FUNC(sub_8221E0F0), lines 944-1594) through to its own
// VGT_DRAW_INITIATOR construction (line ~1376-1377: `ctx.r22 = ctx.r18 &
// 0x3F` where r18 == its own `mr r18,r4` -- prim_type, arg1/r4, same
// position as sub_82217EE8; then `ctx.r7 = rotate(ctx.r27, 16) &
// 0xFFFF0000` where r27 == its own `mr r19,r7` copy re-read -- index_count,
// arg4/r7, ALSO the same position as sub_82217EE8, unlike sub_8221C518
// which used arg3/r6). So this one's entry-argument layout for prim_type/
// index_count happens to match sub_82217EE8's exactly -- confirmed by
// tracing this function's own body, not assumed from the other two.
//
// Same shape as the sub_8221C518 override above: native_poc-only (no CSV
// participation), reads entry args before calling through, hands off to the
// same CaptureAndValidate/OnValidatedDraw/g_have_draw path, tagged with its
// own hook_addr so the round-13 filter and the overlay's "source hook" line
// both work for it. Behavior-preserving by the same argument as the other
// two overrides: __imp__sub_8221E0F0 is always called, unconditionally,
// with ctx/base completely untouched.
REX_EXTERN(__imp__sub_8221E0F0);

// Round 16 sixth follow-up (native-rendering-phase3.md): fable2_078.log came
// back with fetch_dirty_mask=0x00000000 on all 3 captures (seq=1/2/3,
// frames 1725/7029/7117, all sharing the SAME self=0x44142480 -- itself a
// finding: this state object is a single long-lived instance reused across
// widely separated frames, not allocated fresh per draw/per mesh). That
// result is explained by a bug in where this was read, not by the fetch
// bank genuinely being clean: it was read AFTER __imp__sub_8221E0F0 already
// ran, and that function's own body flushes this exact bank (via
// sub_8221D2E0) as part of normal execution -- flushing clears the dirty
// bits, so reading them afterward will show 0 essentially always,
// regardless of what was actually bound going into the draw. Fixed by
// capturing BEFORE the call (same "must be read here" reasoning already
// used for vertex_addr_pre/index_addr_pre above) and keeping an AFTER dump
// too, so the two can be diffed: pre-nonzero + post-zero would confirm the
// flush-clears theory and gives real descriptor bytes to decode; pre-zero
// too would mean this draw genuinely doesn't (re)bind fetch state at all
// and reuses whatever an earlier, uncaptured draw already set -- a
// different, still-useful result that would redirect the search toward
// that earlier draw instead.
// Round 16, ninth follow-up (native-rendering-phase3.md): fable2_079.log's
// fixed pre/post capture came back with pre=0xffffffff, post=0x00000000 --
// confirming the flush-clears theory outright -- and, decoding the 32 raw
// slot dumps by hand against the real xe_gpu_vertex_fetch_t layout, slot 31
// (register 0x48BA -- the same "odd slot" hardcoded in sub_82217EE8/
// sub_822060A0, see the fifth follow-up) held two real, sane-looking
// SHADER_CONSTANT_FETCH entries: type=kVertex, address in the same
// "canonical physical" 0x1fxxxxxx/0x1cxxxxxx address family already
// established as real guest memory by the candidate-A/B work, and a size
// (64000 bytes) that divides evenly by BOTH previously-confirmed candidate
// strides (16 and 32 -- see round 15's ground-truth stride evidence). This
// is the strongest concrete vertex-buffer candidate this whole
// investigation has produced. Added a real decode (not just the raw-word
// dump) plus an attempt to actually snapshot bytes from a decoded
// constant's address when it looks like a real vertex buffer, so the next
// test can show the actual vertex data instead of just its address.
//
// DiagIsChunkReadable/DiagSnapshotGuestBytes below are a direct, renamed
// port of fable_native_poc.cpp's IsChunkReadable/SnapshotGuestBytes
// (Round 15, thirteenth/fourteenth follow-ups there) -- NOT a call into
// that file's versions, because both live in an anonymous namespace there
// (internal linkage, not callable from this translation unit). Logic is
// unchanged: validate each small (16-byte) chunk is committed+readable
// before touching it (kernel_memory()->LookupHeap()->QueryRegionInfo()),
// __try/__except kept only as defense-in-depth, stop at the first
// unreadable chunk rather than guessing at a smaller size.
bool DiagIsChunkReadable(uint32_t addr, uint32_t num_bytes) {
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

uint32_t DiagSnapshotGuestBytes(uint8_t* base, uint32_t addr, uint32_t want_bytes, uint8_t* out) {
  std::memset(out, 0, want_bytes);
  if (addr == 0) return 0;
  constexpr uint32_t kDiagSnapshotChunkBytes = 16;
  uint32_t captured = 0;
  while (captured < want_bytes) {
    const uint32_t chunk = std::min<uint32_t>(kDiagSnapshotChunkBytes, want_bytes - captured);
    bool ok = false;
    if (DiagIsChunkReadable(addr + captured, chunk)) {
      __try {
        std::memcpy(out + captured, REX_RAW_ADDR(addr + captured), chunk);
        ok = true;
      } __except (EXCEPTION_EXECUTE_HANDLER) {
        ok = false;
      }
    }
    if (!ok) break;
    captured += chunk;
  }
  return captured;
}

// Round 16, twenty-sixth follow-up: a SECOND, separate snapshot path for
// addresses already known to be "canonical physical" (exactly what
// DiagDecodeFetchConstant's d.address is -- every fetch constant this file
// has ever decoded, constA/constB/constC alike). The function above reads
// through `base` (the recompiled PPC calling convention's guest VIRTUAL
// address base, REX_RAW_ADDR's own `base + addr` -- see fable2_pch.h) with
// a hand-derived "+0xDFFFF000" correction empirically reverse-engineered
// from ONE matched (alias,canonical) pair that happened to also be in
// constA's own 0x1f8dxxxx address family (round 16, tenth follow-up). That
// worked for constA every single time it's been tried, but constB has
// returned "0 bytes / not committed" in EVERY capture this whole
// investigation has ever taken, and constC (fable2_086.2.log, this round)
// just did too -- both are in a different address family (0x1c/0x1dxxxxxx)
// than the one the correction constant was derived from. Rather than
// guess at a second empirical constant for that family, use the SDK's own
// general-purpose physical-memory API instead: `rex::memory::Memory` (the
// same header/class fable_native_poc.cpp's own LogAddressDiagnostics
// already uses for exactly this kind of canonical-physical-address
// resolution) exposes `physical_membase()`/`TranslatePhysical()` -- a
// COMPLETELY SEPARATE host memory region from `virtual_membase()`/`base`
// (xmemory.h: "Base address of virtual memory... often 0x100000000" vs
// "Base address of physical memory... often 0x200000000") -- plus
// `GetPhysicalHeap()` for the matching QueryRegionInfo-based validity
// check. This is the exact mechanism `command_processor.cpp`'s own
// `LoadShader`/`ExecutePacketType3_IM_LOAD` uses to turn a canonical
// physical address into shader microcode bytes
// (`memory_->TranslatePhysical<uint32_t*>(addr)`), i.e. it is already
// proven, ReX-native, and not something this file is inventing.
// Round 18 (Fable2PC, forty-third follow-up): this gate is exactly what
// round 17 found and fixed on the rexgpu-xenos.dll side
// (command_processor.cpp's LogWatchedVertexFetchCapture) -- QueryRegionInfo
// checks the GUEST's own tracked allocator bookkeeping, not whether the
// underlying host memory is actually readable. D3D12SharedMemory::
// UploadRanges() never calls it at all; it just calls TranslatePhysical()
// and reads unconditionally, because TranslatePhysical() is bare pointer
// arithmetic into one bulk host allocation covering the entire guest
// physical address space (see round 17's writeup, native-rendering-
// phase3.md). Round 17's engine-side fix, tested twice
// (fable2_102.log/102.1.log), captured real, locally-coherent vertex data
// -- decoded by hand against the SAME shader's already-validated
// attribute layout, landing in the same magnitude range as an existing
// confirmed-good decode for that shader -- for constC addresses that this
// exact function had already reported as "0 bytes, not committed" both
// pre-draw and post-draw, and that the deferred-retry queue then spent a
// further 3 full seconds and 146,867+ attempts failing to read from this
// same thread. So: stop gating the read here too, for the same reason.
// DiagIsPhysicalChunkReadable is no longer called (kept, unused, as a
// record of what the old gate checked -- not deleted, in case a future
// round wants the query-only information it computes). The
// __try/__except below is unchanged and is the real safety net now, the
// same as it always technically was: TranslatePhysical() itself cannot
// fail, but keeping structured exception handling around the memcpy is
// a free, cheap belt-and-suspenders that costs nothing when (as expected)
// it never fires.
bool DiagIsPhysicalChunkReadable(rex::memory::Memory* mem, uint32_t phys_addr, uint32_t num_bytes) {
  if (!mem) return false;
  rex::memory::VirtualHeap* heap = mem->GetPhysicalHeap();
  if (!heap) return false;
  rex::memory::HeapAllocationInfo info{};
  if (!heap->QueryRegionInfo(phys_addr, &info)) return false;
  const bool committed = (info.state & rex::memory::kMemoryAllocationCommit) != 0;
  const bool readable = (info.protect & rex::memory::kMemoryProtectRead) != 0;
  if (!committed || !readable) return false;
  const uint64_t region_start = info.allocation_base;
  const uint64_t region_end = region_start + info.region_size;
  const uint64_t read_start = phys_addr;
  const uint64_t read_end = read_start + num_bytes;
  return read_start >= region_start && read_end <= region_end;
}

uint32_t DiagSnapshotGuestPhysicalBytes(uint32_t phys_addr, uint32_t want_bytes, uint8_t* out) {
  std::memset(out, 0, want_bytes);
  if (phys_addr == 0) return 0;
  rex::memory::Memory* mem = rex::system::kernel_memory();
  if (!mem) return 0;
  constexpr uint32_t kDiagSnapshotChunkBytes = 16;
  uint32_t captured = 0;
  while (captured < want_bytes) {
    const uint32_t chunk = std::min<uint32_t>(kDiagSnapshotChunkBytes, want_bytes - captured);
    // Round 18: unconditional read, matching UploadRanges() and round 17's
    // engine-side fix -- no DiagIsPhysicalChunkReadable gate. Still
    // wrapped in __try/__except as a safety net (unchanged from before).
    bool ok = false;
    __try {
      const uint8_t* phys_ptr = mem->TranslatePhysical<const uint8_t*>(phys_addr + captured);
      std::memcpy(out + captured, phys_ptr, chunk);
      ok = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
      ok = false;
    }
    if (!ok) break;
    captured += chunk;
  }
  return captured;
}

struct DiagFetchConstantDecoded {
  uint32_t type = 0;  // FetchConstantType: 0=kInvalidTexture 1=kInvalidVertex 2=kTexture 3=kVertex
  uint32_t address = 0;
  uint32_t size_bytes = 0;
};

DiagFetchConstantDecoded DiagDecodeFetchConstant(uint32_t dword0, uint32_t dword1) {
  DiagFetchConstantDecoded d;
  d.type = dword0 & 0x3u;
  d.address = dword0 & 0xFFFFFFFCu;  // dword0 >> 2 dwords, *4 back to bytes == mask low 2 bits
  d.size_bytes = ((dword1 >> 2) & 0xFFFFFFu) * 4u;
  return d;
}

// Log a decoded constant, and if it looks like a real, sizeable vertex
// buffer (type==kVertex(3), address above the trivial/near-null range,
// size at least one plausible vertex record), attempt to snapshot and hex
// dump its first bytes so the raw content (not just address/size) is
// visible in the log.
void LogAndMaybeSnapshotFetchConstant(uint8_t* base, uint32_t slot, const char* which,
                                       uint32_t dword0, uint32_t dword1) {
  DiagFetchConstantDecoded d = DiagDecodeFetchConstant(dword0, dword1);
  static constexpr const char* kTypeNames[4] = {"kInvalidTexture", "kInvalidVertex", "kTexture",
                                                 "kVertex"};
  REXLOG_INFO(
      "[FableDrawCapture] [FetchDecode] slot={} {} dword0={:#010x} dword1={:#010x} type={} "
      "address={:#x} size_bytes={}",
      slot, which, dword0, dword1, kTypeNames[d.type], d.address, d.size_bytes);
  constexpr uint32_t kMinPlausibleVertexAddr = 0x01000000u;
  constexpr uint32_t kMinPlausibleVertexSize = 16u;
  // Round 16, twenty-fifth follow-up: bumped 128 -> 224 (exactly 8 full
  // records at the now-authoritative 28-byte/7-dword stride for the
  // validated slot-31 binding -- see LogActiveVertexShaderBindingForSlot
  // and native-rendering-phase3.md. Still a small, fixed stack buffer;
  // DiagSnapshotGuestBytes is unchanged and remains chunked/safety-checked.
  constexpr uint32_t kSnapshotBytes = 224u;
  if (d.type != 3u || d.address < kMinPlausibleVertexAddr || d.size_bytes < kMinPlausibleVertexSize) {
    return;
  }
  // Round 16, twenty-sixth follow-up: try the proper physical-memory path
  // (DiagSnapshotGuestPhysicalBytes -- Memory::TranslatePhysical(), the
  // same mechanism ReX's own command_processor.cpp uses for this exact
  // "canonical physical address -> readable bytes" need) FIRST. Only if
  // that finds nothing does this fall back to the round-16-tenth-follow-up
  // empirical alias hack ("+0xDFFFF000" into virtual-address space) that
  // has only ever been confirmed to work for constA's specific address
  // family. This is address-translation robustness, not a data/format
  // heuristic: both paths are exact, general memory-translation APIs (one
  // ReX-native, one this file's own reverse-engineered fallback for
  // whatever constA's family turns out to need that the physical path
  // doesn't cover) -- neither one guesses at vertex content. Both attempts
  // are logged so which path a given address needed is visible, not just
  // whether the read succeeded.
  uint8_t snap[kSnapshotBytes] = {};
  uint32_t got = DiagSnapshotGuestPhysicalBytes(d.address, kSnapshotBytes, snap);
  const char* path_used = "physical";
  uint32_t alias_address = 0;
  if (got == 0) {
    // Round 16, tenth follow-up (fable2_080.log): derived from a matching
    // (alias, canonical) pair logged together in fable2_078.log:
    // alias=0xff8d5c70 <-> canonical=0x1f8d6c70 for the SAME location,
    // giving alias = canonical + 0xE0000000 - 0x1000 = canonical +
    // 0xDFFFF000.
    constexpr uint32_t kCanonicalToAliasDelta = 0xDFFFF000u;
    alias_address = d.address + kCanonicalToAliasDelta;
    got = DiagSnapshotGuestBytes(base, alias_address, kSnapshotBytes, snap);
    path_used = "alias-fallback";
  }
  if (got == 0) {
    REXLOG_INFO(
        "[FableDrawCapture] [FetchDecode]   slot={} {} address={:#x}: snapshot got 0 bytes via "
        "BOTH the physical path and the alias fallback (tried alias={:#x}) -- not "
        "committed/readable at capture time via either mechanism",
        slot, which, d.address, alias_address);
    return;
  }
  // Log as 32-bit words (big-endian guest values, matching REX_LOAD_U32's
  // own byte-swap) so this can be read directly against the stride-16/
  // stride-32 candidate decode already established elsewhere in this file.
  std::string hex;
  for (uint32_t i = 0; i + 4 <= got; i += 4) {
    uint32_t w = (static_cast<uint32_t>(snap[i]) << 24) | (static_cast<uint32_t>(snap[i + 1]) << 16) |
                 (static_cast<uint32_t>(snap[i + 2]) << 8) | static_cast<uint32_t>(snap[i + 3]);
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%08x ", w);
    hex += buf;
  }
  REXLOG_INFO(
      "[FableDrawCapture] [FetchDecode]   slot={} {} address={:#x} via={} snapshot({} bytes)={}",
      slot, which, d.address, path_used, got, hex);
}

// Round 16, twentieth follow-up: for a given SHADER_CONSTANT_FETCH slot,
// read ReX's OWN already-computed vertex-binding info off the shader that
// is bound on the CommandProcessor at this exact draw boundary -- NOT a
// second microcode parser and NOT a raw-memory scan. Shader::AnalyzeUcode()
// (called from PipelineCache::LoadShader() in both the D3D12 and Vulkan
// backends, before LoadShader() ever returns, so this is guaranteed
// populated -- no race) has already walked the real vfetch_full/vfetch_mini
// instructions via ParseVertexFetchInstruction() and recorded, per fetch
// constant, the authoritative VertexFetchInstruction::stride() value as
// VertexBinding::stride_words, plus per-attribute format/offset metadata.
// Backend confirmed D3D12 for this build (pc/xenia-canary.config.toml:
// gpu = "d3d12"; corroborated by an actual runtime log line, fable2_009
// .log: "[gpu] D3D12 draw resolution scaling is enabled..."), but the
// lookup below only downcasts to the common rex::graphics::GraphicsSystem
// base (command_processor() lives there, not on the D3D12-specific
// subclass), so it works unchanged if that config or the runtime instance
// selection ever differs from what a plain read of the config implies.
//
// Called synchronously from LogFetchTable("pre", ...), i.e. before
// __imp__sub_8221E0F0 runs -- the same draw-boundary point every other
// piece of this capture is causally tied to. Nothing here is deferred to
// overlay/UI time.
// Round 16, twenty-ninth follow-up (native-rendering-phase3.md): holds the
// most recently found const_index_sel==2 ("constC") binding for slot 31,
// so RetryPostDrawSlot31Snapshot() (which runs strictly later in the same
// draw's capture sequence -- see that function's own comment) can pair a
// freshly succeeded constC byte snapshot with the SAME draw's real
// ReX-derived attribute layout, instead of guessing at one. Cleared at the
// top of every LogActiveVertexShaderBindingForSlot() call (including its
// early-return paths) so a draw with no slot-31 binding, or no vertex
// shader at all, never leaves a stale, mismatched previous draw's metadata
// sitting here for a later unrelated snapshot to pick up. Plain global
// (not atomic/mutex-guarded): this whole capture pipeline already runs
// synchronously, single-threaded, within one recompiled-PPC hook
// invocation, exactly like every other piece of per-draw state in this
// file (e.g. the SHADER_CONSTANT_FETCH shadow table reads above).
fable2::native_poc::ValidatedFetchConstantVertexInfo g_slot31_constC_binding;

void LogActiveVertexShaderBindingForSlot(uint32_t fetch_constant_slot) {
  // Round 16, twenty-ninth follow-up: invalidate up front so every
  // early-return path below (no kernel_state, no vertex shader, slot
  // ABSENT from this shader's bindings, etc.) leaves this correctly empty
  // rather than stale from whatever draw last populated it.
  if (fetch_constant_slot == 31u) {
    g_slot31_constC_binding = fable2::native_poc::ValidatedFetchConstantVertexInfo{};
  }
  rex::system::KernelState* kstate = rex::system::kernel_state();
  if (!kstate) {
    REXLOG_INFO("[FableDrawCapture] [ShaderBinding] slot={} kernel_state() is null", fetch_constant_slot);
    return;
  }
  rex::Runtime* runtime = kstate->emulator();
  if (!runtime) {
    REXLOG_INFO("[FableDrawCapture] [ShaderBinding] slot={} KernelState::emulator() is null",
                fetch_constant_slot);
    return;
  }
  rex::system::IGraphicsSystem* igfx = runtime->graphics_system();
  if (!igfx) {
    REXLOG_INFO("[FableDrawCapture] [ShaderBinding] slot={} Runtime::graphics_system() is null",
                fetch_constant_slot);
    return;
  }
  auto* gfx = dynamic_cast<rex::graphics::GraphicsSystem*>(igfx);
  if (!gfx) {
    REXLOG_INFO(
        "[FableDrawCapture] [ShaderBinding] slot={} IGraphicsSystem is not a "
        "rex::graphics::GraphicsSystem -- unexpected backend, cannot reach CommandProcessor",
        fetch_constant_slot);
    return;
  }
  rex::graphics::CommandProcessor* cp = gfx->command_processor();
  if (!cp) {
    REXLOG_INFO("[FableDrawCapture] [ShaderBinding] slot={} GraphicsSystem::command_processor() is null",
                fetch_constant_slot);
    return;
  }
  rex::graphics::Shader* vs = cp->active_vertex_shader();
  if (!vs) {
    REXLOG_INFO(
        "[FableDrawCapture] [ShaderBinding] slot={} CommandProcessor::active_vertex_shader() is null "
        "(no vertex shader bound at this draw boundary)",
        fetch_constant_slot);
    return;
  }

  const auto& bindings = vs->vertex_bindings();
  REXLOG_INFO(
      "[FableDrawCapture] [ShaderBinding] slot={} active_vertex_shader ucode_hash={:#018x} "
      "vertex_bindings_count={}",
      fetch_constant_slot, vs->ucode_data_hash(), bindings.size());

  // Round 16, twenty-second follow-up (native-rendering-phase3.md): the
  // first attempt at this match compared Shader::VertexBinding::fetch_constant
  // directly against our own shadow-table row index ("slot", 0-31) using
  // exact equality. That is WRONG -- confirmed directly from the SDK's own
  // doc comment on VertexFetchInstruction::fetch_constant_index() (ucode.h):
  // "Vertex fetch constant index [0-95]", computed as
  // `data_.const_index * 3 + data_.const_index_sel` (const_index: 5 bits,
  // 0-31; const_index_sel: 2 bits). VertexBinding::fetch_constant is set
  // directly from this 0-95 value (translator.cpp), NOT from const_index
  // alone. Our own "slot" numbering (0-31, from the 32-row/24-byte-per-row
  // SHADER_CONSTANT_FETCH shadow table established in earlier rounds) is
  // const_index space, not fetch_constant_index() space. The two are related
  // by fetch_constant_index() == const_index*3 + const_index_sel, so a
  // shader instruction addressing our slot 31 can show up as ReX
  // fetch_constant 93, 94, OR 95 (31*3 + 0/1/2) -- never literally "31"
  // unless const_index happens to be 10 (10*3+1==31), a different hardware
  // slot entirely. The very first real-world capture this diagnostic ever
  // produced (fable2_083.log, frame=7301) hit exactly this: it reported
  // fetch_constant=95 as the shader's only binding and was incorrectly
  // logged as "ABSENT" for slot=31, when 95 / 3 == 31 -- it WAS our slot,
  // just reported in the wrong space by the old exact-equality check.
  std::vector<const rex::graphics::Shader::VertexBinding*> matches;
  for (const auto& binding : bindings) {
    if (binding.fetch_constant / 3u == fetch_constant_slot) {
      matches.push_back(&binding);
    }
  }
  if (matches.empty()) {
    // Per instructions: this is new evidence about draw/shader correlation,
    // not a reason to fall back to heuristic decoding. Log exactly what
    // fetch_constant values ARE present (translated back to const_index/sel
    // so they're directly comparable to our own slot numbering) so it's
    // visible whether slot 31 is simply not used by this particular shader,
    // or something upstream (which shader is "active" vs. which fetch
    // constant this draw's geometry actually reads) is mismatched.
    std::string present;
    for (const auto& binding : bindings) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%u(const_index=%u,sel=%u) ", binding.fetch_constant,
                    binding.fetch_constant / 3u, binding.fetch_constant % 3u);
      present += buf;
    }
    REXLOG_INFO(
        "[FableDrawCapture] [ShaderBinding] slot={} ABSENT from active_vertex_shader()'s "
        "vertex_bindings() -- present fetch_constant values: [ {}]",
        fetch_constant_slot, present);
    return;
  }

  for (const auto* match : matches) {
    REXLOG_INFO(
        "[FableDrawCapture] [ShaderBinding] slot={} FOUND binding_index={} fetch_constant={} "
        "(const_index={} const_index_sel={}) stride_words={} stride_bytes={} attribute_count={}",
        fetch_constant_slot, match->binding_index, match->fetch_constant,
        match->fetch_constant / 3u, match->fetch_constant % 3u, match->stride_words,
        match->stride_words * 4u, match->attributes.size());
    for (size_t i = 0; i < match->attributes.size(); ++i) {
      const auto& fi = match->attributes[i].fetch_instr;
      REXLOG_INFO(
          "[FableDrawCapture] [ShaderBinding]   slot={} attr[{}] is_mini_fetch={} "
          // Attributes::offset has no unit documented in shader.h (unlike
          // ::stride, which is explicitly "In dwords.") -- logging the raw
          // field value under its own name rather than asserting a unit.
          "data_format={} offset={} exp_adjust={} is_signed={} is_integer={} "
          "used_result_components={:#x}",
          fetch_constant_slot, i, fi.is_mini_fetch, static_cast<uint32_t>(fi.attributes.data_format),
          fi.attributes.offset, fi.attributes.exp_adjust, fi.attributes.is_signed,
          fi.attributes.is_integer, fi.result.GetUsedResultComponents());
    }

    // Round 16, twenty-ninth follow-up: stash the const_index_sel==2
    // ("constC") binding specifically -- fetch_constant % 3u == 2 -- since
    // that is the one this whole investigation's runtime evidence (message
    // 1's original spec, and every real capture since) ties to the
    // slot-31 vertex stream. A shader can report more than one binding for
    // this same slot (e.g. the 3-binding case, fable2_088.log
    // ucode_hash=0x87d4404fb36af71d: fetch_constant 93/94/95, sel 0/1/2 --
    // only 95/sel=2 is the one RetryPostDrawSlot31Snapshot's "constC" bytes
    // actually correspond to); binding_index/sel 0 or 1 matches in
    // `matches` are logged above like any other match but deliberately not
    // handed off here.
    if (fetch_constant_slot == 31u && match->fetch_constant % 3u == 2u) {
      fable2::native_poc::ValidatedFetchConstantVertexInfo& out = g_slot31_constC_binding;
      out.stride_words = match->stride_words;
      out.ucode_hash = vs->ucode_data_hash();
      out.attribute_count = static_cast<int>(std::min<size_t>(
          match->attributes.size(), fable2::native_poc::kMaxValidatedFetchConstantAttributes));
      for (int i = 0; i < out.attribute_count; ++i) {
        const auto& fi = match->attributes[static_cast<size_t>(i)].fetch_instr;
        fable2::native_poc::ValidatedFetchConstantAttribute& a = out.attributes[i];
        a.data_format = static_cast<uint32_t>(fi.attributes.data_format);
        a.offset_dwords = fi.attributes.offset;
        a.is_mini_fetch = fi.is_mini_fetch;
        a.is_signed = fi.attributes.is_signed;
        a.is_integer = fi.attributes.is_integer;
        a.used_result_components = fi.result.GetUsedResultComponents();
      }
      // `valid`/address/snapshot are deliberately NOT set here -- this only
      // records the shader-side layout. RetryPostDrawSlot31Snapshot() below
      // fills in the address+bytes and sets valid=true only once both
      // halves are present for the same draw; see its own comment.

      // Round 20 (Fable2PC, native 3D draw path): capture this same
      // shader's real, currently-bound float constant register values --
      // the vertex shader's own transform-constant data, whatever it
      // turns out to be -- at this exact same draw boundary, using this
      // exact same `vs`/`cp` already resolved above. See
      // ValidatedFetchConstantVertexInfo::float_constant_* in
      // fable_native_poc.h for what this does and, explicitly, does not
      // claim about the result.
      const rex::graphics::RegisterFile* regs = cp->register_file();
      if (!regs) {
        REXLOG_INFO(
            "[FableDrawCapture] [ShaderBinding] slot={} CommandProcessor::register_file() is null "
            "-- cannot capture float constant register values for this draw",
            fetch_constant_slot);
      } else {
        // Fix (build error, screenshot): SQ_VS_CONST is declared inside
        // registers.h's nested `namespace reg` (lines 60-963), NOT directly
        // in rex::graphics -- a real scoping mistake on my part, same class
        // of error as fable_draw_capture.cpp's earlier g_sequence/
        // fable2::diag qualification bug. XE_GPU_REG_* enum values (used
        // elsewhere below) ARE directly in rex::graphics -- only the
        // SQ_VS_CONST/SQ_PS_CONST bitfield TYPES need the extra reg::.
        const rex::graphics::reg::SQ_VS_CONST sq_vs_const =
            regs->Get<rex::graphics::reg::SQ_VS_CONST>();
        const auto& crm = vs->constant_register_map();
        // Round 20, eleventh follow-up (view/projection reconstruction):
        // count the TOTAL number of logical registers this shader flags as
        // used, independent of the capture cap below, purely so a
        // mismatch between this total and what actually gets captured is
        // directly visible in the log -- see
        // ValidatedFetchConstantVertexInfo::kMaxCapturedFloatConstants's
        // own comment for why this matters (a real, evidenced truncation
        // risk at the old cap of 32, not a theoretical one).
        uint32_t total_flagged = 0;
        for (uint32_t logical = 0; logical < 256; ++logical) {
          const uint32_t block = logical / 64u;
          const uint32_t bit = logical % 64u;
          if (crm.float_bitmap[block] & (uint64_t(1) << bit)) ++total_flagged;
        }
        out.float_constant_count = 0;
        for (uint32_t logical = 0; logical < 256 && out.float_constant_count <
                                                          fable2::native_poc::ValidatedFetchConstantVertexInfo::
                                                              kMaxCapturedFloatConstants;
             ++logical) {
          const uint32_t block = logical / 64u;
          const uint32_t bit = logical % 64u;
          if (!(crm.float_bitmap[block] & (uint64_t(1) << bit))) continue;
          const uint32_t physical = sq_vs_const.base + logical;
          const int slot = out.float_constant_count++;
          out.float_constant_logical_index[slot] = logical;
          for (int comp = 0; comp < 4; ++comp) {
            out.float_constant_value[slot][comp] =
                regs->Get<float>(rex::graphics::XE_GPU_REG_SHADER_CONSTANT_000_X + 4u * physical +
                                  static_cast<uint32_t>(comp));
          }
        }
        if (static_cast<uint32_t>(out.float_constant_count) < total_flagged) {
          REXLOG_INFO(
              "[FableDrawCapture] [ShaderBinding] slot={} [FloatConstants] TRUNCATED: this "
              "shader flags {} total logical float registers as used, but only the first {} "
              "(kMaxCapturedFloatConstants) were captured -- {} were dropped, in ascending "
              "logical-index order. If a view/projection matrix is not found among the captured "
              "set for this shader, this is why -- it may be among the {} dropped registers.",
              fetch_constant_slot, total_flagged, out.float_constant_count,
              total_flagged - static_cast<uint32_t>(out.float_constant_count), total_flagged);
        }
        REXLOG_INFO(
            "[FableDrawCapture] [ShaderBinding] slot={} [FloatConstants] sq_vs_const.base={} "
            "shader float_count={} float_dynamic_addressing={} captured={} (raw register data "
            "only -- no claim yet about which, if any, of these is a transform matrix; see "
            "native-rendering-phase3.md round 20)",
            fetch_constant_slot, sq_vs_const.base, crm.float_count, crm.float_dynamic_addressing,
            out.float_constant_count);
        for (int i = 0; i < out.float_constant_count; ++i) {
          REXLOG_INFO(
              "[FableDrawCapture] [ShaderBinding]   slot={} [FloatConstants] c[{}] (logical, "
              "physical={}) = ({:.6g}, {:.6g}, {:.6g}, {:.6g})",
              fetch_constant_slot, out.float_constant_logical_index[i],
              sq_vs_const.base + out.float_constant_logical_index[i], out.float_constant_value[i][0],
              out.float_constant_value[i][1], out.float_constant_value[i][2],
              out.float_constant_value[i][3]);
        }

        // Round 20, sixteenth follow-up (view/projection reconstruction,
        // new angle -- user asked "Then what now?" after the vertex-
        // constant search found zero genuine perspective candidates
        // across ~6 real sessions of hardening). The Xenos/R600 GPU
        // implements the FINAL NDC-to-screen-pixel step in fixed-
        // function hardware registers, downstream of whatever the
        // vertex shader computes for clip-space position:
        // PA_CL_VPORT_XSCALE/XOFFSET/YSCALE/YOFFSET/ZSCALE/ZOFFSET (plain
        // floats) plus PA_CL_VTE_CNTL/PA_CL_CLIP_CNTL (bitfield enable/
        // mode controls). Confirmed real and load-bearing: ReXGlue's own
        // D3D12 command_processor.cpp and draw.cpp's GetHostViewportInfo()
        // read these exact registers to reconstruct the host viewport.
        // Logging them here is explicitly NOT a claim that this reveals
        // the view/projection matrix -- it is a separate, later stage
        // than the vertex shader's own transform. It gives an
        // independent, ground-truth cross-check instead: the viewport
        // scale/offset values imply the render-target resolution and
        // aspect ratio actually in effect for this draw, and
        // PA_CL_CLIP_CNTL's dx_clip_space_def bit tells us which Z/depth
        // convention (D3D vs OpenGL-style) the hardware expects -- both
        // useful later when validating any future projection-matrix
        // candidate's implied near/far planes. See
        // native-rendering-phase3.md round 20, sixteenth follow-up.
        const float vport_xscale =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XSCALE);
        const float vport_xoffset =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_XOFFSET);
        const float vport_yscale =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YSCALE);
        const float vport_yoffset =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_YOFFSET);
        const float vport_zscale =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZSCALE);
        const float vport_zoffset =
            regs->Get<float>(rex::graphics::XE_GPU_REG_PA_CL_VPORT_ZOFFSET);
        // Bitfield TYPES need the extra reg:: namespace, same as
        // SQ_VS_CONST above.
        const rex::graphics::reg::PA_CL_VTE_CNTL vte_cntl =
            regs->Get<rex::graphics::reg::PA_CL_VTE_CNTL>();
        const rex::graphics::reg::PA_CL_CLIP_CNTL clip_cntl =
            regs->Get<rex::graphics::reg::PA_CL_CLIP_CNTL>();
        REXLOG_INFO(
            "[FableDrawCapture] [ShaderBinding] slot={} [Viewport] PA_CL_VPORT "
            "xscale={:.6g} xoffset={:.6g} yscale={:.6g} yoffset={:.6g} zscale={:.6g} "
            "zoffset={:.6g} | PA_CL_VTE_CNTL vport_x_scale_ena={} vport_x_offset_ena={} "
            "vport_y_scale_ena={} vport_y_offset_ena={} vport_z_scale_ena={} "
            "vport_z_offset_ena={} vtx_xy_fmt={} vtx_z_fmt={} vtx_w0_fmt={} | "
            "PA_CL_CLIP_CNTL dx_clip_space_def={} clip_disable={} (this is the "
            "hardware's downstream NDC->pixel transform, NOT the vertex shader's own "
            "view/projection matrix -- see native-rendering-phase3.md round 20, "
            "sixteenth follow-up)",
            fetch_constant_slot, vport_xscale, vport_xoffset, vport_yscale, vport_yoffset,
            vport_zscale, vport_zoffset, vte_cntl.vport_x_scale_ena,
            vte_cntl.vport_x_offset_ena, vte_cntl.vport_y_scale_ena,
            vte_cntl.vport_y_offset_ena, vte_cntl.vport_z_scale_ena,
            vte_cntl.vport_z_offset_ena, vte_cntl.vtx_xy_fmt, vte_cntl.vtx_z_fmt,
            vte_cntl.vtx_w0_fmt, clip_cntl.dx_clip_space_def, clip_cntl.clip_disable);
      }
    }
  }
}

// Round 16, twenty-seventh follow-up: constB has failed to snapshot in
// EVERY capture this whole investigation has ever taken (many sessions),
// and constC has now failed twice in a row too -- via BOTH the alias hack
// AND the correct physical-memory path added last round. That consistency
// (not intermittent) is the signal that this isn't "try again and hope":
// something about these specific per-draw dynamic buffers isn't committed
// at the exact moment `LogFetchTable("pre", ...)` reads them, which is
// BEFORE `__imp__sub_8221E0F0` (the real, original low-level draw/flush
// call) runs. One concrete, testable hypothesis: whatever actually
// commits/uploads these buffers' backing memory happens INSIDE that real
// call (plausible -- this file's own resource-call-counting machinery
// elsewhere already shows this hook's neighborhood is where vertex-buffer
// resolution against ReX's resource cache happens), so re-reading and
// re-attempting the snapshot AFTER `__imp__sub_8221E0F0` returns might
// succeed where the "pre" attempt did not. This is a genuinely different
// mechanism from the physical-memory-path fix (which addressed HOW an
// address is translated; this addresses WHEN it's read), not a repeat of
// the same fix. If this ALSO fails, that rules out a timing gap within
// this one function call and points to something later/async instead
// (e.g. an actual GPU-side upload that hasn't happened by the time this
// whole hook returns) -- itself a useful, reportable answer, not nothing.
//
// Deliberately bypasses LogFetchTable's own fetch_mask==0 early-return
// (confirmed, every single capture: the dirty mask is always clear by the
// "post" pass -- that's a CPU-side "this register changed" bookkeeping
// bit, unrelated to whether the GPU-visible memory a fetch constant
// already pointed to is resident) by reading slot 31's words directly
// here instead of going through LogFetchTable("post", ...) at all.
// Round 16, thirty-eighth follow-up: "so what do you want me to do, play
// the whole game hoping something shows up?" -- research, not more play,
// answered this: traced the REAL renderer's own vertex-buffer-residency
// path (tools/rexglue-source/src/graphics/d3d12/command_processor.cpp,
// D3D12CommandProcessor::IssueDraw(), ~line 2596) and found it resolves
// each vfetch constant straight from the live GPU register file and calls
// SharedMemory::RequestRange() to GUARANTEE the backing guest memory is
// uploaded/resident before the draw proceeds -- that's the authoritative,
// always-correct path; our own guest-memory snapshot here is a parallel,
// unsynchronized read that races it. Calling RequestRange() ourselves
// isn't reachable cleanly: D3D12CommandProcessor::shared_memory_ is
// private with no public accessor (TextureCache::shared_memory() is the
// one public accessor that exists anywhere in the shipped SDK headers,
// but CommandProcessor::texture_cache() isn't exposed either) -- reaching
// it would mean a private-member offset hack or rebuilding rexgpu-xenos*
// .dll from tools/rexglue-source with a new public accessor added, a much
// bigger undertaking than anything in this file. See
// native-rendering-phase3.md's round 16 thirty-eighth follow-up for the
// full write-up, including why that bigger rebuild is a real, buildable
// option (rexglue-source is a genuine local CMake project, not just
// reference text) but a separate decision from this file's normal
// additive-diagnostic-only scope.
//
// This is the cheap, purely-local experiment that write-up leads to: a
// bounded, sustained retry that keeps re-attempting a constC address that
// failed its own draw's immediate pre/post snapshot (RetryPostDrawSlot31
// Snapshot below), across MANY SUBSEQUENT sub_8221E0F0 calls -- not just
// the one draw it first failed on. Deliberately runs unconditionally (see
// its call site in sub_8221E0F0, before the capture_active early-return)
// because the immediate-retry evidence so far shows the "arm" window
// usually closes after exactly one draw (CaptureAndValidate's own
// OnValidatedDraw latch fires on the very first candidate almost every
// time), so confining retries to within one arm window would give this
// close to zero extra chances -- __imp__sub_8221E0F0 itself still runs on
// every call regardless of capture_active, so a shader's buffer can
// legitimately become resident on a LATER call than the one that first
// tried to snapshot it, even with the diagnostic's own capture window
// already closed.
struct PendingSlot31Retry {
  bool in_use = false;
  uint32_t address = 0;
  // Round 16, thirty-ninth follow-up: fable2_096.1.log caught the original
  // call-count budget (64 sub_8221E0F0 calls) being exhausted in under 2ms
  // of real time -- "enqueued" and "giving up" logged one millisecond
  // apart. This hook evidently fires in tight bursts (many draws per
  // frame), not steadily over time, so a call count is not a meaningful
  // stand-in for elapsed time -- it never actually tested whether a
  // longer WALL-CLOCK wait helps. Switched to a real QueryPerformanceCounter
  // deadline instead (see kSlot31RetryDurationMs below), which is what the
  // thirty-eighth follow-up's hypothesis actually needs tested.
  LONGLONG deadline_qpc = 0;
  int attempts_made = 0;  // diagnostic only, logged on success/give-up
  // Binding metadata captured at enqueue time (g_slot31_constC_binding is
  // overwritten on every subsequent draw's LogActiveVertexShaderBindingFor
  // Slot(31) call, so this must be a copy, not a reference to the global).
  fable2::native_poc::ValidatedFetchConstantVertexInfo binding;
};
constexpr int kMaxPendingSlot31Retries = 4;
constexpr LONGLONG kSlot31RetryDurationMs = 3000;
PendingSlot31Retry g_pending_slot31_retries[kMaxPendingSlot31Retries];

LONGLONG QpcNow() {
  LARGE_INTEGER qpc{};
  QueryPerformanceCounter(&qpc);
  return qpc.QuadPart;
}

LONGLONG QpcMillisecondsFromNow(LONGLONG ms) {
  LARGE_INTEGER freq{};
  QueryPerformanceFrequency(&freq);
  return QpcNow() + (ms * freq.QuadPart) / 1000;
}

void EnqueuePendingSlot31Retry(uint32_t address,
                               const fable2::native_poc::ValidatedFetchConstantVertexInfo& binding) {
  for (const PendingSlot31Retry& p : g_pending_slot31_retries) {
    if (p.in_use && p.address == address) return;  // already pending, don't duplicate
  }
  for (PendingSlot31Retry& p : g_pending_slot31_retries) {
    if (!p.in_use) {
      p.in_use = true;
      p.address = address;
      p.deadline_qpc = QpcMillisecondsFromNow(kSlot31RetryDurationMs);
      p.attempts_made = 0;
      p.binding = binding;
      REXLOG_INFO(
          "[FableDrawCapture] [DeferredRetry] slot=31 constC address={:#x} enqueued for up to {} ms "
          "of real time across however many sub_8221E0F0 calls that spans (immediate pre/post "
          "attempt failed; see this queue's own comment for why a later call might still succeed)",
          address, kSlot31RetryDurationMs);
      return;
    }
  }
  // All slots already full -- drop silently rather than evict an existing
  // one; the next resolution (success or exhaustion) frees a slot up.
}

// Called unconditionally on every sub_8221E0F0 call (see call site) so a
// pending address gets checked on calls that happen well after the draw
// that first failed to snapshot it, including calls where capture isn't
// currently armed at all -- deliberately independent of capture_active,
// see this queue's own comment above for why.
void RetryPendingSlot31Snapshots(uint8_t* base) {
  // Round 20, native-3D-draw-path follow-up ("widen the snapshot window"):
  // this feeds ValidatedFetchConstantVertexInfo::snapshot via the same
  // OnValidatedFetchConstantVertexStream() hand-off as
  // RetryPostDrawSlot31Snapshot() below, so it must use the SAME size --
  // reference the struct's own constant instead of a separate local
  // literal (there were three independent copies of "224u" in this file
  // before this round; see kValidatedFetchConstantSnapshotBytes's own
  // comment in fable_native_poc.h for why 16384, and why this one and
  // RetryPostDrawSlot31Snapshot's copy need to move together while
  // LogAndMaybeSnapshotFetchConstant's own copy deliberately does not --
  // that one is a generic, high-volume diagnostic hex-dump path unrelated
  // to this struct, and growing it too would bloat every fetch-constant
  // log line with up to 16KB of hex per line for no benefit).
  constexpr uint32_t kSnapshotBytes = fable2::native_poc::kValidatedFetchConstantSnapshotBytes;
  for (PendingSlot31Retry& p : g_pending_slot31_retries) {
    if (!p.in_use) continue;
    uint8_t snap[kSnapshotBytes] = {};
    uint32_t got = DiagSnapshotGuestPhysicalBytes(p.address, kSnapshotBytes, snap);
    const char* path_used = "physical";
    if (got == 0) {
      constexpr uint32_t kCanonicalToAliasDelta = 0xDFFFF000u;
      got = DiagSnapshotGuestBytes(base, p.address + kCanonicalToAliasDelta, kSnapshotBytes, snap);
      path_used = "alias-fallback";
    }
    ++p.attempts_made;
    if (got == 0) {
      if (QpcNow() >= p.deadline_qpc) {
        REXLOG_INFO(
            "[FableDrawCapture] [DeferredRetry] slot=31 constC address={:#x}: giving up after {} ms "
            "({} attempts), still 0 bytes via both paths -- this buffer never became resident this "
            "window",
            p.address, kSlot31RetryDurationMs, p.attempts_made);
        p.in_use = false;
      }
      continue;
    }
    std::string hex;
    for (uint32_t i = 0; i + 4 <= got; i += 4) {
      uint32_t w = (static_cast<uint32_t>(snap[i]) << 24) | (static_cast<uint32_t>(snap[i + 1]) << 16) |
                   (static_cast<uint32_t>(snap[i + 2]) << 8) | static_cast<uint32_t>(snap[i + 3]);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%08x ", w);
      hex += buf;
    }
    REXLOG_INFO(
        "[FableDrawCapture] [DeferredRetry] slot=31 constC address={:#x} via={} SUCCEEDED on a "
        "later sub_8221E0F0 call ({} attempts in) where the same draw's own immediate attempt did "
        "not -- snapshot({} bytes)={}",
        p.address, path_used, p.attempts_made, got, hex);
    if (p.binding.attribute_count > 0) {
      fable2::native_poc::ValidatedFetchConstantVertexInfo info = p.binding;
      info.address = p.address;
      info.snapshot_bytes_captured =
          std::min<uint32_t>(got, fable2::native_poc::kValidatedFetchConstantSnapshotBytes);
      std::memcpy(info.snapshot, snap, info.snapshot_bytes_captured);
      info.valid = true;
      fable2::native_poc::OnValidatedFetchConstantVertexStream(info);
    }
    p.in_use = false;
  }
}

void RetryPostDrawSlot31Snapshot(uint8_t* base, uint32_t self, uint32_t old_cursor) {
  const uint32_t slot_base = self + fable2::diag::kOffFetchShadowTable +
                              31u * fable2::diag::kFetchShadowSlotStride;
  uint32_t words[8] = {};
  for (int i = 0; i < 8; ++i) {
    words[i] = REX_LOAD_U32(slot_base - 4 + static_cast<uint32_t>(i) * 4);
  }
  static constexpr const char* kTypeNames[4] = {"kInvalidTexture", "kInvalidVertex", "kTexture",
                                                 "kVertex"};
  struct Attempt {
    const char* which;
    uint32_t dword0;
    uint32_t dword1;
  };
  const Attempt attempts[2] = {
      {"constB", words[4], words[5]},
      {"constC", words[6], words[7]},
  };
  constexpr uint32_t kMinPlausibleVertexAddr = 0x01000000u;
  constexpr uint32_t kMinPlausibleVertexSize = 16u;
  // Round 20, native-3D-draw-path follow-up: see
  // RetryPendingSlot31Snapshots's own comment above -- this is the other
  // (and, in practice, the main/hot) path that feeds
  // ValidatedFetchConstantVertexInfo::snapshot, so it must track the same
  // constant.
  constexpr uint32_t kSnapshotBytes = fable2::native_poc::kValidatedFetchConstantSnapshotBytes;
  for (const Attempt& a : attempts) {
    DiagFetchConstantDecoded d = DiagDecodeFetchConstant(a.dword0, a.dword1);
    if (d.type != 3u || d.address < kMinPlausibleVertexAddr || d.size_bytes < kMinPlausibleVertexSize) {
      REXLOG_INFO(
          "[FableDrawCapture] [FetchDecode-PostDraw] slot=31 {} dword0={:#010x} dword1={:#010x} "
          "type={} address={:#x} size_bytes={} -- not a plausible vertex buffer, skipping retry",
          a.which, a.dword0, a.dword1, kTypeNames[d.type], d.address, d.size_bytes);
      continue;
    }
    uint8_t snap[kSnapshotBytes] = {};
    uint32_t got = DiagSnapshotGuestPhysicalBytes(d.address, kSnapshotBytes, snap);
    const char* path_used = "physical";
    uint32_t alias_address = 0;
    if (got == 0) {
      constexpr uint32_t kCanonicalToAliasDelta = 0xDFFFF000u;
      alias_address = d.address + kCanonicalToAliasDelta;
      got = DiagSnapshotGuestBytes(base, alias_address, kSnapshotBytes, snap);
      path_used = "alias-fallback";
    }
    if (got == 0) {
      REXLOG_INFO(
          "[FableDrawCapture] [FetchDecode-PostDraw] slot=31 {} address={:#x}: STILL 0 bytes via "
          "both paths after __imp__sub_8221E0F0 returned (tried alias={:#x}) -- not a within-this-"
          "call timing gap; the buffer is not resident even after the real draw call completes",
          a.which, d.address, alias_address);
      // Round 16, thirty-eighth follow-up: constC (with real binding
      // metadata to pair it with) gets one more chance, spread across many
      // LATER sub_8221E0F0 calls -- see EnqueuePendingSlot31Retry's own
      // comment. constB is deliberately excluded: it has never once
      // succeeded in this whole investigation and has no binding metadata
      // to pair with even if it did, so queuing it would just burn a slot.
      // Round 16, fortieth follow-up: the thirty-ninth follow-up's own
      // 3-second/150,000+-attempt retry conclusively ruled out "just needs
      // more time" for this address family (native-rendering-phase3.md).
      // So THIS is not another wait -- it arms a one-shot watch inside
      // rexgpu-xenos.dll itself (gpu_fetch_watch_address, see that cvar's
      // comment in tools/rexglue-source/.../d3d12/command_processor.cpp),
      // set via the generic cross-DLL rex::cvar::SetFlagByName so no new
      // exported symbol is needed. It fires once, at the exact point the
      // real renderer's own SharedMemory::RequestRange() resolves this
      // SAME address for this SAME shader, tagged [GpuFetchWatch] in the
      // log -- a direct, apples-to-apples test of whether the identical
      // TranslatePhysical() mechanism succeeds there where it fails here.
      if (std::strcmp(a.which, "constC") == 0 && g_slot31_constC_binding.attribute_count > 0) {
        EnqueuePendingSlot31Retry(d.address, g_slot31_constC_binding);
        const bool armed =
            rex::cvar::SetFlagByName("gpu_fetch_watch_address", std::to_string(d.address));
        REXLOG_INFO(
            "[FableDrawCapture] [GpuFetchWatchArm] slot=31 constC address={:#x} armed={} -- "
            "watching for rexgpu-xenos.dll's own SharedMemory::RequestRange() resolution of "
            "this exact address (tagged [GpuFetchWatch] when it fires); armed=false means the "
            "cvar wasn't found/set and this whole experiment didn't run this time",
            d.address, armed);
      }
      continue;
    }
    std::string hex;
    for (uint32_t i = 0; i + 4 <= got; i += 4) {
      uint32_t w = (static_cast<uint32_t>(snap[i]) << 24) | (static_cast<uint32_t>(snap[i + 1]) << 16) |
                   (static_cast<uint32_t>(snap[i + 2]) << 8) | static_cast<uint32_t>(snap[i + 3]);
      char buf[16];
      std::snprintf(buf, sizeof(buf), "%08x ", w);
      hex += buf;
    }
    REXLOG_INFO(
        "[FableDrawCapture] [FetchDecode-PostDraw] slot=31 {} address={:#x} via={} SUCCEEDED "
        "post-draw where the pre-draw attempt did not -- snapshot({} bytes)={}",
        a.which, d.address, path_used, got, hex);

    // Round 16, twenty-ninth follow-up: hand off to NativeDrawPoc the
    // moment constC's real bytes are in hand, pairing them with whatever
    // sel==2 binding LogActiveVertexShaderBindingForSlot() found moments
    // earlier in this SAME draw's LogFetchTable("pre", ...) pass (that call
    // always runs before __imp__sub_8221E0F0, which always runs before this
    // retry -- see this function's own comment -- so g_slot31_constC_binding
    // is never stale here, only either correctly-paired or (attribute_count
    // == 0) genuinely absent for this draw). constB is real, captured data
    // too (see native-rendering-phase3.md round 28's honest coherence
    // writeup) but is not part of any shader's declared vertex_bindings for
    // slot 31 in any capture so far, so there is no binding metadata to
    // pair it with yet -- not hidden, just nothing validated to hand off.
    if (std::strcmp(a.which, "constC") == 0 && g_slot31_constC_binding.attribute_count > 0) {
      fable2::native_poc::ValidatedFetchConstantVertexInfo info = g_slot31_constC_binding;
      info.address = d.address;
      info.snapshot_bytes_captured =
          std::min<uint32_t>(got, fable2::native_poc::kValidatedFetchConstantSnapshotBytes);
      std::memcpy(info.snapshot, snap, info.snapshot_bytes_captured);
      info.valid = true;
      // Round 19: read BEFORE CaptureAndValidate() (which runs after this
      // function returns, back in the caller -- see that call site) bumps
      // g_sequence for this same draw, so this is exactly this draw's own
      // about-to-be-assigned sequence/frame, not the next draw's.
      info.sequence = static_cast<uint32_t>(fable2::diag::g_sequence.load(std::memory_order_relaxed));
      info.guest_frame = static_cast<uint32_t>(fable2::submitted_frames.load(std::memory_order_relaxed));

      // Round 20, fourth-build follow-up (native-rendering-phase3.md): real
      // index-buffer capture for this same draw. See
      // ValidatedFetchConstantVertexInfo::index_data_valid's own comment
      // (fable_native_poc.h) for the full reasoning -- summary: an
      // independent, read-only, side-effect-free re-scan for the real
      // DRAW_INDX packet (the SAME scan CaptureAndValidate() performs
      // moments later for this same draw, back in the caller). Any
      // failure below just leaves info.index_data_valid false --
      // BuildDrawCommandFromValidatedFetchConstant() already falls back
      // to its prior sequential-decode behavior in that case, so this is
      // purely additive.
      //
      // Round 20, fifth-build follow-up: the first real run of this code
      // (fable2_111.1.log) showed address_verified=false on EVERY single
      // capture -- not occasionally, always -- with self's own
      // index_addr_raw (self+kOffIndexAddr) reading back values like
      // 0xffa060d0/0xff5bb6d0/... (a completely different, implausible
      // address family) against the real packet's actual DMA base
      // (payload[2]) reading 0x1d62de80/0x1be57640/... (the SAME
      // "canonical physical" 0x1b-0x1f range every other real buffer in
      // this investigation has used). That is not a small mismatch to
      // "verify" -- it is direct evidence that self+kOffIndexAddr simply
      // does not track the real index buffer for sub_8221E0F0-hooked
      // draws (the ONLY hook this constC path is ever fed from -- see
      // RetryPostDrawSlot31Snapshot's own call site). kOffIndexAddr/
      // PredictScratchAddressField were validated (Phase 1/2) against the
      // OTHER draw-flush hooks (sub_82217EE8/sub_8221C518); nothing in
      // this investigation ever confirmed they apply to sub_8221E0F0 too,
      // and this is now direct runtime evidence that they do not. Dropped
      // entirely for this path -- using payload[2] (the packet's own real
      // DMA base -- ground truth, not a self-field guess) as the read
      // address directly instead, exactly the same trust level already
      // given to payload[2]/index_addr_raw's role in the candidate-A/B
      // path's own OnValidatedDraw (see ValidatedDrawInfo's own comment:
      // "the real, packet-carried index-buffer guest address -- ground
      // truth"). Verified the same way constB/constC vertex attempts
      // already are: by whether a real read at that address actually
      // succeeds, not by a separate arithmetic round-trip.
      {
        const uint32_t new_cursor_for_scan = REX_LOAD_U32(self + fable2::diag::kOffCursor);
        const uint32_t buffer_limit_for_scan = REX_LOAD_U32(self + fable2::diag::kOffLimit);
        const uint32_t slack_end = new_cursor_for_scan + fable2::diag::kForwardSlackDwords * 4u;
        const uint32_t scan_end_for_index =
            (buffer_limit_for_scan != 0 && buffer_limit_for_scan < slack_end) ? buffer_limit_for_scan
                                                                               : slack_end;
        if (old_cursor < scan_end_for_index) {
          fable2::diag::ScanResult index_scan =
              fable2::diag::ScanForDrawIndx(base, old_cursor, scan_end_for_index);
          if (index_scan.found) {
            const uint32_t real_dma_base = index_scan.payload[2];
            const uint32_t real_index_count = (index_scan.payload[1] >> 16) & 0xFFFFu;
            const bool real_is_32bit = ((index_scan.payload[1] >> 11) & 0x1u) != 0u;
            // Same plausibility floor as the constB/constC vertex-buffer
            // attempts above (kMinPlausibleVertexAddr) -- a cheap guard
            // against wasting a read attempt on an obviously-bogus
            // address, not a claim of correctness by itself.
            constexpr uint32_t kMinPlausibleIndexAddr = 0x01000000u;
            const bool plausible = real_dma_base >= kMinPlausibleIndexAddr && real_index_count > 0;
            REXLOG_INFO(
                "[FableDrawCapture] [IndexCapture] slot=31 constC real_dma_base={:#010x} "
                "real_index_count={} real_is_32bit={} plausible={}",
                real_dma_base, real_index_count, real_is_32bit, plausible);
            if (plausible) {
              // Round 20, fifth-build follow-up: cap BYTES CAPTURED at
              // kMaxCapturedIndices, not the draw REJECTED outright, when
              // real_index_count exceeds it -- many real constC draws
              // (fable2_111.1.log: 437, 932, 1367, 2760, 3356, 4837,
              // 13835...) are far larger meshes than the 64-vertex
              // ceiling native_draw_repr.h's kMaxDrawVertices already
              // imposes on the whole native-render pipeline (a limit
              // that long predates this round). Capturing only the first
              // kMaxCapturedIndices real indices of a larger draw still
              // gives BuildDrawCommandFromValidatedFetchConstant real,
              // correctly-selected/ordered vertices for however much of
              // the draw fits -- strictly better than the all-or-nothing
              // reject this round's first build used, and no different in
              // spirit from record_count already being capped at
              // kMaxDrawVertices for the sequential fallback.
              // info.index_count keeps the TRUE full count (honest
              // logging/display); only the captured byte window is capped.
              const uint32_t index_stride = real_is_32bit ? 4u : 2u;
              const uint32_t indices_to_capture = std::min<uint32_t>(
                  real_index_count,
                  static_cast<uint32_t>(
                      fable2::native_poc::ValidatedFetchConstantVertexInfo::kMaxCapturedIndices));
              const uint32_t want_bytes = indices_to_capture * index_stride;
              uint8_t index_snap[fable2::native_poc::ValidatedFetchConstantVertexInfo::kMaxCapturedIndices *
                                  4] = {};
              uint32_t index_got = DiagSnapshotGuestPhysicalBytes(real_dma_base, want_bytes, index_snap);
              const char* index_path_used = "physical";
              if (index_got == 0) {
                constexpr uint32_t kCanonicalToAliasDeltaIdx = 0xDFFFF000u;
                index_got = DiagSnapshotGuestBytes(base, real_dma_base + kCanonicalToAliasDeltaIdx,
                                                    want_bytes, index_snap);
                index_path_used = "alias-fallback";
              }
              if (index_got > 0) {
                info.index_data_valid = true;
                info.index_addr = real_dma_base;
                info.index_count = real_index_count;
                info.index_is_32bit = real_is_32bit;
                info.index_snapshot_bytes_captured = index_got;
                std::memcpy(info.index_snapshot, index_snap, index_got);
                REXLOG_INFO(
                    "[FableDrawCapture] [IndexCapture] slot=31 constC SUCCEEDED via={}: captured "
                    "{} index bytes ({} of {} indices captured, {}-bit) from address={:#x}",
                    index_path_used, index_got, indices_to_capture, real_index_count,
                    real_is_32bit ? 32 : 16, real_dma_base);
              } else {
                REXLOG_INFO(
                    "[FableDrawCapture] [IndexCapture] slot=31 constC: plausible address but 0 "
                    "bytes read from address={:#x} via both paths",
                    real_dma_base);
              }
            }
          } else {
            REXLOG_INFO(
                "[FableDrawCapture] [IndexCapture] slot=31 constC: independent DRAW_INDX re-scan "
                "found no packet for this draw (old_cursor={:#x} scan_end={:#x})",
                old_cursor, scan_end_for_index);
          }
        }
      }

      fable2::native_poc::OnValidatedFetchConstantVertexStream(info);
    }
  }
}

static void LogFetchTable(const char* when, uint8_t* base, uint32_t self) {
  const uint64_t fetch_mask64 = REX_LOAD_U64(self + fable2::diag::kOffFetchDirtyMask);
  const uint32_t fetch_mask = static_cast<uint32_t>(fetch_mask64 & 0xFFFFFFFFu);
  REXLOG_INFO(
      "[FableDrawCapture] [FetchTable] {} self={:#x} fetch_dirty_mask={:#010x}",
      when, self, fetch_mask);
  if (fetch_mask == 0) {
    return;
  }
  // Dump every slot, not just the ones a guessed bit-numbering says are
  // dirty -- which end of the mask corresponds to slot 0 (cntlzd counts
  // from the MSB, but whether that lines up with "slot 0" here is not
  // confirmed) is exactly the kind of thing this capture is meant to
  // settle, so don't let a wrong guess there silently drop real data.
  for (uint32_t slot = 0; slot < fable2::diag::kFetchShadowSlotCount; ++slot) {
    const uint32_t slot_base = self + fable2::diag::kOffFetchShadowTable +
                                slot * fable2::diag::kFetchShadowSlotStride;
    // Dump 32 bytes (8 dwords) starting 4 bytes BEFORE the naive slot
    // boundary through 8 bytes past its naive end. Originally only 7 words
    // (through +20) were captured; extended to 8 (through +24) in round 16,
    // twenty-fifth follow-up once the ShaderBinding cross-check
    // (native-rendering-phase3.md) proved the validated shader's slot-31
    // binding is at const_index_sel==2, i.e. the THIRD 8-byte
    // xe_gpu_vertex_fetch_t packed into this 24-byte fetch group. True
    // group start is slot_base+4 (confirmed alignment below), so sel=2 spans
    // true-group bytes 16 through 23 = slot_base+20 through slot_base+27 -- the old
    // 7-word window only ever captured that sub-constant's dword0
    // (words[6]=slot_base+20), never its dword1/size (would need
    // slot_base+24, one word past the old window's end). Reading one word
    // further is safe: self is a large, live engine object (fields already
    // referenced elsewhere in this investigation go out past self+13000),
    // so slot_base+24 for even the last slot (31) is nowhere near any real
    // allocation boundary, and every word in this window is already read
    // with a plain REX_LOAD_U32 (no __try/__except), consistent with that.
    uint32_t words[8] = {};
    for (int i = 0; i < 8; ++i) {
      words[i] = REX_LOAD_U32(slot_base - 4 + static_cast<uint32_t>(i) * 4);
    }
    REXLOG_INFO(
        "[FableDrawCapture] [FetchTable] {} slot={} addr={:#x} "
        "words[-4,0,4,8,12,16,20,24]={:#010x},{:#010x},{:#010x},{:#010x},{:#010x},{:#010x},{:#010x},{:#010x}",
        when, slot, slot_base, words[0], words[1], words[2], words[3], words[4], words[5], words[6],
        words[7]);

    // Round 16, ninth follow-up: decode the two fully-captured constants in
    // this window as real xe_gpu_vertex_fetch_t pairs. Byte alignment
    // (dword0 at words[2]/words[4], i.e. the naive slot boundary +4/+12,
    // not +0/+8) is confirmed by three independent lines of evidence from
    // fable2_079.log: (1) adjacent slots' word windows overlap exactly at
    // this offset (slot N's words[6] == slot N+1's words[1]), (2) the
    // resulting "type" field reads a real, valid enum value (kTexture=2)
    // consistently across nearly every slot rather than noise, and (3) at
    // this alignment slot 31 decodes to a plausible real vertex buffer
    // (address in the already-established "canonical physical" 0x1f/0x1c
    // range, size divisible by both previously-confirmed candidate
    // strides 16 and 32) -- see the round-16-ninth-follow-up doc entry.
    // Only decode+snapshot on "pre" (the real pre-flush state) -- "post"
    // is expected to be the just-cleared table and would just waste a
    // snapshot attempt.
    if (std::strcmp(when, "pre") == 0) {
      // const_index_sel 0/1/2 -> constA/constB/constC, per the real
      // xe_gpu_fetch_group_t layout (one 24-byte group == either one
      // 6-dword texture fetch constant, or three 2-dword vertex fetch
      // constants selected by const_index_sel). Round 16, twenty-fifth
      // follow-up added constC (sel=2) after discovering the validated
      // slot-31 shader binding (fable2_085.2.log: const_index=31
      // const_index_sel=2) is NEITHER constA (sel=0, the address this
      // whole investigation had been treating as "the slot-31 buffer"
      // since the ninth follow-up) NOR constB (sel=1) -- see the doc entry
      // for the full derivation. constA/constB are kept exactly as before
      // (still useful, real fetch constants -- just not this shader's
      // binding) so nothing already relying on them changes.
      LogAndMaybeSnapshotFetchConstant(base, slot, "constA", words[2], words[3]);
      LogAndMaybeSnapshotFetchConstant(base, slot, "constB", words[4], words[5]);
      LogAndMaybeSnapshotFetchConstant(base, slot, "constC", words[6], words[7]);
      // Round 16, twentieth follow-up: tie the shader-side stride lookup to
      // this exact slot/draw, right where the shadow-table side of slot 31
      // is already being logged, rather than querying it separately (and
      // out of sync) elsewhere.
      if (slot == 31u) {
        LogActiveVertexShaderBindingForSlot(slot);
      }
    }
  }
}

extern "C" void sub_8221E0F0(PPCContext& __restrict ctx, uint8_t* base) {
  // Round 16, thirty-sixth follow-up: an unconditional, log-once build
  // marker -- prompted directly by a real back-and-forth where a rebuilt
  // binary's logs still showed no [FetchConstantDecode] line, and it took
  // a manual re-check to realize that line is conditional (it only fires
  // when a real slot-31 binding AND a successful constC snapshot land in
  // the SAME draw, which is genuinely rare) rather than proof of a stale
  // build. This line fires on the very first call to this hook in the
  // process's lifetime, unconditionally -- unlike everything else in this
  // function, which returns early below whenever capture isn't currently
  // armed. Its presence/absence in a log settles the "was this rebuilt"
  // question directly instead of by inference from unrelated evidence.
  static bool logged_build_marker = false;
  if (!logged_build_marker) {
    logged_build_marker = true;
    REXLOG_INFO(
        "[FableDrawCapture] [BuildMarker] this build includes round 29 (validated fetch-constant "
        "-> NativeDrawCommand hand-off) and round 34 (constC plausible_magnitude heuristic in "
        "OnValidatedFetchConstantVertexStream). If this line is ever missing from a log, the "
        "running exe predates those changes. If present but no [FetchConstantDecode] line shows "
        "up later, the build is current -- no draw this session simply met the hand-off's own "
        "precondition yet (a real slot-31 binding AND a successful constC snapshot together).");
  }
  // Round 16, thirty-eighth follow-up: unconditional, like the build
  // marker above -- deliberately BEFORE the capture_active early-return,
  // so a constC address enqueued by a previous (possibly already-closed)
  // capture window keeps getting retried on every call to this hook, not
  // just while a window happens to still be open. See
  // RetryPendingSlot31Snapshots's own comment for why this needs to run
  // independent of capture_active.
  RetryPendingSlot31Snapshots(base);

  const bool capture_active = fable2::native_poc::WantsNextValidatedDrawFrom(0x8221E0F0u);
  if (!capture_active) {
    __imp__sub_8221E0F0(ctx, base);
    return;
  }

  const uint32_t self = ctx.r3.u32;
  uint32_t old_cursor = 0;
  fable2::diag::EntryArgs entry{};
  entry.hook_addr = 0x8221E0F0;
  if (self != 0) {
    old_cursor = REX_LOAD_U32(self + fable2::diag::kOffCursor);
    // Must be read here, before __imp__sub_8221E0F0 runs -- same reasoning
    // as the other two wrappers.
    entry.prim_type = ctx.r4.u32;    // arg1 -- CONFIRMED for this function (see block comment)
    entry.index_count = ctx.r7.u32;  // arg4 -- CONFIRMED for this function (see block comment)
    entry.flags = ctx.r8.u32;        // arg5 -- NOT re-traced; informational only
    entry.r6_raw = ctx.r6.u32;
    entry.r9_raw = ctx.r9.u32;
    entry.vertex_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffVertexAddr);
    entry.index_addr_pre = REX_LOAD_U32(self + fable2::diag::kOffIndexAddr);
    LogFetchTable("pre", base, self);
  }

  fable2::diag::g_resource_call_count.store(0, std::memory_order_relaxed);
  fable2::diag::g_capturing_resource_calls.store(true, std::memory_order_relaxed);
  __imp__sub_8221E0F0(ctx, base);
  fable2::diag::g_capturing_resource_calls.store(false, std::memory_order_relaxed);

  if (self != 0) {
    LogFetchTable("post", base, self);
    // Round 16, twenty-seventh follow-up: see RetryPostDrawSlot31Snapshot's
    // own comment. Unconditional (not gated on fetch_mask like LogFetchTable
    // itself) -- the whole point is to test readability independent of the
    // CPU-side dirty-bit bookkeeping.
    RetryPostDrawSlot31Snapshot(base, self, old_cursor);
  }

  if (self != 0) {
    fable2::diag::CaptureAndValidate(ctx, base, self, old_cursor, entry);
  }
}
