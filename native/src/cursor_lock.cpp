// fable2 - cursor lock (2026-11-15). See cursor_lock.h.

#include "cursor_lock.h"

#include <atomic>
#include <chrono>
#include <cstdint>

#include <imgui.h>
#include <imgui_internal.h>

#include <rex/cvar.h>
#include <rex/input/mnk/mnk_input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#if defined(_MSC_VER) || defined(__clang__)
#pragma comment(lib, "user32.lib")
#endif

REXCVAR_DEFINE_BOOL(fable2_lock_cursor, true, "Fable II",
    "Hide the mouse cursor and confine it to the game window during gameplay "
    "(released on alt-tab and while an interactive overlay is open).");

namespace fable2::cursor_lock {
namespace {

// UI-thread-only state.
rex::ui::Window* g_window = nullptr;
rex::ui::ImGuiDrawer* g_drawer = nullptr;
bool g_app_overlay_needs_mouse = false;
bool g_locked = false;
bool g_we_clipped = false;
RECT g_applied_clip{};
bool g_cursor_hidden_by_us = false;
bool g_mouse_look_suppressed = false;

// Cross-thread state.
std::atomic<rex::ui::WindowedAppContext*> g_app_context{nullptr};
std::atomic<bool> g_shutdown{false};
std::atomic<bool> g_update_queued{false};
std::atomic<int64_t> g_last_request_ms{0};
std::atomic<bool> g_game_owns_mouse{false};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool RectInside(const RECT& inner, const RECT& outer) {
  return inner.left >= outer.left && inner.top >= outer.top &&
         inner.right <= outer.right && inner.bottom <= outer.bottom;
}

bool RectEqual(const RECT& a, const RECT& b) {
  return a.left == b.left && a.top == b.top && a.right == b.right &&
         a.bottom == b.bottom;
}

// True if any ImGui window that takes mouse input is currently shown (SDK
// overlays, F4 PoC overlay, popups). NoInputs windows (F1 FPS overlay, the
// F3 watermark, achievement toasts), child windows, tooltips and ImGui's
// implicit "Debug##Default" fallback window are ignored. Also true while
// ImGui reports WantCaptureMouse.
bool ImGuiNeedsMouse() {
  if (!g_drawer) return false;
  // ImGuiDrawer::GetIO() makes the drawer's context current first.
  ImGuiIO& io = g_drawer->GetIO();
  if (io.WantCaptureMouse) return true;
  ImGuiContext* ctx = ImGui::GetCurrentContext();
  if (!ctx) return false;
  constexpr ImGuiWindowFlags kIgnore = ImGuiWindowFlags_NoMouseInputs |
                                       ImGuiWindowFlags_ChildWindow |
                                       ImGuiWindowFlags_Tooltip;
  for (int i = 0; i < ctx->Windows.Size; ++i) {
    const ImGuiWindow* w = ctx->Windows[i];
    if (!w || w->IsFallbackWindow || w->Hidden) continue;
    if (!(w->Active || w->WasActive)) continue;
    if (w->Flags & kIgnore) continue;
    return true;
  }
  return false;
}

HWND GetHwnd() {
  if (!g_window) return nullptr;
  return static_cast<HWND>(g_window->GetNativeWindowHandle());
}

bool GetClientRectOnScreen(HWND hwnd, RECT& out) {
  RECT rc{};
  if (!GetClientRect(hwnd, &rc)) return false;
  POINT tl{rc.left, rc.top};
  POINT br{rc.right, rc.bottom};
  if (!ClientToScreen(hwnd, &tl) || !ClientToScreen(hwnd, &br)) return false;
  out = RECT{tl.x, tl.y, br.x, br.y};
  return out.right > out.left && out.bottom > out.top;
}

void SetMouseLookSuppressed(bool suppressed) {
  if (suppressed == g_mouse_look_suppressed) return;
  g_mouse_look_suppressed = suppressed;
  rex::input::mnk::SetMouseLookActive(!suppressed);
}

void ReleaseLock() {
  if (g_we_clipped) {
    RECT cur{};
    // Only undo our own clip; leave a clip SDL set (relative mode) to SDL.
    if (GetClipCursor(&cur) && RectEqual(cur, g_applied_clip)) {
      ClipCursor(nullptr);
    }
    g_we_clipped = false;
  }
  if (g_cursor_hidden_by_us) {
    ShowCursor(TRUE);
    g_cursor_hidden_by_us = false;
  }
  if (g_locked) {
    g_locked = false;
    REXLOG_INFO("[CursorLock] released");
  }
}

void ApplyLock(HWND hwnd, const RECT& client) {
  RECT cur{};
  const bool have_cur = GetClipCursor(&cur) != FALSE;
  bool need_clip = true;
  if (have_cur) {
    if (RectEqual(cur, client)) {
      need_clip = false;
    } else if (!(g_we_clipped && RectEqual(cur, g_applied_clip)) &&
               RectInside(cur, client)) {
      // A tighter clip that isn't ours (SDL relative mouse mode clips to the
      // window centre) -- already confined, don't fight it.
      need_clip = false;
    }
  }
  if (need_clip) {
    if (ClipCursor(&client)) {
      g_we_clipped = true;
      g_applied_clip = client;
    }
  }

  if (!g_cursor_hidden_by_us) {
    // Display counter, independent of SDL's SetCursor-based hiding, so it
    // does not interfere with the SDK's own capture bookkeeping.
    ShowCursor(FALSE);
    g_cursor_hidden_by_us = true;
  }

  if (!g_locked) {
    g_locked = true;
    // Start from the centre of the window if the pointer is outside it.
    POINT pt{};
    if (GetCursorPos(&pt) &&
        !(pt.x >= client.left && pt.x < client.right && pt.y >= client.top &&
          pt.y < client.bottom)) {
      SetCursorPos((client.left + client.right) / 2,
                   (client.top + client.bottom) / 2);
    }
    REXLOG_INFO("[CursorLock] locked to client rect ({},{})-({},{})",
                client.left, client.top, client.right, client.bottom);
  }
  (void)hwnd;
}

}  // namespace

void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer) {
  g_window = window;
  g_drawer = drawer;
  g_app_context.store(window ? &window->app_context() : nullptr,
                      std::memory_order_release);
  g_shutdown.store(false, std::memory_order_release);
  Update();
}

void SetAppOverlayNeedsMouse(bool needs_mouse) {
  g_app_overlay_needs_mouse = needs_mouse;
  Update();
}

void Update() {
  if (g_shutdown.load(std::memory_order_acquire) || !g_window) return;

  if (!REXCVAR_GET(fable2_lock_cursor)) {
    // Feature off: back to stock SDK behaviour.
    g_game_owns_mouse.store(g_window->HasFocus(), std::memory_order_relaxed);
    SetMouseLookSuppressed(false);
    ReleaseLock();
    return;
  }

  const bool overlay_needs_mouse = g_app_overlay_needs_mouse || ImGuiNeedsMouse();
  SetMouseLookSuppressed(overlay_needs_mouse);

  HWND hwnd = GetHwnd();
  RECT client{};
  const bool should_lock = hwnd && IsWindow(hwnd) && !IsIconic(hwnd) &&
                           g_window->HasFocus() &&
                           GetForegroundWindow() == hwnd &&
                           !overlay_needs_mouse &&
                           GetClientRectOnScreen(hwnd, client);
  g_game_owns_mouse.store(!overlay_needs_mouse && hwnd && IsWindow(hwnd) && !IsIconic(hwnd) &&
                              g_window->HasFocus() && GetForegroundWindow() == hwnd,
                          std::memory_order_relaxed);
  if (should_lock) {
    ApplyLock(hwnd, client);
  } else {
    ReleaseLock();
  }
}

void RequestUpdate() {
  if (g_shutdown.load(std::memory_order_acquire)) return;
  rex::ui::WindowedAppContext* ctx = g_app_context.load(std::memory_order_acquire);
  if (!ctx) return;
  // ~60 Hz is plenty; the guest polls XInput several times per frame.
  const int64_t now = NowMs();
  if (now - g_last_request_ms.load(std::memory_order_relaxed) < 15) return;
  bool expected = false;
  if (!g_update_queued.compare_exchange_strong(expected, true)) return;
  g_last_request_ms.store(now, std::memory_order_relaxed);
  if (!ctx->CallInUIThread([] {
        g_update_queued.store(false, std::memory_order_release);
        Update();
      })) {
    g_update_queued.store(false, std::memory_order_release);
  }
}

bool GameOwnsMouse() {
  return g_game_owns_mouse.load(std::memory_order_relaxed);
}

void Shutdown() {
  if (g_shutdown.exchange(true, std::memory_order_acq_rel)) return;
  g_game_owns_mouse.store(false, std::memory_order_relaxed);
  g_app_context.store(nullptr, std::memory_order_release);
  SetMouseLookSuppressed(false);
  const bool was_locked = g_locked || g_we_clipped;
  ReleaseLock();
  // Belt and braces: never leave the cursor clipped after exit (also drops a
  // clip SDL relative mode may still hold at this point).
  if (was_locked) ClipCursor(nullptr);
  g_window = nullptr;
  g_drawer = nullptr;
}

}  // namespace fable2::cursor_lock
