/**
 * @file MenuTheme.cpp
 * @brief ImGui style + font loading for the in-game Mod Menu (see MenuTheme.hpp).
 */

#include "pl/menu/MenuTheme.hpp"

#include <cstdio>
#include <string>

namespace pl::menu::theme {
namespace {

Fonts g_fonts;

/// The role a loaded pointer belongs to, so a later AddFont for the same role is a no-op.
ImFont *&SlotFor(FontRole role) {
  switch (role) {
  case FontRole::BodyMedium: return g_fonts.bodyMedium;
  case FontRole::BodyBold: return g_fonts.bodyBold;
  case FontRole::Title: return g_fonts.title;
  case FontRole::Display: return g_fonts.display;
  case FontRole::Body:
  default: return g_fonts.body;
  }
}

} // namespace

Fonts &GetFonts() { return g_fonts; }

bool AddFont(FontRole role, const void *data, int size, float pixelSize) {
  if (data == nullptr || size <= 0) return false;
  ImGuiIO &io = ImGui::GetIO();
  if (io.Fonts == nullptr) return false;

  // The atlas is owned by the ImGui context; adding a face after it has been built is undefined, so
  // the caller must do this before the first frame. A null result means the face could not be read,
  // which is a soft failure: the widgets fall back to the default font.
  ImFontConfig config;
  config.FontDataOwnedByAtlas = false; // the caller keeps the bytes alive for the session
  ImFont *font = io.Fonts->AddFontFromMemoryTTF(const_cast<void *>(data), size, pixelSize, &config);
  if (font == nullptr) return false;
  SlotFor(role) = font;
  if (role == FontRole::Body && io.FontDefault == nullptr) io.FontDefault = font;
  return true;
}

Fonts LoadFonts(const char *assetDir) {
  if (assetDir == nullptr) return g_fonts;
  const std::string dir(assetDir);
  auto load = [&](const char *file, float px) {
    const std::string path = dir + "/" + file;
    return ImGui::GetIO().Fonts->AddFontFromFileTTF(path.c_str(), px);
  };
  g_fonts.body = load("Inter-Medium.otf", 15.0f);
  g_fonts.bodyMedium = load("Inter-SemiBold.otf", 15.0f);
  g_fonts.bodyBold = load("Inter-Bold.otf", 13.0f);
  g_fonts.title = load("Poppins-Medium.ttf", 19.0f);
  g_fonts.display = load("Poppins-Bold.ttf", 17.0f);
  if (g_fonts.body != nullptr) ImGui::GetIO().FontDefault = g_fonts.body;
  return g_fonts;
}

void ApplyStyle() {
  ImGuiStyle &s = ImGui::GetStyle();
  s.WindowRounding = 18;
  s.ChildRounding = 14;
  s.FrameRounding = 11;
  s.PopupRounding = 16;
  s.GrabRounding = 10;
  s.ScrollbarRounding = 8;
  s.WindowBorderSize = 0;
  s.ChildBorderSize = 0;
  s.FrameBorderSize = 1;
  s.PopupBorderSize = 0;
  s.WindowPadding = ImVec2(0, 0);
  s.FramePadding = ImVec2(14, 9);
  s.ItemSpacing = ImVec2(10, 10);
  s.ScrollbarSize = 6;

  ImVec4 *c = s.Colors;
  c[ImGuiCol_Text] = ImColor(234, 240, 255);
  c[ImGuiCol_TextDisabled] = ImColor(102, 116, 158);
  c[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_PopupBg] = ImColor(14, 24, 58, 245);
  c[ImGuiCol_Border] = ImColor(255, 255, 255, 36);
  c[ImGuiCol_FrameBg] = ImColor(4, 9, 26, 120);
  c[ImGuiCol_FrameBgHovered] = ImColor(255, 255, 255, 20);
  c[ImGuiCol_FrameBgActive] = ImColor(74, 120, 255, 40);
  c[ImGuiCol_ScrollbarBg] = ImVec4(0, 0, 0, 0);
  c[ImGuiCol_ScrollbarGrab] = ImColor(255, 255, 255, 70);
  c[ImGuiCol_ScrollbarGrabHovered] = ImColor(255, 255, 255, 110);
  c[ImGuiCol_ScrollbarGrabActive] = ImColor(255, 255, 255, 140);
  c[ImGuiCol_Header] = ImColor(74, 120, 255, 70);
  c[ImGuiCol_HeaderHovered] = ImColor(74, 120, 255, 100);
  c[ImGuiCol_HeaderActive] = ImColor(74, 120, 255, 130);
  c[ImGuiCol_Button] = ImColor(255, 255, 255, 20);
  c[ImGuiCol_ButtonHovered] = ImColor(255, 255, 255, 36);
  c[ImGuiCol_ButtonActive] = ImColor(255, 255, 255, 50);
  c[ImGuiCol_TextSelectedBg] = ImColor(74, 120, 255, 110);
}

} // namespace pl::menu::theme
