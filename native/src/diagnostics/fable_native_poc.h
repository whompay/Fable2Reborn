// Fable2PC -- Phase 3 diagnostic instrumentation: NativeDrawPoc
//
// Smallest possible native-rendering proof of concept, built directly on top
// of the runtime-validated pre-PM4 draw boundary from Phase 2 (see
// native/performance-investigation-phase2.md, attempt 3). It consumes ONE
// real, fully-validated sub_82217EE8 draw call -- its real primitive type,
// index count, index buffer bytes, and (best-effort) vertex buffer bytes,
// all read directly from guest memory at the runtime-confirmed addresses --
// and renders it with Dear ImGui's own existing 2D draw-list API (already
// wired into this title's real D3D12 presentation path via the F1/F3
// overlays), as a debug overlay panel toggled with F4.
//
// This does NOT implement a renderer, does NOT touch the ReXGlue/Xenos
// rendering path in any way, and does NOT interpret shader bytecode or
// invent a shader abstraction -- it uses ImGui's own already-compiled,
// already-working solid-fill draw-list shader, which is the minimum
// "shader problem" needed to put real captured geometry on screen. See
// native/native-rendering-phase3.md for the full design writeup, the
// vertex-format assumption, and its confidence level.
#pragma once

#include <cstdint>
#include <memory>  // Round 20 (native 3D draw path): std::unique_ptr<D3D12NativeRenderBackend>

#include <rex/cvar.h>
#include <rex/ui/imgui_dialog.h>

// Round 8 (native-rendering-phase3.md, "GateDiag"): cross-TU declaration so
// fable_draw_capture.cpp (which already includes this header for
// WantsNextValidatedDrawFrom()/OnValidatedDraw()) can also read the raw
// fable2_native_poc_enable cvar value directly via REXCVAR_GET, to log it
// independent of WantsNextValidatedDrawFrom()'s own combined
// (cvar && !g_have_draw) result. The definition (REXCVAR_DEFINE_BOOL) lives
// in fable_native_poc.cpp, at global scope, matching this declaration's
// scope exactly -- see rex/cvar.h's own comment on REXCVAR_DECLARE ("use in
// headers and TUs that need to read it"). This was the actual compile
// failure this round: fable_draw_capture.cpp called REXCVAR_GET on this
// cvar without any declaration of FLAGS_fable2_native_poc_enable_storage_
// in scope, which only fable_native_poc.cpp (the defining TU) had.
REXCVAR_DECLARE(bool, fable2_native_poc_enable);

namespace fable2::native_render {
class D3D12NativeRenderBackend;
}  // namespace fable2::native_render

namespace fable2::native_poc {

// The subset of one CaptureAndValidate() row this module needs, passed by
// value from fable_draw_capture.cpp so the two diagnostic files stay
// decoupled (this header does not depend on fable_draw_capture.cpp's
// internal DrawCaptureRow type, and fable_draw_capture.cpp does not depend
// on this file beyond the two calls below). Every field here is a
// runtime-validated (attempt 3: 500/500) value, not a prediction.
struct ValidatedDrawInfo {
  uint32_t sequence = 0;        // this capture's FableDrawCapture sequence number, for cross-reference
  uint32_t guest_frame = 0;     // fable2::submitted_frames at capture time
  uint32_t prim_type = 0;       // confirmed D3DPT_TRIANGLELIST (4) for the validated draw
  uint32_t index_count = 0;     // confirmed 6
  uint32_t index_addr_raw = 0;  // self+13452, BEFORE the address-packing formula
  uint32_t vertex_addr_raw = 0; // self+13448, BEFORE the address-packing formula (see .cpp)
  uint32_t drawindx_header = 0; // confirmed 0xC0032201
  uint32_t payload[4] = {};     // confirmed actual DRAW_INDX payload dwords (payload[2] is the
                                 // real, packet-carried index-buffer guest address -- ground
                                 // truth, used directly instead of re-deriving it)
  uint32_t rt_slot0 = 0;        // self+12440, one representative render-target snapshot field
  uint32_t shadow_0x2000 = 0;   // self+10368, RB_SURFACE bank snapshot field

  // Phase 4 (native/native-rendering-phase3.md, vertex-stream tracing):
  // directly-observed state from the specific sub_82206820 resource-resolver
  // calls the producing hook made for THIS draw, captured by a tightly-scoped
  // weak-symbol hook in fable_draw_capture.cpp (armed/disarmed only around
  // this draw's own __imp__sub_XXXXXXXX call) -- not another guessed
  // address formula. resource_calls_captured is true only when both calls
  // were observed. Round 15: this is NO LONGER implied by
  // resource_fields_updated (the hand-off gate dropped that requirement --
  // see fable_draw_capture.cpp's CaptureAndValidate); a high-frequency caller
  // reusing an already-bound buffer without re-resolving it is now a normal,
  // expected way to reach this struct with resource_calls_captured=false. If
  // false, every vertex_call_*/index_call_* field below is zero and must not
  // be trusted -- OnValidatedDraw() already falls back to payload[2]/a cvar
  // stride guess in that case, which remains valid ground truth regardless
  // (vertex_addr_raw/index_addr_raw always reflect the currently-bound
  // buffer, whether or not THIS call refreshed it).
  bool resource_calls_captured = false;
  uint32_t vertex_call_count_arg = 0;    // sub_82206820 call #1 (feeds self+13448): r4 (dword count)
  uint32_t vertex_call_stride_arg = 0;   // sub_82206820 call #1: r5 (16 for both calls per Phase 1)
  uint32_t vertex_call_return_addr = 0;  // sub_82206820 call #1: r3 AFTER the call -- the real,
                                          // observed return value (== self+13448's raw contents
                                          // when this call's success path ran)
  uint32_t index_call_count_arg = 0;     // sub_82206820 call #2 (feeds self+13452): r4
  uint32_t index_call_stride_arg = 0;    // sub_82206820 call #2: r5
  uint32_t index_call_return_addr = 0;   // sub_82206820 call #2: r3 AFTER the call

  // self+13516, read fresh at hand-off time (not part of the Phase 2 CSV
  // schema). Confirmed by a static trace of sub_82206820's own body
  // (fable2_recomp.182.cpp, ~line 833) to gate a branch INSIDE
  // sub_82206820 itself: zero takes a "simple" allocation path
  // (sub_822065F0); non-zero takes a bump-allocator path involving
  // self+14916, self+13516+152/156, and self+48+164. Its exact identity
  // (a bound vertex-declaration/stream-source object pointer is the
  // leading hypothesis, NOT confirmed) is UNKNOWN -- surfaced here as raw
  // evidence only, never asserted.
  uint32_t stream_binding_ptr = 0;

  // Phase 4 follow-up (address-semantics trace, this pass): runtime capture
  // confirmed self+13516 == 0 for the captured draw, so sub_82206820 (with
  // this call site's flags=1) actually runs sub_822065F0's "self+14916==0"
  // branch, which computes its returned address starting from `self+52`
  // (NOT self+48) when the per-object bump allocator has never been
  // initialized -- see native-rendering-phase3.md's Phase 4 address-
  // semantics section for the disassembly trace
  // (fable2_recomp.82.cpp, DEFINE_REX_FUNC(sub_822065F0), ~line 885-1065).
  // self+48/self+56 are already confirmed (Phase 1) as this same device
  // object's record-buffer cursor/limit; self+52 is the one still-
  // unconfirmed member of that field triple, and this is raw evidence for
  // it, not a finding. Both read fresh at hand-off time, AFTER both
  // sub_82206820 calls (so vertex_bump_cursor_raw reflects whatever the
  // vertex call's own success path may have written to self+14916 before
  // the index call ran).
  uint32_t record_buffer_base_raw = 0;   // self+52
  uint32_t vertex_bump_cursor_raw = 0;   // self+14916

  // Round 12 (native-rendering-phase3.md): which of the nine draw-flush-
  // family functions (performance-investigation-phase1.md) produced this
  // draw -- e.g. 0x82217EE8 or 0x8221C518. Round 11 found sub_82217EE8's
  // own four call sites empirically only ever draw tiny (index_count 6-36),
  // screen-space-looking NDC/UV quads across many re-armed captures; this
  // field lets the overlay (and any future analysis) tell which underlying
  // hook actually produced a given capture once more than one is armed.
  uint32_t source_hook_addr = 0;
};

// Round 16, twenty-ninth follow-up (native-rendering-phase3.md): a second,
// completely separate hand-off path, additive to ValidatedDrawInfo/
// OnValidatedDraw above -- this one carries the SHADER_CONSTANT_FETCH
// slot-31 vertex stream fable_draw_capture.cpp's LogActiveVertexShaderBindingForSlot()
// (ReX's own Shader::AnalyzeUcode()-derived VertexBinding metadata) and
// RetryPostDrawSlot31Snapshot()/LogAndMaybeSnapshotFetchConstant() (real,
// physical-memory-backed captured bytes for the const_index_sel==2 /
// "constC" fetch constant) have validated this session -- NOT the
// sub_82217EE8/sub_8221C518 draw-flush hook family's own guessed
// vertex_addr_raw/candidate-A/B addressing above, which this does not
// touch or depend on. See message-1's original instruction and the
// round-24/25/28 doc entries for how this layout was derived and what its
// known limitations are (round 28: does not yet decode into visually
// coherent records for every shader tried -- this hand-off exposes the
// raw validated bytes+layout for a debug overlay to keep inspecting, it is
// not a claim that every field below is semantically understood).
struct ValidatedFetchConstantAttribute {
  uint32_t data_format = 0;             // xenos::VertexFormat, raw numeric value (see xenos.h)
  int32_t offset_dwords = 0;            // Attributes::offset -- dword-granularity, confirmed via
                                         // ucode.h's VertexFetchInstruction::offset() doc comment
                                         // ("Dword offset [-4194304, 4194303]")
  bool is_mini_fetch = false;
  bool is_signed = false;
  bool is_integer = false;
  uint32_t used_result_components = 0;  // bit i set => component i (x=0,y=1,z=2,w=3) is read
};

constexpr int kMaxValidatedFetchConstantAttributes = 16;
// Round 20, native-3D-draw-path follow-up ("widen the snapshot window"):
// bumped 224 -> 16384. Round 19's own first-build-follow-up comment
// (OnValidatedFetchConstantVertexStream, fable_native_poc.cpp) already
// caught and documented the underlying problem once: at 224 bytes, a
// 192-byte/12-attribute shader's binding only ever fits ONE full decoded
// record (224/192==1), and that round's fix was to skip past that
// specific capture and latch onto a smaller-stride shader (28-40 bytes/
// record) that happened to fit ~6-11 records instead -- a workaround for
// ONE draw's overlay display, not a fix of the underlying cap. Task #23's
// closing investigation (native-rendering-phase3.md, sixth follow-up)
// found the SAME root cause again from a different angle: fable2_112.log's
// LATCHED draw (20-byte/4-attribute stride, so 224 bytes = 11 decodable
// records) has a REAL index count of 1367 and a captured index buffer of
// up to kMaxCapturedIndices=64 real indices (see
// ValidatedFetchConstantVertexInfo::index_data_valid below) -- but with
// only 11 of this mesh's real vertex records actually decoded, the large
// majority of those 64 real index values point past the decoded window
// and get skipped by BuildDrawCommandFromValidatedFetchConstant's
// index-based selection (it explicitly skips any index >=
// record_count_available -- see that function's own comment), leaving a
// handful of real-but-arbitrary vertices, including the v[4]=(...,24480,
// ...) outlier hand-verified there to be correctly-decoded raw bytes, not
// a bug. Confirmed by direct runtime evidence, not guessed: the first
// real D3D12 draw call (fable2_113.log, this same round) rendered that
// exact draw as a thin, mostly-degenerate diagonal sliver rather than a
// recognizable filled shape, consistent with a bounding-box-fit
// projection dominated by that one huge outlier among ~11 real points.
//
// 16384 bytes is sized to comfortably decode up to kMaxDrawVertices (64,
// native_draw_repr.h -- there is no point decoding more records than
// BuildDrawCommandFromValidatedFetchConstant can ever emit as vertices)
// even at a worst-case stride: the largest ATTRIBUTE COUNT this struct
// supports is kMaxValidatedFetchConstantAttributes=16, and Xenos vertex
// fetch attributes are at most 4 dwords (16 bytes) wide, so 16 attributes
// x 16 bytes = 256 bytes/record is the theoretical ceiling; 64 x 256 =
// 16384 exactly. Every stride actually observed in this investigation so
// far (20, 28, 36, 40, 192 bytes) is well under that. This is still a
// small, fixed stack buffer (used as a handful of struct copies per real
// draw event, not per-frame-per-vertex -- see this struct's own users in
// fable_draw_capture.cpp/fable_native_poc.cpp, all local variables or a
// mutex-guarded global, none of them hot per-vertex code), so the size
// increase costs nothing measurable. DiagSnapshotGuestBytes/
// DiagSnapshotGuestPhysicalBytes (fable_draw_capture.cpp) are both
// general-purpose, chunked, size-parametric reads with no hidden cap, so
// this is a pure capacity increase with no other code path to adjust.
constexpr uint32_t kValidatedFetchConstantSnapshotBytes = 16384;

struct ValidatedFetchConstantVertexInfo {
  bool valid = false;
  uint32_t address = 0;         // physical/canonical guest address the snapshot bytes came from
  uint32_t stride_words = 0;    // ReX's own VertexBinding::stride_words (dwords per record)
  uint64_t ucode_hash = 0;      // which shader's binding this came from, for diagnostics only
  int attribute_count = 0;      // clamped to kMaxValidatedFetchConstantAttributes
  ValidatedFetchConstantAttribute attributes[kMaxValidatedFetchConstantAttributes];
  uint32_t snapshot_bytes_captured = 0;
  uint8_t snapshot[kValidatedFetchConstantSnapshotBytes] = {};

  // Round 19 (Fable2PC, first-native-geometry milestone): mirrors
  // ValidatedDrawInfo::sequence/guest_frame above -- same meaning, same
  // source (FableDrawCapture's own g_sequence/fable2::submitted_frames),
  // just not previously carried on this struct because nothing downstream
  // needed to cite a specific draw until now. Populated in
  // fable_draw_capture.cpp's RetryPostDrawSlot31Snapshot(), which runs
  // strictly before CaptureAndValidate() increments g_sequence for this
  // same draw (see that call site's own comment) -- so reading
  // g_sequence/submitted_frames there, unincremented, gives exactly the
  // value CaptureAndValidate() is about to assign as this same draw's own
  // row.sequence/row.guest_frame, not a guess or an off-by-one.
  uint32_t sequence = 0;
  uint32_t guest_frame = 0;

  // Round 20 (Fable2PC, native 3D draw path -- native-rendering-phase3.md):
  // raw vertex-shader float constant register values, captured at the SAME
  // draw boundary as the geometry above, via CommandProcessor::
  // register_file() (this round's own SDK accessor addition -- see
  // command_processor.h) plus Shader::constant_register_map() (which
  // logical float4 registers [0-255] this shader actually reads) and
  // SQ_VS_CONST.base (this shader stage's physical offset into the
  // 512-wide float constant bank -- see registers.h; the doc comment on
  // ConstantRegisterMap::float_bitmap in shader.h is explicit that the
  // physical base depends on this, so the logical index alone is not
  // enough). RAW DATA ONLY: this makes no claim about which (if any) of
  // these registers is a world/view/projection matrix -- see this
  // struct's own file-level discipline (never claim identity before
  // runtime evidence) and native-rendering-phase3.md's round 20 entry.
  // Round 20, eleventh follow-up (view/projection reconstruction --
  // native-rendering-phase3.md): bumped 32 -> 256, the full width of
  // ConstantRegisterMap::float_bitmap's own logical-index range (2 x
  // uint64 x 64 bits below covers exactly [0,255], nothing more is
  // possible). The capture loop (fable_draw_capture.cpp) scans logical
  // indices 0..255 in ascending order and stops once
  // float_constant_count reaches this cap -- with the old cap of 32, any
  // shader using MORE than 32 total flagged registers would silently
  // drop everything past the 32nd (in ascending logical-index order),
  // with no log or warning that truncation had even happened. Direct
  // evidence this was a real, not theoretical, risk: fable2_115.log's
  // own LATCHED draw (round 20, tenth follow-up) logged EXACTLY 32
  // captured float constants (c[0] through c[31], contiguous) -- exactly
  // hitting the old cap is a strong sign this specific shader uses MORE
  // than 32 registers and was being truncated, not that it happens to use
  // precisely 32. Since view/projection matrices are frequently uploaded
  // at higher, separate logical-register ranges than per-object data
  // (the already-found world transform sits at logical 4-7, but nothing
  // rules out a view/projection matrix at, say, logical 32+, which the
  // old cap could never have reached for this shader), this was a real
  // blind spot for the exact search this round is trying to do -- fixed
  // here, not assumed fixed by the earlier 16384-byte snapshot-window
  // change (that was the VERTEX snapshot; this is the completely
  // separate FLOAT CONSTANT capture, sized independently). See
  // fable_draw_capture.cpp's own capture loop for the accompanying
  // truncated-vs-total logging this round also added, so any future
  // truncation (should be impossible now, at 256 -- the map's own natural
  // ceiling) is directly visible in the log rather than assumed away.
  static constexpr int kMaxCapturedFloatConstants = 256;
  int float_constant_count = 0;
  // Logical index (0-255, from float_bitmap's bit position) for each
  // entry below -- NOT the physical register, so this is directly
  // comparable across shaders/draws regardless of SQ_VS_CONST.base.
  uint32_t float_constant_logical_index[kMaxCapturedFloatConstants] = {};
  float float_constant_value[kMaxCapturedFloatConstants][4] = {};  // raw x,y,z,w

  // Round 20, fourth-build follow-up (native-rendering-phase3.md): real
  // index-buffer data for this SAME draw, captured in
  // fable_draw_capture.cpp's RetryPostDrawSlot31Snapshot() via an
  // independent, read-only, side-effect-free re-scan for the real
  // DRAW_INDX packet -- the SAME scan CaptureAndValidate() (its caller)
  // performs moments later for this same draw; calling it twice is safe
  // because it has no side effects and reads nothing but already-written
  // guest memory. This exists to fix
  // BuildDrawCommandFromValidatedFetchConstant()'s "blind sequential
  // decode" limitation (documented on that function): without real
  // indices, every decoded vertex record was assumed to be used exactly
  // once, in buffer order, which produced two out-of-place outlier
  // vertices (fable2_110.log) once real transforms (world_transform_applied
  // above) started placing geometry in a meaningful coordinate space --
  // previously masked by the debug overlay's own auto-fit-to-canvas
  // projection.
  //
  // Round 20, fifth-build follow-up: index_addr is read from the real
  // DRAW_INDX packet's own payload[2] (VGT_DMA_BASE -- ground truth),
  // NOT from self+kOffIndexAddr. The first build's design used
  // self+kOffIndexAddr, verified against payload[2] via
  // PredictScratchAddressField -- that field is real and validated for
  // the OTHER draw-flush hooks (sub_82217EE8/sub_8221C518), but the first
  // real run of this code (fable2_111.1.log) showed it reads a
  // completely different, implausible address family
  // (0xff.......0/0xffa060d0 etc.) for every single sub_8221E0F0-hooked
  // constC draw -- the ONLY hook this path is ever fed from -- while the
  // real packet's own payload[2] consistently reads the same "canonical
  // physical" 0x1b-0x1f address range every other real buffer in this
  // investigation has used. Direct evidence that field simply does not
  // apply to this hook, not a guess. index_data_valid is now set true
  // when the independent DRAW_INDX scan locates a real packet AND a real
  // read at payload[2] itself actually succeeds (same "prove it via a
  // successful read" discipline already used for constB/constC's own
  // vertex-buffer attempts) -- no self-field verification step. A draw
  // that fails either check simply leaves index_data_valid false, and
  // BuildDrawCommandFromValidatedFetchConstant falls back to its exact
  // prior sequential-decode behavior -- this remains strictly additive,
  // every earlier capture's behavior is unchanged.
  bool index_data_valid = false;
  uint32_t index_addr = 0;    // real DRAW_INDX payload[2] (VGT_DMA_BASE), read-verified
  // True full index count from the real packet's VGT_DRAW_INITIATOR --
  // may exceed kMaxCapturedIndices below (large draws are common; see
  // the fifth-build-follow-up comment above). Downstream code must clamp
  // against index_snapshot_bytes_captured, not assume this many indices
  // were actually captured.
  uint32_t index_count = 0;
  bool index_is_32bit = false;  // real VGT_DRAW_INITIATOR::index_size (bit 11):
                                 // false=16-bit indices, true=32-bit indices
  // Matches native_draw_repr.h's kMaxDrawVertices -- there is no point
  // capturing more indices than BuildDrawCommandFromValidatedFetchConstant
  // can ever emit as vertices. When the real draw's index_count exceeds
  // this, only the first kMaxCapturedIndices real indices are captured
  // (index_count above still reports the true full count).
  static constexpr int kMaxCapturedIndices = 64;
  uint32_t index_snapshot_bytes_captured = 0;
  uint8_t index_snapshot[kMaxCapturedIndices * 4] = {};  // raw bytes, 2 or 4 per index
};

// Called from fable_draw_capture.cpp's RetryPostDrawSlot31Snapshot(), only
// when a real constC snapshot for slot 31 succeeded AND ReX's own
// vertex-binding metadata for the SAME draw (const_index_sel==2 match) was
// captured moments earlier in that same draw's LogFetchTable("pre", ...)
// pass -- see the call site's ordering comment for why that pairing is
// safe (single-threaded, strictly sequential capture within one hook
// invocation), not a guess.
void OnValidatedFetchConstantVertexStream(const ValidatedFetchConstantVertexInfo& info);

// Cheap (single cvar + two atomic loads) check each weak-symbol wrapper in
// fable_draw_capture.cpp uses to decide whether to stay in capture-active
// mode purely to feed this module, independent of fable2_draw_capture_max.
// False as soon as one draw has been captured (this is a one-shot proof of
// concept, not a continuous renderer) or if fable2_native_poc_enable is off.
//
// Round 13 (native-rendering-phase3.md): hook_addr is the caller's OWN
// address (e.g. 0x82217EE8 or 0x8221C518) -- this returns true only if that
// specific hook is the one the overlay's filter radio buttons currently
// allow (or if the filter is 0, "any hook", the round-11 default). Added
// because round 12's evidence showed sub_82217EE8 firing every frame while
// sub_8221C518 fires only occasionally, so an unfiltered re-arm almost
// always let sub_82217EE8 win the race back even during real gameplay.
bool WantsNextValidatedDrawFrom(uint32_t hook_addr);

// Called at most once (subsequent calls after the first successful one are
// no-ops) from CaptureAndValidate(), only when that call's scan located a
// real DRAW_INDX packet (scan.found -- direct proof a draw was actually
// emitted). Round 15: no longer additionally requires resource_fields_updated
// -- see CaptureAndValidate's hand-off comment for why that requirement
// silently discarded 100% of a real, high-frequency caller's traffic.
// info.resource_calls_captured (separate flag) still tells this function
// whether the sub_82206820 resolver calls were directly observed for THIS
// draw; when not, it falls back to ground-truth/cvar values that remain
// valid regardless. Reads the real index/vertex bytes from guest memory and
// stores a decoded snapshot for NativePocOverlay to render. base must be the
// same guest-memory base pointer CaptureAndValidate itself was given.
void OnValidatedDraw(uint8_t* base, const ValidatedDrawInfo& info);

// F4-toggled debug overlay (see fable2_app.h), following the exact same
// construct-to-open/reset-to-close pattern as fable2::FpsOverlay
// (pc_features.h). Renders the most recent OnValidatedDraw() snapshot, if
// any, as filled triangles via ImGui's own draw list plus a text panel of
// the raw validated fields -- or a "waiting for a validated draw" message
// if fable2_native_poc_enable hasn't captured one yet.
class NativePocOverlay final : public rex::ui::ImGuiDialog {
 public:
  // Declared out-of-line (defined in fable_native_poc.cpp), even though
  // the body is trivial: this class holds a
  // std::unique_ptr<fable2::native_render::D3D12NativeRenderBackend>,
  // and D3D12NativeRenderBackend is only forward-declared here. An
  // inline-defined constructor still needs that type complete in this
  // translation unit, because the compiler generates unwind code to
  // destroy already-constructed members if a later member's
  // initialization throws -- the same reason the destructor must be
  // out-of-line for the pimpl-with-unique_ptr pattern applies equally
  // to the constructor. Moving only the destructor out-of-line was not
  // sufficient (confirmed by a real build failure: "invalid application
  // of 'sizeof' to an incomplete type" at this constructor's
  // instantiation, in every translation unit that includes this header
  // without also including fable_native_d3d12_backend.h).
  explicit NativePocOverlay(rex::ui::ImGuiDrawer* drawer);
  // Round 20 (native 3D draw path, native-rendering-phase3.md): declared
  // (not defaulted) so the implicit body -- which must see
  // fable2::native_render::D3D12NativeRenderBackend's complete type to
  // destroy d3d12_backend_ below -- is emitted in fable_native_poc.cpp,
  // not wherever this header happens to be included, keeping the heavy
  // D3D12 SDK header chain (rex/ui/d3d12/d3d12_api.h) out of this header
  // entirely (standard pimpl-style forward-declaration discipline; this
  // header only ever needs to know the type exists, never its layout).
  ~NativePocOverlay() override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  // Round 20 (native 3D draw path): lazily constructed on first use
  // inside OnDraw() (needs imgui_drawer(), only valid once this dialog
  // is attached -- see D3D12NativeRenderBackend's own comment on why it
  // must only ever be driven from within OnDraw()). Forward-declared
  // only -- see native/src/diagnostics/fable_native_d3d12_backend.h for
  // the full type and native-rendering-phase3.md's round 20 entry for
  // why this exists (the ImDrawCmd::UserCallback dead end, and the two
  // new zero-layout-change SDK accessors used instead).
  std::unique_ptr<fable2::native_render::D3D12NativeRenderBackend> d3d12_backend_;
  // Overlay checkbox state -- defaults to true so the new draw path is
  // visible without an extra click, per this round's own brief ("take
  // this out of the diagnostic 2D visualization"); the existing
  // ImGuiDrawListBackend keeps rendering unconditionally alongside it
  // either way (preserved as reference/fallback, per that same brief).
  bool d3d12_backend_enabled_ = true;
};

}  // namespace fable2::native_poc
