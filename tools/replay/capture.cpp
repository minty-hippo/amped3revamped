#include "capture.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace replay {

namespace {

uint32_t Hex(const std::string& s) { return uint32_t(std::stoul(s, nullptr, 16)); }

// Value of `key=` in a line, up to the next space (or "" if absent).
std::string Arg(const std::string& line, const std::string& key) {
  const std::string k = " " + key + "=";
  size_t p = line.find(k);
  if (p == std::string::npos) return {};
  p += k.size();
  size_t e = line.find(' ', p);
  return line.substr(p, e == std::string::npos ? std::string::npos : e - p);
}

// Text after `key=` up to the next " word=" (values containing spaces).
std::string ArgSpaced(const std::string& line, const std::string& key) {
  const std::string k = " " + key + "=";
  size_t p = line.find(k);
  if (p == std::string::npos) return {};
  p += k.size();
  size_t e = p;
  while (true) {
    size_t sp = line.find(' ', e);
    if (sp == std::string::npos) return line.substr(p);
    size_t eq = line.find('=', sp);
    size_t next_sp = line.find(' ', sp + 1);
    if (eq != std::string::npos && (next_sp == std::string::npos || eq < next_sp)) {
      return line.substr(p, sp - p);
    }
    e = sp + 1;
  }
}

std::vector<uint32_t> HexList(const std::string& s) {
  std::vector<uint32_t> out;
  std::istringstream in(s);
  std::string w;
  while (in >> w) out.push_back(Hex(w));
  return out;
}

bool StartsWith(const std::string& s, const char* prefix) {
  return s.rfind(prefix, 0) == 0;
}

}  // namespace

Capture LoadCapture(const std::filesystem::path& txt) {
  std::ifstream f(txt);
  if (!f) throw std::runtime_error("cannot open " + txt.string());
  Capture cap;
  cap.txt = txt;
  cap.resources = txt.parent_path() / txt.stem();

  GpuState state;
  Draw* draw = nullptr;
  Clear* clear = nullptr;
  Resolve* resolve = nullptr;
  uint32_t pending_tex_slot = UINT32_MAX;
  bool pending_ib = false;
  uint32_t pending_ib_object = 0;

  auto finish_draw = [&]() {
    if (draw) draw->state = std::make_shared<GpuState>(state);
    draw = nullptr;
  };

  std::string line;
  while (std::getline(f, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    if (line[0] == '[') {
      finish_draw();
      clear = nullptr;
      resolve = nullptr;
      pending_tex_slot = UINT32_MAX;
      pending_ib = false;
      const size_t close = line.find(']');
      const uint32_t number = uint32_t(std::stoul(line.substr(1, close - 1)));
      std::string rest = line.substr(close + 2);
      std::string name = rest.substr(0, rest.find(' '));
      const std::string args = " " + rest;
      if (name == "DrawVertices" || name == "DrawIndexedVertices" || name == "DrawVerticesUP" ||
          name == "DrawIndexedVerticesUP" || name == "BeginVertices") {
        Draw d;
        d.prim = uint32_t(std::stoul(Arg(args, "prim")));
        if (name == "DrawVertices") {
          d.kind = Draw::Kind::kVertices;
          d.start = uint32_t(std::stoul(Arg(args, "start")));
          d.count = uint32_t(std::stoul(Arg(args, "count")));
        } else if (name == "DrawIndexedVertices") {
          d.kind = Draw::Kind::kIndexed;
          d.base_vertex = int32_t(std::stol(Arg(args, "base_vertex")));
          d.start = uint32_t(std::stoul(Arg(args, "start_index")));
          d.count = uint32_t(std::stoul(Arg(args, "count")));
        } else if (name == "DrawVerticesUP" || name == "BeginVertices") {
          d.kind = Draw::Kind::kVerticesUP;
          d.count = uint32_t(std::stoul(Arg(args, "count")));
        } else {
          d.kind = Draw::Kind::kIndexedUP;
          d.count = uint32_t(std::stoul(Arg(args, "index_count")));
          d.index_32bit = Arg(args, "index_format") != "0";
        }
        cap.draws.push_back(std::move(d));
        draw = &cap.draws.back();
        cap.events.push_back({EventType::kDraw, uint32_t(cap.draws.size() - 1), number});
      } else if (name == "ClearF") {
        Clear c;
        c.flags = Hex(Arg(args, "flags"));
        std::string color = ArgSpaced(args, "color");
        if (color != "none") {
          std::istringstream in(color);
          for (float& v : c.color) in >> v;
        }
        c.z = std::stof(Arg(args, "z"));
        c.stencil = uint32_t(std::stoul(Arg(args, "stencil")));
        cap.clears.push_back(c);
        clear = &cap.clears.back();
        cap.events.push_back({EventType::kClear, uint32_t(cap.clears.size() - 1), number});
      } else if (name == "Resolve") {
        Resolve r;
        r.flags = Hex(Arg(args, "flags"));
        std::string rect = ArgSpaced(args, "src_rect");
        if (rect != "full") {
          auto v = HexList(rect);
          if (v.size() == 4) {
            r.full = false;
            for (int i = 0; i < 4; ++i) r.rect[i] = v[i];
          }
        }
        r.dest_object = Hex(Arg(args, "dest_tex"));
        r.clear_z = std::stof(Arg(args, "z"));
        cap.resolves.push_back(r);
        resolve = &cap.resolves.back();
        cap.events.push_back({EventType::kResolve, uint32_t(cap.resolves.size() - 1), number});
      } else if (name == "BeginTiling") {
        Tiling t;
        size_t p = 0;
        while ((p = args.find('[', p)) != std::string::npos) {
          size_t e = args.find(']', p);
          auto v = HexList(args.substr(p + 1, e - p - 1));
          if (v.size() == 4) {
            t.width = std::max(t.width, v[2]);
            t.height = std::max(t.height, v[3]);
          }
          p = e;
        }
        cap.tilings.push_back(t);
        cap.events.push_back({EventType::kBeginTiling, uint32_t(cap.tilings.size() - 1), number});
      } else if (name == "EndTiling") {
        cap.events.push_back({EventType::kEndTiling, 0, number});
      } else if (name == "Swap") {
        cap.swap_front_buffer = Hex(Arg(args, "front_buffer"));
        cap.swap_data = Arg(args, "data");
        cap.events.push_back({EventType::kSwap, 0, number});
      }
      continue;
    }

    // Indented detail lines.
    std::string t = line.substr(line.find_first_not_of(' '));
    if (StartsWith(t, "object ")) {
      // object <kind> ADDR: words
      std::istringstream in(t);
      std::string word, kind, addr;
      in >> word >> kind >> addr;
      addr.pop_back();  // ':'
      std::vector<uint32_t> words;
      while (in >> word) words.push_back(Hex(word));
      cap.objects[Hex(addr)] = words;
      continue;
    }
    if (clear) {
      if (StartsWith(t, "targets")) {
        for (uint32_t i = 0; i < 6; ++i) {
          char key[8];
          std::snprintf(key, sizeof(key), "r%04X", 0x2000 + i);
          std::string v = Arg(" " + t, key);
          if (!v.empty()) clear->targets[i] = Hex(v);
        }
        clear->has_targets = true;
      }
      continue;
    }
    if (resolve) {
      if (StartsWith(t, "dest ")) {
        size_t p = t.find("raw=[");
        if (p != std::string::npos) {
          auto v = HexList(t.substr(p + 5, t.find(']', p) - p - 5));
          for (size_t i = 0; i < 6 && i < v.size(); ++i) resolve->dest_raw[i] = v[i];
          resolve->has_dest = true;
        }
      } else if (StartsWith(t, "targets")) {
        for (uint32_t i = 0; i < 6; ++i) {
          char key[8];
          std::snprintf(key, sizeof(key), "r%04X", 0x2000 + i);
          std::string v = Arg(" " + t, key);
          if (!v.empty()) resolve->targets[i] = Hex(v);
        }
        resolve->has_targets = true;
      } else if (StartsWith(t, "dest_point=")) {
        std::istringstream in(t.substr(11));
        in >> resolve->dest_point[0] >> resolve->dest_point[1];
        resolve->has_dest_point = true;
      } else if (StartsWith(t, "clear_rgba=")) {
        std::istringstream in(t.substr(11));
        for (float& v : resolve->clear_rgba) in >> v;
      }
      continue;
    }
    if (!draw) continue;

    if (StartsWith(t, "data=")) {
      std::string name = t.substr(5);
      if (pending_tex_slot != UINT32_MAX) {
        for (auto& tb : draw->textures) {
          if (tb.slot == pending_tex_slot) tb.data = name;
        }
        pending_tex_slot = UINT32_MAX;
      } else if (pending_ib) {
        // Indexed UP draws carry their own indices (up_indices=); the device
        // index buffer listed after them is stale.
        if (draw->kind != Draw::Kind::kIndexedUP) draw->index_data = name;
        pending_ib = false;
      }
      continue;
    }
    pending_tex_slot = UINT32_MAX;
    pending_ib = false;
    if (StartsWith(t, "tex") && t.size() > 3 && std::isdigit(static_cast<unsigned char>(t[3]))) {
      FrameTexture tb;
      tb.slot = uint32_t(std::stoul(t.substr(3, t.find('=') - 3)));
      size_t p = t.find("raw=[");
      if (p != std::string::npos) {
        auto v = HexList(t.substr(p + 5, t.find(']', p) - p - 5));
        for (size_t i = 0; i < 6 && i < v.size(); ++i) tb.raw[i] = v[i];
      }
      draw->textures.push_back(tb);
      pending_tex_slot = tb.slot;
    } else if (StartsWith(t, "ib=")) {
      pending_ib = true;
      pending_ib_object = Hex(t.substr(3));
      auto it = cap.objects.find(pending_ib_object);
      if (it != cap.objects.end() && !it->second.empty() &&
          draw->kind != Draw::Kind::kIndexedUP) {
        draw->index_32bit = (it->second[0] & 0x80000000u) != 0;
      }
    } else if (StartsWith(t, "index_32bit=")) {  // native renderer frame dumps
      draw->index_32bit = t.substr(12) != "0";
    } else if (StartsWith(t, "up_vertices=")) {
      draw->up_vertices = t.substr(12);
    } else if (StartsWith(t, "up_indices=")) {
      draw->index_data = t.substr(11);
    } else if (StartsWith(t, "regs")) {
      std::istringstream in(t.substr(4));
      std::string kv;
      while (in >> kv) {
        if (kv[0] != 'r') continue;
        size_t eq = kv.find('=');
        uint32_t index = Hex(kv.substr(1, eq - 1));
        if (index >= 0x2000 && index < 0x2400) state.regs.set(index - 0x2000, Hex(kv.substr(eq + 1)));
      }
    } else if (StartsWith(t, "vs_consts") || StartsWith(t, "ps_consts")) {
      auto& consts = t[0] == 'v' ? state.vs_consts : state.ps_consts;
      size_t p = 9;
      while ((p = t.find(" c", p)) != std::string::npos) {
        size_t eq = t.find('=', p);
        uint32_t index = uint32_t(std::stoul(t.substr(p + 2, eq - p - 2)));
        size_t lb = t.find('[', eq), rb = t.find(']', lb);
        auto v = HexList(t.substr(lb + 1, rb - lb - 1));
        for (size_t i = 0; i < 4 && i < v.size() && index < 256; ++i) consts.set(index * 4 + i, v[i]);
        p = rb;
      }
    } else if (StartsWith(t, "bool_loop")) {
      std::istringstream in(t.substr(9));
      std::string kv;
      while (in >> kv) {
        size_t eq = kv.find('=');
        uint32_t index = uint32_t(std::stoul(kv.substr(1, eq - 1)));
        uint32_t slot = kv[0] == 'b' ? index : 8 + index;
        if (slot < 40) state.bool_loop.set(slot, Hex(kv.substr(eq + 1)));
      }
    } else if (StartsWith(t, "fetch")) {
      std::istringstream in(t.substr(5));
      std::string kv;
      while (in >> kv) {
        size_t eq = kv.find('=');
        uint32_t index = uint32_t(std::stoul(kv.substr(1, eq - 1)));
        if (index < 192) state.fetch.set(index, Hex(kv.substr(eq + 1)));
      }
    } else if (StartsWith(t, "ucode")) {
      draw->vs = Arg(" " + t, "vs");
      draw->ps = Arg(" " + t, "ps");
      if (draw->vs == "?") draw->vs.clear();
      if (draw->ps == "?") draw->ps.clear();
    } else if (StartsWith(t, "vmem")) {
      std::istringstream in(t.substr(4));
      std::string kv;
      while (in >> kv) {
        size_t eq = kv.find('=');
        draw->vmem[uint32_t(std::stoul(kv.substr(2, eq - 2)))] = kv.substr(eq + 1);
      }
    }
  }
  finish_draw();
  // The swapped texture's fetch constant: words 4..9 of its object dump.
  auto front = cap.objects.find(cap.swap_front_buffer);
  if (cap.swap_front_buffer && front != cap.objects.end() && front->second.size() >= 10) {
    std::copy(front->second.begin() + 4, front->second.begin() + 10, cap.swap_fetch.begin());
    cap.has_swap_fetch = true;
  }
  return cap;
}

}  // namespace replay
