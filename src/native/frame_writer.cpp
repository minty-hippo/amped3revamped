#include "frame_writer.h"

#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <set>

namespace amped3::native {
namespace {

std::string Fmt(const char* format, ...) {
  char buf[512];
  va_list args;
  va_start(args, format);
  std::vsnprintf(buf, sizeof(buf), format, args);
  va_end(args);
  return buf;
}

template <typename A>
std::string HexList(const A& a, size_t from, size_t count) {
  std::string r;
  for (size_t i = 0; i < count; ++i) r += Fmt(i ? " %08X" : "%08X", unsigned(a[from + i]));
  return r;
}

std::string Targets(const std::array<uint32_t, 6>& t) {
  std::string r = "  targets";
  for (uint32_t i = 0; i < 6; ++i) r += Fmt(" r%04X=%08X", 0x2000 + i, t[i]);
  return r + "\n";
}

}  // namespace

bool WriteFrameCapture(const Frame& frame, FrameResources& resources,
                       const std::filesystem::path& txt,
                       const std::filesystem::path& resource_dir, std::string* error) {
  namespace fs = std::filesystem;
  std::error_code ec;
  fs::create_directories(resource_dir, ec);
  std::set<std::string> saved;
  auto save = [&](const std::string& name) {
    if (name.empty() || !saved.insert(name).second) return;
    auto bytes = resources.Read(name);
    if (bytes.empty()) return;
    std::ofstream(resource_dir / (name + ".bin"), std::ios::binary)
        .write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  };

  std::string o = "# Amped 3 native renderer: recorded frame (src/native/frame_writer.cpp)\n";
  const GpuState* prev = nullptr;
  for (const auto& e : frame.events) {
    switch (e.type) {
      case EventType::kDraw: {
        const Draw& d = frame.draws[e.index];
        switch (d.kind) {
          case Draw::Kind::kVertices:
            o += Fmt("[%u] DrawVertices prim=%u start=%u count=%u\n", e.number, d.prim, d.start,
                     d.count);
            break;
          case Draw::Kind::kIndexed:
            o += Fmt("[%u] DrawIndexedVertices prim=%u base_vertex=%d start_index=%u count=%u\n",
                     e.number, d.prim, d.base_vertex, d.start, d.count);
            break;
          case Draw::Kind::kVerticesUP:
            o += Fmt("[%u] DrawVerticesUP prim=%u count=%u\n", e.number, d.prim, d.count);
            break;
          case Draw::Kind::kIndexedUP:
            o += Fmt("[%u] DrawIndexedVerticesUP prim=%u index_count=%u index_format=%u\n",
                     e.number, d.prim, d.count, d.index_32bit ? 1u : 0u);
            break;
        }
        for (const auto& t : d.textures) {
          o += Fmt("  tex%u=0 raw=[", t.slot) + HexList(t.raw, 0, 6) + "]\n";
          if (!t.data.empty()) {
            o += "    data=" + t.data + "\n";
            save(t.data + ".base");
            save(t.data + ".mips");
          }
        }
        if (!d.index_data.empty()) {
          if (d.kind == Draw::Kind::kIndexed) {
            o += "  ib=0\n    data=" + d.index_data + "\n";
          } else {
            o += "  up_indices=" + d.index_data + "\n";
          }
          o += Fmt("  index_32bit=%u\n", d.index_32bit ? 1u : 0u);
          save(d.index_data);
        }
        if (!d.up_vertices.empty()) {
          o += "  up_vertices=" + d.up_vertices + "\n";
          save(d.up_vertices);
        }
        if (d.state) {
          const GpuState& st = *d.state;
          std::string regs, vs, ps, bl, fetch;
          for (uint32_t i = 0; i < 0x400; ++i) {
            if (!prev || prev->regs[i] != st.regs[i]) regs += Fmt(" r%04X=%08X", 0x2000 + i, st.regs[i]);
          }
          for (uint32_t r = 0; r < 256; ++r) {
            bool cv = !prev, cp = !prev;
            for (uint32_t c = 0; c < 4 && prev; ++c) {
              cv = cv || prev->vs_consts[r * 4 + c] != st.vs_consts[r * 4 + c];
              cp = cp || prev->ps_consts[r * 4 + c] != st.ps_consts[r * 4 + c];
            }
            if (cv) vs += Fmt(" c%u=[", r) + HexList(st.vs_consts, r * 4, 4) + "]";
            if (cp) ps += Fmt(" c%u=[", r) + HexList(st.ps_consts, r * 4, 4) + "]";
          }
          for (uint32_t i = 0; i < 40; ++i) {
            if (!prev || prev->bool_loop[i] != st.bool_loop[i]) {
              bl += Fmt(" %s%u=%08X", i < 8 ? "b" : "l", i < 8 ? i : i - 8, st.bool_loop[i]);
            }
          }
          for (uint32_t i = 0; i < 192; ++i) {
            if (!prev || prev->fetch[i] != st.fetch[i]) fetch += Fmt(" f%u=%08X", i, st.fetch[i]);
          }
          if (!regs.empty()) o += "  regs" + regs + "\n";
          if (!vs.empty()) o += "  vs_consts" + vs + "\n";
          if (!ps.empty()) o += "  ps_consts" + ps + "\n";
          if (!bl.empty()) o += "  bool_loop" + bl + "\n";
          if (!fetch.empty()) o += "  fetch" + fetch + "\n";
          prev = &st;
        }
        o += "  ucode vs=" + (d.vs.empty() ? std::string("?") : d.vs) +
             " ps=" + (d.ps.empty() ? std::string("?") : d.ps) + "\n";
        save(d.vs);
        save(d.ps);
        std::string vmem;
        for (const auto& [vf, name] : d.vmem) {
          vmem += Fmt(" vf%u=", vf) + name;
          save(name);
        }
        if (!vmem.empty()) o += "  vmem" + vmem + "\n";
        break;
      }
      case EventType::kClear: {
        const auto& c = frame.clears[e.index];
        o += Fmt("[%u] ClearF flags=%08X rect=full color=%.9g %.9g %.9g %.9g z=%.9g stencil=%u\n",
                 e.number, c.flags, c.color[0], c.color[1], c.color[2], c.color[3], c.z,
                 c.stencil);
        if (c.has_targets) o += Targets(c.targets);
        break;
      }
      case EventType::kResolve: {
        const auto& r = frame.resolves[e.index];
        const std::string rect = r.full ? std::string("full")
                                        : Fmt("%08X %08X %08X %08X", r.rect[0], r.rect[1],
                                              r.rect[2], r.rect[3]);
        o += Fmt("[%u] Resolve flags=%08X src_rect=", e.number, r.flags) + rect +
             Fmt(" dest_tex=%08X z=%.9g\n", r.dest_object, r.clear_z);
        if (r.has_dest) o += "  dest raw=[" + HexList(r.dest_raw, 0, 6) + "]\n";
        if (r.has_targets) o += Targets(r.targets);
        if (r.has_dest_point) o += Fmt("  dest_point=%u %u\n", r.dest_point[0], r.dest_point[1]);
        o += Fmt("  clear_rgba=%.9g %.9g %.9g %.9g\n", r.clear_rgba[0], r.clear_rgba[1],
                 r.clear_rgba[2], r.clear_rgba[3]);
        break;
      }
      case EventType::kBeginTiling: {
        const auto& t = frame.tilings[e.index];
        o += Fmt("[%u] BeginTiling flags=0 tiles=1 [00000000 00000000 %08X %08X]\n", e.number,
                 t.width, t.height);
        break;
      }
      case EventType::kEndTiling:
        o += Fmt("[%u] EndTiling\n", e.number);
        break;
      case EventType::kSwap:
        o += Fmt("[%u] Swap front_buffer=%08X", e.number, frame.swap_front_buffer);
        if (!frame.swap_data.empty()) {
          o += " data=" + frame.swap_data;
          save(frame.swap_data + ".base");
          save(frame.swap_data + ".mips");
        }
        o += "\n";
        if (frame.has_swap_fetch) {
          o += Fmt("  object texture %08X: 00000000 00000000 00000000 00000000 ",
                   frame.swap_front_buffer) +
               HexList(frame.swap_fetch, 0, 6) + "\n";
        }
        break;
    }
  }
  std::ofstream out(txt, std::ios::binary);
  if (!out) {
    *error = "cannot write " + txt.string();
    return false;
  }
  out << o;
  return true;
}

}  // namespace amped3::native
