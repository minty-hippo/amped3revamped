// amped3 - patches for the game's menus (see gui_patch_core.h).

#include "gui_patch_core.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <functional>
#include <cmath>
#include <map>
#include <memory>

// Raw deflate in and out: miniz (third_party/miniz, compiled as C by the
// build with these options).
#define MINIZ_NO_STDIO
#define MINIZ_NO_TIME
#define MINIZ_NO_ARCHIVE_APIS
#define MINIZ_NO_ZLIB_COMPATIBLE_NAMES
#include "../third_party/miniz/miniz.h"

namespace amped3::gui_patch {
namespace {

// ---- big-endian helpers -------------------------------------------------------

uint32_t Be32(const uint8_t* p) {
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
uint16_t Be16(const uint8_t* p) { return uint16_t((p[0] << 8) | p[1]); }
void Put32(Bytes& b, uint32_t v) {
  b.push_back(uint8_t(v >> 24));
  b.push_back(uint8_t(v >> 16));
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void Put16(Bytes& b, uint16_t v) {
  b.push_back(uint8_t(v >> 8));
  b.push_back(uint8_t(v));
}
void Set32(Bytes& b, size_t at, uint32_t v) {
  b[at] = uint8_t(v >> 24);
  b[at + 1] = uint8_t(v >> 16);
  b[at + 2] = uint8_t(v >> 8);
  b[at + 3] = uint8_t(v);
}
uint32_t Le32(const uint8_t* p) {
  return uint32_t(p[0]) | (uint32_t(p[1]) << 8) | (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
}
uint16_t Le16(const uint8_t* p) { return uint16_t(p[0] | (p[1] << 8)); }
void PutLe32(Bytes& b, uint32_t v) {
  for (int i = 0; i < 4; ++i) b.push_back(uint8_t(v >> (8 * i)));
}
void PutLe16(Bytes& b, uint16_t v) {
  b.push_back(uint8_t(v));
  b.push_back(uint8_t(v >> 8));
}
uint32_t BeFloat(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

bool Fail(std::string* error, const std::string& why) {
  if (error) *error = why;
  return false;
}

// A bounds-checked reader.
struct Reader {
  const uint8_t* p;
  size_t size;
  size_t pos = 0;
  bool ok = true;
  bool Need(size_t n) {
    if (!ok || pos + n > size) ok = false;
    return ok;
  }
  uint8_t U8() { return Need(1) ? p[pos++] : 0; }
  uint16_t U16() {
    if (!Need(2)) return 0;
    pos += 2;
    return Be16(p + pos - 2);
  }
  uint32_t U32() {
    if (!Need(4)) return 0;
    pos += 4;
    return Be32(p + pos - 4);
  }
  std::string Str(size_t n) {
    if (!Need(n)) return {};
    pos += n;
    return std::string(reinterpret_cast<const char*>(p + pos - n), n);
  }
};

uint32_t Crc32(const uint8_t* p, size_t n) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

// ---- Lua 5.0 chunks -----------------------------------------------------------
//
// The game's Lua is a modified 5.0.2 (big-endian; 4-byte int, size_t and
// instruction; double numbers; four bitwise opcodes added after DIV; wide
// strings). Scenes are compiled chunks that only build tables, loaded into
// states with no standard library, compiled outside any error handler (a
// compile error there crashes the game). So a patched scene is a compiled
// chunk too: the original is evaluated here (the few opcodes table
// constructors use), the tables are changed, and they are compiled back.

bool IsLuaChunk(const Bytes& b) {
  return b.size() > 22 && b[0] == 0x1B && b[1] == 'L' && b[2] == 'u' && b[3] == 'a' &&
         b[4] == 0x50 && b[5] == 0 /* big-endian */ && b[6] == 4 && b[7] == 4 && b[8] == 4 &&
         b[13] == 8;
}

struct LTable;

struct LValue {
  enum Type { kNil, kBool, kNumber, kString, kTable } type = kNil;
  bool b = false;
  double n = 0;
  std::string s;
  std::shared_ptr<LTable> t;

  static LValue Bool(bool v) {
    LValue r;
    r.type = kBool;
    r.b = v;
    return r;
  }
  static LValue Number(double v) {
    LValue r;
    r.type = kNumber;
    r.n = v;
    return r;
  }
  static LValue String(std::string v) {
    LValue r;
    r.type = kString;
    r.s = std::move(v);
    return r;
  }
  static LValue Table(std::shared_ptr<LTable> v) {
    LValue r;
    r.type = kTable;
    r.t = std::move(v);
    return r;
  }
  bool operator==(const LValue& o) const {
    if (type != o.type) return false;
    switch (type) {
      case kNil: return true;
      case kBool: return b == o.b;
      case kNumber: return n == o.n;
      case kString: return s == o.s;
      case kTable: return t == o.t;
    }
    return false;
  }
};

struct LTable {
  std::vector<std::pair<LValue, LValue>> items;  // in the order first set

  const LValue* Find(const LValue& key) const {
    for (const auto& [k, v] : items) {
      if (k == key) return &v;
    }
    return nullptr;
  }
  LValue Get(const LValue& key) const {
    const LValue* v = Find(key);
    return v ? *v : LValue();
  }
  LValue Get(const char* key) const { return Get(LValue::String(key)); }
  LValue Get(double key) const { return Get(LValue::Number(key)); }
  std::shared_ptr<LTable> Sub(const char* key) const { return Get(key).t; }
  std::shared_ptr<LTable> Sub(double key) const { return Get(key).t; }
  void Set(const LValue& key, LValue value) {
    for (auto it = items.begin(); it != items.end(); ++it) {
      if (it->first == key) {
        if (value.type == LValue::kNil) {
          items.erase(it);
        } else {
          it->second = std::move(value);
        }
        return;
      }
    }
    if (value.type != LValue::kNil) items.emplace_back(key, std::move(value));
  }
  void Set(const char* key, LValue value) { Set(LValue::String(key), std::move(value)); }
  void Set(double key, LValue value) { Set(LValue::Number(key), std::move(value)); }
};

LValue DeepCopy(const LValue& v, int depth = 0) {
  if (v.type != LValue::kTable || !v.t || depth > 64) return v;
  auto t = std::make_shared<LTable>();
  for (const auto& [k, x] : v.t->items) t->items.emplace_back(k, DeepCopy(x, depth + 1));
  return LValue::Table(t);
}

// What a chunk did: globals it assigned, and keys it set in globals it only
// read (tables the game made, like Scenes).
struct LuaResult {
  std::vector<std::pair<std::string, LValue>> assigned;
  std::vector<std::pair<std::string, std::shared_ptr<LTable>>> read;

  LValue Global(const std::string& name) const {
    for (const auto& [n, v] : assigned) {
      if (n == name) return v;
    }
    return {};
  }
  std::shared_ptr<LTable> ReadGlobal(const std::string& name) const {
    for (const auto& [n, t] : read) {
      if (n == name) return t;
    }
    return nullptr;
  }
};

bool RunLuaChunk(const Bytes& b, LuaResult* result, std::string* error) {
  if (!IsLuaChunk(b)) return Fail(error, "not a compiled Lua chunk");
  Reader r{b.data(), b.size()};
  r.pos = 22;
  auto skip_string = [&]() {
    const uint32_t n = r.U32();
    if (n) r.Str(n);
  };
  skip_string();  // source
  r.U32();        // line defined
  r.Str(3);       // nups, numparams, is_vararg
  const uint8_t max_stack = r.U8();
  const uint32_t lines = r.U32();
  if (!r.Need(size_t(lines) * 4)) return Fail(error, "truncated chunk");
  r.pos += size_t(lines) * 4;
  const uint32_t locals = r.U32();
  for (uint32_t i = 0; i < locals && r.ok; ++i) {
    skip_string();
    r.U32();
    r.U32();
  }
  const uint32_t upvalues = r.U32();
  for (uint32_t i = 0; i < upvalues && r.ok; ++i) skip_string();
  const uint32_t nk = r.U32();
  if (!r.ok || nk > (1u << 18)) return Fail(error, "bad constants");
  std::vector<LValue> k;
  for (uint32_t i = 0; i < nk && r.ok; ++i) {
    const uint8_t type = r.U8();
    if (type == 3) {
      const uint8_t* p = r.Need(8) ? b.data() + r.pos : nullptr;
      r.pos += 8;
      if (!p) break;
      uint64_t bits = 0;
      for (int j = 0; j < 8; ++j) bits = (bits << 8) | p[j];
      double d;
      std::memcpy(&d, &bits, 8);
      k.push_back(LValue::Number(d));
    } else if (type == 4) {
      const uint32_t n = r.U32();
      std::string s = n ? r.Str(n) : std::string();
      if (!s.empty()) s.pop_back();  // the stored terminator
      k.push_back(LValue::String(std::move(s)));
    } else if (type == 0) {
      k.push_back(LValue());
    } else {
      return Fail(error, "unknown constant type");
    }
  }
  if (r.U32() != 0) return Fail(error, "chunk has functions in it");
  const uint32_t ncode = r.U32();
  if (!r.ok || !r.Need(size_t(ncode) * 4)) return Fail(error, "truncated code");
  std::vector<uint32_t> code(ncode);
  for (auto& c : code) c = r.U32();
  if (r.pos != b.size()) return Fail(error, "trailing data after the chunk");

  // The game's opcode numbers (5.0's, with BAND / BOR / BSHL / BSHR at 16-19).
  enum {
    MOVE = 0, LOADK = 1, LOADBOOL = 2, LOADNIL = 3, GETGLOBAL = 5, GETTABLE = 6,
    SETGLOBAL = 7, SETTABLE = 9, NEWTABLE = 10, JMP = 24, RETURN = 31, SETLIST = 35,
  };
  constexpr uint32_t kMaxStack = 250;      // RK operands at or above this are constants
  constexpr uint32_t kFieldsPerFlush = 32;  // SETLIST batches
  std::vector<LValue> reg(size_t(max_stack) + 2);
  auto at = [&](uint32_t a) -> LValue* { return a < reg.size() ? &reg[a] : nullptr; };
  auto rk = [&](uint32_t x, LValue* out) {
    if (x >= kMaxStack) {
      if (x - kMaxStack >= k.size()) return false;
      *out = k[x - kMaxStack];
      return true;
    }
    if (x >= reg.size()) return false;
    *out = reg[x];
    return true;
  };
  auto get_global = [&](const std::string& name) {
    for (const auto& [n, v] : result->assigned) {
      if (n == name) return v;
    }
    for (const auto& [n, t] : result->read) {
      if (n == name) return LValue::Table(t);
    }
    auto t = std::make_shared<LTable>();
    result->read.emplace_back(name, t);
    return LValue::Table(t);
  };
  size_t steps = 0;
  for (size_t pc = 0; pc < code.size(); ++pc) {
    if (++steps > 1000000) return Fail(error, "chunk runs too long");
    const uint32_t i = code[pc];
    const uint32_t op = i & 0x3F;
    const uint32_t c = (i >> 6) & 0x1FF;
    const uint32_t bb = (i >> 15) & 0x1FF;
    const uint32_t a = i >> 24;
    const uint32_t bx = (i >> 6) & 0x3FFFF;
    LValue* ra = at(a);
    if (!ra) return Fail(error, "bad register");
    switch (op) {
      case MOVE:
        if (!at(bb)) return Fail(error, "bad register");
        *ra = reg[bb];
        break;
      case LOADK:
        if (bx >= k.size()) return Fail(error, "bad constant");
        *ra = k[bx];
        break;
      case LOADBOOL:
        *ra = LValue::Bool(bb != 0);
        if (c) ++pc;
        break;
      case LOADNIL:
        for (uint32_t j = a; j <= bb && j < reg.size(); ++j) reg[j] = LValue();
        break;
      case GETGLOBAL:
        if (bx >= k.size() || k[bx].type != LValue::kString) return Fail(error, "bad global");
        *ra = get_global(k[bx].s);
        break;
      case SETGLOBAL: {
        if (bx >= k.size() || k[bx].type != LValue::kString) return Fail(error, "bad global");
        bool found = false;
        for (auto& [n, v] : result->assigned) {
          if (n == k[bx].s) {
            v = *ra;
            found = true;
          }
        }
        if (!found) result->assigned.emplace_back(k[bx].s, *ra);
        break;
      }
      case GETTABLE: {
        LValue key;
        if (!at(bb) || reg[bb].type != LValue::kTable || !rk(c, &key)) {
          return Fail(error, "bad table read");
        }
        *ra = reg[bb].t->Get(key);
        break;
      }
      case SETTABLE: {
        LValue key, value;
        if (ra->type != LValue::kTable || !rk(bb, &key) || !rk(c, &value) ||
            key.type == LValue::kNil) {
          return Fail(error, "bad table write");
        }
        ra->t->Set(key, value);
        break;
      }
      case NEWTABLE:
        *ra = LValue::Table(std::make_shared<LTable>());
        break;
      case SETLIST: {
        if (ra->type != LValue::kTable) return Fail(error, "bad list");
        const uint32_t n = (bx & (kFieldsPerFlush - 1)) + 1;
        const uint32_t first = bx & ~(kFieldsPerFlush - 1);
        for (uint32_t j = 1; j <= n; ++j) {
          if (!at(a + j)) return Fail(error, "bad list");
          ra->t->Set(double(first + j), reg[a + j]);
        }
        break;
      }
      case JMP:
        pc = size_t(int64_t(pc) + int64_t(bx) - 131071);
        break;
      case RETURN:
        return true;
      default:
        return Fail(error, "unexpected opcode " + std::to_string(op));
    }
  }
  return true;
}

// The chunk's effects compiled back into a chunk like the game's own (its
// opcode numbers and dump format), so the game's compiler isn't involved.
class ChunkWriter {
 public:
  bool Ok() const { return ok_; }

  // GETGLOBAL / SETGLOBAL / SETTABLE / NEWTABLE / LOADK / LOADBOOL only.
  void Read(const std::string& name, const std::shared_ptr<LTable>& t) {
    Abx(kGetGlobal, 0, Constant(LValue::String(name)));
    for (const auto& [key, v] : t->items) {
      const uint32_t k = Operand(key, 1);
      const uint32_t x = Value(v, 2);
      Abc(kSetTable, 0, k, x);
    }
  }
  void Assign(const std::string& name, const LValue& v) {
    if (v.type == LValue::kTable) {
      Table(v, 0);
    } else {
      Load(v, 0);
    }
    Abx(kSetGlobal, 0, Constant(LValue::String(name)));
  }

  // `like` is the original chunk: its header and source name are kept.
  Bytes Finish(const Bytes& like) {
    Abc(kReturn, 0, 1, 0);
    Bytes o(like.begin(), like.begin() + 22);
    Reader r{like.data(), like.size()};
    r.pos = 22;
    const uint32_t source_size = r.U32();
    const std::string source = source_size ? r.Str(source_size) : std::string();
    r.U32();
    r.U8();
    const uint8_t params = r.U8();
    const uint8_t vararg = r.U8();
    if (!r.ok) ok_ = false;
    Put32(o, source_size);
    o.insert(o.end(), source.begin(), source.end());
    Put32(o, 0);  // line defined
    o.push_back(0);  // upvalues
    o.push_back(params);
    o.push_back(vararg);
    const uint32_t stack = std::max<uint32_t>(max_reg_ + 1, 2) + params + vararg;
    if (stack > 250) ok_ = false;
    o.push_back(uint8_t(std::min<uint32_t>(stack, 250)));
    Put32(o, uint32_t(code_.size()));  // line info: all line 1
    for (size_t i = 0; i < code_.size(); ++i) Put32(o, 1);
    Put32(o, 0);  // locals
    Put32(o, 0);  // upvalue names
    Put32(o, uint32_t(k_.size()));
    for (const auto& v : k_) {
      if (v.type == LValue::kNumber) {
        o.push_back(3);
        uint64_t bits;
        std::memcpy(&bits, &v.n, 8);
        for (int i = 7; i >= 0; --i) o.push_back(uint8_t(bits >> (8 * i)));
      } else {
        o.push_back(4);
        Put32(o, uint32_t(v.s.size() + 1));
        o.insert(o.end(), v.s.begin(), v.s.end());
        o.push_back(0);
      }
    }
    Put32(o, 0);  // functions
    Put32(o, uint32_t(code_.size()));
    for (uint32_t i : code_) Put32(o, i);
    return o;
  }

 private:
  // The game's opcode numbers.
  enum : uint32_t {
    kLoadK = 1, kLoadBool = 2, kGetGlobal = 5, kSetGlobal = 7, kSetTable = 9, kNewTable = 10,
    kReturn = 31,
  };
  static constexpr uint32_t kMaxStack = 250;
  static constexpr uint32_t kMaxRkConstant = 511 - kMaxStack;

  void Use(uint32_t reg) {
    if (reg >= kMaxStack - 1) ok_ = false;
    max_reg_ = std::max(max_reg_, reg);
  }
  void Abc(uint32_t op, uint32_t a, uint32_t b, uint32_t c) {
    Use(a);
    code_.push_back(op | (c << 6) | (b << 15) | (a << 24));
  }
  void Abx(uint32_t op, uint32_t a, uint32_t bx) {
    Use(a);
    if (bx >= (1u << 18)) ok_ = false;
    code_.push_back(op | (bx << 6) | (a << 24));
  }
  uint32_t Constant(const LValue& v) {
    if (v.type != LValue::kString && v.type != LValue::kNumber) {
      ok_ = false;
      return 0;
    }
    for (size_t i = 0; i < k_.size(); ++i) {
      if (k_[i].type == v.type &&
          (v.type == LValue::kString ? k_[i].s == v.s
                                     : std::memcmp(&k_[i].n, &v.n, sizeof(double)) == 0)) {
        return uint32_t(i);
      }
    }
    k_.push_back(v);
    return uint32_t(k_.size() - 1);
  }
  // A string / number / boolean into `reg`.
  void Load(const LValue& v, uint32_t reg) {
    if (v.type == LValue::kBool) {
      Abc(kLoadBool, reg, v.b ? 1 : 0, 0);
    } else {
      Abx(kLoadK, reg, Constant(v));
    }
  }
  // An RK operand: a constant if it fits, else loaded into `reg`.
  uint32_t Operand(const LValue& v, uint32_t reg) {
    if (v.type == LValue::kString || v.type == LValue::kNumber) {
      const uint32_t k = Constant(v);
      if (k <= kMaxRkConstant) return kMaxStack + k;
    }
    Load(v, reg);
    return reg;
  }
  uint32_t Value(const LValue& v, uint32_t reg) {
    if (v.type == LValue::kTable) {
      Table(v, reg);
      return reg;
    }
    return Operand(v, reg);
  }
  void Table(const LValue& v, uint32_t reg) {
    Abc(kNewTable, reg, 0, 0);
    for (const auto& [key, x] : v.t->items) {
      const uint32_t k = Operand(key, reg + 1);
      const uint32_t val = Value(x, reg + 2);
      Abc(kSetTable, reg, k, val);
    }
  }

  std::vector<LValue> k_;
  std::vector<uint32_t> code_;
  uint32_t max_reg_ = 0;
  bool ok_ = true;
};

bool LuaBytecode(const LuaResult& result, const Bytes& like, Bytes* out) {
  ChunkWriter w;
  for (const auto& [name, t] : result.read) w.Read(name, t);
  for (const auto& [name, v] : result.assigned) w.Assign(name, v);
  *out = w.Finish(like);
  return w.Ok();
}

// ---- string tables (StringTables/<lang>/StringTable.dat) ----------------------
//
// +0x000 u32 font count, u32 0, 5 x 48-byte font names
// +0x0F4 u32 string count, then that many u32 offsets (from the data start)
//        u32 size of the offsets + data
// data   per string, sorted by ID (byte order, binary searched):
//          u8 ID length, ID, u8 0, [pad to even], u16 characters,
//          UTF-16BE characters, u8 font, f32 scale

struct StringEntry {
  std::string id;
  std::u16string text;
  uint8_t font = 0;
  uint32_t scale = 0;
};

}  // namespace

bool PatchStringTable(const Bytes& in, Bytes* out, std::string* error) {
  constexpr size_t kCountAt = 0xF4;
  if (in.size() < kCountAt + 8) return Fail(error, "string table too small");
  const uint32_t count = Be32(in.data() + kCountAt);
  const size_t base = kCountAt + 4 + size_t(count) * 4 + 4;
  if (count == 0 || count > 100000 || base > in.size()) return Fail(error, "bad string count");
  if (Be32(in.data() + base - 4) != in.size() - (kCountAt + 8)) {
    return Fail(error, "string table size field doesn't match");
  }
  std::vector<StringEntry> entries;
  entries.reserve(count + 1);
  Reader r{in.data(), in.size()};
  r.pos = base;
  for (uint32_t i = 0; i < count; ++i) {
    if (r.pos - base != Be32(in.data() + kCountAt + 4 + i * 4)) {
      return Fail(error, "string table offsets don't match its entries");
    }
    StringEntry e;
    const uint8_t n = r.U8();
    e.id = r.Str(n);
    if (r.U8() != 0) return Fail(error, "string table: missing terminator");
    if (r.pos & 1) r.pos++;
    const uint16_t chars = r.U16();
    for (uint16_t c = 0; c < chars && r.ok; ++c) e.text.push_back(char16_t(r.U16()));
    e.font = r.U8();
    e.scale = r.U32();
    if (!r.ok) return Fail(error, "string table truncated");
    entries.push_back(std::move(e));
  }
  if (r.pos != in.size()) return Fail(error, "string table has trailing data");

  const std::string id = kLabelId;
  auto at = std::lower_bound(entries.begin(), entries.end(), id,
                             [](const StringEntry& e, const std::string& k) { return e.id < k; });
  if (at != entries.end() && at->id == id) {
    *out = in;  // already there
    return true;
  }
  StringEntry label;
  label.id = id;
  for (const char* c = kLabelText; *c; ++c) label.text.push_back(char16_t(*c));
  label.font = 0;  // BronzeBigFont_30, as the other rows
  label.scale = BeFloat(1.0f);
  entries.insert(at, std::move(label));

  Bytes o(in.begin(), in.begin() + kCountAt);
  Put32(o, uint32_t(entries.size()));
  const size_t offsets_at = o.size();
  o.resize(o.size() + entries.size() * 4 + 4);
  const size_t data_at = o.size();
  for (size_t i = 0; i < entries.size(); ++i) {
    const auto& e = entries[i];
    Set32(o, offsets_at + i * 4, uint32_t(o.size() - data_at));
    o.push_back(uint8_t(e.id.size()));
    o.insert(o.end(), e.id.begin(), e.id.end());
    o.push_back(0);
    if (o.size() & 1) o.push_back(0);
    Put16(o, uint16_t(e.text.size()));
    for (char16_t c : e.text) Put16(o, uint16_t(c));
    o.push_back(e.font);
    Put32(o, e.scale);
  }
  Set32(o, data_at - 4, uint32_t(o.size() - (kCountAt + 8)));
  *out = std::move(o);
  return true;
}

// ---- action scripts (Scenes/*.actions) ----------------------------------------
//
// u32 record count, u32 records size, u32 record offsets (sorted by name),
// u32 string count, u32 string offsets, records, strings (u8 length + text).
// A record: u32 0, u16 name length, name, then its body; strings it uses are
// tokens (0x80 + index, 0x00).

namespace {

struct Actions {
  std::vector<std::pair<std::string, Bytes>> records;  // name, body after the name
  std::vector<std::string> strings;
};

bool ReadActions(const Bytes& in, Actions* a, std::string* error) {
  Reader r{in.data(), in.size()};
  const uint32_t count = r.U32();
  const uint32_t size = r.U32();
  if (!r.ok || count > 4096) return Fail(error, "actions: bad header");
  std::vector<uint32_t> offsets(count);
  for (auto& o : offsets) o = r.U32();
  const uint32_t strings = r.U32();
  if (!r.ok || strings > 128) return Fail(error, "actions: bad string count");
  std::vector<uint32_t> string_offsets(strings);
  for (auto& o : string_offsets) o = r.U32();
  if (!r.ok) return Fail(error, "actions: truncated");
  const size_t records_at = r.pos;
  const size_t strings_at = records_at + size;
  if (strings_at > in.size()) return Fail(error, "actions: records past the end");
  for (uint32_t i = 0; i < count; ++i) {
    const size_t start = records_at + offsets[i];
    const size_t end = records_at + (i + 1 < count ? offsets[i + 1] : size);
    if (end < start + 6 || end > strings_at || Be32(in.data() + start) != 0) {
      return Fail(error, "actions: bad record");
    }
    const uint16_t n = Be16(in.data() + start + 4);
    if (start + 6 + n > end) return Fail(error, "actions: bad record name");
    a->records.emplace_back(std::string(reinterpret_cast<const char*>(in.data() + start + 6), n),
                            Bytes(in.begin() + start + 6 + n, in.begin() + end));
  }
  for (uint32_t i = 0; i < strings; ++i) {
    const size_t at = strings_at + string_offsets[i];
    if (at >= in.size() || at + 1 + in[at] > in.size()) return Fail(error, "actions: bad string");
    a->strings.emplace_back(reinterpret_cast<const char*>(in.data() + at + 1), in[at]);
  }
  return true;
}

Bytes WriteActions(Actions a) {
  std::sort(a.records.begin(), a.records.end(),
            [](const auto& x, const auto& y) { return x.first < y.first; });
  Bytes records;
  std::vector<uint32_t> offsets;
  for (const auto& [name, body] : a.records) {
    offsets.push_back(uint32_t(records.size()));
    Put32(records, 0);
    Put16(records, uint16_t(name.size()));
    records.insert(records.end(), name.begin(), name.end());
    records.insert(records.end(), body.begin(), body.end());
    while (records.size() & 3) records.push_back(0);
  }
  Bytes strings;
  std::vector<uint32_t> string_offsets;
  for (const auto& s : a.strings) {
    string_offsets.push_back(uint32_t(strings.size()));
    strings.push_back(uint8_t(s.size()));
    strings.insert(strings.end(), s.begin(), s.end());
  }
  Bytes o;
  Put32(o, uint32_t(a.records.size()));
  Put32(o, uint32_t(records.size()));
  for (uint32_t v : offsets) Put32(o, v);
  Put32(o, uint32_t(a.strings.size()));
  for (uint32_t v : string_offsets) Put32(o, v);
  o.insert(o.end(), records.begin(), records.end());
  o.insert(o.end(), strings.begin(), strings.end());
  return o;
}

const Bytes* FindRecord(const Actions& a, const std::string& name) {
  for (const auto& [n, body] : a.records) {
    if (n == name) return &body;
  }
  return nullptr;
}

}  // namespace

bool PatchActions(const Bytes& in, Bytes* out, std::string* error) {
  Actions a;
  if (!ReadActions(in, &a, error)) return false;
  if (FindRecord(a, "GraphicsArrowsOn")) {
    *out = in;
    return true;
  }
  const Bytes* item_state = FindRecord(a, "ButtonListHints_ItemState");
  const Bytes* arrows_on = FindRecord(a, "HintArrowsOn");
  const Bytes* arrows_off = FindRecord(a, "HintArrowsOff");
  if (!item_state || !arrows_on || !arrows_off) return Fail(error, "actions: Hints scripts missing");
  const auto hints = std::find(a.strings.begin(), a.strings.end(), "OnOffHints");
  if (hints == a.strings.end()) return Fail(error, "actions: no OnOffHints string");
  if (a.strings.size() >= 0x7F) return Fail(error, "actions: string table full");
  const uint8_t hints_token = uint8_t(0x80 + (hints - a.strings.begin()));
  const uint8_t ours_token = uint8_t(0x80 + a.strings.size());
  a.strings.push_back("OnOffGraphics");
  // The arrows: the same animation on our switch instead of the Hints one.
  auto retarget = [&](const Bytes& body) {
    Bytes b = body;
    int replaced = 0;
    for (size_t i = 0; i + 1 < b.size(); ++i) {
      if (b[i] == hints_token && b[i + 1] == 0x00) {
        b[i] = ours_token;
        ++replaced;
      }
    }
    return std::make_pair(b, replaced);
  };
  auto [on, on_n] = retarget(*arrows_on);
  auto [off, off_n] = retarget(*arrows_off);
  if (on_n != 1 || off_n != 1) return Fail(error, "actions: unexpected arrow scripts");
  Bytes state = *item_state;
  a.records.emplace_back("ButtonListGraphics_ItemState", std::move(state));
  a.records.emplace_back("GraphicsArrowsOn", std::move(on));
  a.records.emplace_back("GraphicsArrowsOff", std::move(off));
  *out = WriteActions(std::move(a));
  return true;
}

// ---- the pause menu's Options scene ------------------------------------------
//
// Scenes/GlobalOptionsMenu2: rows Audio, Cheat Codes (disabled), Hints, Media
// Callouts. The unused second row becomes "Graphics", enabled; A on it calls
// this.ToggleHint(), which the port's hook (options_graphics.cpp) turns into
// opening its Graphics panel instead.

namespace {

std::shared_ptr<LTable> FindNode(const std::shared_ptr<LTable>& n, const char* name, int depth) {
  if (!n || depth > 64) return nullptr;
  const LValue nm = n->Get("Name");
  if (nm.type == LValue::kString && nm.s == name) return n;
  if (auto kids = n->Sub("Children")) {
    for (const auto& [k, v] : kids->items) {
      if (auto r = FindNode(v.t, name, depth + 1)) return r;
    }
  }
  return nullptr;
}

}  // namespace

bool PatchSceneData(const Bytes& in, Bytes* out, std::string* error) {
  LuaResult run;
  if (!RunLuaChunk(in, &run, error)) return false;
  const auto sd = run.Global("SceneData").t;
  const auto graph = sd ? sd->Sub("Graph") : nullptr;
  const auto row = FindNode(graph, "Item2", 0);
  const auto row_kids = row ? row->Sub("Children") : nullptr;
  const auto row_template = row_kids ? row_kids->Sub(1) : nullptr;
  const auto changed = row_template ? row_template->Sub("ChangedAttributes") : nullptr;
  const auto row_text = changed ? changed->Sub("Menu|Color_Text|Text") : nullptr;
  if (!row_text || row_text->Get("TextID").type != LValue::kString) {
    return Fail(error, "sceneData: the second row isn't laid out as expected");
  }
  row_text->Set("TextID", LValue::String(kLabelId));
  if (!LuaBytecode(run, in, out)) return Fail(error, "sceneData: can't write it back out");
  return true;
}

bool PatchScene(const Bytes& in, Bytes* out, std::string* error) {
  LuaResult run;
  if (!RunLuaChunk(in, &run, error)) return false;
  const auto scenes = run.ReadGlobal("Scenes");
  const auto scene = scenes ? scenes->Sub("GlobalOptionsMenu2") : nullptr;
  const auto controls = scene ? scene->Sub("Controls") : nullptr;
  const auto info = scene ? scene->Sub("ControlsInfo") : nullptr;
  if (!controls || !info) return Fail(error, "scene: not the Options menu expected");
  const LValue list_id = controls->Get("ButtonList");
  if (list_id.type != LValue::kNumber) return Fail(error, "scene: controls not as expected");
  const auto list = info->Sub(list_id.n);
  const auto items = list ? list->Sub("Items") : nullptr;
  const auto row = items ? items->Sub(2) : nullptr;
  if (!row) return Fail(error, "scene: rows not as expected");
  row->Set("Enabled", LValue::Bool(true));
  row->Set("AButton", LValue::String("this.ToggleHint()"));
  if (!LuaBytecode(run, in, out)) return Fail(error, "scene: can't write it back out");
  return true;
}

// The chunk evaluated and compiled again, unchanged (for testing).
bool LuaChunkRecompiled(const Bytes& in, Bytes* out, std::string* error) {
  LuaResult run;
  if (!RunLuaChunk(in, &run, error)) return false;
  return LuaBytecode(run, in, out) || Fail(error, "can't write it out");
}

// ---- zip archives -------------------------------------------------------------

bool ReadZip(const Bytes& in, std::vector<ZipEntry>* entries, std::string* error) {
  if (in.size() < 22) return Fail(error, "zip: too small");
  size_t eocd = std::string::npos;
  for (size_t i = in.size() - 22 + 1; i-- > 0 && in.size() - i <= 22 + 0xFFFF;) {
    if (Le32(in.data() + i) == 0x06054B50u) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) return Fail(error, "zip: no end record");
  const uint16_t count = Le16(in.data() + eocd + 10);
  const uint32_t dir_size = Le32(in.data() + eocd + 12);
  const uint32_t dir_at = Le32(in.data() + eocd + 16);
  if (size_t(dir_at) + dir_size > in.size()) return Fail(error, "zip: bad directory");
  size_t p = dir_at;
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 46 > in.size() || Le32(in.data() + p) != 0x02014B50u) {
      return Fail(error, "zip: bad directory entry");
    }
    ZipEntry e;
    const uint16_t flags = Le16(in.data() + p + 8);
    e.method = Le16(in.data() + p + 10);
    e.time = Le16(in.data() + p + 12);
    e.date = Le16(in.data() + p + 14);
    e.crc = Le32(in.data() + p + 16);
    const uint32_t csize = Le32(in.data() + p + 20);
    const uint32_t usize = Le32(in.data() + p + 24);
    const uint16_t n = Le16(in.data() + p + 28);
    const uint16_t extra = Le16(in.data() + p + 30);
    const uint16_t comment = Le16(in.data() + p + 32);
    const uint32_t local = Le32(in.data() + p + 42);
    if (p + 46 + n > in.size()) return Fail(error, "zip: bad name");
    e.name.assign(reinterpret_cast<const char*>(in.data() + p + 46), n);
    p += 46 + n + extra + comment;
    if (flags & 1) return Fail(error, "zip: encrypted entry");
    if (size_t(local) + 30 > in.size() || Le32(in.data() + local) != 0x04034B50u) {
      return Fail(error, "zip: bad local header for " + e.name);
    }
    const size_t data_at =
        local + 30 + Le16(in.data() + local + 26) + Le16(in.data() + local + 28);
    if (data_at + csize > in.size()) return Fail(error, "zip: data past the end for " + e.name);
    e.raw.assign(in.begin() + data_at, in.begin() + data_at + csize);
    if (e.method == 0) {
      e.data = e.raw;
    } else if (e.method == 8) {
      size_t len = 0;
      void* d = usize ? tinfl_decompress_mem_to_heap(e.raw.data(), e.raw.size(), &len, 0)
                      : nullptr;
      if (!d && usize) return Fail(error, "zip: can't inflate " + e.name);
      if (d) e.data.assign(static_cast<uint8_t*>(d), static_cast<uint8_t*>(d) + len);
      mz_free(d);
    } else {
      return Fail(error, "zip: unsupported method for " + e.name);
    }
    if (e.data.size() != usize || Crc32(e.data.data(), e.data.size()) != e.crc) {
      return Fail(error, "zip: bad data for " + e.name);
    }
    entries->push_back(std::move(e));
  }
  return true;
}

Bytes WriteZip(const std::vector<ZipEntry>& entries) {
  Bytes o, dir;
  for (const auto& e : entries) {
    Bytes packed;
    uint16_t method = e.method;
    uint32_t crc = e.crc;
    if (e.changed) {
      crc = Crc32(e.data.data(), e.data.size());
      // Best compression, raw deflate (as the game's own packages): the
      // game keeps its menu packages in a fixed amount of memory, so they
      // mustn't grow.
      size_t len = 0;
      const int flags = int(tdefl_create_comp_flags_from_zip_params(10, -15, 0));
      void* z = e.data.empty()
                    ? nullptr
                    : tdefl_compress_mem_to_heap(e.data.data(), e.data.size(), &len, flags);
      if (z && len < e.data.size()) {
        packed.assign(static_cast<uint8_t*>(z), static_cast<uint8_t*>(z) + len);
        method = 8;
      } else {
        packed = e.data;
        method = 0;
      }
      if (z) mz_free(z);
    } else {
      packed = e.raw;
    }
    const uint32_t local = uint32_t(o.size());
    PutLe32(o, 0x04034B50u);
    PutLe16(o, 20);
    PutLe16(o, 2);
    PutLe16(o, method);
    PutLe16(o, e.time);
    PutLe16(o, e.date);
    PutLe32(o, crc);
    PutLe32(o, uint32_t(packed.size()));
    PutLe32(o, uint32_t(e.data.size()));
    PutLe16(o, uint16_t(e.name.size()));
    PutLe16(o, 0);
    o.insert(o.end(), e.name.begin(), e.name.end());
    o.insert(o.end(), packed.begin(), packed.end());

    PutLe32(dir, 0x02014B50u);
    PutLe16(dir, 20);
    PutLe16(dir, 20);
    PutLe16(dir, 2);
    PutLe16(dir, method);
    PutLe16(dir, e.time);
    PutLe16(dir, e.date);
    PutLe32(dir, crc);
    PutLe32(dir, uint32_t(packed.size()));
    PutLe32(dir, uint32_t(e.data.size()));
    PutLe16(dir, uint16_t(e.name.size()));
    PutLe16(dir, 0);
    PutLe16(dir, 0);
    PutLe16(dir, 0);
    PutLe16(dir, 0);
    PutLe32(dir, 0);
    PutLe32(dir, local);
    dir.insert(dir.end(), e.name.begin(), e.name.end());
  }
  const uint32_t dir_at = uint32_t(o.size());
  o.insert(o.end(), dir.begin(), dir.end());
  PutLe32(o, 0x06054B50u);
  PutLe16(o, 0);
  PutLe16(o, 0);
  PutLe16(o, uint16_t(entries.size()));
  PutLe16(o, uint16_t(entries.size()));
  PutLe32(o, uint32_t(dir.size()));
  PutLe32(o, dir_at);
  PutLe16(o, 0);
  return o;
}

// ---- the resource directory (d:\resources.dir) ----------------------------------
//
// A zip with one entry per archive on the disc (e.g. GuiPackages/Base.rui)
// listing that archive's entries, which the game reads instead of the
// archive's own directory: per entry u32 0, u32 time, u32 CRC, u32 packed
// size, u32 size, u32 local header offset, u32 flags, name + NUL; then u32
// count. A rewritten archive needs its listing rewritten to match.

namespace {

std::string Lower(std::string s);

struct ZipDirEntry {
  std::string name;
  uint32_t crc = 0, packed = 0, size = 0, offset = 0;
};

bool ReadZipDirectory(const Bytes& in, std::vector<ZipDirEntry>* out, std::string* error) {
  if (in.size() < 22) return Fail(error, "zip: too small");
  size_t eocd = std::string::npos;
  for (size_t i = in.size() - 22 + 1; i-- > 0 && in.size() - i <= 22 + 0xFFFF;) {
    if (Le32(in.data() + i) == 0x06054B50u) {
      eocd = i;
      break;
    }
  }
  if (eocd == std::string::npos) return Fail(error, "zip: no end record");
  const uint16_t count = Le16(in.data() + eocd + 10);
  size_t p = Le32(in.data() + eocd + 16);
  for (uint16_t i = 0; i < count; ++i) {
    if (p + 46 > in.size() || Le32(in.data() + p) != 0x02014B50u) {
      return Fail(error, "zip: bad directory entry");
    }
    ZipDirEntry e;
    e.crc = Le32(in.data() + p + 16);
    e.packed = Le32(in.data() + p + 20);
    e.size = Le32(in.data() + p + 24);
    const uint16_t n = Le16(in.data() + p + 28);
    e.offset = Le32(in.data() + p + 42);
    if (p + 46 + n > in.size()) return Fail(error, "zip: bad name");
    e.name.assign(reinterpret_cast<const char*>(in.data() + p + 46), n);
    p += 46 + n + Le16(in.data() + p + 30) + Le16(in.data() + p + 32);
    out->push_back(std::move(e));
  }
  return true;
}

bool PatchListing(const Bytes& in, const std::vector<ZipDirEntry>& now, Bytes* out,
                  std::string* error) {
  if (in.size() < 4) return Fail(error, "listing too small");
  const uint32_t count = Be32(in.data() + in.size() - 4);
  Bytes o;
  size_t p = 0;
  uint32_t n = 0;
  while (p + 4 < in.size()) {
    if (p + 28 > in.size() - 4) return Fail(error, "listing truncated");
    const uint8_t* r = in.data() + p;
    size_t end = p + 28;
    while (end < in.size() - 4 && in[end] != 0) ++end;
    if (end >= in.size() - 4) return Fail(error, "listing name not terminated");
    const std::string name(reinterpret_cast<const char*>(in.data() + p + 28), end - (p + 28));
    const ZipDirEntry* e = nullptr;
    for (const auto& x : now) {
      if (x.name == name) e = &x;
    }
    if (!e) return Fail(error, "listing: " + name + " not in the new archive");
    o.insert(o.end(), r, r + 8);  // 0, time
    Put32(o, e->crc);
    Put32(o, e->packed);
    Put32(o, e->size);
    Put32(o, e->offset);
    o.insert(o.end(), r + 24, r + 28);  // flags
    o.insert(o.end(), in.begin() + p + 28, in.begin() + end + 1);
    p = end + 1;
    ++n;
  }
  if (n != count || n != now.size()) return Fail(error, "listing count doesn't match");
  Put32(o, count);
  *out = std::move(o);
  return true;
}

}  // namespace

bool PatchResourceDirectory(const Bytes& in,
                            const std::vector<std::pair<std::string, const Bytes*>>& archives,
                            Bytes* out, std::string* error) {
  std::vector<ZipEntry> entries;
  if (!ReadZip(in, &entries, error)) return false;
  size_t done = 0;
  for (const auto& [name, archive] : archives) {
    ZipEntry* listing = nullptr;
    for (auto& e : entries) {
      if (Lower(e.name) == Lower(name)) listing = &e;
    }
    if (!listing) return Fail(error, "resources.dir: no listing for " + name);
    std::vector<ZipDirEntry> now;
    if (!ReadZipDirectory(*archive, &now, error)) return false;
    Bytes patched;
    std::string why;
    if (!PatchListing(listing->data, now, &patched, &why)) {
      return Fail(error, "resources.dir: " + name + ": " + why);
    }
    listing->data = std::move(patched);
    listing->changed = true;
    ++done;
  }
  if (done != archives.size()) return Fail(error, "resources.dir: listings missing");
  *out = WriteZip(entries);
  return true;
}

// ---- packages -----------------------------------------------------------------

namespace {

std::string Lower(std::string s) {
  for (auto& c : s) c = char(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

bool EndsWith(const std::string& s, const std::string& tail) {
  return s.size() >= tail.size() && s.compare(s.size() - tail.size(), tail.size(), tail) == 0;
}

}  // namespace

bool PatchGamePackage(const Bytes& in, Bytes* out, std::string* error) {
  std::vector<ZipEntry> entries;
  if (!ReadZip(in, &entries, error)) return false;
  int done = 0;
  for (auto& e : entries) {
    const std::string n = Lower(e.name);
    bool (*patch)(const Bytes&, Bytes*, std::string*) = nullptr;
    if (n == "scenes/globaloptionsmenu2.scene") patch = PatchScene;
    if (n == "scenes/globaloptionsmenu2.scenedata") patch = PatchSceneData;
    if (!patch) continue;
    Bytes patched;
    std::string why;
    if (!patch(e.data, &patched, &why)) return Fail(error, e.name + ": " + why);
    e.data = std::move(patched);
    e.changed = true;
    ++done;
  }
  if (done != 2) return Fail(error, "Game package: the Options menu's files weren't all found");
  *out = WriteZip(entries);
  return true;
}

bool PatchBasePackage(const Bytes& in, Bytes* out, std::string* error) {
  std::vector<ZipEntry> entries;
  if (!ReadZip(in, &entries, error)) return false;
  int done = 0;
  for (auto& e : entries) {
    const std::string n = Lower(e.name);
    if (n.rfind("stringtables/", 0) != 0 || !EndsWith(n, "/stringtable.dat")) continue;
    Bytes patched;
    std::string why;
    if (!PatchStringTable(e.data, &patched, &why)) return Fail(error, e.name + ": " + why);
    e.data = std::move(patched);
    e.changed = true;
    ++done;
  }
  if (!done) return Fail(error, "Base package: no string tables found");
  *out = WriteZip(entries);
  return true;
}

}  // namespace amped3::gui_patch
