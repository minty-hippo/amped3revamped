// amped3 - night lights (night_lights.h).

#include "night_lights.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <vector>

#include <rex/logging.h>

#include "live_renderer.h"

namespace amped3::night_lights {
namespace {

using Light = native::Renderer::NightLight;
using Instance = native::Renderer::Instance;

struct Kind {
  const char* name;
  float color[3];
  float radius;
  float intensity;
  float lift;  // metres above the point aimed at (a bulb hangs, a fire's light is over its logs)
  float out;   // metres out from the surface toward the camera
};
// (In the renderer's order: NightLight::kind.)
constexpr Kind kKinds[] = {
    {"Lamp", {1.0f, 0.78f, 0.5f}, 14.0f, 1.0f, 0.3f, 0.25f},
    {"Floodlight", {0.86f, 0.92f, 1.0f}, 34.0f, 1.6f, 0.35f, 0.35f},
    {"Window", {1.0f, 0.66f, 0.32f}, 5.0f, 0.6f, 0.0f, 0.05f},
    {"Campfire", {1.0f, 0.52f, 0.2f}, 10.0f, 1.2f, 0.4f, 0.0f},
};
constexpr int kKindCount = int(sizeof(kKinds) / sizeof(kKinds[0]));

// A light on every copy of an object: which object (its mesh), where on it
// (its own space), and the light.
struct Template {
  uint64_t mesh = 0;
  float local[3] = {};
  float local_dir[3] = {0.0f, -1.0f, 0.0f};
  Light light;
};
// A light found on a copy (as the copies come into view).
struct Found {
  int tmpl;
  Light light;
};

std::mutex g_mutex;
std::filesystem::path g_folder;
uint64_t g_key = 0;
std::vector<Light> g_lights;      // placed one by one
std::vector<Template> g_templates;
std::vector<Found> g_found;
int g_kind = 0;
uint64_t g_instances_version = 0;
std::vector<Instance> g_instances;
bool g_dirty = false;
std::chrono::steady_clock::time_point g_last_save{};

std::filesystem::path FileFor(uint64_t key) {
  char name[32];
  std::snprintf(name, sizeof(name), "%016llx.txt", (unsigned long long)key);
  return g_folder / name;
}

bool ReadLight(std::istringstream& ss, Light* l) {
  if (!(ss >> l->pos[0] >> l->pos[1] >> l->pos[2] >> l->color[0] >> l->color[1] >> l->color[2] >>
        l->radius >> l->intensity)) {
    return false;
  }
  // (Then, from later versions: kind and direction.)
  int kind;
  float d[3];
  if (ss >> kind >> d[0] >> d[1] >> d[2]) {
    l->kind = std::clamp(kind, 0, 3);
    std::memcpy(l->dir, d, sizeof(d));
  }
  return true;
}

void Load(uint64_t key) {
  g_lights.clear();
  g_templates.clear();
  g_found.clear();
  std::ifstream in(FileFor(key));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    // "T mesh lx ly lz r g b radius intensity": on every copy; "A t x y z
    // ...": found on a copy of template t; else one light.
    if (line[0] == 'T') {
      std::string tag, mesh;
      Template t;
      if (ss >> tag >> mesh >> t.local[0] >> t.local[1] >> t.local[2] >> t.light.color[0] >>
          t.light.color[1] >> t.light.color[2] >> t.light.radius >> t.light.intensity) {
        t.mesh = std::stoull(mesh, nullptr, 16);
        int kind;
        if (ss >> kind >> t.local_dir[0] >> t.local_dir[1] >> t.local_dir[2]) {
          t.light.kind = std::clamp(kind, 0, 3);
        }
        g_templates.push_back(t);
      }
    } else if (line[0] == 'A') {
      std::string tag;
      Found f;
      if (ss >> tag >> f.tmpl && ReadLight(ss, &f.light) && f.tmpl >= 0 &&
          f.tmpl < int(g_templates.size())) {
        g_found.push_back(f);
      }
    } else {
      Light l;
      if (ReadLight(ss, &l)) g_lights.push_back(l);
    }
  }
  REXLOG_INFO("Night lights: {} placed, {} on every copy of {} objects ({})", g_lights.size(),
              g_found.size(), g_templates.size(), FileFor(key).string());
}

void WriteLight(std::ofstream& out, const Light& l) {
  char b[200];
  std::snprintf(b, sizeof(b), "%.3f %.3f %.3f  %.3f %.3f %.3f  %.2f %.2f  %d %.3f %.3f %.3f\n",
                l.pos[0], l.pos[1], l.pos[2], l.color[0], l.color[1], l.color[2], l.radius,
                l.intensity, l.kind, l.dir[0], l.dir[1], l.dir[2]);
  out << b;
}

void Save() {
  if (!g_key) return;
  std::error_code ec;
  std::filesystem::create_directories(g_folder, ec);
  std::ofstream out(FileFor(g_key));
  out << "# Amped 3 Revamped night lights. A light a line: x y z  r g b  radius intensity  kind dx dy dz\n"
         "# (kind: 0 lamp, 1 floodlight, 2 window, 3 campfire; d: where it faces)\n"
         "# T mesh  lx ly lz  r g b  radius intensity: one on every copy of an object\n"
         "# A template  x y z ...: found on a copy so far\n";
  for (const Light& l : g_lights) WriteLight(out, l);
  for (const Template& t : g_templates) {
    char b[200];
    std::snprintf(b, sizeof(b),
                  "T %016llx  %.3f %.3f %.3f  %.3f %.3f %.3f  %.2f %.2f  %d %.3f %.3f %.3f\n",
                  (unsigned long long)t.mesh, t.local[0], t.local[1], t.local[2], t.light.color[0],
                  t.light.color[1], t.light.color[2], t.light.radius, t.light.intensity,
                  t.light.kind, t.local_dir[0], t.local_dir[1], t.local_dir[2]);
    out << b;
  }
  for (const Found& f : g_found) {
    out << "A " << f.tmpl << "  ";
    WriteLight(out, f.light);
  }
  g_dirty = false;
  g_last_save = std::chrono::steady_clock::now();
}

void Publish() {
  std::vector<Light> all = g_lights;
  for (const Found& f : g_found) all.push_back(f.light);
  live::SetNightLights(all);
}

void Apply(const Instance& in, const float local[3], float out[3]) {
  for (int r = 0; r < 3; ++r) {
    out[r] = in.m[r * 4] * local[0] + in.m[r * 4 + 1] * local[1] + in.m[r * 4 + 2] * local[2] +
             in.m[r * 4 + 3];
  }
}

void ApplyDir(const Instance& in, const float local[3], float out[3]) {
  for (int r = 0; r < 3; ++r) {
    out[r] = in.m[r * 4] * local[0] + in.m[r * 4 + 1] * local[1] + in.m[r * 4 + 2] * local[2];
  }
  const float n = std::sqrt(out[0] * out[0] + out[1] * out[1] + out[2] * out[2]);
  if (n > 1e-6f) {
    for (int i = 0; i < 3; ++i) out[i] /= n;
  }
}

// Into the object's own space (its 3 x 3 part inverted); `dir`: a direction.
bool ToLocal(const Instance& in, const float p[3], float out[3], bool dir = false) {
  const float* m = in.m;
  const double a = m[0], b = m[1], c = m[2], d = m[4], e = m[5], f = m[6], g = m[8], h = m[9],
               i = m[10];
  const double det = a * (e * i - f * h) - b * (d * i - f * g) + c * (d * h - e * g);
  if (std::fabs(det) < 1e-12) return false;
  const double q[3] = {p[0] - (dir ? 0.0f : m[3]), p[1] - (dir ? 0.0f : m[7]),
                       p[2] - (dir ? 0.0f : m[11])};
  const double inv[9] = {(e * i - f * h) / det, (c * h - b * i) / det, (b * f - c * e) / det,
                         (f * g - d * i) / det, (a * i - c * g) / det, (c * d - a * f) / det,
                         (d * h - e * g) / det, (b * g - a * h) / det, (a * e - b * d) / det};
  for (int r = 0; r < 3; ++r) out[r] = float(inv[r * 3] * q[0] + inv[r * 3 + 1] * q[1] + inv[r * 3 + 2] * q[2]);
  return true;
}

// The light for the point aimed at (out from the surface toward the camera,
// up a little), in the current kind.
bool AimedLight(Light* l) {
  float p[3];
  if (!live::CenterPoint(p)) return false;
  const Kind& k = kKinds[g_kind];
  for (int i = 0; i < 3; ++i) l->pos[i] = p[i];
  // The camera: where from (out from the surface toward it) and which way
  // it looks (a floodlight shines that way; a window faces back along it).
  float cam[9] = {};
  float fc[6];
  const bool have_fc = live::FreecamShowing() && live::FreecamPose(fc);
  if (have_fc) std::memcpy(cam, fc, sizeof(fc));
  const bool have_cam = have_fc || live::GameCamera(cam);
  float fwd[3] = {0.0f, 0.0f, 1.0f};
  if (have_cam) {
    float d[3] = {cam[0] - p[0], cam[1] - p[1], cam[2] - p[2]};
    const float n = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (n > 1e-3f) {
      for (int i = 0; i < 3; ++i) {
        l->pos[i] += d[i] / n * k.out;
        fwd[i] = -d[i] / n;
      }
    }
  }
  l->pos[1] += k.lift;
  for (int i = 0; i < 3; ++i) l->color[i] = k.color[i];
  l->radius = k.radius;
  l->intensity = k.intensity;
  l->kind = g_kind;
  switch (g_kind) {
    case 1:  // shining the way the camera looks
      std::memcpy(l->dir, fwd, sizeof(fwd));
      break;
    case 2: {  // facing the camera, level
      const float h = std::sqrt(fwd[0] * fwd[0] + fwd[2] * fwd[2]);
      l->dir[0] = h > 1e-3f ? -fwd[0] / h : 0.0f;
      l->dir[1] = 0.0f;
      l->dir[2] = h > 1e-3f ? -fwd[2] / h : 1.0f;
      break;
    }
    default:
      l->dir[0] = 0.0f;
      l->dir[1] = -1.0f;
      l->dir[2] = 0.0f;
  }
  return true;
}

// The lights on copies come into view.
void Discover() {
  if (g_templates.empty()) return;
  bool added = false;
  for (const Instance& in : g_instances) {
    for (int t = 0; t < int(g_templates.size()); ++t) {
      const Template& tm = g_templates[t];
      if (tm.mesh != in.key) continue;
      float p[3];
      Apply(in, tm.local, p);
      bool known = false;
      for (const Found& f : g_found) {
        if (f.tmpl != t) continue;
        const float dx = f.light.pos[0] - p[0], dy = f.light.pos[1] - p[1], dz = f.light.pos[2] - p[2];
        if (dx * dx + dy * dy + dz * dz < 0.5f * 0.5f) {
          known = true;
          break;
        }
      }
      if (known) continue;
      Found f{t, tm.light};
      std::memcpy(f.light.pos, p, sizeof(p));
      ApplyDir(in, tm.local_dir, f.light.dir);
      g_found.push_back(f);
      added = true;
    }
  }
  if (added) {
    g_dirty = true;
    Publish();
  }
}

}  // namespace

void Install(const std::filesystem::path& folder) {
  std::lock_guard lock(g_mutex);
  g_folder = folder;
}

void Tick() {
  const uint64_t key = live::MountainKey();
  std::lock_guard lock(g_mutex);
  if (key && key != g_key) {
    if (g_dirty) Save();
    g_key = key;
    Load(key);
    Publish();
  }
  if (!g_key || g_templates.empty()) return;
  const uint64_t v = live::Instances(&g_instances, g_instances_version);
  if (v != g_instances_version) {
    g_instances_version = v;
    Discover();
  }
  if (g_dirty && std::chrono::steady_clock::now() - g_last_save > std::chrono::seconds(5)) Save();
}

std::string Place() {
  std::lock_guard lock(g_mutex);
  if (!g_key) return "Night lights: no mountain yet";
  Light l;
  if (!AimedLight(&l)) return "Night lights: aim at something (not the sky)";
  g_lights.push_back(l);
  Save();
  Publish();
  return std::string("Night light placed: ") + kKinds[g_kind].name + " (" +
         std::to_string(g_lights.size() + g_found.size()) + " here)";
}

std::string PlaceOnEveryCopy() {
  std::lock_guard lock(g_mutex);
  if (!g_key) return "Night lights: no mountain yet";
  Light l;
  float p[3];
  if (!live::CenterPoint(p) || !AimedLight(&l)) return "Night lights: aim at an object";
  // The object aimed at: the copy whose own origin (a lift tower's base, a
  // lamp post's foot) is right below the point (nearest across, not above
  // it, not far below).
  live::Instances(&g_instances, ~0ull);  // (the latest, whatever version)
  const Instance* best = nullptr;
  float best_d = 6.0f;
  for (const Instance& in : g_instances) {
    const float up = p[1] - in.m[7];
    if (up < -2.0f || up > 40.0f) continue;
    const float dx = p[0] - in.m[3], dz = p[2] - in.m[11];
    const float d = std::sqrt(dx * dx + dz * dz);
    if (d < best_d) {
      best_d = d;
      best = &in;
    }
  }
  if (!best) return "Night lights: no object found there (try aiming at a lift tower or post)";
  Template t;
  t.mesh = best->key;
  t.light = l;
  if (!ToLocal(*best, l.pos, t.local) || !ToLocal(*best, l.dir, t.local_dir, true)) {
    return "Night lights: that object can't take one";
  }
  int copies = 0;
  for (const Instance& in : g_instances) copies += in.key == t.mesh ? 1 : 0;
  g_templates.push_back(t);
  Discover();
  Save();
  Publish();
  return std::string("Night lights: ") + kKinds[g_kind].name + " on every copy (" +
         std::to_string(copies) + " in view, more as they come into view)";
}

std::string RemoveNearest() {
  float p[3];
  if (!live::CenterPoint(p)) return "Night lights: aim at the light to remove";
  std::lock_guard lock(g_mutex);
  int best = -1;
  bool found = false;
  float best_d = 6.0f * 6.0f;  // within 6 m of the point aimed at
  auto consider = [&](const Light& l, int i, bool is_found) {
    const float* q = l.pos;
    const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) +
                    (q[2] - p[2]) * (q[2] - p[2]);
    if (d < best_d) {
      best_d = d;
      best = i;
      found = is_found;
    }
  };
  for (size_t i = 0; i < g_lights.size(); ++i) consider(g_lights[i], int(i), false);
  for (size_t i = 0; i < g_found.size(); ++i) consider(g_found[i].light, int(i), true);
  if (best < 0) return "Night lights: none near where you're aiming";
  std::string what;
  if (found) {
    // A light on every copy: all of them go (the template too).
    const int t = g_found[best].tmpl;
    std::vector<Found> keep;
    for (const Found& f : g_found) {
      if (f.tmpl == t) continue;
      Found k = f;
      if (k.tmpl > t) --k.tmpl;
      keep.push_back(k);
    }
    g_found.swap(keep);
    g_templates.erase(g_templates.begin() + t);
    what = "Night lights: removed from every copy";
  } else {
    g_lights.erase(g_lights.begin() + best);
    what = "Night light removed";
  }
  Save();
  Publish();
  return what + " (" + std::to_string(g_lights.size() + g_found.size()) + " left here)";
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
  return int(g_lights.size() + g_found.size());
}

}  // namespace amped3::night_lights
