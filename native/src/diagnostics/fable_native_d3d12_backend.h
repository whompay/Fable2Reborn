// Fable2PC -- Phase 3 diagnostic instrumentation: native 3D draw path
// (native-rendering-phase3.md round 20, "native 3D draw path" follow-up).
//
// The FIRST real write into the live D3D12 pipeline this investigation has
// made -- every earlier round (Phase 1 through round 20's own index-buffer
// work) was strictly read-only guest-memory/register instrumentation. This
// backend submits an actual GPU draw call: a real compiled vertex+pixel
// shader, a real root signature, a real PSO, and a real uploaded vertex
// buffer, issued into the SAME live D3D12 command list the F4 overlay's own
// ImGui rendering already safely uses this frame -- not ImGui's own 2D
// draw-list fill shader (that's ImGuiDrawListBackend, kept unchanged as the
// reference/fallback path -- see native_draw_repr.h and round 7's own
// comment: "A future real-D3D12/Vulkan backend implements the same
// INativeRenderBackend interface and does not touch this class or anything
// upstream of it").
//
// How the live command list is reached without ImDrawCmd::UserCallback
// (confirmed, by reading this SDK's own ImGuiDrawer::RenderDrawLists body,
// to not be forwarded) and without any binary hooking of prebuilt code: two
// new zero-layout-change SDK accessors (ImGuiDrawer::presenter(),
// D3D12Presenter::current_paint_command_list() -- see their own comments in
// rex/ui/imgui_drawer.h and rex/ui/d3d12/d3d12_presenter.h) expose EXISTING
// private members as plain inline getters, the same technique already
// validated this round by CommandProcessor::register_file()
// (rex/graphics/command_processor.h). SubmitDrawCommand() must only ever be
// called from within an ImGuiDialog::OnDraw() override -- the one place
// those accessors are guaranteed to return live, correctly-bound state
// (same thread, inside the SAME PaintAndPresent call that constructed the
// D3D12UIDrawContext and is about to hand it to RenderDrawLists).
#pragma once

#include <cstdint>

#include <rex/ui/d3d12/d3d12_api.h>

#include "diagnostics/native_draw_repr.h"

namespace rex::ui {
class ImGuiDrawer;
}  // namespace rex::ui

namespace fable2::native_render {

class D3D12NativeRenderBackend final : public INativeRenderBackend {
 public:
  explicit D3D12NativeRenderBackend(rex::ui::ImGuiDrawer* imgui_drawer)
      : imgui_drawer_(imgui_drawer) {}

  void SubmitDrawCommand(const NativeDrawCommand& cmd) override;

  // For the overlay's own honest status display -- distinct booleans so
  // "pipeline never created" vs. "pipeline created but nothing valid
  // submitted yet" vs. "drew successfully at least once" are all
  // distinguishable, matching this project's calibrated-reporting
  // discipline.
  bool pipeline_create_attempted() const { return pipeline_create_attempted_; }
  bool pipeline_create_ok() const { return pipeline_create_ok_; }
  bool has_drawn_at_least_once() const { return has_drawn_at_least_once_; }
  const char* last_status() const { return last_status_; }

 private:
  static constexpr int kUploadRingSize = 3;

  bool EnsurePipelineCreated(ID3D12Device* device);

  rex::ui::ImGuiDrawer* imgui_drawer_ = nullptr;

  bool pipeline_create_attempted_ = false;
  bool pipeline_create_ok_ = false;
  const char* last_status_ = "not yet attempted";

  Microsoft::WRL::ComPtr<ID3D12RootSignature> root_signature_;
  Microsoft::WRL::ComPtr<ID3D12PipelineState> pipeline_state_;

  // Round 20 (native 3D draw path): a small ring of upload-heap vertex
  // buffers, not just one, cycled by frame -- a single persistently-mapped
  // buffer overwritten every frame would race the GPU's own read of the
  // PREVIOUS frame's draw if this resource is not on the same
  // wait-for-completion cadence the presenter's swap-chain buffers
  // themselves use (paint_submission_tracker, kSwapChainBufferCount --
  // see d3d12_presenter.h). This is a cheap, standard mitigation (not a
  // full fence-based wait, which this diagnostic overlay does not have
  // the hooks to perform) -- accepted as a known simplification, flagged
  // here rather than silently assumed safe.
  Microsoft::WRL::ComPtr<ID3D12Resource> vertex_upload_buffers_[kUploadRingSize];
  void* vertex_upload_mapped_[kUploadRingSize] = {};
  int upload_ring_index_ = 0;

  bool has_drawn_at_least_once_ = false;
};

}  // namespace fable2::native_render
