// Fable2PC -- Phase 3 diagnostic instrumentation: native 3D draw path
// (implementation). See fable_native_d3d12_backend.h for the full design
// rationale and native/native-rendering-phase3.md's round 20 entry for the
// investigation that led here (the ImDrawCmd::UserCallback dead end, and
// the two new zero-layout-change SDK accessors this relies on instead).
#include "diagnostics/fable_native_d3d12_backend.h"

#include <rex/logging.h>
#include <rex/ui/d3d12/d3d12_presenter.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>

#include <algorithm>
#include <cstring>

#include <imgui.h>

#include "diagnostics/native_shader_bytecode.h"

namespace fable2::native_render {

namespace {
// Solid, flat orange -- deliberately visually distinct from ImGui's own
// default fill colors (light blue/orange used by ImGuiDrawListBackend
// above) so a screenshot can tell the two draw paths apart unambiguously.
// No lighting/materials/textures -- matches round 20's own scope note.
constexpr float kDrawColor[4] = {1.0f, 0.35f, 0.05f, 1.0f};
}  // namespace

bool D3D12NativeRenderBackend::EnsurePipelineCreated(ID3D12Device* device) {
  if (pipeline_create_attempted_) {
    return pipeline_create_ok_;
  }
  pipeline_create_attempted_ = true;

  // The vertex shader's own DXIL container carries an embedded root
  // signature (RTS0 chunk, via its [RootSignature(...)] HLSL attribute --
  // see native_shader_bytecode.h's own comment), so CreateRootSignature
  // is called directly with that same blob -- no separate
  // D3D12SerializeRootSignature() call, so no d3d12.lib link dependency
  // is needed anywhere in native/'s own build for this.
  HRESULT hr = device->CreateRootSignature(
      0, d3d12_shaders::kNativeVsBytecode, d3d12_shaders::kNativeVsBytecode_size,
      IID_PPV_ARGS(&root_signature_));
  if (FAILED(hr)) {
    last_status_ = "CreateRootSignature failed";
    REXLOG_INFO("[NativeDrawPoc] [D3D12Backend] CreateRootSignature failed hr={:#010x}",
                static_cast<unsigned int>(hr));
    return false;
  }

  D3D12_INPUT_ELEMENT_DESC input_element = {};
  input_element.SemanticName = "POSITION";
  input_element.SemanticIndex = 0;
  input_element.Format = DXGI_FORMAT_R32G32B32_FLOAT;
  input_element.InputSlot = 0;
  input_element.AlignedByteOffset = 0;
  input_element.InputSlotClass = D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA;
  input_element.InstanceDataStepRate = 0;

  D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc = {};
  pso_desc.pRootSignature = root_signature_.Get();
  pso_desc.VS = {d3d12_shaders::kNativeVsBytecode, d3d12_shaders::kNativeVsBytecode_size};
  pso_desc.PS = {d3d12_shaders::kNativePsBytecode, d3d12_shaders::kNativePsBytecode_size};
  pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  pso_desc.SampleMask = UINT_MAX;
  pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  // No known real winding order for this decoded geometry yet (round 20's
  // own scope: geometry + topology + transforms, not yet a validated
  // front-face convention) -- cull nothing, so the triangle is visible
  // regardless of which way it winds.
  pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  pso_desc.RasterizerState.DepthClipEnable = FALSE;
  pso_desc.DepthStencilState.DepthEnable = FALSE;
  pso_desc.DepthStencilState.StencilEnable = FALSE;
  pso_desc.InputLayout = {&input_element, 1};
  pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  pso_desc.NumRenderTargets = 1;
  // Matches the presenter's own back-buffer format exactly (public
  // constant, no new accessor needed -- rex::ui::d3d12::D3D12Presenter::
  // kSwapChainFormat, d3d12_presenter.h).
  pso_desc.RTVFormats[0] = rex::ui::d3d12::D3D12Presenter::kSwapChainFormat;
  pso_desc.SampleDesc.Count = 1;

  hr = device->CreateGraphicsPipelineState(&pso_desc, IID_PPV_ARGS(&pipeline_state_));
  if (FAILED(hr)) {
    last_status_ = "CreateGraphicsPipelineState failed";
    REXLOG_INFO("[NativeDrawPoc] [D3D12Backend] CreateGraphicsPipelineState failed hr={:#010x}",
                static_cast<unsigned int>(hr));
    return false;
  }

  for (int i = 0; i < kUploadRingSize; ++i) {
    D3D12_HEAP_PROPERTIES heap_props = {};
    heap_props.Type = D3D12_HEAP_TYPE_UPLOAD;
    heap_props.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_UNKNOWN;
    heap_props.MemoryPoolPreference = D3D12_MEMORY_POOL_UNKNOWN;

    D3D12_RESOURCE_DESC buffer_desc = {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Alignment = 0;
    buffer_desc.Width = sizeof(float) * 3 * static_cast<UINT64>(kMaxDrawVertices);
    buffer_desc.Height = 1;
    buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1;
    buffer_desc.Format = DXGI_FORMAT_UNKNOWN;
    buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    buffer_desc.Flags = D3D12_RESOURCE_FLAG_NONE;

    hr = device->CreateCommittedResource(&heap_props, D3D12_HEAP_FLAG_NONE, &buffer_desc,
                                         D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                         IID_PPV_ARGS(&vertex_upload_buffers_[i]));
    if (FAILED(hr)) {
      last_status_ = "CreateCommittedResource (vertex upload buffer) failed";
      REXLOG_INFO(
          "[NativeDrawPoc] [D3D12Backend] CreateCommittedResource (upload buffer {}) failed "
          "hr={:#010x}",
          i, static_cast<unsigned int>(hr));
      return false;
    }
    D3D12_RANGE no_read = {0, 0};
    hr = vertex_upload_buffers_[i]->Map(0, &no_read, &vertex_upload_mapped_[i]);
    if (FAILED(hr)) {
      last_status_ = "Map (vertex upload buffer) failed";
      REXLOG_INFO("[NativeDrawPoc] [D3D12Backend] Map (upload buffer {}) failed hr={:#010x}", i,
                  static_cast<unsigned int>(hr));
      return false;
    }
  }

  pipeline_create_ok_ = true;
  last_status_ = "pipeline created OK";
  REXLOG_INFO(
      "[NativeDrawPoc] [D3D12Backend] pipeline created OK (root signature from embedded RTS0 + "
      "PSO + {} upload-ring vertex buffers, {} vertices/{} bytes each)",
      kUploadRingSize, kMaxDrawVertices, sizeof(float) * 3 * kMaxDrawVertices);
  return true;
}

void D3D12NativeRenderBackend::SubmitDrawCommand(const NativeDrawCommand& cmd) {
  if (!cmd.valid || cmd.vertex_count < 3 || cmd.topology != PrimitiveTopology::kTriangleList) {
    last_status_ = "command not valid / <3 vertices / not triangle_list";
    return;
  }
  if (!imgui_drawer_) {
    last_status_ = "no imgui_drawer_ (constructed without one?)";
    return;
  }
  rex::ui::Presenter* presenter = imgui_drawer_->presenter();
  if (!presenter) {
    last_status_ = "ImGuiDrawer::presenter() returned null (no presenter attached this frame)";
    return;
  }
  auto* d3d12_presenter = dynamic_cast<rex::ui::d3d12::D3D12Presenter*>(presenter);
  if (!d3d12_presenter) {
    last_status_ = "presenter is not a D3D12Presenter (non-D3D12 graphics backend?)";
    return;
  }
  ID3D12GraphicsCommandList* command_list = d3d12_presenter->current_paint_command_list();
  if (!command_list) {
    last_status_ = "current_paint_command_list() returned null (called outside a live paint?)";
    return;
  }
  ID3D12Device* device = d3d12_presenter->provider().GetDevice();
  if (!device) {
    last_status_ = "D3D12Provider::GetDevice() returned null";
    return;
  }

  if (!EnsurePipelineCreated(device)) {
    return;  // last_status_ already set by EnsurePipelineCreated
  }

  const int vertex_count = std::min(cmd.vertex_count, kMaxDrawVertices);
  if (vertex_count < 3) {
    last_status_ = "vertex_count clamped below 3";
    return;
  }

  // Same CPU-side bounding-box-fit "projection" ImGuiDrawListBackend uses
  // for its own canvas (see that class's own comment) -- NOT a real view/
  // projection transform (round 20's own scope: geometry + topology +
  // transforms first, not full camera placement yet). This makes the two
  // backends a like-for-like comparison: same input geometry, same
  // placement logic, genuinely different (real GPU vs. ImGui 2D
  // draw-list) rendering path.
  float min_x = cmd.vertices[0].x, max_x = cmd.vertices[0].x;
  float min_y = cmd.vertices[0].y, max_y = cmd.vertices[0].y;
  for (int i = 1; i < vertex_count; ++i) {
    min_x = std::min(min_x, cmd.vertices[i].x);
    max_x = std::max(max_x, cmd.vertices[i].x);
    min_y = std::min(min_y, cmd.vertices[i].y);
    max_y = std::max(max_y, cmd.vertices[i].y);
  }
  const float span_x = (max_x - min_x) > 1e-6f ? (max_x - min_x) : 1.0f;
  const float span_y = (max_y - min_y) > 1e-6f ? (max_y - min_y) : 1.0f;

  upload_ring_index_ = (upload_ring_index_ + 1) % kUploadRingSize;
  float* dst = static_cast<float*>(vertex_upload_mapped_[upload_ring_index_]);
  for (int i = 0; i < vertex_count; ++i) {
    const float nx = (cmd.vertices[i].x - min_x) / span_x;
    const float ny = (cmd.vertices[i].y - min_y) / span_y;
    // [0,1] -> [-1,1] clip space. Unlike ImGuiDrawListBackend's own
    // to_screen (which flips Y because screen space is +Y-down), clip
    // space is +Y-up, so mapping ny directly (no flip) lands on the same
    // visual "up" -- not verified against a known-good reference beyond
    // that reasoning; a real run's screenshot is what actually confirms
    // it looks right side up.
    dst[i * 3 + 0] = nx * 2.0f - 1.0f;
    dst[i * 3 + 1] = ny * 2.0f - 1.0f;
    dst[i * 3 + 2] = 0.0f;
  }

  D3D12_VERTEX_BUFFER_VIEW vbv = {};
  vbv.BufferLocation = vertex_upload_buffers_[upload_ring_index_]->GetGPUVirtualAddress();
  vbv.StrideInBytes = sizeof(float) * 3;
  vbv.SizeInBytes = sizeof(float) * 3 * static_cast<UINT>(vertex_count);

  ImGuiIO& io = ImGui::GetIO();
  const float width = io.DisplaySize.x > 0.0f ? io.DisplaySize.x : 1.0f;
  const float height = io.DisplaySize.y > 0.0f ? io.DisplaySize.y : 1.0f;
  D3D12_VIEWPORT viewport = {0.0f, 0.0f, width, height, 0.0f, 1.0f};
  D3D12_RECT scissor = {0, 0, static_cast<LONG>(width), static_cast<LONG>(height)};

  // ImGui's own Begin() (D3D12ImmediateDrawer, called later this same
  // frame from RenderDrawLists -- see D3D12Presenter::
  // current_paint_command_list()'s own comment for the exact ordering)
  // always re-sets its own root signature/PSO/viewport/scissor before ITS
  // draws, so setting all of this here does not leak into or corrupt
  // ImGui's own rendering afterwards; everything below is set explicitly
  // (rather than relying on whatever an earlier pass this frame left
  // behind) so THIS draw is correct regardless of prior state.
  command_list->SetGraphicsRootSignature(root_signature_.Get());
  command_list->SetPipelineState(pipeline_state_.Get());
  command_list->SetGraphicsRoot32BitConstants(0, 4, kDrawColor, 0);
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  command_list->IASetVertexBuffers(0, 1, &vbv);
  command_list->RSSetViewports(1, &viewport);
  command_list->RSSetScissorRects(1, &scissor);

  command_list->DrawInstanced(static_cast<UINT>(vertex_count), 1, 0, 0);

  has_drawn_at_least_once_ = true;
  last_status_ = "drew OK (real D3D12 DrawInstanced)";
}

}  // namespace fable2::native_render
