/**
 * @file MenuHost.cpp
 * @brief Owns the in-game Mod Menu's ImGui context, GLES backend and frame loop (see MenuHost.hpp).
 */

#include "pl/menu/MenuHost.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include "imgui.h"
#include "imgui_impl_opengl3.h"

#include "pl/Logger.hpp"
#include "pl/menu/MenuTheme.hpp"

namespace pl::menu {
namespace {

std::atomic_bool g_wanted{false};
std::atomic_bool g_live{false};
std::atomic_bool g_open{false};
std::atomic_bool g_backPressed{false};

/// Guards the launcher-thread setters against the render-thread frame loop.
std::mutex g_mutex;
std::vector<MenuModuleRow> g_catalogue;
std::string g_emptyReason;
std::vector<TouchInput> g_pendingTouches;
MenuState g_state;
/** Events the launcher drains each frame (a toggle, a config change, a close). */
std::vector<MenuEvent> g_events;

bool g_imguiReady = false;

/// Tracks the touch pointer ImGui is following, so a move without a fresh down still maps.
bool g_touchActive = false;
ImVec2 g_lastTouchPos{0, 0};

void PushEvent(MenuEvent event) {
  std::lock_guard<std::mutex> lock(g_mutex);
  // Bounded: a launcher that stops draining must not grow this without limit.
  if (g_events.size() > 256) g_events.erase(g_events.begin());
  g_events.push_back(std::move(event));
}

MenuActions BuildActions() {
  MenuActions actions;
  actions.onToggleModule = [](const std::string &key, bool enabled) {
    PushEvent(MenuEvent{MenuEventKind::ToggleModule, key, enabled ? "true" : "false"});
  };
  actions.onSetConfig = [](const std::string &key, const std::string &configKey,
                           const std::string &value) {
    PushEvent(MenuEvent{MenuEventKind::SetConfig, key, configKey + "=" + value});
  };
  actions.onToggleFavorite = [](const std::string &key) {
    PushEvent(MenuEvent{MenuEventKind::ToggleFavorite, key, ""});
  };
  actions.onClose = []() { PushEvent(MenuEvent{MenuEventKind::Close, "", ""}); };
  return actions;
}

/// Translates the queued Android touches into ImGui mouse state. Called on the render thread.
void ApplyInput(float width, float height) {
  ImGuiIO &io = ImGui::GetIO();
  io.DisplaySize = ImVec2(width, height);
  if (io.DeltaTime <= 0.0f) io.DeltaTime = 1.0f / 60.0f;

  std::vector<TouchInput> touches;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    touches.swap(g_pendingTouches);
  }
  if (g_backPressed.exchange(false, std::memory_order_relaxed)) {
    g_open.store(false, std::memory_order_relaxed);
    PushEvent(MenuEvent{MenuEventKind::Close, "", ""});
  }
  for (const TouchInput &t : touches) {
    // The touch arrives in the view's coordinate space; ImGui's display is the same surface, so no
    // scaling is needed when the two agree. When the view reports its own size, scale to it so a
    // letterboxed or rotated surface still maps correctly.
    float x = t.x;
    float y = t.y;
    if (t.viewportWidth > 0.0f && t.viewportHeight > 0.0f) {
      x *= width / t.viewportWidth;
      y *= height / t.viewportHeight;
    }
    g_lastTouchPos = ImVec2(x, y);
    io.AddMousePosEvent(x, y);
    switch (t.action) {
    case 0: // ACTION_DOWN
      g_touchActive = true;
      io.AddMouseButtonEvent(0, true);
      break;
    case 1: // ACTION_UP
      g_touchActive = false;
      io.AddMouseButtonEvent(0, false);
      break;
    case 2: // ACTION_MOVE
      if (g_touchActive) io.AddMouseButtonEvent(0, true);
      break;
    case 3: // ACTION_CANCEL
      g_touchActive = false;
      io.AddMouseButtonEvent(0, false);
      break;
    default:
      break;
    }
  }
}

bool InitImGui() {
  if (g_imguiReady) return true;
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO &io = ImGui::GetIO();
  io.IniFilename = nullptr; // no imgui.ini on the game's storage
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  theme::ApplyStyle();
  if (!ImGui_ImplOpenGL3_Init(nullptr)) {
    sweetLoaderLogger.warn("Mod Menu: the GLES3 backend refused to initialise");
    ImGui::DestroyContext();
    return false;
  }
  g_imguiReady = true;
  sweetLoaderLogger.info("Mod Menu: ImGui context live");
  return true;
}

} // namespace

void InstallMenu() {
  g_wanted.store(true, std::memory_order_relaxed);
}

bool IsMenuLive() { return g_live.load(std::memory_order_relaxed); }

bool IsOpen() { return g_open.load(std::memory_order_relaxed); }

void SetOpen(bool open) {
  g_open.store(open, std::memory_order_relaxed);
  if (!open) g_touchActive = false;
}

void SetCatalogue(std::vector<MenuModuleRow> modules, std::string emptyReason) {
  std::lock_guard<std::mutex> lock(g_mutex);
  g_catalogue = std::move(modules);
  g_emptyReason = std::move(emptyReason);
}

void PushTouch(const TouchInput &touch) {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_pendingTouches.size() > 64) return; // a stalled frame must not queue unbounded input
  g_pendingTouches.push_back(touch);
}

void PushBack() { g_backPressed.store(true, std::memory_order_relaxed); }

bool RegisterFont(theme::FontRole role, const void *data, int size) {
  // Sizes match the pack's roles: body 15, bold 13, title 19, display 17.
  const float px = role == theme::FontRole::BodyBold ? 13.0f
                   : role == theme::FontRole::Title  ? 19.0f
                   : role == theme::FontRole::Display ? 17.0f
                                                     : 15.0f;
  return theme::AddFont(role, data, size, px);
}

std::vector<MenuEvent> DrainEvents() {
  std::lock_guard<std::mutex> lock(g_mutex);
  std::vector<MenuEvent> out;
  out.swap(g_events);
  return out;
}

void Frame(float viewportWidth, float viewportHeight) {
  if (!g_wanted.load(std::memory_order_relaxed)) return;
  if (viewportWidth <= 0.0f || viewportHeight <= 0.0f) return;
  if (!InitImGui()) return;
  g_live.store(true, std::memory_order_relaxed);

  ApplyInput(viewportWidth, viewportHeight);

  // Copy the catalogue under the lock, then render from the copy: the render thread must not hold
  // the launcher's mutex for the length of a frame.
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    g_state.modules = g_catalogue;
    g_state.emptyReason = g_emptyReason;
  }
  g_state.open = g_open.load(std::memory_order_relaxed);

  ImGui_ImplOpenGL3_NewFrame();
  ImGui::NewFrame();
  RenderMenu(g_state, BuildActions(), ImVec2(viewportWidth, viewportHeight));
  ImGui::Render();
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

} // namespace pl::menu
