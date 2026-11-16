#pragma once

#include <atomic>
#include <chrono>
#include <imgui.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/overlay/debug_overlay.h>

namespace fable2 {
inline std::atomic<uint64_t> submitted_frames{0};

// Wires the F3 debug overlay's existing (previously unused for this title)
// "Guest: X.X FPS (X.X ms)" line to the same submitted_frames signal the F1
// FpsOverlay already uses, via ReXApp::SetGuestFrameStats. Pure read of an
// existing atomic counter plus a 0.5s sampling window (same approach as
// FpsOverlay::OnDraw) -- no gameplay/timing/rendering effect. Added
// 2026-09-16; see native/performance-investigation-phase1.md.
rex::ui::FrameStats GetGuestFrameStats();

// [FPS120] Resolve fable2_frame_rate (30/60/120) into the presentation interval
// and the guest refresh rate. Call once, before the runtime/GPU is set up.
void ApplyFrameRateSetting();

class FpsOverlay final : public rex::ui::ImGuiDialog {
 public:
  explicit FpsOverlay(rex::ui::ImGuiDrawer* drawer) : ImGuiDialog(drawer) {}
 protected:
  void OnDraw(ImGuiIO&) override {
    const auto now = std::chrono::steady_clock::now();
    const double seconds = std::chrono::duration<double>(now - sampled_at_).count();
    if (seconds >= 0.5) {
      const auto frames = submitted_frames.load(std::memory_order_relaxed);
      fps_ = double(frames - sampled_frames_) / seconds;
      sampled_frames_ = frames;
      sampled_at_ = now;
      ready_ = true;
    }
    ImGui::SetNextWindowPos(ImVec2(12, 12));
    ImGui::SetNextWindowBgAlpha(0.65f);
    if (ImGui::Begin("##fable2_fps", nullptr,
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
        ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_AlwaysAutoResize)) {
      if (!ready_) ImGui::TextUnformatted("FPS: ...");
      else if (fps_ > 0.0) ImGui::Text("%.1f FPS  |  %.1f ms", fps_, 1000.0 / fps_);
      else ImGui::TextUnformatted("0.0 FPS  |  waiting");
    }
    ImGui::End();
  }
 private:
  std::chrono::steady_clock::time_point sampled_at_ = std::chrono::steady_clock::now();
  uint64_t sampled_frames_ = submitted_frames.load(std::memory_order_relaxed);
  double fps_ = 0;
  bool ready_ = false;
};
}
