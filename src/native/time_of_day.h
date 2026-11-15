// amped3 native renderer - time of day: where the sun and moon are and what
// their light, and the sky's, look like at a given hour, relative to how the
// mountain was made (its own fixed sun).
//
// Each mountain has one sun, fixed: the lighting pass's c14 (toward the sun)
// with its color in c39..c42 and the sky's light (spherical harmonics) in
// c15..c38. The day here is laid out so that the mountain's own sun is where
// the sun is at one hour of it (the mountain's hour, an afternoon one): at
// that hour everything looks as the game made it, and every other hour is
// that look scaled: the sun's light by how much air it comes through (redder
// low down, gone at night), the sky's by how bright the sky is, bluer at dusk;
// the moon's light at night. Header only (the renderer and its tests).

#pragma once

#include <algorithm>
#include <cmath>

namespace amped3::native::tod {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;

// The day's layout: a latitude like the mountains' (Utah, Oregon, ~40 N) in
// winter (the sun's declination).
constexpr double kLatitude = 40.0 * kDeg;
constexpr double kDeclination = -15.0 * kDeg;

struct Vec3 {
  double x = 0, y = 0, z = 0;
};

inline Vec3 Normalize(Vec3 v) {
  const double n = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
  if (!(n > 1e-12)) return {0, 1, 0};
  return {v.x / n, v.y / n, v.z / n};
}

inline double SmoothStep(double e0, double e1, double x) {
  const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
  return t * t * (3.0 - 2.0 * t);
}

// Direction toward a body at hour angle h (radians, 0 at noon, negative in
// the morning) and declination dec, in local axes: north, east, up.
inline Vec3 Local(double h, double dec) {
  const double sl = std::sin(kLatitude), cl = std::cos(kLatitude);
  const double sd = std::sin(dec), cd = std::cos(dec);
  const double up = sl * sd + cl * cd * std::cos(h);
  const double east = -cd * std::sin(h);  // morning (h < 0): east
  const double north = cl * sd - sl * cd * std::cos(h);
  return Normalize({north, east, up});
}

// How much light comes through the air at elevation e (radians), per color
// channel (relative air mass, Kasten & Young, through a thin mountain
// atmosphere).
inline Vec3 Transmittance(double e) {
  const double ed = std::max(e / kDeg, -2.0);
  const double m = 1.0 / (std::sin(std::max(e, -2.0 * kDeg)) + 0.50572 * std::pow(ed + 6.07995, -1.6364));
  const double air = std::min(m, 45.0);
  return {std::exp(-0.045 * air), std::exp(-0.085 * air), std::exp(-0.18 * air)};
}

// The sun's disc going below the horizon (its light fading out).
inline double SunUp(double e) { return SmoothStep(-2.5 * kDeg, 2.0 * kDeg, e); }

// How bright the sky's light is (relative; 1 well into the day).
inline double SkyLight(double e) {
  const double day = 0.55 + 0.45 * SmoothStep(0.0, 40.0 * kDeg, e);
  return day * SmoothStep(-14.0 * kDeg, 2.0 * kDeg, e);
}

struct State {
  bool valid = false;
  double hour = 12.0;          // this one (0..24)
  double mountain_hour = 15.0; // where the mountain's own sun is
  Vec3 sun, moon;              // toward them (world)
  double sun_elevation = 0.0, moon_elevation = 0.0;  // radians
  // The lighting: toward the light (the sun, or the moon at night) and its
  // color relative to the mountain's sun; the sky's light relative to the
  // mountain's.
  Vec3 light;
  Vec3 light_scale;   // times c39..c42
  Vec3 ambient_scale; // times c15..c38
  // The sky (for drawing it): how night it is (0 day .. 1 dark), the stars'
  // strength, the moon's brightness (phase), how much the sun's disc shows.
  double night = 0.0, stars = 0.0, moon_bright = 0.0, sun_disc = 0.0;
  Vec3 sun_color;     // the sun's disc and glow (absolute, 0..1)
  Vec3 moon_color;
  Vec3 zenith, horizon, glow;  // the sky's colors (absolute, display), and toward the sun
  Vec3 mountain_zenith, mountain_horizon, mountain_glow;  // at the mountain's hour
  // How far the sun is from the mountain's own (0 there .. 1 well away):
  // what the game's own fixed shadows are faded out by.
  double away = 0.0;
};

// The world's axes for the day (north and east, level; up is +y), turned so
// the sun at the mountain's hour is where the mountain's sun is.
struct Layout {
  bool valid = false;
  Vec3 north, east;
  double mountain_hour = 15.0;
  Vec3 mountain_sun;
};

inline Vec3 ToWorld(const Layout& l, Vec3 v) {
  return {l.north.x * v.x + l.east.x * v.y, v.z, l.north.z * v.x + l.east.z * v.y};
}

inline Layout MakeLayout(Vec3 mountain_sun) {
  Layout l;
  mountain_sun = Normalize(mountain_sun);
  l.mountain_sun = mountain_sun;
  const double e0 = std::asin(std::clamp(mountain_sun.y, -1.0, 1.0));
  // The afternoon hour the sun is that high (noon if it's higher than the
  // day's highest, a little after sunset if below the horizon).
  const double sl = std::sin(kLatitude), cl = std::cos(kLatitude);
  const double sd = std::sin(kDeclination), cd = std::cos(kDeclination);
  const double ch = std::clamp((std::sin(std::max(e0, -1.0 * kDeg)) - sl * sd) / (cl * cd), -1.0, 1.0);
  const double h0 = std::acos(ch);
  l.mountain_hour = 12.0 + h0 / kDeg / 15.0;
  // Turned about up: the local sun's level direction onto the mountain's.
  const Vec3 v = Local(h0, kDeclination);
  const double lh = std::hypot(v.x, v.y), wh = std::hypot(mountain_sun.x, mountain_sun.z);
  if (!(wh > 1e-6) || !(lh > 1e-6)) {
    l.north = {1, 0, 0};
    l.east = {0, 0, 1};
  } else {
    // world level (x, z) = north_w * n + east_w * e, with north_w, east_w
    // level and at right angles: the angle that takes (n, e) to (x, z).
    const double a_local = std::atan2(v.y, v.x);
    const double a_world = std::atan2(mountain_sun.z, mountain_sun.x);
    const double t = a_world - a_local;
    l.north = {std::cos(t), 0, std::sin(t)};
    l.east = {-std::sin(t), 0, std::cos(t)};
  }
  l.valid = true;
  return l;
}

// The sky's colors for a sun elevation e (display values, 0..1).
inline void SkyColors(double e, double moon_light, Vec3* zenith, Vec3* horizon, Vec3* glow) {
  const double day = SmoothStep(-6.0 * kDeg, 8.0 * kDeg, e);
  const double dusk = SmoothStep(-14.0 * kDeg, -2.0 * kDeg, e) * (1.0 - SmoothStep(4.0 * kDeg, 18.0 * kDeg, e));
  const double high = SmoothStep(8.0 * kDeg, 40.0 * kDeg, e);
  auto mix = [](Vec3 a, Vec3 b, double t) {
    return Vec3{a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t};
  };
  const Vec3 night_z{0.006 + 0.02 * moon_light, 0.010 + 0.03 * moon_light, 0.025 + 0.06 * moon_light};
  const Vec3 night_h{0.015 + 0.04 * moon_light, 0.022 + 0.05 * moon_light, 0.040 + 0.08 * moon_light};
  const Vec3 day_z = mix(Vec3{0.30, 0.48, 0.78}, Vec3{0.20, 0.40, 0.78}, high);
  const Vec3 day_h = mix(Vec3{0.78, 0.78, 0.80}, Vec3{0.70, 0.80, 0.92}, high);
  Vec3 z = mix(night_z, day_z, day);
  Vec3 h = mix(night_h, day_h, day);
  // Dusk: the horizon warm and the zenith deep blue.
  const Vec3 dusk_h{0.95, 0.55, 0.32};
  h = mix(h, dusk_h, dusk * 0.75);
  z = mix(z, Vec3{0.16, 0.20, 0.42}, dusk * 0.4);
  *zenith = z;
  *horizon = h;
  const Vec3 day_glow{1.0, 0.95, 0.85};
  const Vec3 low_glow{1.0, 0.55, 0.25};
  *glow = mix(low_glow, day_glow, high);
}

// At `hour` (0..24); moon_phase 0 new .. 0.5 full .. 1 new; night_light:
// how much light the night has (1: moonlit snow, enough to ride by).
inline State At(const Layout& l, double hour, double moon_phase, double night_light = 1.0) {
  State s;
  if (!l.valid) return s;
  s.valid = true;
  s.hour = hour;
  s.mountain_hour = l.mountain_hour;
  const double h = (hour - 12.0) * 15.0 * kDeg;
  const Vec3 sun_l = Local(h, kDeclination);
  s.sun = Normalize(ToWorld(l, sun_l));
  const double mh = h - moon_phase * 2.0 * kPi;
  const double mdec = kDeclination * std::cos(moon_phase * 2.0 * kPi);
  s.moon = Normalize(ToWorld(l, Local(mh, mdec)));
  const double e = std::asin(std::clamp(s.sun.y, -1.0, 1.0));
  const double me = std::asin(std::clamp(s.moon.y, -1.0, 1.0));
  const double e0 = std::asin(std::clamp(l.mountain_sun.y, -1.0, 1.0));
  s.sun_elevation = e;
  s.moon_elevation = me;
  s.moon_bright = 0.5 - 0.5 * std::cos(moon_phase * 2.0 * kPi);
  // The sun's light, relative to the mountain's.
  const Vec3 t = Transmittance(e), t0 = Transmittance(e0);
  const double up = SunUp(e), up0 = std::max(SunUp(e0), 0.05);
  auto ratio = [](double a, double b) { return std::min(a / std::max(b, 1e-3), 1.15); };
  Vec3 sun_scale{ratio(t.x * up, t0.x * up0), ratio(t.y * up, t0.y * up0), ratio(t.z * up, t0.z * up0)};
  // The moon's: faint and blue, coming in once the sun is well down.
  const double dark = SmoothStep(-3.0 * kDeg, -10.0 * kDeg, e);
  const double moon_up = SmoothStep(0.0, 8.0 * kDeg, me);
  const double moon_k = 0.13 * night_light * (0.25 + 0.75 * s.moon_bright) * moon_up * dark;
  const Vec3 moon_scale{moon_k * 0.62, moon_k * 0.74, moon_k * 1.0};
  if (e > -3.0 * kDeg || moon_up <= 0.0) {
    s.light = s.sun;
    s.light_scale = sun_scale;
  } else {
    s.light = s.moon;
    s.light_scale = moon_scale;
  }
  // The sky's light, relative to the mountain's: bluer at dusk; at night a
  // little (starlight, more with the moon up).
  const double sky = SkyLight(e), sky0 = std::max(SkyLight(e0), 0.05);
  const double twilight = 1.0 - SmoothStep(0.0, 15.0 * kDeg, e);
  const Vec3 tint{1.0 - 0.22 * twilight, 1.0 - 0.1 * twilight, 1.0 + 0.1 * twilight};
  const double floor_k = (0.12 + 0.1 * s.moon_bright * moon_up) * night_light;
  const double sky_k = std::min(sky / sky0, 1.1);
  s.ambient_scale = {sky_k * tint.x + floor_k * 0.55 * dark, sky_k * tint.y + floor_k * 0.7 * dark,
                     sky_k * tint.z + floor_k * 1.0 * dark};
  // The sky.
  s.night = 1.0 - SmoothStep(-12.0 * kDeg, 0.0, e);
  s.stars = SmoothStep(-4.0 * kDeg, -14.0 * kDeg, e);
  s.sun_disc = SunUp(e + 1.0 * kDeg);
  const Vec3 ts = Transmittance(std::max(e, 0.5 * kDeg));
  const double sm = std::max({ts.x, ts.y, ts.z});
  s.sun_color = {ts.x / sm, ts.y / sm, ts.z / sm};
  s.moon_color = {0.85, 0.88, 0.95};
  const double moon_light = s.moon_bright * moon_up;
  SkyColors(e, moon_light, &s.zenith, &s.horizon, &s.glow);
  SkyColors(e0, 0.0, &s.mountain_zenith, &s.mountain_horizon, &s.mountain_glow);
  // The sky's light takes the sky's color too: bluer by day than the
  // mountain's (often warm) hour, deep blue at night (its hue only, part way).
  {
    auto lum = [](const Vec3& v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
    const Vec3 a{s.zenith.x * 0.85 + s.horizon.x * 0.15, s.zenith.y * 0.85 + s.horizon.y * 0.15,
                 s.zenith.z * 0.85 + s.horizon.z * 0.15};
    const Vec3 b{s.mountain_zenith.x * 0.85 + s.mountain_horizon.x * 0.15,
                 s.mountain_zenith.y * 0.85 + s.mountain_horizon.y * 0.15,
                 s.mountain_zenith.z * 0.85 + s.mountain_horizon.z * 0.15};
    const double la = std::max(lum(a), 1e-4), lb = std::max(lum(b), 1e-4);
    auto hue = [&](double x, double y) {
      return std::pow(std::clamp((x / la) / std::max(y / lb, 1e-4), 0.5, 2.0), 0.45);
    };
    const Vec3 h{hue(a.x, b.x), hue(a.y, b.y), hue(a.z, b.z)};
    const double hl = lum(h);
    s.ambient_scale.x *= h.x / hl;
    s.ambient_scale.y *= h.y / hl;
    s.ambient_scale.z *= h.z / hl;
  }
  const double c = s.sun.x * l.mountain_sun.x + s.sun.y * l.mountain_sun.y + s.sun.z * l.mountain_sun.z;
  const double ang = std::acos(std::clamp(c, -1.0, 1.0));
  s.away = std::max(SmoothStep(8.0 * kDeg, 20.0 * kDeg, ang), 1.0 - up);
  return s;
}

}  // namespace amped3::native::tod
