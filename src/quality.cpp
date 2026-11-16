// amped3 - graphics quality presets and per-part levels (quality.h).

#include "quality.h"

#include <cmath>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(native_quality, 2, "GPU",
                     "Graphics quality preset: sets each part of the lighting below (-1: "
                     "custom, 0: low, 1: medium, 2: high, 3: ultra)")
    .range(-1, 3);
REXCVAR_DEFINE_INT32(native_quality_sun_shadows, 2, "GPU",
                     "Sun shadows: 0 the game's own painted shadows; 1 low (the world's shadows "
                     "baked as you ride, riders' and objects' at the game's resolution); 2 "
                     "medium (twice that, and contact shadows); 3 high (the world's at four "
                     "times, riders' and objects' sharper and reaching farther). -1: custom")
    .range(-1, 3);
REXCVAR_DEFINE_INT32(native_quality_lamp_shadows, 2, "GPU",
                     "Night light shadows: 0 off; 1 low (the two nearest lamps at each spot); "
                     "2 medium (every lamp); 3 high (every lamp, finer). -1: custom")
    .range(-1, 3);
REXCVAR_DEFINE_INT32(native_quality_atmosphere, 3, "GPU",
                     "Atmosphere: 0 distance fog only; 1 low (fog and screen-space god rays); 2 "
                     "medium (volumetric light, quarter size); 3 high (volumetric, half size); "
                     "4 ultra (volumetric, half size, finer steps). -1: custom")
    .range(-1, 4);
REXCVAR_DEFINE_INT32(native_quality_ambient_occlusion, 1, "GPU",
                     "Ambient occlusion: 0 off, 1 on. -1: custom")
    .range(-1, 1);
REXCVAR_DEFINE_INT32(native_quality_surface, 1, "GPU",
                     "Surface detail: 0 low (no settling snow, snow tracks, cloud shadows or sun "
                     "sheen); 1 high (all four). -1: custom")
    .range(-1, 1);

namespace amped3::quality {
namespace {

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
double Num(const char* name) {
  const std::string v = Get(name);
  if (v == "true") return 1.0;
  if (v == "false") return 0.0;
  try {
    return std::stod(v);
  } catch (...) {
    return 0.0;
  }
}
void Set(const char* name, const std::string& value) {
  if (Get(name) != value) rex::cvar::SetFlagByName(name, value);
}
void SetNum(const char* name, double v) {
  // (Integers without decimals.)
  if (std::fabs(v - std::round(v)) < 1e-9) {
    Set(name, std::to_string(long(std::lround(v))));
  } else {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.3f", v);
    Set(name, buf);
  }
}
void SetBool(const char* name, bool v) { Set(name, v ? "true" : "false"); }
bool Near(double a, double b) { return std::fabs(a - b) < 1e-3; }

// A part: how to set each level, and which level the settings match (-1:
// none).
struct Part {
  const char* cvar;
  int levels;
  void (*set)(int level);
  int (*match)();
};

// Sun shadows.
struct SunLevel {
  int world, res, soft, dist, contact;
};
constexpr SunLevel kSun[4] = {
    {0, 100, 0, 0, 0}, {2, 100, 100, 0, 0}, {2, 200, 100, 0, 1}, {4, 300, 100, 80, 1}};
void SetSun(int l) {
  SetNum("native_world_shadows", kSun[l].world);
  SetNum("native_shadow_resolution", kSun[l].res);
  SetNum("native_shadow_softness", kSun[l].soft);
  SetNum("native_shadow_distance", kSun[l].dist);
  SetNum("native_contact_shadows", kSun[l].contact);
}
int MatchSun() {
  for (int l = 0; l < 4; ++l) {
    if (Near(Num("native_world_shadows"), kSun[l].world) && Near(Num("native_shadow_resolution"), kSun[l].res) &&
        Near(Num("native_shadow_softness"), kSun[l].soft) && Near(Num("native_shadow_distance"), kSun[l].dist) &&
        Near(Num("native_contact_shadows"), kSun[l].contact)) {
      return l;
    }
  }
  return -1;
}

// Lamp shadows: steps (0 off, 1 on, 2 fine) and how many lamps (0: all).
constexpr int kLamp[4][2] = {{0, 0}, {1, 2}, {1, 0}, {2, 0}};
void SetLamp(int l) {
  SetNum("native_night_light_shadows", kLamp[l][0]);
  SetNum("native_night_light_shadow_lamps", kLamp[l][1]);
}
int MatchLamp() {
  const double s = Num("native_night_light_shadows"), n = Num("native_night_light_shadow_lamps");
  if (Near(s, 0)) return 0;
  for (int l = 1; l < 4; ++l) {
    if (Near(s, kLamp[l][0]) && Near(n, kLamp[l][1])) return l;
  }
  return -1;
}

// The atmosphere.
void SetAtmosphere(int l) {
  SetBool("native_fog", true);
  SetBool("native_god_rays", l == 1);
  if (l >= 2) {
    if (Num("native_volumetric") <= 0.0) SetNum("native_volumetric", 1.0);
    SetNum("native_volumetric_quality", l - 2);
  } else {
    SetNum("native_volumetric", 0.0);
  }
}
int MatchAtmosphere() {
  const bool fog = Num("native_fog") > 0.5, rays = Num("native_god_rays") > 0.5;
  const double vol = Num("native_volumetric");
  if (!fog) return -1;
  if (vol <= 0.0) return rays ? 1 : 0;
  const int q = int(std::lround(Num("native_volumetric_quality")));
  return q >= 0 && q <= 2 ? 2 + q : -1;
}

void SetAo(int l) { SetBool("native_ambient_occlusion", l == 1); }
int MatchAo() { return Num("native_ambient_occlusion") > 0.5 ? 1 : 0; }

void SetSurface(int l) {
  SetBool("native_snow_cover", l == 1);
  if (l == 0) {
    SetNum("native_snow_tracks", 0.0);
    SetNum("native_cloud_shadows", 0.0);
    SetNum("native_sun_sheen", 0.0);
    SetBool("native_drifting_snow", false);
  } else {
    SetBool("native_drifting_snow", true);
    if (Num("native_snow_tracks") <= 0.0) SetNum("native_snow_tracks", 1.0);
    if (Num("native_cloud_shadows") <= 0.0) SetNum("native_cloud_shadows", 0.5);
    if (Num("native_sun_sheen") <= 0.0) SetNum("native_sun_sheen", 1.0);
  }
}
int MatchSurface() {
  const bool cover = Num("native_snow_cover") > 0.5, tracks = Num("native_snow_tracks") > 0.0,
             clouds = Num("native_cloud_shadows") > 0.0, sheen = Num("native_sun_sheen") > 0.0,
             drift = Num("native_drifting_snow") > 0.5;
  if (!cover && !tracks && !clouds && !sheen && !drift) return 0;
  if (cover && tracks && clouds && sheen && drift) return 1;
  return -1;
}

const Part kParts[] = {
    {"native_quality_sun_shadows", 4, SetSun, MatchSun},
    {"native_quality_lamp_shadows", 4, SetLamp, MatchLamp},
    {"native_quality_atmosphere", 5, SetAtmosphere, MatchAtmosphere},
    {"native_quality_ambient_occlusion", 2, SetAo, MatchAo},
    {"native_quality_surface", 2, SetSurface, MatchSurface},
};
constexpr int kPartCount = sizeof(kParts) / sizeof(kParts[0]);

// The presets: each part's level.
constexpr int kPresets[4][kPartCount] = {
    {1, 0, 0, 0, 0},  // low
    {2, 1, 1, 1, 1},  // medium
    {2, 2, 3, 1, 1},  // high
    {3, 3, 4, 1, 1},  // ultra
};

int Level(const char* cvar) { return int(std::lround(Num(cvar))); }

}  // namespace

void Apply() {
  // What was last seen (or written): a change from that is the player's.
  static bool first = true;
  static int seen_preset = -2;
  static int seen[kPartCount];
  if (first) {
    first = false;
    // (As loaded: the detailed settings are what count; the parts and the
    // preset just show what they match.)
    seen_preset = Level("native_quality");
    for (int i = 0; i < kPartCount; ++i) seen[i] = Level(kParts[i].cvar);
  }
  const int preset = Level("native_quality");
  if (preset != seen_preset && preset >= 0 && preset < 4) {
    for (int i = 0; i < kPartCount; ++i) SetNum(kParts[i].cvar, kPresets[preset][i]);
    REXLOG_INFO("Graphics quality: preset {}", preset);
  }
  // A part changed: what's under it.
  for (int i = 0; i < kPartCount; ++i) {
    const int l = Level(kParts[i].cvar);
    if (l != seen[i] && l >= 0 && l < kParts[i].levels) kParts[i].set(l);
  }
  // Each shows what its settings match; the preset, what the parts do.
  int levels[kPartCount];
  for (int i = 0; i < kPartCount; ++i) {
    levels[i] = kParts[i].match();
    SetNum(kParts[i].cvar, levels[i]);
    seen[i] = levels[i];
  }
  int p = -1;
  for (int k = 0; k < 4 && p < 0; ++k) {
    bool all = true;
    for (int i = 0; i < kPartCount; ++i) all = all && levels[i] == kPresets[k][i];
    if (all) p = k;
  }
  SetNum("native_quality", p);
  seen_preset = p;
}

}  // namespace amped3::quality
