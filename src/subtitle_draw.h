// amped3 - how the subtitles look: laid out and drawn over the game's 16:9
// picture. The game's overlay (subtitles.cpp) and the launcher's subtitle
// editor preview (over a movie's picture) both draw them with this, so the
// preview is the game's, scaled to the picture.
//
// The look is the player's (Options > Audio, the launcher's Subtitles page,
// the editor's Look): the cvars subtitles_size, subtitles_background,
// subtitles_box, subtitles_edge, subtitles_position and subtitles_width.

#pragma once

#include <algorithm>
#include <cfloat>
#include <string>
#include <vector>

#include <imgui.h>

namespace amped3::subtitle_draw {

struct Style {
  int size = 100;       // text size, % (the picture's height x 4.2% at 100)
  int background = 60;  // the shading behind the text, % opaque (0: none)
  int box = 0;          // 0: a box behind each row, 1: one behind them all
  int edge = 1;         // around the letters: 0 nothing, 1 a drop shadow, 2 an outline
  int position = 8;     // the last row's bottom, % of the picture's height up from its bottom
  int width = 78;       // rows wrapped at this % of the picture's width
};

struct Line {
  std::string text;
  ImU32 color = IM_COL32(255, 255, 255, 255);
};

// The text size for a picture `pic_h` high (min_px: not smaller, in game).
inline float TextPx(const Style& st, float pic_h, float min_px) {
  return std::max(min_px, pic_h * 0.042f * float(std::clamp(st.size, 25, 300)) / 100.0f);
}

// Draws `lines` (the newest last, at the bottom) over the picture at
// `pic0` (`pic_w` x `pic_h`).
inline void Draw(ImDrawList* dl, ImFont* font, const Style& st, ImVec2 pic0, float pic_w, float pic_h,
                 const std::vector<Line>& lines, float min_px = 12.0f) {
  if (lines.empty() || !font || pic_w < 16 || pic_h < 16) return;
  const float px = TextPx(st, pic_h, min_px);
  const float max_w = pic_w * float(std::clamp(st.width, 30, 100)) / 100.0f;
  // Wrapped rows, word by word.
  std::vector<Line> rows;
  for (const Line& line : lines) {
    const std::string& t = line.text;
    std::string row;
    size_t pos = 0;
    while (pos < t.size()) {
      size_t sp = t.find(' ', pos);
      if (sp == std::string::npos) sp = t.size();
      const std::string word = t.substr(pos, sp - pos);
      const std::string attempt = row.empty() ? word : row + " " + word;
      if (!row.empty() && font->CalcTextSizeA(px, FLT_MAX, 0, attempt.c_str()).x > max_w) {
        rows.push_back({row, line.color});
        row = word;
      } else {
        row = attempt;
      }
      pos = sp + 1;
    }
    if (!row.empty()) rows.push_back({row, line.color});
  }
  if (rows.empty()) return;
  const float line_h = px * 1.22f;
  const float pad_x = px * 0.45f, pad_y = px * 0.18f;
  const float bottom = pic0.y + pic_h - pic_h * float(std::clamp(st.position, 0, 60)) / 100.0f;
  const float top = bottom - line_h * float(rows.size());
  const int bg = std::clamp(st.background, 0, 100) * 255 / 100;
  const float cx = pic0.x + pic_w * 0.5f;
  if (bg > 0 && st.box == 1) {
    // One block behind them all, as wide as the widest row.
    float widest = 0;
    for (const Line& r : rows) widest = std::max(widest, font->CalcTextSizeA(px, FLT_MAX, 0, r.text.c_str()).x);
    dl->AddRectFilled(ImVec2(cx - widest * 0.5f - pad_x * 1.4f, top - pad_y * 2.0f),
                      ImVec2(cx + widest * 0.5f + pad_x * 1.4f, bottom + pad_y * 0.6f), IM_COL32(0, 0, 0, bg),
                      px * 0.3f);
  }
  float y = top;
  for (const Line& r : rows) {
    const ImVec2 size = font->CalcTextSizeA(px, FLT_MAX, 0, r.text.c_str());
    const float x = cx - size.x * 0.5f;
    if (bg > 0 && st.box != 1) {
      dl->AddRectFilled(ImVec2(x - pad_x, y - pad_y), ImVec2(x + size.x + pad_x, y + line_h - pad_y),
                        IM_COL32(0, 0, 0, bg), px * 0.2f);
    }
    const float o = std::max(1.0f, px / 22.0f);
    if (st.edge == 1) {
      dl->AddText(font, px, ImVec2(x + o, y + o), IM_COL32(0, 0, 0, 200), r.text.c_str());
    } else if (st.edge == 2) {
      const float ox[8] = {-1, 0, 1, -1, 1, -1, 0, 1}, oy[8] = {-1, -1, -1, 0, 0, 1, 1, 1};
      for (int k = 0; k < 8; ++k) {
        dl->AddText(font, px, ImVec2(x + ox[k] * o, y + oy[k] * o), IM_COL32(0, 0, 0, 230), r.text.c_str());
      }
    }
    dl->AddText(font, px, ImVec2(x, y), r.color, r.text.c_str());
    y += line_h;
  }
}

}  // namespace amped3::subtitle_draw
