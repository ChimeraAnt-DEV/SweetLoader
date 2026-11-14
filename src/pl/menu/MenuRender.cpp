/**
 * @file MenuRender.cpp
 * @brief The in-game Mod Menu layout (see MenuRender.hpp).
 *
 * <p>Screen-level port of the UI pack's `imgui/src/CaramelMenu.cpp`. The pack's panel geometry,
 * header, tab bar, card grid and HUD-layout editor are kept; the "Example" module is replaced by the
 * real module catalogue and the settings panel is driven by the module's own config rows. Where the
 * pack draws a stand-in HUD ("144 FPS", "X: 128  Y: 64") this draws nothing, because the launcher
 * already has HUD elements and inventing numbers here would be the same dishonesty the FPS readout
 * was fixed for.
 */

#include "pl/menu/MenuRender.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui.h"

namespace pl::menu {
namespace {

using ui::Icon;
using ui::Tone;

const ImVec2 kSize(1040, 580);

ImDrawList *DL() { return ImGui::GetWindowDrawList(); }
void At(ImVec2 o, float x, float y) { ImGui::SetCursorScreenPos({o.x + x, o.y + y}); }

void Txt(ImVec2 o, float x, float y, const char *t, bool strong = true) {
  ui::TextAt({o.x + x, o.y + y}, strong ? theme::GetFonts().bodyMedium : theme::GetFonts().body,
             strong ? theme::colour::Text() : theme::colour::Text2(), t);
}
void Title(ImVec2 o, float x, float y, const char *t) {
  ui::TextAt({o.x + x, o.y + y}, theme::GetFonts().title, theme::colour::Text(), t);
}
void Rule(ImVec2 a, float x0, float x1, float y) { ui::Hairline(a.x + x0, a.x + x1, a.y + y); }

const ImU32 kSwatches[] = {IM_COL32(255, 255, 255, 255), theme::colour::Caramel(),
                           theme::colour::Blue(),       theme::colour::Green(),
                           theme::colour::Red(),        IM_COL32(150, 110, 255, 255)};

/// A stable ImGui id from a module key + a suffix, so two modules' widgets never collide.
std::string Keyed(const std::string &stableKey, const char *suffix) {
  return stableKey + "##" + suffix;
}

// ---- header -----------------------------------------------------------------------------------
void Header(ImVec2 o, MenuState &s, const MenuActions &actions) {
  float x = o.x + 18;
  for (const char *c = "CARAMEL CLIENT"; *c != '\0'; ++c) {
    const char ch[2] = {*c, '\0'};
    ui::TextAt({x, o.y + 31}, theme::GetFonts().display, theme::colour::Text(), ch);
    ImGui::PushFont(theme::GetFonts().display);
    x += ImGui::CalcTextSize(ch).x + 1.6f;
    ImGui::PopFont();
  }
  static const char *labels[] = {"MODS", "REPLAY", "COSMETICS", "VC", "SETTINGS"};
  static const Icon icons[] = {Icon::Puzzle, Icon::Replay, Icon::Cube, Icon::Mic, Icon::Gear};
  At(o, kSize.x * 0.5f - 280 + 40, 14);
  int tab = static_cast<int>(s.tab);
  if (ui::TabBar("tabs", labels, icons, 5, &tab, 112)) s.tab = static_cast<MenuTab>(tab);
  At(o, kSize.x - 52, 16);
  static const Icon closeIcon = Icon::Close;
  if (ui::GlassButton("close", nullptr, {34, 32}, false, false, &closeIcon) && actions.onClose)
    actions.onClose();
  Rule(o, 18, kSize.x - 18, 62);
}

// ---- Mods -------------------------------------------------------------------------------------
void ModuleSettingsPanel(ImVec2 o, ImVec2 display, MenuState &s, const MenuActions &actions) {
  DL()->AddRectFilled({0, 0}, display, IM_COL32(3, 7, 20, 150));
  const ImVec2 a{o.x + (kSize.x - 520) * 0.5f, o.y - 20};
  const ImVec2 b{a.x + 520, a.y + 620};
  ui::Glass(a, b, 20, true, IM_COL32(120, 150, 255, 18));
  auto at = [&](float x, float y) { ImGui::SetCursorScreenPos({a.x + x, a.y + y}); };

  if (s.settingsModule < 0 || s.settingsModule >= static_cast<int>(s.modules.size())) {
    s.moduleSettingsOpen = false;
    return;
  }
  MenuModuleRow &m = s.modules[static_cast<std::size_t>(s.settingsModule)];
  const bool unavailable = !m.unavailableReason.empty();

  static const Icon leftIcon = Icon::Left;
  at(18, 16);
  if (ui::GlassButton("back", nullptr, {34, 34}, false, false, &leftIcon)) s.moduleSettingsOpen = false;
  ui::TextAt({a.x + 68, a.y + 33}, theme::GetFonts().title, theme::colour::Text(), m.name.c_str());
  at(520 - 66, 22);
  bool enabled = m.enabled;
  if (ui::Toggle("mod_on_settings", &enabled) && !unavailable) {
    m.enabled = enabled;
    if (actions.onToggleModule) actions.onToggleModule(m.stableKey, enabled);
  }
  Rule(a, 18, 520 - 18, 64);

  float y = 90;
  if (unavailable) {
    at(28, y);
    ui::StatusLabel(m.unavailableReason.c_str(), theme::colour::Orange());
    y += 34;
  }
  if (!m.description.empty()) {
    ui::TextAt({a.x + 28, a.y + y}, theme::GetFonts().body, theme::colour::Text2(),
               m.description.c_str());
    y += 30;
  }

  ui::TextAt({a.x + 28, a.y + y}, theme::GetFonts().bodyMedium, theme::colour::Text2(), "Options");
  y += 36;

  if (m.configs.empty()) {
    ui::TextAt({a.x + 28, a.y + y}, theme::GetFonts().body, theme::colour::Text3(),
               "This module has no options.");
  }

  ImGui::PushItemWidth(464);
  for (std::size_t i = 0; i < m.configs.size(); ++i) {
    MenuConfigRow &cfg = m.configs[i];
    if (y > 520) break; // keep the panel from overflowing; the pack's fixed layout has no scroll
    ui::TextAt({a.x + 28, a.y + y}, theme::GetFonts().bodyMedium, theme::colour::Text(),
               cfg.label.c_str());
    at(28, y + 18);
    const std::string id = Keyed(cfg.key, "cfg");
    if (cfg.kind == MenuConfigKind::Toggle) {
      bool v = cfg.floatValue > 0.5f;
      if (ui::Toggle(id.c_str(), &v) && actions.onSetConfig) {
        cfg.floatValue = v ? 1.0f : 0.0f;
        actions.onSetConfig(m.stableKey, cfg.key, v ? "true" : "false");
      }
    } else if (cfg.kind == MenuConfigKind::Slider) {
      float v = cfg.floatValue;
      if (ui::Slider(id.c_str(), &v, cfg.floatMin, cfg.floatMax) && actions.onSetConfig) {
        cfg.floatValue = v;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3f", v);
        actions.onSetConfig(m.stableKey, cfg.key, buf);
      }
    } else {
      // A colour, a keybind or a text field has no in-game editor yet. It is shown with its value
      // rather than dropped, so the player is not told a module has no options it does have.
      ui::TextAt({a.x + 28, a.y + y + 18}, theme::GetFonts().body, theme::colour::Text3(),
                 cfg.value.empty() ? "Edit in the launcher" : cfg.value.c_str());
    }
    y += 58;
  }
  ImGui::PopItemWidth();

  Rule(a, 18, 520 - 18, b.y - 70);
  static const Icon swapIcon = Icon::Swap;
  at(24, 620 - 54);
  if (ui::GlassButton("reset", "Reset defaults", {160, 36}, false, false, &swapIcon) &&
      actions.onSetConfig) {
    for (const MenuConfigRow &cfg : m.configs) actions.onSetConfig(m.stableKey, cfg.key, cfg.value);
  }
  at(520 - 164, 620 - 54);
  if (ui::PrimaryButton("done", "Done", {140, 36}, Tone::Green)) s.moduleSettingsOpen = false;
}

void ModsTab(ImVec2 o, MenuState &s, const MenuActions &actions) {
  static const char *profiles[] = {"Default", "PvP", "Survival"};
  Txt(o, 18, 86, "Profiles", false);
  for (int i = 0; i < 3; ++i) {
    const ImVec2 a{o.x + 18, o.y + 104 + i * 42};
    At(o, 18, 104 + i * 42);
    char id[16];
    std::snprintf(id, sizeof id, "prof%d", i);
    if (ui::GlassButton(id, nullptr, {200, 36}, false, s.profile == i)) s.profile = i;
    ui::TextAt({a.x + 14, a.y + 18}, theme::GetFonts().bodyMedium,
               s.profile == i ? theme::colour::Text() : theme::colour::Text2(), profiles[i]);
    ui::DrawIcon(Icon::Dots, {a.x + 180, a.y + 18}, 14, theme::colour::Text3());
  }
  DL()->AddLine({o.x + 232, o.y + 78}, {o.x + 232, o.y + kSize.y - 24},
                IM_COL32(255, 255, 255, 20));

  static const char *cats[] = {"All", "New", "HUD", "Server", "Mechanic"};
  At(o, 256, 78);
  for (int i = 0; i < 5; ++i) {
    if (ui::Chip(cats[i], s.category == i)) s.category = i;
    ImGui::SameLine(0, 6);
  }
  At(o, kSize.x - 18 - 230, 74);
  ui::SearchField("##search", s.search, sizeof s.search, 230, "Search modules");
  At(o, kSize.x - 18 - 230 - 92, 74);
  if (ui::GlassButton("favonly", "Favs", {80, 36}, false, s.favoritesOnly)) {
    s.favoritesOnly = !s.favoritesOnly;
  }

  const std::vector<int> rows =
      VisibleModuleIndices(s.modules, MenuFilter{std::string(s.search), s.favoritesOnly});
  if (rows.empty()) {
    const char *msg = s.modules.empty()
                          ? (s.emptyReason.empty() ? "No modules available" : s.emptyReason.c_str())
                          : "No modules match the search";
    ui::TextAt({o.x + 256 + 214 * 0.5f, o.y + 300}, theme::GetFonts().title,
               theme::colour::Text2(), msg, true);
    return;
  }

  // A two-column card grid, the pack's card geometry (214x158, 12px gutter).
  const float cardW = 214, cardH = 158, gutter = 12;
  const float originX = o.x + 256, originY = o.y + 130;
  for (std::size_t n = 0; n < rows.size(); ++n) {
    const int col = static_cast<int>(n) % 2;
    const int line = static_cast<int>(n) / 2;
    const ImVec2 a{originX + col * (cardW + gutter), originY + line * (cardH + gutter)};
    const ImVec2 b{a.x + cardW, a.y + cardH};
    // Scissor so a long catalogue scrolls by clipping at the panel edge rather than overflowing.
    if (a.y > o.y + kSize.y - 20) break;
    MenuModuleRow &m = s.modules[static_cast<std::size_t>(rows[n])];
    const bool unavailable = !m.unavailableReason.empty();

    ui::Glass(a, b, 16, true);
    const ImVec2 ic{(a.x + b.x) * 0.5f, a.y + 42};
    DL()->AddRectFilled({ic.x - 26, ic.y - 26}, {ic.x + 26, ic.y + 26}, IM_COL32(74, 120, 255, 46),
                        14);
    DL()->AddRect({ic.x - 26, ic.y - 26}, {ic.x + 26, ic.y + 26}, IM_COL32(74, 120, 255, 140), 14);
    ui::DrawIcon(Icon::Puzzle, ic, 26, IM_COL32(140, 175, 255, 255));

    ui::TextAt({ic.x, a.y + 88}, theme::GetFonts().title, theme::colour::Text(), m.name.c_str(), true);
    if (unavailable) {
      ui::TextAt({ic.x, a.y + 106}, theme::GetFonts().body, theme::colour::Orange(), "Unavailable",
                 true);
    }

    char openId[64], favId[64], toggleId[64];
    std::snprintf(openId, sizeof openId, "open_%d", rows[n]);
    std::snprintf(favId, sizeof favId, "fav_%d", rows[n]);
    std::snprintf(toggleId, sizeof toggleId, "mod_on_%d", rows[n]);

    static const Icon slidersIcon = Icon::Sliders;
    ImGui::SetCursorScreenPos({a.x + 14, a.y + 110});
    if (ui::GlassButton(openId, "OPTIONS", {cardW - 28 - 52, 32}, false, false, &slidersIcon)) {
      s.settingsModule = rows[n];
      s.moduleSettingsOpen = true;
    }
    ImGui::SetCursorScreenPos({b.x - 14 - 40, a.y + 115});
    bool enabled = m.enabled;
    if (ui::Toggle(toggleId, &enabled) && !unavailable) {
      m.enabled = enabled;
      if (actions.onToggleModule) actions.onToggleModule(m.stableKey, enabled);
    }
    static const Icon starIcon = Icon::Star;
    ImGui::SetCursorScreenPos({b.x - 30, a.y + 6});
    if (ui::GlassButton(favId, nullptr, {24, 24}, false, m.favorite, &starIcon)) {
      if (actions.onToggleFavorite) actions.onToggleFavorite(m.stableKey);
    }
    if (unavailable) DL()->AddRectFilled(a, b, IM_COL32(4, 9, 28, 90), 16);
  }

  if (s.moduleSettingsOpen) ModuleSettingsPanel(o, ImVec2(kSize), s, actions);
}

// ---- Replay -----------------------------------------------------------------------------------
void ReplayTab(ImVec2 o, MenuState &s) {
  const ImVec2 l{o.x + 28, o.y + 84};
  const float lw = 600;
  ui::Glass(l, {l.x + lw, o.y + kSize.y - 28}, 16, false);
  const ImVec2 r{l.x + lw + 24, l.y};
  ui::Glass(r, {o.x + kSize.x - 28, o.y + kSize.y - 28}, 16, false);
  Title(l, 22, 30, "Recordings");
  ImGui::SetCursorScreenPos({l.x + lw - 178, l.y + 14});
  ui::PrimaryButton("rec", "Start recording", {160, 32}, Tone::Red);
  ui::TextAt({l.x + 22, l.y + 120}, theme::GetFonts().body, theme::colour::Text2(),
             "Recordings appear here once capture is wired to the engine.");
  const float rw = o.x + kSize.x - 28 - r.x - 44;
  Title(r, 22, 30, "Recording");
  ui::TextAt({r.x + 22, r.y + 76}, theme::GetFonts().body, theme::colour::Text(),
             "Record automatically");
  ImGui::SetCursorScreenPos({r.x + 22 + rw - 40, r.y + 65});
  ui::Toggle("auto", &s.autoRecord);
  ui::TextAt({r.x + 22, r.y + 126}, theme::GetFonts().body, theme::colour::Text(), "Buffer length");
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 142});
  ImGui::PushItemWidth(rw);
  ui::Slider("buf", &s.buffer, 0, 1);
  ImGui::PopItemWidth();
  ui::TextAt({r.x + 22, r.y + 204}, theme::GetFonts().body, theme::colour::Text(), "Quality");
  ImGui::SetCursorScreenPos({r.x + 22 + rw - 150, r.y + 186});
  static const char *q[] = {"Low", "High", "Ultra"};
  ui::Dropdown("q", q, 3, &s.quality, 150);
  ui::TextAt({r.x + 22, r.y + 254}, theme::GetFonts().body, theme::colour::Text(), "Hotkey");
  ImGui::SetCursorScreenPos({r.x + 22 + rw - 150, r.y + 236});
  ImGui::SetNextItemWidth(150);
  ImGui::InputText("##rk", s.replayKey, sizeof s.replayKey);
}

// ---- Cosmetics --------------------------------------------------------------------------------
void StudioTab(ImVec2 o, MenuState &s) {
  const ImVec2 l{o.x + 28, o.y + 84};
  ui::Glass(l, {l.x + 300, o.y + kSize.y - 28}, 16, false);
  DL()->AddRectFilled({l.x + 22, l.y + 22}, {l.x + 278, l.y + 316}, IM_COL32(255, 255, 255, 240),
                      20.f);
  const char *msg = "The 3D skin preview renders here";
  ImGui::PushFont(theme::GetFonts().title);
  const ImVec2 ts = ImGui::CalcTextSize(msg, nullptr, false, 180);
  ImGui::PopFont();
  DL()->AddText(theme::GetFonts().title, theme::GetFonts().title->FontSize,
                {l.x + 150 - ts.x * 0.5f, l.y + 169 - ts.y * 0.5f}, IM_COL32(28, 40, 84, 255), msg,
                nullptr, 180);
  Txt(l, 22, 348, "Rotation");
  ImGui::SetCursorScreenPos({l.x + 22, l.y + 364});
  ImGui::PushItemWidth(256);
  ui::Slider("rot", &s.range, 0, 1);
  ImGui::PopItemWidth();
  static const Icon swapIcon = Icon::Swap;
  ImGui::SetCursorScreenPos({l.x + 22, l.y + 400});
  ui::GlassButton("rview", "Reset view", {256, 36}, false, false, &swapIcon);

  const float gx = l.x + 324;
  ImGui::SetCursorScreenPos({gx, l.y});
  static const char *cats[] = {"All", "Capes", "Wings", "Hats"};
  for (int i = 0; i < 4; ++i) {
    if (ui::Chip(cats[i], s.category == i)) s.category = i;
    ImGui::SameLine(0, 6);
  }
  ImVec2 e{gx, l.y + 372};
  ui::Glass(e, {o.x + kSize.x - 28, o.y + kSize.y - 28}, 16, false);
  ui::TextAt({e.x + 18, e.y + 24}, theme::GetFonts().bodyMedium, theme::colour::Text(),
             "Equip cosmetics from the launcher; the studio preview is launcher-side.");
  ImGui::SetCursorScreenPos({o.x + kSize.x - 28 - 164, e.y + 12});
  ui::PrimaryButton("equip", "Open launcher", {136, 34}, Tone::Green);
}

// ---- Voice ------------------------------------------------------------------------------------
void VoiceTab(ImVec2 o, MenuState &s) {
  const float cw = (kSize.x - 84) * 0.5f;
  const ImVec2 l{o.x + 28, o.y + 84}, r{l.x + cw + 28, l.y};
  ui::Glass(l, {l.x + cw, o.y + kSize.y - 28}, 16, false);
  ui::Glass(r, {r.x + cw, o.y + kSize.y - 28}, 16, false);
  const float iw = cw - 44;
  Title(l, 22, 30, "Devices");
  static const char *in[] = {"Default microphone", "USB microphone"};
  static const char *out[] = {"Default speakers", "Headphones"};
  ui::TextAt({l.x + 22, l.y + 76}, theme::GetFonts().body, theme::colour::Text(), "Input device");
  ImGui::SetCursorScreenPos({l.x + 22 + iw - 230, l.y + 58});
  ui::Dropdown("in", in, 2, &s.inDevice, 230);
  ui::TextAt({l.x + 22, l.y + 126}, theme::GetFonts().body, theme::colour::Text(), "Output device");
  ImGui::SetCursorScreenPos({l.x + 22 + iw - 230, l.y + 108});
  ui::Dropdown("out", out, 2, &s.outDevice, 230);
  ui::TextAt({l.x + 22, l.y + 184}, theme::GetFonts().body, theme::colour::Text(),
             "Microphone volume");
  ImGui::SetCursorScreenPos({l.x + 22, l.y + 200});
  ImGui::PushItemWidth(iw);
  ui::Slider("mic", &s.micVol, 0, 1);
  ui::TextAt({l.x + 22, l.y + 256}, theme::GetFonts().body, theme::colour::Text(), "Output volume");
  ImGui::SetCursorScreenPos({l.x + 22, l.y + 272});
  ui::Slider("vol", &s.outVol, 0, 1);
  ImGui::PopItemWidth();
  Title(r, 22, 30, "Voice chat");
  struct Row {
    const char *t;
    const char *d;
    bool *v;
  } rows[] = {{"Push to talk", "Hold a key to speak", &s.ptt},
              {"Noise suppression", "Filter background sound", &s.noiseSuppression},
              {"Proximity chat", "Hear players near you", &s.proximity}};
  for (int i = 0; i < 3; ++i) {
    ui::TextAt({r.x + 22, r.y + 76 + i * 52}, theme::GetFonts().body, theme::colour::Text(),
               rows[i].t);
    ui::TextAt({r.x + 22, r.y + 94 + i * 52}, theme::GetFonts().body, theme::colour::Text2(),
               rows[i].d);
    ImGui::SetCursorScreenPos({r.x + 22 + iw - 40, r.y + 65 + i * 52});
    char id[8];
    std::snprintf(id, sizeof id, "v%d", i);
    ui::Toggle(id, rows[i].v);
  }
  ui::TextAt({r.x + 22, r.y + 246}, theme::GetFonts().body, theme::colour::Text(),
             "Push to talk key");
  ImGui::SetCursorScreenPos({r.x + 22 + iw - 150, r.y + 228});
  ImGui::SetNextItemWidth(150);
  ImGui::InputText("##ptk", s.pttKey, sizeof s.pttKey);
  ui::TextAt({r.x + 22, r.y + 296}, theme::GetFonts().body, theme::colour::Text(),
             "Proximity range");
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 312});
  ImGui::PushItemWidth(iw);
  ui::Slider("range", &s.range, 0, 1);
  ImGui::PopItemWidth();
  static const Icon micIcon = Icon::Mic, closeIcon = Icon::Close;
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 366});
  ui::GlassButton("mute", "Mute", {150, 36}, false, false, &micIcon);
  ImGui::SetCursorScreenPos({r.x + 182, r.y + 366});
  ui::GlassButton("deaf", "Deafen", {150, 36}, true, false, &closeIcon);
}

// ---- Settings ---------------------------------------------------------------------------------
void SettingsTab(ImVec2 o, MenuState &s) {
  const float cw = (kSize.x - 84) * 0.5f;
  const ImVec2 l{o.x + 28, o.y + 84}, r{l.x + cw + 28, l.y};
  ui::Glass(l, {l.x + cw, o.y + kSize.y - 28}, 16, false);
  ui::Glass(r, {r.x + cw, o.y + kSize.y - 28}, 16, false);
  Title(l, 22, 30, "General");
  struct Row {
    const char *t;
    bool *v;
  } rows[] = {{"Auto-join last server", &s.autoJoin},
              {"Show module toggles in chat", &s.chatToggles},
              {"Notifications", &s.notifications},
              {"Smooth camera", &s.smoothCamera}};
  for (int i = 0; i < 4; ++i) {
    ui::TextAt({l.x + 22, l.y + 76 + i * 46}, theme::GetFonts().body, theme::colour::Text(),
               rows[i].t);
    ImGui::SetCursorScreenPos({l.x + cw - 62, l.y + 65 + i * 46});
    char id[16];
    std::snprintf(id, sizeof id, "g%d", i);
    ui::Toggle(id, rows[i].v);
  }
  ui::TextAt({l.x + 22, l.y + 276}, theme::GetFonts().body, theme::colour::Text(), "Menu scale");
  ImGui::SetCursorScreenPos({l.x + 22, l.y + 293});
  ImGui::PushItemWidth(cw - 44);
  ui::Slider("ms", &s.menuScale, 0, 1);
  ImGui::PopItemWidth();
  Title(r, 22, 30, "Appearance");
  ui::TextAt({r.x + 22, r.y + 76}, theme::GetFonts().body, theme::colour::Text(), "Accent color");
  ImGui::SetCursorScreenPos({r.x + cw - 22 - 160, r.y + 64});
  ui::ColorSwatches("ac", kSwatches + 1, 5, &s.accent);
  ui::TextAt({r.x + 22, r.y + 122}, theme::GetFonts().body, theme::colour::Text(), "Blur strength");
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 140});
  ImGui::PushItemWidth(cw - 44);
  ui::Slider("bl", &s.blur, 0, 1);
  ImGui::PopItemWidth();
  ui::TextAt({r.x + 22, r.y + 192}, theme::GetFonts().body, theme::colour::Text(), "Language");
  ImGui::SetCursorScreenPos({r.x + cw - 22 - 190, r.y + 174});
  static const char *langs[] = {"English (US)", "Espanol", "Francais", "Deutsch"};
  ui::Dropdown("lang", langs, 4, &s.language, 190);
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 250});
  ui::CheckBox("rc", "Rounded corners", &s.rounded);
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 290});
  ui::CheckBox("an", "Animations", &s.animations);
  ImGui::SetCursorScreenPos({r.x + 22, r.y + 340});
  ui::GlassButton("rst", "Reset client", {160, 36}, true);
}

// ---- HUD-layout editor ------------------------------------------------------------------------
void RenderHudEditor(MenuState &s, ImVec2 display) {
  ImDrawList *d = DL();
  d->AddRectFilled({0, 0}, display, IM_COL32(4, 9, 28, 90));
  if (s.showGrid) {
    for (float x = 0; x < display.x; x += 40)
      d->AddLine({x, 0}, {x, display.y}, IM_COL32(255, 255, 255, 14));
    for (float y = 0; y < display.y; y += 40)
      d->AddLine({0, y}, {display.x, y}, IM_COL32(255, 255, 255, 14));
  }
  const float cx = display.x * 0.5f;
  const ImVec2 a{cx - 330, 20};
  ui::Glass(a, {a.x + 660, a.y + 64}, 20, true);
  ui::TextAt({a.x + 54, a.y + 32}, theme::GetFonts().title, theme::colour::Text(),
             "Edit HUD layout");
  ImGui::SetCursorScreenPos({a.x + 190, a.y + 23});
  ui::CheckBox("snap", "Snap to grid", &s.snap);
  ImGui::SetCursorScreenPos({a.x + 310, a.y + 23});
  ui::CheckBox("grid", "Show grid", &s.showGrid);
  ImGui::SetCursorScreenPos({a.x + 508, a.y + 14});
  if (ui::GlassButton("hcancel", "Cancel", {70, 36})) s.hudEditing = false;
  ImGui::SetCursorScreenPos({a.x + 586, a.y + 14});
  if (ui::PrimaryButton("hsave", "Save", {64, 36}, Tone::Green)) s.hudEditing = false;
}

} // namespace

void RenderMenu(MenuState &s, const MenuActions &actions, ImVec2 display) {
  if (!s.open) return;
  ImGui::SetNextWindowPos({0, 0});
  ImGui::SetNextWindowSize(display);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
  ImGui::Begin("##caramel", nullptr,
               ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground |
                   ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                   ImGuiWindowFlags_NoBringToFrontOnFocus);
  if (s.hudEditing) {
    RenderHudEditor(s, display);
    ImGui::End();
    ImGui::PopStyleVar();
    return;
  }
  DL()->AddRectFilled({0, 0}, display, IM_COL32(4, 9, 28, 120)); // game dim
  const ImVec2 o{(display.x - kSize.x) * 0.5f, (display.y - kSize.y) * 0.5f};
  ui::Glass(o, {o.x + kSize.x, o.y + kSize.y}, 18, true, IM_COL32(120, 150, 255, 14));
  Header(o, s, actions);
  switch (s.tab) {
  case MenuTab::Mods: ModsTab(o, s, actions); break;
  case MenuTab::Replay: ReplayTab(o, s); break;
  case MenuTab::Cosmetics: StudioTab(o, s); break;
  case MenuTab::Voice: VoiceTab(o, s); break;
  case MenuTab::Settings: SettingsTab(o, s); break;
  }
  ImGui::End();
  ImGui::PopStyleVar();
}

} // namespace pl::menu
