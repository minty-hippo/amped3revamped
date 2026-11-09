// amped3 - the subtitle file (subtitles/amped3.subtitles in the install
// folder): written by tools/subtitles (amped3-subtitles, the installer's
// "Subtitles" step), read by the game (subtitles.cpp).
//
// Nothing in it comes with the project: the tool writes it on the player's PC
// by listening to their own copy of the game (offline speech recognition,
// whisper.cpp), so every player's file is made from their own disc.
//
// Lines of text (UTF-8):
//
//   amped3-subtitles 1
//   # comments
//   clip <kind> <duration ms> <bytes> <keys> <source>
//   <start ms> <end ms> <text>
//   ...
//
// A clip is one sound the game plays (a wave in a wave bank, an XMA file):
//   kind      speech (dialogue anywhere) or video (a movie's sound track)
//   bytes     the size of its XMA data
//   keys      where it's recognised while it plays: comma-separated
//             <length>:<hash>, the FNV-1a 64-bit hash (16 hex digits) of the
//             first <length> bytes of its XMA data (2048, 4096, 8192 and
//             16384, and the whole size when smaller)
//   source    where it came from (the rest of the line; for the log only)
// and the lines after it, until the next clip, are its subtitles: times from
// the start of the sound.
//
// The game sees which XMA data each of the audio hardware's decoder contexts
// is reading (guest memory), hashes the start of it and so knows which clip
// is playing and how far into it.

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace amped3::subtitle_file {

inline constexpr const char* kFileName = "amped3.subtitles";
inline constexpr const char* kMagic = "amped3-subtitles 1";
inline constexpr uint32_t kKeyLengths[] = {2048, 4096, 8192, 16384};
inline constexpr uint32_t kMaxKeyLength = 16384;

inline uint64_t Fnv1a64(const uint8_t* p, size_t n) {
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) {
    h ^= p[i];
    h *= 0x100000001B3ull;
  }
  return h;
}

struct Key {
  uint32_t length = 0;
  uint64_t hash = 0;
};

// The keys of XMA data of `size` bytes starting at `p`.
inline std::vector<Key> KeysOf(const uint8_t* p, size_t size) {
  std::vector<Key> keys;
  for (uint32_t n : kKeyLengths) {
    if (n <= size) keys.push_back({n, Fnv1a64(p, n)});
  }
  if (size < kMaxKeyLength && size > 0) {
    bool have = false;
    for (const Key& k : keys) have |= k.length == size;
    if (!have) keys.push_back({uint32_t(size), Fnv1a64(p, size)});
  }
  return keys;
}

struct Line {
  uint32_t start_ms = 0;
  uint32_t end_ms = 0;
  std::string text;
};

struct Clip {
  bool video = false;
  uint32_t duration_ms = 0;
  uint32_t bytes = 0;
  std::vector<Key> keys;
  std::string source;
  std::vector<Line> lines;
};

inline std::string FormatKeys(const std::vector<Key>& keys) {
  std::string s;
  char buf[40];
  for (const Key& k : keys) {
    std::snprintf(buf, sizeof(buf), "%s%u:%016llx", s.empty() ? "" : ",", k.length,
                  static_cast<unsigned long long>(k.hash));
    s += buf;
  }
  return s;
}

// Parses the file's text; false (with `error`) if it isn't one.
inline bool Parse(const std::string& text, std::vector<Clip>* clips, std::string* error) {
  clips->clear();
  size_t pos = 0;
  bool first = true;
  while (pos < text.size()) {
    size_t end = text.find('\n', pos);
    if (end == std::string::npos) end = text.size();
    std::string line = text.substr(pos, end - pos);
    pos = end + 1;
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (first) {
      first = false;
      if (line != kMagic) {
        if (error) *error = "not a subtitle file (or another version)";
        return false;
      }
      continue;
    }
    if (line.empty() || line[0] == '#') continue;
    if (line.compare(0, 5, "clip ") == 0) {
      Clip c;
      char kind[16] = {};
      unsigned duration = 0, bytes = 0;
      int used = 0;
      if (std::sscanf(line.c_str() + 5, "%15s %u %u %n", kind, &duration, &bytes, &used) < 3) {
        continue;
      }
      c.video = std::string(kind) == "video";
      c.duration_ms = duration;
      c.bytes = bytes;
      std::string rest = line.substr(5 + used);
      const size_t space = rest.find(' ');
      const std::string keys = rest.substr(0, space);
      c.source = space == std::string::npos ? std::string() : rest.substr(space + 1);
      size_t k = 0;
      while (k < keys.size()) {
        size_t comma = keys.find(',', k);
        if (comma == std::string::npos) comma = keys.size();
        const std::string one = keys.substr(k, comma - k);
        const size_t colon = one.find(':');
        if (colon != std::string::npos) {
          Key key;
          key.length = uint32_t(std::strtoul(one.substr(0, colon).c_str(), nullptr, 10));
          key.hash = std::strtoull(one.substr(colon + 1).c_str(), nullptr, 16);
          if (key.length) c.keys.push_back(key);
        }
        k = comma + 1;
      }
      clips->push_back(std::move(c));
      continue;
    }
    if (clips->empty()) continue;
    unsigned a = 0, b = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%u %u %n", &a, &b, &used) >= 2 && used > 0) {
      Line l;
      l.start_ms = a;
      l.end_ms = b;
      l.text = line.substr(used);
      if (!l.text.empty()) clips->back().lines.push_back(std::move(l));
    }
  }
  if (first) {
    if (error) *error = "empty file";
    return false;
  }
  return true;
}

}  // namespace amped3::subtitle_file
