// amped3 - the controls along the bottom of the screen (help_bar.h).

#include "help_bar.h"

#include <algorithm>
#include <atomic>
#include <cfloat>

#include <imgui.h>

#include "graphics_panel.h"

namespace amped3::help_bar {
namespace {
std::atomic<bool> g_visible{true};
}  // namespace

bool Visible() { return g_visible.load(); }
void Toggle() { g_visible = !g_visible.load(); }

int Draw(const ImGuiIO& io, const std::string& title, const std::vector<std::string>& items,
         int lift) {
  const float w = io.DisplaySize.x, h = io.DisplaySize.y;
  if (w < 64 || h < 64) return 0;
  // The picture's 16:9 part (letterboxed).
  const float pic_w = std::min(w, h * 16.0f / 9.0f), pic_h = std::min(h, w * 9.0f / 16.0f);
  const float left = (w - pic_w) * 0.5f, bottom = (h + pic_h) * 0.5f;
  const float px = std::max(12.0f, pic_h * 0.02f);
  ImFont* font = amped3::UiFont(px);
  const float gap = px * 1.4f, row_h = px * 1.5f, margin = pic_w * 0.03f;
  // Rows: the title alone when hidden (with how to bring the rest back).
  std::vector<std::string> shown;
  shown.push_back(title);
  if (g_visible.load()) {
    shown.insert(shown.end(), items.begin(), items.end());
  } else {
    shown.push_back("View / H: show controls");
  }
  std::vector<std::vector<std::string>> rows(1);
  float x = 0.0f;
  const float max_w = pic_w - 2.0f * margin;
  for (const auto& item : shown) {
    const float iw = font->CalcTextSizeA(px, FLT_MAX, 0, item.c_str()).x;
    if (!rows.back().empty() && x + iw > max_w) {
      rows.emplace_back();
      x = 0.0f;
    }
    rows.back().push_back(item);
    x += iw + gap;
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const int n = int(rows.size());
  const float y_top = bottom - margin * 0.5f - row_h * float(n + lift);
  dl->AddRectFilled(ImVec2(left + margin * 0.5f, y_top - px * 0.3f),
                    ImVec2(left + pic_w - margin * 0.5f, y_top + row_h * float(n) + px * 0.1f),
                    IM_COL32(0, 0, 0, 140), px * 0.3f);
  for (int r = 0; r < n; ++r) {
    float rx = left + margin;
    for (size_t i = 0; i < rows[r].size(); ++i) {
      const auto& item = rows[r][i];
      const bool is_title = r == 0 && i == 0;
      dl->AddText(font, px, ImVec2(rx, y_top + row_h * float(r)),
                  is_title ? IM_COL32(250, 210, 40, 240) : IM_COL32(255, 255, 255, 225),
                  item.c_str());
      rx += font->CalcTextSizeA(px, FLT_MAX, 0, item.c_str()).x + gap;
    }
  }
  return n;
}

}  // namespace amped3::help_bar
