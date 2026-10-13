// amped3-launcher - a small JSON reader (GitHub API replies).

#pragma once

#include <cstdlib>
#include <utility>
#include <memory>
#include <string>
#include <vector>

namespace launcher {

class Json {
 public:
  enum class Type { kNull, kBool, kNumber, kString, kArray, kObject };

  static Json Parse(const std::string& text, bool* ok = nullptr) {
    size_t i = 0;
    Json j;
    const bool good = ParseValue(text, i, j) && (SkipWs(text, i), i == text.size());
    if (ok) *ok = good;
    return good ? j : Json();
  }

  Type type() const { return type_; }
  bool is_object() const { return type_ == Type::kObject; }
  bool is_array() const { return type_ == Type::kArray; }
  const std::string& str() const { return str_; }
  double num() const { return num_; }
  bool boolean() const { return bool_; }
  const std::vector<Json>& items() const { return items_; }
  size_t size() const { return items_.size(); }

  // Missing members are null.
  const Json& operator[](const std::string& key) const {
    static const Json kNull;
    for (const auto& [k, v] : members_) {
      if (k == key) return v;
    }
    return kNull;
  }
  const Json& operator[](size_t index) const {
    static const Json kNull;
    return index < items_.size() ? items_[index] : kNull;
  }

 private:
  static void SkipWs(const std::string& s, size_t& i) {
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) ++i;
  }

  static void AppendUtf8(std::string& out, unsigned cp) {
    if (cp < 0x80) {
      out += char(cp);
    } else if (cp < 0x800) {
      out += char(0xC0 | (cp >> 6));
      out += char(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18));
      out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    }
  }

  static bool ParseString(const std::string& s, size_t& i, std::string& out) {
    if (i >= s.size() || s[i] != '"') return false;
    ++i;
    while (i < s.size()) {
      const char c = s[i++];
      if (c == '"') return true;
      if (c != '\\') {
        out += c;
        continue;
      }
      if (i >= s.size()) return false;
      const char e = s[i++];
      switch (e) {
        case '"': out += '"'; break;
        case '\\': out += '\\'; break;
        case '/': out += '/'; break;
        case 'b': out += '\b'; break;
        case 'f': out += '\f'; break;
        case 'n': out += '\n'; break;
        case 'r': out += '\r'; break;
        case 't': out += '\t'; break;
        case 'u': {
          if (i + 4 > s.size()) return false;
          unsigned cp = unsigned(std::strtoul(s.substr(i, 4).c_str(), nullptr, 16));
          i += 4;
          if (cp >= 0xD800 && cp < 0xDC00 && i + 6 <= s.size() && s[i] == '\\' && s[i + 1] == 'u') {
            const unsigned lo = unsigned(std::strtoul(s.substr(i + 2, 4).c_str(), nullptr, 16));
            if (lo >= 0xDC00 && lo < 0xE000) {
              cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
              i += 6;
            }
          }
          AppendUtf8(out, cp);
          break;
        }
        default: return false;
      }
    }
    return false;
  }

  static bool ParseValue(const std::string& s, size_t& i, Json& out) {
    SkipWs(s, i);
    if (i >= s.size()) return false;
    const char c = s[i];
    if (c == '{') {
      out.type_ = Type::kObject;
      ++i;
      SkipWs(s, i);
      if (i < s.size() && s[i] == '}') return ++i, true;
      while (true) {
        SkipWs(s, i);
        std::string key;
        if (!ParseString(s, i, key)) return false;
        SkipWs(s, i);
        if (i >= s.size() || s[i] != ':') return false;
        ++i;
        Json v;
        if (!ParseValue(s, i, v)) return false;
        out.members_.emplace_back(std::move(key), std::move(v));
        SkipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == '}') return ++i, true;
        return false;
      }
    }
    if (c == '[') {
      out.type_ = Type::kArray;
      ++i;
      SkipWs(s, i);
      if (i < s.size() && s[i] == ']') return ++i, true;
      while (true) {
        Json v;
        if (!ParseValue(s, i, v)) return false;
        out.items_.push_back(std::move(v));
        SkipWs(s, i);
        if (i < s.size() && s[i] == ',') { ++i; continue; }
        if (i < s.size() && s[i] == ']') return ++i, true;
        return false;
      }
    }
    if (c == '"') {
      out.type_ = Type::kString;
      return ParseString(s, i, out.str_);
    }
    if (s.compare(i, 4, "true") == 0) { out.type_ = Type::kBool; out.bool_ = true; i += 4; return true; }
    if (s.compare(i, 5, "false") == 0) { out.type_ = Type::kBool; i += 5; return true; }
    if (s.compare(i, 4, "null") == 0) { i += 4; return true; }
    char* end = nullptr;
    const double v = std::strtod(s.c_str() + i, &end);
    if (end == s.c_str() + i) return false;
    out.type_ = Type::kNumber;
    out.num_ = v;
    i = size_t(end - s.c_str());
    return true;
  }

  Type type_ = Type::kNull;
  bool bool_ = false;
  double num_ = 0;
  std::string str_;
  std::vector<Json> items_;
  std::vector<std::pair<std::string, Json>> members_;  // (vector: Json is incomplete here)
};

}  // namespace launcher
