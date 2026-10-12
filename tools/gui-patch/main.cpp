// amped3-gui-patch: runs the menu patches (src/gui_patch_core.cpp) on a
// package from the command line, for testing:
//
//   amped3-gui-patch game|base <in.rui> <out.rui> [extract-dir]
//
// With an extract dir, the patched package's changed files are written there
// too.

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

#include "../../src/gui_patch_core.h"

using amped3::gui_patch::Bytes;

static bool Load(const std::string& path, Bytes* out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) return false;
  out->assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
  return true;
}

static bool Store(const std::filesystem::path& path, const Bytes& b) {
  if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
  std::ofstream f(path, std::ios::binary);
  f.write(reinterpret_cast<const char*>(b.data()), std::streamsize(b.size()));
  return bool(f);
}

int main(int argc, char** argv) {
  if (argc < 4) {
    std::fprintf(stderr, "usage: %s game|base <in.rui> <out.rui> [extract-dir]\n", argv[0]);
    return 2;
  }
  const std::string kind = argv[1];
  Bytes in, out;
  if (!Load(argv[2], &in)) {
    std::fprintf(stderr, "can't read %s\n", argv[2]);
    return 1;
  }
  std::string error;
  const bool ok = kind == "game" ? amped3::gui_patch::PatchGamePackage(in, &out, &error)
                                 : amped3::gui_patch::PatchBasePackage(in, &out, &error);
  if (!ok) {
    std::fprintf(stderr, "patch failed: %s\n", error.c_str());
    return 1;
  }
  if (!Store(argv[3], out)) return 1;
  std::printf("%s: %zu -> %zu bytes\n", argv[3], in.size(), out.size());
  if (argc > 4) {
    std::vector<amped3::gui_patch::ZipEntry> before, after;
    amped3::gui_patch::ReadZip(in, &before, &error);
    if (!amped3::gui_patch::ReadZip(out, &after, &error)) {
      std::fprintf(stderr, "re-reading the output failed: %s\n", error.c_str());
      return 1;
    }
    for (size_t i = 0; i < after.size(); ++i) {
      if (i < before.size() && before[i].data == after[i].data) continue;
      Store(std::filesystem::path(argv[4]) / after[i].name, after[i].data);
      std::printf("  changed: %s (%zu bytes)\n", after[i].name.c_str(), after[i].data.size());
    }
  }
  return 0;
}
