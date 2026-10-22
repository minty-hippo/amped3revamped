// amped3-replay: draws an F10 frame capture with the native renderer
// (src/native/renderer.cpp) and writes the result as PNG. Stage 1 of step 4
// (docs/native-replay.md).
//
//   amped3-replay <logs\frame_<time>.txt> [-o <out folder>] [--shaders <folder>]
//                 [--glslang <glslangValidator>] [--draws <n>] [--dump]
//
// Runs headless on any Vulkan 1.3 device (VK_ICD_FILENAMES selects one).

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include <chrono>

#include "../../src/native/frame_writer.h"
#include "../../src/native/glsl_compiler.h"
#include "../../src/native/renderer.h"
#include "../../src/native/vk.h"

#if __has_include(<valgrind/callgrind.h>)
#include <valgrind/callgrind.h>
#define PROFILE_ZERO() CALLGRIND_ZERO_STATS
#else
#define PROFILE_ZERO() ((void)0)
#endif
#include "capture.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> ReadFile(const fs::path& p) {
  std::ifstream f(p, std::ios::binary);
  if (!f) return {};
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

std::string Quote(const fs::path& p) {
#ifdef _WIN32
  return "\"" + p.string() + "\"";
#else
  return "'" + p.string() + "'";
#endif
}

int RunCommand(std::string command) {
#ifdef _WIN32
  command = "\"" + command + "\"";
#endif
  return std::system(command.c_str());
}

// Capture resources: <frame folder>/<name>.bin, microcode from the shaders
// folder.
class CaptureFiles : public amped3::native::FrameResources {
 public:
  CaptureFiles(fs::path resources, fs::path shaders)
      : resources_(std::move(resources)), shaders_(std::move(shaders)) {}
  std::vector<uint8_t> Read(const std::string& name) override {
    if (name.rfind("ucode_", 0) == 0) {
      auto bytes = ReadFile(shaders_ / (name + ".bin"));
      // Native renderer frame dumps keep their microcode with the resources.
      return bytes.empty() ? ReadFile(resources_ / (name + ".bin")) : bytes;
    }
    return ReadFile(resources_ / (name + ".bin"));
  }
  // Vertex and index data stay in memory, as the game's recorded blobs do.
  const std::vector<uint8_t>* Peek(const std::string& name) override {
    if (amped3::native::IsTextureResource(name) || name.rfind("ucode_", 0) == 0) return nullptr;
    auto it = blobs_.find(name);
    if (it == blobs_.end()) it = blobs_.emplace(name, ReadFile(resources_ / (name + ".bin"))).first;
    return &it->second;
  }
  // Capture files never change: a number per resource (unique across
  // captures, as the game's versions are), so the renderer keeps them on the
  // GPU from frame to frame without hashing them each time.
  uint64_t Version(const std::string& name) override {
    static uint64_t next = 0;
    auto it = versions_.find(name);
    if (it == versions_.end()) it = versions_.emplace(name, ++next).first;
    return it->second;
  }

 private:
  fs::path resources_, shaders_;
  std::unordered_map<std::string, std::vector<uint8_t>> blobs_;
  std::unordered_map<std::string, uint64_t> versions_;
};

// GLSL -> SPIR-V with glslangValidator, cached in <out>/spv. A shader is
// recompiled when its translation changes; an empty <name>.keep next to it
// keeps a hand-edited <name>.frag/.vert (compiled to <name>.spv by hand).
amped3::native::ShaderCompiler MakeCompiler(const fs::path& dir, const std::string& glslang) {
  return [dir, glslang](bool frag, const std::string& name, const std::string& glsl,
                        std::vector<uint32_t>* spirv, std::string* error) {
    fs::create_directories(dir);
    const fs::path src = dir / (name + (frag ? ".frag" : ".vert"));
    const fs::path spv = dir / (name + ".spv");
    bool reuse = false;
    if (fs::exists(spv)) {
      auto old = ReadFile(src);
      reuse = fs::exists(dir / (name + ".keep")) || std::string(old.begin(), old.end()) == glsl;
    }
    if (!reuse) {
      std::ofstream(src, std::ios::binary) << glsl;
      if (RunCommand(Quote(glslang) + " -V --target-env vulkan1.3 -o " + Quote(spv) + " " +
                     Quote(src) + " > " + Quote(dir / (name + ".log")) + " 2>&1") != 0) {
        *error = "glslang failed (see spv/" + name + ".log)";
        return false;
      }
    }
    auto code = ReadFile(spv);
    if (code.size() < 20 || code.size() % 4) {
      *error = "no SPIR-V in spv/" + name + ".spv";
      return false;
    }
    spirv->resize(code.size() / 4);
    std::memcpy(spirv->data(), code.data(), code.size());
    return true;
  };
}

void WritePng(amped3::native::Renderer& r, const amped3::native::RenderedImage& im,
              const fs::path& path) {
  std::vector<uint8_t> rgba;
  if (!r.ReadRGBA(im, &rgba)) return;
  stbi_write_png(path.string().c_str(), int(im.width), int(im.height), 4, rgba.data(),
                 int(im.width) * 4);
}

// All render targets and resolved images; depth targets also get their
// stencil as <name>_stencil.png.
void DumpAll(amped3::native::Renderer& r, const fs::path& out) {
  for (const auto& im : r.AllImages()) {
    WritePng(r, im, out / (im.name + ".png"));
    std::vector<uint8_t> rgba;
    if (r.ReadStencil(im, &rgba)) {
      stbi_write_png((out / (im.name + "_stencil.png")).string().c_str(), int(im.width),
                     int(im.height), 4, rgba.data(), int(im.width) * 4);
    }
    // Exact depth (the PNG has 8 bits): little-endian float32, row by row.
    std::vector<float> depth;
    if (r.ReadDepth(im, &depth)) {
      std::ofstream(out / (im.name + "_depth.f32"), std::ios::binary)
          .write(reinterpret_cast<const char*>(depth.data()),
                 std::streamsize(depth.size() * sizeof(float)));
    }
  }
}

std::vector<uint32_t> ParseSteps(const std::string& s) {
  std::vector<uint32_t> steps;
  size_t at = 0;
  while (at < s.size()) {
    size_t comma = s.find(',', at);
    if (comma == std::string::npos) comma = s.size();
    const std::string part = s.substr(at, comma - at);
    steps.push_back(part == "all" ? UINT32_MAX : uint32_t(std::stoul(part)));
    at = comma + 1;
  }
  return steps;
}

// --output-test: DrawOutput (gamma table on the GPU, packed A2B10G10R10)
// against the CPU path the game used before (ReadRGBA, table, Pack1010102);
// and, where the device can export memory, the shared output imported into
// a second Vulkan device and read back there.
uint32_t Pack1010102(const uint8_t* p) {
  auto ten = [](uint32_t v) { return (v << 2) | (v >> 6); };
  return ten(p[0]) | (ten(p[1]) << 10) | (ten(p[2]) << 20) | (3u << 30);
}

#ifndef _WIN32
}  // namespace
#include <dlfcn.h>
namespace {
// Imports the shared image into a fresh device and returns its pixels.
bool ImportAndRead(const amped3::native::OutputInfo& info, std::vector<uint32_t>* pixels,
                   std::string* error) {
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#define G(name) auto name = reinterpret_cast<PFN_##name>(gipa(inst, #name))
  VkInstance inst = VK_NULL_HANDLE;
  G(vkCreateInstance);
  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.apiVersion = VK_API_VERSION_1_3;
  VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ici.pApplicationInfo = &app;
  if (vkCreateInstance(&ici, nullptr, &inst) != VK_SUCCESS) { *error = "instance"; return false; }
  G(vkEnumeratePhysicalDevices); G(vkCreateDevice); G(vkGetDeviceProcAddr);
  G(vkGetPhysicalDeviceMemoryProperties);
  uint32_t n = 1;
  VkPhysicalDevice phys;
  vkEnumeratePhysicalDevices(inst, &n, &phys);
  float prio = 1;
  VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;
  const char* ext[] = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME};
  VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = 1;
  dci.ppEnabledExtensionNames = ext;
  VkDevice dev;
  if (vkCreateDevice(phys, &dci, nullptr, &dev) != VK_SUCCESS) { *error = "device"; return false; }
#define D(name) auto name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(dev, #name))
  D(vkCreateImage); D(vkGetImageMemoryRequirements); D(vkAllocateMemory); D(vkBindImageMemory);
  D(vkCreateBuffer); D(vkGetBufferMemoryRequirements); D(vkBindBufferMemory); D(vkMapMemory);
  D(vkCreateCommandPool); D(vkAllocateCommandBuffers); D(vkBeginCommandBuffer);
  D(vkEndCommandBuffer); D(vkCmdPipelineBarrier); D(vkCmdCopyImageToBuffer); D(vkGetDeviceQueue);
  D(vkQueueSubmit); D(vkQueueWaitIdle);
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(phys, &mp);
  auto type = [&](uint32_t bits, VkMemoryPropertyFlags f) {
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
      if ((bits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & f) == f) return i;
    return 0u;
  };
  // Same creation parameters as the exporter's image.
  VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  emi.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.pNext = &emi;
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  ii.extent = {info.width, info.height, 1};
  ii.mipLevels = ii.arrayLayers = 1;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  VkImage image;
  vkCreateImage(dev, &ii, nullptr, &image);
  VkMemoryRequirements req;
  vkGetImageMemoryRequirements(dev, image, &req);
  VkMemoryDedicatedAllocateInfo ded{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  ded.image = image;
  VkImportMemoryFdInfoKHR imp{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  imp.pNext = &ded;
  imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  imp.fd = int(info.handle);
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &imp;
  mai.allocationSize = info.memory_size;
  mai.memoryTypeIndex = type(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  VkDeviceMemory mem;
  if (vkAllocateMemory(dev, &mai, nullptr, &mem) != VK_SUCCESS) { *error = "import"; return false; }
  vkBindImageMemory(dev, image, mem, 0);
  VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
  bci.size = VkDeviceSize(info.width) * info.height * 4;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf;
  vkCreateBuffer(dev, &bci, nullptr, &buf);
  vkGetBufferMemoryRequirements(dev, buf, &req);
  mai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.allocationSize = req.size;
  mai.memoryTypeIndex = type(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                     VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  VkDeviceMemory bmem;
  vkAllocateMemory(dev, &mai, nullptr, &bmem);
  vkBindBufferMemory(dev, buf, bmem, 0);
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  VkCommandPool pool;
  vkCreateCommandPool(dev, &pci, nullptr, &pool);
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = pool;
  cai.commandBufferCount = 1;
  VkCommandBuffer cb;
  vkAllocateCommandBuffers(dev, &cai, &cb);
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  vkBeginCommandBuffer(cb, &bi);
  // Acquire from the external queue family (the exporter released it in GENERAL).
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
  b.dstQueueFamilyIndex = 0;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                       nullptr, 0, nullptr, 1, &b);
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {info.width, info.height, 1};
  vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
  vkEndCommandBuffer(cb);
  VkQueue q;
  vkGetDeviceQueue(dev, 0, 0, &q);
  VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cb;
  vkQueueSubmit(q, 1, &si, VK_NULL_HANDLE);
  vkQueueWaitIdle(q);
  void* p;
  vkMapMemory(dev, bmem, 0, VK_WHOLE_SIZE, 0, &p);
  pixels->resize(size_t(info.width) * info.height);
  std::memcpy(pixels->data(), p, pixels->size() * 4);
#undef G
#undef D
  return true;  // (the test process exits; nothing freed)
}
#endif

bool OutputTest(amped3::native::Renderer& renderer, const amped3::native::RenderedImage& image,
                const amped3::native::Frame& frame, amped3::native::FrameResources& files,
                amped3::native::RendererOptions options) {
  uint8_t lut[768];
  for (uint32_t v = 0; v < 256; ++v) {
    lut[v] = uint8_t(std::lround(std::pow(v / 255.0, 0.8) * 255));
    lut[256 + v] = uint8_t(255 - v);
    lut[512 + v] = uint8_t(std::min(255u, v * 2));
  }
  std::vector<uint8_t> rgba, packed;
  if (!renderer.ReadRGBA(image, &rgba)) return false;
  // Compared with the CPU path: no map clouds unless asked for (diag
  // "clouds...", "cloudhold").
  renderer.SetMapClouds(options.diag.find("cloud") != std::string::npos);
  amped3::native::OutputInfo info;
  if (options.diag.find("cloudhold") != std::string::npos) {
    // The map revealed (clouds parted), its frame held, then closing in.
    for (int i = 0; i < 2; ++i) {
      renderer.DrawOutput(image, lut, false, 0, &info, &packed);
      std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }
  }
  if (!renderer.DrawOutput(image, lut, false, 0, &info, &packed)) {
    std::cout << "output test: DrawOutput failed\n";
    return false;
  }
  if (options.diag.find("cloud") != std::string::npos) {
    // The output with the map clouds (diag "clouds", "cloudhold"), as a PNG.
    std::vector<uint8_t> png(size_t(image.width) * image.height * 4);
    for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {
      uint32_t v;
      std::memcpy(&v, packed.data() + i * 4, 4);
      for (int c = 0; c < 3; ++c) png[i * 4 + c] = uint8_t(((v >> (10 * c)) & 1023) >> 2);
      png[i * 4 + 3] = 255;
    }
    stbi_write_png("output_clouds.png", int(image.width), int(image.height), 4, png.data(),
                   int(image.width) * 4);
    std::cout << "output test: wrote output_clouds.png\n";
    return true;
  }
  size_t bad = 0, worst = 0;
  for (size_t i = 0; i < size_t(image.width) * image.height; ++i) {
    uint8_t t[4] = {lut[rgba[i * 4]], lut[256 + rgba[i * 4 + 1]], lut[512 + rgba[i * 4 + 2]], 255};
    const uint32_t want = Pack1010102(t);
    uint32_t got;
    std::memcpy(&got, packed.data() + i * 4, 4);
    for (int c = 0; c < 3; ++c) {
      const int d = std::abs(int((want >> (10 * c)) & 1023) - int((got >> (10 * c)) & 1023));
      worst = std::max<size_t>(worst, size_t(d));
      if (d > 2) ++bad;
    }
  }
  std::cout << "output test (readback): " << image.width << "x" << image.height
            << ", worst channel difference " << worst << "/1023, " << bad << " bad\n";
  if (bad) return false;
#ifndef _WIN32
  if (!renderer.CanShareOutput()) {
    std::cout << "output test (shared): device can't export memory\n";
    return true;
  }
  for (uint32_t slot = 0; slot < 2; ++slot) {
    if (!renderer.DrawOutput(image, lut, true, slot, &info, nullptr)) {
      std::cout << "output test (shared): DrawOutput failed\n";
      return false;
    }
    std::vector<uint32_t> pixels;
    std::string error;
    if (!ImportAndRead(info, &pixels, &error)) {
      std::cout << "output test (shared): " << error << "\n";
      return false;
    }
    const bool same = std::memcmp(pixels.data(), packed.data(), packed.size()) == 0;
    std::cout << "output test (shared slot " << slot << ", generation " << info.generation
              << "): " << (same ? "identical to the readback" : "DIFFERENT") << "\n";
    if (!same) return false;
  }
  {
    // Drawing a slot again must keep its image (and generation): the
    // presenter imports each image once.
    amped3::native::OutputInfo again;
    const uint64_t before = info.generation;
    renderer.DrawOutput(image, lut, true, 1, &again, nullptr);
    std::cout << "output test (shared slot 1 again): generation " << again.generation
              << (again.generation == before ? " (kept)" : " (CHANGED)") << "\n";
    if (again.generation != before) return false;
  }
#endif
  // Async renderer (as the game runs it): output recorded into the frame's
  // own submission, read after WaitFrame; three frames, two in flight.
  {
    options.async = true;
    amped3::native::Renderer async_renderer(options);
    amped3::native::Renderer::OutputRequest req;
    req.lut = lut;
    uint64_t tickets[3];
    for (int k = 0; k < 3; ++k) {
      amped3::native::OutputInfo ai;
      auto st = async_renderer.Render(frame, files, UINT32_MAX, &req, &ai);
      if (!st.output_drawn) {
        std::cout << "output test (async): no output drawn\n";
        return false;
      }
      tickets[k] = st.ticket;
      if (k >= 1) {
        async_renderer.WaitFrame(tickets[k - 1]);
        std::vector<uint8_t> got;
        if (!async_renderer.ReadOutput(tickets[k - 1], &got) || got != packed) {
          std::cout << "output test (async frame " << k - 1 << "): DIFFERENT\n";
          return false;
        }
      }
    }
    async_renderer.WaitFrame(tickets[2]);
    std::vector<uint8_t> got;
    const bool same = async_renderer.ReadOutput(tickets[2], &got) && got == packed;
    std::cout << "output test (async, 3 frames): " << (same ? "identical to the readback" : "DIFFERENT")
              << "\n";
    if (!same) return false;
  }
  return true;
}

}  // namespace

int main(int argc, char** argv) {
  fs::path txt, out, shaders;
  std::string glslang = "glslangValidator";
  uint32_t max_draws = UINT32_MAX;
  bool dump = false;
  int device = -1;
  int repeat = 1;
  bool reactive_debug = false;  // --reactive-debug: show FSR's reactive mask  // render the frame this many times (tests the caches)
  std::string pipeline_cache;  // --pipeline-cache file
  std::string cloud_sprite;    // --cloud-sprite file (RendererOptions::cloud_sprite_path)
  bool async_pipelines = false;
  float speed_blur = -1.0f;
  float fog = 0.0f, fog_start = 150.0f;
  float god_rays = 0.0f;  // --god-rays strength  // --fog density [--fog-start metres]  // --speed-blur amount (0..1): speed blur as at that speed
  fs::path rewrite;  // write the loaded frame back out in capture format (tests the writer)
  // Draw counts to stop at, each drawn by a fresh renderer and dumped to
  // <out>/step_<n> (all images and stencils): shows a frame being built up.
  std::vector<uint32_t> steps;
  float depth_bias_scale = 1.0f;  // diagnostics: scales the polygon offset
  float res_scale = 1.0f;         // internal resolution scale (--scale, fractional allowed)
  int upscaler = 0;               // --upscaler xess|fsr: temporal upscaler (needs its DLL)
  float output_scale = 0.0f;      // --output-scale s: upscaler output (and HUD) scale
  int aniso = -1;                 // anisotropic filtering override (-1 = guest's)
  bool msaa = true;               // --no-msaa: single-sample targets
  float motion_blur = 0.0f;       // --motion-blur scale: camera motion blur
  fs::path motion_prev;           // --motion-prev: capture drawn first (the previous frame)
  float motion_yaw = 0.0f;        // --motion-yaw deg: previous frame = this one, camera turned
  bool object_blur = false;       // --object-blur: per-object motion blur too
  bool scenery_blur = false;      // --object-blur-scenery: also on the scenery
  bool merge_clears = true;       // --no-merge-clears: every clear a pass of its own
  bool parallel_commands = true;  // --no-parallel-commands: commands recorded in place
  bool no_camera_blur = false;    // --no-camera-blur: objects only
  bool blur_debug = false;        // --blur-debug: show the motion instead
  bool motion_shuffle = false;    // --motion-shuffle: previous frame's draws in reverse order
  std::string motion_perturb;     // --motion-perturb vs: previous frame's constants of draws
                                  // with that vertex shader changed (shifted: they "move")
  bool output_test = false;       // checks DrawOutput against the CPU path
  fs::path async_with;            // second capture: both drawn alternately by an async renderer
  std::string diag;               // diagnostics: RendererOptions::diag
  uint32_t diag_first = 0, diag_last = 0;  // --diag-draws a-b
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "-o" && i + 1 < argc) {
      out = argv[++i];
    } else if (a == "--shaders" && i + 1 < argc) {
      shaders = argv[++i];
    } else if (a == "--glslang" && i + 1 < argc) {
      glslang = argv[++i];
    } else if (a == "--draws" && i + 1 < argc) {
      max_draws = uint32_t(std::stoul(argv[++i]));
    } else if (a == "--device" && i + 1 < argc) {
      device = std::stoi(argv[++i]);
    } else if (a == "--rewrite" && i + 1 < argc) {
      rewrite = argv[++i];
    } else if (a == "--god-rays" && i + 1 < argc) {
      god_rays = std::stof(argv[++i]);
    } else if (a == "--fog" && i + 1 < argc) {
      fog = std::stof(argv[++i]);
    } else if (a == "--fog-start" && i + 1 < argc) {
      fog_start = std::stof(argv[++i]);
    } else if (a == "--speed-blur" && i + 1 < argc) {
      speed_blur = std::stof(argv[++i]);
    } else if (a == "--async-pipelines") {
      async_pipelines = true;
    } else if (a == "--pipeline-cache" && i + 1 < argc) {
      pipeline_cache = argv[++i];
    } else if (a == "--cloud-sprite" && i + 1 < argc) {
      cloud_sprite = argv[++i];
    } else if (a == "--repeat" && i + 1 < argc) {
      repeat = std::max(1, std::stoi(argv[++i]));
    } else if (a == "--diag" && i + 1 < argc) {
      diag = argv[++i];
    } else if (a == "--diag-draws" && i + 1 < argc) {
      const std::string r = argv[++i];
      const size_t dash = r.find('-');
      diag_first = uint32_t(std::stoul(r.substr(0, dash)));
      diag_last = dash == std::string::npos ? diag_first : uint32_t(std::stoul(r.substr(dash + 1)));
    } else if (a == "--depth-bias-scale" && i + 1 < argc) {
      depth_bias_scale = std::stof(argv[++i]);
    } else if (a == "--steps" && i + 1 < argc) {
      steps = ParseSteps(argv[++i]);
    } else if (a == "--async-with" && i + 1 < argc) {
      async_with = argv[++i];
    } else if (a == "--output-test") {
      output_test = true;
    } else if (a == "--upscaler" && i + 1 < argc) {
      const std::string u = argv[++i];
      upscaler = u == "xess" ? 1 : u == "fsr" ? 2 : u == "test" ? 9 : 0;
    } else if (a == "--output-scale" && i + 1 < argc) {
      output_scale = std::stof(argv[++i]);
    } else if (a == "--scale" && i + 1 < argc) {
      res_scale = std::stof(argv[++i]);
    } else if (a == "--aniso" && i + 1 < argc) {
      aniso = std::stoi(argv[++i]);
    } else if (a == "--reactive-debug") {
      reactive_debug = true;
    } else if (a == "--no-msaa") {
      msaa = false;
    } else if (a == "--motion-blur" && i + 1 < argc) {
      motion_blur = std::stof(argv[++i]);
    } else if (a == "--motion-prev" && i + 1 < argc) {
      motion_prev = argv[++i];
    } else if (a == "--object-blur") {
      object_blur = true;
    } else if (a == "--object-blur-scenery") {
      object_blur = true;
      scenery_blur = true;
    } else if (a == "--no-camera-blur") {
      no_camera_blur = true;
    } else if (a == "--motion-shuffle") {
      motion_shuffle = true;
    } else if (a == "--motion-perturb" && i + 1 < argc) {
      motion_perturb = argv[++i];
    } else if (a == "--no-parallel-commands") {
      parallel_commands = false;
    } else if (a == "--no-merge-clears") {
      merge_clears = false;
    } else if (a == "--blur-debug") {
      blur_debug = true;
    } else if (a == "--motion-yaw" && i + 1 < argc) {
      motion_yaw = std::stof(argv[++i]);

    } else if (a == "--dump") {
      dump = true;
    } else if (a[0] != '-') {
      txt = a;
    } else {
      std::cerr << "unknown option " << a << "\n";
      return 2;
    }
  }
  if (txt.empty()) {
    std::cerr << "usage: amped3-replay <frame_<time>.txt> [-o out] [--shaders dir] "
                 "[--glslang exe] [--draws n] [--dump] [--device n] [--steps a,b,...|all]\n";
    return 2;
  }
  if (out.empty()) out = txt.parent_path() / (txt.stem().string() + "_replay");
  if (shaders.empty()) shaders = txt.parent_path() / "shaders";
  try {
    replay::Capture cap = replay::LoadCapture(txt);
    std::cout << "Capture: " << cap.draws.size() << " draws, " << cap.clears.size() << " clears, "
              << cap.resolves.size() << " resolves\n";
    if (!rewrite.empty()) {
      CaptureFiles files(cap.resources, shaders);
      std::string error;
      if (!amped3::native::WriteFrameCapture(cap, files, rewrite,
                                             rewrite.parent_path() / rewrite.stem(), &error)) {
        std::cerr << "error: " << error << "\n";
        return 1;
      }
      std::cout << "Wrote " << rewrite.string() << "\n";
      return 0;
    }
    fs::create_directories(out);
    amped3::native::RendererOptions options;
    options.device_index = device;
    options.depth_bias_scale = depth_bias_scale;
    options.resolution_scale = res_scale;
    options.upscaler = upscaler;
    options.output_scale = output_scale;
    options.anisotropy_override = aniso;
    if (!async_with.empty()) {
      // Two different frames alternately through one async renderer (as the
      // game's consecutive frames go), each output compared with the same
      // frame drawn alone by a fresh synchronous renderer.
      replay::Capture cap2 = replay::LoadCapture(async_with);
      CaptureFiles files1(cap.resources, shaders), files2(cap2.resources, shaders);
      options.compile = MakeCompiler(out / "spv", glslang);
      options.msaa = msaa;
      std::vector<uint8_t> ref[2];
      const amped3::native::Frame* frames[2] = {&cap, &cap2};
      amped3::native::FrameResources* res[2] = {&files1, &files2};
      for (int k = 0; k < 2; ++k) {
        amped3::native::Renderer r(options);
        amped3::native::Renderer::OutputRequest req;
        amped3::native::OutputInfo info;
        r.Render(*frames[k], *res[k], UINT32_MAX, &req, &info);
        r.Render(*frames[k], *res[k], UINT32_MAX, &req, &info);  // steady state
        r.DrawOutput(r.FinalImage(), nullptr, false, 0, &info, &ref[k]);
      }
      options.async = true;
      amped3::native::Renderer r(options);
      amped3::native::Renderer::OutputRequest req;
      uint64_t prev = 0;
      int prev_k = 0, bad = 0;
      for (int n = 0; n < 8; ++n) {
        const int k = n & 1;
        amped3::native::OutputInfo info;
        auto st = r.Render(*frames[k], *res[k], UINT32_MAX, &req, &info);
        if (prev) {
          r.WaitFrame(prev);
          std::vector<uint8_t> got;
          size_t diff = 0;
          if (r.ReadOutput(prev, &got) && got.size() == ref[prev_k].size()) {
            for (size_t b = 0; b < got.size(); b += 4) {
              if (std::memcmp(&got[b], &ref[prev_k][b], 4)) ++diff;
            }
          } else {
            diff = SIZE_MAX;
          }
          std::cout << "async frame " << n - 1 << " (capture " << prev_k + 1 << "): " << diff
                    << " pixels differ, GPU " << r.GpuFrameMs(prev) << " ms\n";
          if (diff) ++bad;
        }
        prev = st.ticket;
        prev_k = k;
      }
      return bad ? 1 : 0;
    }
    options.msaa = msaa;
    options.pipeline_cache_path = pipeline_cache;
    options.cloud_sprite_path = cloud_sprite;
    options.async_pipelines = async_pipelines;
    options.diag = diag;
    options.diag_first = diag_first;
    options.diag_last = diag_last;
    if (glslang == "internal") {
      // The game's compiler (glslang library), when this build has it.
      // The GLSL is kept in <out>/glsl for comparing translations between
      // builds (MSVC / GCC).
      const fs::path glsl_dir = out / "glsl";
      options.compile = [glsl_dir](bool frag, const std::string& name, const std::string& glsl,
                                   std::vector<uint32_t>* spirv, std::string* error) {
        std::error_code ec;
        fs::create_directories(glsl_dir, ec);
        std::ofstream(glsl_dir / (name + (frag ? ".frag" : ".vert")), std::ios::binary) << glsl;
        return amped3::native::CompileGlsl(frag, glsl, spirv, error);
      };
    } else {
      options.compile = MakeCompiler(out / "spv", glslang);
    }
    CaptureFiles files(cap.resources, shaders);
    if (!steps.empty()) {
      for (uint32_t step : steps) {
        amped3::native::Renderer renderer(options);
        auto stats = renderer.Render(cap, files, step);
        const fs::path dir =
            out / (step == UINT32_MAX ? std::string("step_all") : "step_" + std::to_string(step));
        fs::create_directories(dir);
        DumpAll(renderer, dir);
        std::vector<float> dbg;
        if (renderer.ReadDebugStores(&dbg)) {
          std::ofstream pd(dir / "posdump.txt");
          pd << "# per vertex: xe_pos (x y z w) | gl_Position | gl_VertexIndex guest_index "
                "fetch190_bits fetch191_bits | r10\n";
          char line[512];
          for (size_t v = 0; v < 4096; ++v) {
            const float* e = &dbg[v * 16];
            if (std::isnan(e[8])) continue;  // not written
            uint32_t f190, f191;
            std::memcpy(&f190, &e[10], 4);
            std::memcpy(&f191, &e[11], 4);
            std::snprintf(line, sizeof(line),
                          "%zu: %.6g %.6g %.6g %.6g | %.6g %.6g %.6g %.6g | %.0f %.0f %08X %08X | "
                          "%.6g %.6g %.6g %.6g\n",
                          v, e[0], e[1], e[2], e[3], e[4], e[5], e[6], e[7], e[8], e[9], f190, f191,
                          e[12], e[13], e[14], e[15]);
            pd << line;
          }
        }
        std::ofstream log(dir / "replay.txt");
        log << "Device: " << renderer.device_name() << "\nDraws replayed: " << stats.draws_done
            << " of " << stats.draws << "\nDepth bias scale: " << depth_bias_scale
            << "\nDiagnostics: " << (diag.empty() ? "-" : diag)
            << "\n\nNot replayed or approximated (count):\n";
        for (const auto& [why, n] : stats.skipped) log << "  " << n << " x " << why << "\n";
        log << "\nDetails:\n" << stats.details;
        std::cout << "Step " << dir.filename().string() << ": " << stats.draws_done << " of "
                  << stats.draws << " draws on " << renderer.device_name() << "\n";
      }
      return 0;
    }
    amped3::native::Renderer renderer(options);
    std::cout << "Device: " << renderer.device_name() << "\n";
    renderer.SetMotionBlur(motion_blur, !no_camera_blur, object_blur, blur_debug);
    renderer.SetSceneryObjectBlur(scenery_blur);
    renderer.SetMergeClears(merge_clears);
    renderer.SetParallelCommands(parallel_commands);
    if (motion_yaw != 0.0f) {
      // Test input: the same frame with the lighting pass's inverse
      // view-projection turned about the camera (c4) around world Y.
      replay::Capture prev = replay::LoadCapture(txt);
      const float a = motion_yaw * 3.14159265f / 180.0f, ca = std::cos(a), sa = std::sin(a);
      for (auto& d : prev.draws) {
        if (d.ps.find("C17CD159D811B298") == std::string::npos) continue;
        auto st = std::make_shared<amped3::native::GpuState>(*d.state);
        auto f = [&](int i) { float v; uint32_t b = st->ps_consts[i]; std::memcpy(&v, &b, 4); return v; };
        float row[4][4], cam[3] = {f(16), f(17), f(18)};
        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) row[r][c] = f(r * 4 + c);
        const float R[3][3] = {{ca, 0, sa}, {0, 1, 0}, {-sa, 0, ca}};
        float t[3];
        for (int r = 0; r < 3; ++r) t[r] = cam[r] - (R[r][0] * cam[0] + R[r][1] * cam[1] + R[r][2] * cam[2]);
        for (int r = 0; r < 3; ++r) {
          for (int c = 0; c < 4; ++c) {
            float v = R[r][0] * row[0][c] + R[r][1] * row[1][c] + R[r][2] * row[2][c] + t[r] * row[3][c];
            uint32_t b; std::memcpy(&b, &v, 4); st->ps_consts.set(r * 4 + c, b);
          }
        }
        d.state = st;
      }
      CaptureFiles prev_files(prev.resources, shaders);
      renderer.Render(prev, prev_files);
    }
    if (!motion_prev.empty()) {
      replay::Capture prev = replay::LoadCapture(motion_prev);
      if (!motion_perturb.empty()) {
        int n = 0;
        for (auto& d : prev.draws) {
          if (d.vs.find(motion_perturb) == std::string::npos) continue;
          auto st = std::make_shared<amped3::native::GpuState>(*d.state);
          for (uint32_t i = 0; i < 1024; ++i) {
            float v;
            uint32_t b = st->vs_consts.data()[i];
            std::memcpy(&v, &b, 4);
            if (i % 16 == 3) v += 1.5f;  // translation of a row (x, when rows are c0..c3)
            std::memcpy(&b, &v, 4);
            st->vs_consts.set(i, b);
          }
          d.state = st;
          ++n;
        }
        std::cout << "Perturbed " << n << " draws\n";
        if (motion_perturb == "list") {
          std::map<std::string, int> counts;
          for (auto& d : prev.draws) counts[d.vs]++;
          for (auto& [k, c] : counts) std::cout << c << " " << k << "\n";
        }
      }
      if (motion_shuffle) std::reverse(prev.draws.begin(), prev.draws.end());
      CaptureFiles prev_files(prev.resources, shaders);
      renderer.Render(prev, prev_files);
    }
    auto first_use = [](const char* what, const amped3::native::RenderStats& st) {
      std::printf("%s: prepass %.2f ms, record %.2f ms; first use: %u textures (%.1f MB, %.1f ms), "
                  "%u pipelines (%.1f ms, %u to the background, %u draws waiting), %u shaders (%.1f ms); "
                  "texture cache %u (%.0f MB); texture sets written %u, reset %u\n",
                  what, st.prepass_ms, st.record_ms, st.new_textures, st.texture_mb, st.texture_ms,
                  st.new_pipelines, st.pipeline_ms, st.deferred_pipelines, st.draws_waiting,
                  st.new_shaders, st.shader_ms, st.textures_cached, st.texture_cache_mb,
                  st.texture_sets_written, st.texture_set_resets);
      if (st.draw_parts_sampled) {
        const auto& p = st.draw_part_ms;
        std::printf("  draws by part (ms): setup %.2f, targets %.2f, textures %.2f, pipeline %.2f, "
                    "uniforms %.2f, texture sets %.2f, indices %.2f, commands %.2f, after %.2f\n",
                    p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7], p[8]);
      }
    };
    if (reactive_debug) renderer.SetUpscaleParams(16.7f, 0.0f, true);
    if (speed_blur >= 0.0f) renderer.SetSpeedBlur(1.0f, 1.0f / 60.0f, speed_blur);
    if (fog > 0.0f) renderer.SetFog(fog, fog_start);
    if (god_rays > 0.0f) renderer.SetGodRays(god_rays);
    auto stats = renderer.Render(cap, files, max_draws);
    first_use("first frame", stats);
    if (stats.speed_blurred) std::cout << "Speed blur applied\n";
    if (stats.fogged) std::cout << "Fog applied\n";
    if (stats.god_rays) std::cout << "God rays applied\n";
    std::printf("Nearest character to the camera: %.1f\n", stats.nearest_character);
    if (stats.motion_blurred) std::cout << "Motion blur applied\n";
    if (upscaler) std::cout << "Upscaler: " << stats.upscaler << "\n";
    if (stats.velocity_draws) {
      std::cout << "Object motion: " << stats.velocity_draws << " draws, "
                << stats.velocity_matched << " matched to the previous frame, "
                << stats.velocity_player << " the player's\n";
    }
    // Profiling under callgrind: count only the repeats (caches warm).
    PROFILE_ZERO();
    for (int i = 1; i < repeat; ++i) {
      stats = renderer.Render(cap, files, max_draws);
      first_use(("repeat " + std::to_string(i)).c_str(), stats);
    }
    std::cout << "Recorded " << stats.draws_done << " of " << stats.draws << " draws\n";
    std::cout << "Render passes " << stats.passes << "; clears: " << stats.clear_passes
              << " passes of their own, " << stats.clears_merged << " folded into a load op, "
              << stats.clears_in_pass << " inside a pass\n";

    const auto final_image = renderer.FinalImage();
    if (!final_image.name.empty()) {
      WritePng(renderer, final_image, out / "frame.png");
      std::cout << "Wrote " << (out / "frame.png").string() << " (" << final_image.name << ")\n";
    }
    if (dump) DumpAll(renderer, out);
    if (output_test && !final_image.name.empty()) {
      if (!OutputTest(renderer, final_image, cap, files, options)) return 1;
    }
    std::ofstream log(out / "replay.txt");
    log << "Capture: " << cap.txt.string() << "\n";
    log << "Draws replayed: " << stats.draws_done << " of " << stats.draws << "\n\n";
    log << "Not replayed or approximated (count):\n";
    for (const auto& [why, n] : stats.skipped) log << "  " << n << " x " << why << "\n";
    log << "\nDetails:\n" << stats.details;
    for (const auto& [why, n] : stats.skipped) std::cout << "  " << n << " x " << why << "\n";
  } catch (const std::exception& e) {
    std::cerr << "error: " << e.what() << "\n";
    return 1;
  }
  return 0;
}
