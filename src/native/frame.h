// One frame of the game's Direct3D work, as the native renderer consumes it:
// the draws, clears, resolves and tiling brackets in order, each draw with the
// full GPU state it was made with. Filled from an F10 capture file by the
// replayer (tools/replay/capture.cpp) or recorded live in the game
// (src/native/live_renderer.cpp).
//
// Resource data (vertex memory, index buffers, textures, microcode) is named
// here and fetched through FrameResources when the renderer needs it.

#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace amped3::native {

// A block of state words shared between GpuStates until one of them writes
// to it (copy on write): consecutive draws mostly change a few constants, so
// copying a state copies five pointers instead of ~13 KB. Each block content
// has a version number, unique for the run, so the renderer can tell whether
// it already uploaded it.
template <size_t N>
class StateBlock {
 public:
  using Array = std::array<uint32_t, N>;
  StateBlock() : p_(std::make_shared<Node>()) {}

  // Reading.
  const Array& get() const { return p_->value; }
  operator const Array&() const { return p_->value; }
  const uint32_t* data() const { return p_->value.data(); }
  static constexpr size_t size() { return N; }
  const uint32_t& operator[](size_t i) const { return p_->value[i]; }
  // Writing: unshares the block first; the content gets a new version.
  Array& mut() {
    if (p_.use_count() > 1) p_ = std::make_shared<Node>(p_->value);
    p_->version.store(0, std::memory_order_relaxed);
    return p_->value;
  }
  void set(size_t i, uint32_t v) { mut()[i] = v; }
  // Version of the current content (assigned on first request).
  uint64_t version() const {
    uint64_t v = p_->version.load(std::memory_order_acquire);
    if (v) return v;
    static std::atomic<uint64_t> next{0};
    const uint64_t fresh = next.fetch_add(1, std::memory_order_relaxed) + 1;
    return p_->version.compare_exchange_strong(v, fresh, std::memory_order_acq_rel) ? fresh : v;
  }

 private:
  struct Node {
    Array value{};
    mutable std::atomic<uint64_t> version{0};
    Node() = default;
    explicit Node(const Array& v) : value(v) {}
  };
  std::shared_ptr<Node> p_;
};

// Accumulated GPU-visible state at a draw (after Direct3D's flush). Blocks are
// written with set(i, v) or mut().
struct GpuState {
  StateBlock<0x400> regs;     // registers 0x2000..0x23FF
  StateBlock<1024> vs_consts;  // c0..c255 as float bits
  StateBlock<1024> ps_consts;
  StateBlock<40> bool_loop;    // b0..b7 dwords, l0..l31
  StateBlock<192> fetch;       // fetch constant dwords
  uint32_t reg(uint32_t index) const { return regs[index - 0x2000]; }
  float regf(uint32_t index) const {
    float f;
    const uint32_t bits = reg(index);
    std::memcpy(&f, &bits, 4);
    return f;
  }
};

struct FrameTexture {
  uint32_t slot = 0;
  std::array<uint32_t, 6> raw{};
  std::string data;  // resource name (tex_...); "<data>.base" / "<data>.mips"
};

enum class EventType { kDraw, kClear, kResolve, kSwap, kBeginTiling, kEndTiling };

struct Draw {
  enum class Kind { kVertices, kIndexed, kVerticesUP, kIndexedUP } kind = Kind::kVertices;
  uint32_t prim = 0;
  uint32_t start = 0;  // first vertex (non-indexed) or first index
  uint32_t count = 0;  // vertex or index count
  int32_t base_vertex = 0;
  std::string vs, ps;  // microcode resource names (ucode_vs_<hash>)
  std::vector<FrameTexture> textures;
  std::map<uint32_t, std::string> vmem;  // vertex fetch constant -> vertex memory resource
  std::string index_data;                // index resource (ib_ / upi_)
  std::string up_vertices;               // user-pointer vertex resource (upv_)
  bool index_32bit = false;
  std::shared_ptr<const GpuState> state;
};

struct Clear {
  uint32_t flags = 0;
  float color[4] = {};
  float z = 0;
  uint32_t stencil = 0;
  std::array<uint32_t, 6> targets{};  // r2000..r2005
  bool has_targets = false;
};

struct Resolve {
  uint32_t flags = 0;
  bool full = true;
  uint32_t rect[4] = {};  // x0 y0 x1 y1
  uint32_t dest_object = 0;
  std::array<uint32_t, 6> dest_raw{};
  bool has_dest = false;
  uint32_t dest_point[2] = {};
  bool has_dest_point = false;
  float clear_rgba[4] = {};
  float clear_z = 1.0f;
  std::array<uint32_t, 6> targets{};
  bool has_targets = false;
};

// BeginTiling: the screen area the tiles cover (the union of the tile rects).
struct Tiling {
  uint32_t width = 0, height = 0;
};

struct Event {
  EventType type;
  uint32_t index;   // into the vector for its type
  uint32_t number;  // [N] in a capture, or the event counter
};

struct Frame {
  std::vector<Event> events;
  std::vector<Draw> draws;
  std::vector<Clear> clears;
  std::vector<Resolve> resolves;
  std::vector<Tiling> tilings;
  uint32_t swap_front_buffer = 0;
  // The swapped texture's fetch constant (from its object), and the name of
  // its data (tex_*) for a front buffer the CPU filled rather than a resolve.
  std::array<uint32_t, 6> swap_fetch{};
  bool has_swap_fetch = false;
  std::string swap_data;
  std::map<uint32_t, std::vector<uint32_t>> objects;  // capture "object" lines by address
};

// Where the renderer gets resource bytes by name. Names are the capture's
// resource stems: mem_*, ib_*, upi_*, upv_* (raw guest bytes), tex_*.base /
// tex_*.mips (tiled, big-endian), ucode_* (big-endian microcode).
class FrameResources {
 public:
  virtual ~FrameResources() = default;
  // Empty if not available.
  virtual std::vector<uint8_t> Read(const std::string& name) = 0;
  // The bytes without a copy, valid for the rest of the frame (nullptr: not
  // held in memory; the renderer then calls Read once and keeps the copy).
  virtual const std::vector<uint8_t>* Peek(const std::string& name) {
    (void)name;
    return nullptr;
  }
  // A number that changes whenever the bytes of `name` change (0: unknown;
  // the renderer then hashes them). Lets the renderer keep vertex data on the
  // GPU across frames.
  virtual uint64_t Version(const std::string& name) {
    (void)name;
    return 0;
  }
};

}  // namespace amped3::native
