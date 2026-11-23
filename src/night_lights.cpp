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
using Clock = std::chrono::steady_clock;

struct Kind {
  const char* name;
  float color[3];
  float radius;
  float intensity;
  float spread;  // half its cone (degrees; 180 all round)
};
// (In the renderer's order: NightLight::kind.)
constexpr Kind kKinds[] = {
    {"Lamp", {1.0f, 0.78f, 0.5f}, 14.0f, 1.0f, 180.0f},
    {"Floodlight", {0.86f, 0.92f, 1.0f}, 34.0f, 1.6f, 35.0f},
    {"Window / panel", {1.0f, 0.66f, 0.32f}, 5.0f, 0.6f, 75.0f},
    {"Campfire", {1.0f, 0.52f, 0.2f}, 10.0f, 1.2f, 180.0f},
    {"Street light", {1.0f, 0.84f, 0.62f}, 22.0f, 1.7f, 90.0f},
    {"Caged light", {1.0f, 0.42f, 0.12f}, 10.0f, 1.3f, 180.0f},
};
constexpr int kKindCount = int(sizeof(kKinds) / sizeof(kKinds[0]));

// Colors to pick from (-1: the kind's own).
struct Tint {
  const char* name;
  float color[3];
};
constexpr Tint kTints[] = {
    {"warm white", {1.0f, 0.78f, 0.5f}},  {"daylight", {1.0f, 0.95f, 0.88f}},
    {"cool white", {0.8f, 0.9f, 1.0f}},   {"amber", {1.0f, 0.6f, 0.2f}},
    {"red", {1.0f, 0.22f, 0.16f}},        {"green", {0.35f, 1.0f, 0.42f}},
    {"blue", {0.32f, 0.52f, 1.0f}},       {"purple", {0.75f, 0.38f, 1.0f}},
    {"pink", {1.0f, 0.45f, 0.75f}},
};
constexpr int kTintCount = int(sizeof(kTints) / sizeof(kTints[0]));

// A light on every copy of an object: which object (its mesh), where on it
// and which way (its own space), and the light.
struct Template {
  uint64_t mesh = 0;
  float local[3] = {};
  float local_dir[3] = {0.0f, -1.0f, 0.0f};
  Light light;
};
// A light on one copy, found as it came into view; followed while its
// object moves (a moving one isn't kept in the file, nor drawn once out of
// sight).
struct Found {
  int tmpl = 0;
  Light light;
  bool moving = false;
  uint64_t seen = 0;  // the instances' version it was last seen in
};
struct State {
  std::vector<Light> lights;
  std::vector<Template> templates;
  std::vector<Found> found;
};

std::mutex g_mutex;
std::filesystem::path g_folder;
uint64_t g_key = 0;
State g_s;
std::vector<State> g_undo;  // before each change (the last 50)
int g_kind = 0;
bool g_editor = false;
float g_yaw = 0.0f;    // the preview's turn (degrees)
float g_pitch = 0.0f;  // and tilt (degrees, up)
int g_tint = -1;       // its color (kTints; -1: the kind's own)
// What the D-pad changes (NextMode): 0 turn / tilt, 1 width / height (a
// window or panel), 2 reach / spread, 3 color / brightness.
int g_mode = 0;
// The preview's size (a window or panel), and its reach, spread and
// brightness against its kind's own (1: as the kind has it).
float g_size[2] = {1.3f, 1.1f};
float g_reach = 1.0f, g_spread = 1.0f, g_bright = 1.0f;
// The picked light: one placed (index), or a template's copy (template, the
// copy's position: copies come and go).
struct Pick {
  bool on = false;
  bool every = false;
  int index = -1;
  float pos[3] = {};
};
Pick g_pick;
uint64_t g_instances_version = 0;
std::vector<Instance> g_instances;
bool g_dirty = false;
Clock::time_point g_last_save{};
Light g_preview;
bool g_preview_ok = false;

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
    l->kind = std::clamp(kind, 0, kKindCount - 1);
    std::memcpy(l->dir, d, sizeof(d));
    // (Then: a panel's size, its spread.)
    float w, h, sp;
    if (ss >> w >> h >> sp) {
      l->size[0] = std::clamp(w, 0.1f, 30.0f);
      l->size[1] = std::clamp(h, 0.1f, 30.0f);
      l->spread = std::clamp(sp, 0.0f, 180.0f);
    }
  }
  return true;
}

void Load(uint64_t key) {
  g_s = State{};
  g_undo.clear();
  g_pick = Pick{};
  std::ifstream in(FileFor(key));
  std::string line;
  while (std::getline(in, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::istringstream ss(line);
    if (line[0] == 'T') {
      std::string tag, mesh;
      Template t;
      if (ss >> tag >> mesh >> t.local[0] >> t.local[1] >> t.local[2] >> t.light.color[0] >>
          t.light.color[1] >> t.light.color[2] >> t.light.radius >> t.light.intensity) {
        t.mesh = std::stoull(mesh, nullptr, 16);
        int kind;
        if (ss >> kind >> t.local_dir[0] >> t.local_dir[1] >> t.local_dir[2]) {
          t.light.kind = std::clamp(kind, 0, kKindCount - 1);
          float w, h, sp;
          if (ss >> w >> h >> sp) {
            t.light.size[0] = std::clamp(w, 0.1f, 30.0f);
            t.light.size[1] = std::clamp(h, 0.1f, 30.0f);
            t.light.spread = std::clamp(sp, 0.0f, 180.0f);
          }
        }
        g_s.templates.push_back(t);
      }
    } else if (line[0] == 'A') {
      std::string tag;
      Found f;
      if (ss >> tag >> f.tmpl && ReadLight(ss, &f.light) && f.tmpl >= 0 &&
          f.tmpl < int(g_s.templates.size())) {
        g_s.found.push_back(f);
      }
    } else {
      Light l;
      if (ReadLight(ss, &l)) g_s.lights.push_back(l);
    }
  }
  REXLOG_INFO("Night lights: {} placed, {} on every copy of {} objects ({})", g_s.lights.size(),
              g_s.found.size(), g_s.templates.size(), FileFor(key).string());
}

void WriteLight(std::ofstream& out, const Light& l) {
  char b[240];
  std::snprintf(b, sizeof(b), "%.3f %.3f %.3f  %.3f %.3f %.3f  %.2f %.2f  %d %.3f %.3f %.3f  %.2f %.2f %.1f\n",
                l.pos[0], l.pos[1], l.pos[2], l.color[0], l.color[1], l.color[2], l.radius,
                l.intensity, l.kind, l.dir[0], l.dir[1], l.dir[2], l.size[0], l.size[1], l.spread);
  out << b;
}

void Save() {
  if (!g_key) return;
  std::error_code ec;
  std::filesystem::create_directories(g_folder, ec);
  std::ofstream out(FileFor(g_key));
  out << "# Amped 3 Revamped night lights. A light a line: x y z  r g b  radius intensity  "
         "kind dx dy dz  width height spread\n"
         "# (kind: 0 lamp, 1 floodlight, 2 window or panel, 3 campfire, 4 street light, 5 caged light;\n"
         "#  d: its mounting or aim; width height: a window or panel's (m); spread: half its cone,\n"
         "#  degrees, 0 the kind's own)\n"
         "# T mesh  lx ly lz  r g b  radius intensity  kind ldx ldy ldz: one on every copy of an object\n"
         "# A template  x y z ...: found on a copy so far\n";
  for (const Light& l : g_s.lights) WriteLight(out, l);
  for (const Template& t : g_s.templates) {
    char b[260];
    std::snprintf(b, sizeof(b),
                  "T %016llx  %.3f %.3f %.3f  %.3f %.3f %.3f  %.2f %.2f  %d %.3f %.3f %.3f  %.2f %.2f %.1f\n",
                  (unsigned long long)t.mesh, t.local[0], t.local[1], t.local[2], t.light.color[0],
                  t.light.color[1], t.light.color[2], t.light.radius, t.light.intensity,
                  t.light.kind, t.local_dir[0], t.local_dir[1], t.local_dir[2], t.light.size[0],
                  t.light.size[1], t.light.spread);
    out << b;
  }
  for (const Found& f : g_s.found) {
    if (f.moving) continue;
    out << "A " << f.tmpl << "  ";
    WriteLight(out, f.light);
  }
  g_dirty = false;
  g_last_save = Clock::now();
}

void Changed() {
  g_dirty = true;
  Save();
}

void Remember() {
  g_undo.push_back(g_s);
  if (g_undo.size() > 50) g_undo.erase(g_undo.begin());
}

const Light* Picked() {
  if (!g_pick.on) return nullptr;
  if (!g_pick.every) {
    return g_pick.index >= 0 && g_pick.index < int(g_s.lights.size()) ? &g_s.lights[g_pick.index]
                                                                       : nullptr;
  }
  // The copy nearest where it was picked (it may have moved a little).
  const Light* best = nullptr;
  float best_d = 4.0f;
  for (const Found& f : g_s.found) {
    if (f.tmpl != g_pick.index) continue;
    const float* q = f.light.pos;
    const float d = std::sqrt((q[0] - g_pick.pos[0]) * (q[0] - g_pick.pos[0]) +
                              (q[1] - g_pick.pos[1]) * (q[1] - g_pick.pos[1]) +
                              (q[2] - g_pick.pos[2]) * (q[2] - g_pick.pos[2]));
    if (d < best_d) {
      best_d = d;
      best = &f.light;
    }
  }
  return best;
}

void Publish() {
  std::vector<Light> all = g_s.lights;
  for (const Found& f : g_s.found) {
    if (f.moving && f.seen != g_instances_version) continue;  // (a moving one out of sight)
    all.push_back(f.light);
  }
  // The picked one pulses: all of its copies when it's on every copy.
  if (g_pick.on) {
    if (!g_pick.every && g_pick.index >= 0 && g_pick.index < int(all.size())) {
      all[g_pick.index].selected = true;
    } else if (g_pick.every) {
      size_t i = g_s.lights.size();
      for (const Found& f : g_s.found) {
        if (f.moving && f.seen != g_instances_version) continue;
        if (f.tmpl == g_pick.index) all[i].selected = true;
        ++i;
      }
    }
  }
  if (g_editor && g_preview_ok && !g_pick.on) {
    Light p = g_preview;
    p.preview = true;
    all.push_back(p);
  }
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
  for (int r = 0; r < 3; ++r) {
    out[r] = float(inv[r * 3] * q[0] + inv[r * 3 + 1] * q[1] + inv[r * 3 + 2] * q[2]);
  }
  return true;
}

// Tilted up (positive) or down in its vertical plane, keeping its length and
// way round (not past straight up or down).
void Tilt(float v[3], float degrees) {
  const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (len < 1e-6f) return;
  float h = std::sqrt(v[0] * v[0] + v[2] * v[2]);
  float ax = 1.0f, az = 0.0f;
  if (h > 1e-5f) {
    ax = v[0] / h;
    az = v[2] / h;
  }
  const float e = std::clamp(std::atan2(v[1], h) + degrees * 3.14159265f / 180.0f, -1.48f, 1.48f);
  h = std::cos(e) * len;
  v[0] = ax * h;
  v[1] = std::sin(e) * len;
  v[2] = az * h;
}

void TurnY(float v[3], float degrees) {
  const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
  const float x = v[0] * c - v[2] * s, z = v[0] * s + v[2] * c;
  v[0] = x;
  v[2] = z;
}

bool Camera(float pos[3], float fwd[3]) {
  float cam[9];
  float fc[6];
  if (live::FreecamShowing() && live::FreecamPose(fc)) {
    std::memcpy(pos, fc, 12);
    std::memcpy(fwd, fc + 3, 12);
    return true;
  }
  if (!live::GameCamera(cam)) return false;
  std::memcpy(pos, cam, 12);
  std::memcpy(fwd, cam + 3, 12);
  return true;
}

// The light the cross points at, mounted on the surface there: out along its
// normal (a bulb on an arm off a wall or tower, on a post off the ground; a
// floodlight on its mount aimed down and out; a window flat on a wall; a
// fire on the ground), turned by the editor's turn.
bool AimedLight(Light* l) {
  float p[3], n[3];
  if (!live::CenterPoint(p, n)) return false;
  const Kind& k = kKinds[g_kind];
  float cpos[3], fwd[3] = {0.0f, 0.0f, 1.0f};
  Camera(cpos, fwd);
  const bool ground = n[1] > 0.7f;
  for (int i = 0; i < 3; ++i) l->color[i] = g_tint >= 0 ? kTints[g_tint].color[i] : k.color[i];
  l->radius = k.radius * g_reach;
  l->intensity = k.intensity * g_bright;
  l->kind = g_kind;
  l->size[0] = g_size[0];
  l->size[1] = g_size[1];
  l->spread = g_spread == 1.0f ? 0.0f : std::clamp(k.spread * g_spread, 3.0f, 180.0f);
  // Level "out": the surface's (a wall), or back toward the camera (the ground).
  float out[3] = {n[0], 0.0f, n[2]};
  float ol = std::sqrt(out[0] * out[0] + out[2] * out[2]);
  if (ground || ol < 0.2f) {
    out[0] = -fwd[0];
    out[2] = -fwd[2];
    ol = std::sqrt(out[0] * out[0] + out[2] * out[2]);
  }
  if (ol > 1e-4f) {
    out[0] /= ol;
    out[2] /= ol;
  } else {
    out[0] = 0.0f;
    out[2] = 1.0f;
  }
  TurnY(out, g_yaw);
  switch (g_kind) {
    case 0: {
      // A bulb: off a wall or tower on an arm (0.4 m), or on a post (2.4 m)
      // off the ground; the arm back to the surface (its length: dir's).
      const float arm = ground ? 2.4f : 0.4f;
      for (int i = 0; i < 3; ++i) l->dir[i] = n[i] * arm;
      if (!ground) Tilt(l->dir, g_pitch);  // (the arm up or down off the wall)
      for (int i = 0; i < 3; ++i) l->pos[i] = p[i] + l->dir[i];
      break;
    }
    case 1: {
      // A floodlight: 0.35 m out (3 m up off the ground), aimed out and
      // 35 degrees down.
      const float off = ground ? 3.0f : 0.35f;
      for (int i = 0; i < 3; ++i) l->pos[i] = p[i] + n[i] * off;
      const float c = std::cos(0.61f), s = std::sin(0.61f);
      l->dir[0] = out[0] * c;
      l->dir[1] = -s;
      l->dir[2] = out[2] * c;
      Tilt(l->dir, g_pitch);
      break;
    }
    case 2:
      // A window: flat on the wall, facing out.
      for (int i = 0; i < 3; ++i) l->pos[i] = p[i] + n[i] * 0.02f;
      l->dir[0] = out[0];
      l->dir[1] = 0.0f;
      l->dir[2] = out[2];
      Tilt(l->dir, g_pitch);  // (a sloping wall, a roof light)
      break;
    case 4: {
      // A street light: a 7.5 m pole standing where aimed, its head on an
      // arm 1.3 m out over the way (toward the camera, turned), lighting
      // down. Its dir: from the pole's foot up to the head.
      l->dir[0] = out[0] * 1.3f;
      l->dir[1] = 7.5f;
      l->dir[2] = out[2] * 1.3f;
      for (int i = 0; i < 3; ++i) l->pos[i] = p[i] + l->dir[i];
      break;
    }
    case 5: {
      // A caged light: on its base, out from the surface (up off the
      // ground, out from a wall), its axis along that (tilted with it).
      for (int i = 0; i < 3; ++i) l->dir[i] = n[i] * 0.16f;
      Tilt(l->dir, g_pitch);
      for (int i = 0; i < 3; ++i) l->pos[i] = p[i] + l->dir[i];
      break;
    }
    default:
      // A campfire: on the ground (its light over the logs).
      std::memcpy(l->pos, p, sizeof(p));
      l->pos[1] += 0.4f;
      l->dir[0] = out[0];
      l->dir[1] = 0.0f;
      l->dir[2] = out[2];
  }
  return true;
}

// Copies of the objects with a light on every copy: new ones found, known
// ones followed (moving objects: lift chairs).
void Discover() {
  if (g_s.templates.empty()) return;
  bool changed = false;
  for (const Instance& in : g_instances) {
    for (int t = 0; t < int(g_s.templates.size()); ++t) {
      const Template& tm = g_s.templates[t];
      if (tm.mesh != in.key) continue;
      float p[3], dir[3];
      Apply(in, tm.local, p);
      ApplyDir(in, tm.local_dir, dir);
      Found* best = nullptr;
      float best_d = 3.0f * 3.0f;  // (a copy moves less than this a frame)
      for (Found& f : g_s.found) {
        if (f.tmpl != t || f.seen == g_instances_version) continue;
        const float dx = f.light.pos[0] - p[0], dy = f.light.pos[1] - p[1], dz = f.light.pos[2] - p[2];
        const float d = dx * dx + dy * dy + dz * dz;
        if (d < best_d) {
          best_d = d;
          best = &f;
        }
      }
      if (best) {
        if (best_d > 0.02f * 0.02f) {
          best->moving = best->moving || best_d > 0.05f * 0.05f;
          std::memcpy(best->light.pos, p, sizeof(p));
          std::memcpy(best->light.dir, dir, sizeof(dir));
          changed = true;
        }
        if (best->seen + 1 != g_instances_version) changed = true;
        best->seen = g_instances_version;
        continue;
      }
      Found f;
      f.tmpl = t;
      f.light = tm.light;
      f.seen = g_instances_version;
      std::memcpy(f.light.pos, p, sizeof(p));
      std::memcpy(f.light.dir, dir, sizeof(dir));
      g_s.found.push_back(f);
      changed = true;
      g_dirty = true;
    }
  }
  // (Moving ones out of sight: not drawn; publish when that changes.)
  for (const Found& f : g_s.found) {
    if (f.moving && f.seen + 1 == g_instances_version) changed = true;
  }
  if (changed) Publish();
}

// The placed lights in picking order: by object (each light alone; every
// copy of one object together), the nearest objects first.
struct Entry {
  bool every;
  int index;  // light, or template
  float pos[3];
  float dist;
};
std::vector<Entry> Order() {
  float cam[3], fwd[3];
  const bool have = Camera(cam, fwd);
  auto dist = [&](const float* p) {
    if (!have) return 0.0f;
    return std::sqrt((p[0] - cam[0]) * (p[0] - cam[0]) + (p[1] - cam[1]) * (p[1] - cam[1]) +
                     (p[2] - cam[2]) * (p[2] - cam[2]));
  };
  std::vector<Entry> groups;
  for (int i = 0; i < int(g_s.lights.size()); ++i) {
    Entry e{false, i, {}, dist(g_s.lights[i].pos)};
    std::memcpy(e.pos, g_s.lights[i].pos, 12);
    groups.push_back(e);
  }
  for (int t = 0; t < int(g_s.templates.size()); ++t) {
    // The template's nearest copy stands for it (its group then each copy
    // by distance).
    std::vector<Entry> copies;
    for (const Found& f : g_s.found) {
      if (f.tmpl != t || (f.moving && f.seen != g_instances_version)) continue;
      Entry e{true, t, {}, dist(f.light.pos)};
      std::memcpy(e.pos, f.light.pos, 12);
      copies.push_back(e);
    }
    std::sort(copies.begin(), copies.end(), [](const Entry& a, const Entry& b) { return a.dist < b.dist; });
    groups.insert(groups.end(), copies.begin(), copies.end());
  }
  // Stable by group's nearest: single lights and template groups.
  std::vector<std::pair<float, int>> key(groups.size());
  for (size_t i = 0; i < groups.size(); ++i) {
    float g = groups[i].dist;
    if (groups[i].every) {
      for (const Entry& e : groups) {
        if (e.every && e.index == groups[i].index) g = std::min(g, e.dist);
      }
    }
    key[i] = {g + (groups[i].every ? 0.001f * float(groups[i].index) : 0.0f), int(i)};
  }
  std::vector<int> idx(groups.size());
  for (size_t i = 0; i < idx.size(); ++i) idx[i] = int(i);
  std::stable_sort(idx.begin(), idx.end(), [&](int a, int b) { return key[a].first < key[b].first; });
  std::vector<Entry> out;
  for (int i : idx) out.push_back(groups[i]);
  return out;
}

std::string Describe() {
  if (!g_pick.on) return "";
  const Light* l = Picked();
  if (!l) return "";
  std::string s = kKinds[std::clamp(l->kind, 0, kKindCount - 1)].name;
  if (g_pick.every) {
    int n = 0;
    for (const Found& f : g_s.found) n += f.tmpl == g_pick.index ? 1 : 0;
    s += " on every copy (" + std::to_string(n) + ")";
  }
  return s;
}

int Total() { return int(g_s.lights.size() + g_s.found.size()); }

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
  if (!g_key) return;
  bool publish = false;
  if (!g_s.templates.empty()) {
    const uint64_t v = live::Instances(&g_instances, g_instances_version);
    if (v != g_instances_version) {
      g_instances_version = v;
      Discover();
    }
  }
  // The preview follows the cross.
  if (g_editor) {
    Light p;
    const bool ok = AimedLight(&p);
    if (ok != g_preview_ok || (ok && std::memcmp(p.pos, g_preview.pos, 12) != 0) ||
        (ok && std::memcmp(p.dir, g_preview.dir, 12) != 0) || (ok && p.kind != g_preview.kind)) {
      g_preview = p;
      g_preview_ok = ok;
      publish = true;
    }
  }
  if (publish) Publish();
  if (g_dirty && Clock::now() - g_last_save > std::chrono::seconds(5)) Save();
  live::SetLightNeeds(g_editor || !g_s.templates.empty(), g_editor);
}

void SetEditor(bool on) {
  std::lock_guard lock(g_mutex);
  if (g_editor == on) return;
  g_editor = on;
  g_preview_ok = false;
  g_pick = Pick{};
  Publish();
}

bool EditorOn() {
  std::lock_guard lock(g_mutex);
  return g_editor;
}

std::string Place() {
  std::lock_guard lock(g_mutex);
  if (!g_key) return "Night lights: no mountain yet";
  Light l;
  if (!AimedLight(&l)) return "Night lights: aim at something (not the sky)";
  Remember();
  g_s.lights.push_back(l);
  Changed();
  Publish();
  return std::string("Placed: ") + kKinds[g_kind].name + " (" + std::to_string(Total()) + " here)";
}

std::string PlaceOnEveryCopy() {
  std::lock_guard lock(g_mutex);
  if (!g_key) return "Night lights: no mountain yet";
  Light l;
  float p[3];
  if (!live::CenterPoint(p) || !AimedLight(&l)) return "Night lights: aim at an object";
  // The object aimed at: of the copies near the point, the one whose own
  // origin (a tower's base, a post's foot, a chair's hanger) is nearest it
  // across, and not far above or below.
  live::Instances(&g_instances, ~0ull);  // (the latest, whatever version)
  const Instance* best = nullptr;
  // What's drawn at the cross (any object, however big, wherever its
  // origin): the renderer's pick; failing that, a guess by origins.
  Instance hit;
  const int picked = live::PickedObject(&hit);
  if (picked == 1) {
    // (The same object in this frame's list, if it's there: its copies.)
    best = &hit;
    for (const Instance& in : g_instances) {
      if (in.key == hit.key && std::memcmp(in.m, hit.m, sizeof(in.m)) == 0) best = &in;
    }
  } else if (picked == 2) {
    if (hit.why == 0) {
      return "Night lights: that's part of the mountain itself, not an object (place one light instead)";
    }
    // Drawn where it stands (its points already in the world: no
    // placement, so no copies to find), or a placement not recognised
    // (logged: "object placement not recognised"): one light there.
    Remember();
    g_s.lights.push_back(l);
    Changed();
    Publish();
    return hit.why == 1
               ? std::string("Placed one ") + kKinds[g_kind].name +
                     ": that object is drawn in place, with no copies of its own"
               : std::string("Placed one ") + kKinds[g_kind].name +
                     ": that object's copies can't be told apart yet (its draw is in the log)";
  }
  float best_s = best ? -1.0f : 1e9f;
  for (const Instance& in : g_instances) {
    if (best_s < 0.0f) break;
    const float dy = p[1] - in.m[7];
    const float dx = p[0] - in.m[3], dz = p[2] - in.m[11];
    const float across = std::sqrt(dx * dx + dz * dz);
    if (across > 10.0f || dy < -6.0f || dy > 45.0f) continue;
    const float score = across + 0.15f * std::fabs(dy);
    if (score < best_s) {
      best_s = score;
      best = &in;
    }
  }
  if (!best) return "Night lights: no object found there (aim at a tower, post, chair or building)";
  Template t;
  t.mesh = best->key;
  t.light = l;
  if (!ToLocal(*best, l.pos, t.local) || !ToLocal(*best, l.dir, t.local_dir, true)) {
    return "Night lights: that object can't take one";
  }
  int copies = 0;
  for (const Instance& in : g_instances) copies += in.key == t.mesh ? 1 : 0;
  Remember();
  g_s.templates.push_back(t);
  Discover();
  Changed();
  Publish();
  return std::string("Placed: ") + kKinds[g_kind].name + " on every copy (" +
         std::to_string(copies) + " in view; more as they come into view)";
}

std::string NextKind(int step) {
  std::lock_guard lock(g_mutex);
  g_kind = ((g_kind + step) % kKindCount + kKindCount) % kKindCount;
  return std::string("Placing: ") + kKinds[g_kind].name;
}

namespace {
// The picked light changed: alone, or on every copy (the template and each
// copy found).
template <typename F>
void EditPicked(F&& f) {
  if (!g_pick.every) {
    if (g_pick.index >= 0 && g_pick.index < int(g_s.lights.size())) f(g_s.lights[g_pick.index]);
    return;
  }
  if (g_pick.index < 0 || g_pick.index >= int(g_s.templates.size())) return;
  f(g_s.templates[g_pick.index].light);
  for (Found& fd : g_s.found) {
    if (fd.tmpl == g_pick.index) f(fd.light);
  }
}
const char* kModeNames[] = {"turn / tilt", "width / height", "reach / spread", "color / brightness"};
std::string Fmt(const char* f, double a, double b = 0.0) {
  char buf[96];
  std::snprintf(buf, sizeof(buf), f, a, b);
  return buf;
}
float SpreadOf(const Light& l) { return l.spread > 0.0f ? l.spread : kKinds[std::clamp(l.kind, 0, kKindCount - 1)].spread; }
}  // namespace

std::string NextMode(int step) {
  std::lock_guard lock(g_mutex);
  g_mode = ((g_mode + step) % 4 + 4) % 4;
  return std::string("D-pad: ") + kModeNames[g_mode];
}

int Mode() {
  std::lock_guard lock(g_mutex);
  return g_mode;
}

std::string Resize(int wider, int taller) {
  std::lock_guard lock(g_mutex);
  const float fw = std::pow(1.04f, float(wider)), fh = std::pow(1.04f, float(taller));
  auto apply = [&](float* sz) {
    sz[0] = std::clamp(sz[0] * fw, 0.1f, 30.0f);
    sz[1] = std::clamp(sz[1] * fh, 0.1f, 30.0f);
  };
  if (!g_pick.on) {
    apply(g_size);
    return Fmt("Panel %.2f x %.2f m", g_size[0], g_size[1]) + (g_kind == 2 ? "" : " (for windows and panels)");
  }
  Remember();
  float shown[2] = {};
  EditPicked([&](Light& l) {
    apply(l.size);
    std::memcpy(shown, l.size, sizeof(shown));
  });
  Changed();
  Publish();
  return Fmt("Panel %.2f x %.2f m", shown[0], shown[1]);
}

std::string Reach(int step) {
  std::lock_guard lock(g_mutex);
  const float f = std::pow(1.2f, float(step));
  if (!g_pick.on) {
    g_reach = std::clamp(g_reach * f, 0.1f, 10.0f);
    return Fmt("Reach %.0f m", kKinds[g_kind].radius * g_reach);
  }
  Remember();
  float r = 0.0f;
  EditPicked([&](Light& l) {
    l.radius = std::clamp(l.radius * f, 1.0f, 150.0f);
    r = l.radius;
  });
  Changed();
  Publish();
  return Fmt("Reach %.0f m", r);
}

std::string Spread(int step) {
  std::lock_guard lock(g_mutex);
  const float f = std::pow(1.15f, float(step));
  if (!g_pick.on) {
    g_spread = std::clamp(g_spread * f, 0.05f, 6.0f);
    const float sp = std::clamp(kKinds[g_kind].spread * g_spread, 3.0f, 180.0f);
    return sp >= 179.5f ? std::string("Spread: all round") : Fmt("Spread %.0f degrees", sp * 2.0f);
  }
  Remember();
  float sp = 0.0f;
  EditPicked([&](Light& l) {
    l.spread = std::clamp(SpreadOf(l) * f, 3.0f, 180.0f);
    sp = l.spread;
  });
  Changed();
  Publish();
  return sp >= 179.5f ? std::string("Spread: all round") : Fmt("Spread %.0f degrees", sp * 2.0f);
}

std::string Brightness(int step) {
  std::lock_guard lock(g_mutex);
  const float f = std::pow(1.2f, float(step));
  if (!g_pick.on) {
    g_bright = std::clamp(g_bright * f, 0.05f, 8.0f);
    return Fmt("Brightness %.0f%%", g_bright * 100.0f);
  }
  Remember();
  float b = 0.0f;
  EditPicked([&](Light& l) {
    l.intensity = std::clamp(l.intensity * f, 0.02f, 12.0f);
    b = l.intensity / std::max(kKinds[std::clamp(l.kind, 0, kKindCount - 1)].intensity, 0.01f);
  });
  Changed();
  Publish();
  return Fmt("Brightness %.0f%%", b * 100.0f);
}

std::string Rotate(float degrees) {
  std::lock_guard lock(g_mutex);
  if (!g_pick.on) {
    g_yaw = std::fmod(g_yaw + degrees, 360.0f);
    return "Turned " + std::to_string(int(std::lround(g_yaw))) + " degrees";
  }
  Remember();
  if (!g_pick.every) {
    Light& l = g_s.lights[g_pick.index];
    if (l.kind == 0 || l.kind == 4 || l.kind == 5) {
      // A bulb: its arm swings round where it's mounted (a street light's
      // head round its pole; a caged light round its base).
      float base[3];
      for (int i = 0; i < 3; ++i) base[i] = l.pos[i] - l.dir[i];
      TurnY(l.dir, degrees);
      for (int i = 0; i < 3; ++i) l.pos[i] = base[i] + l.dir[i];
      std::memcpy(g_pick.pos, l.pos, 12);
    } else {
      TurnY(l.dir, degrees);
    }
  } else {
    // Every copy: turned in the object's own space (about its up).
    Template& t = g_s.templates[g_pick.index];
    if (t.light.kind == 0 || t.light.kind == 4 || t.light.kind == 5) {
      float base[3];
      for (int i = 0; i < 3; ++i) base[i] = t.local[i] - t.local_dir[i];
      TurnY(t.local_dir, degrees);
      for (int i = 0; i < 3; ++i) t.local[i] = base[i] + t.local_dir[i];
    } else {
      TurnY(t.local_dir, degrees);
    }
    // (The copies follow on the next frame: moved to fit.)
    for (Found& f : g_s.found) {
      if (f.tmpl == g_pick.index) f.seen = 0;
    }
    std::vector<Found> keep;
    for (const Found& f : g_s.found) {
      if (f.tmpl != g_pick.index) keep.push_back(f);
    }
    g_s.found.swap(keep);
    Discover();
  }
  Changed();
  Publish();
  return "Turned " + Describe();
}

std::string TiltBy(float degrees) {
  std::lock_guard lock(g_mutex);
  if (!g_pick.on) {
    g_pitch = std::clamp(g_pitch + degrees, -80.0f, 80.0f);
    return "Tilted " + std::to_string(int(std::lround(g_pitch))) + " degrees";
  }
  Remember();
  auto tilt = [&](float pos[3], float dir[3], int kind) {
    if (kind == 3) return;  // (a fire stays level)
    if (kind == 0 || kind == 5) {
      float base[3];
      for (int i = 0; i < 3; ++i) base[i] = pos[i] - dir[i];
      Tilt(dir, degrees);
      for (int i = 0; i < 3; ++i) pos[i] = base[i] + dir[i];
    } else {
      Tilt(dir, degrees);
    }
  };
  if (!g_pick.every) {
    Light& l = g_s.lights[g_pick.index];
    tilt(l.pos, l.dir, l.kind);
    std::memcpy(g_pick.pos, l.pos, 12);
  } else {
    Template& t = g_s.templates[g_pick.index];
    tilt(t.local, t.local_dir, t.light.kind);
    std::vector<Found> keep;
    for (const Found& f : g_s.found) {
      if (f.tmpl != g_pick.index) keep.push_back(f);
    }
    g_s.found.swap(keep);
    Discover();
  }
  Changed();
  Publish();
  return "Tilted " + Describe();
}

std::string NextColor(int step) {
  std::lock_guard lock(g_mutex);
  if (!g_pick.on) {
    // The preview: through the colors, back to the kind's own.
    g_tint = (g_tint + 1 + step + (kTintCount + 1)) % (kTintCount + 1) - 1;
    return std::string("Color: ") + (g_tint < 0 ? std::string(kKinds[g_kind].name) + "'s own" : kTints[g_tint].name);
  }
  Remember();
  // The picked one: the next color after the one it has.
  const Light* now = Picked();
  int at = -1;
  if (now) {
    for (int i = 0; i < kTintCount; ++i) {
      if (std::fabs(kTints[i].color[0] - now->color[0]) + std::fabs(kTints[i].color[1] - now->color[1]) +
              std::fabs(kTints[i].color[2] - now->color[2]) < 0.02f) {
        at = i;
      }
    }
  }
  const int next = ((at + step) % kTintCount + kTintCount) % kTintCount;
  const float* c = kTints[next].color;
  if (!g_pick.every) {
    std::memcpy(g_s.lights[g_pick.index].color, c, 12);
  } else {
    std::memcpy(g_s.templates[g_pick.index].light.color, c, 12);
    for (Found& f : g_s.found) {
      if (f.tmpl == g_pick.index) std::memcpy(f.light.color, c, 12);
    }
  }
  Changed();
  Publish();
  return std::string("Color: ") + kTints[next].name;
}

std::string Select(int step) {
  std::lock_guard lock(g_mutex);
  const std::vector<Entry> order = Order();
  if (order.empty()) return "No lights here yet";
  int at = -1;
  if (g_pick.on) {
    float best = 4.0f;
    for (int i = 0; i < int(order.size()); ++i) {
      if (order[i].every != g_pick.every || order[i].index != g_pick.index) continue;
      const float* q = order[i].pos;
      const float d = std::sqrt((q[0] - g_pick.pos[0]) * (q[0] - g_pick.pos[0]) +
                                (q[1] - g_pick.pos[1]) * (q[1] - g_pick.pos[1]) +
                                (q[2] - g_pick.pos[2]) * (q[2] - g_pick.pos[2]));
      if (d < best) {
        best = d;
        at = i;
      }
    }
  }
  int next;
  if (at < 0) {
    next = step > 0 ? 0 : int(order.size()) - 1;
  } else {
    // By object: the next object's first light (every copy of one counts as
    // one object: its copies are skipped over).
    next = at;
    const int n = int(order.size());
    for (int i = 0; i < n; ++i) {
      next = ((next + step) % n + n) % n;
      if (!(order[next].every && order[at].every && order[next].index == order[at].index)) break;
    }
  }
  const Entry& e = order[next];
  g_pick.on = true;
  g_pick.every = e.every;
  g_pick.index = e.index;
  std::memcpy(g_pick.pos, e.pos, 12);
  Publish();
  int obj = 0, objects = 0;
  {
    std::vector<std::pair<bool, int>> seen;
    for (int i = 0; i < int(order.size()); ++i) {
      const std::pair<bool, int> id{order[i].every, order[i].every ? order[i].index : -1 - i};
      if (std::find(seen.begin(), seen.end(), id) == seen.end()) {
        seen.push_back(id);
        if (i <= next) obj = int(seen.size());
      }
    }
    objects = int(seen.size());
  }
  char b[64];
  std::snprintf(b, sizeof(b), " (%d of %d, %.0f m away)", obj, objects, double(e.dist));
  return "Picked: " + Describe() + b;
}

bool HasSelection() {
  std::lock_guard lock(g_mutex);
  return g_pick.on;
}

std::string Deselect() {
  std::lock_guard lock(g_mutex);
  g_pick = Pick{};
  Publish();
  return "Nothing picked";
}

std::string DeleteSelected() {
  std::lock_guard lock(g_mutex);
  if (!g_pick.on) return "Pick a light first (LB / RB)";
  const std::string what = Describe();
  Remember();
  if (!g_pick.every) {
    if (g_pick.index >= 0 && g_pick.index < int(g_s.lights.size())) {
      g_s.lights.erase(g_s.lights.begin() + g_pick.index);
    }
  } else {
    const int t = g_pick.index;
    std::vector<Found> keep;
    for (const Found& f : g_s.found) {
      if (f.tmpl == t) continue;
      Found k = f;
      if (k.tmpl > t) --k.tmpl;
      keep.push_back(k);
    }
    g_s.found.swap(keep);
    g_s.templates.erase(g_s.templates.begin() + t);
  }
  g_pick = Pick{};
  Changed();
  Publish();
  return "Deleted: " + what + " (" + std::to_string(Total()) + " left)";
}

std::string Undo() {
  std::lock_guard lock(g_mutex);
  if (g_undo.empty()) return "Nothing to undo";
  g_s = g_undo.back();
  g_undo.pop_back();
  g_pick = Pick{};
  Changed();
  Publish();
  return "Undone (" + std::to_string(Total()) + " lights here)";
}

std::string KindName() {
  std::lock_guard lock(g_mutex);
  return kKinds[g_kind].name;
}

std::string PreviewDetails() {
  std::lock_guard lock(g_mutex);
  char b[200];
  const Kind& k = kKinds[g_kind];
  const float sp = std::clamp(k.spread * g_spread, 3.0f, 180.0f);
  std::snprintf(b, sizeof(b), "%s, turned %d, tilted %d, reach %.0f m, %s%s",
                g_tint < 0 ? "its own color" : kTints[g_tint].name, int(std::lround(g_yaw)),
                int(std::lround(g_pitch)), double(k.radius * g_reach),
                sp >= 179.5f ? std::string("all round").c_str() : (std::to_string(int(sp * 2.0f)) + " deg").c_str(),
                g_kind == 2 ? (", " + Fmt("%.2f x %.2f m", g_size[0], g_size[1])).c_str() : "");
  return b;
}

std::string SelectionName() {
  std::lock_guard lock(g_mutex);
  return Describe();
}

int Count() {
  std::lock_guard lock(g_mutex);
  return Total();
}

}  // namespace amped3::night_lights
