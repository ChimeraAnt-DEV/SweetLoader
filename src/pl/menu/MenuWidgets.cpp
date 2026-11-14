/**
 * @file MenuWidgets.cpp
 * @brief The in-game Mod Menu's glass widget kit (see MenuWidgets.hpp).
 *
 * <p>Line-for-line ported from the UI pack's `imgui/src/CaramelWidgets.cpp`, with the colour tokens
 * pulled from {@link pl::menu::theme} and the desktop backdrop-blur hook removed (see the header).
 */

#include "pl/menu/MenuWidgets.hpp"

#include <cmath>
#include <cstdio>
#include <cstring>

#include "imgui_internal.h"

namespace pl::menu::ui {
namespace {

ImU32 Mul(ImU32 c, float a) {
  ImVec4 v = ImGui::ColorConvertU32ToFloat4(c);
  v.w *= a;
  return ImGui::ColorConvertFloat4ToU32(v);
}

ImDrawList *DL() { return ImGui::GetWindowDrawList(); }

ImFont *FontOr(ImFont *font) { return font != nullptr ? font : ImGui::GetFont(); }

/// Registers an invisible hit target at {@code pos} and reports hover/active for the caller's ease.
bool Hit(const char *id, ImVec2 pos, ImVec2 size, bool *hovered, bool *held) {
  ImGui::SetCursorScreenPos(pos);
  const bool pressed = ImGui::InvisibleButton(id, size);
  *hovered = ImGui::IsItemHovered();
  *held = ImGui::IsItemActive();
  return pressed;
}

void GradientRounded(ImVec2 a, ImVec2 b, float r, ImU32 top, ImU32 bottom) {
  ImDrawList *d = DL();
  d->AddRectFilled(a, b, bottom, r);
  d->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.55f), top, r, ImDrawFlags_RoundCornersTop);
  d->AddRectFilledMultiColor(ImVec2(a.x, a.y + r), ImVec2(b.x, b.y - r), top, top, bottom, bottom);
}

} // namespace

float Anim(ImGuiID id, float target, float speed) {
  float *v = ImGui::GetStateStorage()->GetFloatRef(id, target);
  *v += (target - *v) * (1.0f - std::exp(-speed * ImGui::GetIO().DeltaTime));
  return *v;
}

void TextAt(ImVec2 p, ImFont *font, ImU32 c, const char *t, bool cx, bool cy) {
  ImGui::PushFont(FontOr(font));
  const ImVec2 s = ImGui::CalcTextSize(t);
  if (cx) p.x -= s.x * 0.5f;
  if (cy) p.y -= s.y * 0.5f;
  DL()->AddText(p, c, t);
  ImGui::PopFont();
}

void Glass(ImVec2 a, ImVec2 b, float r, bool shadow, ImU32 tint) {
  ImDrawList *d = DL();
  if (shadow) {
    for (int i = 0; i < 7; ++i) {
      const float e = 3.0f + i * 4.0f;
      d->AddRectFilled(ImVec2(a.x - e, a.y - e + 12), ImVec2(b.x + e, b.y + e + 12),
                       IM_COL32(0, 0, 8, 16), r + e);
    }
  }
  d->AddRectFilled(a, b, IM_COL32(12, 22, 56, 238), r); // frosted navy base
  d->AddRectFilled(a, b, tint, r);                      // tint
  d->AddRectFilled(a, ImVec2(b.x, a.y + (b.y - a.y) * 0.42f), IM_COL32(255, 255, 255, 14), r,
                   ImDrawFlags_RoundCornersTop); // sheen
  d->AddRect(a, b, IM_COL32(255, 255, 255, 40), r, 0, 1.0f); // hairline border
  d->AddLine(ImVec2(a.x + r, a.y + 0.5f), ImVec2(b.x - r, a.y + 0.5f),
             IM_COL32(255, 255, 255, 70)); // top light
}

void Hairline(float x0, float x1, float y) {
  DL()->AddLine(ImVec2(x0, y), ImVec2(x1, y), IM_COL32(255, 255, 255, 24));
}

void DrawIcon(Icon ic, ImVec2 c, float s, ImU32 col, float t) {
  ImDrawList *d = DL();
  const float h = s * 0.5f;
  switch (ic) {
  case Icon::Close:
    d->AddLine({c.x - h * 0.55f, c.y - h * 0.55f}, {c.x + h * 0.55f, c.y + h * 0.55f}, col, t);
    d->AddLine({c.x + h * 0.55f, c.y - h * 0.55f}, {c.x - h * 0.55f, c.y + h * 0.55f}, col, t);
    break;
  case Icon::Check:
    d->PathLineTo({c.x - h * 0.5f, c.y});
    d->PathLineTo({c.x - h * 0.1f, c.y + h * 0.4f});
    d->PathLineTo({c.x + h * 0.55f, c.y - h * 0.4f});
    d->PathStroke(col, 0, t);
    break;
  case Icon::Search:
    d->AddCircle({c.x - h * 0.15f, c.y - h * 0.15f}, h * 0.5f, col, 0, t);
    d->AddLine({c.x + h * 0.22f, c.y + h * 0.22f}, {c.x + h * 0.6f, c.y + h * 0.6f}, col, t);
    break;
  case Icon::Down:
    d->PathLineTo({c.x - h * 0.5f, c.y - h * 0.22f});
    d->PathLineTo({c.x, c.y + h * 0.28f});
    d->PathLineTo({c.x + h * 0.5f, c.y - h * 0.22f});
    d->PathStroke(col, 0, t);
    break;
  case Icon::Up:
    d->PathLineTo({c.x - h * 0.5f, c.y + h * 0.22f});
    d->PathLineTo({c.x, c.y - h * 0.28f});
    d->PathLineTo({c.x + h * 0.5f, c.y + h * 0.22f});
    d->PathStroke(col, 0, t);
    break;
  case Icon::Left:
    d->PathLineTo({c.x + h * 0.22f, c.y - h * 0.5f});
    d->PathLineTo({c.x - h * 0.28f, c.y});
    d->PathLineTo({c.x + h * 0.22f, c.y + h * 0.5f});
    d->PathStroke(col, 0, t);
    break;
  case Icon::Plus:
    d->AddLine({c.x - h * 0.55f, c.y}, {c.x + h * 0.55f, c.y}, col, t);
    d->AddLine({c.x, c.y - h * 0.55f}, {c.x, c.y + h * 0.55f}, col, t);
    break;
  case Icon::Sliders:
    d->AddLine({c.x - h * 0.7f, c.y - h * 0.3f}, {c.x + h * 0.7f, c.y - h * 0.3f}, col, t);
    d->AddLine({c.x - h * 0.7f, c.y + h * 0.3f}, {c.x + h * 0.7f, c.y + h * 0.3f}, col, t);
    d->AddCircleFilled({c.x - h * 0.25f, c.y - h * 0.3f}, h * 0.2f, col);
    d->AddCircleFilled({c.x + h * 0.25f, c.y + h * 0.3f}, h * 0.2f, col);
    break;
  case Icon::Hud:
    d->AddRect({c.x - h * 0.75f, c.y - h * 0.5f}, {c.x + h * 0.75f, c.y + h * 0.5f}, col, 3.0f, 0, t);
    d->AddRectFilled({c.x - h * 0.5f, c.y - h * 0.25f}, {c.x, c.y}, col, 1.0f);
    break;
  case Icon::Swap:
    d->AddLine({c.x - h * 0.7f, c.y - h * 0.25f}, {c.x + h * 0.7f, c.y - h * 0.25f}, col, t);
    d->AddLine({c.x - h * 0.7f, c.y + h * 0.25f}, {c.x + h * 0.7f, c.y + h * 0.25f}, col, t);
    break;
  case Icon::Dots:
    for (int i = -1; i <= 1; ++i) d->AddCircleFilled({c.x + i * h * 0.5f, c.y}, h * 0.12f, col);
    break;
  case Icon::Puzzle:
    d->AddRect({c.x - h * 0.6f, c.y - h * 0.35f}, {c.x + h * 0.6f, c.y + h * 0.7f}, col, 3.0f, 0, t);
    d->AddCircle({c.x, c.y - h * 0.5f}, h * 0.26f, col, 0, t);
    d->AddCircle({c.x + h * 0.62f, c.y + h * 0.15f}, h * 0.24f, col, 0, t);
    break;
  case Icon::Replay:
    d->PathArcTo(c, h * 0.65f, 0.7f, 5.5f);
    d->PathStroke(col, 0, t);
    d->AddTriangleFilled({c.x - h * 0.12f, c.y - h * 0.3f}, {c.x + h * 0.3f, c.y},
                         {c.x - h * 0.12f, c.y + h * 0.3f}, col);
    break;
  case Icon::Cube: {
    ImVec2 p[6] = {{c.x, c.y - h * 0.8f},  {c.x + h * 0.7f, c.y - h * 0.4f},
                   {c.x + h * 0.7f, c.y + h * 0.4f}, {c.x, c.y + h * 0.8f},
                   {c.x - h * 0.7f, c.y + h * 0.4f}, {c.x - h * 0.7f, c.y - h * 0.4f}};
    d->AddPolyline(p, 6, col, ImDrawFlags_Closed, t);
    d->AddLine(p[5], {c.x, c.y}, col, t);
    d->AddLine(p[1], {c.x, c.y}, col, t);
    d->AddLine({c.x, c.y}, p[3], col, t);
    break;
  }
  case Icon::Mic:
    d->AddRect({c.x - h * 0.26f, c.y - h * 0.75f}, {c.x + h * 0.26f, c.y + h * 0.1f}, col,
               h * 0.26f, 0, t);
    d->PathArcTo({c.x, c.y - h * 0.05f}, h * 0.52f, 0.0f, IM_PI, 16);
    d->PathStroke(col, 0, t);
    d->AddLine({c.x, c.y + h * 0.47f}, {c.x, c.y + h * 0.8f}, col, t);
    break;
  case Icon::Gear:
    for (int i = 0; i < 8; ++i) {
      const float a = i * IM_PI / 4;
      d->AddLine({c.x + h * 0.62f * std::cos(a), c.y + h * 0.62f * std::sin(a)},
                 {c.x + h * 0.88f * std::cos(a), c.y + h * 0.88f * std::sin(a)}, col, t + 0.4f);
    }
    d->AddCircle(c, h * 0.5f, col, 0, t);
    d->AddCircle(c, h * 0.2f, col, 0, t);
    break;
  case Icon::Star:
  case Icon::StarFill: {
    ImVec2 p[11];
    for (int i = 0; i < 10; ++i) {
      const float a = -IM_PI / 2 + i * IM_PI / 5;
      const float r = (i % 2 == 0) ? h * 0.95f : h * 0.42f;
      p[i] = {c.x + r * std::cos(a), c.y + h * 0.06f + r * std::sin(a)};
    }
    p[10] = p[0];
    if (ic == Icon::StarFill)
      for (int i = 0; i < 10; ++i) d->AddTriangleFilled(c, p[i], p[i + 1], col);
    d->AddPolyline(p, 11, col, 0, t);
    break;
  }
  case Icon::Play:
    d->AddTriangleFilled({c.x - h * 0.4f, c.y - h * 0.6f}, {c.x + h * 0.6f, c.y},
                         {c.x - h * 0.4f, c.y + h * 0.6f}, col);
    break;
  case Icon::Map:
    d->AddCircle({c.x, c.y - h * 0.15f}, h * 0.45f, col, 0, t);
    d->AddLine({c.x - h * 0.3f, c.y + h * 0.15f}, {c.x, c.y + h * 0.8f}, col, t);
    d->AddLine({c.x + h * 0.3f, c.y + h * 0.15f}, {c.x, c.y + h * 0.8f}, col, t);
    break;
  }
}

bool PrimaryButton(const char *id, const char *label, ImVec2 size, Tone tone) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool hov = false, held = false;
  const bool clicked = Hit(id, p, size, &hov, &held);
  const float h = Anim(ImGui::GetID(id), hov ? 1.0f : 0.0f);
  ImDrawList *d = DL();
  const float r = size.y / 2.6f;
  ImU32 top, bot, on = theme::colour::White(255);
  switch (tone) {
  case Tone::Green: top = theme::colour::GreenLight(); bot = theme::colour::GreenDark();
    on = theme::colour::OnGreen(); break;
  case Tone::Caramel: top = IM_COL32(255, 190, 90, 255); bot = IM_COL32(222, 134, 36, 255); break;
  case Tone::Blue: top = theme::colour::BlueLight(); bot = theme::colour::BlueDark(); break;
  default: top = IM_COL32(255, 108, 126, 255); bot = IM_COL32(214, 52, 78, 255); break;
  }
  for (int i = 0; i < 4; ++i)
    d->AddRectFilled({p.x - i * 2, p.y - i * 2 + 5}, {p.x + size.x + i * 2, p.y + size.y + i * 2 + 5},
                     Mul(bot, (0.05f + 0.03f * h)), r + i * 2);
  GradientRounded(p, {p.x + size.x, p.y + size.y}, r, top, bot);
  d->AddRectFilled({p.x + 2, p.y + 2}, {p.x + size.x - 2, p.y + size.y * 0.5f},
                   theme::colour::White(int(36 + 30 * h)), r - 2, ImDrawFlags_RoundCornersTop);
  d->AddRect(p, {p.x + size.x, p.y + size.y}, theme::colour::White(72), r);
  TextAt({p.x + size.x * 0.5f, p.y + size.y * 0.5f}, theme::GetFonts().title, on, label, true);
  return clicked;
}

bool GlassButton(const char *id, const char *label, ImVec2 size, bool danger, bool active,
                 const Icon *icon) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool hov = false, held = false;
  const bool clicked = Hit(id, p, size, &hov, &held);
  const float h = Anim(ImGui::GetID(id), hov ? 1.0f : 0.0f);
  ImDrawList *d = DL();
  const float r = ImMin(12.0f, size.y / 2.4f);
  const ImU32 t = danger ? theme::colour::Red()
                         : (active ? theme::colour::Blue() : theme::colour::White(255));
  const ImU32 fg = danger ? theme::colour::Red() : theme::colour::Text();
  d->AddRectFilled(p, {p.x + size.x, p.y + size.y},
                   Mul(t, (danger || active) ? 0.16f : 0.07f + 0.06f * h), r);
  d->AddRect(p, {p.x + size.x, p.y + size.y},
             Mul(t, (danger || active) ? 0.5f : 0.17f + 0.12f * h), r);
  const ImVec2 c{p.x + size.x * 0.5f, p.y + size.y * 0.5f};
  if (icon != nullptr && label != nullptr && *label != '\0') {
    const float tw = ImGui::CalcTextSize(label).x;
    DrawIcon(*icon, {c.x - tw * 0.5f - 10, c.y}, 15, fg, 1.7f);
    TextAt({c.x + 9, c.y}, theme::GetFonts().bodyMedium, fg, label, true);
  } else if (icon != nullptr) {
    DrawIcon(*icon, c, 17, fg, 1.8f);
  } else {
    TextAt(c, theme::GetFonts().bodyMedium, fg, label, true);
  }
  return clicked;
}

bool Toggle(const char *id, bool *v) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(40, 22);
  bool hov = false, held = false;
  const bool clicked = Hit(id, p, size, &hov, &held);
  if (clicked) *v = !*v;
  const float t = Anim(ImGui::GetID(id), *v ? 1.0f : 0.0f, 18.0f);
  ImDrawList *d = DL();
  if (t > 0.01f)
    GradientRounded(p, {p.x + 40, p.y + 22}, 11, Mul(theme::colour::GreenLight(), t),
                    Mul(theme::colour::GreenDark(), t));
  d->AddRectFilled(p, {p.x + 40, p.y + 22}, theme::colour::White(int(26 * (1 - t))), 11);
  d->AddRect(p, {p.x + 40, p.y + 22}, theme::colour::White(int(60 * (1 - t))), 11);
  d->AddCircleFilled({p.x + 11 + 18 * t, p.y + 11}, 8.0f,
                     *v ? IM_COL32(255, 255, 255, 255) : IM_COL32(190, 202, 235, 255));
  return clicked;
}

bool CheckBox(const char *id, const char *label, bool *v) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(ImGui::CalcTextSize(label).x + 32, 18);
  bool hov = false, held = false;
  const bool clicked = Hit(id, p, size, &hov, &held);
  if (clicked) *v = !*v;
  ImDrawList *d = DL();
  if (*v) {
    GradientRounded(p, {p.x + 18, p.y + 18}, 5, theme::colour::BlueLight(), theme::colour::BlueDark());
    DrawIcon(Icon::Check, {p.x + 9, p.y + 9}, 13, theme::colour::White(255), 2.2f);
  } else {
    d->AddRectFilled(p, {p.x + 18, p.y + 18}, theme::colour::White(14), 5);
    d->AddRect(p, {p.x + 18, p.y + 18}, theme::colour::White(72), 5);
  }
  TextAt({p.x + 28, p.y + 9}, theme::GetFonts().body, theme::colour::Text(), label);
  return clicked;
}

bool Slider(const char *id, float *v, float lo, float hi) {
  const float w = ImGui::GetContentRegionAvail().x;
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(w, 22);
  bool hov = false, held = false;
  Hit(id, p, size, &hov, &held);
  bool changed = false;
  if (held) {
    const float n = ImClamp((ImGui::GetIO().MousePos.x - p.x) / w, 0.0f, 1.0f);
    const float nv = lo + n * (hi - lo);
    if (nv != *v) {
      *v = nv;
      changed = true;
    }
  }
  const float t = (*v - lo) / (hi - lo);
  const float cy = p.y + 11;
  ImDrawList *d = DL();
  d->AddRectFilled({p.x, cy - 3}, {p.x + w, cy + 3}, theme::colour::White(30), 3.0f);
  d->AddRectFilledMultiColor({p.x, cy - 3}, {p.x + w * t, cy + 3}, theme::colour::BlueLight(),
                             IM_COL32(74, 190, 255, 255), IM_COL32(74, 190, 255, 255),
                             theme::colour::BlueLight());
  d->AddCircleFilled({p.x + w * t, cy + 1.5f}, 9.0f, IM_COL32(0, 0, 0, 80));
  d->AddCircleFilled({p.x + w * t, cy}, 8.0f, IM_COL32(255, 255, 255, 255));
  d->AddCircleFilled({p.x + w * t, cy}, 3.0f, theme::colour::Blue());
  return changed;
}

bool Chip(const char *label, bool active) {
  ImGui::PushFont(FontOr(theme::GetFonts().bodyBold));
  const float tw = ImGui::CalcTextSize(label).x;
  ImGui::PopFont();
  const ImVec2 p = ImGui::GetCursorScreenPos();
  const ImVec2 size(tw + 26, 26);
  bool hov = false, held = false;
  const bool clicked = Hit(label, p, size, &hov, &held);
  ImDrawList *d = DL();
  if (active) {
    GradientRounded(p, {p.x + size.x, p.y + 26}, 13, theme::colour::BlueLight(),
                    theme::colour::BlueDark());
  } else {
    d->AddRectFilled(p, {p.x + size.x, p.y + 26}, theme::colour::White(hov ? 26 : 18), 13);
    d->AddRect(p, {p.x + size.x, p.y + 26}, theme::colour::White(40), 13);
  }
  TextAt({p.x + size.x * 0.5f, p.y + 13}, theme::GetFonts().bodyBold,
         active ? theme::colour::White(255) : theme::colour::Text2(), label, true);
  return clicked;
}

bool TextField(const char *id, char *buf, size_t cap, float w) {
  ImGui::SetNextItemWidth(w);
  return ImGui::InputText(id, buf, cap);
}

bool SearchField(const char *id, char *buf, size_t cap, float w, const char *hint) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::SetNextItemWidth(w);
  ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(38, 9));
  const bool r = ImGui::InputTextWithHint(id, hint, buf, cap);
  ImGui::PopStyleVar();
  DrawIcon(Icon::Search, {p.x + 20, p.y + ImGui::GetFrameHeight() * 0.5f}, 15,
           theme::colour::Text3(), 1.8f);
  return r;
}

bool Segmented(const char *id, const char *const *items, int n, int *sel, float iw) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *d = DL();
  bool changed = false;
  const float w = n * iw + 8;
  d->AddRectFilled(p, {p.x + w, p.y + 34}, theme::colour::White(16), 17);
  d->AddRect(p, {p.x + w, p.y + 34}, theme::colour::White(36), 17);
  for (int i = 0; i < n; ++i) {
    const ImVec2 a{p.x + 4 + i * iw, p.y + 4};
    bool hov = false, held = false;
    char key[64];
    std::snprintf(key, sizeof key, "%s_%d", id, i);
    if (Hit(key, a, {iw, 26}, &hov, &held)) {
      *sel = i;
      changed = true;
    }
    if (i == *sel)
      GradientRounded(a, {a.x + iw, a.y + 26}, 13, theme::colour::BlueLight(),
                      theme::colour::BlueDark());
    TextAt({a.x + iw * 0.5f, a.y + 13}, theme::GetFonts().bodyBold,
           i == *sel ? theme::colour::White(255) : theme::colour::Text2(), items[i], true);
  }
  ImGui::SetCursorScreenPos({p.x, p.y + 38});
  return changed;
}

bool TabBar(const char *id, const char *const *labels, const Icon *icons, int n, int *sel,
            float iw) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImDrawList *d = DL();
  bool changed = false;
  const float w = n * iw + 8;
  d->AddRectFilled(p, {p.x + w, p.y + 34}, theme::colour::White(16), 17);
  d->AddRect(p, {p.x + w, p.y + 34}, theme::colour::White(36), 17);
  for (int i = 0; i < n; ++i) {
    const ImVec2 a{p.x + 4 + i * iw, p.y + 4};
    bool hov = false, held = false;
    char key[64];
    std::snprintf(key, sizeof key, "%s_%d", id, i);
    if (Hit(key, a, {iw - 2, 26}, &hov, &held)) {
      *sel = i;
      changed = true;
    }
    if (i == *sel)
      GradientRounded(a, {a.x + iw - 2, a.y + 26}, 13, theme::colour::BlueLight(),
                      theme::colour::BlueDark());
    const ImU32 fg = i == *sel ? theme::colour::White(255) : theme::colour::Text2();
    ImGui::PushFont(FontOr(theme::GetFonts().bodyBold));
    const float tw = ImGui::CalcTextSize(labels[i]).x;
    ImGui::PopFont();
    const float start = a.x + (iw - 2) * 0.5f - (tw + 22) * 0.5f;
    DrawIcon(icons[i], {start + 7, a.y + 13}, 14, fg, 1.7f);
    TextAt({start + 22, a.y + 13}, theme::GetFonts().bodyBold, fg, labels[i]);
  }
  ImGui::SetCursorScreenPos({p.x, p.y + 38});
  return changed;
}

void DashedRect(ImVec2 a, ImVec2 b, ImU32 col, float dash, float gap, float th) {
  ImDrawList *d = DL();
  const ImVec2 q[4][2] = {{a, {b.x, a.y}}, {{a.x, b.y}, b}, {a, {a.x, b.y}}, {{b.x, a.y}, b}};
  for (const auto &s : q) {
    const float dx = s[1].x - s[0].x, dy = s[1].y - s[0].y;
    const float L = std::sqrt(dx * dx + dy * dy);
    for (float t = 0; t < L; t += dash + gap) {
      const float e = ImMin(t + dash, L);
      d->AddLine({s[0].x + dx * t / L, s[0].y + dy * t / L},
                 {s[0].x + dx * e / L, s[0].y + dy * e / L}, col, th);
    }
  }
}

bool ColorSwatches(const char *id, const ImU32 *colors, int n, int *sel) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool changed = false;
  ImDrawList *d = DL();
  for (int i = 0; i < n; ++i) {
    const ImVec2 c{p.x + 11 + i * 32, p.y + 11};
    bool hov = false, held = false;
    char key[64];
    std::snprintf(key, sizeof key, "%s_%d", id, i);
    if (Hit(key, {c.x - 11, c.y - 11}, {22, 22}, &hov, &held)) {
      *sel = i;
      changed = true;
    }
    d->AddCircleFilled(c, 10.0f, colors[i]);
    d->AddCircle(c, 11.0f, i == *sel ? theme::colour::White(235) : theme::colour::White(50), 0,
                 2.0f);
  }
  return changed;
}

bool Dropdown(const char *id, const char *const *items, int n, int *sel, float w) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  bool hov = false, held = false;
  const bool open = Hit(id, p, {w, 36}, &hov, &held);
  ImDrawList *d = DL();
  bool changed = false;
  d->AddRectFilled(p, {p.x + w, p.y + 36}, theme::colour::White(hov ? 30 : 22), 11);
  d->AddRect(p, {p.x + w, p.y + 36}, theme::colour::White(46), 11);
  TextAt({p.x + 14, p.y + 18}, theme::GetFonts().bodyMedium, theme::colour::Text(), items[*sel]);
  DrawIcon(Icon::Down, {p.x + w - 18, p.y + 18}, 15, theme::colour::Text2(), 1.8f);
  char pid[64];
  std::snprintf(pid, sizeof pid, "%s_popup", id);
  if (open) ImGui::OpenPopup(pid);
  ImGui::SetNextWindowPos({p.x, p.y + 42});
  ImGui::SetNextWindowSize({w, n * 34.0f + 12});
  ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(6, 6));
  ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
  if (ImGui::BeginPopup(pid)) {
    for (int i = 0; i < n; ++i)
      if (ImGui::Selectable(items[i], i == *sel, 0, {0, 34})) {
        *sel = i;
        changed = true;
      }
    ImGui::EndPopup();
  }
  ImGui::PopStyleVar(2);
  return changed;
}

void StatusLabel(const char *text, ImU32 c) {
  const ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::PushFont(FontOr(theme::GetFonts().bodyBold));
  const ImVec2 s = ImGui::CalcTextSize(text);
  ImGui::PopFont();
  ImDrawList *d = DL();
  const ImVec2 b{p.x + s.x + 18, p.y + 20};
  d->AddRectFilled(p, b, Mul(c, 0.16f), 10);
  d->AddRect(p, b, Mul(c, 0.5f), 10);
  TextAt({p.x + 9, p.y + 10}, theme::GetFonts().bodyBold, c, text);
  ImGui::Dummy({s.x + 18, 20});
}

} // namespace pl::menu::ui
