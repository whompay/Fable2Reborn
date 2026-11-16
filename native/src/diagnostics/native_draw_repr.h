// Fable2PC -- native renderer: backend-neutral draw representation.
//
// MAXIMUM FORWARD PROGRESS pass (native-rendering-phase3.md, round 7):
// everything upstream of this file (fable_draw_capture.cpp,
// fable_native_poc.cpp's DecodeCandidate/DecodeLooksPlausible) is still
// actively changing -- candidate vertex addresses, stride, byte order are
// all still being corrected experimentally, one runtime capture at a time.
// Until this file existed, that same decode code ALSO did the ImGui
// drawing inline (DrawCandidateSection), so every future backend (a real
// D3D12/Vulkan native path) would have had to be spliced into the middle
// of Fable/Xenos-specific decode logic. This header is the seam: a
// completely backend-neutral "what to draw" contract that knows nothing
// about Fable II, Xenos, PM4, guest memory, or even that a PPC recompiler
// exists upstream of it. A backend consumes NativeDrawCommand and nothing
// else; a decoder (today: fable_native_poc.cpp's DecodeAttempt) produces
// NativeDrawCommand and nothing else. Neither one includes the other's
// headers.
//
// This is deliberately minimal (native-rendering-phase3.md's explicit
// instruction: the first renderer does not need to be correct, complete,
// pretty, or fast) -- one topology, one fixed-size vertex array, no
// indices (the existing decode already expands indices into a flat
// triangle-list vertex stream; see DecodeCandidate in fable_native_poc.cpp),
// no material/texture/shader state (none of that has been captured from
// the guest yet -- see fable_native_poc.cpp's own "shader identity:
// UNKNOWN" note). Grow this struct only when a real capture/backend need
// requires it, not speculatively.
#pragma once

#include <cstdint>

namespace fable2::native_render {

// The only primitive topology this stack has runtime-validated evidence
// for (Phase 2 attempt 3: prim_type=4/D3DPT_TRIANGLELIST, 500/500). Not
// extended further until a captured draw actually needs it.
enum class PrimitiveTopology {
  kUnknown = 0,
  kTriangleList,
};

// Matches fable_native_poc.cpp's kMaxDecodedIndices -- generous headroom
// for this investigation's single-draw PoC, not a real engine limit.
constexpr int kMaxDrawVertices = 64;

struct NativeVertex {
  float x = 0.0f;
  float y = 0.0f;
  float z = 0.0f;
};

// One decoded, backend-ready draw command. `valid` is the only field a
// backend is required to check before touching the rest -- every other
// field is guaranteed well-formed (vertex_count in [0, kMaxDrawVertices],
// every vertices[i] finite) whenever valid is true. All of that validation
// already happened upstream (DecodeCandidate/DecodeLooksPlausible in
// fable_native_poc.cpp) before this struct is ever constructed; a backend
// must not need to re-derive or re-validate any Fable/Xenos-side
// assumption to safely consume this.
//
// No view/projection matrix: nothing upstream of this struct has ever
// captured a real transform (see fable_native_poc.cpp's own note on this),
// so none is invented here either. Vertices are in whatever space the
// guest's own vertex buffer stored them in -- a debug backend is free to
// fit them to a canvas however it likes (see ImGuiDrawListBackend in
// fable_native_poc.cpp); a real backend would need this investigation to
// first locate and capture the actual transform, which it has not done.
struct NativeDrawCommand {
  bool valid = false;
  PrimitiveTopology topology = PrimitiveTopology::kUnknown;
  int vertex_count = 0;
  NativeVertex vertices[kMaxDrawVertices];

  // Provenance only, for a debug backend's own display -- not part of the
  // "what to draw" contract above, and never consulted to decide anything.
  const char* debug_label = "";
  uint32_t debug_source_addr = 0;

  // Round 20 (Fable2PC, native 3D draw path): true when vertices[] above
  // have already had a real, captured rigid transform (rotation +
  // translation, structurally validated -- see
  // fable_native_poc.cpp's FindRigidTransformCandidate) applied, i.e. they
  // are in whatever space that transform's OUTPUT is (most likely world/
  // object space, going by the evidence -- see native-rendering-phase3.md
  // round 20), rather than raw guest vertex-buffer-local space. Still
  // provenance/debug metadata only, same as the two fields above -- a
  // backend is free to ignore it, and it carries no claim that this is a
  // complete camera-relative (view+projection) transform, only that SOME
  // real captured transform was used instead of none.
  bool world_transform_applied = false;

  // Round 20, fourth-build follow-up (native-rendering-phase3.md): true
  // when vertices[] above were selected/ordered using a real, captured
  // index buffer (ValidatedFetchConstantVertexInfo::index_data_valid --
  // see that field's own comment) rather than the blind "each decoded
  // record used exactly once, in buffer order" sequential assumption
  // every earlier capture used. Provenance/debug metadata only, same as
  // every other field in this block -- a backend is free to ignore it.
  bool used_real_indices = false;
};

// A native rendering backend accepts already-decoded, backend-neutral draw
// commands and is responsible for getting them on screen however its own
// graphics API requires. It must not know anything about Fable II, Xenos,
// PM4 packets, or guest memory -- see NativeDrawCommand's own comment for
// why that separation is the entire point of this header. Today's only
// implementation (ImGuiDrawListBackend, in fable_native_poc.cpp) draws into
// the existing F4 ImGui overlay via ImGui's own already-compiled solid-fill
// draw-list shader; a future implementation targeting a real native D3D12/
// Vulkan pipeline can be substituted here without touching any decode code
// above this interface.
class INativeRenderBackend {
 public:
  virtual ~INativeRenderBackend() = default;
  virtual void SubmitDrawCommand(const NativeDrawCommand& cmd) = 0;
};

}  // namespace fable2::native_render
