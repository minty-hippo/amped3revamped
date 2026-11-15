// amped3 - night lights (night_lights.h).

#include "night_lights.h"

#include <cmath>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/logging.h>

#include "live_renderer.h"

namespace amped3::night_lights {
namespace {

using Light = native::Renderer::NightLight;

struct Kind {
  const char* name;
  float color[3];
  float radius;
  float intensity;
  float lift;  // metres above the point aimed at (a lamp hangs a little out)
};
constexpr Kind kKinds[] = {
    {"Lamp", {1.0f, 0.78f, 0.5f}, 14.0f, 1.0f, 0.3f},
    {"Floodlight", {0.86f, 0.92f, 1.0f}, 32.0f, 1.5f, 0.3f},
    {"Window", {1.0f, 0.66f, 0.32f}, 6.0f, 0.6f, 0.15f},
    {"Fire", {1.0f, 0.5f, 0.2f}, 9.0f, 1.1f, 0.4f},
};
constexpr int kKindCount = int(sizeof(kKinds) / sizeof(kKinds[0]));

std::mutex g_mutex;
std::filesystem::path g_folder;
uint64_t g_key = 0;
std::vector<Light> g_lights;
int g_kind = 0;

std::filesystem::path FileFor(uint64_t key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.txt", (unsigned long long)key);
  return g_folder / name;
}

void Load(uint64_t key) {
  g_lights.clear();
  std::ifstream in(FileFor(key));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    Light l;
    if (ss >> l.pos[0] >> l.pos[1] >> l.pos[2] >> l.color[0] >> l.color[1] >> l.color[2] >>
        l.radius >> l.intensity) {
      g_lights.push_back(l);
    }
  }
  REXLOG_INFO("Night lights: {} for this mountain ({})", g_lights.size(), FileFor(key).string());
}

void Save() {
  if (!g_key) return;
  std::error_code ec;
  std::filesystem::create_directories(g_folder, ec);
  std::ofstream out(FileFor(g_key));
  out << "# Amped 3 Revamped night lights: x y z  r g b  radius intensity\n";
  for (const Light& l : g_lights) {
    char b[160];
    std::snprintf(b, sizeof(b), "%.3f %.3f %.3f  %.3f %.3f %.3f  %.2f %.2f\n", l.pos[0], l.pos[1],
                  l.pos[2], l.color[0], l.color[1], l.color[2], l.radius, l.intensity);
    out << b;
  }
}

void Publish() { live::SetNightLights(g_lights); }

}  // namespace

void Install(const std::filesystem::path& folder) {
  std::lock_guard lock(g_mutex);
  g_folder = folder;
}

void Tick() {
  const uint64_t key = live::MountainKey();
  std::lock_guard lock(g_mutex);
  if (!key || key == g_key) return;
  g_key = key;
  Load(key);
  Publish();
}

std::string Place() {
  float p[3];
  if (!live::CenterPoint(p)) return "Night lights: aim at something (not the sky)";
  float cam[9];
  std::lock_guard lock(g_mutex);
  if (!g_key) return "Night lights: no mountain yet";
  const Kind& k = kKinds[g_kind];
  Light l;
  for (int i = 0; i < 3; ++i) l.pos[i] = p[i];
  // A little out from the surface, toward the camera, and up.
  float fc[6];
  const bool have_fc = live::FreecamShowing() && live::FreecamPose(fc);
  if (have_fc) std::memcpy(cam, fc, sizeof(fc));
  if (have_fc || live::GameCamera(cam)) {
    float d[3] = {cam[0] - p[0], cam[1] - p[1], cam[2] - p[2]};
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n > 1e-3f) {
      for (int i = 0; i < 3; ++i) l.pos[i] += d[i] / n * 0.25f;
    }
  }
  l.pos[1] += k.lift;
  for (int i = 0; i < 3; ++i) l.color[i] = k.color[i];
  l.radius = k.radius;
  l.intensity = k.intensity;
  g_lights.push_back(l);
  Save();
  Publish();
  return std::string("Night light placed: ") + k.name + " (" + std::to_string(g_lights.size()) +
         " here)";
}

std::string RemoveNearest() {
  float p[3];
  if (!live::CenterPoint(p)) return "Night lights: aim at the light to remove";
  std::lock_guard lock(g_mutex);
  int best = -1;
  float best_d = 6.0f * 6.0f;  // within 6 m of the point aimed at
  for (size_t i = 0; i < g_lights.size(); ++i) {
    const float* q = g_lights[i].pos;
    const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                    (q[2] - p[2]) * (q[2] - p[2]);
    if (d < best_d) {
      best_d = d;
      best = int(i);
    }
  }
  if (best < 0) return "Night lights: none near where you're aiming";
  g_lights.erase(g_lights.begin() + best);
  Save();
  Publish();
  return "Night light removed (" + std::to_string(g_lights.size()) + " left here)";
}

std::string NextKind() {
  std::lock_guard lock(g_mutex);
  g_kind = (g_kind + 1) % kKindCount;
  return std::string("Night lights: placing ") + kKinds[g_kind].name;
}

std::string KindName() {
  std::lock_guard lock(g_mutex);
  return kKinds[g_kind].name;
}

int Count() {
  std::lock_guard lock(g_mutex);
  return int(g_lights.size());
}

}  // namespace amped3::night_lights
