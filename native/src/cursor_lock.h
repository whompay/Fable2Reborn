// fable2 - cursor lock (2026-11-15)
//
// Keeps the mouse cursor hidden and confined (Win32 ClipCursor) to the game
// window's client area while the game owns the mouse, and releases it when the
// window loses focus, when an interactive ImGui overlay is open (F4
// NativePocOverlay, the SDK's F3 debug / console / settings / achievements
// overlays -- anything that is not ImGuiWindowFlags_NoMouseInputs, so the F1
// FPS overlay does NOT release it), when the cvar fable2_lock_cursor is false,
// and on shutdown.
//
// While an interactive overlay is open it also calls
// rex::input::mnk::SetMouseLookActive(false), which makes the (prebuilt) SDK
// MnkInputDriver drop its own relative-mode mouse capture so the overlay can
// actually be clicked. This is what lets mnk_mouse (mouse-look) be on again;
// it had been turned off in fable2.toml only because the F4 overlay was
// unclickable while the SDK held the mouse.
//
// Threading: Attach / SetAppOverlayNeedsMouse / Update / Shutdown must be
// called on the UI thread. RequestUpdate may be called from any thread (it is
// called once per guest XInput poll) and just queues a coalesced, throttled
// Update onto the UI thread.

#pragma once

namespace rex::ui {
class Window;
class ImGuiDrawer;
}  // namespace rex::ui

namespace fable2::cursor_lock {

void Attach(rex::ui::Window* window, rex::ui::ImGuiDrawer* drawer);
void SetAppOverlayNeedsMouse(bool needs_mouse);
void Update();
void RequestUpdate();
void Shutdown();

// 2026-11-15 raw mouse: any thread. True while the game window is focused
// and no interactive overlay needs the mouse (independent of the
// fable2_lock_cursor cvar). Updated by Update().
bool GameOwnsMouse();

}  // namespace fable2::cursor_lock
