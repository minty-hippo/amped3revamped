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
//   amped3-subtitles 2
//   # comments
//   clip <kind>[+edited] <duration ms> <bytes> <keys> <source>
//   names <speaker 1>|<speaker 2>|...|<speaker 10>   (optional)
//   <start ms> <end ms> s<speaker> <text>
//   ...
//
// (Version 1 had no +edited and no speaker; it's still read.)
//
// A clip is one sound the game plays (a wave in a wave bank) or a movie's
// dialogue track:
//   kind      story (story cutscene and story mission dialogue), speech
//             (riders, coaches and announcers while riding) or video (a movie)
//   bytes     the size of its XMA data (0 for a movie)
//   keys      where it's recognised while it plays: comma-separated
//             <length>:<hash>, the FNV-1a 64-bit hash (16 hex digits) of the
//             first <length> bytes of its XMA data (2048, 4096, 8192 and
//             16384, and the whole size when smaller); for a movie
//             file:<name>, its file name without the extension, lower case
//   source    where it came from (the rest of the line; for the log only)
// and the lines after it, until the next clip, are its subtitles: times from
// the start of the sound, and who says it (s1 to s4: the clip's first,
// second, third and fourth voice, shown white, yellow, pink and green).
// +edited: corrected by hand (the subtitle editor); making the subtitles
// again keeps it as it is. names: who the clip's speakers are (typed in the
// editor; any may be empty), shown before their lines when the game's
// subtitles_speaker_names is on. (Older readers skip the line.)
//
// The game sees which XMA data each of the audio hardware's decoder contexts
// is reading (guest memory), hashes the start of it and so knows which clip
// is playing and how far into it; a movie by its file being open.

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace amped3::subtitle_file {

inline constexpr const char* kFileName = "amped3.subtitles";
inline constexpr const char* kMagic = "amped3-subtitles 2";
inline constexpr const char* kMagicV1 = "amped3-subtitles 1";
inline constexpr int kSpeakers = 10;
// Their colours (RGB): white, then the yellow, pink and green of the game's
// menus (the four the subtitle maker tells apart), then six more for the
// editor's: sky blue, orange, lavender, coral, mint, sand.
inline constexpr uint8_t kSpeakerRgb[kSpeakers][3] = {
    {255, 255, 255}, {250, 232, 74}, {246, 96, 240}, {128, 224, 40}, {96, 200, 255},
    {255, 160, 60},  {180, 150, 255}, {255, 110, 110}, {120, 240, 190}, {225, 200, 150}};
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
  int speaker = 0;  // 0..kSpeakers-1
};

enum class Kind { kSpeech, kStory, kVideo };

inline const char* KindName(Kind k) {
  return k == Kind::kStory ? "story" : k == Kind::kVideo ? "video" : "speech";
}

struct Clip {
  Kind kind = Kind::kSpeech;
  std::string file;  // a movie: its file name, no extension, lower case
  uint32_t duration_ms = 0;
  uint32_t bytes = 0;
  std::vector<Key> keys;
  std::string source;
  bool edited = false;  // corrected by hand: kept as it is
  std::string names[kSpeakers];  // who each speaker is ("": not named)
  std::vector<Line> lines;
  bool Named() const {
    for (const std::string& n : names) {
      if (!n.empty()) return true;
    }
    return false;
  }
};

inline std::string FormatKeys(const std::vector<Key>& keys, const std::string& file = "") {
  std::string s = file.empty() ? std::string() : "file:" + file;
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
      if (line != kMagic && line != kMagicV1) {
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
      std::string kind_name = kind;
      const size_t plus = kind_name.find('+');
      if (plus != std::string::npos) {
        c.edited = kind_name.find("edited", plus) != std::string::npos;
        kind_name.resize(plus);
      }
      c.kind = kind_name == "video"   ? Kind::kVideo
               : kind_name == "story" ? Kind::kStory
                                      : Kind::kSpeech;
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
        if (one.compare(0, 5, "file:") == 0) {
          c.file = one.substr(5);
        } else if (colon != std::string::npos) {
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
    if (line.compare(0, 6, "names ") == 0) {
      Clip& c = clips->back();
      size_t k = 6;
      for (int i = 0; i < kSpeakers && k <= line.size(); ++i) {
        size_t bar = line.find('|', k);
        if (bar == std::string::npos) bar = line.size();
        c.names[i] = line.substr(k, bar - k);
        k = bar + 1;
      }
      continue;
    }
    unsigned a = 0, b = 0;
    int used = 0;
    if (std::sscanf(line.c_str(), "%u %u %n", &a, &b, &used) >= 2 && used > 0) {
      Line l;
      l.start_ms = a;
      l.end_ms = b;
      l.text = line.substr(used);
      // s1 .. s10.
      if (l.text.size() >= 3 && l.text[0] == 's' && l.text[1] >= '1' && l.text[1] <= '9') {
        size_t k = 1;
        int n = 0;
        while (k < l.text.size() && k < 3 && l.text[k] >= '0' && l.text[k] <= '9') n = n * 10 + (l.text[k++] - '0');
        if (k < l.text.size() && l.text[k] == ' ' && n >= 1 && n <= kSpeakers) {
          l.speaker = n - 1;
          l.text.erase(0, k + 1);
        }
      }
      if (!l.text.empty()) clips->back().lines.push_back(std::move(l));
    }
  }
  if (first) {
    if (error) *error = "empty file";
    return false;
  }
  return true;
}

// Text on one line (no line breaks or tabs).
inline std::string OneLine(std::string s) {
  for (char& c : s) {
    if (c == '\n' || c == '\r' || c == '\t') c = ' ';
  }
  return s;
}

// The file's text for `clips` (header comment lines after the magic).
inline std::string Format(const std::vector<Clip>& clips, const std::string& comment = "") {
  std::string out = std::string(kMagic) + "\n";
  if (!comment.empty()) out += comment;
  out += "# Lines: clip <kind>[+edited] <ms> <bytes> <keys> <source> / names <s1>|...|<s10> / <start ms> <end ms> s<speaker> <text>\n";
  char buf[64];
  for (const Clip& c : clips) {
    std::snprintf(buf, sizeof(buf), " %u %u ", c.duration_ms, c.bytes);
    out += std::string("clip ") + KindName(c.kind) + (c.edited ? "+edited" : "") + buf +
           FormatKeys(c.keys, c.file) + " " + OneLine(c.source) + "\n";
    if (c.Named()) {
      out += "names ";
      for (int i = 0; i < kSpeakers; ++i) {
        std::string n = OneLine(c.names[i]);
        n.erase(std::remove(n.begin(), n.end(), '|'), n.end());
        out += (i ? "|" : "") + n;
      }
      out += "\n";
    }
    for (const Line& l : c.lines) {
      std::snprintf(buf, sizeof(buf), "%u %u s%d ", l.start_ms, l.end_ms, std::clamp(l.speaker, 0, kSpeakers - 1) + 1);
      out += buf + OneLine(l.text) + "\n";
    }
  }
  return out;
}

}  // namespace amped3::subtitle_file
