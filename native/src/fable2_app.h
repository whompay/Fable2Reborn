// fable2 - ReXGlue Recompiled Project
//
// Customize your app by overriding virtual hooks from rex::ReXApp.

#pragma once

#include <rex/rex_app.h>
#include <windows.h>
#include <rex/ui/keybinds.h>
#include <rex/ui/window.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include "pc_features.h"
#include "pc_perf.h"
#include "diagnostics/fable_native_poc.h"
#include "glyph_system.h"
#include "cursor_lock.h"
#include "raw_mouse.h"
#include <rex/perf/counter.h>

class Fable2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(
      rex::ui::WindowedAppContext& ctx) {
    return std::unique_ptr<Fable2App>(new Fable2App(ctx, "fable2",
        PPCImageConfig));
  }

  void OnPreSetup(rex::RuntimeConfig& config) override {
    // PERF-1006 (2026-10-06): 1 ms timer resolution, PGO profile writer (gen
    // build only), "[PERF-1006]" build marker. See src/pc_perf.cpp.
    fable2::perf::Startup();
    // Perf-only build (RelWithDebInfo, REXGLUE_ENABLE_PROFILING): activates the
    // existing rex::perf Tracy/counter registry as early as possible. This is
    // the only call site for Profiler::Startup() in the whole tree -- without
    // it, TracyIsStarted() (real Tracy manual-lifetime API) stays false for
    // the entire run and every SCOPE_profile_cpu_f zone (e.g. the function
    // dispatcher's per-call zone) and FrameMark are silent no-ops, even though
    // the F3 perf-counter overlay itself works independently of this. No-op in
    // Release builds (Profiler::Startup()'s body is itself #ifdef-guarded).
    // Added 2026-09-15 for the Bowerstone Market CPU-vs-GPU investigation;
    // see native/performance-investigation-phase1.md.
    rex::perf::Profiler::Startup();
    // Round 10 (2026-09-18, performance-investigation pivot): rex::perf's
    // counter registry zero-initializes its static storage either way, so
    // skipping this call was harmless before -- but Init() is also where
    // the perf_log_csv cvar now gets applied (see counter.cpp), and that
    // needs one real call site. Init() is documented safe to call more
    // than once, so adding this here carries no risk to the existing
    // Profiler::Startup() call above. See
    // native/performance-investigation-phase4.md.
    rex::perf::Init();
    // [FPS120] 30/60/120 FPS: must run before the GPU plugin starts the guest
    // vblank timer (it reads video_mode_refresh_rate once).
    fable2::ApplyFrameRateSetting();
    config.gpu_plugin = "xenos";
    fable2::glyph::Initialize();
  }

  // OnPostSetup() previously used for this call caused a boot regression
  // (black-screen hang, see git history / glyph-vfs-redirect-status.md) --
  // it fires synchronously inside ConstructRuntime(), on the same thread
  // that is concurrently finishing runtime setup, and the SDK gives no
  // assurance it's safe to call back into VFS-mutating code from there.
  //
  // OnPostLoadXexImage() is used instead: the SDK explicitly documents it
  // as intended for "data patches" (rex_app.h), and a REX_LOG_LEVEL=debug
  // capture (2026-09-11) confirmed the timing is safe for this specific
  // redirect: VFS exists ~174ms before this hook fires, and the engine's
  // "Permanent Bank Mounter" thread (which mounts art/gui from whatever
  // the redirect currently points at) doesn't start until ~472ms after --
  // a comfortable margin, not a race. This closes the same gap Initialize()
  // (too early, VFS doesn't exist yet) and OnResourceResolved() (too late,
  // ~19s in, long after the Bank Mounter already ran) each missed.
  void OnPostLoadXexImage() override {
    REXLOG_INFO("[PAGE063-V1] build includes sub_825588A8 / sub_82558CC0 / sub_82557A30 entry probes");
    REXLOG_INFO("[LifecycleTrace] OnPostLoadXexImage fired (XEX loaded/mapped, module not yet launched)");

    // Calling this here (rather than any earlier/later hook) was never
    // itself the cause of the black-screen boot freeze seen on 2026-09-11.
    // The real cause: VirtualFileSystem::ResolveSymbolicLink() infinite-loops
    // if a symlink's target basename starts with its source's basename (see
    // glyph_system.cpp's kGuiDirKbmTarget/kGuiDirX360Target comment for the
    // full mechanism). The mirror directories were renamed
    // (gui_kbm/gui_x360 -> _gui_kbm/_gui_x360) to eliminate that prefix
    // collision, which fixes the freeze without needing to move this call
    // anywhere else -- OnPostLoadXexImage remains the correct hook, firing
    // safely before the Bank Mounter thread's one-time directory mount.
    fable2::glyph::RefreshBankRedirect();
  }

  // F3's debug overlay already had a "Guest: X.X FPS (X.X ms)" line in its
  // source (rex/ui/overlay/debug_overlay.cpp), but nothing in this title ever
  // called SetGuestFrameStats to supply it, so it silently never rendered
  // (only the always-on Frame(ms) graph showed). OnCreateDialogs is the SDK's
  // documented hook for exactly this kind of dialog/overlay wiring, and runs
  // before the user can press F3. Wires the same submitted_frames signal the
  // F1 FpsOverlay already uses; adds a display only, no gameplay/render/timing
  // effect. Added 2026-09-16; see native/performance-investigation-phase1.md.
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {
    SetGuestFrameStats([] { return fable2::GetGuestFrameStats(); });
    // 2026-11-15 cursor lock: window and ImGui drawer both exist here.
    fable2::cursor_lock::Attach(window(), drawer);
  }

  void OnKeyDown(rex::ui::KeyEvent& event) override {
    fable2::glyph::OnKeyEvent(event, /*is_down=*/true);
    const auto key = event.virtual_key();
    // Round 8 diagnostic (native-rendering-phase3.md): a full Windows test
    // pressing F4 opened the SDK's own built-in Settings overlay
    // (rex::ui::ReXApp's "bind_settings" -> F4, registered in
    // tools/rexglue-source/src/ui/rex_app.cpp's SetupOverlays) instead of
    // this class's own NativePocOverlay -- even though the branch below
    // special-cases kF4 and returns before ever calling
    // rex::ui::ProcessKeyEvent(event) (the function that dispatches
    // "bind_settings"). That should be structurally impossible unless this
    // override either isn't being reached for F4, or `key` doesn't compare
    // equal to VirtualKey::kF4 the way this code expects. Logged
    // unconditionally (every key down, not just F1/F4/F11) so the next test
    // shows definitively what `key` resolves to and which branch runs,
    // instead of guessing further from source alone.
    REXLOG_INFO(
        "[Fable2App] [KeyDiag] OnKeyDown: virtual_key=0x{:02X} prev_state={} "
        "is_F1={} is_F4={} is_F11={}",
        static_cast<uint32_t>(key), event.prev_state(),
        key == rex::ui::VirtualKey::kF1, key == rex::ui::VirtualKey::kF4,
        key == rex::ui::VirtualKey::kF11);
    if (key == rex::ui::VirtualKey::kF1 || key == rex::ui::VirtualKey::kF11 ||
        key == rex::ui::VirtualKey::kF4) {
      if (!event.prev_state()) {
        if (key == rex::ui::VirtualKey::kF1) {
          if (fps_overlay_) fps_overlay_.reset();
          else fps_overlay_ = std::make_unique<fable2::FpsOverlay>(imgui_drawer());
        } else if (key == rex::ui::VirtualKey::kF4) {
          // Phase 3 (native/native-rendering-phase3.md): same construct-to-open/
          // reset-to-close toggle pattern as F1's FpsOverlay above.
          REXLOG_INFO(
              "[Fable2App] [KeyDiag] F4 branch taken -- toggling native_poc_overlay_ "
              "(currently {})",
              native_poc_overlay_ ? "open, will close" : "closed, will open");
          if (native_poc_overlay_) native_poc_overlay_.reset();
          else native_poc_overlay_ =
                   std::make_unique<fable2::native_poc::NativePocOverlay>(imgui_drawer());
          fable2::cursor_lock::SetAppOverlayNeedsMouse(native_poc_overlay_ != nullptr);
        } else {
          window()->SetFullscreen(!window()->IsFullscreen());
          fable2::cursor_lock::Update();
        }
      }
      event.set_handled(true);
      return;
    }
    REXLOG_INFO(
        "[Fable2App] [KeyDiag] virtual_key=0x{:02X} did not match F1/F4/F11 -- falling "
        "through to rex::ui::ProcessKeyEvent (this is what dispatches the SDK's own "
        "F3/F4/F7/Backtick binds)",
        static_cast<uint32_t>(key));
    rex::ui::ProcessKeyEvent(event);
    fable2::cursor_lock::Update();
  }

  void OnKeyUp(rex::ui::KeyEvent& event) override {
    fable2::glyph::OnKeyEvent(event, /*is_down=*/false);
  }

  void OnMouseDown(rex::ui::MouseEvent& event) override {
    fable2::glyph::OnMouseEvent(event, fable2::glyph::MouseEventKind::kDown);
    fable2::cursor_lock::Update();
  }

  void OnMouseUp(rex::ui::MouseEvent& event) override {
    fable2::glyph::OnMouseEvent(event, fable2::glyph::MouseEventKind::kUp);
  }

  void OnMouseMove(rex::ui::MouseEvent& event) override {
    fable2::glyph::OnMouseEvent(event, fable2::glyph::MouseEventKind::kMove);
    fable2::raw_mouse::OnMouseMove(event.dx(), event.dy());  // 2026-11-15 raw mouse look
  }

  void OnMouseWheel(rex::ui::MouseEvent& event) override {
    fable2::glyph::OnMouseEvent(event, fable2::glyph::MouseEventKind::kWheel);
  }

  // 2026-11-15 cursor lock: re-evaluate on focus / size / minimize changes
  // (these are protected virtuals in the prebuilt SDK's rex_app.h).
  void OnWindowFocusChanged(bool focused) override {
    (void)focused;
    fable2::cursor_lock::Update();
  }
  void OnWindowResized(uint32_t logical_width, uint32_t logical_height) override {
    (void)logical_width;
    (void)logical_height;
    fable2::cursor_lock::Update();
  }
  void OnWindowMinimized() override { fable2::cursor_lock::Update(); }
  void OnWindowRestored() override { fable2::cursor_lock::Update(); }

  void OnShutdown() override {
    fable2::cursor_lock::Shutdown();  // release ClipCursor / show cursor
    fable2::perf::Shutdown();  // PERF-1006: final PGO profile write (gen build only)
    fps_overlay_.reset();
    native_poc_overlay_.reset();
  }

  // Round 15 (native-rendering-phase3.md): lets ReXApp's input-active
  // callback (rex_app.cpp, ConstructRuntime) release mouse capture for
  // ImGui while native_poc_overlay_ (F4) is open -- see
  // HasExtraOverlaysOpen()'s own comment in rex_app.h for why this was
  // needed: without it, every click meant for this overlay's radio
  // buttons/buttons was being consumed as gameplay camera-look input
  // instead, because this overlay isn't one of the base class's own
  // tracked overlay members.
  //
  // Round 15, sixteenth follow-up: NOT marked `override` here. The game
  // project builds against the prebuilt tools/rexglue-sdk/win-amd64 package,
  // not the tools/rexglue-source tree this hook's base-class declaration
  // lives in (see the build-system note in rex_app.h / phase3.md) -- so
  // ReXApp::HasExtraOverlaysOpen() is invisible to this compiler and
  // `override` here was a hard compile error ("only virtual member
  // functions can be marked 'override'"), not just inert. Left as a plain
  // (non-virtual, for now) member function instead: harmless today, and it
  // will silently start actually overriding the base method -- picking up
  // real effect -- the moment the SDK is eventually rebuilt from
  // tools/rexglue-source, since the signature already matches.
  bool HasExtraOverlaysOpen() const {
    return native_poc_overlay_ != nullptr;
  }

  void OnConfigurePaths(rex::PathConfig& paths) override {
    wchar_t executable[32768] = {};
    const auto length = GetModuleFileNameW(nullptr, executable, 32768);
    if (!length || length >= 32768) return;
    auto directory = std::filesystem::path(executable).parent_path();
    for (int level = 0; level < 7; ++level) {
      if (std::filesystem::is_regular_file(directory / "extracted" / "default.xex")) {
        if (rex::cvar::GetFlagByName("game_data_root").empty())
          paths.game_data_root = directory / "extracted";
        if (rex::cvar::GetFlagByName("user_data_root").empty())
          paths.user_data_root = directory / "native" / "user-data";
        if (rex::cvar::GetFlagByName("cache_root").empty())
          paths.cache_root = directory / "native" / "cache";
        paths.config_path = directory / "native" / "fable2.toml";
        break;
      }
      auto parent = directory.parent_path();
      if (parent == directory) break;
      directory = parent;
    }
  }

 private:
  std::unique_ptr<fable2::FpsOverlay> fps_overlay_;
  std::unique_ptr<fable2::native_poc::NativePocOverlay> native_poc_overlay_;

  // Override virtual hooks for customization:
  // void OnPostInitLogging() override {}
  // void OnPreSetup(rex::RuntimeConfig& config) override {}
  // void OnLoadXexImage(std::string& xex_image) override {}
  // void OnPostLoadXexImage() override {}
  // void OnPostSetup() override {}
  // void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override {}
  // std::unique_ptr<rex::ui::ImGuiDialog> CreateAchievementsOverlay() override;
  // std::unique_ptr<rex::ui::AchievementNotificationDialog>
  // CreateAchievementNotificationDialog() override;
  // void OnShutdown() override {}
  // void OnConfigurePaths(rex::PathConfig& paths) override {}
};
