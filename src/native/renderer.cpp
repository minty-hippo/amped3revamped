// The native renderer: see renderer.h. Grew out of the step 4 stage 1
// replayer (tools/replay), which is now a thin front end to this.

#include "renderer.h"
#include "time_of_day.h"

#include <chrono>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "vk.h"  // vulkan_core.h before the platform header
#include <vulkan/vulkan_win32.h>
#else
#include <unistd.h>
#endif

#include <algorithm>
#include <limits>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <bit>
#include <string_view>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <future>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <array>
#include <thread>
#include <deque>
#include <filesystem>
#include <cstdarg>
#include <fstream>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "vk.h"
#include "cmd_stream.h"
#include "xenos_shader.h"
#include "crash_note.h"
#include "xenos_texture.h"
#include "upscaler.h"
#include "texture_replace.h"
#include "../../third_party/miniz/miniz.h"

namespace amped3::native {

namespace {
std::atomic<const char*> g_phase{""};
}  // namespace
const char* RendererPhase() { return g_phase.load(std::memory_order_relaxed); }
PhaseScope::PhaseScope(const char* phase)
    : previous_(g_phase.exchange(phase, std::memory_order_relaxed)) {}
PhaseScope::~PhaseScope() { g_phase.store(previous_, std::memory_order_relaxed); }

namespace {

// --- Small helpers ----------------------------------------------------------------

float HalfToFloat(uint16_t h) {
  const uint32_t sign = (h >> 15) & 1, exp = (h >> 10) & 31, mant = h & 1023;
  float v;
  if (exp == 0) {
    v = std::ldexp(float(mant), -24);
  } else if (exp == 31) {
    v = mant ? NAN : INFINITY;
  } else {
    v = std::ldexp(float(mant | 1024), int(exp) - 25);
  }
  return sign ? -v : v;
}

uint8_t ToByte(float v) {
  if (!(v > 0.0f)) return 0;
  if (v >= 1.0f) return 255;
  return uint8_t(v * 255.0f + 0.5f);
}

// --- Vulkan context ---------------------------------------------------------------

struct Buffer {
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint8_t* mapped = nullptr;
  VkDeviceSize size = 0;
};

struct Image {
  uint32_t levels = 1;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;    // all aspects usable for attachments
  VkImageView sample_view = VK_NULL_HANDLE;  // depth aspect only for depth images
  VkImageView stencil_view = VK_NULL_HANDLE; // stencil aspect (depth images)
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  bool depth = false;
  std::string name;
  // Multisampling: host samples, and host samples per guest sample (2 when a
  // guest count isn't supported and each guest sample uses two; 0 when MSAA
  // fell back to one sample).
  uint32_t samples = 1;
  uint32_t sample_scale = 1;
  // A rendered image (target or resolve) at an internal resolution scale:
  // `scale` times the guest's size (the scene's or the output's scale).
  bool scaled = false;
  float scale = 1.0f;
  VkImageUsageFlags usage = 0;
  // A texture replacement (texture_replace.h): bigger than the guest's, with
  // its own mip chain, sampled with every level and linear filtering.
  bool replaced = false;
  // A texture's surface (SurfaceOf: 0 unknown, 1 snow, 2 metal, 3 wood,
  // 4 rock, 5 paint, 6 fabric), tagged into the normals' alpha when it's a
  // draw's albedo (the lighting's sheen reads it).
  uint8_t surface = 0;
  // A render target (GetTarget): draws never sample one directly.
  bool target = false;
  // The guest's sample count for a render target (1 sample here when MSAA is
  // off: see "One sample for the guest's two" in DoDraw / DoResolve).
  uint32_t guest_samples = 1;
  // Draws into it that wrote every guest sample (DoResolve: a resolve of
  // another sample is the same as one of sample 0 made since).
  uint64_t full_draws = 0;
  // Memory from a shared block (Gpu::SubAllocate) instead of its own
  // allocation: the block's index and the range taken (-1: its own).
  int block = -1;
  VkDeviceSize block_offset = 0, block_size = 0;
};

// A graphics pipeline's create info with everything it points at (built on
// the render thread, compiled there or by a background builder).
struct PipelineBuild {
  VkPipelineShaderStageCreateInfo stages[2] = {
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
      {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
  VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo ia{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  VkSampleMask sample_mask = 0;
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  std::vector<VkPipelineColorBlendAttachmentState> cb;
  VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  VkDynamicState dyn[7] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                           VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE,
                           VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK,
                           VK_DYNAMIC_STATE_STENCIL_WRITE_MASK, VK_DYNAMIC_STATE_DEPTH_BIAS};
  VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  std::vector<VkFormat> color_formats;
  VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
  VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  std::string key;
  std::string note;  // for the crash report (crash_note.h): which pipeline
};

class Gpu {
 public:
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  uint32_t timestamp_bits = 0;  // the queue's valid timestamp bits (0: no timestamps)
  // Pipelines can be asked for only if the driver's cache has them (built
  // in the background otherwise): VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED.
  bool cache_control = false;
  VkPhysicalDeviceProperties props{};
  VkPhysicalDeviceMemoryProperties mem_props{};
  VkCommandPool pool = VK_NULL_HANDLE;
  // The frames' command buffers (recorded by the command stream's thread while
  // the renderer allocates upload command buffers from `pool`).
  VkCommandPool frame_pool = VK_NULL_HANDLE;
  bool anisotropy = false;
  bool bc_textures = false;  // textureCompressionBC enabled
  VkFormat d24s8 = VK_FORMAT_D24_UNORM_S8_UINT;
  VkSampleCountFlags msaa_counts = VK_SAMPLE_COUNT_1_BIT;
  std::string device_name;
  // Output images exportable to another device (VK_KHR_external_memory_win32
  // / _fd).
  bool can_export = false;
  // Shared by every pipeline build (internally synchronised); see
  // RendererOptions::pipeline_cache_path.
  VkPipelineCache pipeline_cache = VK_NULL_HANDLE;
#ifdef _WIN32
  PFN_vkGetMemoryWin32HandleKHR get_memory_handle = nullptr;
  static constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
  static constexpr const char* kExportExtension = VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME;
#else
  PFN_vkGetMemoryFdKHR get_memory_handle = nullptr;
  static constexpr VkExternalMemoryHandleTypeFlagBits kHandleType =
      VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
  static constexpr const char* kExportExtension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
#endif

  // `upscaler` (may be null): its instance and device extensions and
  // features are enabled; *upscaler_error says why it can't be used (then it
  // is reset).
  void Init(int device_index, const uint8_t* uuid = nullptr,
            std::unique_ptr<amped3::native::Upscaler>* upscaler = nullptr,
            std::string* upscaler_error = nullptr) {
    vk::LoadGlobal();
    uint32_t api = VK_API_VERSION_1_0;
    if (vk::vkEnumerateInstanceVersion) vk::vkEnumerateInstanceVersion(&api);
    if (api < VK_API_VERSION_1_3) throw std::runtime_error("Vulkan 1.3 loader required");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "amped3-native";
    app.apiVersion = VK_API_VERSION_1_3;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    std::vector<const char*> instance_extensions;
    if (upscaler && *upscaler) {
      uint32_t need = VK_API_VERSION_1_3;
      (*upscaler)->InstanceRequirements(&instance_extensions, &need);
      app.apiVersion = std::max(app.apiVersion, need);
      // Only extensions the loader has (else the upscaler can't be used).
      uint32_t n = 0;
      vk::vkEnumerateInstanceExtensionProperties(nullptr, &n, nullptr);
      std::vector<VkExtensionProperties> have(n);
      vk::vkEnumerateInstanceExtensionProperties(nullptr, &n, have.data());
      for (const char* e : instance_extensions) {
        bool found = false;
        for (const auto& h : have) found = found || std::strcmp(h.extensionName, e) == 0;
        if (!found) {
          if (upscaler_error) *upscaler_error = std::string("instance extension missing: ") + e;
          upscaler->reset();
          instance_extensions.clear();
          app.apiVersion = VK_API_VERSION_1_3;
          break;
        }
      }
    }
    ici.enabledExtensionCount = uint32_t(instance_extensions.size());
    ici.ppEnabledExtensionNames = instance_extensions.data();
    vk::Check(vk::vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");
    vk::LoadInstance(instance);
    uint32_t count = 0;
    vk::vkEnumeratePhysicalDevices(instance, &count, nullptr);
    if (!count) throw std::runtime_error("no Vulkan devices");
    std::vector<VkPhysicalDevice> devices(count);
    vk::vkEnumeratePhysicalDevices(instance, &count, devices.data());
    int pick = device_index;
    if (uuid) {
      // The presenter's GPU, so output images can be shared with it.
      for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
        VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        p2.pNext = &id;
        vk::vkGetPhysicalDeviceProperties2(devices[i], &p2);
        if (std::memcmp(id.deviceUUID, uuid, VK_UUID_SIZE) == 0) {
          pick = int(i);
          break;
        }
      }
    }
    if (pick < 0) {
      pick = 0;
      for (uint32_t i = 0; i < count; ++i) {
        VkPhysicalDeviceProperties p;
        vk::vkGetPhysicalDeviceProperties(devices[i], &p);
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ||
            p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU) {
          pick = int(i);
          break;
        }
      }
    }
    physical = devices[size_t(std::min<int>(pick, int(count) - 1))];
    vk::vkGetPhysicalDeviceProperties(physical, &props);
    if (props.apiVersion < VK_API_VERSION_1_3) throw std::runtime_error("device lacks Vulkan 1.3");
    vk::vkGetPhysicalDeviceMemoryProperties(physical, &mem_props);
    device_name = props.deviceName;

    uint32_t qcount = 0;
    vk::vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, nullptr);
    std::vector<VkQueueFamilyProperties> qprops(qcount);
    vk::vkGetPhysicalDeviceQueueFamilyProperties(physical, &qcount, qprops.data());
    family = UINT32_MAX;
    for (uint32_t i = 0; i < qcount; ++i) {
      if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
        family = i;
        break;
      }
    }
    if (family == UINT32_MAX) throw std::runtime_error("no graphics queue");
    timestamp_bits = qprops[family].timestampValidBits;

    VkPhysicalDeviceVulkan13Features f13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    VkPhysicalDeviceFeatures2 f2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    f2.pNext = &f13;
    vk::vkGetPhysicalDeviceFeatures2(physical, &f2);
    if (!f13.dynamicRendering) throw std::runtime_error("dynamicRendering not supported");
    VkPhysicalDeviceVulkan13Features want13{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES};
    want13.dynamicRendering = VK_TRUE;
    want13.shaderDemoteToHelperInvocation = f13.shaderDemoteToHelperInvocation;
    want13.pipelineCreationCacheControl = f13.pipelineCreationCacheControl;
    cache_control = f13.pipelineCreationCacheControl == VK_TRUE;
    VkPhysicalDeviceFeatures2 want{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
    want.pNext = &want13;
    want.features.independentBlend = f2.features.independentBlend;
    want.features.samplerAnisotropy = f2.features.samplerAnisotropy;
    // Point sprites (snow, board spray) are bigger than one pixel; without
    // largePoints drivers may draw every point at size 1 (SwiftShader doesn't).
    want.features.largePoints = f2.features.largePoints;
    // Bounds-checked buffer reads: a shader reading past the vertex data gets
    // zeros instead of faulting.
    want.features.robustBufferAccess = f2.features.robustBufferAccess;
    want.features.depthClamp = f2.features.depthClamp;  // diagnostics ("clamp")
    want.features.vertexPipelineStoresAndAtomics =
        f2.features.vertexPipelineStoresAndAtomics;  // diagnostics ("posdump")
    // Upscalers (compute): what they use when it's there.
    VkPhysicalDeviceVulkan12Features have12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    {
      VkPhysicalDeviceFeatures2 q{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
      q.pNext = &have12;
      vk::vkGetPhysicalDeviceFeatures2(physical, &q);
    }
    VkPhysicalDeviceVulkan12Features want12{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES};
    if (upscaler && *upscaler) {
      want13.pNext = &want12;
      want12.shaderFloat16 = have12.shaderFloat16;
      want12.shaderInt8 = have12.shaderInt8;
      want13.subgroupSizeControl = f13.subgroupSizeControl;
      want13.computeFullSubgroups = f13.computeFullSubgroups;
      want13.shaderIntegerDotProduct = f13.shaderIntegerDotProduct;
      want.features.shaderStorageImageWriteWithoutFormat =
          f2.features.shaderStorageImageWriteWithoutFormat;
      want.features.shaderInt16 = f2.features.shaderInt16;
    }
    anisotropy = f2.features.samplerAnisotropy;
    // Compressed textures (DXT) uploaded as they are.
    want.features.textureCompressionBC = f2.features.textureCompressionBC;
    bc_textures = f2.features.textureCompressionBC;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = family;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority;
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.pNext = &want;
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    std::vector<const char*> extensions;
    {
      uint32_t n = 0;
      vk::vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, nullptr);
      std::vector<VkExtensionProperties> ext(n);
      vk::vkEnumerateDeviceExtensionProperties(physical, nullptr, &n, ext.data());
      for (const auto& e : ext) {
        if (std::strcmp(e.extensionName, kExportExtension) == 0) {
          extensions.push_back(kExportExtension);
          can_export = true;
        }
      }
    }
    if (upscaler && *upscaler) {
      std::string why;
      std::vector<const char*> up_ext;
      if (!(*upscaler)->DeviceRequirements(instance, physical, &up_ext, &want, &why)) {
        if (upscaler_error) *upscaler_error = why;
        upscaler->reset();
      } else {
        for (const char* e : up_ext) {
          bool dup = false;
          for (const char* x : extensions) dup = dup || std::strcmp(x, e) == 0;
          if (!dup) extensions.push_back(e);
        }
      }
    }
    dci.enabledExtensionCount = uint32_t(extensions.size());
    dci.ppEnabledExtensionNames = extensions.data();
    vk::Check(vk::vkCreateDevice(physical, &dci, nullptr, &device), "vkCreateDevice");
    vk::LoadDevice(device);
    amped3::native::cmd::Install();  // command streams (cmd_stream.h)
    if (can_export) {
#ifdef _WIN32
      get_memory_handle = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
          vk::vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR"));
#else
      get_memory_handle =
          reinterpret_cast<PFN_vkGetMemoryFdKHR>(vk::vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
#endif
      can_export = get_memory_handle != nullptr;
    }
    vk::vkGetDeviceQueue(device, family, 0, &queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = family;
    vk::Check(vk::vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
    vk::Check(vk::vkCreateCommandPool(device, &pci, nullptr, &frame_pool), "vkCreateCommandPool");

    VkFormatProperties fp;
    vk::vkGetPhysicalDeviceFormatProperties(physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
    if (!(fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
      d24s8 = VK_FORMAT_D32_SFLOAT_S8_UINT;
    }
    // Sample counts usable for render targets that resolves also sample.
    const auto& l = props.limits;
    msaa_counts = l.framebufferColorSampleCounts & l.framebufferDepthSampleCounts &
                  l.framebufferStencilSampleCounts & l.sampledImageColorSampleCounts &
                  l.sampledImageDepthSampleCounts & l.sampledImageStencilSampleCounts;
  }

  // Whether `format` can be a render target with `samples` samples that
  // shaders also read (resolves).
  bool SupportsSamples(VkFormat format, bool depth, uint32_t samples) {
    if (samples == 1) return true;
    if (!(msaa_counts & samples)) return false;
    VkImageFormatProperties ifp;
    const VkImageUsageFlags usage =
        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
        VK_IMAGE_USAGE_TRANSFER_DST_BIT |
        (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    if (vk::vkGetPhysicalDeviceImageFormatProperties(physical, format, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL, usage, 0,
                                                     &ifp) != VK_SUCCESS) {
      return false;
    }
    return (ifp.sampleCounts & samples) != 0;
  }

  bool HasMemoryType(uint32_t bits, VkMemoryPropertyFlags flags) const {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags) return true;
    }
    return false;
  }

  // An output image (A2B10G10R10) whose memory can be exported; *handle and
  // *size describe it for the importer (see OutputInfo).
  Image CreateExportImage(uint32_t w, uint32_t h, uint64_t* handle, uint64_t* size) {
    Image im;
    im.width = w;
    im.height = h;
    im.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    im.name = "output";
    VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
    emi.handleTypes = kHandleType;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.pNext = &emi;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = im.format;
    ici.extent = {w, h, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    vk::Check(vk::vkCreateImage(device, &ici, nullptr, &im.image), "vkCreateImage(output)");
    VkMemoryRequirements req;
    vk::vkGetImageMemoryRequirements(device, im.image, &req);
    VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
    dedicated.image = im.image;
    VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
    export_info.pNext = &dedicated;
    export_info.handleTypes = kHandleType;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.pNext = &export_info;
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk::Check(vk::vkAllocateMemory(device, &mai, nullptr, &im.memory), "vkAllocateMemory(output)");
    vk::vkBindImageMemory(device, im.image, im.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = im.format;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::Check(vk::vkCreateImageView(device, &vci, nullptr, &im.view), "vkCreateImageView(output)");
    im.sample_view = im.view;
    *size = req.size;
#ifdef _WIN32
    VkMemoryGetWin32HandleInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR};
    gi.memory = im.memory;
    gi.handleType = kHandleType;
    HANDLE h_out = nullptr;
    vk::Check(get_memory_handle(device, &gi, &h_out), "vkGetMemoryWin32HandleKHR");
    *handle = uint64_t(reinterpret_cast<uintptr_t>(h_out));
#else
    VkMemoryGetFdInfoKHR gi{VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR};
    gi.memory = im.memory;
    gi.handleType = kHandleType;
    int fd = -1;
    vk::Check(get_memory_handle(device, &gi, &fd), "vkGetMemoryFdKHR");
    *handle = uint64_t(uint32_t(fd));
#endif
    return im;
  }

  uint32_t MemoryType(uint32_t bits, VkMemoryPropertyFlags flags) {
    for (uint32_t i = 0; i < mem_props.memoryTypeCount; ++i) {
      if ((bits & (1u << i)) && (mem_props.memoryTypes[i].propertyFlags & flags) == flags) return i;
    }
    throw std::runtime_error("no suitable memory type");
  }

  Buffer CreateBuffer(VkDeviceSize size, VkBufferUsageFlags usage, bool cached = false) {
    Buffer b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size;
    bci.usage = usage;
    vk::Check(vk::vkCreateBuffer(device, &bci, nullptr, &b.buffer), "vkCreateBuffer");
    VkMemoryRequirements req;
    vk::vkGetBufferMemoryRequirements(device, b.buffer, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    // CPU reads (readback) want cached memory: uncached reads are very slow.
    const VkMemoryPropertyFlags visible =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    mai.memoryTypeIndex = cached && HasMemoryType(req.memoryTypeBits,
                                                  visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                              ? MemoryType(req.memoryTypeBits,
                                           visible | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)
                              : MemoryType(req.memoryTypeBits, visible);
    vk::Check(vk::vkAllocateMemory(device, &mai, nullptr, &b.memory), "vkAllocateMemory");
    vk::vkBindBufferMemory(device, b.buffer, b.memory, 0);
    void* p = nullptr;
    vk::Check(vk::vkMapMemory(device, b.memory, 0, VK_WHOLE_SIZE, 0, &p), "vkMapMemory");
    b.mapped = static_cast<uint8_t*>(p);
    return b;
  }

  Image CreateImage(uint32_t w, uint32_t h, VkFormat format, bool depth, const std::string& name,
                    uint32_t levels = 1, uint32_t samples = 1, bool storage = false) {
    Image im;
    im.levels = levels;
    im.samples = samples;
    im.width = w;
    im.height = h;
    im.format = format;
    im.depth = depth;
    im.name = name;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VkSampleCountFlagBits(samples);
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                (depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT
                       : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) |
                (storage ? VK_IMAGE_USAGE_STORAGE_BIT : 0);
    im.usage = ici.usage;
    vk::Check(vk::vkCreateImage(device, &ici, nullptr, &im.image), "vkCreateImage");
    VkMemoryRequirements req;
    vk::vkGetImageMemoryRequirements(device, im.image, &req);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vk::Check(vk::vkAllocateMemory(device, &mai, nullptr, &im.memory), "vkAllocateMemory(image)");
    vk::vkBindImageMemory(device, im.image, im.memory, 0);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.subresourceRange = {Aspects(im), 0, levels, 0, 1};
    vk::Check(vk::vkCreateImageView(device, &vci, nullptr, &im.view), "vkCreateImageView");
    if (depth) {
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
      vk::Check(vk::vkCreateImageView(device, &vci, nullptr, &im.sample_view), "vkCreateImageView");
      vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_STENCIL_BIT;
      vk::Check(vk::vkCreateImageView(device, &vci, nullptr, &im.stencil_view), "vkCreateImageView");
    } else {
      im.sample_view = im.view;
    }
    return im;
  }

  // Device memory blocks textures are placed in: one allocation holds many
  // textures (allocations are slow, and drivers allow only a few thousand).
  // Render thread only.
  struct MemBlock {
    VkDeviceMemory memory = VK_NULL_HANDLE;
    uint32_t type = 0;
    VkDeviceSize size = 0;
    std::map<VkDeviceSize, VkDeviceSize> free;  // offset -> size
  };
  std::vector<MemBlock> blocks;
  static constexpr VkDeviceSize kBlockSize = 64ull << 20;

  // `req` placed in a block (a new one when none has room): false when it's
  // too big for one.
  bool SubAllocate(const VkMemoryRequirements& req, uint32_t type, int* block,
                   VkDeviceSize* offset) {
    if (req.size > kBlockSize / 4) return false;
    auto take = [&](MemBlock& b) {
      for (auto it = b.free.begin(); it != b.free.end(); ++it) {
        const VkDeviceSize start = it->first, end = it->first + it->second;
        const VkDeviceSize at = (start + req.alignment - 1) / req.alignment * req.alignment;
        if (at + req.size > end) continue;
        b.free.erase(it);
        if (at > start) b.free.emplace(start, at - start);
        if (end > at + req.size) b.free.emplace(at + req.size, end - (at + req.size));
        *offset = at;
        return true;
      }
      return false;
    };
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (blocks[i].type == type && blocks[i].memory && take(blocks[i])) {
        *block = int(i);
        return true;
      }
    }
    MemBlock b;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = kBlockSize;
    mai.memoryTypeIndex = type;
    if (vk::vkAllocateMemory(device, &mai, nullptr, &b.memory) != VK_SUCCESS) return false;
    b.type = type;
    b.size = kBlockSize;
    b.free.emplace(0, kBlockSize);
    size_t slot = blocks.size();
    for (size_t i = 0; i < blocks.size(); ++i) {
      if (!blocks[i].memory) slot = i;
    }
    if (slot == blocks.size()) blocks.emplace_back();
    blocks[slot] = std::move(b);
    take(blocks[slot]);
    *block = int(slot);
    return true;
  }

  void SubFree(int block, VkDeviceSize offset, VkDeviceSize size) {
    MemBlock& b = blocks[size_t(block)];
    auto it = b.free.emplace(offset, size).first;
    // Join with the free ranges on either side.
    if (auto next = std::next(it); next != b.free.end() && it->first + it->second == next->first) {
      it->second += next->second;
      b.free.erase(next);
    }
    if (it != b.free.begin()) {
      auto prev = std::prev(it);
      if (prev->first + prev->second == it->first) {
        prev->second += it->second;
        b.free.erase(it);
      }
    }
  }

  // A sampled texture (upload target only: no attachment usage, so block
  // compressed formats work), its view with `components`.
  Image CreateTexture(uint32_t w, uint32_t h, VkFormat format, uint32_t levels,
                      const std::string& name, const VkComponentMapping& components) {
    Image im;
    im.levels = levels;
    im.width = w;
    im.height = h;
    im.format = format;
    im.name = name;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = {w, h, 1};
    ici.mipLevels = levels;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    im.usage = ici.usage;
    vk::Check(vk::vkCreateImage(device, &ici, nullptr, &im.image), "vkCreateImage(texture)");
    VkMemoryRequirements req;
    vk::vkGetImageMemoryRequirements(device, im.image, &req);
    const uint32_t type = MemoryType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    int block = -1;
    VkDeviceSize offset = 0;
    if (SubAllocate(req, type, &block, &offset)) {
      im.block = block;
      im.block_offset = offset;
      im.block_size = req.size;
      im.memory = blocks[size_t(block)].memory;
      vk::vkBindImageMemory(device, im.image, im.memory, offset);
    } else {
      VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
      mai.allocationSize = req.size;
      mai.memoryTypeIndex = type;
      vk::Check(vk::vkAllocateMemory(device, &mai, nullptr, &im.memory),
                "vkAllocateMemory(texture)");
      vk::vkBindImageMemory(device, im.image, im.memory, 0);
    }
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = format;
    vci.components = components;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, levels, 0, 1};
    vk::Check(vk::vkCreateImageView(device, &vci, nullptr, &im.view), "vkCreateImageView(texture)");
    im.sample_view = im.view;
    return im;
  }

  void DestroyBuffer(Buffer& b) {
    if (b.buffer) vk::vkDestroyBuffer(device, b.buffer, nullptr);
    if (b.memory) vk::vkFreeMemory(device, b.memory, nullptr);
    b = Buffer{};
  }

  // Views destroyed since the renderer last looked (its cached texture
  // descriptor sets must not use them again).
  std::vector<VkImageView> destroyed_views;

  void DestroyImage(Image& im) {
    if (im.sample_view) destroyed_views.push_back(im.sample_view);
    if (im.stencil_view) vk::vkDestroyImageView(device, im.stencil_view, nullptr);
    if (im.sample_view && im.sample_view != im.view) vk::vkDestroyImageView(device, im.sample_view, nullptr);
    if (im.view) vk::vkDestroyImageView(device, im.view, nullptr);
    if (im.image) vk::vkDestroyImage(device, im.image, nullptr);
    if (im.block >= 0) {
      SubFree(im.block, im.block_offset, im.block_size);
    } else if (im.memory) {
      vk::vkFreeMemory(device, im.memory, nullptr);
    }
    im = Image{};
  }

  // The pipeline cache from `path`, when it was made by this GPU and driver
  // (checked here: some drivers misbehave on foreign data).
  void CreatePipelineCache(const std::string& path) {
    std::vector<uint8_t> data;
    if (!path.empty()) {
      if (FILE* f = std::fopen(path.c_str(), "rb")) {
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        if (n > 32 && n < (512l << 20)) {
          data.resize(size_t(n));
          if (std::fread(data.data(), 1, data.size(), f) != data.size()) data.clear();
        }
        std::fclose(f);
      }
    }
    if (!data.empty()) {
      uint32_t header[4];
      std::memcpy(header, data.data(), sizeof(header));
      const bool ours = header[0] >= 32 && header[1] == VK_PIPELINE_CACHE_HEADER_VERSION_ONE &&
                        header[2] == props.vendorID && header[3] == props.deviceID &&
                        std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE) == 0;
      if (!ours) data.clear();
    }
    VkPipelineCacheCreateInfo ci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    ci.initialDataSize = data.size();
    ci.pInitialData = data.empty() ? nullptr : data.data();
    if (vk::vkCreatePipelineCache(device, &ci, nullptr, &pipeline_cache) != VK_SUCCESS) {
      ci.initialDataSize = 0;
      ci.pInitialData = nullptr;
      if (vk::vkCreatePipelineCache(device, &ci, nullptr, &pipeline_cache) != VK_SUCCESS) {
        pipeline_cache = VK_NULL_HANDLE;
      }
    }
    pipeline_cache_loaded = data.size();
  }
  size_t pipeline_cache_loaded = 0;

  // Writes the pipeline cache to `path` (through a temporary file, so a crash
  // midway leaves the old one). Safe from any thread.
  bool SavePipelineCache(const std::string& path) const {
    if (path.empty() || !pipeline_cache) return false;
    size_t n = 0;
    if (vk::vkGetPipelineCacheData(device, pipeline_cache, &n, nullptr) != VK_SUCCESS || !n) {
      return false;
    }
    std::vector<uint8_t> data(n);
    if (vk::vkGetPipelineCacheData(device, pipeline_cache, &n, data.data()) != VK_SUCCESS) {
      return false;
    }
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) return false;
    const bool ok = std::fwrite(data.data(), 1, n, f) == n;
    if (std::fclose(f) != 0 || !ok) return false;
    std::remove(path.c_str());
    return std::rename(tmp.c_str(), path.c_str()) == 0;
  }

  void Shutdown() {
    if (!device) return;
    vk::vkDeviceWaitIdle(device);
    if (pipeline_cache) vk::vkDestroyPipelineCache(device, pipeline_cache, nullptr);
    pipeline_cache = VK_NULL_HANDLE;
    for (auto& b : blocks) {
      if (b.memory) vk::vkFreeMemory(device, b.memory, nullptr);
    }
    blocks.clear();
    if (pool) vk::vkDestroyCommandPool(device, pool, nullptr);
    if (frame_pool) vk::vkDestroyCommandPool(device, frame_pool, nullptr);
    vk::vkDestroyDevice(device, nullptr);
    if (instance) vk::vkDestroyInstance(instance, nullptr);
    device = VK_NULL_HANDLE;
    instance = VK_NULL_HANDLE;
  }

  static VkImageAspectFlags Aspects(const Image& im) {
    if (!im.depth) return VK_IMAGE_ASPECT_COLOR_BIT;
    return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
  }

  VkCommandBuffer BeginCommands(VkCommandPool from = VK_NULL_HANDLE) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = from ? from : pool;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    vk::Check(vk::vkAllocateCommandBuffers(device, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vk::vkBeginCommandBuffer(cb, &bi);
    return cb;
  }

  // Submits without waiting; `fence` signals when done. `first` (ended by
  // the caller, may be null) runs before `cb`.
  void SubmitFenced(VkCommandBuffer cb, VkFence fence, VkCommandBuffer first = VK_NULL_HANDLE) {
    vk::vkEndCommandBuffer(cb);
    VkCommandBuffer cbs[2] = {first, cb};
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = first ? 2 : 1;
    si.pCommandBuffers = first ? cbs : &cbs[1];
    PhaseScope phase("submitting the frame (vkQueueSubmit)");
    vk::Check(vk::vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
  }

  void Submit(VkCommandBuffer cb, VkCommandBuffer first = VK_NULL_HANDLE,
              VkCommandPool cb_pool = VK_NULL_HANDLE) {
    PhaseScope phase("a one-off GPU job (submit and wait)");
    if (first) {
      VkSubmitInfo fi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
      fi.commandBufferCount = 1;
      fi.pCommandBuffers = &first;
      vk::Check(vk::vkQueueSubmit(queue, 1, &fi, VK_NULL_HANDLE), "vkQueueSubmit");
      vk::Check(vk::vkQueueWaitIdle(queue), "vkQueueWaitIdle");
      vk::vkFreeCommandBuffers(device, pool, 1, &first);
    }
    vk::vkEndCommandBuffer(cb);
    // Profiling the CPU side under a tool that can't run the driver's GPU
    // work (callgrind with SwiftShader): AMPED3_PROFILE_NO_SUBMIT=1 records
    // everything and submits nothing (the pictures are garbage).
    static const bool no_submit = std::getenv("AMPED3_PROFILE_NO_SUBMIT") != nullptr;
    if (no_submit) {
      vk::vkFreeCommandBuffers(device, cb_pool ? cb_pool : pool, 1, &cb);
      return;
    }
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vk::Check(vk::vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    vk::Check(vk::vkQueueWaitIdle(queue), "vkQueueWaitIdle");
    vk::vkFreeCommandBuffers(device, cb_pool ? cb_pool : pool, 1, &cb);
  }
};

// textures_ key: the texture's resource name and fetch constant.
std::string TextureKey(const std::string& data, const amped3::native::TextureFetch& tf) {
  char key_buf[160];
  std::snprintf(key_buf, sizeof(key_buf), "%s.%08X.%08X.%08X.%08X", data.c_str(), tf.words[0],
                tf.words[1], tf.words[2], tf.words[3]);
  return key_buf;
}

// A 64-bit hash of the same (for the per-frame prefetch scan: no strings).
uint64_t TextureHash(const std::string& data, const amped3::native::TextureFetch& tf) {
  uint64_t h = 0xCBF29CE484222325ull ^ data.size();
  size_t i = 0;
  for (; i + 8 <= data.size(); i += 8) {
    uint64_t v;
    std::memcpy(&v, data.data() + i, 8);
    h = (h ^ v) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  for (; i < data.size(); ++i) h = (h ^ uint8_t(data[i])) * 0x100000001B3ull;
  for (int w = 0; w < 4; ++w) {
    h = (h ^ tf.words[w]) * 0x100000001B3ull;
    h ^= h >> 29;
  }
  return h;
}

// Reads a texture's guest data and decodes it (any thread).
std::vector<amped3::native::DecodedTexture> DecodeFromResources(
    FrameResources& res, const std::string& data, const amped3::native::TextureFetch& tf,
    bool keep_blocks) {
  auto bytes = res.Read(data + ".base");
  std::vector<uint8_t> mip_bytes;
  if (tf.mip_max_level() > 0) mip_bytes = res.Read(data + ".mips");
  return amped3::native::DecodeTextureMips(tf, bytes.data(), bytes.size(), mip_bytes.data(),
                                           mip_bytes.size(), keep_blocks);
}

double MsSince(std::chrono::steady_clock::time_point t) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
}

void FullBarrier(VkCommandBuffer cb) {
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  vk::vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
}

void ToGeneral(VkCommandBuffer cb, const Image& im) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = 0;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {Gpu::Aspects(im), 0, im.levels, 0, 1};
  vk::vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
}

// --- Xenos -> Vulkan state ------------------------------------------------------------

VkFormat ColorFormat(uint32_t xenos, std::string* name) {
  switch (xenos) {
    case 0: *name = "8_8_8_8"; return VK_FORMAT_R8G8B8A8_UNORM;
    case 1: *name = "8_8_8_8_GAMMA"; return VK_FORMAT_R8G8B8A8_SRGB;
    case 2:
    case 10: *name = "2_10_10_10"; return VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    case 3:
    case 12: *name = "2_10_10_10_FLOAT"; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 4: *name = "16_16"; return VK_FORMAT_R16G16_SNORM;
    case 5: *name = "16_16_16_16"; return VK_FORMAT_R16G16B16A16_SNORM;
    case 6: *name = "16_16_FLOAT"; return VK_FORMAT_R16G16_SFLOAT;
    case 7: *name = "16_16_16_16_FLOAT"; return VK_FORMAT_R16G16B16A16_SFLOAT;
    case 14: *name = "32_FLOAT"; return VK_FORMAT_R32_SFLOAT;
    case 15: *name = "32_32_FLOAT"; return VK_FORMAT_R32G32_SFLOAT;
    default: *name = "unknown"; return VK_FORMAT_R8G8B8A8_UNORM;
  }
}

VkBlendFactor BlendFactor(uint32_t f) {
  switch (f) {
    case 0: return VK_BLEND_FACTOR_ZERO;
    case 1: return VK_BLEND_FACTOR_ONE;
    case 4: return VK_BLEND_FACTOR_SRC_COLOR;
    case 5: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
    case 6: return VK_BLEND_FACTOR_SRC_ALPHA;
    case 7: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case 8: return VK_BLEND_FACTOR_DST_COLOR;
    case 9: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
    case 10: return VK_BLEND_FACTOR_DST_ALPHA;
    case 11: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case 12: return VK_BLEND_FACTOR_CONSTANT_COLOR;
    case 13: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
    case 14: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
    case 15: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
    case 16: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
    default: return VK_BLEND_FACTOR_ONE;
  }
}

VkBlendOp BlendOp(uint32_t op) {
  switch (op) {
    case 1: return VK_BLEND_OP_SUBTRACT;
    case 2: return VK_BLEND_OP_MIN;
    case 3: return VK_BLEND_OP_MAX;
    case 4: return VK_BLEND_OP_REVERSE_SUBTRACT;
    default: return VK_BLEND_OP_ADD;
  }
}

VkSamplerAddressMode AddressMode(uint32_t clamp) {
  switch (clamp) {
    case 0: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case 1: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
    case 6:
    case 7: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  }
}

// Render target identity: EDRAM base + format + pitch.
struct TargetKey {
  bool depth = false;
  uint32_t base = 0, format = 0, pitch = 0;
  uint32_t samples = 1;  // guest MSAA samples (RB_SURFACE_INFO msaa_samples)
  uint32_t scale_id = 0;  // the scale it is drawn at (x 80), filled in by GetTarget
  bool operator<(const TargetKey& o) const {
    return std::tie(depth, base, format, pitch, samples, scale_id) <
           std::tie(o.depth, o.base, o.format, o.pitch, o.samples, o.scale_id);
  }
};

uint32_t SurfaceSamples(uint32_t surface_info) { return 1u << std::min((surface_info >> 16) & 3, 2u); }

TargetKey ColorKey(uint32_t surface_info, uint32_t color_info) {
  return {false, color_info & 0xFFF, (color_info >> 16) & 15, surface_info & 0x3FFF,
          SurfaceSamples(surface_info)};
}
TargetKey DepthKey(uint32_t surface_info, uint32_t depth_info) {
  return {true, depth_info & 0xFFF, (depth_info >> 16) & 1, surface_info & 0x3FFF,
          SurfaceSamples(surface_info)};
}

struct SystemConstants {  // std140 layout of XeSystem
  float ndc_scale[4];
  float ndc_offset[4];
  uint32_t flags;
  uint32_t vertex_index_offset;
  uint32_t vertex_index_mask;
  uint32_t index_load_address;
  uint32_t alpha_func;
  float alpha_ref;
  uint32_t param_gen_register;
  float point_size;
  float res_scale;
  uint32_t scaled_textures;  // VS bindings in bits 0..15, PS in 16..31
  float rt_inv_size[2];      // 1 / render target size (std140: the array starts at 80)
  float tex_scale[32];       // per binding (VS 0..15, PS 16..31): 1, or the image's scale
  float reproject[16];       // VS, XE_FLAG_REPROJECT (16384): column-major mat4
  float wind_m[16];          // VS, wind (wind2[0] > 0): c0..c3 (column-major)
  float wind_minv[16];       // its inverse
  float wind[4];             // direction (x, z) in its space, strength (m), clock (s)
  float wind2[4];            // mode (1 leaves, 2 tree, 3 cloth), units a metre, seed, dt
  uint32_t material[4];      // PS: x the surface tagged into oC1.w's high bits (0: none)
};
static_assert(offsetof(SystemConstants, tex_scale) == 80, "XeSystem std140 layout");

// A texture's surface from its colors (the base level: RGBA texels, or BC1-3
// blocks' end colors, through the view's swizzle): 1 snow (bright, grey),
// 2 metal (mid grey, even), 3 wood (brown), 4 rock (grey, varied), 5 paint
// (strong colors), 0 anything else.
uint8_t SurfaceOf(const amped3::native::DecodedTexture& t, const VkComponentMapping& view) {
  double sum[3] = {}, sum_l = 0, sum_l2 = 0;
  size_t n = 0;
  auto add = [&](double r, double g, double b) {
    double c[3] = {r, g, b};
    double o[3];
    const VkComponentSwizzle sw[3] = {view.r, view.g, view.b};
    for (int i = 0; i < 3; ++i) {
      switch (sw[i]) {
        case VK_COMPONENT_SWIZZLE_R: o[i] = c[0]; break;
        case VK_COMPONENT_SWIZZLE_G: o[i] = c[1]; break;
        case VK_COMPONENT_SWIZZLE_B: o[i] = c[2]; break;
        case VK_COMPONENT_SWIZZLE_ONE: o[i] = 1.0; break;
        case VK_COMPONENT_SWIZZLE_ZERO: o[i] = 0.0; break;
        default: o[i] = c[i]; break;
      }
    }
    for (int i = 0; i < 3; ++i) sum[i] += o[i];
    const double l = 0.3 * o[0] + 0.59 * o[1] + 0.11 * o[2];
    sum_l += l;
    sum_l2 += l * l;
    ++n;
  };
  if (t.bc == 0) {
    const size_t px = t.rgba.size() / 4;
    const size_t step = std::max<size_t>(1, px / 4096);
    for (size_t i = 0; i < px; i += step) {
      add(t.rgba[i * 4] / 255.0, t.rgba[i * 4 + 1] / 255.0, t.rgba[i * 4 + 2] / 255.0);
    }
  } else if (t.bc <= 3) {
    const size_t bs = t.bc == 1 ? 8 : 16, off = t.bc == 1 ? 0 : 8;
    const size_t blocks = t.rgba.size() / bs;
    const size_t step = std::max<size_t>(1, blocks / 2048);
    for (size_t i = 0; i < blocks; i += step) {
      const uint8_t* b = t.rgba.data() + i * bs + off;
      for (int e = 0; e < 2; ++e) {
        const uint32_t c = uint32_t(b[e * 2]) | uint32_t(b[e * 2 + 1]) << 8;
        add(((c >> 11) & 31) / 31.0, ((c >> 5) & 63) / 63.0, (c & 31) / 31.0);
      }
    }
  } else {
    return 0;
  }
  if (n < 8) return 0;
  const double r = sum[0] / n, g = sum[1] / n, b = sum[2] / n;
  const double l = sum_l / n;
  const double var = std::max(sum_l2 / n - l * l, 0.0);
  const double sd = std::sqrt(var);
  const double mx = std::max({r, g, b}), mn = std::min({r, g, b});
  const double sat = mx > 1e-3 ? (mx - mn) / mx : 0.0;
  if (l > 0.62 && sat < 0.14) return 1;                             // snow
  if (sat > 0.45 && mx > 0.35) return 5;                            // paint, signs
  if (r > g && g > b && sat > 0.2 && sat < 0.75 && l < 0.55) return 3;  // wood
  if (sat < 0.12 && l > 0.12 && l < 0.6) return sd < 0.07 ? 2 : 4;    // metal / rock
  return 0;
}

// --- Replayer -------------------------------------------------------------------


// Amped 3's deferred lighting pixel shader (camera motion blur reads its
// constants and textures).
constexpr uint64_t kLightingPixelShader = 0xC17CD159D811B298ull;
// The scene pass's trees and objects (vertex shaders), drawn again from the
// sun for tree shadows: trunks and foliage, distant trees, rocks and props.
// Not the terrain (it would shade itself everywhere).
bool IsShadowCaster(uint64_t vs_hash) {
  return vs_hash == 0x5919B6B651142594ull || vs_hash == 0x9BAF953BF548C874ull ||
         vs_hash == 0xBEF747C713765676ull || vs_hash == 0x8D6CC76801E89D1Aull;
}
// Wind (time of day): how a draw's vertices move, by its vertex shader.
// 1: leaves and needles in world space (c0..c3 the view-projection: the
// trees' foliage cards, each tree an instance: bent from where it stands,
// with its trunk); 2: a whole tree placed by c0..c3 (object -> clip: the
// clustered and distant trees), bending from its foot. 5: a tree's trunk
// (the world's instanced meshes with a tree's texture: told in the draw).
// 0: not moved.
int WindMode(uint64_t vs_hash) {
  if (vs_hash == 0x9BAF953BF548C874ull) return 1;
  if (vs_hash == 0x8D6CC76801E89D1Aull) return 2;
  return 0;
}
// Cloth: banners and flags, told by their texture (ContentHash), among the
// world's static meshes (vertex shader 5919...: c0..c3 the view-projection)
// and its placed props (bef7...: c0..c3 their world matrix). 3 / 4.
constexpr uint64_t kWorldMeshVs = 0x5919B6B651142594ull, kPropVs = 0xBEF747C713765676ull;
bool IsCloth(uint64_t content, const std::vector<uint64_t>& extra) {
  static constexpr uint64_t kCloth[] = {
      0x3744FCC1E61D318Eull,  // Northstar: the Method banners
      0x62E4858827629B7Dull,  // the 2K Sports banners
      0x95241613D9084EE7ull,  // the red banner
  };
  for (uint64_t c : kCloth) {
    if (c == content) return true;
  }
  return std::find(extra.begin(), extra.end(), content) != extra.end();
}
// The snow trails' vertex shader (it projects into the shadow map, c9-c12).
constexpr uint64_t kTrailVertexShader = 0xCBEB0B975321F043ull;
// The shadow map's blur (two passes over the map read as 8_8_8_8).
constexpr uint64_t kShadowBlurPixelShader = 0x7A84B7E8B62F305Eull;
// The shadow pass's terrain's pixel shader (what it draws doesn't cast).
constexpr uint64_t kTerrainShadowPixelShader = 0x515E86DBD1D61732ull;

struct Shader {
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderInfo info;
  std::string error;
  std::vector<uint8_t> vfetch;  // the vertex fetch constants it reads (info's bits as a list)
  // How to make it again (the pipeline list for precompiling): 1 a vertex
  // shader, 2 a pixel shader, 3 a velocity vertex shader, from the microcode
  // with this hash; 4 the velocity, 5 the exemption pixel shader (built in).
  // 0: not listed.
  uint8_t recipe = 0;
  uint64_t recipe_hash = 0;
  int safe_level = 0;  // TranslateOptions::safe_level it was made with
  std::string glsl;    // pixel shaders: the source (for the depth-only variant)
};

std::string fmt_line(const char* f, ...) {
  char buf[256];
  va_list a;
  va_start(a, f);
  const int n = std::vsnprintf(buf, sizeof(buf), f, a);
  va_end(a);
  return std::string(buf, size_t(std::clamp(n, 0, int(sizeof(buf)) - 1)));
}

uint64_t HashBytes(const uint8_t* p, size_t n) {
  uint64_t h = 1469598103934665603ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// Texture fetch constant words for tf<fc>: the draw's texture line when there
// is one (older captures have no fetch lines), else the fetch constants.
const uint32_t* FetchWords(const Draw& draw, uint32_t fc, const GpuState& st) {
  for (const auto& t : draw.textures) {
    if (t.slot == fc) return t.raw.data();
  }
  return &st.fetch[fc * 6];
}

// Bits per texel of formats a resolve can write (0 = never a resolve
// destination: compressed, YUV, ...).
uint32_t ResolvableBits(uint32_t format) {
  switch (format) {
    case 2: case 8: case 9:
      return 8;
    case 3: case 4: case 5: case 10: case 15: case 24: case 30:
      return 16;
    case 6: case 7: case 14: case 16: case 17: case 22: case 23: case 25: case 31: case 36:
      return 32;
    case 21: case 26: case 32: case 37:
      return 64;
    case 38:
      return 128;
    default:
      return 0;
  }
}

// A texture reads a resolved image when its format has the resolve's texel
// size (a resolve as k_8_8_8_8 sampled as k_8_8_8_8 or k_2_10_10_10, say).
bool ResolveCompatible(uint32_t resolve_format, uint32_t texture_format) {
  const uint32_t a = ResolvableBits(resolve_format), b = ResolvableBits(texture_format);
  return a != 0 && a == b;
}

}  // namespace

struct Renderer::Impl {
  explicit Impl(RendererOptions options) : options_(std::move(options)) {
    debug_stores_ = Diag("posdump");
    scale_ = std::clamp(std::round(options_.resolution_scale * 80.0f) / 80.0f, 0.25f, 4.0f);
    out_scale_ = options_.output_scale > 0.0f
                     ? std::clamp(std::round(options_.output_scale * 80.0f) / 80.0f, 0.25f, 4.0f)
                     : scale_;
    if (options_.upscaler) {
      upscaler_ = amped3::native::Upscaler::Create(
          amped3::native::UpscalerKind(options_.upscaler), &upscaler_status_);
    }
    gpu_.Init(options_.device_index, options_.match_device_uuid ? options_.device_uuid : nullptr,
              &upscaler_, &upscaler_status_);
    gpu_.CreatePipelineCache(options_.pipeline_cache_path);
    {
      const std::string& pcp = options_.pipeline_cache_path;
      const size_t slash = pcp.find_last_of("/\\");
      if (slash != std::string::npos) {
        bake_dir_ = pcp.substr(0, slash) + "/world_shadows";
        precompile_dir_ = pcp.substr(0, slash) + "/native_pipelines";
      }
    }
    serial_pipes_ = (gpu_.device_name.find("Adreno") != std::string::npos ||
                     gpu_.device_name.find("Turnip") != std::string::npos || Diag("serialpipes")) &&
                    !Diag("parallelpipes");
    bit_zero_mul_ = (gpu_.device_name.find("Adreno") != std::string::npos ||
                     gpu_.device_name.find("Turnip") != std::string::npos || Diag("bitmulz")) &&
                    !Diag("floatmulz");
    if (serial_pipes_) {
      pending_notices_.push_back("pipelines built one at a time (" + gpu_.device_name +
                                 ": its driver crashes building several at once)");
    }
    ReadCrashed();
    LoadCloudSprite();
    if (options_.texture_pack || options_.hd_fonts || options_.upscaled_textures) {
      const std::string& pc = options_.pipeline_cache_path;
      const size_t slash = pc.find_last_of("/\\");
      replacer_ = std::make_unique<amped3::native::TextureReplacer>(
          options_.texture_pack_folder, options_.texture_pack, options_.hd_fonts,
          slash == std::string::npos ? std::string() : pc.substr(0, slash),
          options_.upscaled_textures);
      if (options_.texture_pack) {
        pending_notices_.push_back(
            "texture pack: " + std::to_string(replacer_->pack_size()) + " replacement(s) in " +
            (options_.texture_pack_folder.empty() ? std::string("(no folder)")
                                                  : options_.texture_pack_folder));
      }
      if (options_.upscaled_textures && replacer_->upscaled_size()) {
        pending_notices_.push_back("upscaled textures: " +
                                   std::to_string(replacer_->upscaled_size()) + " available");
      }
    }
    smooth_alpha_ = options_.smooth_alpha && !Diag("hardalpha");
    index_cache_ = !Diag("noixcache");
    foliage_fade_ = options_.foliage_fade && !Diag("nofade");
    {
      VkDeviceSize local = 0;
      for (uint32_t i = 0; i < gpu_.mem_props.memoryHeapCount; ++i) {
        if (gpu_.mem_props.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) {
          local = std::max(local, gpu_.mem_props.memoryHeaps[i].size);
        }
      }
      texture_budget_ = uint64_t(options_.texture_budget_mb) << 20;
      if (local) texture_budget_ = std::min<uint64_t>(texture_budget_, local / 2);
    }
    cache_saved_at_ = std::chrono::steady_clock::now();
    if (upscaler_) upscaler_status_ = upscaler_->description();
    cb_ = gpu_.BeginCommands();
    CreateOnce();
    gpu_.Submit(cb_, TakeUploads());
    cb_ = VK_NULL_HANDLE;
  }
  ~Impl();

  RenderStats Render(const Frame& frame, FrameResources& resources, uint32_t max_draws,
                     const Renderer::OutputRequest* output, OutputInfo* info);
  // Async frames in flight: each owns the per-frame buffers, descriptor pool,
  // uploads and replaced images until its fence signals.
  struct FrameSlot {
    Buffer uniforms, memory, indices, readback;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    uint32_t pool_draws = 0;
    std::vector<Buffer> staging;
    Buffer upload;
    std::vector<Image> retired;
    VkFence fence = VK_NULL_HANDLE;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    VkCommandBuffer upload_cb = VK_NULL_HANDLE;  // ran before cb (texture uploads)
    VkCommandPool cb_pool = VK_NULL_HANDLE;      // where cb came from
    bool pending = false;
    uint64_t ticket = 0;
    bool timed = false;  // GPU timestamps at the frame's start and end (ts_pool_)
    // GPU time by part (part_pool_): the name of what ended at each timestamp
    // after the first.
    std::vector<std::string> part_names;
    // The light editor's object under the cross (pick_pool_): the object
    // each query's draw placed (key 0: the world itself), how many queries.
    std::vector<Renderer::Instance> pick_objs;
    std::vector<std::string> pick_why;  // (an object whose placement wasn't recognised: its draw)
    uint32_t pick_count = 0;
  };
  // Texture uploads of the frame being recorded: their own command buffer,
  // submitted before the frame's, so an upload never ends a render pass.
  VkCommandBuffer upload_cb_ = VK_NULL_HANDLE;
  VkCommandBuffer UploadCommands() {
    if (!upload_cb_) upload_cb_ = gpu_.BeginCommands();
    return upload_cb_;
  }
  // Ends the uploads (made visible to everything after); null if none.
  VkCommandBuffer TakeUploads() {
    VkCommandBuffer cb = upload_cb_;
    upload_cb_ = VK_NULL_HANDLE;
    if (cb) {
      FullBarrier(cb);
      vk::vkEndCommandBuffer(cb);
    }
    return cb;
  }
  VkQueryPool part_pool_ = VK_NULL_HANDLE;  // kMaxParts timestamps per slot
  static constexpr uint32_t kMaxParts = 64;
  FrameSlot* cur_slot_ = nullptr;           // the frame being recorded (async)
  // Timestamp after what was recorded so far, named (outside a rendering scope).
  void MarkPart(const std::string& name);
  FrameSlot slots_[2];
  VkQueryPool ts_pool_ = VK_NULL_HANDLE;  // two timestamps per slot
  uint32_t next_slot_ = 0;
  uint64_t next_ticket_ = 0;
  void SwapSlot(FrameSlot& fs) {
    std::swap(uniforms_, fs.uniforms);
    std::swap(memory_, fs.memory);
    std::swap(indices_, fs.indices);
    std::swap(pool_, fs.pool);
    std::swap(pool_draws_, fs.pool_draws);
    std::swap(staging_, fs.staging);
    std::swap(upload_, fs.upload);
    std::swap(retired_images_, fs.retired);
  }
  // Waits for a slot's frame and frees what it held for the GPU.
  double FinishSlot(FrameSlot& fs) {
    if (!fs.pending) return 0;
    const auto t0 = std::chrono::steady_clock::now();
    PhaseScope phase("waiting for an earlier frame's GPU work (its fence)");
    vk::Check(vk::vkWaitForFences(gpu_.device, 1, &fs.fence, VK_TRUE, UINT64_MAX), "fence");
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - t0).count();
    vk::vkResetFences(gpu_.device, 1, &fs.fence);
    vk::vkFreeCommandBuffers(gpu_.device, fs.cb_pool ? fs.cb_pool : gpu_.pool, 1, &fs.cb);
    fs.cb = VK_NULL_HANDLE;
    if (fs.upload_cb) vk::vkFreeCommandBuffers(gpu_.device, gpu_.pool, 1, &fs.upload_cb);
    fs.upload_cb = VK_NULL_HANDLE;
    fs.pending = false;
    for (auto& b : fs.staging) gpu_.DestroyBuffer(b);
    fs.staging.clear();
    for (auto& im : fs.retired) gpu_.DestroyImage(im);
    fs.retired.clear();
    return ms;
  }
  bool ReadRGBA(Image* im, std::vector<uint8_t>* rgba);
  bool ReadStencil(Image* im, std::vector<uint8_t>* rgba);
  bool ReadDepth(Image* im, std::vector<float>* depth);
  // A single-sample copy of a multisampled image for readback: the average of
  // all samples (color), or sample 0's depth as R32F (depth). The caller
  // destroys it.
  Image Downsample(Image* im);

  Shader* GetShader(const std::string& stem, ShaderStage stage);
  VkShaderModule CompileBuiltin(const std::string& name, const std::string& source, bool frag);
  Image* GetTarget(const TargetKey& key, bool resolve = false);
  Image* GetTargetUncached(const TargetKey& key, bool resolve);
  struct TargetCacheEntry {
    TargetKey key;
    uint32_t scale_id = 0;
    uint32_t* last_scale = nullptr;  // its target_last_scale_ entry (map nodes are stable)
    Image* image = nullptr;
  };
  TargetCacheEntry target_cache_[4];
  uint32_t target_cache_next_ = 0;
  // The scale_id each surface (TargetKey without scale) was last drawn or
  // cleared at.
  std::map<TargetKey, uint32_t> target_last_scale_;
  // A draw's texture (fetch constant fc) by its contents' hash (TextureHash,
  // as replacements are named): 0 if it has none.
  uint64_t DrawTextureHash(const Draw& draw, const GpuState& st, uint32_t fc) {
    amped3::native::TextureFetch tf;
    std::memcpy(tf.words, FetchWords(draw, fc, st), sizeof(tf.words));
    for (const auto& t : draw.textures) {
      if (t.slot == fc) return TextureHash(t.data, tf);
    }
    return 0;
  }
  // A draw's texture by its content (ContentHash: stable across sessions),
  // once it's been loaded (0 before, or not block-compressed).
  uint64_t DrawContentHash(const Draw& draw, const GpuState& st, uint32_t fc) {
    amped3::native::TextureFetch tf;
    std::memcpy(tf.words, FetchWords(draw, fc, st), sizeof(tf.words));
    for (const auto& t : draw.textures) {
      if (t.slot != fc || t.data.empty()) continue;
      auto it = textures_.find(TextureKey(t.data, tf));
      if (it == textures_.end()) return 0;
      if (!it->second.content_known) {
        it->second.content_known = true;
        auto bytes = res_->Read(t.data + ".base");
        it->second.content = amped3::native::TextureReplacer::ContentHash(tf, bytes.data(), bytes.size());
      }
      return it->second.content;
    }
    return 0;
  }
  Image* TextureFor(const Draw& draw, uint32_t fetch_constant, const GpuState& st,
                    std::string* note);
  // `lod_bias`: added to the mip level (sharper textures for a scene drawn
  // below the output resolution and upscaled).
  VkSampler SamplerFor(const uint32_t* fetch, const amped3::native::TextureBinding& b,
                       float lod_bias = 0.0f);
  VkPipeline PipelineFor(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                         const std::vector<Image*>& colors, Image* depth, uint32_t color_mask_bits,
                         bool velocity = false, bool mask = false, bool reactive = false,
                         bool caster = false);
  void Prepass();
  void CreateOnce();
  void BeginFrame();
  void EndFrame();
  void BeginRendering(const std::vector<Image*>& colors, Image* depth);
  void EndRendering();
  void DoDraw(const Draw& d, uint32_t number);
  void DoClear(const Clear& c);
  void DoResolve(const Resolve& r, uint32_t number);
  void ClearImage(Image* im, const float* color, float z, uint32_t stencil, bool c, bool zf, bool s,
                  const VkRect2D* rect = nullptr);
  uint32_t WriteUniform(const void* data, size_t size);
  // Destroyed once the frame using it has finished on the GPU.
  void Retire(Image& im) {
    if (im.image) retired_images_.push_back(im);
    im = Image{};
  }
  // Diagnostics limited to draws options_.diag_first..diag_last (1-based).
  bool DiagRange() const {
    return options_.diag_last && current_draw_ >= options_.diag_first &&
           current_draw_ <= options_.diag_last;
  }
  bool Diag(const char* name) const {
    const std::string& d = options_.diag;
    const size_t n = std::strlen(name);
    for (size_t at = 0; at <= d.size();) {
      size_t comma = d.find(',', at);
      if (comma == std::string::npos) comma = d.size();
      if (comma - at == n && d.compare(at, n, name) == 0) return true;
      at = comma + 1;
    }
    return false;
  }
  void Skip(uint32_t number, const std::string& why) {
    uint32_t& n = stats_.skipped[why];
    if (++n <= 3) log_ << "[" << number << "] skipped: " << why << "\n";
  }

  RendererOptions options_;
  std::unique_ptr<amped3::native::TextureReplacer> replacer_;  // null: none
  std::vector<std::string> pending_notices_;  // for the next frame's stats
  Image* UploadReplacement(const std::string& key, const std::string& data,
                           const amped3::native::TextureFetch& tf,
                           const amped3::native::Replacement& rep, std::string* note);
  Gpu gpu_;
  const Frame* frame_ = nullptr;
  FrameResources* res_ = nullptr;
  // The GpuState whose constants were last written, and where.
  // Uploaded uniform blocks this frame (reset in BeginFrame): constants by
  // block version and shader (only the constants a shader reads are written),
  // and the last bool/loop, fetch and system blocks.
  struct ConstUpload {
    uint64_t version = 0;
    const Shader* shader = nullptr;
    uint32_t offset = 0;
  };
  std::array<ConstUpload, 8> const_uploads_[2];
  uint32_t const_upload_next_[2] = {};
  // The same brought to the hour (time of day: TodApply), offset UINT32_MAX
  // when nothing changed: draw after draw of one shader with the same
  // constants (a rider's parts, the props) share one.
  std::array<ConstUpload, 8> tod_uploads_[2];
  uint32_t tod_upload_next_[2] = {};
  bool TodUploadFind(int stage, uint64_t version, const Shader* sh, uint32_t* offset) const {
    for (const ConstUpload& c : tod_uploads_[stage]) {
      if (c.shader == sh && c.version == version) {
        *offset = c.offset;
        return true;
      }
    }
    return false;
  }
  void TodUploadKeep(int stage, uint64_t version, const Shader* sh, uint32_t offset) {
    tod_uploads_[stage][tod_upload_next_[stage]++ & 7] = {version, sh, offset};
  }
  uint64_t bool_loop_version_ = 0;
  uint32_t bool_loop_offset_ = 0;
  uint32_t last_fetch_[192] = {};
  uint32_t last_fetch_offset_ = UINT32_MAX;
  SystemConstants last_sys_{};
  uint32_t last_sys_offset_ = UINT32_MAX;
  uint32_t ReserveUniform(size_t size, uint8_t** out);
  uint32_t ReserveConstants(size_t used, uint8_t** out);
  uint32_t ConstantsFor(int stage, const StateBlock<1024>& block, const Shader& shader);
  // Internal resolution scale (options_.resolution_scale), and a guest size
  // or coordinate in it (rounded to the nearest pixel, the same everywhere).
  float scale_ = 1.0f;
  // With an upscaler: the output's scale, for everything drawn outside the
  // scene's tiled pass (the composite, the HUD, menus); else scale_.
  float out_scale_ = 1.0f;
  // The 3D scale for the 3D scene's tiled pass; the output scale for
  // everything else, including tiled passes without the scene (the menus are
  // drawn in one: with an upscaler they'd otherwise stay at the 3D scale).
  // Whether a tiled pass is the scene is known once its lighting pass is
  // drawn, so it goes by the last frame.
  // The scale a render target is drawn at: the 3D scene's inside its tiled
  // pass, else the output's; the sun's shadow map (drawn before the scene,
  // about 500 draws, see ShadowMapTarget) at the Shadow resolution setting,
  // or the scene's without one.
  float TargetScale(const TargetKey& k) const {
    if (tiling_.width) return tiled_scale_;
    if (ShadowMapTarget(k)) return shadow_res_ > 0.0f ? shadow_res_ : scale_;
    return out_scale_;
  }
  // Amped 3's sun shadow map: drawn before the scene into a surface the
  // scene's size (1280 x 720), from the sun through a perspective warp that
  // follows the camera (about 40 m of the slope ahead fill it); depth plus a
  // stencil ID per kind of object, read back as 8_8_8_8, blurred twice and
  // looked up by the lighting pass.
  bool ShadowMapTarget(const TargetKey& k) const {
    if (tiling_.width || scene_begun_ || !last_frame_lit_ || !last_tiling_.width ||
        k.samples != 1 || k.pitch != last_tiling_.width) {
      return false;
    }
    TargetKey hk = k;
    hk.scale_id = 0;
    auto h = target_heights_.find(hk);
    return h != target_heights_.end() && h->second == last_tiling_.height;
  }
  // Shadow settings (SetShadows): the map's scale (0: the scene's) and how
  // far it reaches (metres, 0: the game's).
  float shadow_res_ = 0.0f, shadow_distance_ = 0.0f;
  // This frame's reach, worked out from the last frame's lighting pass:
  // the map's v remapped as v' = av (v - 1) + 1 (the edge at the camera
  // kept, the far edge pushed out), and the lighting's shadow fade (c10, c11:
  // raw scene depths where shadows fade out) scaled by fade.
  float shadow_av_ = 1.0f, shadow_fade_ = 1.0f;
  // Sun shadows (DrawSunShadows): the game's shadow map filtered again
  // with penumbras that widen away from the caster (SetShadowSoftness), and
  // the world's shadows baked again (SetWorldShadows) from maps of the world
  // seen from the sun that stay put and keep everything drawn since.
  int world_detail_ = 0;          // World shadows: the bake's size (x the painted map), 0 = off
  float shadow_softness_ = 0.0f;  // penumbra scale, 0 = an even blur
  bool SunShadowsOn() const { return world_detail_ > 0 || shadow_softness_ > 0.0f; }
  struct CasterDraw {
    VkPipeline pipeline;
    VkDescriptorSet tex_sets[2];
    uint32_t offsets[5];
    bool indexed;
    VkIndexType index_type;
    VkBuffer index_buffer;
    uint32_t first, count;
    SystemConstants sys;  // as drawn; reprojected when drawn again
  };
  std::vector<CasterDraw> env_draws_;     // what stays put (scene pass)
  std::vector<CasterDraw> height_draws_;  // the same without the trees
  std::unordered_set<uint64_t> shadow_caster_vs_;
  std::map<uint64_t, uint32_t> sun_rec_counts_;  // diagnostics ("sunrec")  // the shadow pass's casters' shaders
  std::vector<std::array<double, 16>> light_candidates_;  // shadow pass c0-c3
  // This frame's shadow pass: its depth target, the light's view-projection
  // (rows; the terrain's vertex shader has it in c0-c3) and viewport.
  Image* sun_depth_target_ = nullptr;
  bool sun_float_depth_ = false;
  bool sun_light_ok_ = false, sun_prepared_ = false, sun_inputs_ok_ = false;
  bool sun_depth_ok_ = false;
  double sun_light_vp_[16] = {};
  SystemConstants sun_light_sys_{};
  // Kept at the first blur: the unblurred map, its depth, the terrain's.
  Image sun_raw_, sun_zo_, sun_prep_, sun_out_;
  // The sun maps: depth along the sun (orthographic), centred near the
  // camera, moved a whole number of texels at a time, never cleared while
  // the camera stays near: the trees (and props), and the ground. Their size
  // follows the Shadow resolution setting (1024 texels a side at 1x).
  static constexpr double kSunDepth = 2000.0;  // metres the depth spans
  Image sun_env_, sun_height_, sun_scratch_;
  Image sun_seen_, sun_seen_scratch_;  // R8: rays whose sunward side has been in view
  uint64_t sun_world_drawn_ = 0;  // frames that drew into the world map
  bool sun_map_ok_ = false, sun_frame_ok_ = false;
  // Updates (SunCycle): -1 none this frame; 0-2 a third of the world's draws
  // (by their order this frame); 3 all of them.
  int sun_phase_ = -1, sun_next_phase_ = 0;
  uint32_t sun_draw_seq_ = 0;
  double sun_cycle_cam_[3] = {}, sun_cycle_dir_[3] = {};
  void SunCycle(const double cam[3], const double dir[3]);
  // The game's shadow map (this frame): world -> its texel (u, v, depth), and
  // its depth per metre towards the sun (0: unknown): the riders' own
  // shadows (ShadedMovers).
  double sun_tex_from_world_[16] = {};
  double sun_depth_per_m_ = 0.0;
  Image sun_tiles_, sun_tiles2_;
  // World shadows kept on disk (per mountain, per size): loaded when the
  // mountain's painted map is first seen, saved as more is baked.
  bool bake_saved_only_ = false;   // SetWorldShadows: no updates, what's saved
  std::string bake_dir_;
  uint64_t bake_key_ = 0;          // the painted map's TextureHash
  bool bake_dirty_ = false;
  uint64_t bake_last_save_ = 0;    // frame
  struct BakeIo {
    Buffer buffer;                 // host: download or upload
    std::thread thread;
    std::atomic<int> state{0};     // 0 idle, 1 waiting for the GPU (save), 2 working, 3 done
    bool upload = false;
    bool ok = false;
    uint64_t frame = 0;
    uint32_t w = 0, h = 0;
    uint32_t sig = 0;              // the settings it was baked with (BakeSignature)
    std::string path;
  };
  BakeIo bake_io_;
  // The settings a bake depends on (World shadows, Shadow softness, Shadow
  // resolution): saved bakes made with others are deleted when they change,
  // and what's baked from then on is saved as the current bake.
  uint32_t BakeSignature() const;
  void CheckBakeSettings();
  uint32_t bake_sig_applied_ = 0;
  std::vector<std::pair<uint64_t, Buffer>> bake_retired_;  // (frame, buffer)
  bool bake_loaded_ = false;
  float bake_c5_[4] = {};  // the saved one is in (no updates needed in saved-only mode)
  void BakeIoTick(Image* src);
  void BakeSave();
  std::string BakePath(uint32_t w, uint32_t h) const;
  double sun_f_[3] = {}, sun_e1_[3] = {}, sun_e2_[3] = {}, sun_c0_[3] = {};
  int64_t sun_ci_ = 0, sun_cj_ = 0, sun_hi_ = 0, sun_hj_ = 0;
  double sun_r_ = 0.0, sun_ref_ = 0.0, sun_href_ = 0.0, sun_last_cam_[3] = {};
  double sun_O_[16] = {};  // this frame's sun map rows
  // World shadows: the slopes' shadow map baked again (BakeWorldShadows), a
  // copy the lighting reads instead of its tf2.
  Image baked_;
  // Over a step of the time of day's sun: the part of the world's shadows
  // around the camera as it was (bake_old_) and as baked for the new sun
  // (bake_new_), eased from one to the other over a second or so.
  Image bake_old_, bake_new_;
  VkRect2D bake_ease_rect_{};
  uint64_t bake_ease_frame_ = 0;
  bool bake_ease_pending_ = false, bake_ease_on_ = false;
  Image* baked_src_ = nullptr;
  bool baked_tod_ = false;  // baked for the time of day's sun (not saved, not loaded)
  // The time of day's shadows of riders, rails and objects (the game's
  // shadow pass's casters, drawn again from the hour's sun into a map like
  // the world's) as a screen mask the lighting reads in place of the game's
  // shadow map (its tf0; the lookup rows c6, c7, c9 the screen's).
  Image tod_movers_map_, tod_mask_;
  std::vector<CasterDraw> tod_movers_draws_;
  bool tod_mask_ok_ = false;
  Image* tod_game_map_ = nullptr;  // the game's shadow map (the lighting's tf0) this frame
  // Night lights (SetNightLights): added after the scene from the lighting's
  // own inputs (its albedo tf3 and normals tf4, kept at the lighting pass).
  std::vector<Renderer::NightLight> lights_;
  Image* tod_albedo_ = nullptr;
  // Fresh snow on what the lighting lights (tod_.snow_cover): its albedo
  // (tf3) whitened where it faces up, given to it instead.
  Image snow_albedo_;
  Image* snow_albedo_src_ = nullptr;
  bool snow_cover_ok_ = false;
  Image* tod_normal_ = nullptr;
  Image lights_img_, lights_out_, lights_albedo_, lights_wdepth_;
  bool lights_wdepth_ok_ = false;
  bool lights_done_ = false;
  Image* lights_result_ = nullptr;
  Image* NightLights(Image* scene, Image* depth);
  // Object instances (SetCollectInstances): this frame's scene draws, then
  // as object -> world (affine ones only: placed objects, not the terrain or
  // what's drawn in the camera's own space).
  bool collect_instances_ = false;
  struct PendingInstance {
    int query = -1;  // its draw's pick query (pick_pool_), if any
    uint64_t key;
    double m[16];
    double zs, zo;
    bool has_world;
    double world[12];  // object -> world found in its constants (has_world)
    uint64_t vs = 0;  // (its vertex shader: logged when a pick can't place it)
  };
  std::vector<PendingInstance> pending_instances_;
  // The object under the cross (the light editor): each scene draw drawn
  // again over the middle pixel alone, in an occlusion query; the last
  // whose samples passed is what's seen there (read when its frame's slot
  // comes round again).
  static constexpr uint32_t kPickMax = 4096;
  VkQueryPool pick_pool_ = VK_NULL_HANDLE;
  bool pick_queries_ = false;  // this frame's
  uint32_t pick_used_ = 0;
  int pick_q_ = -1;  // the current draw's
  Renderer::Instance picked_{};
  int picked_state_ = 0;  // 0 nothing (or not known), 1 an object, 2 the world itself
  std::string picked_why_;  // (the last unrecognised one logged)
  std::vector<Renderer::Instance> instances_;
  void ResolveInstances();
  // The middle of the screen's depth, read back 4 frames later (CenterPoint).
  Buffer center_rb_;
  uint64_t center_frame_[4] = {};
  double center_inv_[4][16] = {};
  float center_pt_[3] = {};
  bool center_ok_ = false;
  uint32_t center_size_[4][2] = {};
  float center_normal_[3] = {0.0f, 1.0f, 0.0f};
  bool center_normal_ok_ = false;
  void CenterProbe(Image* depth);
  bool center_probe_ = true;  // SetCenterProbe
  // The night lights that can show this frame: within 400 m, their reach in
  // view (their light, glow and fitting), nearest first, at most 128.
  // far: those seen from afar instead (as points of light, 150 m to 2.5 km,
  // at most 256).
  std::vector<std::pair<float, size_t>> VisibleLights(bool afar = false) const;
  void CenterRead();
  double tod_mask_rows_[12] = {};  // c6, c7, c9
  double tod_Om_[16] = {};         // the casters' map's rows (world -> its ndc, depth)
  bool baked_ok_ = false;
  void BakeWorldShadows(const Draw& d, const GpuState& st, const double H[16], bool bake);
  void ShiftMap(Image* map, Image* scratch, int64_t dx, int64_t dy);
  // Riders (drawn after the lighting) shaded by the world (ShadedMovers).
  Image movers_pos_, movers_shade_, movers_blur_, movers_out_;
  bool movers_done_ = false;
  Image* movers_result_ = nullptr;
  Image* ShadedMovers(Image* scene);
  // clip_to_world: the draws' clip space to world (null: the scene camera's,
  // its depth as the lighting has it).
  void DrawIntoSunMap(std::vector<CasterDraw>& draws, Image* map, const double O[16],
                      const double* clip_to_world = nullptr);
  void PrepareSunShadows(const Draw& d, const GpuState& st);
  void DrawSunShadows(const Draw& d, const GpuState& st);
  float next_shadow_av_ = 1.0f, next_shadow_fade_ = 1.0f;
  void UpdateShadowReach(const GpuState& st);
  bool scene_begun_ = false;  // this frame's tiled pass has begun
  Tiling last_tiling_;        // the last tiled pass's area
  bool last_frame_lit_ = true;
  float tiled_scale_ = 1.0f;  // this tiled pass's, fixed at BeginTiling
  static uint32_t Scaled(uint32_t v, float s) { return uint32_t(std::lround(double(v) * s)); }
  // Dynamic state last recorded by DoDraw in the current render pass.
  struct DynState {
    bool valid = false;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet sets[4] = {};
    uint32_t offsets[5] = {};
    uint32_t rt_w = 0, rt_h = 0;
    VkRect2D scissor{};
    float blend[4] = {};
    float bias[2] = {};
    uint32_t stencil_front = 0, stencil_back = 0;
    bool index_bound = false;
    VkIndexType index_type = VK_INDEX_TYPE_UINT32;
    VkBuffer index_buffer = VK_NULL_HANDLE;
  } dyn_;
  // Texture lookups this frame (DoDraw), cleared when a resolve may change
  // what a texture address shows.
  struct TexLookup {
    Image* image;
    VkSampler sampler;
    std::string note;
  };
  std::unordered_map<uint64_t, TexLookup> frame_tex_lookup_;
  // Texture descriptor sets by their contents (set, bindings, samplers,
  // views), kept from frame to frame: most draws bind the same textures every
  // frame, and writing a set costs far more than finding it. They live in a
  // pool of their own. When a view one of them uses is destroyed, or the pool
  // is full, all of them are dropped at once: the pool is retired (destroyed
  // three frames later, when no frame in flight binds its sets any more) and
  // a new one started.
  struct StringHash {
    using is_transparent = void;
    size_t operator()(std::string_view v) const { return std::hash<std::string_view>{}(v); }
  };
  std::unordered_map<std::string, VkDescriptorSet, StringHash, std::equal_to<>> tex_sets_;
  std::unordered_set<VkImageView> tex_set_views_;  // views the cached sets use
  VkDescriptorPool tex_set_pool_ = VK_NULL_HANDLE;
  uint32_t tex_set_pool_left_ = 0;
  struct RetiredPool {
    uint64_t frame;
    VkDescriptorPool pool;
  };
  std::vector<RetiredPool> retired_tex_set_pools_;
  static constexpr uint32_t kTexSetPoolSets = 4096;
  // The last set found for each of the two (draws in a row mostly bind the
  // same textures: a compare instead of a hash lookup).
  std::string last_tex_key_[2];
  VkDescriptorSet last_tex_set_[2] = {};
  void DropTextureSets();
  // Per-draw scratch (kept to avoid an allocation per draw).
  struct TexDesc {
    uint32_t set, binding;
    VkDescriptorImageInfo info;
  };
  std::vector<TexDesc> descs_scratch_;
  VkDescriptorSet TextureSet(uint32_t set, const std::vector<TexDesc>& descs);
  std::vector<Image*> colors_scratch_;
  // Last shader looked up per stage (draws mostly repeat the previous one).
  std::string last_shader_stem_[2];
  Shader* last_shader_[2] = {};
  // Per-frame resource bytes (vertex and index data), read once per name.
  std::unordered_map<std::string, std::vector<uint8_t>> frame_bytes_;
  const std::vector<uint8_t>& Bytes(const std::string& name) {
    if (const auto* p = res_->Peek(name)) return *p;
    auto it = frame_bytes_.find(name);
    if (it == frame_bytes_.end()) it = frame_bytes_.emplace(name, res_->Read(name)).first;
    return it->second;
  }
  std::ostringstream log_;
  RenderStats stats_;
  uint64_t frame_number_ = 0;
  uint32_t current_draw_ = 0;

  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layouts_[4] = {};
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkDescriptorPool pool_ = VK_NULL_HANDLE;
  uint32_t pool_draws_ = 0;
  VkDescriptorSet set0_ = VK_NULL_HANDLE, set1_ = VK_NULL_HANDLE;
  Buffer uniforms_, memory_, indices_, readback_;
  Buffer debug_;  // diagnostic vertex stores ("posdump")
  static constexpr VkDeviceSize kDebugBytes = 4096 * 4 * 16;
  bool debug_stores_ = false;
  size_t uniform_used_ = 0, index_used_ = 0;
  std::unordered_map<std::string, uint64_t> mem_offsets_;
  // Index buffers in the vertex arena, byte-swapped once (kept across frames
  // like the vertex data): resource name -> arena offset.
  std::unordered_map<std::string, uint64_t> ix_offsets_, ix_sizes_;
  // The index buffer the current draw reads (velocity / mask copies).
  VkBuffer cur_index_buffer_ = VK_NULL_HANDLE;
  bool index_cache_ = true;
  bool profile_frame_ = false;
  double draw_part_ms_[13] = {};  // 10, 11: parts of 'after' (the shadow maps' and tracks' copies; instances); 12: of 'draw state' (before its vertex data)  // off with the diagnostic "noixcache"
  std::unordered_map<std::string, uint64_t> mem_sizes_;
  // Vertex data kept in one GPU buffer across frames (Prepass): a resource
  // is copied only when its bytes changed (FrameResources::Version). A region
  // that is replaced or unused is freed 3 frames later, once no frame in
  // flight can still read it.
  struct ArenaEntry {
    uint64_t version = 0, offset = 0, size = 0, last_frame = 0;
  };
  bool arena_reset_ = false;          // ArenaGrow started it over (Prepass)
  uint64_t arena_frame_bytes_ = 0;    // last frame's vertex data
  Buffer arena_;
  std::unordered_map<std::string, ArenaEntry> arena_entries_;
  std::map<uint64_t, uint64_t> arena_free_;  // offset -> size
  struct ArenaRetired {
    uint64_t frame, offset, size;
  };
  std::vector<ArenaRetired> arena_retired_;
  uint64_t arena_copied_ = 0, arena_kept_ = 0;  // bytes, this frame
  bool ArenaAlloc(uint64_t size, uint64_t* offset);
  void ArenaFree(uint64_t offset, uint64_t size);
  void ArenaGrow(uint64_t need);
  std::map<std::string, Shader> shaders_;
  Shader missing_shader_;
  std::map<TargetKey, Image> targets_;
  std::map<TargetKey, uint32_t> target_heights_;
  struct CachedTexture {
    Image image;
    uint64_t last_used = 0;
    std::string note;
    size_t bytes = 0;
    uint64_t hash = 0;  // TextureHash
    bool replaced = false;     // a replacement's (texture pack, fonts, upscaled)
    bool may_replace = false;  // the game's own, one a replacement could be for
    // Its base level's ContentHash (as replacements are named; the same
    // in every session), worked out when first asked (DrawContentHash).
    uint64_t content = 0;
    bool content_known = false;
  };
  std::map<std::string, CachedTexture> textures_;
  // Textures first used this frame, read and decoded in parallel by Prepass
  // (PrefetchTextures); TextureFor takes them from here.
  std::unordered_map<std::string, std::vector<amped3::native::DecodedTexture>> prefetched_;
  std::unordered_map<std::string, uint64_t> prefetched_hash_;
  void PrefetchTextures();
  // TextureHash of the textures in textures_, and of prefetches not used.
  std::unordered_set<uint64_t> known_textures_, prefetch_skip_;
  bool KeepBlocks() const { return gpu_.bc_textures && !Diag("nobc"); }
  // Latest textures_ key per texture (name prefix + fetch words).
  std::unordered_map<std::string, std::string> texture_versions_;
  std::vector<Buffer> staging_;            // this frame's texture uploads
  // Texture uploads share one host buffer per frame in flight (no allocation
  // per texture); bigger than it, or once it's full, a buffer of their own.
  Buffer upload_;
  VkDeviceSize upload_used_ = 0;
  // Room for `size` bytes of upload data (offset aligned to 16): the buffer,
  // the offset in it and the mapped pointer.
  void UploadSpace(VkDeviceSize size, VkBuffer* buffer, VkDeviceSize* offset, uint8_t** mapped) {
    const VkDeviceSize at = (upload_used_ + 15) & ~VkDeviceSize(15);
    if (upload_.buffer && at + size <= upload_.size) {
      upload_used_ = at + size;
      *buffer = upload_.buffer;
      *offset = at;
      *mapped = static_cast<uint8_t*>(upload_.mapped) + at;
      return;
    }
    if (size <= (8u << 20)) {
      // Full (or not made yet): a bigger one for the rest of this frame and
      // later ones; the old one is freed with this frame's uploads.
      VkDeviceSize grown = upload_.buffer ? std::min<VkDeviceSize>(upload_.size * 2, 128u << 20)
                                          : (16u << 20);
      if (!upload_.buffer || grown > upload_.size || upload_used_ > 0) {
        if (upload_.buffer) staging_.push_back(upload_);
        upload_ = gpu_.CreateBuffer(std::max(grown, size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
        upload_used_ = size;
        *buffer = upload_.buffer;
        *offset = 0;
        *mapped = static_cast<uint8_t*>(upload_.mapped);
        return;
      }
    }
    Buffer own = gpu_.CreateBuffer(size, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    staging_.push_back(own);
    *buffer = own.buffer;
    *offset = 0;
    *mapped = static_cast<uint8_t*>(own.mapped);
  }
  std::vector<Image> retired_images_;      // replaced during this frame
  // DoResolve, one sample for the guest's two: the last resolve of sample 0
  // into each destination (cleared each frame).
  struct ResolveRecord {
    const Image* src = nullptr;
    uint64_t full_draws = 0;
    uint32_t x = 0, y = 0, w = 0, h = 0;
    bool operator==(const ResolveRecord& o) const {
      return src == o.src && full_draws == o.full_draws && x == o.x && y == o.y && w == o.w &&
             h == o.h;
    }
  };
  std::unordered_map<const Image*, ResolveRecord> last_resolve_;
  std::map<uint32_t, Image> resolved_;          // masked base address -> image
  struct SpareKey {
    const void* set;
    uint32_t base, width, height;
    VkFormat format;
    bool operator<(const SpareKey& o) const {
      return std::tie(set, base, width, height, format) <
             std::tie(o.set, o.base, o.width, o.height, o.format);
    }
  };
  struct SpareResolve {
    Image image;
    uint64_t frame = 0;
  };
  // Resolve images of another size at the same address (ResolveImage).
  std::map<SpareKey, SpareResolve> spare_resolves_;
  std::map<uint32_t, uint32_t> resolved_object_;  // dest texture object -> base
  std::map<uint32_t, uint32_t> resolved_format_;  // base -> Xenos format of the resolve
  // Resolved images outlive their frame (games sample last frame's resolves),
  // but not a rewrite of that memory by the CPU: the first later frame that
  // samples one notes the texture's content name (fetch constant + sampled
  // hash of guest memory); when it changes with no new resolve, the memory
  // was rewritten and the resolve is dropped.
  struct ResolveInfo {
    uint64_t frame = 0;  // frame_number_ of the resolve
    std::string seen;    // content name noted in a later frame
  };
  std::map<uint32_t, ResolveInfo> resolved_info_;
  bool ResolveStillValid(uint32_t base, const std::string& data) {
    auto it = resolved_info_.find(base);
    if (it == resolved_info_.end() || it->second.frame == frame_number_ || data.empty()) return true;
    if (it->second.seen.empty()) {
      it->second.seen = data;
      return true;
    }
    return it->second.seen == data;
  }
  void DropResolve(uint32_t base) {
    for (auto* images : {&resolved_, &resolved_packed_}) {
      auto it = images->find(base);
      if (it != images->end()) {
        Retire(it->second);
        images->erase(it);
      }
    }
    resolved_format_.erase(base);
    resolved_info_.erase(base);
    frame_tex_lookup_.clear();
    log_ << "resolve at " << std::hex << base << std::dec << " rewritten since: dropped\n";
  }
  // Depth resolves also keep the raw depth/stencil words as 8_8_8_8, for
  // textures that alias the resolved depth with a color format.
  std::map<uint32_t, Image> resolved_packed_;
  Image* ResolveImage(std::map<uint32_t, Image>& images, uint32_t base, uint32_t w, uint32_t h,
                      VkFormat format, const char* suffix, float scale);
  // Copies through a fragment shader. kDepth / kDepthMs: depth (mode 0, as
  // R32F) or the raw EDRAM words (mode 1, as 8_8_8_8), from sample `first` of
  // a multisampled image for kDepthMs; kColorMs: the average of samples
  // first .. first + count - 1.
  // kOutput / kOutputMs: the front buffer through the gamma table (binding
  // 1, 256 x 1) into the output image.
  // Motion blur (BlurredScene): kMotion: each pixel's motion from the depth
  // (binding 1, R32F) and the object velocities (binding 2); kTileMax /
  // kNeighborMax: the largest motion per tile / around it (binding 0);
  // kMotionBlur: the scene (binding 0, linear) reconstructed along the
  // neighbourhood's motion (motion binding 1, tiles binding 2).
  enum class CopyKind {
    kDepth = 0, kDepthMs = 1, kColorMs = 2, kOutput = 3, kOutputMs = 4, kMotionBlur = 5,
    kMotion = 6, kTileMax = 7, kNeighborMax = 8, kRows = 9, kMotionVectors = 10,
    kRadialBlur = 11, kFogColor = 12, kFog = 13, kRayMask = 14, kRayBlur = 15, kRayAdd = 16,
    kClouds = 17, kReactiveMerge = 18, kReactiveView = 19, kAo = 20, kAoBlur = 21, kAoApply = 22,
    kSunPrep = 23, kSunPcss = 24, kBakeInit = 25, kBake = 26, kMoverPos = 27,
    kMoverShade = 28, kMoverApply = 29, kBakeMin = 30, kSunSeen = 31, kSunTiles = 32,
    kMoverBlur = 33, kTodSky = 34, kTodTint = 35, kEyeMeasure = 36, kTodShadow = 37, kTodLights = 38,
    kSnow = 39, kSnowCover = 40, kBloom = 41, kCloudShadow = 42,
    kTracksMerge = 43, kTracksApply = 44, kVolume = 45, kVolumeAdd = 46, kFogVolume = 47, kCloudMap = 48, kSunSpec = 49, kSpecAdd = 50, kTracksShadow = 51, kDrift = 52, kFlare = 53, kDust = 54, kContactShadow = 55, kMaskEase = 56, kTodFade = 57, kKeepIcons = 58, kAoPrep = 59, kMaskFinish = 60, kSunSpecMs = 61, kBounce = 62, kGiOcc = 63, kGiTrace = 64, kGiBlur = 65, kGiTemporal = 66, kGiProbes = 67, kGiApply = 68, kGiMovers = 69, kGiSun = 70
  };
  VkPipeline CopyPipeline(CopyKind kind, VkFormat format);
  void CopyPass(CopyKind kind, Image* src, Image* dst, uint32_t mode, bool float_depth,
                uint32_t first, uint32_t count, uint32_t sx0, uint32_t sy0, uint32_t dx0,
                uint32_t dy0, uint32_t w, uint32_t h);
  void CopyDepth(Image* src, Image* dst, uint32_t mode, bool float_depth, uint32_t sample,
                 uint32_t sx0, uint32_t sy0, uint32_t dx0, uint32_t dy0, uint32_t w, uint32_t h) {
    CopyPass(src->samples > 1 ? CopyKind::kDepthMs : CopyKind::kDepth, src, dst, mode,
             float_depth, sample, 1, sx0, sy0, dx0, dy0, w, h);
  }
  std::unordered_map<uint64_t, VkSampler> samplers_;
  std::unordered_map<std::string, VkPipeline, StringHash, std::equal_to<>> pipelines_;
  // Background pipeline builds (RendererOptions::async_pipelines): keys in
  // flight, the queue and the builders, and what they finished.
  static constexpr double kPipelineSyncBudgetMs = 4.0;
  double pipeline_sync_ms_ = 0;  // this frame's synchronous builds
  uint32_t slow_pipes_told_ = 0;
  bool last_pipeline_pending_ = false;
  std::unordered_set<std::string> pipelines_pending_;
  std::mutex pq_mutex_;
  std::condition_variable pq_cv_;
  std::deque<std::unique_ptr<PipelineBuild>> pq_;
  std::vector<std::pair<std::string, VkPipeline>> pq_done_;
  std::vector<std::thread> pq_threads_;
  bool pq_stop_ = false;
  void QueuePipeline(std::unique_ptr<PipelineBuild> build);
  void SpawnBuilder();
  // Precompiling (StartPrecompile): the pipeline list, the microcode kept
  // with it, and what's being built from it.
  std::string precompile_dir_;
  bool precompiling_ = false;     // PipelineFor called for the list: queued, not listed
  std::string last_queued_key_;
  std::unordered_set<std::string> listed_;     // recipes in the list (this session's, and read)
  // Pipelines that crashed the graphics driver as it built them (a crash
  // report wrote what was being built to crashed_pipelines.txt): not built
  // again (their draws left out), nor precompiled from the list.
  std::unordered_set<std::string> crashed_ids_, crashed_lines_;
  std::unordered_set<std::string> crashed_told_;
  // Per vertex shader (its microcode hash) that crashed the driver: the
  // plainer translation to try next (TranslateOptions::safe_level).
  std::unordered_map<uint64_t, int> safe_level_;
  static constexpr int kSafeLevels = 1;
  static constexpr bool kPlainerSteps = true;  // the plainest (TranslateOptions::safe_level)
  // Game pixel shaders as drawn into a depth target alone (one sample): no
  // color outputs, no sample mask (DepthOnlyPs).
  std::unordered_map<const Shader*, Shader> depth_only_ps_;
  const Shader& DepthOnlyPs(const Shader& ps);
  int SafeLevel(const uint32_t* ucode, size_t n) const {
    if (safe_level_.empty()) return 0;
    auto it = safe_level_.find(amped3::native::HashMicrocode(ucode, n));
    return it == safe_level_.end() ? 0 : it->second;
  }
  std::string precompile_line_;  // the list's line being queued (PrecompileStep)
  void ReadCrashed();
  bool listed_read_ = false;
  std::unordered_set<uint64_t> ucode_kept_;    // microcode already in the store
  struct PrecompileItem {
    Draw d;
    GpuState st;
    std::vector<Image> images;  // the colors', then the depth's (format, samples)
    size_t ncolors = 0;
    Shader* vs = nullptr;
    Shader* ps = nullptr;
    uint32_t mask_bits = 0, flags = 0;
    std::string line;  // the list's
  };
  std::mutex pc_mutex_;
  std::condition_variable pc_cv_;
  std::thread pc_thread_;
  bool pc_stop_ = false;
  uint32_t pc_busy_ = 0;
  std::deque<std::unique_ptr<PrecompileItem>> pc_ready_;
  std::atomic<uint32_t> precompile_handled_{0};
  void PrecompileMain();
  void StopPrecompile();
  std::deque<std::string> precompile_todo_;
  std::vector<std::string> precompile_keys_;   // queued, to tell when they're done
  std::unordered_set<std::string> precompiled_;  // recipes done this session
  uint32_t precompile_total_ = 0;
  std::atomic<uint32_t> precompile_failed_{0};
  std::map<std::pair<uint8_t, uint64_t>, Shader> precompile_shaders_;
  std::string ListPath() const;
  void ReadList();
  uint64_t KeepMicrocode(const std::vector<uint8_t>& bytes);
  void ListPipeline(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                    const std::vector<Image*>& colors, Image* depth, uint32_t mask_bits, bool velocity,
                    bool mask, bool reactive, bool caster);
  Shader* PrecompileShader(uint8_t recipe, uint64_t hash);
  void PrecompileStep(double budget_ms);
  // Pipelines built one at a time (all threads): Turnip (Mesa 26.3, the
  // Odin 3's Adreno) crashes in its shader compiler when several are built
  // at once - the crashes came with many different shaders, always on the
  // background builders, since builds went parallel (17 November).
  bool serial_pipes_ = false;
  // Shaders test multiplication's zeros on the bits (TranslateOptions::
  // bit_zero_mul): Turnip folds the float tests into an fmadz its Adreno
  // backend can't emit ("Unhandled ALU op: fmadz") and crashes. Diags
  // "bitmulz" / "floatmulz" force it on / off.
  bool bit_zero_mul_ = false;
  std::mutex pipe_build_mutex_;
  std::unique_lock<std::mutex> PipeLock() {
    return serial_pipes_ ? std::unique_lock<std::mutex>(pipe_build_mutex_) : std::unique_lock<std::mutex>();
  }
  size_t PrecompileThreads() const {
    if (serial_pipes_) return 1;
    const unsigned n = std::thread::hardware_concurrency();
    return std::clamp<size_t>(n > 4 ? n - 3 : 2, 2, 8);
  }
  // Holding the picture (SetHoldPicture).
  bool hold_picture_ = false;
  Image hold_img_;
  bool hold_valid_ = false;
  void CollectPipelines();
  void StopPipelineBuilders();
  // Disk pipeline cache: pipelines built since it was last written, and the
  // background write in progress.
  uint32_t pipelines_unsaved_ = 0;
  std::future<bool> cache_save_;
  std::chrono::steady_clock::time_point cache_saved_at_;
  // Total size of textures_ (bytes uploaded), and how much they may take
  // (RendererOptions::texture_budget_mb, capped by the GPU's memory).
  uint64_t texture_bytes_ = 0, texture_budget_ = 0;
  Image dummy_;
  // Depth -> color copy for depth resolves.
  VkPipelineLayout copy_layout_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout copy_set_layout_ = VK_NULL_HANDLE;
  VkShaderModule copy_vs_ = VK_NULL_HANDLE;
  VkShaderModule copy_fs_[71] = {};  // by CopyKind
  // Map clouds: one quad per puff (instanced), placed by its vertex shader,
  // with push constants in both stages (a layout of their own).
  VkShaderModule clouds_vs_ = VK_NULL_HANDLE;
  VkPipelineLayout clouds_layout_ = VK_NULL_HANDLE;
  // Output (DrawOutput): shared slots, the local image for readback, the
  // gamma table.
  struct OutputSlot {
    Image image;
    uint64_t handle = 0, memory_size = 0, generation = 0;
  };
  OutputSlot out_slots_[Renderer::kOutputSlots];
  Image out_local_;
  Image lut_;
  uint8_t lut_data_[768] = {};
  bool lut_set_ = false;
  uint64_t out_generation_ = 0;
  Buffer out_readback_;
  void DestroyOutputSlot(OutputSlot& s) {
#ifdef _WIN32
    if (s.handle) CloseHandle(reinterpret_cast<HANDLE>(uintptr_t(s.handle)));
#endif
    gpu_.DestroyImage(s.image);
    s = OutputSlot{};
  }
  bool DrawOutput(Image* src, const uint8_t* lut, bool share, uint32_t slot, OutputInfo* info,
                  std::vector<uint8_t>* packed);
  bool RecordOutput(Image* src, const uint8_t* lut, bool share, uint32_t slot, OutputInfo* info,
                    Buffer* readback);
  std::map<std::pair<int, VkFormat>, VkPipeline> copy_pipelines_;
  VkSampler point_sampler_ = VK_NULL_HANDLE;

  // Current rendering.
  bool rendering_ = false;
  std::vector<Image*> cur_colors_;
  Image* cur_depth_ = nullptr;
  Image* last_color_ = nullptr;
  Image* final_ = nullptr;
  Image* final_front_ = nullptr;
  Image* FrontBuffer(const Frame& frame);
  Tiling tiling_;  // active BeginTiling area (width 0 = not tiling)

  // Camera motion blur (Renderer::SetMotionBlur). The game's deferred lighting
  // pass (kLightingPixelShader) gives the frame's inverse view-projection
  // (its pixel constants c0..c3) and the addresses its scene colour (tf3) and
  // depth (tf5) are resolved to. After the tiled pass, the first draw that
  // reads the scene colour (the composite before the HUD) gets a blurred copy.
  float blur_scale_ = 0.0f;  // 0: off
  bool light_seen_ = false, blur_done_ = false, prev_valid_ = false;
  double inv_vp_[16] = {}, prev_inv_vp_[16] = {};
  uint32_t scene_base_ = 0, depth_base_ = 0;
  Image blur_img_;
  // Motion blur passes (BlurredScene): each pixel's motion (pixels) and view
  // distance, the largest motion of each tile, and of each tile's neighbours.
  Image motion_img_, tile_rows_img_, tile_img_, nmax_img_;
  Image player_rows_img_;  // 4 x 1: pixel offset from the player = rows * v / w
  void FullscreenPass(CopyKind kind, Image* dst, Image* b0, Image* b1, Image* b2,
                      bool linear0, const float* pc, uint32_t floats,
                      const VkRect2D* rect = nullptr, Image* b3 = nullptr,
                      Image* b4 = nullptr, Image* b5 = nullptr);
  VkSampler linear_sampler_ = VK_NULL_HANDLE;
  void NoteLightingPass(const Draw& d, const GpuState& st);
  Image* BlurredScene(Image* scene);
  bool camera_blur_ = true, blur_debug_ = false;

  // Temporal upscaling (RendererOptions::upscaler). The scene's geometry is
  // jittered (Halton) inside the tiled pass; where the composite reads the
  // scene, the upscaler turns it (with its depth and motion vectors) into an
  // output-sized image, and the rest of the frame is drawn at out_scale_.
  std::unique_ptr<amped3::native::Upscaler> upscaler_;
  std::string upscaler_status_;
  bool up_inited_ = false, up_reset_ = true, up_done_ = false;
  uint32_t up_in_w_ = 0, up_in_h_ = 0, up_out_w_ = 0, up_out_h_ = 0;
  Image up_out_, mv_img_;
  Image* up_result_ = nullptr;
  uint32_t jitter_index_ = 0;
  int jitter_phases_ = 8;
  float jitter_x_ = 0.0f, jitter_y_ = 0.0f;  // render pixels, +x right, +y down
  float frame_ms_ = 16.7f, up_sharpness_ = 0.3f;
  Image* UpscaledScene(Image* src);
  void Transition(const Image& im, VkImageLayout from, VkImageLayout to);
  uint32_t max_draws_ = UINT32_MAX;  // this Render's

  // Object motion blur. Each solid scene draw (depth-written, before the
  // lighting pass) keeps its vertex constants under a key that finds the same
  // draw next frame (shader, vertex and index buffers, range; repeats
  // numbered in order). A velocity copy of the draw is recorded: the vertex
  // shader run with this and last frame's constants (velocity variant), its
  // screen motion written where it is the visible surface (depth EQUAL). At
  // the lighting pass the copies are drawn in one pass against the finished
  // scene depth; the blur reads the result (vel_img_, xy = motion in uv,
  // z = 1 where an object's own motion is known).
  bool object_blur_ = false;
  bool scenery_blur_ = false;  // SetSceneryObjectBlur
  // SetMergeClears: clears wait for the next pass on their image (its load
  // op), or run inside the pass drawing to it.
  bool merge_clears_ = true;
  struct PendingClear {
    bool color = false, depth = false, stencil = false;
    float rgba[4] = {};
    float z = 0;
    uint32_t s = 0;
    // Parts of the image so far (tiles resolved one after another, each
    // cleared): a pass's load op takes it once they cover all of it.
    std::vector<VkRect2D> rects;
  };
  // The barrier of a resolve reading `rect` of `src`: only clears waiting for
  // that part of `src` are done first; the rest (other images, the other
  // tiles of `src`) may go on waiting.
  void ResolveBarrier(const Image* src, const VkRect2D& rect);
  // Clears waiting for `src` (all of it, or the parts overlapping `rect`)
  // and for `dst`; with no `src`, every one.
  void FlushPendingClearsExcept(const Image* src, const VkRect2D* rect,
                                const Image* dst = nullptr);
  std::unordered_map<Image*, PendingClear> pending_clears_;
  // A full barrier, after the clears still waiting (they come first).
  void Barrier();
  void FlushPendingClears();
  // DoDraw's pass: clears waiting for other render targets may go on
  // waiting (a draw samples textures and resolves, never a target).
  bool keep_target_clears_ = false;
  // The frame's commands recorded on another thread (SetParallelCommands).
  amped3::native::cmd::Stream stream_;
  bool parallel_commands_ = true;
  // Does the waiting clears except render targets other than `but` / `but2`,
  // which may go on waiting (into the next frame: its first pass on them
  // takes them as load ops).
  void FlushPendingClearsKeepTargets(const Image* but, const Image* but2);
  void ClearImageNow(Image* im, const float* color, float z, uint32_t stencil, bool c, bool zf,
                     bool s, const VkRect2D* rect);
  struct VelDraw {
    VkPipeline pipeline;
    VkDescriptorSet tex_sets[2];
    uint32_t offsets[5];
    bool indexed;
    VkIndexType index_type;
    VkBuffer index_buffer;
    uint32_t first, count;
    VkRect2D scissor;
    float bias[2];
    uint32_t rt_w, rt_h;
    SystemConstants sys;  // flags completed when drawn (XE_FLAG_VELOCITY*)
    bool matched;         // last frame's constants found
    bool post_light;      // drawn after the lighting pass (characters, effects)
    bool has_anchor;      // anchor: world position from a bone / world matrix
    float anchor[3];
    const Shader* vs;
    bool skinned;         // the vertex shader indexes its constants (bones)
  };
  std::vector<VelDraw> vel_draws_;
  bool vel_post_light_ = false;  // a copy was recorded after the lighting pass
  float cam_pos_[3] = {};        // this frame's camera (lighting pass c4; the freecam's when on)
  // The game's own camera, whatever is seen (the freecam): what follows the
  // rider (speed, riding, the player) goes by it.
  float game_cam_pos_[3] = {};
  double game_inv_vp_[16] = {};  // the game's inverse view-projection (the lighting's c0..c3)
  // Freecam (SetFreecam): the camera's pose, from the game's camera when it
  // was turned on (entry: its view-projection, position and axes).
  Renderer::FreecamInput fc_in_;
  bool fc_on_ = false;           // this frame: a scene seen from the freecam
  bool fc_have_entry_ = false;
  double fc_vpe_[16] = {};       // entry view-projection (world -> x, y, 1 - depth, 1)
  double fc_c0_[3] = {}, fc_fwd_[3] = {}, fc_right_[3] = {};
  double fc_yaw_sign_ = 1.0, fc_pitch_sign_ = 1.0;
  double fc_p_[3] = {}, fc_yaw_ = 0.0, fc_pitch_ = 0.0, fc_zoom_ = 1.0;
  double fc_vpf_[16] = {}, fc_ivpf_[16] = {};  // the freecam's, this frame
  double fc_ivpg_[16] = {};      // the game's inverse, this frame (from the frame's lighting pass)
  float fc_pose_[6] = {};        // position, forward: this frame's (FreecamPose)
  TargetKey fc_depth_{};         // the scene's depth target (the lighting pass's)
  std::string lighting_ps_;      // the lighting pass's pixel shader resource name
  std::chrono::steady_clock::time_point fc_last_{};
  void FreecamFrame();
  // Speed blur (SetSpeedBlur): the camera's speed (it follows the rider),
  // smoothed so the effect fades in and out, the direction of travel, and the
  // radial blur of this frame's scene.
  float speed_strength_ = 0.0f, speed_dt_ = 1.0f / 60.0f, speed_force_ = -1.0f;
  float speed_ = 0.0f;           // smoothed, world units (metres) per second
  float speed_amount_ = 0.0f;    // 0..1, from speed_
  float speed_dir_[3] = {};      // smoothed direction of travel
  float speed_center_[2] = {0.5f, 0.5f};  // where the streaks point (uv)
  float prev_cam_[3] = {};
  bool prev_cam_valid_ = false;
  uint64_t speed_frame_ = UINT64_MAX;
  Image radial_img_;
  bool radial_done_ = false;
  // Distance fog (SetFog): settings, the light direction from the lighting
  // pass (c14), the fog's color (the sky's average, over time: two 1x1
  // images in turn) and the fogged scene.
  float fog_density_ = 0.0f, fog_start_ = 150.0f, fog_most_ = 0.75f;
  float fog_presence_ = 1.0f;  // 0..1: fades out without a rider close by
  float sun_dir_[3] = {};
  bool sun_valid_ = false;
  float sun_dir_painted_[3] = {};  // where the panorama paints the sun (time of day)
  Image fog_color_img_[2];
  uint32_t fog_color_cur_ = 0;
  Image fog_img_;
  bool fog_done_ = false;
  Image* FoggedScene(Image* scene);
  // Weather: falling snow over the scene (tod_.snow), hidden behind what's
  // in front of each flake.
  // Wind on the trees (SetWind per draw): on with the time of day; its
  // velocity (m/s, world x / z) and clock, last frame's clock.
  bool wind_on_ = false;
  std::vector<uint64_t> cloth_extra_;  // SetClothTextures
  float wind_vel_[2] = {};
  double wind_t_ = 0.0, wind_prev_t_ = 0.0;
  void SetWind(SystemConstants& sys, const GpuState& st, int mode);
  // How far the night lights are on (0..1): after dark, and part way in a
  // gloomy snowstorm by day (lamps on in a blizzard).
  double LightsDark() const {
    if (!TodOn()) return 0.0;
    const double night = tod::SmoothStep(0.25, 0.85, tod_state_.night);
    const double gloom = 0.55 * tod::SmoothStep(0.6, 1.0, tod_.overcast) * (0.4 + 0.6 * std::clamp(double(tod_.snow), 0.0, 1.0));
    return std::max(night, gloom);
  }
  // Bloom (SetBloom): the bright parts (lamps, fires, the sun on snow)
  // glowing out over what's round them; quarter-size, blurred twice.
  float bloom_ = 0.0f;
  float cloud_shadows_ = 0.0f;  // how dark the clouds' shadows are (0: none)
  // Snow tracks (SetSnowTracks): the boards' trails pressed into the snow and
  // kept (a map from above, 4096 texels of 5 cm round the camera: r how deep
  // a groove, g the snow pushed up beside it), the snow they push aside piled
  // at their sides and where they skid. From the game's own trail draws
  // (each frame's drawn again from above into a stamp round the camera).
  // Applied to the lighting's normals and albedo.
  float tracks_ = 0.0f;
  static constexpr uint32_t kTracksN = 4096, kTracksW = 1024;
  static constexpr double kTracksTexel = 0.05;
  static constexpr double kTracksLife = 240.0;  // seconds a track lasts, unworn by wind or snow
  Image tracks_map_, tracks_scratch_, tracks_stamp_, tracks_tmp_;
  Image tracks_stamp_more_[2];  // the trails drawn again where their texture's stronger (its alpha)
  Image tracks_albedo_, tracks_normal_;
  Image* tracks_albedo_src_ = nullptr;
  Image* tracks_normal_src_ = nullptr;
  bool tracks_ok_ = false;     // this frame's lighting given them
  bool tracks_valid_ = false;  // the map holds this place's tracks
  int64_t tracks_cx_ = 0, tracks_cz_ = 0;  // the map's middle (in texels of the world)
  double tracks_last_cam_[3] = {};
  std::vector<CasterDraw> tracks_draws_;
  std::unordered_set<uint64_t> tracks_seen_;
  void TracksUpdate();
  void TracksInputs(const Draw& d, const GpuState& st, Image* albedo_in);
  void SceneInputs(const Draw& d, const GpuState& st);
  uint32_t lighting_aa_ = 0;  // the lighting pass's sample mask (its inputs made for it)
  Image tod_mask_raw_;  // the sun's shadows before the clouds'
  // Over a step of the time of day's sun (the sun maps started over for it):
  // the frame it went, last frame's mask and view (eased from, TodShadows).
  uint64_t sun_turn_frame_ = 0;
  double sun_step_light_[3] = {};  // the time of day's light at the sun maps' last start
  bool sun_turned_ = false;
  // A step of the time of day's sun: the maps from before it held for
  // what reads them (the terrain's mask, the riders' shade, the light in the
  // air) while the new are drawn (three frames), then let go.
  Image sun_env_hold_;
  double sun_O_hold_[16] = {};
  bool sun_hold_on_ = false, sun_hold_swapped_ = false, sun_hold_phase0_ = false;
  uint64_t sun_hold_frame_ = 0;
  void SunHoldApply();
  Image tod_mask_prev_;
  bool tod_mask_prev_ok_ = false;
  double tod_mask_prev_inv_vp_[16] = {};
  double tod_mask_prev_cam_[3] = {};
  float cloud_here_ = 0.0f;  // the clouds' shade at the camera (for the riders: 0..1)
  Image bloom_a_, bloom_b_, bloom_out_, pillar_img_;
  Image* BloomScene(Image* scene);
  // Lens flare (SetLensFlare): the sun's, drawn (ghosts of the aperture along
  // the line through the middle, a starburst, glare), as much as the sun is
  // seen (flare_vis_: the sky's share of the sun's disc, eased); and the
  // lamps' and fires' at night, mirrored from the bright parts.
  float flare_ = 0.0f;
  float lens_dirt_ = 0.0f;  // SetLensDirt: dust on the lens lit by the sun
  Image flare_img_, flare_tmp_, flare_out_, flare_vis_[2];
  uint32_t flare_vis_cur_ = 0;
  Image* FlareScene(Image* scene, bool ghosts_ready);
  void FlareGhosts(Image* bright, Image* scene);
  // Volumetric light (SetVolumetric): the air lit by the sun (or moon) where
  // it reaches, shadowed by the world's maps and the clouds: shafts through
  // trees, past ridges, under clouds; thicker in mist, overcast and falling
  // snow, and low down. Half size, marched; added to the scene. The night
  // lights' own (their light in the air) in their pass. Replaces the god
  // rays while on.
  float volumetric_ = 0.0f;
  int volumetric_quality_ = 1;  // 0 quarter size, 1 half, 2 half finer
  Image vol_img_, vol_params_, vol_out_;
  float vol_shadow_pc_[32] = {};
  bool vol_shadow_ok_ = false, vol_world_ok_ = false;
  float vol_cloud_[4] = {};
  Image cloud_map_;           // the cloud layer round the camera (builtin_cloud_map)
  float cloud_map_at_[4] = {};  // its corner (layer coordinates), 1 / its span
  // The fog left to the volumetric light's pass (one pass for both): its
  // constants and colors.
  bool fog_merged_ = false;
  float fog_pc_[32] = {};
  Image* fog_colors_now_ = nullptr;
  bool VolumeReady(const Image* scene, const Image* depth) const;
  float VolumeDensity() const;  // the air's (per metre, at the camera's height)
  bool VolumeOn() const { return volumetric_ > 0.0f && TodOn() && !Diag("novolume"); }
  Image* VolumeScene(Image* scene);
  Image drift_out_;
  bool drift_ = false;  // SetDriftingSnow
  float halo_ = 1.0f;     // SetIceHalo
  Image* DriftScene(Image* scene);
  // Diamond dust (SetDiamondDust): glinting ice crystals adrift near the
  // camera on clear, calm days, the most with the sun low.
  float dust_ = 0.0f;
  float contact_ = 0.0f;  // SetContactShadows
  Image dust_out_;
  Image* DustScene(Image* scene);
  // Time of day turned on or off: the picture eased from the last one shown
  // before to the new over most of a second (TodFade), rather than cut.
  Image fade_img_, fade_out_;
  Image icons_out_;  // the scene with the event icons kept clear of the effects
  uint64_t fade_copy_frame_ = ~0ull;
  bool fade_last_on_ = false, fade_active_ = false;
  float fade_t_ = 0.0f;
  Image* TodFade(Image* scene);
  double PixelAngle(uint32_t h) const;  // a pixel's angle at the screen's middle (radians)
  // The sun's (or moon's) sheen (SetSpecular): what the game's lighting
  // leaves out (it has no specular): a glossy highlight by surface (snow:
  // a broad sheen and glints; packed snow and ice a glare toward a low sun;
  // the rest a dull dielectric; foliage little), shadowed by the sun's mask.
  // Made at the lighting pass (the world's albedo, normals and depth, no
  // riders yet: its depth kept in alpha) and added after (where the depth
  // still matches: not over the riders).
  float specular_ = 0.0f;
  float rider_shine_ = 1.0f;  // SetRiderShine (1: the game's)
  std::unordered_map<uint64_t, uint8_t> material_overrides_;  // content hash -> surface
  bool surfaces_on_ = true;  // (diag nosurfaces: none tagged; read once a frame)
  // The surface picker (StartSurfacePick): which texture is at the screen's
  // middle, found through the tags: a frame listing the textures drawn
  // (fetch 0 of the draws tagged), then a frame per base-7 digit of its place
  // in that list, each texture tagged 1 + that digit; the normals' alpha at
  // the middle read back (4 frames later) gives each digit.
  int pick_state_ = 0;  // 0 idle, 1 listing, 2 digits
  std::vector<uint64_t> pick_hashes_;
  std::vector<uint8_t> pick_guess_;
  std::unordered_map<uint64_t, uint32_t> pick_index_;
  int pick_digits_ = 0, pick_cur_ = 0, pick_frames_ = 0, pick_misses_ = 0;
  int pick_have_[8] = {};
  int pick_slot_digit_[4] = {-1, -1, -1, -1};
  uint64_t pick_slot_frame_[4] = {};
  Buffer pick_rb_;
  int pick_result_ = 0;  // 0 pending, 1 found, 2 nothing there
  uint64_t pick_hash_ = 0;
  int pick_found_guess_ = 0;
  void PickProbe(const Draw& d, const GpuState& st);
  void PickRead();
  Image spec_img_, spec_out_;
  // Light bounce (SetLightBounce): the albedo kept at the lighting pass, the
  // light bounced onto each half-size pixel from what's lit round it.
  float bounce_ = 0.0f;
  bool bounce_ok_ = false;
  Image bounce_albedo_, bounce_img_;
  // World-space light (SetGi, prototype): the world that stays put
  // voxelised into two camera-centred cascades (128 voxels a side: 1 m,
  // then 4 m), and each half-size pixel's light from round it traced
  // through them: the sky's light where it's blocked taken off (the game
  // lights everything as under an open sky), the light of what's there put
  // on instead (from the screen where it shows, else worked out).
  // Voxelising: the scene pass's draws of what stays put (the sun maps'
  // env draws) drawn again, orthographically, into five depth maps per
  // cascade (from above, and from either side along x and z), each texel
  // the nearest surface that way; the cascade's occupancy built from them
  // (one 128-bit column of y per texel: RGBA32UI), kept and shifted as the
  // camera moves, so what's been seen stays.
  float gi_ = 0.0f;
  static constexpr uint32_t kGiN = 128, kGiMap = 256;
  struct GiCascade {
    double voxel = 1.0;
    int64_t org[3] = {0, 0, 0};      // the occupancy's (voxels)
    int64_t pending[3] = {0, 0, 0};  // the maps' being drawn
    Image maps[5];
    Image occ[2];
    int cur = 0;
    bool ok = false, drawing = false;
    bool skip = false;  // this update: still near enough its box
  };
  GiCascade gi_c_[2];
  Image gi_params_, gi_blur_, gi_img_;  // (gi_img_: quarter size)
  // Kept from frame to frame (quarter size, its depth in alpha), each new
  // frame's half of the pixels traced blended in where the last frame's
  // result lands on the same surface.
  Image gi_hist_[2];
  // The probe grid (the default; diagnostic "gipixel": every quarter-size
  // pixel traced instead): 32 x 16 x 32 probes 4 m apart round the camera,
  // world-aligned and kept as it moves (each slot the probe whose world
  // index matches it, modulo the grid), each probe six faces (the light onto
  // a surface facing +x, -x, +y, -y, +z, -z), an eighth of them traced
  // again each frame and blended in. Texture: 6 x 32 wide, 16 x 32 high.
  // Under it in the same texture (rows 16 x 32 on) a finer grid, the same
  // count 2 m apart (64 x 32 x 64 m round the camera): the shade near by
  // (under a tree, by a wall) sharper; an eighth of it traced a frame, the
  // coarse grid a sixteenth.
  static constexpr int kProbeDims[3] = {32, 16, 32};
  static constexpr double kProbeSpacing = 4.0;
  static constexpr double kProbeFine = 2.0;
  Image gi_probe_[2];
  int gi_probe_cur_ = 0;
  bool gi_probes_now_ = false;  // this frame's probes (and gi_params_) up to date
  Image gi_movers_;             // the riders' factor from them (half size)
  Image gi_sun_;
  Image uint_dummy_;  // 1 x 1 RGBA32UI, bound where a voxel map would be (none)
  Image* UintDummy() {
    if (!uint_dummy_.image) {
      uint_dummy_ = gpu_.CreateImage(1, 1, VK_FORMAT_R32G32B32A32_UINT, false, "uint_dummy");
      ToGeneral(cb_, uint_dummy_);
    }
    return &uint_dummy_;
  }                // the sun mask with the far shadows from the voxels (builtin_gi_sun)
  int gi_hist_cur_ = 0;
  bool gi_hist_ok_ = false;
  double gi_prev_vp_[16] = {};
  uint64_t gi_prev_frame_ = 0;
  bool GiReady() const { return gi_ > 0.0f && gi_c_[0].ok && gi_c_[1].ok && !Diag("nogi"); }
  void GiVoxelize(std::vector<CasterDraw>& env, bool start, bool done);
  void GiTrace(Image* scene, Image* depth);
  Image relight_params_;  // the sheen's settings: the sky, the far-off scenery's light (SunSpecular)
  Image spec_stencil_dummy_;  // (its stencil when the lighting has none)
  bool spec_ok_ = false;
  void SunSpecular(const Draw& d, const GpuState& st);
  Image* SpecularScene(Image* scene);
  bool snow_done_ = false;
  Image* snow_result_ = nullptr;
  Image snow_img_;
  Image snow_params_;  // builtin_snow's settings past the push constants
  // Smooth noise of time (-1..1), and the wind's gusts by it: a factor on
  // its speed, about 0.4 .. 1.6.
  static double TimeNoise(double x) {
    const double i = std::floor(x), f = x - i;
    auto h = [](double n) { double v = std::sin(n * 12.9898) * 43758.5453; return (v - std::floor(v)) * 2.0 - 1.0; };
    const double u = f * f * (3.0 - 2.0 * f);
    return h(i) * (1.0 - u) + h(i + 1.0) * u;
  }
  static double WindGust(double t) { return 1.0 + 0.45 * TimeNoise(t * 0.35) + 0.2 * TimeNoise(t * 1.3 + 17.0); }
  Image* SnowScene(Image* scene);
  // God rays (SetGodRays): strength, the light mask and its shafts (half
  // size) and the scene with them.
  float rays_strength_ = 0.0f;
  // Anti-aliased alpha test / alpha to coverage on multisampled targets
  // (RendererOptions::smooth_alpha).
  bool smooth_alpha_ = true;
  std::unordered_set<uint64_t> tree_textures_;  // the trees' foliage's (their trunks share them)
  // Revamped Graphics (SetRevamped): replacements on; changed: the cache's
  // replaced textures dropped at the frame's end.
  bool replace_on_ = true, drop_replaced_ = false;
  bool foliage_fade_ = true;  // RendererOptions::foliage_fade (with smooth_alpha_)
  Image* fog_result_ = nullptr;  // this frame's fogged / rayed scene
  // Ambient occlusion (SetAmbientOcclusion): strength (0 = off), the
  // occlusion at half size (two images: the estimate and its blur, R = the
  // light left, G = distance) and the darkened scene.
  float ao_strength_ = 0.0f;
  bool ao_debug_ = false;
  Image ao_img_[2], ao_out_img_;
  Image ao_prep_[3];  // builtin_ao_prep: the smaller depths for the far taps
  // The scene's depth as the lighting pass reads it: the world alone. The
  // game resolves depth again at the end of the frame with the riders,
  // particles that write depth (snow spray) and 3D HUD text in it, which
  // would cast occlusion where nothing shows.
  Image ao_depth_;
  bool ao_depth_ok_ = false;
  // ... and with what's drawn after the lighting up to the first
  // see-through draw that writes depth (snow spray, in-world labels): the
  // riders and their boards, which do cast occlusion.
  Image ao_late_depth_;
  bool ao_late_ok_ = false, ao_late_done_ = false;
  // Around the depth-only character passes: 0 none yet, 1 in them, 2 after.
  Image ao_before_hidden_, ao_after_hidden_;
  int ao_hidden_state_ = 0;
  bool ao_before_ok_ = false, ao_hidden_ok_ = false;
  bool ao_done_ = false;
  Image* ao_result_ = nullptr;
  Image* OccludedScene(Image* scene);
  bool SnapshotDepth(Image* depth, bool float_depth, Image& out, const char* name);
  Image ray_mask_img_, ray_img_, rays_out_img_;
  // Where the sun is on screen (uv) and how much of it counts (0 behind the
  // camera or far off screen .. 1 on screen).
  bool SunOnScreen(float uv[2], float* visible) const;
  void UpdateSpeed();
  bool CharacterAnchor(const Shader& vs, const uint32_t* src, float out[3]) const;
  // The nearest character (skinned, drawn after the lighting pass) to the
  // camera this frame and last frame (world units; large when none): riding
  // has the rider close by, the trail map and other overviews don't.
  float nearest_character_ = 1e9f, last_nearest_character_ = 0.0f;
  static constexpr float kRidingCameraDistance = 15.0f;
  float nearest_character_pos_[3] = {};
  float rider_pos_[3] = {}, riding_prev_cam_[3] = {};
  bool rider_known_ = false;
  bool riding_ = true;  // last frame (see EndFrame's riding test)
  // Map clouds (SetMapClouds): cartoon clouds cover the screen when the view
  // cuts between riding and the trail map, then part over kCloudSeconds; the
  // fog fades out (or in) with them.
  bool map_clouds_ = true;
  // Time of day (SetTimeOfDay, native/time_of_day.h).
  Renderer::TimeOfDay tod_;
  tod::Layout tod_layout_;
  tod::State tod_state_;          // this frame's (valid: in use)
  float tod_game_light_[3] = {};  // the mountain's own sun (c14), the layout's
  float tod_mountain_hour_ = -1.0f;
  // The lighting's sky-light table tinted (its tf1), and a plain white
  // stand-in for the game's fixed shadows once the sun has moved away.
  Image tod_lut_, tod_white_, tod_colors_, tod_sky_img_;
  Image tod_params_;  // the finished shadow mask's settings (TodShadows)
  Image* tod_lut_src_ = nullptr;
  bool tod_lut_ok_ = false;
  bool tod_sky_done_ = false;
  Image* tod_sky_result_ = nullptr;
  bool TodOn() const { return tod_.on && tod_state_.valid; }
  // (On, but only for its effects: the game's own light and sky kept.)
  bool TodPassive() const { return TodOn() && tod_.passive; }
  // The lighting's change this frame (time of day and/or eye adaptation):
  // toward the light (time of day), its color and the sky's light scaled.
  bool lm_on_ = false, lm_set_dir_ = false;
  tod::Vec3 lm_dir_, lm_light_scale_{1, 1, 1}, lm_ambient_scale_{1, 1, 1};
  bool LightModOn() const { return lm_on_; }
  // Eye adaptation (SetEyeAdaptation): the exposure the lighting is scaled
  // by, easing toward what the scene's average brightness (measured a few
  // frames back: read back without waiting) calls for.
  float eye_strength_ = 0.0f;
  int light_shadows_ = 1;  // SetLightShadows
  int light_shadow_lamps_ = 0;  // (how many of the nearest; 0: all)
  double exposure_ = 1.0;
  Buffer eye_rb_;                 // 4 slots of 16 bytes: the measure, by frame % 4
  double eye_rb_exposure_[4] = {};
  uint64_t eye_rb_frame_[4] = {};
  Image eye_img_;
  bool eye_done_ = false;
  void EyeUpdate();
  void EyeMeasure(Image* scene);
  bool TodNoGameShadows() const { return TodOn() && tod_state_.away >= 0.5; }
  void TodLightingInputs(const Draw& d, const GpuState& st);
  void TodShadows();
  // The game's lighting block in a copy of constants (time of day).
  void TodLightingBlock(uint32_t* c, int dir_reg, int sh_base, int sun_base) const {
    float v[4];
    if (lm_set_dir_) {
      std::memcpy(v, &c[dir_reg * 4], 16);
      v[0] = float(lm_dir_.x);
      v[1] = float(lm_dir_.y);
      v[2] = float(lm_dir_.z);
      std::memcpy(&c[dir_reg * 4], v, 16);
    }
    const tod::Vec3& ls = lm_light_scale_;
    const double amb[3] = {lm_ambient_scale_.x, lm_ambient_scale_.y, lm_ambient_scale_.z};
    for (int r = 0; r < 24; ++r) {
      const float k = float(amb[(r / 4) % 3]);
      std::memcpy(v, &c[(sh_base + r) * 4], 16);
      for (float& f : v) f *= k;
      std::memcpy(&c[(sh_base + r) * 4], v, 16);
    }
    for (int r = 0; r < 4; ++r) {
      std::memcpy(v, &c[(sun_base + r) * 4], 16);
      v[0] *= float(ls.x);
      v[1] *= float(ls.y);
      v[2] *= float(ls.z);
      std::memcpy(&c[(sun_base + r) * 4], v, 16);
    }
  }
  // Other shaders lit by the same block (riders, characters, props: their
  // vertex or pixel shaders, each keeping it, or parts of it, at its own
  // registers): every register holding one of the lighting pass's (the frame
  // before), by value, gets that register's value for the hour. Where they
  // are is looked up for each shader in its first few draws.
  struct TodBlock {
    std::vector<std::pair<uint16_t, uint16_t>> regs;  // (register, lighting pass register - 14)
    int looks = 0;
  };
  std::unordered_map<uint64_t, TodBlock> tod_blocks_[2];  // by stage, ucode hash
  uint32_t tod_ref_[29 * 4] = {};  // the lighting pass's c14..c42 (bits)
  uint32_t tod_new_[29 * 4] = {};  // the same for the hour
  bool tod_ref_ok_ = false;
  bool TodMatches(const uint32_t* v, int ref) const {
    const uint32_t* r = &tod_ref_[ref * 4];
    if ((r[0] | r[1] | r[2]) == 0) return false;  // (nothing to tell it by: zeros)
    return std::memcmp(v, r, ref == 0 ? 12 : 16) == 0;
  }
  bool TodNeeds(int stage, const Shader& sh) {
    if (!tod_ref_ok_) return false;
    auto it = tod_blocks_[stage].find(sh.info.ucode_hash);
    return it == tod_blocks_[stage].end() || it->second.looks < 4 || !it->second.regs.empty();
  }
  // (A register the shader never reads: the game's whole lighting block
  // sits in most shaders' constants, read or not; nothing to change there.)
  static bool TodReads(const Shader& sh, int r) {
    return sh.info.dynamic_constants || ((sh.info.float_constants[r >> 5] >> (r & 31)) & 1u);
  }
  // Where a shader keeps the lighting block (looked for in its first few
  // draws' constants `c`).
  TodBlock& TodScan(int stage, const Shader& sh, const uint32_t* c) {
    TodBlock& b = tod_blocks_[stage][sh.info.ucode_hash];
    if (b.looks < 4) {
      ++b.looks;
      for (int r = 0; r < 256; ++r) {
        if (!TodReads(sh, r) && !Diag("todallregs")) continue;
        for (int k = 0; k < 29; ++k) {
          if (!TodMatches(&c[r * 4], k)) continue;
          const std::pair<uint16_t, uint16_t> e{uint16_t(r), uint16_t(k)};
          if (std::find(b.regs.begin(), b.regs.end(), e) == b.regs.end()) b.regs.push_back(e);
          break;
        }
      }
    }
    return b;
  }
  // A draw's constants uploaded brought to the hour (once for each block
  // version and shader this frame): only the registers holding the lighting
  // block changed, in the upload itself.
  uint32_t TodConstants(int stage, const StateBlock<1024>& block, const Shader& sh);
  // The constants `c` (a copy) brought to the hour; false: nothing to change.
  bool TodApply(int stage, const Shader& sh, uint32_t* c) {
    if (!tod_ref_ok_) return false;
    TodBlock& b = tod_blocks_[stage][sh.info.ucode_hash];
    if (b.looks < 4) {
      ++b.looks;
      for (int r = 0; r < 256; ++r) {
        if (!TodReads(sh, r) && !Diag("todallregs")) continue;
        for (int k = 0; k < 29; ++k) {
          if (!TodMatches(&c[r * 4], k)) continue;
          const std::pair<uint16_t, uint16_t> e{uint16_t(r), uint16_t(k)};
          if (std::find(b.regs.begin(), b.regs.end(), e) == b.regs.end()) b.regs.push_back(e);
          break;
        }
      }
      if (Diag("tod") && b.looks == 1) {
        std::fprintf(stderr, "tod: %s %016llx %zu registers\n", stage ? "ps" : "vs",
                     (unsigned long long)sh.info.ucode_hash, b.regs.size());
      }
    }
    bool changed = false;
    for (const auto& [r, k] : b.regs) {
      if (!TodMatches(&c[r * 4], k)) continue;  // (not set to it this time)
      std::memcpy(&c[r * 4], &tod_new_[k * 4], k == 0 ? 12 : 16);
      changed = true;
    }
    return changed;
  }
  Image* TodSky(Image* scene);
  bool cloud_known_ = false, cloud_riding_ = true, cloud_active_ = false;
  float cloud_p_ = 1.0f;  // 0 .. 1 through the parting
  std::chrono::steady_clock::time_point cloud_start_{};
  static constexpr float kCloudSeconds = 1.2f;
  // A cut to or from the map: the clouds first close in over the last frame
  // before it (the parting in reverse, kCloseSeconds), then part over the
  // new view.
  static constexpr float kCloseSeconds = 0.9f;
  bool cloud_closing_ = false;
  Image cloud_held_;               // the last output before a cut (A2B10G10R10)
  bool cloud_held_valid_ = false;
  // Outputs in a row without the scene (a loading screen): after enough, the
  // next scene is a new level, and its trail map is revealed by the clouds.
  uint32_t cloud_dark_ = 0;
  bool cloud_after_load_ = false;  // the last such gap was long (a loading screen)
  // Outputs since the riding state was (re)established: a change this early
  // is the state settling (characters not drawn yet), not a cut.
  uint32_t cloud_settle_ = 0;
  // The volumetric fog fades (out to the map, in back to riding) over longer
  // than the clouds take to part, from when they start parting.
  static constexpr float kFogSeconds = 3.0f;
  bool fog_anim_ = false;
  // The game's trail map state (SetTrailMapOpen): open now, and when it last
  // changed. A cut plays the clouds only near such a change.
  bool map_known_ = false, map_open_ = false;
  bool in_scene_ = false;  // SetInCutscene
  // SetHideInterface: draws after the composite (the HUD, menus) are skipped
  // once composite_seen_ (this frame's composite has read the scene).
  bool hide_interface_ = false, composite_seen_ = false;
  int game_state_ = -1;    // SetGameState
  std::chrono::steady_clock::time_point map_change_{};
  bool MapChangedRecently(std::chrono::steady_clock::time_point now) const {
    return !map_known_ || now - map_change_ < std::chrono::seconds(3);
  }
  std::chrono::steady_clock::time_point fog_anim_start_{};
  void StartCloudsParting(std::chrono::steady_clock::time_point now) {
    cloud_active_ = true;
    cloud_closing_ = false;
    cloud_p_ = 0.0f;
    cloud_start_ = now;
    fog_anim_ = true;
    fog_anim_start_ = now;
  }
  void UpdateClouds();  // at the output: this frame's riding state
  // The map clouds drawn under the interface: into the picture right after
  // the composite (before the HUD, menus and the save notice draw over it),
  // while they part; at the output otherwise (closing over the held frame).
  bool clouds_updated_ = false;    // UpdateClouds run this frame
  bool clouds_drawn_ = false;      // drawn under the interface this frame
  Image* composite_rt_ = nullptr;  // where the composite drew (this frame)
  void CloudsUnderInterface();
  // The save notice (on the way into the trail map) kept over the clouds:
  // its draws held back while they part, drawn again after them.
  std::vector<std::pair<const Draw*, uint32_t>> notice_draws_;
  bool replaying_notice_ = false;
  bool notice_started_ = false;  // this frame: the notice's draws begun (the rest of the frame is it)
  bool IsSaveNotice(const Draw& d, const GpuState& st);
  // The game's own cloud puff (128x128, the pause menu's): found among the
  // textures the game uses (by its contents), or loaded from the file kept
  // from an earlier session; until then the puffs are drawn procedurally.
  static constexpr uint64_t kCloudSpriteHash = 0x3C157055F5ED7FBAull;
  static constexpr uint32_t kCloudSpriteSize = 128;
  Image cloud_sprite_;
  std::vector<uint8_t> cloud_sprite_pending_;  // RGBA to upload
  bool cloud_sprite_found_ = false;
  std::string CloudSpritePath() const;
  void LoadCloudSprite();
  void FindCloudSprite(const std::string& data, const amped3::native::TextureFetch& tf);
  void UploadCloudSprite();
  Image* RadialScene(Image* scene);
  bool player_known_ = false;    // the player character's position last frame
  float player_pos_[3] = {};
  uint32_t player_draws_ = 0;
  Image* vel_depth_ = nullptr;  // the depth target the copies are drawn against
  Image vel_img_, vel_single_;  // velocities (depth's sample count), resolved
  bool vel_ready_ = false;      // drawn this frame
  std::unordered_map<std::string, Shader> vel_shaders_;  // velocity variant by stem
  Shader vel_ps_;
  Shader exempt_ps_;  // blur exemption mask: w = 1
  // Constants by draw key (shader, buffers, range), every draw with that key
  // in drawing order (the same mesh drawn many times: trees, rocks).
  std::unordered_map<uint64_t, std::vector<std::vector<uint32_t>>> vel_prev_, vel_cur_;
  // For a mesh drawn many times last frame: the constants that differ between
  // its copies (where each one is placed), worked out once a frame, to match
  // each copy to last frame's by those alone. (Comparing every copy's
  // constants in full with every other's cost a carving challenge's 200-odd
  // markers some 25 ms a frame.)
  std::unordered_map<uint64_t, std::vector<uint32_t>> vel_prev_differ_;
  // ... and for many copies, them sorted by the constant that differs most
  // (with the next two), to find the nearest by looking either side.
  struct VelSorted {
    uint32_t dims[3] = {};
    uint32_t n_dims = 0;
    std::vector<std::pair<float, uint32_t>> order;
  };
  std::unordered_map<uint64_t, VelSorted> vel_prev_sorted_;
  double vel_match_ms_ = 0.0;  // (diagnostic velstat)
  std::vector<std::vector<uint32_t>> words_pool_;  // vel_prev_'s vectors, reused
  std::unordered_map<const Shader*, Shader*> vel_by_vs_;    // draw shader -> velocity shader
  std::unordered_map<VkPipeline, VkPipeline> vel_pipelines_;  // draw pipeline -> velocity one
  // The same for the copies drawn again into the sun maps (world, heights)
  // and the time of day's casters' map: draw pipeline -> theirs.
  std::unordered_map<VkPipeline, VkPipeline> sun_env_pipes_, sun_height_pipes_, tod_mover_pipes_;
  VkPipeline DerivedPipeline(std::unordered_map<VkPipeline, VkPipeline>& cache, VkPipeline from,
                             const std::function<VkPipeline()>& make) {
    if (from == VK_NULL_HANDLE || Diag("nopipecache")) return make();
    if (auto it = cache.find(from); it != cache.end()) return it->second;
    const VkPipeline p = make();
    if (p) cache.emplace(from, p);
    return p;
  }
  std::vector<Image*> vel_colors_;
  Shader* VelocityShader(const std::string& stem);
  void DrawVelocities();
  bool vel_mask_ = false;      // blur-exempt draws (event icons) masked this frame
  // Blur-exempt draws waiting for the velocity pass (drawn after it, over it).
  struct MaskDraw {
    VkPipeline pipeline;
    VkDescriptorSet tex_sets[2];
    uint32_t offsets[5];
    bool indexed;
    VkIndexType index_type;
    VkBuffer index_buffer;
    uint32_t first, count;
    VkRect2D scissor;
    float bias[2];
    uint32_t rt_w, rt_h;
  };
  std::vector<MaskDraw> mask_draws_;
  // FSR's reactive mask: the scene's blended draws that don't write depth
  // (snow, particles, glows) drawn again into reactive_img_ at the upscale,
  // so FSR leans on the current frame there instead of history (without it
  // a falling flake left a trail: its pixels had no motion of their own).
  std::vector<MaskDraw> reactive_draws_;
  Image reactive_img_;      // this frame's blended draws
  Image reactive_hist_[2];  // what FSR gets: with last frame's, carried along the motion
  uint32_t reactive_flip_ = 0;
  bool reactive_debug_ = false;  // show the mask over the (stretched) scene
  void QueueReactive(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                     const VkDescriptorSet* tex_sets, const uint32_t* offsets, bool indexed,
                     VkIndexType index_type, uint32_t first, uint32_t count, const VkRect2D& sc,
                     const float* bias, uint32_t rt_w, uint32_t rt_h);
  Image* DrawReactive(uint32_t w, uint32_t h, Image* motion);
  Image* mask_depth_ = nullptr;
  uint32_t post_light_draws_ = 0;  // scene draws after the lighting pass
  void DrawMasks(const MaskDraw* m, size_t n);
  bool vel_resolved_ = false;  // vel_single_ holds vel_img_'s samples' average
  bool EnsureVelocityImage(Image* depth);
  void MaskExempt(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                  Image* depth, const VkDescriptorSet* tex_sets, const uint32_t* offsets,
                  bool indexed, VkIndexType index_type, uint32_t first, uint32_t count,
                  const VkRect2D& sc, const float* bias, uint32_t rt_w, uint32_t rt_h);
  Image* Velocities() {
    if (!vel_ready_) return nullptr;
    if (vel_img_.samples == 1) return &vel_img_;
    if (!vel_resolved_) {
      // One sample per pixel for the blur: the samples' average.
      EndRendering();
      Barrier();
      CopyPass(CopyKind::kColorMs, &vel_img_, &vel_single_, 0, false, 0, vel_img_.samples, 0, 0,
               0, 0, vel_img_.width, vel_img_.height);
      Barrier();
      dyn_ = DynState{};
      vel_resolved_ = true;
    }
    return &vel_single_;
  }
};

// The part of a 4 KB constant block a shader reads: up to its highest float
// constant (all of it when it indexes them).
static size_t ConstBytes(const Shader& shader) {
  if (shader.info.dynamic_constants) return 4096;
  for (int w = 7; w >= 0; --w) {
    if (const uint32_t bits = shader.info.float_constants[w]) {
      return size_t(w * 32 + 32 - std::countl_zero(bits)) * 16;
    }
  }
  return 16;
}

uint32_t Renderer::Impl::ReserveConstants(size_t used, uint8_t** out) {
  // The descriptor still covers 4 KB from the offset (it must stay inside the
  // ring); only the part read is set aside, so the next block can start
  // right after it (less of the ring touched per frame).
  const size_t align = std::max<size_t>(gpu_.props.limits.minUniformBufferOffsetAlignment, 16);
  uniform_used_ = (uniform_used_ + align - 1) & ~(align - 1);
  if (uniform_used_ + 4096 > uniforms_.size) throw std::runtime_error("uniform ring full");
  const uint32_t offset = uint32_t(uniform_used_);
  *out = uniforms_.mapped + uniform_used_;
  uniform_used_ += used;
  return offset;
}

uint32_t Renderer::Impl::ReserveUniform(size_t size, uint8_t** out) {
  const size_t align = std::max<size_t>(gpu_.props.limits.minUniformBufferOffsetAlignment, 16);
  uniform_used_ = (uniform_used_ + align - 1) & ~(align - 1);
  if (uniform_used_ + size > uniforms_.size) throw std::runtime_error("uniform ring full");
  const uint32_t offset = uint32_t(uniform_used_);
  *out = uniforms_.mapped + uniform_used_;
  uniform_used_ += size;
  return offset;
}

// A stage's 4 KB float constant block for a draw: reused when a draw this
// frame already uploaded the same content for the same shader; otherwise
// only the constants the shader reads are copied (all of them when it
// indexes them), the rest of the block is left as it is.
uint32_t Renderer::Impl::ConstantsFor(int stage, const StateBlock<1024>& block,
                                      const Shader& shader) {
  const uint64_t version = block.version();
  for (const ConstUpload& c : const_uploads_[stage]) {
    if (c.version == version && c.shader == &shader) return c.offset;
  }
  uint8_t* out;
  const uint32_t offset = ReserveConstants(ConstBytes(shader), &out);
  const uint32_t* src = block.data();
  if (shader.info.dynamic_constants) {
    std::memcpy(out, src, 4096);
  } else {
    for (uint32_t w = 0; w < 8; ++w) {
      for (uint32_t bits = shader.info.float_constants[w]; bits; bits &= bits - 1) {
        const uint32_t c = w * 32 + uint32_t(std::countr_zero(bits));
        std::memcpy(out + c * 16, src + c * 4, 16);
      }
    }
  }
  ConstUpload& slot = const_uploads_[stage][const_upload_next_[stage]++ & 7];
  slot = {version, &shader, offset};
  return offset;
}

uint32_t Renderer::Impl::TodConstants(int stage, const StateBlock<1024>& block, const Shader& sh) {
  const uint64_t version = block.version();
  uint32_t off = 0;
  if (TodUploadFind(stage, version, &sh, &off)) return off;
  const uint32_t* src = block.data();
  const TodBlock& b = TodScan(stage, sh, src);
  bool any = false;
  for (const auto& [r, k] : b.regs) {
    if ((any = TodMatches(&src[r * 4], k))) break;
  }
  if (!any) {
    off = ConstantsFor(stage, block, sh);
  } else {
    uint8_t* out;
    off = ReserveConstants(ConstBytes(sh), &out);
    if (sh.info.dynamic_constants) {
      std::memcpy(out, src, 4096);
    } else {
      for (uint32_t w = 0; w < 8; ++w) {
        for (uint32_t bits = sh.info.float_constants[w]; bits; bits &= bits - 1) {
          const uint32_t c = w * 32 + uint32_t(std::countr_zero(bits));
          std::memcpy(out + c * 16, src + c * 4, 16);
        }
      }
    }
    for (const auto& [r, k] : b.regs) {
      if (TodMatches(&src[r * 4], k)) std::memcpy(out + r * 16, &tod_new_[k * 4], k == 0 ? 12 : 16);
    }
  }
  TodUploadKeep(stage, version, &sh, off);
  return off;
}

uint32_t Renderer::Impl::WriteUniform(const void* data, size_t size) {
  const size_t align = std::max<size_t>(gpu_.props.limits.minUniformBufferOffsetAlignment, 16);
  uniform_used_ = (uniform_used_ + align - 1) & ~(align - 1);
  if (uniform_used_ + size > uniforms_.size) throw std::runtime_error("uniform ring full");
  std::memcpy(uniforms_.mapped + uniform_used_, data, size);
  const uint32_t offset = uint32_t(uniform_used_);
  uniform_used_ += size;
  return offset;
}

// The snow tracks' clumps (builtin_tracks_apply, _shadow): broken-off lumps
// of snow lying on the heaps a board pushed up, round and a few to a dozen
// cm across, in cells of 3 texels (15 cm); their height over the heap (m,
// times the heap's full height), as much as there's heap.
static const std::string kSnowGlint = R"(
// Crystals in the snow: tiny mirrors in about half of the centimetre
// cells, each tilted its own way, flashing where its face meets the half-
// way vector h (so they come and go smoothly as the view or light moves),
// each a soft dot on the surface; fading out where a cell is smaller than a
// pixel (foot: a pixel's width there, metres) rather than shimmering.
// (Needs Hash3.)
float SnowGlint(vec3 P, vec3 N, vec3 h, float foot) {
  const float k = 85.0;
  float fade = clamp(1.8 / (k * max(foot, 1e-5)), 0.0, 1.0);
  if (fade <= 0.03) return 0.0;
  vec3 q = P * k;
  ivec3 c = ivec3(floor(q));
  if (Hash3(c, 1) > 0.5) return 0.0;
  vec3 off = fract(q) - vec3(Hash3(c, 2), Hash3(c, 3), Hash3(c, 4));
  off -= N * dot(off, N);  // (on the surface)
  float dotr = smoothstep(0.42, 0.06, length(off));
  if (dotr <= 0.0) return 0.0;
  vec3 n = normalize(N + (vec3(Hash3(c, 5), Hash3(c, 6), Hash3(c, 7)) * 2.0 - 1.0) * 0.38);
  float flash = pow(max(dot(n, h), 0.0), 900.0);
  return flash * dotr * fade * fade;
}
)";

static const std::string kTracksClumps = R"(
// A texel of the tracks' map as it shows: r groove, g heap, b how fresh
// (1 pressed now, worn to 0 over a few minutes): an old track fading out
// over the last third of its time, the oldest first.
vec2 Live(vec4 t) { return t.rg * smoothstep(0.0, 0.35, t.b); }
float ClumpHash(vec2 c) {
  uvec2 q = uvec2(ivec2(c) + 65536);
  uint h = q.x * 1597334677u ^ q.y * 3812015801u;
  h = (h ^ (h >> 15)) * 2246822519u;
  return float(h ^ (h >> 13)) * (1.0 / 4294967296.0);
}
float Clumps(vec2 tc, float heap) {
  if (heap < 0.04) return 0.0;
  vec2 p = tc / 3.0;
  vec2 i = floor(p), f = p - i;
  float h = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec2 c = i + vec2(x, y);
      if (ClumpHash(c) > 0.5) continue;
      vec2 at = vec2(x, y) + 0.2 + 0.6 * vec2(ClumpHash(c + vec2(7.0, 1.0)), ClumpHash(c + vec2(3.0, 11.0)));
      float rad = 0.22 + 0.35 * ClumpHash(c + vec2(5.0, 17.0));
      float dd = length(f - at) / rad;
      if (dd < 1.0) h = max(h, sqrt(1.0 - dd * dd) * rad);
    }
  }
  // (Up to about a third of a heap's height, on a full heap.)
  return h * 0.55 * smoothstep(0.04, 0.4, heap);
}
)";

VkShaderModule Renderer::Impl::CompileBuiltin(const std::string& name, const std::string& source,
                                              bool frag) {
  std::vector<uint32_t> spirv;
  std::string error;
  if (!options_.compile || !options_.compile(frag, name, source, &spirv, &error)) {
    throw std::runtime_error("built-in shader " + name + " failed to compile: " + error);
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  VkShaderModule m;
  vk::Check(vk::vkCreateShaderModule(gpu_.device, &ci, nullptr, &m), "vkCreateShaderModule");
  return m;
}

Shader* Renderer::Impl::GetShader(const std::string& stem, ShaderStage stage) {
  const int si = stage == ShaderStage::kVertex ? 0 : 1;
  if (last_shader_[si] && stem == last_shader_stem_[si]) return last_shader_[si];
  auto it = shaders_.find(stem);
  if (it != shaders_.end()) {
    last_shader_stem_[si] = stem;
    last_shader_[si] = &it->second;
    return &it->second;
  }
  const auto t_shader = std::chrono::steady_clock::now();
  struct Count {
    RenderStats& st;
    std::chrono::steady_clock::time_point t;
    ~Count() {
      ++st.new_shaders;
      st.shader_ms += MsSince(t);
    }
  } count{stats_, t_shader};
  Shader& s = shaders_[stem];
  auto bytes = res_->Read(stem);
  if (bytes.size() < 12) {
    // Not cached: the microcode may be available with a later frame.
    shaders_.erase(stem);
    missing_shader_ = Shader{};
    missing_shader_.error = "microcode missing: " + stem;
    return &missing_shader_;
  }
  std::vector<uint32_t> ucode(bytes.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) {
    ucode[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) |
               (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  amped3::native::TranslateOptions topt;
  topt.debug_vertex_stores = debug_stores_;
  topt.swap_variant = Diag("swap0") ? 0 : Diag("swap2") ? 2 : Diag("swap3") ? 3 : 1;
  topt.bit_zero_mul = bit_zero_mul_;
  if (stage == ShaderStage::kVertex) topt.safe_level = SafeLevel(ucode.data(), ucode.size());
  s.safe_level = topt.safe_level;
  if (s.safe_level) {
    char m[160];
    std::snprintf(m, sizeof(m), "vertex shader %016llX made plainer (level %d): it crashed the driver before",
                  (unsigned long long)amped3::native::HashMicrocode(ucode.data(), ucode.size()), s.safe_level);
    pending_notices_.push_back(m);
  }
  auto r = amped3::native::TranslateShader(stage, ucode.data(), ucode.size(), topt);
  if (!r.ok) {
    s.error = "translation failed: " + r.error;
    return &s;
  }
  s.info = r.info;
  if (stage == ShaderStage::kPixel) s.glsl = r.glsl;
  for (uint32_t vf = 0; vf < 96; ++vf) {
    if (s.info.vertex_fetch_constants[vf >> 5] & (1u << (vf & 31))) s.vfetch.push_back(uint8_t(vf));
  }
  if (!debug_stores_ && !precompiling_) {
    s.recipe = stage == ShaderStage::kVertex ? 1 : 2;
    s.recipe_hash = KeepMicrocode(bytes);
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!options_.compile ||
      !options_.compile(stage == ShaderStage::kPixel, stem, r.glsl, &spirv, &error)) {
    s.error = "GLSL compile failed for " + stem + (error.empty() ? "" : ": " + error);
    return &s;
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  if (vk::vkCreateShaderModule(gpu_.device, &ci, nullptr, &s.module) != VK_SUCCESS) {
    s.error = "vkCreateShaderModule failed";
  }
  return &s;
}

Image* Renderer::Impl::GetTarget(const TargetKey& base_key, bool resolve) {
  // Draws ask for the same few targets over and over: the last few answers
  // (images live as long as the renderer, never erased from targets_).
  if (!resolve) {
    const uint32_t scale_id = uint32_t(std::lround(TargetScale(base_key) * 80.0f));
    for (auto& c : target_cache_) {
      if (c.image && c.scale_id == scale_id && c.key.depth == base_key.depth &&
          c.key.base == base_key.base && c.key.format == base_key.format &&
          c.key.pitch == base_key.pitch && c.key.samples == base_key.samples) {
        *c.last_scale = scale_id;
        return c.image;
      }
    }
    Image* im = GetTargetUncached(base_key, false);
    auto ls = target_last_scale_.find(base_key);
    if (im && ls != target_last_scale_.end()) {
      auto& c = target_cache_[target_cache_next_++ & 3];
      c.key = base_key;
      c.scale_id = scale_id;
      c.last_scale = &ls->second;
      c.image = im;
    }
    return im;
  }
  return GetTargetUncached(base_key, true);
}

Image* Renderer::Impl::GetTargetUncached(const TargetKey& base_key, bool resolve) {
  float ts = TargetScale(base_key);
  TargetKey key = base_key;
  key.scale_id = uint32_t(std::lround(ts * 80.0f));
  if (resolve) {
    // A resolve copies out what was drawn: the surface at the scale it was
    // last drawn at (the 3D scale when drawn in a tiled pass and resolved
    // after it ended, as the menus are), not the current one.
    auto ls = target_last_scale_.find(base_key);
    if (ls != target_last_scale_.end() && ls->second != key.scale_id) {
      key.scale_id = ls->second;
      ts = float(ls->second) / 80.0f;
    }
  } else {
    target_last_scale_[base_key] = key.scale_id;
  }
  auto it = targets_.find(key);
  if (it != targets_.end()) return &it->second;
  uint32_t width = std::max<uint32_t>(key.pitch, 1);
  uint32_t height = 720;
  auto h = target_heights_.find(base_key);
  if (h != target_heights_.end() && h->second) height = h->second;
  // With predicated tiling the surface is one tile wide in EDRAM; the frame is
  // drawn once here, so the image covers the whole tiled area.
  if (tiling_.width) {
    width = std::max(width, tiling_.width);
    height = std::max(height, tiling_.height);
  }
  width = std::max(Scaled(width, ts), 1u);
  height = std::max(Scaled(height, ts), 1u);
  std::string fmt_name;
  VkFormat format;
  char name[96];
  if (key.depth) {
    format = key.format == 0 ? gpu_.d24s8 : VK_FORMAT_D32_SFLOAT_S8_UINT;
    std::snprintf(name, sizeof(name), "depth_%03X_%s_p%u_%ux%u", key.base,
                  key.format ? "D24FS8" : "D24S8", key.pitch, width, height);
  } else {
    format = ColorFormat(key.format, &fmt_name);
    std::snprintf(name, sizeof(name), "rt_%03X_%s_p%u_%ux%u", key.base, fmt_name.c_str(), key.pitch,
                  width, height);
  }
  // MSAA: the guest's sample count, or twice it when the device lacks that
  // count (SwiftShader has 4 but not 2), else one sample.
  uint32_t host_samples = 1, scale = 0;
  if (key.samples == 1 || !options_.msaa) {
    scale = 1;
  } else if (gpu_.SupportsSamples(format, key.depth, key.samples)) {
    host_samples = key.samples;
    scale = 1;
  } else if (gpu_.SupportsSamples(format, key.depth, key.samples * 2)) {
    host_samples = key.samples * 2;
    scale = 2;
  }
  if (key.samples > 1) {
    const size_t n = std::strlen(name);
    std::snprintf(name + n, sizeof(name) - n, "_%ux", key.samples);
  }
  EndRendering();  // barriers below can't be inside a rendering scope
  Image im = gpu_.CreateImage(width, height, format, key.depth, name, 1, host_samples);
  im.sample_scale = scale;
  im.scaled = true;
  im.scale = ts;
  ToGeneral(cb_, im);
  im.target = true;
  im.guest_samples = key.samples;
  Image& stored = targets_[key] = im;
  pending_clears_.erase(&stored);
  const float zero[4] = {0, 0, 0, 0};
  ClearImage(&stored, zero, 0.0f, 0, !key.depth, key.depth, key.depth);
  log_ << "created " << name << "\n";
  return &stored;
}

void Renderer::Impl::ClearImageNow(Image* im, const float* color, float z, uint32_t stencil,
                                   bool c, bool zf, bool s, const VkRect2D* rect) {
  const bool was_rendering = rendering_;
  auto colors = cur_colors_;
  auto depth = cur_depth_;
  if (rendering_) EndRendering();
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = im->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingAttachmentInfo stencil_att = att;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = rect ? *rect : VkRect2D{{0, 0}, {im->width, im->height}};
  ri.layerCount = 1;
  if (im->depth) {
    if (zf) {
      att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      att.clearValue.depthStencil = {z, stencil};
    }
    if (s) {
      stencil_att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      stencil_att.clearValue.depthStencil = {z, stencil};
    }
    ri.pDepthAttachment = &att;
    ri.pStencilAttachment = &stencil_att;
  } else {
    if (!c) return;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    for (int i = 0; i < 4; ++i) att.clearValue.color.float32[i] = color[i];
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &att;
  }
  Barrier();  // clears still waiting for other images come first
  vk::vkCmdBeginRendering(cb_, &ri);
  vk::vkCmdEndRendering(cb_);
  FullBarrier(cb_);
  ++stats_.clear_passes;
  if (was_rendering) BeginRendering(colors, depth);
}

void Renderer::Impl::ClearImage(Image* im, const float* color, float z, uint32_t stencil, bool c,
                                bool zf, bool s, const VkRect2D* rect) {
  if (!merge_clears_) return ClearImageNow(im, color, z, stencil, c, zf, s, rect);
  if (im->depth ? !(zf || s) : !c) return;
  // Inside the pass drawing to it: cleared there, the pass goes on.
  if (rendering_) {
    int index = -1;
    if (im->depth) {
      if (cur_depth_ == im) index = 0;
    } else {
      for (size_t i = 0; i < cur_colors_.size(); ++i) {
        if (cur_colors_[i] == im) index = int(i);
      }
    }
    if (index >= 0) {
      VkClearAttachment ca{};
      if (im->depth) {
        ca.aspectMask = (zf ? VK_IMAGE_ASPECT_DEPTH_BIT : 0u) | (s ? VK_IMAGE_ASPECT_STENCIL_BIT : 0u);
        ca.clearValue.depthStencil = {z, stencil};
      } else {
        ca.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ca.colorAttachment = uint32_t(index);
        for (int i = 0; i < 4; ++i) ca.clearValue.color.float32[i] = color[i];
      }
      uint32_t w = UINT32_MAX, h = UINT32_MAX;  // the pass's area
      for (Image* c_im : cur_colors_) w = std::min(w, c_im->width), h = std::min(h, c_im->height);
      if (cur_depth_) w = std::min(w, cur_depth_->width), h = std::min(h, cur_depth_->height);
      VkClearRect cr{};
      cr.rect = rect ? *rect : VkRect2D{{0, 0}, {w, h}};
      cr.rect.offset.x = std::min<int32_t>(cr.rect.offset.x, int32_t(w));
      cr.rect.offset.y = std::min<int32_t>(cr.rect.offset.y, int32_t(h));
      cr.rect.extent.width = std::min(cr.rect.extent.width, w - uint32_t(cr.rect.offset.x));
      cr.rect.extent.height = std::min(cr.rect.extent.height, h - uint32_t(cr.rect.offset.y));
      cr.layerCount = 1;
      // The pass may not cover all of the image (another attachment is
      // smaller): then as before.
      const bool covers = rect || (w == im->width && h == im->height);
      if (covers && cr.rect.extent.width && cr.rect.extent.height) {
        vk::vkCmdClearAttachments(cb_, 1, &ca, 1, &cr);
        ++stats_.clears_in_pass;
        return;
      }
      if (covers) return;  // an empty rectangle
    }
  }
  // All of the image: waits for the next pass on it (its load op clears).
  const bool whole = !rect || (rect->offset.x == 0 && rect->offset.y == 0 &&
                               rect->extent.width >= im->width && rect->extent.height >= im->height);
  if (!whole) {
    // A part: collected while the same clear covers the image part by part
    // (EDRAM tiles), else done now.
    auto it = pending_clears_.find(im);
    const bool same =
        it != pending_clears_.end() && !it->second.rects.empty() &&
        (im->depth ? (it->second.depth == zf && it->second.stencil == s &&
                      (!zf || it->second.z == z) && (!s || it->second.s == stencil))
                   : (it->second.color && std::equal(color, color + 4, it->second.rgba)));
    if (it != pending_clears_.end() && !same) FlushPendingClears();  // in order
    if (it == pending_clears_.end() || !same) {
      PendingClear p;
      if (im->depth) {
        p.depth = zf, p.z = z, p.stencil = s, p.s = stencil;
      } else {
        p.color = true;
        std::copy(color, color + 4, p.rgba);
      }
      p.rects.push_back(*rect);
      pending_clears_[im] = std::move(p);
      return;
    }
    PendingClear& p = it->second;
    p.rects.push_back(*rect);
    // Covered: every cell between the rectangles' edges inside one of them.
    std::vector<int64_t> xs = {0, int64_t(im->width)}, ys = {0, int64_t(im->height)};
    for (const VkRect2D& r : p.rects) {
      xs.push_back(r.offset.x), xs.push_back(int64_t(r.offset.x) + r.extent.width);
      ys.push_back(r.offset.y), ys.push_back(int64_t(r.offset.y) + r.extent.height);
    }
    std::sort(xs.begin(), xs.end());
    std::sort(ys.begin(), ys.end());
    bool covered = true;
    for (size_t i = 0; covered && i + 1 < xs.size(); ++i) {
      if (xs[i] == xs[i + 1] || xs[i] >= int64_t(im->width)) continue;
      for (size_t j = 0; covered && j + 1 < ys.size(); ++j) {
        if (ys[j] == ys[j + 1] || ys[j] >= int64_t(im->height)) continue;
        const int64_t cx = xs[i], cy = ys[j];
        bool in = false;
        for (const VkRect2D& r : p.rects) {
          if (cx >= r.offset.x && cx < int64_t(r.offset.x) + r.extent.width && cy >= r.offset.y &&
              cy < int64_t(r.offset.y) + r.extent.height) {
            in = true;
            break;
          }
        }
        covered = in;
      }
    }
    if (covered) p.rects.clear();  // all of it now
    return;
  }
  if (auto it = pending_clears_.find(im); it != pending_clears_.end() && !it->second.rects.empty()) {
    FlushPendingClears();  // parts of an earlier clear first
  }
  PendingClear& p = pending_clears_[im];
  if (im->depth) {
    if (zf) p.depth = true, p.z = z;
    if (s) p.stencil = true, p.s = stencil;
  } else {
    p.color = true;
    for (int i = 0; i < 4; ++i) p.rgba[i] = color[i];
  }
}

void Renderer::Impl::FlushPendingClears() { FlushPendingClearsExcept(nullptr, nullptr); }

void Renderer::Impl::FlushPendingClearsExcept(const Image* src, const VkRect2D* rect,
                                              const Image* dst) {
  if (pending_clears_.empty()) return;
  auto pending = std::move(pending_clears_);
  pending_clears_.clear();
  auto overlaps = [](const VkRect2D& a, const VkRect2D& b) {
    return int64_t(a.offset.x) < int64_t(b.offset.x) + b.extent.width &&
           int64_t(b.offset.x) < int64_t(a.offset.x) + a.extent.width &&
           int64_t(a.offset.y) < int64_t(b.offset.y) + b.extent.height &&
           int64_t(b.offset.y) < int64_t(a.offset.y) + a.extent.height;
  };
  for (auto& [im, p] : pending) {
    // What isn't read or written keeps waiting: other images, and parts of
    // the source outside the rectangle read.
    const bool keep =
        src && im != dst &&
        (im != src || (rect && !p.rects.empty() &&
                       std::none_of(p.rects.begin(), p.rects.end(),
                                    [&](const VkRect2D& r) { return overlaps(r, *rect); })));
    if (keep) {
      pending_clears_[im] = std::move(p);
      continue;
    }
    if (p.rects.empty()) {
      ClearImageNow(im, p.rgba, p.z, p.s, p.color, p.depth, p.stencil, nullptr);
    } else {
      for (const VkRect2D& r : p.rects) {
        ClearImageNow(im, p.rgba, p.z, p.s, p.color, p.depth, p.stencil, &r);
      }
    }
  }
}

void Renderer::Impl::FlushPendingClearsKeepTargets(const Image* but, const Image* but2) {
  if (pending_clears_.empty()) return;
  std::unordered_map<Image*, PendingClear> keep;
  for (auto it = pending_clears_.begin(); it != pending_clears_.end();) {
    Image* im = it->first;
    if (im->target && im != but && im != but2) {
      keep.emplace(im, std::move(it->second));
      it = pending_clears_.erase(it);
    } else {
      ++it;
    }
  }
  FlushPendingClears();
  pending_clears_ = std::move(keep);
}

void Renderer::Impl::ResolveBarrier(const Image* src, const VkRect2D& rect) {
  FlushPendingClearsExcept(src, &rect);
  FullBarrier(cb_);
}

void Renderer::Impl::Barrier() {
  FlushPendingClears();
  FullBarrier(cb_);
}

void Renderer::Impl::BeginRendering(const std::vector<Image*>& colors, Image* depth) {
  if (rendering_ && colors == cur_colors_ && depth == cur_depth_) return;
  if (rendering_) EndRendering();
  dyn_ = DynState{};  // all of it: a new pass or command buffer binds nothing yet
  std::vector<VkRenderingAttachmentInfo> atts(colors.size(),
                                              {VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO});
  uint32_t w = UINT32_MAX, h = UINT32_MAX;
  for (size_t i = 0; i < colors.size(); ++i) {
    atts[i].imageView = colors[i]->view;
    atts[i].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    atts[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    w = std::min(w, colors[i]->width);
    h = std::min(h, colors[i]->height);
  }
  VkRenderingAttachmentInfo datt{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  if (depth) {
    datt.imageView = depth->view;
    datt.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
    datt.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    datt.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    w = std::min(w, depth->width);
    h = std::min(h, depth->height);
  }
  VkRenderingAttachmentInfo satt = datt;  // stencil: its own load op
  // Clears waiting for these images become their load ops (when the pass
  // covers all of the image); the rest are done first, by Barrier.
  if (!pending_clears_.empty()) {
    for (size_t i = 0; i < colors.size(); ++i) {
      auto it = pending_clears_.find(colors[i]);
      if (it == pending_clears_.end() || !it->second.rects.empty() || colors[i]->width != w ||
          colors[i]->height != h) {
        continue;
      }
      atts[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
      for (int c = 0; c < 4; ++c) atts[i].clearValue.color.float32[c] = it->second.rgba[c];
      pending_clears_.erase(it);
      ++stats_.clears_merged;
    }
    if (depth && depth->width == w && depth->height == h) {
      auto it = pending_clears_.find(depth);
      if (it != pending_clears_.end() && it->second.rects.empty()) {
        if (it->second.depth) {
          datt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
          datt.clearValue.depthStencil = {it->second.z, it->second.s};
        }
        if (it->second.stencil) {
          satt.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
          satt.clearValue.depthStencil = {it->second.z, it->second.s};
        }
        pending_clears_.erase(it);
        ++stats_.clears_merged;
      }
    }
  }
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = uint32_t(atts.size());
  ri.pColorAttachments = atts.data();
  ri.pDepthAttachment = depth ? &datt : nullptr;
  ri.pStencilAttachment = depth ? &satt : nullptr;
  if (keep_target_clears_ && !pending_clears_.empty()) {
    // Done now: those for this pass's attachments (not taken as load ops) and
    // for anything that isn't a render target. (Only color attachments are
    // in `colors`; the depth one is checked by the helper's second image.)
    for (Image* c : colors) {
      if (auto it = pending_clears_.find(c); it != pending_clears_.end()) {
        auto one = std::move(*it);
        pending_clears_.erase(it);
        auto rest = std::move(pending_clears_);
        pending_clears_.clear();
        pending_clears_.insert(std::move(one));
        FlushPendingClears();
        pending_clears_ = std::move(rest);
      }
    }
    FlushPendingClearsKeepTargets(depth, nullptr);
    FullBarrier(cb_);
  } else {
    Barrier();
  }
  vk::vkCmdBeginRendering(cb_, &ri);
  ++stats_.passes;
  rendering_ = true;
  cur_colors_ = colors;
  cur_depth_ = depth;
}

void Renderer::Impl::EndRendering() {
  if (!rendering_) return;
  vk::vkCmdEndRendering(cb_);
  rendering_ = false;
  dyn_ = DynState{};  // all of it: a new pass or command buffer binds nothing yet
  cur_colors_.clear();
  cur_depth_ = nullptr;
}

VkSampler Renderer::Impl::SamplerFor(const uint32_t* fetch, const amped3::native::TextureBinding& b,
                                     float lod_bias) {
  amped3::native::TextureFetch tf;
  std::memcpy(tf.words, fetch, sizeof(tf.words));
  const int mag = b.mag_filter >= 0 ? b.mag_filter : int(tf.mag_filter());
  const int min = b.min_filter >= 0 ? b.min_filter : int(tf.min_filter());
  int aniso = b.aniso_filter >= 0 ? b.aniso_filter : int(tf.aniso_filter());
  // Mip filter: 0 point, 1 linear, 2 base map only.
  const int mip = b.mip_filter >= 0 ? b.mip_filter : int(tf.mip_filter());
  const uint32_t min_level = tf.mip_min_level();
  const uint32_t max_level = mip == 2 ? min_level : std::max(tf.mip_max_level(), min_level);
  // User override (same encoding as the guest: 0 off, 1 1x, 2 2x, 3 4x, 4 8x,
  // 5 16x), applied to mipmapped textures only, as the SDK does.
  if (options_.anisotropy_override >= 0 && mip != 2 && max_level > min_level) {
    aniso = options_.anisotropy_override;
  }
  uint64_t key = 0;
  for (uint32_t v : {uint32_t(tf.clamp(0)), uint32_t(tf.clamp(1)), uint32_t(tf.clamp(2)),
                     uint32_t(mag), uint32_t(min), uint32_t(aniso), uint32_t(tf.border_color()),
                     uint32_t(mip), min_level, max_level}) {
    key = (key << 6) | (v & 63);
  }
  if (max_level <= min_level) lod_bias = 0.0f;  // no mipmaps: nothing to bias
  key ^= uint64_t(int32_t(std::lround(lod_bias * 64.0f)) & 0xFFF) << 52;
  auto it = samplers_.find(key);
  if (it != samplers_.end()) return it->second;
  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = mag == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.minFilter = min == 1 ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
  si.mipmapMode = mip == 1 ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST;
  si.addressModeU = AddressMode(tf.clamp(0));
  si.addressModeV = AddressMode(tf.clamp(1));
  si.addressModeW = AddressMode(tf.clamp(2));
  // Clamped to the image's levels by Vulkan; single-level images (render
  // targets, resolves) sample their base.
  si.minLod = float(min_level);
  si.maxLod = float(max_level);
  si.mipLodBias = std::clamp(lod_bias, -gpu_.props.limits.maxSamplerLodBias,
                             gpu_.props.limits.maxSamplerLodBias);
  si.borderColor = tf.border_color() == 1 ? VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE
                   : tf.border_color() == 0 ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK
                                            : VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK;
  if (aniso >= 2 && gpu_.anisotropy) {
    si.anisotropyEnable = VK_TRUE;
    si.maxAnisotropy = std::min(float(1 << (aniso - 1)), gpu_.props.limits.maxSamplerAnisotropy);
  }
  VkSampler s;
  vk::Check(vk::vkCreateSampler(gpu_.device, &si, nullptr, &s), "vkCreateSampler");
  samplers_[key] = s;
  return s;
}

Image* Renderer::Impl::TextureFor(const Draw& draw, uint32_t fc, const GpuState& st, std::string* note) {
  amped3::native::TextureFetch tf;
  std::memcpy(tf.words, FetchWords(draw, fc, st), sizeof(tf.words));
  // Rendered by a resolve (this frame or an earlier one)?
  const uint32_t tex_base = tf.base_address() & 0x1FFFFFFF;
  const uint32_t tex_format = tf.format();
  std::string data;
  for (const auto& t : draw.textures) {
    if (t.slot == fc) data = t.data;
  }
  if (!ResolveStillValid(tex_base, data)) DropResolve(tex_base);
  // Resolved depth read as a color texture (k_8_8_8_8 etc.): the raw words.
  if (tex_format != 22 && tex_format != 23) {
    auto pit = resolved_packed_.find(tex_base);
    if (pit != resolved_packed_.end()) return &pit->second;
  }
  // The finished scene read after the tiled pass: motion blurred (camera).
  // The composite reading the scene: motion blurred, then upscaled.
  const bool blur = blur_scale_ > 0.0f && (camera_blur_ || (object_blur_ && vel_ready_)) &&
                    prev_valid_;
  const bool radial = speed_strength_ > 0.0f && speed_amount_ > 0.002f;
  const bool fog = (fog_density_ > 0.0f || rays_strength_ > 0.0f) && fog_presence_ > 0.002f;
  const bool ao = ao_strength_ > 0.0f;
  // (With the time of day, from the hour's sun: the world maps follow it.)
  const bool movers = world_detail_ > 0;
  const bool tod_sky = TodOn();
  const bool eye = eye_strength_ > 0.0f;
  if (light_seen_ && !tiling_.width && tex_base == scene_base_) {
    composite_seen_ = true;
    // (This frame's trails into the kept tracks, once.)
    if (!tracks_draws_.empty() || (tracks_ > 0.0f && std::getenv("A3_TEST_TRACKS"))) TracksUpdate();
  }
  if ((blur || upscaler_ || radial || fog || ao || movers || tod_sky || eye || fade_active_ || fade_last_on_) && light_seen_ &&
      !tiling_.width &&
      tex_base == scene_base_) {
    auto sit = resolved_.find(tex_base);
    if (sit != resolved_.end()) {
      Image* scene = &sit->second;
      // GPU time by part: each effect on its own (they run once a frame, at
      // the composite's first draw).
      EndRendering();
      MarkPart("scene done (resolves)");
      // The sky for the hour first (the fog takes its colors from it).
      if (tod_sky) {
        if (Image* sky = TodSky(scene)) scene = sky;
        MarkPart("effect: time of day sky");
      }
      // The night lights, and the screen's middle (placing them).
      if (tod_sky) {
        auto dit = resolved_.find(depth_base_);
        Image* dimg = dit != resolved_.end() && dit->second.format == VK_FORMAT_R32_SFLOAT &&
                              dit->second.samples == 1
                          ? &dit->second
                          : nullptr;
        if (dimg) {
          if (center_probe_) CenterProbe(dimg);
          if (Image* lit = NightLights(scene, dimg)) scene = lit;
          MarkPart("effect: night lights");
        }
      }
      // The sun's sheen (made at the lighting).
      if (tod_sky && spec_ok_) {
        if (Image* glossy = SpecularScene(scene)) scene = glossy;
        MarkPart("effect: sun sheen");
      }
      // How bright the lit scene is (eye adaptation).
      if (eye) EyeMeasure(scene);
      // Riders in the world's shade first (they're lit by themselves).
      if (movers) {
        if (Image* shaded = ShadedMovers(scene)) scene = shaded;
        MarkPart("effect: shaded riders");
      }
      // Ambient occlusion first: the fog then covers it in the distance.
      if (ao) {
        if (Image* occluded = OccludedScene(scene)) scene = occluded;
        MarkPart("effect: ambient occlusion");
      }
      if (fog) {
        if (Image* fogged = FoggedScene(scene)) scene = fogged;
        MarkPart("effect: fog / god rays");
      }
      if (VolumeOn()) {
        if (Image* lit_air = VolumeScene(scene)) scene = lit_air;
        MarkPart("effect: volumetric light");
      }
      if (tod_sky && tod_.snow > 0.002f) {
        if (Image* snowy = SnowScene(scene)) scene = snowy;
        MarkPart("effect: snow");
      }
      if (tod_sky && dust_ > 0.0f) {
        if (Image* dusty = DustScene(scene)) scene = dusty;
        MarkPart("effect: diamond dust");
      }
      if (tod_sky && (bloom_ > 0.0f || flare_ > 0.0f)) {
        if (Image* bloomed = BloomScene(scene)) scene = bloomed;
        MarkPart("effect: bloom and lens flare");
      }
      if (blur) {
        if (Image* blurred = BlurredScene(scene)) scene = blurred;
        MarkPart("effect: motion blur");
      }
      if (radial) {
        if (Image* streaked = RadialScene(scene)) scene = streaked;
        MarkPart("effect: speed blur");
      }
      // The event icons kept clear of the lighting effects.
      if (scene != &sit->second && vel_mask_ && !Diag("noiconkeep")) {
        Image* vel = Velocities();
        if (vel && vel->width == scene->width && vel->height == scene->height && scene->samples == 1 &&
            sit->second.width == scene->width && sit->second.height == scene->height && sit->second.samples == 1) {
          EndRendering();
          if (!icons_out_.image || icons_out_.width != scene->width || icons_out_.height != scene->height ||
              icons_out_.format != scene->format) {
            if (icons_out_.image) Retire(icons_out_);
            icons_out_ = gpu_.CreateImage(scene->width, scene->height, scene->format, false, "icons_kept");
            ToGeneral(cb_, icons_out_);
          }
          icons_out_.scaled = scene->scaled;
          icons_out_.scale = scene->scale;
          Barrier();
          float none[4] = {};
          FullscreenPass(CopyKind::kKeepIcons, &icons_out_, scene, &sit->second, vel, false, none, 4);
          Barrier();
          dyn_ = DynState{};
          scene = &icons_out_;
        }
      }
      if (Image* faded = TodFade(scene)) scene = faded;
      if (upscaler_) {
        if (Image* up = UpscaledScene(scene)) scene = up;
        MarkPart("effect: upscaler");
      }
      // Drifting snow: after the upscaler (streaming over the ground, it has
      // no motion of its own the upscaler could follow: it smeared and broke
      // up through it), at the picture's own size.
      if (tod_sky && drift_ && std::hypot(tod_.wind[0], tod_.wind[1]) > 4.5f && !Diag("nodrift")) {
        if (Image* drifting = DriftScene(scene)) scene = drifting;
        MarkPart("effect: drifting snow");
      }
      if (scene != &sit->second) return scene;
    }
  }
  // Resolved color, when the texture reads it in a compatible format (memory
  // the CPU fills, like video frames in YUV formats, isn't a resolve).
  auto rit = resolved_.find(tex_base);
  if (rit != resolved_.end()) {
    auto fit = resolved_format_.find(tex_base);
    if (fit == resolved_format_.end() || ResolveCompatible(fit->second, tex_format)) {
      return &rit->second;
    }
  }
  if (data.empty()) {
    *note = "no texture data for tf" + std::to_string(fc);
    return &dummy_;
  }
  std::string key = TextureKey(data, tf);
  auto it = textures_.find(key);
  if (it != textures_.end()) {
    it->second.last_used = frame_number_;
    if (!it->second.note.empty()) *note = it->second.note;
    return &it->second.image;
  }
  // A replacement (texture pack, sharper fonts)?
  if (replacer_ && replace_on_ && replacer_->MayReplace(tf)) {
    auto bytes = res_->Read(data + ".base");
    auto rep = replacer_->Find(tf, bytes.data(), bytes.size());
    for (auto& line : replacer_->TakeMessages()) {
      log_ << line << "\n";
      stats_.notices.push_back(std::move(line));
    }
    if (rep) {
      prefetched_.erase(key);
      return UploadReplacement(key, data, tf, *rep, note);
    }
  }
  const auto t_tex = std::chrono::steady_clock::now();
  std::vector<amped3::native::DecodedTexture> levels;
  if (auto pf = prefetched_.find(key); pf != prefetched_.end() && !pf->second.empty()) {
    levels = std::move(pf->second);
    prefetched_.erase(pf);
  } else {
    levels = DecodeFromResources(*res_, data, tf, KeepBlocks());
  }
  if (!cloud_sprite_found_) FindCloudSprite(data, tf);
  const auto& decoded = levels[0];
  if (!decoded.note.empty()) *note = data + ": " + decoded.note;
  // Gamma textures (sign = 3 on XYZ) are sampled as sRGB.
  const bool gamma = ((tf.words[0] >> 2) & 3) == 3;
  VkFormat format = gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  VkComponentMapping components{};  // identity: the swizzle is in the texels
  if (decoded.bc) {
    // Blocks as the guest has them: the GPU decodes them, and the fetch
    // swizzle (applied to RGBA texels when the CPU decodes) is the view's.
    switch (decoded.bc) {
      case 1: format = gamma ? VK_FORMAT_BC1_RGBA_SRGB_BLOCK : VK_FORMAT_BC1_RGBA_UNORM_BLOCK; break;
      case 2: format = gamma ? VK_FORMAT_BC2_SRGB_BLOCK : VK_FORMAT_BC2_UNORM_BLOCK; break;
      case 3: format = gamma ? VK_FORMAT_BC3_SRGB_BLOCK : VK_FORMAT_BC3_UNORM_BLOCK; break;
      case 4: format = VK_FORMAT_BC4_UNORM_BLOCK; break;
      default: format = VK_FORMAT_BC5_UNORM_BLOCK; break;
    }
    const uint32_t swizzle = tf.swizzle();
    VkComponentSwizzle* out[4] = {&components.r, &components.g, &components.b, &components.a};
    static const VkComponentSwizzle kChannel[4] = {
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (swizzle >> (3 * c)) & 7;
      *out[c] = sel < 4 ? kChannel[amped3::native::BcChannel(decoded.bc, sel)]
                        : sel == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
    }
  }
  Image im = gpu_.CreateTexture(decoded.width, decoded.height, format, uint32_t(levels.size()),
                                data, components);
  im.surface = SurfaceOf(decoded, components);
  // Upload through a staging buffer recorded into the current command buffer;
  // it is freed once the frame has finished.
  size_t total = 0;
  for (const auto& l : levels) total += l.rgba.size();
  VkBuffer upload_buffer;
  VkDeviceSize upload_offset;
  uint8_t* upload_mapped;
  UploadSpace(total, &upload_buffer, &upload_offset, &upload_mapped);
  std::vector<VkBufferImageCopy> regions;
  size_t at = 0;
  for (uint32_t i = 0; i < levels.size(); ++i) {
    std::memcpy(upload_mapped + at, levels[i].rgba.data(), levels[i].rgba.size());
    VkBufferImageCopy region{};
    region.bufferOffset = upload_offset + at;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    region.imageExtent = {levels[i].width, levels[i].height, 1};
    regions.push_back(region);
    at += levels[i].rgba.size();
  }
  // In the uploads command buffer (runs before the frame's): the render pass
  // in progress goes on.
  {
    VkCommandBuffer ucb = UploadCommands();
    ToGeneral(ucb, im);
    vk::vkCmdCopyBufferToImage(ucb, upload_buffer, im.image, VK_IMAGE_LAYOUT_GENERAL,
                               uint32_t(regions.size()), regions.data());
  }
  // A texture whose contents changed (video frames) replaces its previous
  // version right away instead of keeping one image per frame until they
  // expire: same fetch constant name ("tex_<fetch hash>"), same words.
  const size_t cut = data.find('_', 4);
  if (cut != std::string::npos) {
    char slot_buf[96];
    std::snprintf(slot_buf, sizeof(slot_buf), "%.*s.%08X.%08X.%08X.%08X", int(cut), data.c_str(),
                  tf.words[0], tf.words[1], tf.words[2], tf.words[3]);
    std::string& previous = texture_versions_[slot_buf];
    if (!previous.empty() && previous != key) {
      auto old = textures_.find(previous);
      if (old != textures_.end() && old->second.last_used != frame_number_) {
        texture_bytes_ -= std::min<uint64_t>(texture_bytes_, old->second.bytes);
        known_textures_.erase(old->second.hash);
        Retire(old->second.image);
        textures_.erase(old);
      }
    }
    previous = key;
  }
  CachedTexture& cached = textures_[key];
  cached.image = im;
  cached.last_used = frame_number_;
  cached.note = *note;
  cached.bytes = total;
  cached.hash = TextureHash(data, tf);
  cached.may_replace = replacer_ && replacer_->MayReplace(tf);
  known_textures_.insert(cached.hash);
  texture_bytes_ += total;
  ++stats_.new_textures;
  stats_.texture_mb += double(total) / (1 << 20);
  stats_.texture_ms += MsSince(t_tex);
  return &cached.image;
}

// A replacement's levels into a new image, cached like a decoded texture.
Image* Renderer::Impl::UploadReplacement(const std::string& key, const std::string& data,
                                         const amped3::native::TextureFetch& tf,
                                         const amped3::native::Replacement& rep,
                                         std::string* note) {
  const auto t_tex = std::chrono::steady_clock::now();
  const bool gamma = ((tf.words[0] >> 2) & 3) == 3;
  const VkFormat format = gamma ? VK_FORMAT_R8G8B8A8_SRGB : VK_FORMAT_R8G8B8A8_UNORM;
  VkComponentMapping components{};  // identity: texels as sampled
  if (!rep.swizzled) {
    // In the format's natural order (as the BC blocks decode): the fetch
    // swizzle is the view's, as for BC textures.
    const uint32_t bc = amped3::native::BcFormatFor(tf.format());
    const uint32_t swizzle = tf.swizzle();
    VkComponentSwizzle* out[4] = {&components.r, &components.g, &components.b, &components.a};
    static const VkComponentSwizzle kChannel[4] = {
        VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_G, VK_COMPONENT_SWIZZLE_B,
        VK_COMPONENT_SWIZZLE_A};
    for (int c = 0; c < 4; ++c) {
      const uint32_t sel = (swizzle >> (3 * c)) & 7;
      *out[c] = sel < 4 ? kChannel[amped3::native::BcChannel(bc, sel)]
                        : sel == 5 ? VK_COMPONENT_SWIZZLE_ONE : VK_COMPONENT_SWIZZLE_ZERO;
    }
  }
  const auto& base = rep.levels[0];
  Image im = gpu_.CreateTexture(base.width, base.height, format, uint32_t(rep.levels.size()),
                                data + ".replaced", components);
  im.replaced = true;
  // Larger than the game's texture (an upscaled one is 4x): fetches in guest
  // texels (unnormalized coordinates, texel offsets, filter weights) divide
  // by the guest size, as for a render target drawn at a higher resolution.
  im.scale = tf.width() ? float(base.width) / float(tf.width()) : 1.0f;
  size_t total = 0;
  for (const auto& l : rep.levels) total += l.rgba.size();
  VkBuffer upload_buffer;
  VkDeviceSize upload_offset;
  uint8_t* upload_mapped;
  UploadSpace(total, &upload_buffer, &upload_offset, &upload_mapped);
  std::vector<VkBufferImageCopy> regions;
  size_t at = 0;
  for (uint32_t i = 0; i < rep.levels.size(); ++i) {
    std::memcpy(upload_mapped + at, rep.levels[i].rgba.data(), rep.levels[i].rgba.size());
    VkBufferImageCopy region{};
    region.bufferOffset = upload_offset + at;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
    region.imageExtent = {rep.levels[i].width, rep.levels[i].height, 1};
    regions.push_back(region);
    at += rep.levels[i].rgba.size();
  }
  // In the uploads command buffer (runs before the frame's): the render pass
  // in progress goes on.
  {
    VkCommandBuffer ucb = UploadCommands();
    ToGeneral(ucb, im);
    vk::vkCmdCopyBufferToImage(ucb, upload_buffer, im.image, VK_IMAGE_LAYOUT_GENERAL,
                               uint32_t(regions.size()), regions.data());
  }
  CachedTexture& cached = textures_[key];
  cached.image = im;
  cached.last_used = frame_number_;
  cached.note = *note;
  cached.bytes = total;
  cached.hash = TextureHash(data, tf);
  cached.replaced = true;
  known_textures_.insert(cached.hash);
  texture_bytes_ += total;
  ++stats_.new_textures;
  stats_.texture_mb += double(total) / (1 << 20);
  stats_.texture_ms += MsSince(t_tex);
  return &cached.image;
}

// A game pixel shader for a pipeline with no color targets and one sample:
// the same program without its color outputs and sample mask (they'd go
// nowhere). Turnip (Mesa 26.3, the Odin 3) crashes building such a
// pipeline with them.
const Shader& Renderer::Impl::DepthOnlyPs(const Shader& ps) {
  auto it = depth_only_ps_.find(&ps);
  if (it != depth_only_ps_.end()) return it->second.module ? it->second : ps;
  Shader& s = depth_only_ps_[&ps];
  s.info = ps.info;
  s.recipe = ps.recipe;
  s.recipe_hash = ps.recipe_hash;
  std::string g;
  {
    std::istringstream in(ps.glsl);
    std::string line;
    while (std::getline(in, line)) {
      if (line.find("gl_SampleMask[0] =") != std::string::npos) continue;
      if (line.find("out vec4 xe_color") != std::string::npos) continue;
      if (line.find("xe_color") != std::string::npos && line.find("= xe_oC[") != std::string::npos) continue;
      g += line;
      g += '\n';
    }
  }
  std::vector<uint32_t> spirv;
  std::string error;
  char stem[48];
  std::snprintf(stem, sizeof(stem), "depthonly_%016llX", (unsigned long long)ps.info.ucode_hash);
  if (!options_.compile || !options_.compile(true, stem, g, &spirv, &error)) return ps;
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  if (vk::vkCreateShaderModule(gpu_.device, &ci, nullptr, &s.module) != VK_SUCCESS) {
    s.module = VK_NULL_HANDLE;
    return ps;
  }
  return s;
}

VkPipeline Renderer::Impl::PipelineFor(const Draw& d, const GpuState& st, const Shader& vs,
                                 const Shader& ps_in, const std::vector<Image*>& colors, Image* depth,
                                 uint32_t color_mask_bits, bool velocity, bool mask,
                                 bool reactive, bool caster) {
  // (Depth alone, one sample: the pixel shader without its color outputs.)
  const Shader& ps = colors.empty() && depth && depth->samples == 1 && !ps_in.glsl.empty() &&
                             !Diag("nodepthps")
                         ? DepthOnlyPs(ps_in)
                         : ps_in;
  const uint32_t mode = st.reg(0x2205);        // PA_SU_SC_MODE_CNTL
  const uint32_t depth_ctl = st.reg(0x2200);   // RB_DEPTHCONTROL
  uint32_t blend[4] = {st.reg(0x2201), st.reg(0x2209), st.reg(0x220A), st.reg(0x220B)};
  VkPrimitiveTopology topology;
  switch (d.prim) {
    case 1: topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
    case 2: topology = VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
    case 3: topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP; break;
    case 5: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN; break;
    case 6: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP; break;
    default: topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
  }
  // Diagnostics ("list"): non-indexed strips are drawn as lists (DoDraw).
  if (Diag("list") && d.prim == 6 && d.kind != Draw::Kind::kIndexed &&
      d.kind != Draw::Kind::kIndexedUP) {
    topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  }
  // Binary key (built per draw: no stream formatting).
  char key_buf[160];
  size_t key_len = 0;
  auto put = [&](uint32_t v) {
    if (key_len + 4 <= sizeof(key_buf)) std::memcpy(key_buf + key_len, &v, 4);
    key_len += 4;
  };
  const Shader* shader_ptrs[2] = {&vs, &ps};
  std::memcpy(key_buf, shader_ptrs, sizeof(shader_ptrs));
  key_len = sizeof(shader_ptrs);
  put(uint32_t(topology));
  put(mode & 0x201807);
  put(depth_ctl);
  put(color_mask_bits);
  put(uint32_t(colors.size()));
  for (size_t i = 0; i < colors.size(); ++i) {
    put(blend[i]);
    put(uint32_t(colors[i]->format));
  }
  put(depth ? uint32_t(depth->format) : 0u);
  // Multisampling: the attachments' sample count; PA_SC_AA_MASK selects the
  // guest samples written (bit i = sample i), each covering `scale` host
  // samples; alpha to coverage in hardware.
  const Image* first_att = colors.empty() ? depth : colors[0];
  const uint32_t host_samples = first_att ? first_att->samples : 1;
  const uint32_t scale = first_att ? first_att->sample_scale : 1;
  uint32_t sample_mask = 0xFFFFFFFFu;
  bool alpha_to_coverage = false;
  if (host_samples > 1) {
    const uint32_t guest_samples = host_samples / std::max(scale, 1u);
    const uint32_t aa_mask = st.reg(0x2312);
    sample_mask = 0;
    for (uint32_t i = 0; i < guest_samples; ++i) {
      if (aa_mask & (1u << i)) sample_mask |= ((1u << scale) - 1) << (i * scale);
    }
    // Done by the shader's sample mask instead when smoothing (DoDraw).
    alpha_to_coverage =
        !velocity && !mask && !caster && !smooth_alpha_ && (st.reg(0x2202) & 0x10) != 0;
    put(0x4D530000u | host_samples);
    put(sample_mask);
    put(alpha_to_coverage ? 1u : 0u);
  }
  const bool diag_range = DiagRange() && !velocity;
  if (diag_range) put(0x44494147u);
  if (velocity) put(0x56454Cu);
  if (mask) put(0x4D41534Bu);
  if (reactive) put(0x52454143u);
  if (caster) put(0x43535452u);
  const std::string_view key(key_buf, std::min(key_len, sizeof(key_buf)));
  last_pipeline_pending_ = false;
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  if (pipelines_pending_.count(std::string(key))) {
    last_pipeline_pending_ = true;
    return VK_NULL_HANDLE;
  }
  // Which it is, the same from run to run (for the crash report, and to
  // leave out one that crashed the driver before).
  char pipe_id[112];
  // (" tv<N>": the shader translation's version, bumped when a change may
  // fix a driver crash, so what crashed an older one is tried again.)
  std::snprintf(pipe_id, sizeof(pipe_id), "vs %016llX ps %016llX t%u m%08X s%u%s%s%s%s%s tv7",
                (unsigned long long)vs.info.ucode_hash, (unsigned long long)ps.info.ucode_hash,
                unsigned(topology), mode & 0x201807, host_samples, velocity ? " vel" : "", mask ? " mask" : "",
                reactive ? " reactive" : "", caster ? " caster" : "",
                vs.safe_level ? (" safe" + std::to_string(std::min(vs.safe_level, kSafeLevels))).c_str() : "");
  if (!crashed_ids_.empty() && crashed_ids_.count(pipe_id)) {
    if (crashed_told_.insert(pipe_id).second) {
      pending_notices_.push_back(std::string("left out a pipeline that crashed the driver before: ") + pipe_id);
    }
    pipelines_.emplace(std::string(key), VK_NULL_HANDLE);
    return VK_NULL_HANDLE;
  }

  // Everything the create info points at lives in `build`, so it can be
  // compiled on another thread.
  auto build = std::make_unique<PipelineBuild>();
  build->note = std::string("pipeline ") + pipe_id;
  if (precompiling_ && !precompile_line_.empty()) build->note += " | " + precompile_line_;
  auto& stages = build->stages;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs.module;
  stages[0].pName = "main";
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = ps.module;
  stages[1].pName = "main";
  auto& vi = build->vi;
  auto& ia = build->ia;
  ia.topology = topology;
  ia.primitiveRestartEnable = (topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP ||
                               topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN ||
                               topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP) &&
                              (mode & (1u << 21));
  auto& vp = build->vp;
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  auto& rs = build->rs;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = ((mode & 1) ? VK_CULL_MODE_FRONT_BIT : 0) | ((mode & 2) ? VK_CULL_MODE_BACK_BIT : 0);
  if (caster) rs.cullMode = VK_CULL_MODE_NONE;  // seen from the sun: either side
  rs.frontFace = (mode & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Polygon offset (PA_SU_SC_MODE_CNTL poly_offset_front/back_enable); the
  // values are dynamic (DoDraw).
  rs.depthBiasEnable = (mode & (3u << 11)) != 0 && !caster;
  if (Diag("nobias")) rs.depthBiasEnable = VK_FALSE;
  if (Diag("clamp")) rs.depthClampEnable = VK_TRUE;
  rs.lineWidth = 1.0f;
  auto& ms = build->ms;
  ms.rasterizationSamples = VkSampleCountFlagBits(host_samples);
  build->sample_mask = sample_mask;
  if (host_samples > 1) {
    ms.pSampleMask = &build->sample_mask;
    ms.alphaToCoverageEnable = alpha_to_coverage;
  }
  auto& ds = build->ds;
  if (depth) {
    ds.depthTestEnable = (depth_ctl >> 1) & 1;
    ds.depthWriteEnable = ds.depthTestEnable && ((depth_ctl >> 2) & 1);
    ds.depthCompareOp = VkCompareOp((depth_ctl >> 4) & 7);
    ds.stencilTestEnable = depth_ctl & 1;
    if (diag_range && (Diag("nodepth") || Diag("show"))) {
      ds.depthTestEnable = VK_FALSE;
      ds.depthWriteEnable = VK_FALSE;
    }
    if (diag_range && Diag("show")) ds.stencilTestEnable = VK_FALSE;
    if (mask) {
      // Blur exemption mask: where the draw is the visible surface (its own
      // depth, still in the finished scene), nothing else written.
      ds.depthTestEnable = VK_TRUE;
      ds.depthWriteEnable = VK_FALSE;
      ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
      ds.stencilTestEnable = VK_FALSE;
    }
    if (caster) {
      // The tree map: depth along the sun, the nearest kept.
      ds.depthTestEnable = VK_TRUE;
      ds.depthWriteEnable = VK_TRUE;
      ds.depthCompareOp = VK_COMPARE_OP_LESS;
      ds.stencilTestEnable = VK_FALSE;
    }
    if (velocity) {
      // Motion vectors where this draw is the visible surface.
      ds.depthTestEnable = VK_TRUE;
      ds.depthWriteEnable = VK_FALSE;
      ds.depthCompareOp = VK_COMPARE_OP_EQUAL;
      ds.stencilTestEnable = VK_FALSE;
    }
    ds.front.compareOp = VkCompareOp((depth_ctl >> 8) & 7);
    ds.front.failOp = VkStencilOp((depth_ctl >> 11) & 7);
    ds.front.passOp = VkStencilOp((depth_ctl >> 14) & 7);
    ds.front.depthFailOp = VkStencilOp((depth_ctl >> 17) & 7);
    if ((depth_ctl >> 7) & 1) {
      ds.back.compareOp = VkCompareOp((depth_ctl >> 20) & 7);
      ds.back.failOp = VkStencilOp((depth_ctl >> 23) & 7);
      ds.back.passOp = VkStencilOp((depth_ctl >> 26) & 7);
      ds.back.depthFailOp = VkStencilOp((depth_ctl >> 29) & 7);
    } else {
      ds.back = ds.front;
    }
  }
  auto& cb = build->cb;
  cb.resize(colors.size());
  for (size_t i = 0; i < colors.size(); ++i) {
    const uint32_t b = blend[i];
    VkPipelineColorBlendAttachmentState& a = cb[i];
    a.srcColorBlendFactor = BlendFactor(b & 31);
    a.colorBlendOp = BlendOp((b >> 5) & 7);
    a.dstColorBlendFactor = BlendFactor((b >> 8) & 31);
    a.srcAlphaBlendFactor = BlendFactor((b >> 16) & 31);
    a.alphaBlendOp = BlendOp((b >> 21) & 7);
    a.dstAlphaBlendFactor = BlendFactor((b >> 24) & 31);
    a.blendEnable = !((b & 0x1FFF1FFF) == 0x00010001);
    a.colorWriteMask = (color_mask_bits >> (4 * i)) & 15;
    if ((diag_range && Diag("show")) || velocity) {
      a.colorWriteMask = 15;
      a.blendEnable = VK_FALSE;
    }
    if (mask) {
      // The velocity image's w = 1 (builtin_exempt).
      a.blendEnable = VK_FALSE;
      a.colorWriteMask = VK_COLOR_COMPONENT_A_BIT;
    }
    if (reactive) {
      // The upscaler's reactive mask (R8): how much the draw adds to the
      // pixel, roughly: its red output scaled as the game scales its color
      // (alpha for alpha blending, one for additive), summed (saturating).
      a.blendEnable = VK_TRUE;
      a.srcColorBlendFactor = BlendFactor(b & 31);
      a.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      a.colorBlendOp = VK_BLEND_OP_ADD;
      a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      a.alphaBlendOp = VK_BLEND_OP_ADD;
      a.colorWriteMask = VK_COLOR_COMPONENT_R_BIT;
    }
  }
  auto& cbs = build->cbs;
  cbs.attachmentCount = uint32_t(cb.size());
  cbs.pAttachments = cb.data();
  auto& dy = build->dy;
  dy.dynamicStateCount = 7;
  dy.pDynamicStates = build->dyn;
  auto& color_formats = build->color_formats;
  for (auto* c : colors) color_formats.push_back(c->format);
  auto& pr = build->pr;
  pr.colorAttachmentCount = uint32_t(color_formats.size());
  pr.pColorAttachmentFormats = color_formats.data();
  pr.depthAttachmentFormat = depth ? depth->format : VK_FORMAT_UNDEFINED;
  pr.stencilAttachmentFormat = depth ? depth->format : VK_FORMAT_UNDEFINED;
  auto& pci = build->pci;
  pci.pNext = &pr;
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vp;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = &ds;
  pci.pColorBlendState = &cbs;
  pci.pDynamicState = &dy;
  pci.layout = layout_;
  build->key = std::string(key);
  if (!precompiling_) ListPipeline(d, st, vs, ps, colors, depth, color_mask_bits, velocity, mask, reactive, caster);
  // Scene geometry (and the motion blur's extra passes) can wait: once this
  // frame has spent its budget building pipelines, the rest are built in the
  // background and the draw is left out until its pipeline is ready (a
  // frame or two), instead of the frame stalling on the driver's compiler.
  // (So can the copies drawn again into the sun maps, the casters' and the
  // tracks' maps: a frame without one is a shadow late by a frame.)
  const bool deferrable = options_.async_pipelines && (tiling_.width != 0 || velocity || mask || caster);
  if (precompiling_) {
    // (Precompiling: always in the background.)
    QueuePipeline(std::move(build));
    return VK_NULL_HANDLE;
  }
  // Just after the renderer starts (most pipelines then come from the disk
  // cache, quickly) a longer stall beats a scene missing for a frame.
  const double budget = frame_number_ < 120 ? 100.0 : kPipelineSyncBudgetMs;
  if (deferrable && pipeline_sync_ms_ >= budget) {
    QueuePipeline(std::move(build));
    last_pipeline_pending_ = true;
    return VK_NULL_HANDLE;
  }
  VkPipeline p = VK_NULL_HANDLE;
  const auto t_pipe = std::chrono::steady_clock::now();
  const amped3::crash_note::Scope note(build->note.c_str());
  // Past the start: one the driver would have to compile (not in its cache;
  // a few hundred ms on some) goes to the background at once, even the
  // frame's first, rather than only once the frame's budget is spent.
  if (deferrable && gpu_.cache_control && frame_number_ >= 120 && !Diag("nocachetry")) {
    pci.flags |= VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT;
    VkResult r;
    {
      const auto lock = PipeLock();
      r = vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &pci, nullptr, &p);
    }
    pci.flags &= ~VkPipelineCreateFlags(VK_PIPELINE_CREATE_FAIL_ON_PIPELINE_COMPILE_REQUIRED_BIT);
    if (r != VK_SUCCESS || p == VK_NULL_HANDLE) {
      if (p != VK_NULL_HANDLE) vk::vkDestroyPipeline(gpu_.device, p, nullptr);
      p = VK_NULL_HANDLE;
      pipeline_sync_ms_ += MsSince(t_pipe);
      QueuePipeline(std::move(build));
      last_pipeline_pending_ = true;
      return VK_NULL_HANDLE;
    }
  } else {
    const auto lock = PipeLock();
    if (vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &pci, nullptr, &p) != VK_SUCCESS) {
      p = VK_NULL_HANDLE;
    }
  }
  const double took = MsSince(t_pipe);
  // (Slow ones that couldn't wait, told in the log: what they were for.)
  if (took > 30.0 && slow_pipes_told_ < 30) {
    ++slow_pipes_told_;
    char line[256];
    std::snprintf(line, sizeof(line),
                  "pipeline built while drawing: %.0f ms (%s; vs %016llx ps %016llx, %zu colors, depth %d%s%s%s)", took,
                  deferrable ? "could wait" : tiling_.width ? "scene" : "outside the scene",
                  (unsigned long long)vs.info.ucode_hash, (unsigned long long)ps.info.ucode_hash, colors.size(),
                  depth ? 1 : 0, caster ? ", a caster copy" : "", light_seen_ ? ", after the lighting" : "",
                  composite_seen_ ? ", after the composite" : "");
    pending_notices_.push_back(line);
  }
  ++stats_.new_pipelines;
  ++pipelines_unsaved_;
  stats_.pipeline_ms += took;
  pipeline_sync_ms_ += took;
  pipelines_.emplace(std::string(key), p);
  return p;
}

// --- Precompiling -------------------------------------------------------------

// The list for these settings (what changes the pipelines' make-up: the
// targets' sample counts and the cut-outs' coverage).
std::string Renderer::Impl::ListPath() const {
  if (precompile_dir_.empty()) return {};
  return precompile_dir_ + "/pipelines_" + (options_.msaa ? "msaa" : "nomsaa") +
         (options_.smooth_alpha ? "_smooth" : "") + ".txt";
}

// What crashed the driver last time (crash_note.h), and where a crash notes
// it this time.
void Renderer::Impl::ReadCrashed() {
  if (precompile_dir_.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(precompile_dir_, ec);
  const std::string path = precompile_dir_ + "/crashed_pipelines.txt";
  amped3::crash_note::SetPath(path.c_str());
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    // "pipeline <id>[ | <the list's line>]"
    if (line.rfind("pipeline ", 0) != 0) continue;
    {
      // (Only this translation's crashes: see pipe_id's " tv".)
      const size_t bar0 = line.find(" | ");
      const std::string head = line.substr(0, bar0);
      if (head.size() < 4 || head.compare(head.size() - 4, 4, " tv7") != 0) {
        continue;  // (crashes before pipelines were built one at a time: tried again)
      }
    }
    const size_t bar = line.find(" | ");
    const std::string id = line.substr(9, bar == std::string::npos ? std::string::npos : bar - 9);
    crashed_ids_.insert(id);
    // (A vertex shader's crash: its plainer translation next, one step per
    // crash; past the last, the pipeline's left out.)
    unsigned long long vsh = 0;
    int lvl = 0;
    // (The plainer steps are off: on the Odin 3 none of them helped, so a
    // crash leaves the pipeline out at once. kPlainerSteps re-enables them.)
    if (kPlainerSteps && std::sscanf(id.c_str(), "vs %llX", &vsh) == 1) {
      const size_t at = id.find(" safe");
      if (at != std::string::npos) lvl = std::atoi(id.c_str() + at + 5);
      int& l = safe_level_[uint64_t(vsh)];
      l = std::max(l, std::min(lvl + 1, kSafeLevels));
    }
    // (The list's line only once the plainest has crashed too.)
    if (bar != std::string::npos && (!kPlainerSteps || lvl >= kSafeLevels)) {
      crashed_lines_.insert(line.substr(bar + 3));
    }
  }
  if (!crashed_ids_.empty()) {
    pending_notices_.push_back(std::to_string(crashed_ids_.size()) +
                               " pipeline(s) crashed the graphics driver before: left out (" + path +
                               "; delete it to try them again)");
  }
}

void Renderer::Impl::ReadList() {
  if (listed_read_) return;
  listed_read_ = true;
  const std::string path = ListPath();
  if (path.empty()) return;
  std::ifstream in(path);
  std::string line;
  while (std::getline(in, line)) {
    if (line.rfind("p1 ", 0) == 0 && !crashed_lines_.count(line)) listed_.insert(line);
  }
}

// The microcode kept (its hash: the recipe's), once per session per shader.
uint64_t Renderer::Impl::KeepMicrocode(const std::vector<uint8_t>& bytes) {
  const uint64_t h = HashBytes(bytes.data(), bytes.size());
  if (precompile_dir_.empty() || !ucode_kept_.insert(h).second) return h;
  char name[32];
  std::snprintf(name, sizeof(name), "/ucode/%016llX.bin", (unsigned long long)h);
  const std::string path = precompile_dir_ + name;
  std::error_code ec;
  if (!std::filesystem::exists(path, ec)) {
    std::filesystem::create_directories(precompile_dir_ + "/ucode", ec);
    std::ofstream(path, std::ios::binary).write(reinterpret_cast<const char*>(bytes.data()),
                                                std::streamsize(bytes.size()));
  }
  return h;
}

// A pipeline being built: its recipe added to the list (the shaders, the
// state it's made from, the targets' formats and samples).
void Renderer::Impl::ListPipeline(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                                  const std::vector<Image*>& colors, Image* depth, uint32_t mask_bits,
                                  bool velocity, bool mask, bool reactive, bool caster) {
  if (precompile_dir_.empty() || !vs.recipe || !ps.recipe || DiagRange() || Diag("nolist")) return;
  ReadList();
  std::string r = fmt_line("p1 %u %016llX %u %016llX %u %u", vs.recipe, (unsigned long long)vs.recipe_hash,
                           ps.recipe, (unsigned long long)ps.recipe_hash, d.prim, unsigned(d.kind));
  for (uint32_t reg : {0x2200u, 0x2201u, 0x2202u, 0x2205u, 0x2209u, 0x220Au, 0x220Bu, 0x2312u}) {
    r += fmt_line(" %08X", st.reg(reg));
  }
  r += fmt_line(" %u %u %zu", mask_bits,
                (velocity ? 1u : 0u) | (mask ? 2u : 0u) | (reactive ? 4u : 0u) | (caster ? 8u : 0u),
                colors.size());
  for (const Image* c : colors) r += fmt_line(" %u %u %u", unsigned(c->format), c->samples, c->sample_scale);
  if (depth) {
    r += fmt_line(" %u %u %u", unsigned(depth->format), depth->samples, depth->sample_scale);
  } else {
    r += " 0 0 0";
  }
  if (!listed_.insert(r).second) return;
  precompiled_.insert(r);  // (built this session already)
  std::error_code ec;
  std::filesystem::create_directories(precompile_dir_, ec);
  std::ofstream(ListPath(), std::ios::app) << r << "\n";
}

// A shader for a recipe, from the microcode kept.
Shader* Renderer::Impl::PrecompileShader(uint8_t recipe, uint64_t hash) {
  if (recipe == 4) return &vel_ps_;
  if (recipe == 5) return &exempt_ps_;
  if (recipe < 1 || recipe > 3) return nullptr;
  auto key = std::make_pair(recipe, hash);
  auto it = precompile_shaders_.find(key);
  if (it != precompile_shaders_.end()) return it->second.module ? &it->second : nullptr;
  Shader& s = precompile_shaders_[key];
  char name[32];
  std::snprintf(name, sizeof(name), "/ucode/%016llX.bin", (unsigned long long)hash);
  std::ifstream in(precompile_dir_ + name, std::ios::binary);
  std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < 12 || bytes.size() % 4) return nullptr;
  std::vector<uint32_t> ucode(bytes.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) {
    ucode[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) |
               (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  // (Translated as GetShader / VelocityShader do: the same SPIR-V, so the
  // driver's cache entry is the one the game's draws will ask for.)
  amped3::native::TranslateOptions topt;
  topt.velocity = recipe == 3;
  topt.swap_variant = Diag("swap0") ? 0 : Diag("swap2") ? 2 : Diag("swap3") ? 3 : 1;
  topt.bit_zero_mul = bit_zero_mul_;
  const ShaderStage stage = recipe == 2 ? ShaderStage::kPixel : ShaderStage::kVertex;
  if (stage == ShaderStage::kVertex) topt.safe_level = SafeLevel(ucode.data(), ucode.size());
  s.safe_level = topt.safe_level;
  if (s.safe_level) {
    char m[160];
    std::snprintf(m, sizeof(m), "vertex shader %016llX made plainer (level %d): it crashed the driver before",
                  (unsigned long long)amped3::native::HashMicrocode(ucode.data(), ucode.size()), s.safe_level);
    pending_notices_.push_back(m);
  }
  auto r = amped3::native::TranslateShader(stage, ucode.data(), ucode.size(), topt);
  if (!r.ok) return nullptr;
  s.info = r.info;
  if (stage == ShaderStage::kPixel) s.glsl = r.glsl;
  std::vector<uint32_t> spirv;
  std::string error;
  char stem[40];
  std::snprintf(stem, sizeof(stem), "precompile_%016llX%s", (unsigned long long)hash, recipe == 3 ? "_velocity" : "");
  if (!options_.compile || !options_.compile(stage == ShaderStage::kPixel, stem, r.glsl, &spirv, &error)) {
    return nullptr;
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  if (vk::vkCreateShaderModule(gpu_.device, &ci, nullptr, &s.module) != VK_SUCCESS) s.module = VK_NULL_HANDLE;
  return s.module ? &s : nullptr;
}

// The precompile thread: the list's lines made ready (parsed, their shaders
// translated and compiled: the slow part) for the worker to queue.
void Renderer::Impl::PrecompileMain() {
  for (;;) {
    std::string line;
    {
      std::unique_lock lock(pc_mutex_);
      pc_cv_.wait(lock, [this] { return pc_stop_ || !precompile_todo_.empty(); });
      if (pc_stop_) return;
      line = std::move(precompile_todo_.front());
      precompile_todo_.pop_front();
      ++pc_busy_;
    }
    auto item = std::make_unique<PrecompileItem>();
    item->line = line;
    bool ok = false;
    {
      std::istringstream in(line);
      std::string tag, vh, ph;
      unsigned vk_ = 0, pk_ = 0, prim = 0, kind = 0;
      size_t ncolors = 0;
      uint32_t regs[8] = {};
      in >> tag >> vk_ >> vh >> pk_ >> ph >> prim >> kind;
      for (uint32_t& v : regs) in >> std::hex >> v;
      in >> std::dec >> item->mask_bits >> item->flags >> ncolors;
      ok = bool(in) && ncolors <= 4;
      if (ok) {
        item->images.resize(ncolors + 1);
        for (size_t i = 0; i <= ncolors; ++i) {
          unsigned f = 0, samples = 0, scale = 0;
          in >> f >> samples >> scale;
          Image& im = item->images[i];
          im.format = VkFormat(f);
          im.samples = std::max(samples, 1u);
          im.sample_scale = scale;
          im.depth = i == ncolors;
        }
        ok = bool(in);
        item->ncolors = ncolors;
        item->d.prim = prim;
        item->d.kind = Draw::Kind(kind & 3);
        const uint32_t which[8] = {0x2200, 0x2201, 0x2202, 0x2205, 0x2209, 0x220A, 0x220B, 0x2312};
        for (int i = 0; i < 8; ++i) item->st.regs.set(which[i] - 0x2000, regs[i]);
      }
      if (ok) {
        item->vs = PrecompileShader(uint8_t(vk_), std::strtoull(vh.c_str(), nullptr, 16));
        item->ps = PrecompileShader(uint8_t(pk_), std::strtoull(ph.c_str(), nullptr, 16));
        ok = item->vs && item->ps;
      }
    }
    std::lock_guard lock(pc_mutex_);
    --pc_busy_;
    if (ok) {
      pc_ready_.push_back(std::move(item));
    } else {
      ++precompile_failed_;
      if (Diag("precompile")) std::fprintf(stderr, "precompile: couldn't make %s\n", line.c_str());
    }
  }
}

// The worker: what's ready queued for the pipeline builders.
void Renderer::Impl::PrecompileStep(double budget_ms) {
  const auto t0 = std::chrono::steady_clock::now();
  for (;;) {
    std::unique_ptr<PrecompileItem> item;
    {
      std::lock_guard lock(pc_mutex_);
      if (pc_ready_.empty()) return;
      item = std::move(pc_ready_.front());
      pc_ready_.pop_front();
    }
    std::vector<Image*> colors;
    for (size_t i = 0; i < item->ncolors; ++i) colors.push_back(&item->images[i]);
    Image* depth = item->images[item->ncolors].format != VK_FORMAT_UNDEFINED ? &item->images[item->ncolors] : nullptr;
    precompiling_ = true;
    precompile_line_ = item->line;
    last_queued_key_.clear();
    const VkPipeline p = PipelineFor(item->d, item->st, *item->vs, *item->ps, colors, depth, item->mask_bits,
                                     item->flags & 1, item->flags & 2, item->flags & 4, item->flags & 8);
    precompiling_ = false;
    precompile_line_.clear();
    if (Diag("precompile")) std::fprintf(stderr, "precompile: queued %d\n", !p && !last_queued_key_.empty() ? 1 : 0);
    if (!p && !last_queued_key_.empty()) {
      precompile_keys_.push_back(last_queued_key_);
    } else {
      ++precompile_handled_;  // (built already: nothing to wait for)
    }
    if (MsSince(t0) >= budget_ms) return;
  }
}

void Renderer::Impl::StopPrecompile() {
  {
    std::lock_guard lock(pc_mutex_);
    pc_stop_ = true;
    precompile_todo_.clear();
  }
  pc_cv_.notify_all();
  if (pc_thread_.joinable()) pc_thread_.join();
  pc_ready_.clear();
}

void Renderer::Impl::QueuePipeline(std::unique_ptr<PipelineBuild> build) {
  last_queued_key_ = build->key;
  pipelines_pending_.insert(build->key);
  ++stats_.deferred_pipelines;
  std::lock_guard lock(pq_mutex_);
  if (pq_threads_.empty()) {
    // Two builders: drivers compile pipelines on one thread each.
    for (int i = 0; i < 2; ++i) {
      SpawnBuilder();
    }
  }
  // (Precompiling: more, the cores the game isn't using.)
  if (precompiling_ && pq_threads_.size() < PrecompileThreads()) {
    while (pq_threads_.size() < PrecompileThreads()) SpawnBuilder();
  }
  pq_.push_back(std::move(build));
  pq_cv_.notify_one();
}

void Renderer::Impl::SpawnBuilder() {
  {
    {
      pq_threads_.emplace_back([this] {
        for (;;) {
          std::unique_ptr<PipelineBuild> b;
          {
            std::unique_lock lock(pq_mutex_);
            pq_cv_.wait(lock, [this] { return pq_stop_ || !pq_.empty(); });
            if (pq_stop_) return;
            b = std::move(pq_.front());
            pq_.pop_front();
          }
          VkPipeline p = VK_NULL_HANDLE;
          {
            const amped3::crash_note::Scope note(b->note.c_str());
            const auto lock = PipeLock();
            if (vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &b->pci, nullptr,
                                              &p) != VK_SUCCESS) {
              p = VK_NULL_HANDLE;
            }
          }
          std::lock_guard lock(pq_mutex_);
          pq_done_.emplace_back(std::move(b->key), p);
        }
      });
    }
  }
}

// Pipelines finished in the background since the last frame.
void Renderer::Impl::CollectPipelines() {
  std::vector<std::pair<std::string, VkPipeline>> done;
  {
    std::lock_guard lock(pq_mutex_);
    done.swap(pq_done_);
  }
  for (auto& [key, p] : done) {
    pipelines_pending_.erase(key);
    pipelines_.emplace(std::move(key), p);
    ++pipelines_unsaved_;
  }
}

void Renderer::Impl::StopPipelineBuilders() {
  {
    std::lock_guard lock(pq_mutex_);
    pq_stop_ = true;
    pq_.clear();
  }
  pq_cv_.notify_all();
  for (auto& t : pq_threads_) t.join();
  pq_threads_.clear();
  CollectPipelines();
}

void Renderer::Impl::PrefetchTextures() {
  // Decoded last frame but never asked for (bound but not sampled, or a
  // surface the game rendered later that frame): not prefetched again.
  if (prefetch_skip_.size() > 20000) prefetch_skip_.clear();
  for (auto& [key, levels] : prefetched_) prefetch_skip_.insert(prefetched_hash_[key]);
  prefetched_.clear();
  prefetched_hash_.clear();
  if (Diag("noprefetch")) return;
  struct Job {
    std::string data;
    amped3::native::TextureFetch tf;
    uint64_t hash;
    std::vector<amped3::native::DecodedTexture> levels;
  };
  std::vector<Job> jobs;
  std::unordered_set<uint64_t> queued;
  // Runs every frame over every draw's textures: the common case (a texture
  // already cached, often the same as a recent draw's) is kept cheap.
  uint64_t recent[8] = {};
  uint32_t recent_at = 0;
  for (const Draw& d : frame_->draws) {
    for (const auto& t : d.textures) {
      if (t.data.empty()) continue;
      amped3::native::TextureFetch tf;
      std::memcpy(tf.words, FetchWords(d, t.slot, *d.state), sizeof(tf.words));
      const uint64_t hash = TextureHash(t.data, tf);
      bool seen = false;
      for (uint64_t r : recent) seen = seen || r == hash;
      if (seen) continue;
      recent[recent_at++ & 7] = hash;
      if (known_textures_.count(hash) || prefetch_skip_.count(hash) ||
          !queued.insert(hash).second) {
        continue;
      }
      // Surfaces the game rendered (resolves) aren't decoded from memory.
      const uint32_t base = tf.base_address() & 0x1FFFFFFF;
      if (resolved_.count(base) || resolved_packed_.count(base)) continue;
      if (!amped3::native::IsDecodableTextureFormat(tf.format()) || tf.dimension() > 1) continue;
      jobs.push_back(Job{t.data, tf, hash, {}});
    }
  }
  if (jobs.empty()) return;
  const auto t0 = std::chrono::steady_clock::now();
  const bool keep_blocks = KeepBlocks();
  std::atomic<size_t> next{0};
  auto work = [&] {
    for (size_t i; (i = next.fetch_add(1)) < jobs.size();) {
      jobs[i].levels = DecodeFromResources(*res_, jobs[i].data, jobs[i].tf, keep_blocks);
    }
  };
  // A few threads for a burst of new textures (a camera turn); one alone
  // isn't worth a thread.
  const size_t threads =
      std::min<size_t>({jobs.size() / 2, std::max(1u, std::thread::hardware_concurrency()) - 1,
                        6});
  std::vector<std::thread> pool;
  for (size_t i = 0; i < threads; ++i) pool.emplace_back(work);
  work();
  for (auto& th : pool) th.join();
  for (auto& j : jobs) {
    std::string key = TextureKey(j.data, j.tf);
    prefetched_hash_[key] = j.hash;
    prefetched_.emplace(std::move(key), std::move(j.levels));
  }
  stats_.texture_ms += MsSince(t0);
}

// Big-endian indices to little endian, 8 bytes at a time (`bytes` rounded
// down to whole indices).
static void SwapIndices(const uint8_t* src, uint8_t* dst, size_t bytes, uint32_t index_size) {
  size_t i = 0;
  if (index_size == 2) {
    for (; i + 8 <= bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, src + i, 8);
      v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
      std::memcpy(dst + i, &v, 8);
    }
    for (; i + 2 <= bytes; i += 2) {
      dst[i] = src[i + 1];
      dst[i + 1] = src[i];
    }
  } else {
    for (; i + 8 <= bytes; i += 8) {
      uint64_t v;
      std::memcpy(&v, src + i, 8);
      v = ((v & 0x00FF00FF00FF00FFull) << 8) | ((v >> 8) & 0x00FF00FF00FF00FFull);
      v = ((v & 0x0000FFFF0000FFFFull) << 16) | ((v >> 16) & 0x0000FFFF0000FFFFull);
      std::memcpy(dst + i, &v, 8);
    }
    for (; i + 4 <= bytes; i += 4) {
      dst[i] = src[i + 3];
      dst[i + 1] = src[i + 2];
      dst[i + 2] = src[i + 1];
      dst[i + 3] = src[i];
    }
  }
}

void Renderer::Impl::Prepass() {
  PrefetchTextures();
  // Render target heights from the viewports that draw into them.
  target_heights_.clear();
  const GpuState* last_state = nullptr;
  for (const Draw& d : frame_->draws) {
    // Draws in a row with unchanged state share it: nothing new to see.
    if (d.state.get() == last_state) continue;
    last_state = d.state.get();
    const GpuState& st = *d.state;
    const uint32_t vte = st.reg(0x2206);
    float ys = (vte & 4) ? st.regf(0x2111) : 1.0f, yo = (vte & 8) ? st.regf(0x2112) : 0.0f;
    uint32_t h = uint32_t(std::ceil(std::fabs(ys) + yo));
    if (h < 16 || h > 8192) h = 720;
    auto bump = [&](const TargetKey& k) {
      uint32_t& v = target_heights_[k];
      v = std::max(v, h);
    };
    bump(ColorKey(st.reg(0x2000), st.reg(0x2001)));
    bump(DepthKey(st.reg(0x2000), st.reg(0x2002)));
    bump(ColorKey(st.reg(0x2000), st.reg(0x2003)));
  }
  // Vertex memory: every vertex resource the frame uses, in the arena.
  // Regions retired 3+ frames ago are free again (no frame in flight reads
  // them: at most two are).
  for (size_t i = 0; i < arena_retired_.size();) {
    if (frame_number_ >= arena_retired_[i].frame + 3) {
      ArenaFree(arena_retired_[i].offset, arena_retired_[i].size);
      arena_retired_[i] = arena_retired_.back();
      arena_retired_.pop_back();
    } else {
      ++i;
    }
  }
  mem_offsets_.clear();
  mem_sizes_.clear();
  ix_offsets_.clear();
  ix_sizes_.clear();
  arena_copied_ = arena_kept_ = 0;
  arena_reset_ = false;
  uint32_t buffers = 0;
  // `index_size`: 0 vertex data (copied as is), 2 / 4 an index buffer
  // (byte-swapped to little endian; its own arena entry).
  // The last few names placed: draws in a row mostly reuse the same buffers
  // (a string compare instead of a hash lookup).
  struct Recent {
    const std::string* name;
    uint32_t index_size;
  };
  Recent recent[8] = {};
  uint32_t recent_next = 0;
  auto place = [&](const std::string& name, uint32_t index_size = 0) {
    for (const Recent& r : recent) {
      if (r.name && r.index_size == index_size && *r.name == name) return;
    }
    recent[recent_next++ & 7] = {&name, index_size};
    auto& offsets = index_size ? ix_offsets_ : mem_offsets_;
    if (offsets.count(name)) return;
    const auto& data = Bytes(name);
    if (data.empty()) {
      offsets[name] = UINT64_MAX;
      return;
    }
    ++buffers;
    uint64_t version = res_->Version(name);
    if (!version) {
      // Unknown (replayer): the content decides.
      uint64_t h = 0x9E3779B97F4A7C15ull ^ data.size();
      size_t i = 0;
      for (; i + 8 <= data.size(); i += 8) {
        uint64_t v;
        std::memcpy(&v, data.data() + i, 8);
        h = (h ^ v) * 0x100000001B3ull;
        h ^= h >> 29;
      }
      for (; i < data.size(); ++i) h = (h ^ data[i]) * 0x100000001B3ull;
      version = h | 1;
    }
    const std::string entry_name = index_size ? name + (index_size == 2 ? "#i16" : "#i32") : std::string();
    const std::string& key = index_size ? entry_name : name;
    ArenaEntry* e = &arena_entries_[key];
    if (e->size == data.size() && e->version == version) {
      arena_kept_ += e->size;
    } else {
      if (e->size) {
        arena_retired_.push_back({frame_number_, e->offset, (e->size + 255) & ~uint64_t(255)});
      }
      const uint64_t size = (data.size() + 255) & ~uint64_t(255);
      uint64_t offset;
      if (!ArenaAlloc(size, &offset)) {
        ArenaGrow(size);  // starts the arena over (entries cleared)
        e = &arena_entries_[key];
        if (!ArenaAlloc(size, &offset)) throw std::runtime_error("vertex arena full");
      }
      if (index_size) {
        SwapIndices(data.data(), arena_.mapped + offset, data.size(), index_size);
      } else {
        std::memcpy(arena_.mapped + offset, data.data(), data.size());
      }
      e->version = version;
      e->offset = offset;
      e->size = data.size();
      arena_copied_ += e->size;
    }
    e->last_frame = frame_number_;
    offsets[name] = e->offset;
    (index_size ? ix_sizes_ : mem_sizes_)[name] = e->size;
  };
  auto place_all = [&] {
    std::fill(std::begin(recent), std::end(recent), Recent{});
    for (const Draw& d : frame_->draws) {
      for (const auto& [vf, name] : d.vmem) place(name);
      if (!d.up_vertices.empty()) place(d.up_vertices);
      // Index buffers (not user-pointer indices, new each draw).
      if (index_cache_ && d.kind == Draw::Kind::kIndexed && !d.index_data.empty() &&
          d.index_data.compare(0, 4, "upi_") != 0) {
        place(d.index_data, d.index_32bit ? 4 : 2);
      }
    }
  };
  place_all();
  if (arena_reset_) {
    // Started over midway: what was placed before is in the old contents;
    // place everything again (one more pass, this frame's data only).
    mem_offsets_.clear();
    mem_sizes_.clear();
    ix_offsets_.clear();
    ix_sizes_.clear();
    arena_copied_ = arena_kept_ = 0;
    buffers = 0;
    place_all();
  }
  arena_frame_bytes_ = arena_copied_ + arena_kept_;
  stats_.arena_mb = double(arena_.size) / (1 << 20);
  // Forget resources not drawn for a while.
  if (frame_number_ % 60 == 0) {
    for (auto it = arena_entries_.begin(); it != arena_entries_.end();) {
      if (frame_number_ - it->second.last_frame > 300) {
        if (it->second.size) {
          arena_retired_.push_back(
              {frame_number_, it->second.offset, (it->second.size + 255) & ~uint64_t(255)});
        }
        it = arena_entries_.erase(it);
      } else {
        ++it;
      }
    }
  }
  log_ << "vertex data: " << buffers << " buffers, " << (arena_copied_ >> 10) << " KB copied, "
       << (arena_kept_ >> 10) << " KB kept on the GPU\n";
}

bool Renderer::Impl::ArenaAlloc(uint64_t size, uint64_t* offset) {
  for (auto it = arena_free_.begin(); it != arena_free_.end(); ++it) {
    if (it->second < size) continue;
    *offset = it->first;
    const uint64_t rest = it->second - size, at = it->first + size;
    arena_free_.erase(it);
    if (rest) arena_free_[at] = rest;
    return true;
  }
  return false;
}

void Renderer::Impl::ArenaFree(uint64_t offset, uint64_t size) {
  auto next = arena_free_.lower_bound(offset);
  if (next != arena_free_.end() && offset + size == next->first) {
    size += next->second;
    next = arena_free_.erase(next);
  }
  if (next != arena_free_.begin()) {
    auto prev = std::prev(next);
    if (prev->first + prev->second == offset) {
      prev->second += size;
      return;
    }
  }
  arena_free_[offset] = size;
}

// A bigger arena with the same contents at the same offsets (waits for the
// GPU: rare, while the game's set of vertex buffers grows).
void Renderer::Impl::ArenaGrow(uint64_t need) {
  // Waits for the frames in flight (the descriptor and the regions they read
  // change), then starts the arena over: Prepass places this frame's data
  // again. Nothing is copied from the old contents (a full copy of a few
  // hundred MB was a visible hitch). The same buffer is reused when this
  // frame's data fits comfortably (fragmentation, not growth); else a
  // bigger one.
  vk::vkDeviceWaitIdle(gpu_.device);
  ++stats_.arena_resets;
  const uint64_t frame_bytes = std::max(arena_frame_bytes_, arena_copied_ + arena_kept_) + need;
  const uint64_t old_size = arena_.buffer ? arena_.size : 0;
  if (!old_size || frame_bytes * 2 > old_size) {
    uint64_t size = std::max<uint64_t>(old_size * 2, 64ull << 20);
    while (size < frame_bytes * 3) size *= 2;
    size = std::min<uint64_t>(size, gpu_.props.limits.maxStorageBufferRange & ~uint64_t(255));
    if (size < frame_bytes) throw std::runtime_error("vertex data exceeds maxStorageBufferRange");
    gpu_.DestroyBuffer(arena_);
    arena_ = gpu_.CreateBuffer(size, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                                         VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  }
  arena_entries_.clear();
  arena_free_.clear();
  arena_retired_.clear();
  ArenaFree(0, arena_.size);
  arena_reset_ = true;
}

void Renderer::Impl::CreateOnce() {
  VkDevice dev = gpu_.device;
  // Set 0: 5 dynamic uniform buffers.
  VkDescriptorSetLayoutBinding b0[5];
  for (uint32_t i = 0; i < 5; ++i) {
    b0[i] = {i, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 1, VK_SHADER_STAGE_ALL_GRAPHICS, nullptr};
  }
  VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  lci.bindingCount = 5;
  lci.pBindings = b0;
  vk::Check(vk::vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layouts_[0]), "set layout 0");
  // Binding 1: diagnostic vertex stores ("posdump").
  VkDescriptorSetLayoutBinding b1[2] = {
      {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr}};
  lci.bindingCount = 2;
  lci.pBindings = b1;
  debug_ = gpu_.CreateBuffer(kDebugBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT);
  std::memset(debug_.mapped, 0xFF, kDebugBytes);
  vk::Check(vk::vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layouts_[1]), "set layout 1");
  for (int s = 2; s < 4; ++s) {
    VkDescriptorSetLayoutBinding bt[16];
    for (uint32_t i = 0; i < 16; ++i) {
      bt[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
               VkShaderStageFlags(s == 2 ? VK_SHADER_STAGE_VERTEX_BIT : VK_SHADER_STAGE_FRAGMENT_BIT),
               nullptr};
    }
    lci.bindingCount = 16;
    lci.pBindings = bt;
    vk::Check(vk::vkCreateDescriptorSetLayout(dev, &lci, nullptr, &set_layouts_[s]), "set layout tex");
  }
  VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pli.setLayoutCount = 4;
  pli.pSetLayouts = set_layouts_;
  vk::Check(vk::vkCreatePipelineLayout(dev, &pli, nullptr, &layout_), "pipeline layout");

  // Dummy texture (transparent black) for missing data.
  dummy_ = gpu_.CreateImage(4, 4, VK_FORMAT_R8G8B8A8_UNORM, false, "dummy");
  ToGeneral(cb_, dummy_);
  const float zero[4] = {0, 0, 0, 0};
  ClearImage(&dummy_, zero, 0, 0, true, false, false);

  VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
  si.magFilter = si.minFilter = VK_FILTER_NEAREST;
  si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  vk::Check(vk::vkCreateSampler(dev, &si, nullptr, &point_sampler_), "sampler");
  si.magFilter = si.minFilter = VK_FILTER_LINEAR;
  vk::Check(vk::vkCreateSampler(dev, &si, nullptr, &linear_sampler_), "sampler");

  // Depth -> color copy (depth resolves).
  VkDescriptorSetLayoutBinding cbnd[6] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  // binding 2: motion blur and a few more; 3: the world shadows' bake; 4, 5:
  // the night lights
  lci.bindingCount = 6;
  lci.pBindings = cbnd;
  vk::Check(vk::vkCreateDescriptorSetLayout(dev, &lci, nullptr, &copy_set_layout_), "copy layout");
  VkPushConstantRange pcr{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};  // 128: motion blur
  pli.setLayoutCount = 1;
  pli.pSetLayouts = &copy_set_layout_;
  pli.pushConstantRangeCount = 1;
  pli.pPushConstantRanges = &pcr;
  vk::Check(vk::vkCreatePipelineLayout(dev, &pli, nullptr, &copy_layout_), "copy pipeline layout");
  VkPushConstantRange clouds_pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
  pli.pPushConstantRanges = &clouds_pcr;
  vk::Check(vk::vkCreatePipelineLayout(dev, &pli, nullptr, &clouds_layout_), "clouds pipeline layout");
  copy_vs_ = CompileBuiltin("builtin_fullscreen", R"(#version 450
void main() {
  vec2 p = vec2(float((gl_VertexIndex << 1) & 2), float(gl_VertexIndex & 2));
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
)", false);
  copy_fs_[0] = CompileBuiltin("builtin_copy_depth", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform usampler2D src_stencil;
// source = dest + delta; mode 0: depth as R32F, 1: raw words as 8_8_8_8;
// float_depth: the surface is D24FS8 (20e4 depth) rather than D24S8.
layout(push_constant) uniform Offsets {
  ivec2 delta; uint mode; uint float_depth; uint first; uint count;
} pc;
layout(location = 0) out vec4 o;
uint F32To20e4(float f) {
  uint u = floatBitsToUint(clamp(f, 0.0, 1.99999988));
  if (u < 0x38800000u) {
    u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
  } else {
    u += 0xC8000000u;
  }
  return ((u + 7u + ((u >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + pc.delta;
  float d = texelFetch(src, p, 0).r;
  if (pc.mode == 0u) {
    o = vec4(d, 0.0, 0.0, 1.0);
    return;
  }
  uint s = texelFetch(src_stencil, p, 0).r & 0xFFu;
  uint z = pc.float_depth != 0u ? F32To20e4(d) : uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5);
  uint w = (z << 8) | s;
  // EDRAM depth word: X (byte 0) = stencil, W (byte 3) = depth high byte.
  // Resolved images are bound without the fetch swizzle, in the layout a
  // color resolve leaves them (red/blue swapped against memory, undone by
  // the usual ZYXW fetch swizzle), so store the bytes as Z Y X W.
  o = vec4(float((w >> 16) & 0xFFu), float((w >> 8) & 0xFFu), float(w & 0xFFu),
           float(w >> 24)) / 255.0;
}
)", true);
  copy_fs_[1] = CompileBuiltin("builtin_copy_depth_ms", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2DMS src;
layout(set = 0, binding = 1) uniform usampler2DMS src_stencil;
// source = dest + delta; mode 0: depth as R32F, 1: raw words as 8_8_8_8;
// float_depth: the surface is D24FS8 (20e4 depth) rather than D24S8.
layout(push_constant) uniform Offsets {
  ivec2 delta; uint mode; uint float_depth; uint first; uint count;
} pc;
layout(location = 0) out vec4 o;
uint F32To20e4(float f) {
  uint u = floatBitsToUint(clamp(f, 0.0, 1.99999988));
  if (u < 0x38800000u) {
    u = (0x800000u | (u & 0x7FFFFFu)) >> min(113u - (u >> 23), 24u);
  } else {
    u += 0xC8000000u;
  }
  return ((u + 7u + ((u >> 3) & 1u)) >> 3) & 0xFFFFFFu;
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + pc.delta;
  float d = texelFetch(src, p, int(pc.first)).r;
  if (pc.mode == 0u) {
    o = vec4(d, 0.0, 0.0, 1.0);
    return;
  }
  uint s = texelFetch(src_stencil, p, int(pc.first)).r & 0xFFu;
  uint z = pc.float_depth != 0u ? F32To20e4(d) : uint(clamp(d, 0.0, 1.0) * 16777215.0 + 0.5);
  uint w = (z << 8) | s;
  // EDRAM depth word: X (byte 0) = stencil, W (byte 3) = depth high byte.
  // Resolved images are bound without the fetch swizzle, in the layout a
  // color resolve leaves them (red/blue swapped against memory, undone by
  // the usual ZYXW fetch swizzle), so store the bytes as Z Y X W.
  o = vec4(float((w >> 16) & 0xFFu), float((w >> 8) & 0xFFu), float(w & 0xFFu),
           float(w >> 24)) / 255.0;
}
)", true);
  copy_fs_[2] = CompileBuiltin("builtin_copy_color_ms", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2DMS src;
layout(push_constant) uniform Offsets {
  ivec2 delta; uint mode; uint float_depth; uint first; uint count;
} pc;
layout(location = 0) out vec4 o;
// The average of samples first .. first + count - 1 (a guest resolve of one
// fragment, a pair, or all of them).
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy) + pc.delta;
  vec4 sum = vec4(0.0);
  for (uint i = 0u; i < pc.count; ++i) sum += texelFetch(src, p, int(pc.first + i));
  o = sum / float(max(pc.count, 1u));
}
)", true);
  // The presentable picture: the front buffer through the guest's gamma
  // ramp (8-bit index, like the emulated swap for 8-bit front buffers).
  copy_fs_[3] = CompileBuiltin("builtin_output", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D lut;
layout(push_constant) uniform Offsets {
  ivec2 delta; uint mode; uint float_depth; uint first; uint count;
} pc;
layout(location = 0) out vec4 o;
float Lut(float v, int c) {
  return texelFetch(lut, ivec2(int(clamp(v, 0.0, 1.0) * 255.0 + 0.5), 0), 0)[c];
}
vec3 Srgb(vec3 l) {  // back to the stored bytes of an sRGB image
  return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));
}
void main() {
  vec4 v = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
  if (pc.float_depth != 0u) v.rgb = Srgb(v.rgb);
  o = pc.mode != 0u ? vec4(Lut(v.r, 0), Lut(v.g, 1), Lut(v.b, 2), 1.0) : vec4(v.rgb, 1.0);
}
)", true);
  copy_fs_[4] = CompileBuiltin("builtin_output_ms", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2DMS src;
layout(set = 0, binding = 1) uniform sampler2D lut;
layout(push_constant) uniform Offsets {
  ivec2 delta; uint mode; uint float_depth; uint first; uint count;
} pc;
layout(location = 0) out vec4 o;
float Lut(float v, int c) {
  return texelFetch(lut, ivec2(int(clamp(v, 0.0, 1.0) * 255.0 + 0.5), 0), 0)[c];
}
vec3 Srgb(vec3 l) {
  return mix(l * 12.92, 1.055 * pow(l, vec3(1.0 / 2.4)) - 0.055, step(0.0031308, l));
}
void main() {
  vec4 v = vec4(0.0);
  for (uint i = 0u; i < pc.count; ++i) v += texelFetch(src, ivec2(gl_FragCoord.xy), int(i));
  v /= float(max(pc.count, 1u));
  if (pc.float_depth != 0u) v.rgb = Srgb(v.rgb);
  o = pc.mode != 0u ? vec4(Lut(v.r, 0), Lut(v.g, 1), Lut(v.b, 2), 1.0) : vec4(v.rgb, 1.0);
}
)", true);
  // Motion blur, step 1: each pixel's motion. The camera's: its position
  // last frame from its depth (prev = m * (ndc, 1 - depth, 1), m = last
  // view-projection * this inverse); pixels nearer than near.x (view distance)
  // don't get it (the rider moves with the camera), fading in up to near.y.
  // Where the velocity pass found an object (vel.z), its own motion instead:
  // all of it with camera blur on, else without the camera's part. The player
  // character (vel.w) has none. Out: motion in pixels, view distance, kind
  // (0 scenery, 0.5 object, 1 player).
  copy_fs_[6] = CompileBuiltin("builtin_motion", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D player_rows;  // 4 x 1
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D vel;
layout(push_constant) uniform Motion {
  mat4 m;         // last frame's view-projection * this frame's inverse
  vec4 w_row;     // this frame's inverse, row 3: 1 / dot = view distance
  vec4 params;    // scale (motion per frame -> blur), max length (uv), flags (1 camera,
                  // 2 objects, 4 exemption mask), player radius (world units)
  vec4 size;      // 1 / width, 1 / height
  vec4 player;    // x: the player's position is known (player_rows: world offset from it =
                  // rows * v / dot(w_row, v))
} pc;
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = (floor(gl_FragCoord.xy) + 0.5) * pc.size.xy;
  bool camera_on = mod(pc.params.z, 2.0) >= 1.0, objects_on = mod(pc.params.z, 4.0) >= 2.0;
  bool mask_on = pc.params.z >= 4.0;
  float d = texture(depth, uv).r;
  vec4 v = vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  float inv_dist = dot(pc.w_row, v);
  float dist = inv_dist > 0.0 ? 1.0 / inv_dist : 1e5;
  // The sky (at or past the far plane): the direction at infinity through
  // this pixel, which moves with the camera's turning only.
  vec4 vc = v;
  if (inv_dist <= 1e-7 && abs(pc.w_row.z) > 1e-20) {
    vc.z = -(pc.w_row.x * v.x + pc.w_row.y * v.y + pc.w_row.w) / pc.w_row.z;
  }
  vec4 prev = pc.m * vc;
  bool cam_ok = prev.w > 1e-6 && (inv_dist > 0.0 || abs(pc.w_row.z) > 1e-20);
  vec2 cam = cam_ok ? (v.xy - prev.xy / prev.w) * vec2(0.5, -0.5) : vec2(0.0);
  vec4 ov = objects_on || mask_on ? texture(vel, uv) : vec4(0.0);
  if (!objects_on) ov.z = 0.0;  // only the exemption mask (w) was drawn
  // Not blurred: the player (its own draws, and character pixels within the
  // radius of it: its board and anything else drawn on its own) and event
  // icons (mask).
  bool player = ov.w >= 0.75;
  if (!player && objects_on && ov.w >= 0.25 && pc.player.x > 0.0 && inv_dist > 0.0) {
    vec3 off = vec3(dot(texelFetch(player_rows, ivec2(0, 0), 0), v),
                    dot(texelFetch(player_rows, ivec2(1, 0), 0), v),
                    dot(texelFetch(player_rows, ivec2(2, 0), 0), v)) / inv_dist;
    player = length(off) < pc.params.w;
  }
  vec2 motion = vec2(0.0);
  float kind = 0.0;
  if (player) {
    kind = 1.0;
  } else if (ov.z >= 0.5) {
    motion = camera_on ? ov.xy : ov.xy - cam;
    kind = 0.5;
  } else if (camera_on && cam_ok) {
    // Not nearer than 15 (the rider and the snow around them), full from 40.
    motion = cam * smoothstep(15.0, 40.0, dist);
  }
  motion *= pc.params.x;
  float len = length(motion);
  if (len > pc.params.y) motion *= pc.params.y / len;
  o = vec4(motion / pc.size.xy, dist, kind);
}
)", true);
  // Four vec4s (push constants) into a 4 x 1 image, for a later pass.
  copy_fs_[9] = CompileBuiltin("builtin_rows", R"(#version 450
layout(push_constant) uniform Rows { vec4 r[4]; } pc;
layout(location = 0) out vec4 o;
void main() { o = pc.r[int(gl_FragCoord.x)]; }
)", true);
  // Upscaler motion vectors: each pixel's motion in render pixels from this
  // frame to the last (camera: reprojected from its depth, the jitter taken
  // out; objects: the velocity pass, where the jitter cancels).
  copy_fs_[10] = CompileBuiltin("builtin_motion_vectors", R"(#version 450
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D vel;
layout(push_constant) uniform Mv {
  mat4 m;         // last frame's view-projection * this frame's inverse
  vec4 w_row;     // this frame's inverse, row 3
  vec4 jitter;    // jitter (render pixels, +y down), objects on
  vec4 size;      // 1 / width, 1 / height, width, height
} pc;
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = (floor(gl_FragCoord.xy) + 0.5) * pc.size.xy;
  float d = texture(depth, uv).r;
  // Where this pixel's surface is without the jitter.
  vec2 uu = uv - pc.jitter.xy * pc.size.xy;
  vec4 v = vec4(uu.x * 2.0 - 1.0, 1.0 - uu.y * 2.0, 1.0 - d, 1.0);
  float inv_dist = dot(pc.w_row, v);
  if (inv_dist <= 1e-7 && abs(pc.w_row.z) > 1e-20) {  // the sky: its direction
    v.z = -(pc.w_row.x * v.x + pc.w_row.y * v.y + pc.w_row.w) / pc.w_row.z;
  }
  vec4 prev = pc.m * v;
  vec2 mv = vec2(0.0);
  if (prev.w > 1e-6) {
    vec2 prev_uv = vec2(prev.x / prev.w * 0.5 + 0.5, 0.5 - prev.y / prev.w * 0.5);
    mv = (prev_uv - uu) * pc.size.zw;
  }
  if (pc.jitter.z > 0.5) {
    vec4 ov = texture(vel, uv);
    if (ov.z >= 0.5) mv = -ov.xy * pc.size.zw;
  }
  o = vec4(mv, 0.0, 0.0);
}
)", true);
  // Speed blur: the scene streaked toward the point of travel, only near the
  // edges (a soft ring), by the rider's speed.
  copy_fs_[11] = CompileBuiltin("builtin_radial_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;  // linear, clamped
layout(push_constant) uniform Radial {
  vec4 a;  // center (uv), length (fraction of the way to the center), aspect (w / h)
  vec4 b;  // ring start, ring end (0 center .. 1 corners), 1 / width, 1 / height
} pc;
layout(location = 0) out vec4 o;
const int N = 16;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.b.zw;
  vec2 d = uv - pc.a.xy;
  // Distance from the center, 1 at the far corner, round on screen.
  vec2 far = max(pc.a.xy, 1.0 - pc.a.xy);
  float r = length(d * vec2(pc.a.w, 1.0)) / length(far * vec2(pc.a.w, 1.0));
  float m = smoothstep(pc.b.x, pc.b.y, r);
  float len = pc.a.z * m;
  if (len < 1e-4) {
    o = texture(scene, uv);
    return;
  }
  // Samples from here toward the center, spread by a per-pixel offset
  // (no banding); nearer ones weigh more, so edges keep their shape.
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int i = 0; i < N; ++i) {
    float t = (float(i) + noise) / float(N);
    float w = 1.0 - 0.5 * t;
    sum += texture(scene, uv - d * (len * t)).rgb * w;
    wsum += w;
  }
  o = vec4(sum / wsum, texture(scene, uv).a);
}
)", true);
  // Distance fog, 1: its color, the average of the sky (depth 0: nothing
  // drawn there) over the upper part of the screen, eased toward over a few
  // frames (kept while no sky is in view).
  copy_fs_[12] = CompileBuiltin("builtin_fog_color", R"(#version 450
// Two texels: x = 0 the ambient color (the sky away from the sun), x = 1 the
// sun's (the sky right around it, when it's on screen). Each eased toward
// over a few frames and kept while out of view.
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D prev;
layout(push_constant) uniform FogColor {
  vec4 a;  // ease (0..1), sun u, sun v, sun on screen (0/1)
  vec4 b;  // aspect (w / h), -, -, -
} pc;
layout(location = 0) out vec4 o;
float dist_to_sun(vec2 uv) {
  return length((uv - pc.a.yz) * vec2(pc.b.x, 1.0));
}
void main() {
  int which = int(gl_FragCoord.x);
  vec4 p = texelFetch(prev, ivec2(which, 0), 0);
  vec3 sum = vec3(0.0), far_sum = vec3(0.0);
  float n = 0.0, far_n = 0.0;
  if (which == 0) {
    for (int y = 0; y < 16; ++y) {
      for (int x = 0; x < 24; ++x) {
        vec2 uv = vec2((float(x) + 0.5) / 24.0, 0.04 + 0.8 * (float(y) + 0.5) / 16.0);
        float d = texture(depth, uv).r;
        vec3 c = texture(scene, uv).rgb;
        if (d <= 0.0) {
          // The sky, but not the sun's glare.
          if (pc.a.w > 0.5 && dist_to_sun(uv) < 0.35) continue;
          float w = 0.5 + uv.y;  // nearer the horizon counts more
          sum += c * w;
          n += w;
        } else {
          float w = exp(-d * 4000.0);  // far scenery (inverted depth)
          far_sum += c * w;
          far_n += w;
        }
      }
    }
    if (n > 0.0) {
      vec3 avg = sum / n;
      o = vec4(p.a > 0.75 ? mix(p.rgb, avg, pc.a.x) : avg, 1.0);
    } else if (p.a > 0.75) {
      o = p;
    } else if (far_n > 0.01) {
      // No sky yet: a light haze tinted by the far scenery.
      vec3 avg = mix(vec3(0.66, 0.70, 0.76), far_sum / far_n, 0.3);
      o = vec4(p.a > 0.25 ? mix(p.rgb, avg, pc.a.x) : avg, 0.5);
    } else {
      o = p;
    }
  } else {
    // The sun's color: the sky within a small circle around it (brighter
    // samples weigh more).
    if (pc.a.w > 0.5) {
      for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
          vec2 off = (vec2(x, y) - 3.5) / 3.5 * 0.08;
          vec2 uv = pc.a.yz + off * vec2(1.0 / pc.b.x, 1.0);
          if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
          if (texture(depth, uv).r > 0.0) continue;
          vec3 c = texture(scene, uv).rgb;
          float w = 0.1 + dot(c, vec3(0.3, 0.6, 0.1));
          sum += c * w;
          n += w;
        }
      }
    }
    if (n > 0.0) {
      vec3 avg = sum / n;
      o = vec4(p.a > 0.75 ? mix(p.rgb, avg, pc.a.x) : avg, 1.0);
    } else if (p.a > 0.75) {
      o = p;
    } else {
      // Not seen yet: the ambient color, warmer and brighter.
      vec4 amb = texelFetch(prev, ivec2(0, 0), 0);
      o = vec4(min(amb.rgb * vec3(1.35, 1.2, 0.95), vec3(1.0)), 0.5);
    }
  }
}
)", true);
  // Distance fog, 2: height fog along each pixel's view ray (thicker low
  // down, none near the camera), in the sky's color, brighter and warmer
  // toward the sun.
  copy_fs_[13] = CompileBuiltin("builtin_fog", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D fog_color;
layout(push_constant) uniform Fog {
  mat4 m;      // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 cam;    // camera position, -
  vec4 sun;    // direction toward the sun, strength (0: none)
  vec4 a;      // density (per world unit at the base height), start distance,
               // height falloff (per unit), base height
  vec4 b;      // most fog (0..1), 1 / width, 1 / height, -
} pc;
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.b.yz;
  vec4 c = texture(scene, uv);
  float d = texture(depth, uv).r;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  // The view ray: through a point halfway in depth.
  vec4 mid = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(mid.xyz / mid.w - pc.cam.xyz);
  float dist;
  vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
  if (d <= 0.0 || abs(wp.w) < 1e-7) {
    dist = 20000.0;  // the sky: far away
  } else {
    dist = length(wp.xyz / wp.w - pc.cam.xyz);
  }
  float len = max(dist - pc.a.y, 0.0);
  if (len <= 0.0) {
    o = c;
    return;
  }
  // Exponential height fog, integrated from the start distance onward.
  vec3 s0 = pc.cam.xyz + dir * pc.a.y;
  float k = pc.a.z;
  // At most twice as dense below the base height (deep valleys don't fill
  // up solid).
  float base = pc.a.x * exp(min(-k * (s0.y - pc.a.w), 0.7));
  float t = k * dir.y * len;
  // Looking down, the ray goes into denser air: capped as above.
  float amount = abs(t) > 1e-4 ? base * (1.0 - exp(-t)) / (k * dir.y) : base * len;
  amount = min(amount, pc.a.x * 2.0 * len);
  float f = min(1.0 - exp(-max(amount, 0.0)), pc.b.x);
  // Lit by the sun (or moon) everywhere: the ambient sky color, blending
  // toward the sun's color with a broad forward-scattering lobe (strongest
  // looking toward it, never fully gone).
  vec3 ambient = texelFetch(fog_color, ivec2(0, 0), 0).rgb;
  vec3 fc = ambient;
  if (pc.sun.w > 0.0) {
    vec3 sun_color = texelFetch(fog_color, ivec2(1, 0), 0).rgb;
    float g = 0.6;
    float cos_t = dot(dir, pc.sun.xyz);
    float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cos_t, 1.5);
    float hg_max = (1.0 - g * g) / pow(1.0 - g, 3.0);
    float lit = 0.15 + 0.85 * hg / hg_max;
    fc = mix(ambient, sun_color, clamp(lit * pc.sun.w, 0.0, 1.0));
  }
  o = vec4(mix(c.rgb, fc, f), c.a);
}
)", true);
  // God rays, 1 (half size): the light that can shine through: the sky
  // around the sun, its brighter parts more; anything drawn blocks it.
  copy_fs_[14] = CompileBuiltin("builtin_ray_mask", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(push_constant) uniform Mask {
  vec4 a;  // sun u, sun v, aspect (w / h), radius
  vec4 b;  // 1 / width, 1 / height, -, -
} pc;
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.b.xy;
  // The four depths under this half-size pixel: any geometry blocks.
  vec2 h = 0.25 * pc.b.xy;
  float d = max(max(texture(depth, uv + vec2(-h.x, -h.y)).r, texture(depth, uv + vec2(h.x, -h.y)).r),
                max(texture(depth, uv + vec2(-h.x, h.y)).r, texture(depth, uv + vec2(h.x, h.y)).r));
  if (d > 0.0) {
    o = vec4(0.0);
    return;
  }
  vec3 c = texture(scene, uv).rgb;
  float r = length((uv - pc.a.xy) * vec2(pc.a.z, 1.0));
  float fall = 1.0 - smoothstep(0.0, pc.a.w, r);
  float lum = dot(c, vec3(0.3, 0.6, 0.1));
  // Only the brightest sky (the sun and its glare, lit cloud edges): the
  // structure that makes shafts rather than a wash.
  o = vec4(c * fall * fall * smoothstep(0.6, 1.0, lum), 1.0);
}
)", true);
  // God rays, 2: the light smeared away from the sun (each pixel gathers the
  // mask along the line toward it, fading with distance).
  copy_fs_[15] = CompileBuiltin("builtin_ray_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D mask;  // linear, clamped
layout(push_constant) uniform Rays {
  vec4 a;  // sun u, sun v, length (fraction of the way to the sun), decay
  vec4 b;  // 1 / width, 1 / height, -, -
} pc;
layout(location = 0) out vec4 o;
const int N = 48;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.b.xy;
  vec2 step = (pc.a.xy - uv) * pc.a.z / float(N);
  float noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  vec2 p = uv + step * noise;
  float fall = 1.0;
  vec3 sum = vec3(0.0);
  for (int i = 0; i < N; ++i) {
    sum += texture(mask, p).rgb * fall;
    fall *= pc.a.w;
    p += step;
  }
  o = vec4(sum / float(N), 1.0);
}
)", true);
  // God rays, 3: added onto the scene (only where they're bright: the
  // shafts, not a general wash).
  copy_fs_[16] = CompileBuiltin("builtin_ray_add", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D rays;   // half size, filtered
layout(set = 0, binding = 1) uniform sampler2D scene;  // same size as the output
layout(push_constant) uniform Add {
  vec4 a;  // strength, 1 / width, 1 / height, aspect (w / h)
  vec4 b;  // sun u, sun v, -, -
} pc;
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.a.yz;
  vec4 c = texture(scene, uv);
  // Strongest around the sun, gone by most of a screen away from it.
  float dist = length((uv - pc.b.xy) * vec2(pc.a.w, 1.0));
  float near = 1.0 - smoothstep(0.1, 1.1, dist);
  vec3 r = texture(rays, uv).rgb * pc.a.x * near;
  // Screen blend: bright, but never past white.
  o = vec4(1.0 - (1.0 - c.rgb) * (1.0 - min(r, vec3(1.0))), c.a);
}
)", true);
  // Map clouds: the game's cartoon puffs (its pause menu cloud sprite, a.w
  // = 1; procedural look-alikes until the game has used it) covering the
  // screen, dispersing from the middle outward as a.x goes 0 -> 1: each
  // drifts off away from the middle, growing and turning a little, and fades
  // at the end. Units: screen heights (y down). One instanced quad per puff
  // (b.x columns, rows by the sprite), drawn in order and blended "over",
  // which composes them as one layer would: each pixel only pays for the
  // puffs on it (it used to test every puff, which an ARM GPU took 50 ms
  // a frame over).
  const char* clouds_common = R"(
layout(push_constant) uniform Clouds {
  vec4 a;  // progress 0..1, aspect (w / h), 1 / height, sprite (1) or not (0)
  vec4 b;  // columns
} pc;
float hash(float n) { return fract(sin(n) * 43758.5453); }
)";
  clouds_vs_ = CompileBuiltin("builtin_clouds_vs", std::string(R"(#version 450
)") + clouds_common + R"(
layout(location = 0) flat out vec4 v_puff;   // position (screen heights), radius, fade
layout(location = 1) flat out vec3 v_turn;   // angle, mirrored (1) or not, id
void main() {
  bool textured = pc.a.w > 0.5;
  float aspect = pc.a.y;
  float p = pc.a.x;
  int cols = int(pc.b.x);
  int r = gl_InstanceIndex / cols, c = gl_InstanceIndex - r * cols;
  float spacing = textured ? 0.14 : 0.2, R = textured ? 0.115 : 0.165;
  vec2 center = vec2(aspect * 0.5, 0.5);
  float far = length(center) + 0.2;
  float id = float(r * 64 + c) + 1.0;
  vec2 base = vec2((float(c) - 0.5 + ((r & 1) == 1 ? 0.5 : 0.0)) * spacing,
                   (float(r) - 0.5) * spacing * 0.9);
  base += (vec2(hash(id), hash(id + 17.0)) - 0.5) * spacing * 0.25;
  vec2 away = base - center;
  float dn = clamp(length(away) / far, 0.0, 1.0);
  // The middle parts first, the edges last; moving off at once, then
  // speeding up.
  float s = 0.35 * dn + 0.05 * hash(id + 5.0);
  float q = clamp((p - s) / (1.0 - s), 0.0, 1.0);
  float e = q * q * 0.6 + q * 0.4;
  vec2 dir = length(away) > 1e-3 ? normalize(away) : vec2(0.0, -1.0);
  float size = 0.92 + 0.22 * hash(id + 31.0);
  float scale = size * (1.0 + 0.45 * e);
  vec2 pos = base + dir * e * (textured ? 0.45 + 0.35 * aspect : 0.8 + 0.6 * aspect) +
             vec2(0.0, -0.15) * e;
  float fade = 1.0 - smoothstep(0.75, 1.0, q);
  float radius = R * scale;
  // Turned a little (more as it goes, away from the middle), and mirrored
  // for half of them (textured).
  float ang = (hash(id + 3.0) - 0.5) * 0.6 + e * sign(dir.x + 1e-3) * 0.5;
  v_puff = vec4(pos, radius, fade);
  v_turn = vec3(ang, hash(id + 11.0) > 0.5 ? 1.0 : 0.0, id);
  // The quad around it: the shape test's reach, sqrt(2.6) radii, plus a
  // pixel for the edge's anti-aliasing. Faded out: nothing to draw.
  int k = gl_VertexIndex;
  vec2 corner = vec2((k == 1 || k == 2 || k == 4) ? 1.0 : -1.0,
                     (k == 2 || k == 4 || k == 5) ? 1.0 : -1.0);
  vec2 at = pos + corner * (radius * 1.6125 + 2.0 * pc.a.z);
  gl_Position = fade > 0.0 ? vec4(at.x / aspect * 2.0 - 1.0, at.y * 2.0 - 1.0, 0.0, 1.0)
                           : vec4(2.0, 2.0, 2.0, 1.0);
}
)", false);
  // FSR's reactive mask: this frame's (binding 0) and last frame's (1) at
  // where this pixel was (motion vectors, 2: render pixels, this -> last),
  // the largest of a 3x3 there (flakes move a little of their own), fading
// 15% a frame; this frame's widened by a pixel, small things in it (flakes)
// made nearly all-or-nothing.
  copy_fs_[18] = CompileBuiltin("builtin_reactive_merge", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D cur;
layout(set = 0, binding = 1) uniform sampler2D last;
layout(set = 0, binding = 2) uniform sampler2D motion;
layout(location = 0) out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 lim = textureSize(cur, 0) - 1;
  // This frame's: a pixel around each flake too (its edge, half covered at
  // the render size, and the output pixels around it), and nearly all-or-
  // nothing: FSR keeps history in proportion to what's left, so a faint or
  // half-covered flake pixel kept a faint trail.
  float c = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      float v = texelFetch(cur, clamp(p + ivec2(x, y), ivec2(0), lim), 0).r;
      c = max(c, (x == 0 && y == 0) ? v : v * 0.8);
    }
  }
  // Small things (flakes: little around them three pixels out) go nearly
  // all-or-nothing; broad soft ones (a spray cloud, a glow) keep their own
  // strength, or FSR would drop its history over a whole area.
  float ring = 0.0;
  for (int i = 0; i < 8; ++i) {
    ivec2 o8 = ivec2(i < 3 ? -3 : i < 5 ? 0 : 3, i % 3 == 0 ? -3 : i % 3 == 1 ? 0 : 3);
    if (i == 3) o8 = ivec2(0, -3);
    if (i == 4) o8 = ivec2(0, 3);
    ring = max(ring, texelFetch(cur, clamp(p + o8, ivec2(0), lim), 0).r);
  }
  float r = max(min(c * 1.5, 1.0), smoothstep(0.01, 0.12, c - ring));
  vec2 mv = texelFetch(motion, p, 0).xy;
  ivec2 q = ivec2(floor(vec2(p) + 0.5 + mv));
  float h = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      h = max(h, texelFetch(last, clamp(q + ivec2(x, y), ivec2(0), lim), 0).r);
    }
  }
  o = vec4(max(r, h * 0.85), 0.0, 0.0, 1.0);
}
)", true);
  copy_fs_[19] = CompileBuiltin("builtin_reactive_view", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D mask;
layout(push_constant) uniform View { vec2 inv_size; } pc;  // 1 / output size
layout(location = 0) out vec4 o;
void main() {
  vec2 uv = gl_FragCoord.xy * pc.inv_size;
  vec3 c = texture(scene, uv).rgb;
  float r = texelFetch(mask, ivec2(uv * vec2(textureSize(mask, 0))), 0).r;
  o = vec4(mix(c * 0.6, vec3(1.0, 0.0, 0.0), r), 1.0);
}
)", true);
  // Ambient occlusion, 1 (half size): how much of the open hemisphere above
  // each point the nearby depth hides (scalable ambient obscurance: points
  // on a spiral within a world radius, projected onto the screen). R = the
  // light left (1 = none hidden), G = the point's distance (for the blur and
  // the upsampling).
  // Ambient occlusion, 0: the depth its far taps read, smaller and smaller
  // (half, quarter, eighth, sixteenth size: every other texel of the last),
  // the riders' negative. A tap far from its pixel reads a smaller one: the
  // same few texels for the pixels around it, kept in the cache, where
  // reading the full depth all over the screen (three of them) made it the
  // dearest effect up close (6-9 ms a frame on an Arc).
  copy_fs_[59] = CompileBuiltin("builtin_ao_prep", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D late;    // the first: with the riders; then the last level
layout(set = 0, binding = 1) uniform sampler2D depth;   // before the depth-only passes
layout(set = 0, binding = 2) uniform sampler2D hidden;  // after them
layout(push_constant) uniform Prep { vec4 a; } pc;      // a.x: the first (1) or the next (0)
layout(location = 0) out vec4 o;
void main() {
  ivec2 t = ivec2(gl_FragCoord.xy) * 2;
  if (pc.a.x < 0.5) {
    t = min(t, textureSize(late, 0) - 1);
    o = vec4(texelFetch(late, t, 0).r);
    return;
  }
  t = min(t, textureSize(depth, 0) - 1);
  float late_d = texelFetch(late, t, 0).r;
  bool mover = late_d != texelFetch(hidden, t, 0).r;
  float d = mover ? late_d : texelFetch(depth, t, 0).r;
  o = vec4(mover && d > 0.0 ? -d : d);
}
)", true);
  copy_fs_[20] = CompileBuiltin("builtin_ao", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D late;    // with the riders
layout(set = 0, binding = 1) uniform sampler2D depth;   // before the depth-only passes
layout(set = 0, binding = 2) uniform sampler2D hidden;  // after them
layout(set = 0, binding = 3) uniform sampler2D lv0;     // builtin_ao_prep: half size
layout(set = 0, binding = 4) uniform sampler2D lv1;     // quarter
layout(set = 0, binding = 5) uniform sampler2D lv2;     // eighth
layout(push_constant) uniform Ao {
  mat4 m;   // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 cam; // camera position, radius (world units)
  vec4 a;   // 1 / depth width, 1 / depth height, depth pixels per world unit 1 unit away,
            // farthest distance
  vec4 b;   // 1 / output width, 1 / output height, intensity, largest radius (depth pixels)
  vec4 e;   // e.x: the taps from the full depth (diagnostic "aofull"), -
} pc;
layout(location = 0) out vec4 o;
bool Diag_full;
// Depth texels read by their integer position and placed at their centres:
// a half-size pixel's centre falls exactly between two depth texels, and
// which one a sampler picks there differs between GPUs (and from row to
// row), which put lines of wrong normals across the snow on Intel Arc.
ivec2 kSize;
bool kMover;  // set by World: the point is a rider (or what was drawn with one)
vec3 World(ivec2 t, out bool sky) {
  t = clamp(t, ivec2(0), kSize - 1);
  // What was drawn after the depth-only passes (riders), else what was
  // there before them: what they wrote alone shows nowhere.
  float late_d = texelFetch(late, t, 0).r;
  kMover = late_d != texelFetch(hidden, t, 0).r;
  float d = kMover ? late_d : texelFetch(depth, t, 0).r;
  vec2 uv = (vec2(t) + 0.5) * pc.a.xy;
  vec4 p = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  sky = d <= 0.0 || abs(p.w) < 1e-7;
  return sky ? vec3(0.0) : p.xyz / p.w;
}
// A tap: from the smaller depths, the farther off the smaller (as many
// texels apart, about, at each).
vec3 Tap(ivec2 t, int dist_px, out bool sky) {
  int level = clamp(findMSB(max(dist_px, 1)) - 3, 0, 2);
  int sh = level + 1;
  ivec2 q = clamp(t, ivec2(0), kSize - 1) >> sh;
  float d = level == 0 ? texelFetch(lv0, q, 0).r : level == 1 ? texelFetch(lv1, q, 0).r : texelFetch(lv2, q, 0).r;
  kMover = d < 0.0;
  d = abs(d);
  vec2 uv = (vec2(q << sh) + 0.5) * pc.a.xy;
  vec4 p = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  sky = d <= 0.0 || abs(p.w) < 1e-7;
  return sky ? vec3(0.0) : p.xyz / p.w;
}
void main() {
  kSize = textureSize(depth, 0);
  Diag_full = pc.e.x > 0.5;
  // The depth texel this half-size pixel covers (top left of its 2 x 2).
  ivec2 c = ivec2(gl_FragCoord.xy) * 2;
  bool sky;
  vec3 p = World(c, sky);
  bool here_mover = kMover;
  float dist = length(p - pc.cam.xyz);
  if (sky || dist > pc.a.w) {
    o = vec4(1.0, sky ? 60000.0 : dist, 0.0, 0.0);  // (16-bit float: < 65504)
    return;
  }
  // The surface's normal from its neighbours (the nearer one each way, so
  // edges don't bend it).
  bool s0, s1;
  vec3 x1 = World(c + ivec2(1, 0), s0) - p, x0 = p - World(c - ivec2(1, 0), s1);
  vec3 dx = (s0 || (!s1 && dot(x0, x0) < dot(x1, x1))) ? x0 : x1;
  vec3 y1 = World(c + ivec2(0, 1), s0) - p, y0 = p - World(c - ivec2(0, 1), s1);
  vec3 dy = (s0 || (!s1 && dot(y0, y0) < dot(y1, y1))) ? y0 : y1;
  // Steps to both neighbours large for its distance, both ways: a thin or
  // broken-up surface (stippled foliage, wire, a far edge) whose depth
  // says little about what's around it; the occlusion fades out there.
  float step_x = sqrt(min(dot(x0, x0), dot(x1, x1)));
  float step_y = sqrt(min(dot(y0, y0), dot(y1, y1)));
  float ragged = smoothstep(0.03, 0.08, max(step_x, step_y) / dist);
  vec3 n = cross(dx, dy);
  if (dot(n, n) < 1e-12 || ragged >= 1.0) {
    o = vec4(1.0, dist, 0.0, 0.0);
    return;
  }
  n = normalize(n);
  if (dot(n, pc.cam.xyz - p) < 0.0) n = -n;
  // The radius on screen, at most pc.b.w pixels: up close that's less
  // than pc.cam.w in the world, and the thresholds below follow it (or a
  // rider a few metres away would cast nothing).
  float rpx = min(pc.cam.w * pc.a.z / dist, pc.b.w);
  float r = rpx * dist / pc.a.z;
  if (rpx < 2.0) {
    o = vec4(1.0, dist, 0.0, 0.0);
    return;
  }
  // A different turn of the spiral per pixel (interleaved gradient noise);
  // the blur evens it out.
  float spin = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  const int kTaps = 12;
  float sum = 0.0;
  for (int i = 0; i < kTaps; ++i) {
    float f = (float(i) + 0.5) / float(kTaps);
    float ang = (f * 7.0 + spin) * 6.2831853;
    ivec2 off = ivec2(round(vec2(cos(ang), sin(ang)) * (f * rpx)));
    if (off == ivec2(0)) continue;
    bool sk;
    vec3 v = (Diag_full ? World(c + off, sk) : Tap(c + off, int(f * rpx), sk)) - p;
    if (sk) continue;
    float vv = dot(v, v);
    // A rider darkens only what it touches (the snow right under the board
    // and boots, a few tens of cm): over the full radius it laid a dark
    // halo on the snow around it, on top of (and past) its sun shadow.
    float rr = kMover && !here_mover ? min(r, 0.45) : r;
    float falloff = max(1.0 - vv / (rr * rr), 0.0);
    // Above a ~20 degree horizon and proud of the surface by more than an
    // eighth of the radius (30 cm at the full 2.5 m; 6 cm for a rider): the folds between the
    // terrain's flat triangles, the foot of a snow ramp and the board trails
    // laid on the snow don't count; walls, trunks, riders and corners do.
    sum += max((dot(v, n) - 0.12 * rr) * inversesqrt(vv + 1e-6) - 0.5, 0.0) * falloff;
  }
  float ao = clamp(1.0 - pc.b.z * sum * (2.0 / float(kTaps)), 0.0, 1.0);
  ao = mix(ao, 1.0, ragged);
  // Gone toward the farthest distance.
  ao = mix(ao, 1.0, smoothstep(pc.a.w * 0.6, pc.a.w, dist));
  o = vec4(ao, dist, 0.0, 0.0);
}
)", true);
  // Ambient occlusion, 2: blurred one way (a.xy: the step), only across
  // points at about the same distance.
  copy_fs_[21] = CompileBuiltin("builtin_ao_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D ao;
layout(push_constant) uniform Blur { vec4 a; } pc;  // step (texels), -
layout(location = 0) out vec4 o;
void main() {
  ivec2 size = textureSize(ao, 0);
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec2 here = texelFetch(ao, c, 0).rg;
  float tol = 0.04 * here.y + 0.05;
  float sum = 0.0, wsum = 0.0;
  for (int i = -4; i <= 4; ++i) {
    vec2 s = texelFetch(ao, clamp(c + ivec2(pc.a.xy) * i, ivec2(0), size - 1), 0).rg;
    float w = exp(-float(i * i) / 10.0) * exp(-abs(s.y - here.y) / tol);
    sum += s.x * w;
    wsum += w;
  }
  o = vec4(sum / max(wsum, 1e-6), here.y, 0.0, 0.0);
}
)", true);
  // Ambient occlusion, 3: the scene darkened by it (the half-size result
  // brought up to size from the neighbours at the pixel's own distance).
  copy_fs_[22] = CompileBuiltin("builtin_ao_apply", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D ao;
layout(set = 0, binding = 3) uniform sampler2D sunlit;   // the sun's shadows (1: lit)
layout(set = 0, binding = 4) uniform sampler2D normals;  // the scene's (the lighting's)
layout(push_constant) uniform Apply {
  mat4 m;   // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 cam; // camera position, -
  vec4 a;   // 1 / width, 1 / height, debug view (1), -
  vec4 l;   // toward the sun (or moon), its light over the sky's (0: no split)
} pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 px = ivec2(gl_FragCoord.xy);
  vec2 uv = (vec2(px) + 0.5) * pc.a.xy;
  vec4 c = texelFetch(scene, px, 0);
  float d = texelFetch(depth, px, 0).r;
  vec4 p = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  if (d <= 0.0 || abs(p.w) < 1e-7) {
    o = pc.a.z > 0.5 ? vec4(1.0) : c;
    return;
  }
  float dist = length(p.xyz / p.w - pc.cam.xyz);
  vec2 hs = vec2(textureSize(ao, 0));
  vec2 f = uv * hs - 0.5;
  ivec2 i0 = ivec2(floor(f));
  vec2 fr = f - floor(f);
  float tol = 0.04 * dist + 0.05;
  float sum = 0.0, wsum = 0.0;
  for (int k = 0; k < 4; ++k) {
    ivec2 off = ivec2(k & 1, k >> 1);
    vec2 s = texelFetch(ao, clamp(i0 + off, ivec2(0), ivec2(hs) - 1), 0).rg;
    float bw = (off.x == 1 ? fr.x : 1.0 - fr.x) * (off.y == 1 ? fr.y : 1.0 - fr.y);
    float e = (s.y - dist) / tol;
    float w = bw * exp(-e * e);
    sum += s.x * w;
    wsum += w;
  }
  // None of the four at this pixel's distance: something drawn in front
  // after the occlusion's depth (a rider, snow spray) - left as it is.
  float a = mix(1.0, sum / max(wsum, 1e-6), clamp(wsum * 4.0, 0.0, 1.0));
  // Occlusion hides the sky's light, not the sun's: where the sun reaches
  // (lit, facing it) its share of the light is left as it is - sunlit snow
  // beside a tree isn't dimmed by it, the shaded side is.
  if (pc.l.w > 0.0) {
    float lit = texelFetch(sunlit, px, 0).r;
    vec3 n = normalize(texelFetch(normals, px, 0).xyz * 2.0 - 1.0);
    float s = lit * max(dot(n, pc.l.xyz), 0.0) * pc.l.w;
    a = mix(a, 1.0, s / (1.0 + s));
  }
  o = pc.a.z > 0.5 ? vec4(vec3(a), 1.0) : vec4(c.rgb * a, c.a);
}
)", true);
  // Sun shadows, 1 (DrawSunShadows): for each texel of the game's shadow map
  // (one ray of sunlight), the ground it lands on and the nearest thing
  // casting onto it, as distances along the sun's direction (metres): x the
  // ground (what the world map meets first along the ray, else what the
  // shadow pass met), y the caster (1e9: none). h0-h2: map (u, v, 1) -> the
  // world map's ndc (homogeneous); h0.w, h1.w: its depth -> metres (ref,
  // scale). a: map (u, v, depth) -> world; f: the sun's direction, w = 1
  // when a (the casters' height) is known.
  copy_fs_[23] = CompileBuiltin("builtin_sun_prep", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D zo;     // the shadow pass's depth
layout(set = 0, binding = 1) uniform sampler2D world;  // the world map: depth, 1 = nothing
layout(set = 0, binding = 2) uniform sampler2D raw;    // the map unblurred: b = stencil (0: caster)
layout(push_constant) uniform Prep { vec4 h0; vec4 h1; vec4 h2; mat4 a; vec4 f; } pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 size = textureSize(raw, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec3 uv1 = vec3((vec2(p) + 0.5) / vec2(size), 1.0);
  vec3 s3 = vec3(dot(pc.h0.xyz, uv1), dot(pc.h1.xyz, uv1), dot(pc.h2.xyz, uv1));
  vec2 st = s3.xy / s3.z;
  ivec2 gsize = textureSize(world, 0);
  vec2 g = (0.5 + 0.5 * st) * vec2(gsize);
  float gz = 1.0;
  if (pc.h2.w > 0.5 && all(greaterThanEqual(g, vec2(0.0))) && all(lessThan(g, vec2(gsize)))) {
    gz = texelFetch(world, ivec2(g), 0).r;
  }
  float dr = gz < 1.0 ? pc.h0.w + (gz - 0.5) * pc.h1.w : 1e9;
  float dso = 1e9;
  if (pc.f.w > 0.5) {
    vec4 wo = pc.a * vec4(uv1.xy, texelFetch(zo, p, 0).r, 1.0);
    float d = dot(wo.xyz / wo.w, pc.f.xyz);
    if (texelFetch(raw, p, 0).b < 0.5) dso = d;
    if (gz >= 1.0) dr = d;  // not in the world map: what the ray meets first
  }
  o = vec4(dr, dso, 0.0, 0.0);
}
)", true);
  // Sun shadows, 1b: where the game's casters are, coarsely (1/8 size: the
  // least stencil of each 8 x 8 block), then that spread over 4 blocks each
  // way: the filter skips everywhere else (most of the map).
  // Shaded riders, 2b: the shade blurred one way (a.xy: the step), only
  // across points on the same surface (within a few cm of each other): what
  // was left of the depth's own grain up close (a face, a hand) evened out.
  copy_fs_[33] = CompileBuiltin("builtin_movers_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D shade;
layout(set = 0, binding = 1) uniform sampler2D pos;
layout(push_constant) uniform Blur { vec4 a; } pc;  // step (texels), -
layout(location = 0) out vec4 o;
void main() {
  ivec2 size = textureSize(shade, 0);
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec4 q = texelFetch(pos, c, 0);
  float here = texelFetch(shade, c, 0).r;
  if (q.w < 0.5) {
    o = vec4(here);
    return;
  }
  float sum = 0.0, wsum = 0.0;
  for (int i = -4; i <= 4; ++i) {
    ivec2 t = clamp(c + ivec2(pc.a.xy) * i, ivec2(0), size - 1);
    vec4 qi = texelFetch(pos, t, 0);
    if (qi.w < 0.5) continue;
    vec3 d = qi.xyz - q.xyz;
    float w = exp(-float(i * i) / 8.0) * exp(-dot(d, d) / 0.0016);
    sum += texelFetch(shade, t, 0).r * w;
    wsum += w;
  }
  o = vec4(wsum > 0.0 ? sum / wsum : here);
}
)", true);
  // Time of day, 1: the lighting's sky-light table (its tf1: what the air
  // adds, rgb, and lets through, a, by distance and angle to the sun) tinted
  // for the hour.
  copy_fs_[35] = CompileBuiltin("builtin_tod_tint", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Tint { vec4 a; } pc;  // rgb: times, w: -
layout(location = 0) out vec4 o;
void main() {
  vec4 c = texelFetch(src, ivec2(gl_FragCoord.xy), 0);
  o = vec4(c.rgb * pc.a.rgb, c.a);
}
)", true);
  // Time of day, 2: the sky (where nothing was drawn: depth 0). The game's
  // panorama recolored for the hour (its colors over the same sky's at the
  // mountain's own hour), its painted sun dimmed once the sun has moved
  // away; clouds drifting over it; the sun's disc, the moon's (lit from the
  // sun's side) and the stars.
  copy_fs_[34] = CompileBuiltin("builtin_tod_sky", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
// 4 x 1: the sky's colors now (top, horizon) and at the mountain's hour (top,
// horizon).
layout(set = 0, binding = 2) uniform sampler2D colors;
layout(push_constant) uniform Sky {
  vec4 c;       // the view ray through the screen's middle (xyz), seconds
  vec4 r;       // its change across the screen (ndc x = 1), night (0..1)
  vec4 u;       // (ndc y = 1), stars (0..1)
  vec4 sun;     // toward the sun, its disc (0..1)
  vec4 moon;    // toward the moon, its brightness (phase, 0..1)
  vec4 painted; // toward the panorama's painted sun, how much to dim it
  vec4 sun_col; // the sun's color, cloud cover (0..1)
  vec4 sz;      // 1 / width, 1 / height, dusk (0..1), exposure (eye adaptation)
} pc;
layout(location = 0) out vec4 o;
float hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 w = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), w.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), w.x), w.y);
}
float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 5; ++i) {
    v += a * noise(p);
    p = p * 2.03 + vec2(17.1, 9.7);
    a *= 0.5;
  }
  return v;
}
// The panorama's color at `dir` for the hour: its painted sun dimmed (as
// the sun moves away), recolored (the hour's sky over the mountain's, per
// channel, top to horizon) and, further from the mountain's hour, more the
// hour's own sky keeping the panorama's light and shade.
vec3 recolor(vec3 col, vec3 dir) {
  float up = clamp(dir.y, 0.0, 1.0);
  vec3 c = col;
  float lum = dot(c, vec3(0.333));
  float ps = smoothstep(0.9925, 0.9994, dot(dir, pc.painted.xyz)) * pc.painted.w;
  c = mix(c, c / max(lum, 1e-3) * min(lum, 0.62), ps);
  float hz = pow(up, 0.6);
  vec3 now = mix(texelFetch(colors, ivec2(1, 0), 0).rgb, texelFetch(colors, ivec2(0, 0), 0).rgb, hz);
  vec3 then = mix(texelFetch(colors, ivec2(3, 0), 0).rgb, texelFetch(colors, ivec2(2, 0), 0).rgb, hz);
  vec3 scale = now / max(then, vec3(1e-3));
  vec3 detail = now * clamp(0.6 + 0.4 * dot(col, vec3(0.333)) / max(dot(then, vec3(0.333)), 0.05), 0.4, 1.5);
  return mix(c * scale, detail, pc.painted.w * 0.8);
}
void main() {
  vec2 uv = gl_FragCoord.xy * pc.sz.xy;
  vec4 col = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
  float d = texelFetch(depth, ivec2(gl_FragCoord.xy), 0).r;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec3 dir = normalize(pc.c.xyz + ndc.x * pc.r.xyz + ndc.y * pc.u.xyz);
  if (d > 0.0) {
    // Edges against the sky (branches, a skyline): partly the panorama's
    // color, smoothed in, so recolored by as much (else they glint with the
    // day's sky at night). How much: where the color sits between the
    // nearby sky's and the darkest nearby solid color.
    ivec2 p = ivec2(gl_FragCoord.xy), sz = textureSize(depth, 0) - 1;
    vec3 sky = vec3(0.0), solid = vec3(1e9);
    float n = 0.0;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        if (x == 0 && y == 0) continue;
        ivec2 q = clamp(p + ivec2(x, y), ivec2(0), sz);
        vec3 qc = texelFetch(scene, q, 0).rgb;
        if (texelFetch(depth, q, 0).r <= 0.0) {
          sky += qc;
          n += 1.0;
        } else if (dot(qc, vec3(1.0)) < dot(solid, vec3(1.0))) {
          solid = qc;
        }
      }
    }
    if (n < 0.5 || solid.x > 1e8) {
      o = col;
      return;
    }
    sky /= n;
    vec3 span = sky - solid;
    float f = clamp(dot(col.rgb - solid, span) / max(dot(span, span), 1e-5), 0.0, 1.0);
    o = vec4(col.rgb + f * (recolor(sky, dir) * pc.sz.w - sky), col.a);
    return;
  }
  float up = clamp(dir.y, 0.0, 1.0);
  float night = pc.r.w;
  vec3 c = recolor(col.rgb, dir) * pc.sz.w;
  // Open sky: not a gap between branches (no stars glittering in the trees).
  float open_sky = 1.0;
  {
    ivec2 p = ivec2(gl_FragCoord.xy), sz = textureSize(depth, 0) - 1;
    for (int i = 0; i < 4; ++i) {
      ivec2 off = ivec2(i == 0 ? 3 : (i == 1 ? -3 : 0), i == 2 ? 3 : (i == 3 ? -3 : 0));
      if (texelFetch(depth, clamp(p + off, ivec2(0), sz), 0).r > 0.0) open_sky = 0.0;
    }
  }
  // Sunward glow at dusk: the sky warm around the sun, low down.
  float toward = max(dot(dir, pc.sun.xyz), 0.0);
  c += pc.sun_col.rgb * pc.sz.z * (0.12 * pow(toward, 6.0) + 0.2 * pow(toward, 48.0)) *
       (1.0 - 0.8 * up);
  // Clouds: a layer overhead, drifting.
  float cloud = 0.0;
  if (pc.sun_col.w > 0.0 && dir.y > 0.0) {
    vec2 p = dir.xz / (dir.y + 0.08) * 0.9 + vec2(pc.c.w * 0.006, pc.c.w * 0.0025);
    float n = fbm(p);
    float cover = pc.sun_col.w;
    cloud = smoothstep(1.0 - cover - 0.12, 1.0 - cover + 0.25, n) * smoothstep(0.0, 0.12, dir.y);
    // Lit: thin edges bright toward the sun, the bulk greyer; at night dark,
    // a little moonlight.
    float thick = smoothstep(1.0 - cover, 1.0 - cover + 0.45, n);
    vec3 day_col = mix(vec3(0.92, 0.93, 0.96), pc.sun_col.rgb, pc.sz.z * 0.7) *
                   (1.0 - 0.35 * thick) * (1.0 + 0.6 * pow(toward, 8.0) * (1.0 - thick));
    vec3 night_col = vec3(0.035, 0.04, 0.055) + vec3(0.05, 0.055, 0.07) * pc.moon.w * max(pc.moon.y, 0.0);
    vec3 cc = mix(day_col, night_col, night);
    c = mix(c, cc, cloud * 0.9);
  }
  float clear = 1.0 - cloud;
  // Stars: one cell in a few hundred, twinkling, fading toward the horizon.
  if (pc.u.w > 0.0) {
    vec3 a = abs(dir);
    vec2 sp = a.y > max(a.x, a.z) ? dir.xz / a.y : (a.x > a.z ? dir.yz / a.x : dir.xy / a.z);
    float face = a.y > max(a.x, a.z) ? (dir.y > 0.0 ? 0.0 : 1.0) : (a.x > a.z ? 2.0 : 3.0);
    vec2 cell = floor(sp * 320.0) + face * 1000.0;
    float h = hash(cell);
    if (h > 0.9965) {
      vec2 f = fract(sp * 320.0) - 0.5;
      float b = pow(hash(cell + 7.0), 3.0) * 1.6 + 0.2;
      float tw = 0.75 + 0.25 * sin(pc.c.w * (1.5 + 3.0 * hash(cell + 3.0)) + h * 60.0);
      float star = exp(-dot(f, f) * 30.0) * b * tw;
      c += vec3(0.9, 0.93, 1.0) * star * pc.u.w * clear * open_sky * smoothstep(0.02, 0.25, dir.y);
    }
  }
  // The moon: its disc lit from the sun's side, a faint glow round it.
  {
    float m = dot(dir, pc.moon.xyz);
    const float R = 0.0105;  // its radius (radians, a little big, as it looks)
    float ang = acos(clamp(m, -1.0, 1.0));
    if (pc.moon.y > -0.05) {
      vec3 t1 = normalize(cross(abs(pc.moon.y) < 0.99 ? vec3(0, 1, 0) : vec3(1, 0, 0), pc.moon.xyz));
      vec3 t2 = cross(pc.moon.xyz, t1);
      vec2 q = vec2(dot(dir, t1), dot(dir, t2)) / R;
      float r2 = dot(q, q);
      float vis = smoothstep(-0.02, 0.03, pc.moon.y) * clear;
      if (r2 < 1.0) {
        vec3 nrm = q.x * t1 + q.y * t2 - sqrt(1.0 - r2) * pc.moon.xyz;
        float lit = smoothstep(-0.05, 0.12, dot(nrm, pc.sun.xyz));
        float maria = 0.85 + 0.15 * noise(q * 3.0 + 5.0);
        float edge = smoothstep(1.0, 0.9, r2);
        vec3 mc = vec3(0.86, 0.88, 0.92) * maria * (lit + 0.03);
        c = mix(c, mc, edge * vis * (0.35 + 0.65 * night));
      }
      c += vec3(0.55, 0.6, 0.7) * 0.12 * pc.moon.w * night * exp(-ang * 18.0) * vis;
    }
  }
  // Ice in thin high cloud: the 22-degree halo round the sun (reddish
  // inside, paling out) and the sun dogs either side at its height (a little
  // farther out the higher it is, a white tail outward); where the sky's
  // open.
  float halo = texelFetch(colors, ivec2(0, 0), 0).a;
  if (halo > 0.0 && pc.sun.y > 0.0) {
    float th = acos(clamp(dot(dir, pc.sun.xyz), -1.0, 1.0));
    const float R22 = 0.3840;
    float x = (th - R22) / 0.013;
    float ring = exp(-x * x) + 0.35 * exp(-max(x, 0.0) * 0.6) * step(0.0, x) * exp(-max(x - 3.0, 0.0));
    vec3 rc = mix(vec3(1.0, 0.55, 0.35), vec3(0.95, 0.97, 1.0), smoothstep(-1.0, 1.5, x));
    // (Inside the ring the sky a little darker: the ice turns its light away.)
    float inside = smoothstep(R22, R22 - 0.05, th) * smoothstep(0.05, 0.15, th);
    vec3 hc = rc * ring * 0.12 - vec3(0.02) * inside;
    // Sun dogs.
    float es = asin(clamp(pc.sun.y, -1.0, 1.0)), e = asin(clamp(dir.y, -1.0, 1.0));
    vec2 sh = normalize(pc.sun.xz + vec2(1e-6)), dh = normalize(dir.xz + vec2(1e-6));
    float daz = acos(clamp(dot(sh, dh), -1.0, 1.0));
    float a0 = asin(clamp(sin(R22) / max(cos(es), 0.2), 0.0, 1.0));
    float de = (e - es) / 0.025, dz = (daz - a0) / 0.012;
    float dog = exp(-de * de - dz * dz * (dz < 0.0 ? 1.0 : 0.15));
    vec3 dc = mix(vec3(1.0, 0.5, 0.3), vec3(1.0), smoothstep(-1.0, 2.0, dz));
    hc += dc * dog * 0.35;
    c += max(hc, vec3(-0.03)) * pc.sun_col.rgb * halo * clear * open_sky;
  }
  // A sun pillar: with the sun at the horizon (just up or just set) in still
  // cold air, its light thrown up in a narrow column by the ice plates
  // floating flat (texel 1's alpha: how strong), widening and fading upward;
  // a short stub below it.
  float pillar = texelFetch(colors, ivec2(1, 0), 0).a;
  if (pillar > 0.0) {
    float es = asin(clamp(pc.sun.y, -1.0, 1.0)), e = asin(clamp(dir.y, -1.0, 1.0));
    vec2 sh = normalize(pc.sun.xz + vec2(1e-6)), dh = normalize(dir.xz + vec2(1e-6));
    float side = acos(clamp(dot(sh, dh), -1.0, 1.0)) * cos(e);
    float up = e - max(es, -0.01);
    float width = 0.006 + 0.05 * max(up, 0.0);
    float across = exp(-pow(side / width, 2.0));
    float along = up > 0.0 ? exp(-up / 0.07) * (0.6 + 0.4 * exp(-up / 0.015)) : exp(up / 0.012) * 0.5;
    c += pc.sun_col.rgb * across * along * pillar * 0.45 * clear * open_sky;
  }
  // The sun's disc and a glow, once it isn't where the panorama paints it.
  if (pc.sun.w > 0.0) {
    float s = dot(dir, pc.sun.xyz);
    float ang = acos(clamp(s, -1.0, 1.0));
    float disc = smoothstep(0.0105, 0.0085, ang);
    float glow = exp(-ang * 14.0) * 0.5 + exp(-ang * 60.0) * 0.6;
    c += pc.sun_col.rgb * (disc * 1.2 * (1.0 - 0.8 * cloud) + glow * (1.0 - 0.5 * cloud)) *
         pc.sun.w * pc.painted.w;
  }
  o = vec4(c, col.a);
}
)", true);
  // Eye adaptation: the scene's average brightness (log luminance over a
  // grid, the middle counting most).
  copy_fs_[36] = CompileBuiltin("builtin_eye_measure", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(location = 0) out vec4 o;
void main() {
  float sum = 0.0, wsum = 0.0;
  for (int y = 0; y < 18; ++y) {
    for (int x = 0; x < 32; ++x) {
      vec2 uv = (vec2(x, y) + 0.5) / vec2(32.0, 18.0);
      vec3 c = textureLod(scene, uv, 0.0).rgb;
      float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
      vec2 d = (uv - 0.5) * vec2(1.6, 2.0);
      float w = exp(-dot(d, d) * 1.5);
      sum += log(max(l, 0.002)) * w;
      wsum += w;
    }
  }
  o = vec4(sum / wsum);
}
)", true);
  // Time of day: each pixel's shade from the riders, rails and objects
  // (their map from the hour's sun), percentage closer, for the lighting.
  // Snow tracks, 1: this frame's trails (drawn from above into the stamp:
  // depth under 1 where one is) pressed into the kept map, its window round
  // the camera: the groove where a board went; beside it the snow it pushed
  // aside (what it cut fresh nearby, and any pile it ran over, pushed on:
  // a skid ploughs a heap up ahead of it). a.y < 0: the whole map filling in
  // (falling snow) by a.z.
  copy_fs_[43] = CompileBuiltin("builtin_tracks_merge", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D stamp;   // 1: nothing
layout(set = 0, binding = 1) uniform sampler2D tracks;  // r groove, g pushed up
layout(set = 0, binding = 2) uniform sampler2D stamp2;  // again, where the trail's alpha is 0.15 and up
layout(set = 0, binding = 3) uniform sampler2D stamp3;  // 0.3 and up
layout(push_constant) uniform Merge {
  vec4 a;  // the window's corner in the map, filled, push
  vec4 b;  // (test, A3_TEST_TRACKS: a board instead of the stamp) its middle (window texels), along it
  vec4 c;  // half its length and width (texels), on, graded (stamp2, stamp3)
} pc;
layout(location = 0) out vec4 o;
float Cov(ivec2 t) {
  if (pc.c.z > 0.5) {
    vec2 r = vec2(t) + 0.5 - pc.b.xy;
    vec2 l = vec2(dot(r, pc.b.zw), dot(r, vec2(-pc.b.w, pc.b.z)));
    return all(lessThan(abs(l), pc.c.xy)) ? 1.0 : 0.0;
  }
  ivec2 n = textureSize(stamp, 0);
  if (any(lessThan(t, ivec2(0))) || any(greaterThanEqual(t, n))) return 0.0;
  float c = texelFetch(stamp, t, 0).r < 1.0 ? 1.0 : 0.0;
  // (As deep as the game's mark is strong: a fifth where it's faint.)
  if (pc.c.w > 0.5 && c > 0.0) {
    c = 0.2 + 0.4 * (texelFetch(stamp2, t, 0).r < 1.0 ? 1.0 : 0.0) + 0.4 * (texelFetch(stamp3, t, 0).r < 1.0 ? 1.0 : 0.0);
  }
  return c;
}
vec4 Raw(ivec2 m) { return texelFetch(tracks, clamp(m, ivec2(0), textureSize(tracks, 0) - 1), 0); }
// (As it shows: an old track fading over the last third of its time.)
vec2 Live(vec4 t) { return t.rg * smoothstep(0.0, 0.35, t.b); }
vec2 Old(ivec2 m) { return Live(Raw(m)); }
// Coverage softened over a few texels: rounded, ramped edges (each frame's
// piece of a trail pressed in squarely left the edges stepped).
float CovSoft(ivec2 t) {
  float c = Cov(t) * 0.2;
  c += (Cov(t + ivec2(2, 0)) + Cov(t - ivec2(2, 0)) + Cov(t + ivec2(0, 2)) + Cov(t - ivec2(0, 2))) * 0.12;
  c += (Cov(t + ivec2(3, 3)) + Cov(t - ivec2(3, 3)) + Cov(t + ivec2(3, -3)) + Cov(t - ivec2(3, -3))) * 0.08;
  return c;
}
void main() {
  ivec2 t = ivec2(gl_FragCoord.xy);
  if (pc.a.y < -0.5) {
    vec4 raw = Raw(t);
    // The wind (b.xy: how far the loose snow moved, texels; b.z the heaps
    // scoured, b.w the grooves drifted in): the surface carried downwind
    // (mixed with what's upwind of it: the shapes soften and creep along),
    // the loose heaps blown away first, the grooves filling with what blows
    // into them.
    if (pc.b.z > 0.0 || pc.b.w > 0.0) {
      vec2 from = vec2(t) + 0.5 - pc.b.xy;
      vec2 f = from - 0.5;
      ivec2 i = ivec2(floor(f));
      vec2 w = f - floor(f);
      vec4 up = mix(mix(Raw(i), Raw(i + ivec2(1, 0)), w.x), mix(Raw(i + ivec2(0, 1)), Raw(i + ivec2(1, 1)), w.x), w.y);
      raw = mix(raw, up, 0.6);
      raw.g *= 1.0 - pc.b.z;
      raw.r *= 1.0 - pc.b.w;
    }
    // (What's left very faint goes: 8-bit, a faint groove times a little
    // less rounded back up to itself and never filled, and the wind's
    // mixing spread such traces over the whole map - its steps of one
    // 255th, made slopes by the normals, striped the snow everywhere.)
    vec2 nw = vec2(raw.r * (1.0 - pc.a.z), raw.g * (1.0 - 0.6 * pc.a.z));
    nw = max(nw - vec2(1.5 / 255.0), vec2(0.0)) * step(vec2(0.025), nw);
    // Aging (c.x): fresher to older; worn out, gone.
    float age = max(raw.b - pc.c.x, 0.0);
    if (age <= 0.0) nw = vec2(0.0);
    o = vec4(nw, age, 1.0);
    return;
  }
  ivec2 m = t + ivec2(pc.a.xy);
  vec4 raw = Raw(m);
  vec2 old = Live(raw);
  float cov = Cov(t);
  float soft = pc.c.z > 0.5 ? cov : CovSoft(t);
  float pushed = 0.0;
  if (cov < 1.0) {
    for (int i = 0; i < 32; ++i) {
      float r = sqrt((float(i) + 0.5) / 32.0) * 10.0;
      float th = float(i) * 2.39996323;
      ivec2 off = ivec2(round(vec2(cos(th), sin(th)) * r));
      if (off == ivec2(0)) continue;
      float c = Cov(t + off);
      if (c <= 0.0) continue;
      vec2 on = Old(m + off);
      // (Pushed up mostly where it's pressed deep: a faint skid's edge
      // hardly heaps; spread out from it, smaller the farther: a rounded
      // bank, not a rim.)
      pushed += (max(c - on.r, 0.0) * c + on.g * 2.2) * (1.0 - r / 11.0);
    }
  }
  float add = pc.a.w * pushed;
  // Untouched this frame: kept as it is (aging on).
  if (soft < 0.002 && add < 0.004) {
    o = raw;
    return;
  }
  float groove = max(old.r, soft);
  float berm = old.g * (1.0 - soft) + add;
  // (Touched: fresh again, as it now shows.)
  o = vec4(groove, clamp(berm, 0.0, 1.0), 1.0, 1.0);
}
)", true);
  // Snow tracks, 2: the lighting's albedo (a.w 0) or normals (1) with the
  // tracks: the groove's walls and floor and the heaps beside it shaped into
  // the normals, packed snow a touch greyer, the heaps grainy.
  copy_fs_[44] = CompileBuiltin("builtin_tracks_apply", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;      // albedo or normals
layout(set = 0, binding = 1) uniform sampler2D normals;
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(set = 0, binding = 3) uniform sampler2D tracks;
layout(push_constant) uniform Apply {
  mat4 m;   // ndc (x, y, 1 - depth, 1) -> world
  vec4 g;   // the map's texel 0 (world x, z), texels per metre, mode
  vec4 s;   // groove depth (m), heap height (m), 1 / width, 1 / height
  vec4 cam; // the camera (parallax), -
} pc;
layout(location = 0) out vec4 o;
)" + kTracksClumps + R"(
// (Hashed as whole numbers: a sine of world-sized numbers is noise of its
// own on some GPUs.)
float H(vec2 p) { return ClumpHash(p); }
vec2 T(vec2 tc) {
  vec2 f = tc - 0.5;
  ivec2 i = ivec2(floor(f));
  vec2 w = f - floor(f);
  ivec2 n = textureSize(tracks, 0) - 1;
  vec2 a = Live(texelFetch(tracks, clamp(i, ivec2(0), n), 0));
  vec2 b = Live(texelFetch(tracks, clamp(i + ivec2(1, 0), ivec2(0), n), 0));
  vec2 c = Live(texelFetch(tracks, clamp(i + ivec2(0, 1), ivec2(0), n), 0));
  vec2 d = Live(texelFetch(tracks, clamp(i + ivec2(1, 1), ivec2(0), n), 0));
  return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}
// Height (m): soft over a texel or so; the clumps on the heaps on top.
float Ht(vec2 tc) {
  vec2 t = (T(tc + vec2(1.0, 1.0)) + T(tc + vec2(-1.0, 1.0)) + T(tc + vec2(1.0, -1.0)) + T(tc - vec2(1.0))) * 0.25;
  float heap = t.g * (1.0 - t.r);
  return -pc.s.x * t.r + pc.s.y * heap + Clumps(tc, heap) * pc.s.y;
}
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(src, ip, 0);
  o = c;
  float d = texelFetch(depth, ip, 0).r;
  if (d <= 0.0) return;
  vec2 uv = gl_FragCoord.xy * pc.s.zw;
  vec4 w = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  vec3 P = w.xyz / w.w;
  vec2 tc = (P.xz - pc.g.xy) * pc.g.z;
  vec2 n2 = vec2(textureSize(tracks, 0));
  if (any(lessThan(tc, vec2(2.0))) || any(greaterThan(tc, n2 - 2.0))) return;
  vec4 nw = texelFetch(normals, ip, 0);
  vec3 N = normalize(nw.xyz * 2.0 - 1.0);
  if (N.y < 0.4) return;  // the ground (snow), not walls
  // (Not on what's tagged as something else: rock, wood, metal, paint,
  // cloth - the surface class in the material byte's top bits.)
  int cls = (int(nw.w * 255.0 + 0.5) >> 5) & 7;
  if (cls > 1) return;
  // Parallax: where the view ray meets the tracks' surface (down in a
  // groove, up on a heap or a clump) rather than the flat slope: marched
  // down from above the highest heap to the groove's floor, the first step
  // under the surface narrowed in on, so heaps and clumps stand up and hide
  // what's behind them at a glancing angle. (Nothing near: left be.)
  {
    vec3 D = normalize(P - pc.cam.xyz);
    if (D.y < -0.05) {
      vec2 k = D.xz / max(-D.y, 0.12) * pc.g.z;  // texels a metre down
      float top = pc.s.y * 1.4, bot = -pc.s.x;
      vec2 near_t = T(tc) + T(tc - k * top) + T(tc - k * bot) + T(tc - k * 0.5 * top);
      if (near_t.r + near_t.g > 0.02) {
        float h_prev = top, h = top;
        bool hit = false;
        for (int i = 1; i <= 12; ++i) {
          h = mix(top, bot, float(i) / 12.0);
          if (Ht(tc - k * h) >= h) {
            hit = true;
            break;
          }
          h_prev = h;
        }
        if (hit) {
          float a = h_prev, b = h;  // above, below
          for (int i = 0; i < 4; ++i) {
            float m = 0.5 * (a + b);
            if (Ht(tc - k * m) >= m) b = m; else a = m;
          }
          h = 0.5 * (a + b);
        }
        tc = tc - k * h;
      }
    }
  }
  if (pc.g.w > 2.5) {
    // (Diagnostics: the normals' w, the game's material.)
    float w = texelFetch(normals, ip, 0).w;
    o = vec4(w, fract(w * 4.0), fract(w * 16.0), 1.0);
    return;
  }
  vec2 t = T(tc);
  if (pc.g.w > 1.5) {
    o = vec4(t.r, t.g, 0.0, 1.0);
    return;
  }
  if (t.r + t.g < 0.02) return;
  if (pc.g.w < 0.5) {
    // Packed in the groove (a little greyer, bluer); the heaps grainy, the
    // clumps' tops a touch brighter (loose, fresh-broken).
    float grain = H(floor(P.xz * 30.0));
    vec3 col = c.rgb * mix(vec3(1.0), vec3(0.92, 0.95, 1.0), t.r * (1.0 - 0.5 * t.g));
    col *= 1.0 + t.g * (1.0 - t.r) * (0.03 + 0.08 * (grain - 0.5));
    col *= 1.0 + 0.6 * Clumps(tc, t.g * (1.0 - t.r));
    o = vec4(col, c.a);
  } else {
    float e = 1.0;
    float hx = (Ht(tc + vec2(e, 0.0)) - Ht(tc - vec2(e, 0.0))) * pc.g.z / (2.0 * e);
    float hz = (Ht(tc + vec2(0.0, e)) - Ht(tc - vec2(0.0, e))) * pc.g.z / (2.0 * e);
    // (Packed smooth in the groove: the grooming's ridges pressed out, the
    // surface's normal averaged round about.)
    vec3 Ns = N;
    if (t.r > 0.01) {
      ivec2 sz = textureSize(normals, 0) - 1;
      vec3 a = vec3(0.0);
      for (int i = 0; i < 4; ++i) {
        ivec2 off = ivec2(i == 0 ? 4 : (i == 1 ? -4 : 0), i == 2 ? 4 : (i == 3 ? -4 : 0));
        a += texelFetch(normals, clamp(ip + off, ivec2(0), sz), 0).xyz * 2.0 - 1.0;
      }
      Ns = normalize(mix(N, normalize(a + N), 0.8 * t.r));
    }
    vec3 n = normalize(Ns + vec3(-hx, 0.0, -hz));
    o = vec4(n * 0.5 + 0.5, c.a);
  }
}
)", true);
  // Volumetric light, 1 (half size): along each pixel's ray, the air lit by
  // the sun (or moon) where its maps say it reaches (the casters' near the
  // camera, the world's farther: trees, props, the slopes) and the clouds
  // don't cover; thicker low down. Steps jittered per pixel (smoothed after).
  copy_fs_[45] = CompileBuiltin("builtin_volume", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth;    // full size (0: nothing)
layout(set = 0, binding = 1) uniform sampler2D casters;  // from the sun: depth, 1 = nothing
layout(set = 0, binding = 2) uniform sampler2D world;    // the world map
layout(set = 0, binding = 3) uniform sampler2D params;   // 8 x 1 (below)
layout(set = 0, binding = 4) uniform sampler2D cmap;     // the cloud layer round about (builtin_cloud_map)
layout(push_constant) uniform Maps {
  mat4 m;                    // ndc (x, y, 1 - depth, 1) -> world
  vec4 o0; vec4 o1; vec4 o2; // the casters' map's rows
  vec4 a;                    // the world map's from the casters': offset x, y, scale k, flags
} pc;
layout(location = 0) out vec4 o;
// params: 0 camera, the farthest (m); 1 the light's color, the air's density
// (/m); 2 toward the light, its forward scattering (g); 3 the height it's
// densest at, how fast it thins (/m), steps, maps (1 casters, 2 world);
// 4 the clouds: cover, clock, height, darkness; 5 1 / size (here), the
// depth's size; 6 the cloud map's corner, 1 / its span.
vec4 P(int i) { return texelFetch(params, ivec2(i, 0), 0); }
float hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 w = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), w.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), w.x), w.y);
}
float Clouds(vec3 X, vec3 L, vec4 c, vec4 cm) {
  if (c.w <= 0.0) return 0.0;
  vec2 q = X.xz + L.xz * (c.z / max(L.y, 0.08));
  vec2 p = q / c.z * 0.9 + vec2(c.y * 0.006, c.y * 0.0025);
  vec2 mu = (p - cm.xy) * cm.z;
  if (all(greaterThan(mu, vec2(0.002))) && all(lessThan(mu, vec2(0.998)))) {
    return c.w * smoothstep(1.0 - c.x - 0.03, 1.0 - c.x + 0.1, textureLod(cmap, mu, 0.0).r);
  }
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 3; ++i) {
    v += a * noise(p);
    p = p * 2.03 + vec2(17.1, 9.7);
    a *= 0.5;
  }
  v += 0.06;  // (the octaves left out, on average)
  return c.w * smoothstep(1.0 - c.x - 0.03, 1.0 - c.x + 0.1, v);
}
float Sun(vec3 X, int maps, float dpm) {
  vec4 Q = vec4(X, 1.0);
  float lit = 1.0;
  if ((maps & 1) != 0) {
    vec2 st = vec2(dot(pc.o0, Q), dot(pc.o1, Q));
    float n = float(textureSize(casters, 0).x);
    vec2 tc = (0.5 + 0.5 * st) * n;
    if (all(greaterThan(tc, vec2(1.0))) && all(lessThan(tc, vec2(n - 1.0)))) {
      float z = texelFetch(casters, ivec2(tc), 0).r;
      if (z < 1.0 && z < dot(pc.o2, Q) - 0.1 * dpm) lit = 0.0;
    }
  }
  if ((maps & 2) != 0 && lit > 0.0) {
    vec2 st = vec2(dot(pc.o0, Q), dot(pc.o1, Q)) * pc.a.z + pc.a.xy;
    float n = float(textureSize(world, 0).x);
    vec2 tc = (0.5 + 0.5 * st) * n;
    if (all(greaterThan(tc, vec2(1.0))) && all(lessThan(tc, vec2(n - 1.0)))) {
      float z = texelFetch(world, ivec2(tc), 0).r;
      if (z < 1.0 && z < dot(pc.o2, Q) - 0.4 * dpm) lit = 0.0;
    }
  }
  return lit;
}
void main() {
  vec4 p0 = P(0), p1 = P(1), p2 = P(2), p3 = P(3), p4 = P(4), p5 = P(5), p6 = P(6);
  vec2 uv = gl_FragCoord.xy * p5.xy;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float d = texelFetch(depth, ivec2(uv * p5.zw), 0).r;
  vec3 cam = p0.xyz;
  vec4 wf = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(wf.xyz / wf.w - cam);
  float dist = p0.w;
  if (d > 0.0) {
    vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
    dist = min(length(wp.xyz / wp.w - cam), p0.w);
  }
  int maps = int(p3.w + 0.5);
  float dpm = length(pc.o2.xyz);
  vec3 L = p2.xyz;
  int steps = int(p3.z + 0.5);
  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  float acc = 0.0;
  float tprev = 0.0;
  for (int i = 0; i < steps; ++i) {
    // (Closer together near the camera: finer shafts there.)
    float f = (float(i) + jit) / float(steps);
    float t = dist * f * (0.35 + 0.65 * f);
    float tn = dist * min((float(i) + 1.0) / float(steps), 1.0);
    vec3 X = cam + dir * t;
    float h = X.y - p3.x;
    float dens = p1.w * clamp(exp(-h * p3.y), 0.25, 3.0);
    float vis = Sun(X, maps, dpm);
    if (vis > 0.0) vis *= 1.0 - Clouds(X, L, p4, p6);
    float dt = max(tn * (0.35 + 0.65 * tn / dist) - tprev, 0.0);
    tprev += dt;
    acc += dens * vis * dt;
  }
  // Scattered toward the camera: more looking toward the light.
  float cs = dot(dir, L);
  float g = p2.w;
  float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cs, 1.5);
  float phase = 0.3 + 0.7 * min(hg, 3.5);
  o = vec4(p1.rgb * acc * phase, dist);
}
)", true);
  // Volumetric light, 2: added to the scene, smoothed from half size (what's
  // at the same distance: not across an outline).
  copy_fs_[46] = CompileBuiltin("builtin_volume_add", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D vol;    // half size: rgb light, a distance
layout(set = 0, binding = 1) uniform sampler2D scene;
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(push_constant) uniform Add {
  mat4 m;
  vec4 a;  // 1 / width, 1 / height, strength, the farthest
  vec4 b;  // camera
} pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(scene, ip, 0);
  vec2 uv = gl_FragCoord.xy * pc.a.xy;
  float d = texelFetch(depth, ip, 0).r;
  float dist = pc.a.w;
  if (d > 0.0) {
    vec4 wp = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
    dist = min(length(wp.xyz / wp.w - pc.b.xyz), pc.a.w);
  }
  ivec2 hs = textureSize(vol, 0);
  vec2 hp = uv * vec2(hs) - 0.5;
  ivec2 base = ivec2(floor(hp));
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int y = -1; y <= 2; ++y) {
    for (int x = -1; x <= 2; ++x) {
      ivec2 q = clamp(base + ivec2(x, y), ivec2(0), hs - 1);
      vec4 v = texelFetch(vol, q, 0);
      vec2 dd = vec2(q) - hp;
      float w = exp(-dot(dd, dd) * 0.6) * exp(-abs(v.a - dist) / (0.06 * dist + 0.4));
      sum += v.rgb * w;
      wsum += w;
    }
  }
  vec3 add = wsum > 1e-4 ? sum / wsum : texture(vol, uv).rgb;
  // (Screen-like: what's bright already takes less.)
  vec3 lit = add * pc.a.z;
  // (Rolled off: thick shafts toward a low sun not a white wall.)
  lit /= 1.0 + 0.9 * dot(lit, vec3(0.3, 0.59, 0.11));
  o = vec4(c.rgb + lit * (1.0 - 0.5 * clamp(c.rgb, 0.0, 1.0)), c.a);
}
)", true);
  // The fog and the volumetric light in one pass (while both are on): the
  // fog as builtin_fog, then the air's light over it.
  copy_fs_[47] = CompileBuiltin("builtin_fog_volume", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D fog_color;
layout(set = 0, binding = 3) uniform sampler2D vol;  // half size: rgb the air's light, a distance
layout(push_constant) uniform Fog {
  mat4 m;      // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 cam;    // camera position, -
  vec4 sun;    // direction toward the sun, strength (0: none)
  vec4 a;      // density (per world unit at the base height), start distance,
               // height falloff (per unit), base height
  vec4 b;      // most fog (0..1), 1 / width, 1 / height, the air's light (strength)
} pc;
layout(location = 0) out vec4 o;
vec4 Fogged(out float dist_out) {
  dist_out = 20000.0;
  vec2 uv = gl_FragCoord.xy * pc.b.yz;
  vec4 c = texture(scene, uv);
  float d = texture(depth, uv).r;
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  // The view ray: through a point halfway in depth.
  vec4 mid = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(mid.xyz / mid.w - pc.cam.xyz);
  float dist;
  vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
  if (d <= 0.0 || abs(wp.w) < 1e-7) {
    dist = 20000.0;  // the sky: far away
  } else {
    dist = length(wp.xyz / wp.w - pc.cam.xyz);
  }
  dist_out = dist;
  float len = max(dist - pc.a.y, 0.0);
  if (len <= 0.0) {
    return c;
  }
  // Exponential height fog, integrated from the start distance onward.
  vec3 s0 = pc.cam.xyz + dir * pc.a.y;
  float k = pc.a.z;
  // At most twice as dense below the base height (deep valleys don't fill
  // up solid).
  float base = pc.a.x * exp(min(-k * (s0.y - pc.a.w), 0.7));
  float t = k * dir.y * len;
  // Looking down, the ray goes into denser air: capped as above.
  float amount = abs(t) > 1e-4 ? base * (1.0 - exp(-t)) / (k * dir.y) : base * len;
  amount = min(amount, pc.a.x * 2.0 * len);
  float f = min(1.0 - exp(-max(amount, 0.0)), pc.b.x);
  // Lit by the sun (or moon) everywhere: the ambient sky color, blending
  // toward the sun's color with a broad forward-scattering lobe (strongest
  // looking toward it, never fully gone).
  vec3 ambient = texelFetch(fog_color, ivec2(0, 0), 0).rgb;
  vec3 fc = ambient;
  if (pc.sun.w > 0.0) {
    vec3 sun_color = texelFetch(fog_color, ivec2(1, 0), 0).rgb;
    float g = 0.6;
    float cos_t = dot(dir, pc.sun.xyz);
    float hg = (1.0 - g * g) / pow(1.0 + g * g - 2.0 * g * cos_t, 1.5);
    float hg_max = (1.0 - g * g) / pow(1.0 - g, 3.0);
    float lit = 0.15 + 0.85 * hg / hg_max;
    fc = mix(ambient, sun_color, clamp(lit * pc.sun.w, 0.0, 1.0));
  }
  return vec4(mix(c.rgb, fc, f), c.a);
}

// Then the volumetric light added (smoothed from half size: what's at the
// same distance, not across an outline).
void main() {
  float dist;
  vec4 c = Fogged(dist);
  dist = min(dist, 220.0);
  vec2 uv = gl_FragCoord.xy * pc.b.yz;
  ivec2 hs = textureSize(vol, 0);
  vec2 hp = uv * vec2(hs) - 0.5;
  ivec2 base = ivec2(floor(hp));
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int y = -1; y <= 2; ++y) {
    for (int x = -1; x <= 2; ++x) {
      ivec2 q = clamp(base + ivec2(x, y), ivec2(0), hs - 1);
      vec4 v = texelFetch(vol, q, 0);
      vec2 dd = vec2(q) - hp;
      float w = exp(-dot(dd, dd) * 0.6) * exp(-abs(v.a - dist) / (0.06 * dist + 0.4));
      sum += v.rgb * w;
      wsum += w;
    }
  }
  vec3 add = wsum > 1e-4 ? sum / wsum : texture(vol, uv).rgb;
  vec3 lit = pc.b.w > 0.0 ? add * pc.b.w : vec3(0.0);
  lit /= 1.0 + 0.9 * dot(lit, vec3(0.3, 0.59, 0.11));
  o = vec4(c.rgb + lit * (1.0 - 0.5 * clamp(c.rgb, 0.0, 1.0)), c.a);
}
)", true);
  // The cloud layer's thickness (its noise, five octaves) once a frame, over
  // the part of it that shades what's round the camera (the layer's own
  // coordinates: a.xy its corner, a.z its span): read by the clouds'
  // shadows and the volumetric light instead of the noise each.
  copy_fs_[48] = CompileBuiltin("builtin_cloud_map", R"(#version 450
layout(push_constant) uniform Map { vec4 a; } pc;
layout(location = 0) out vec4 o;
float hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 w = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), w.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), w.x), w.y);
}
void main() {
  vec2 p = pc.a.xy + gl_FragCoord.xy * pc.a.z / 512.0;
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 5; ++i) {
    v += a * noise(p);
    p = p * 2.03 + vec2(17.1, 9.7);
    a *= 0.5;
  }
  o = vec4(v);
}
)", true);
  // The sun's sheen, 1 (at the lighting pass): GGX by surface, from the
  // albedo (snow told by its color), the normals (their w: foliage), the
  // sun's shadow mask. rgb the sheen, a the depth it was made at.
  copy_fs_[49] = CompileBuiltin("builtin_sun_spec", std::string(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D albedo;
layout(set = 0, binding = 1) uniform sampler2D normals;  // xyz * 2 - 1, w the game's material (foliage > 0)
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(set = 0, binding = 3) uniform sampler2D mask;     // the sun's shadows (1 lit)
// 12 x 1: 0-3 the hour's sky (zenith, horizon), 4-7 the backdrop's light,
// 8 the lighting's stencil test (SunSpecular).
layout(set = 0, binding = 4) uniform sampler2D sky;
layout(set = 0, binding = 5) uniform usampler2D stencil;  // the lighting's depth target's
#define relight sky
layout(push_constant) uniform Spec {
  mat4 m;    // ndc (x, y, 1 - depth, 1) -> world
  vec4 L;    // toward the light, -
  vec4 col;  // its color (exposed), -
  vec4 cam;  // camera, mask on (0/1)
  vec4 env;  // the sky's reflection: strength (exposure; 0 none), -
} pc;
layout(location = 0) out vec4 o;
// (An integer hash: a cell's own, far from the origin too.)
float Hash3(ivec3 c, int k) {
  uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ uint(k) * 2654435761u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return float(h & 0xFFFFFFu) / 16777216.0;
}
)") + kSnowGlint + R"(float D_GGX(float nh, float a) {
  float a2 = a * a;
  float dd = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / (3.14159 * dd * dd);
}
float G_Smith(float nv, float nl, float a) {
  float k = a * 0.5;
  return (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
}
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  ivec2 sz = textureSize(depth, 0);
  float d = texelFetch(depth, ip, 0).r;
  if (d <= 0.0) {
    o = vec4(0.0);
    return;
  }
  vec2 uv = (gl_FragCoord.xy) / vec2(sz);
  vec4 wp = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  vec3 P = wp.xyz / wp.w;
  vec3 V = normalize(pc.cam.xyz - P);
  vec4 nw = texelFetch(normals, ip, 0);
  // No normal there (the lighting's clear): the far-off scenery beyond the
  // slopes, drawn with its light painted in and never lit by the lighting
  // pass. Lit again for the hour instead: its surface's facing from the
  // depth (each way, the nearer side's: not across an outline), the painted
  // light taken as the mountain's sun and sky, and the hour's put in their
  // place. Out: that factor, the depth negated (the add multiplies).
  // Not lit by the lighting pass: where its stencil test fails (as the
  // lighting will find it); without one, where there's no normal (the
  // clear: not a normal, its alpha whole - the game's material 3 with our
  // tag 7, which no surface is given; an edge's average isn't).
  vec4 stt = texelFetch(relight, ivec2(8, 0), 0);
  bool unlit;
  if (stt.w > 0.5) {
    uint sv = texelFetch(stencil, ip, 0).r & 0xFFu;
    uint ref = uint(stt.x) & 0xFFu, msk = uint(stt.y) & 0xFFu, fn = uint(stt.z);
    uint a = ref & msk, b = sv & msk;
    bool pass = fn == 0u ? false : fn == 1u ? a < b : fn == 2u ? a == b : fn == 3u ? a <= b
              : fn == 4u ? a > b : fn == 5u ? a != b : fn == 6u ? a >= b : true;
    unlit = !pass;
  } else {
    unlit = abs(length(nw.xyz * 2.0 - 1.0) - 1.0) > 0.2 && nw.w > 254.5 / 255.0;
  }
  if (unlit) {
    vec4 r1 = texelFetch(relight, ivec2(5, 0), 0);
    if (r1.w < 0.5) {
      o = vec4(0.0);
      return;
    }
    vec4 r0 = texelFetch(relight, ivec2(4, 0), 0);
    vec4 r2 = texelFetch(relight, ivec2(6, 0), 0);
    vec4 r3 = texelFetch(relight, ivec2(7, 0), 0);
    vec3 side[4];
    float dist[4];
    const ivec2 kOff[4] = ivec2[4](ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2));
    for (int k = 0; k < 4; ++k) {
      ivec2 q = clamp(ip + kOff[k], ivec2(0), sz - 1);
      float dq = texelFetch(depth, q, 0).r;
      vec4 wq = pc.m * vec4((vec2(q) + 0.5) / vec2(sz) * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - max(dq, 1e-7), 1.0);
      side[k] = wq.xyz / wq.w - P;
      dist[k] = dq <= 0.0 ? 1e9 : abs(dot(side[k], V));
    }
    vec3 dx = dist[0] < dist[1] ? side[0] : -side[1];
    vec3 dy = dist[2] < dist[3] ? side[2] : -side[3];
    vec3 Nb = cross(dx, dy);
    float nlen = length(Nb);
    Nb = nlen > 1e-6 ? Nb / nlen : V;
    if (dot(Nb, V) < 0.0) Nb = -Nb;
    float amb = r0.w;
    float e0 = amb + (1.0 - amb) * max(dot(Nb, r0.xyz), 0.0);
    // (The hour's sun through the mask: the clouds' shadows, the far
    // shadows; not at the mountain's own hour, its painted shade is in.)
    float sh = pc.cam.w > 0.5 ? mix(1.0, texelFetch(mask, ip, 0).r, r2.w) : 1.0;
    vec3 e1 = amb * r1.rgb + (1.0 - amb) * r2.rgb * max(dot(Nb, r3.xyz), 0.0) * sh;
    o = vec4(clamp(e1 / max(e0, 0.25), vec3(0.0), vec3(4.0)), -d);
    return;
  }
  if (texelFetch(relight, ivec2(7, 0), 0).w < 0.5) {
    // (The sheen's off: the backdrop only; the depth kept for the bounce.)
    o = texelFetch(relight, ivec2(9, 0), 0).x > 0.5 ? vec4(0.0, 0.0, 0.0, d) : vec4(0.0);
    return;
  }
  vec3 N = normalize(nw.xyz * 2.0 - 1.0);
  vec3 L = pc.L.xyz;
  float nl = dot(N, L), nv = dot(N, V);
  if (nv <= 0.0) {
    o = vec4(0.0, 0.0, 0.0, d);
    return;
  }
  float lit = pc.cam.w > 0.5 ? texelFetch(mask, ip, 0).r : 1.0;
  if (nl <= 0.0) lit = 0.0;  // (facing away: the sky's reflection only)
  nl = max(nl, 1e-4);
  vec3 alb = texelFetch(albedo, ip, 0).rgb;
  float lum = dot(alb, vec3(0.3, 0.59, 0.11));
  float sat = max(alb.r, max(alb.g, alb.b)) - min(alb.r, min(alb.g, alb.b));
  // The normals' alpha: the game's material in its low bits (3: foliage;
  // it reads them mod 4), our surface tag in the top three.
  uint mb = uint(nw.w * 255.0 + 0.5);
  bool foliage = (mb & 3u) == 3u;
  uint tag = mb >> 5u;
  // Snow: tagged, or by its color (fresh snow over anything).
  float snow = foliage ? 0.0 : max(tag == 1u ? 1.0 : 0.0, smoothstep(0.5, 0.7, lum) * smoothstep(0.2, 0.08, sat));
  // Roughness and reflectance by surface: snow glossier toward a low sun's
  // glare; metal (tinted by its color) and paint shinier; wood, rock and
  // cloth dull; foliage waxy, faint.
  float rough = 0.55, f0 = 0.04, metal = 0.0;
  if (foliage) { rough = 0.6; f0 = 0.03; }
  else if (tag == 2u) { rough = 0.32; f0 = 0.5; metal = 1.0; }
  else if (tag == 3u) { rough = 0.75; }
  else if (tag == 4u) { rough = 0.7; }
  else if (tag == 5u) { rough = 0.35; f0 = 0.05; }
  else if (tag == 6u) { rough = 0.9; f0 = 0.03; }
  // Snow: mostly a soft, wide sheen (not a gloss: it isn't plastic), its
  // sparkle the crystals' below.
  rough = mix(rough, 0.62, snow);
  f0 = mix(f0, 0.02, snow);
  metal *= 1.0 - snow;
  vec3 H = normalize(L + V);
  float nh = max(dot(N, H), 0.0), vh = max(dot(V, H), 0.0);
  float a = rough * rough;
  float fr = pow(1.0 - vh, 5.0);
  vec3 F = mix(vec3(f0 + (1.0 - f0) * fr), mix(alb, vec3(1.0), fr), metal);
  float spec = D_GGX(nh, a) * G_Smith(nv, nl, a) / (4.0 * nv * nl) * nl;
  spec = min(spec, 6.0);
  vec3 c = pc.col.rgb * F * spec * lit * (foliage ? 0.35 : 1.0);
  // The sky in it (what the game's ambient leaves out: a mirror's worth of
  // it), by how smooth: metal and paint show it, snow a faint sheen at a
  // glancing angle, the rough rest none. Below the horizon, the snow.
  if (pc.env.x > 0.0 && !foliage) {
    vec3 R = reflect(-V, N);
    vec3 zen = texelFetch(sky, ivec2(0, 0), 0).rgb, hor = texelFetch(sky, ivec2(1, 0), 0).rgb;
    vec3 env = R.y >= 0.0 ? mix(hor, zen, smoothstep(0.0, 0.6, R.y)) : hor * 0.8;
    float fv = pow(1.0 - nv, 5.0);
    float gloss = (1.0 - rough) * (1.0 - rough);
    vec3 Fe = mix(vec3(f0 + (max(1.0 - rough, f0) - f0) * fv), mix(alb, vec3(1.0), fv), metal);
    // (Out of the sun, less of it: in a shadow, under trees, indoors.)
    c += env * Fe * gloss * mix(0.5, 1.0, lit) * pc.env.x * mix(1.0, 0.5, snow);
  }
  // Snow toward a low sun: its glare, light scattered on through the
  // crystals (looking toward the sun across it), soft.
  if (snow > 0.0) {
    float toward = max(dot(-V, L), 0.0);
    c += pc.col.rgb * snow * lit * pow(toward, 6.0) * pow(1.0 - nv, 2.0) * 0.12;
  }
  // The crystals in the sunlit snow: fine, round, coming and going.
  if (snow > 0.0 && lit > 0.0) {
    vec2 uv1 = (gl_FragCoord.xy + vec2(1.0, 0.0)) / vec2(sz);
    vec4 w1 = pc.m * vec4(uv1.x * 2.0 - 1.0, 1.0 - uv1.y * 2.0, 1.0 - d, 1.0);
    float foot = length(w1.xyz / w1.w - P);
    c += pc.col.rgb * SnowGlint(P, N, H, foot) * snow * lit * nl * 6.0;
  }
  if (pc.L.w > 0.5) {
    // (Diagnostics: the surfaces: snow white, metal blue, wood brown, rock
    // grey, paint magenta, cloth green, foliage dark green, unknown black.)
    const vec3 kTag[7] = vec3[7](vec3(0.0), vec3(1.0), vec3(0.2, 0.4, 1.0), vec3(0.55, 0.3, 0.1),
                                 vec3(0.45), vec3(1.0, 0.1, 0.8), vec3(0.1, 0.9, 0.2));
    c = foliage ? vec3(0.0, 0.3, 0.0) : kTag[min(tag, 6u)];
  }
  o = vec4(c, d);
}
)", true);
  copy_fs_[61] = CompileBuiltin("builtin_sun_spec_ms", std::string(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D albedo;
layout(set = 0, binding = 1) uniform sampler2D normals;  // xyz * 2 - 1, w the game's material (foliage > 0)
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(set = 0, binding = 3) uniform sampler2D mask;     // the sun's shadows (1 lit)
// 12 x 1: 0-3 the hour's sky (zenith, horizon), 4-7 the backdrop's light,
// 8 the lighting's stencil test (SunSpecular).
layout(set = 0, binding = 4) uniform sampler2D sky;
layout(set = 0, binding = 5) uniform usampler2DMS stencil;  // the lighting's depth target's
#define relight sky
layout(push_constant) uniform Spec {
  mat4 m;    // ndc (x, y, 1 - depth, 1) -> world
  vec4 L;    // toward the light, -
  vec4 col;  // its color (exposed), -
  vec4 cam;  // camera, mask on (0/1)
  vec4 env;  // the sky's reflection: strength (exposure; 0 none), -
} pc;
layout(location = 0) out vec4 o;
// (An integer hash: a cell's own, far from the origin too.)
float Hash3(ivec3 c, int k) {
  uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ uint(k) * 2654435761u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return float(h & 0xFFFFFFu) / 16777216.0;
}
)") + kSnowGlint + R"(float D_GGX(float nh, float a) {
  float a2 = a * a;
  float dd = nh * nh * (a2 - 1.0) + 1.0;
  return a2 / (3.14159 * dd * dd);
}
float G_Smith(float nv, float nl, float a) {
  float k = a * 0.5;
  return (nv / (nv * (1.0 - k) + k)) * (nl / (nl * (1.0 - k) + k));
}
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  ivec2 sz = textureSize(depth, 0);
  float d = texelFetch(depth, ip, 0).r;
  if (d <= 0.0) {
    o = vec4(0.0);
    return;
  }
  vec2 uv = (gl_FragCoord.xy) / vec2(sz);
  vec4 wp = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  vec3 P = wp.xyz / wp.w;
  vec3 V = normalize(pc.cam.xyz - P);
  vec4 nw = texelFetch(normals, ip, 0);
  // No normal there (the lighting's clear): the far-off scenery beyond the
  // slopes, drawn with its light painted in and never lit by the lighting
  // pass. Lit again for the hour instead: its surface's facing from the
  // depth (each way, the nearer side's: not across an outline), the painted
  // light taken as the mountain's sun and sky, and the hour's put in their
  // place. Out: that factor, the depth negated (the add multiplies).
  // Not lit by the lighting pass: where its stencil test fails (as the
  // lighting will find it); without one, where there's no normal (the
  // clear: not a normal, its alpha whole - the game's material 3 with our
  // tag 7, which no surface is given; an edge's average isn't).
  vec4 stt = texelFetch(relight, ivec2(8, 0), 0);
  bool unlit;
  if (stt.w > 0.5) {
    uint sv = texelFetch(stencil, ip, 0).r & 0xFFu;
    uint ref = uint(stt.x) & 0xFFu, msk = uint(stt.y) & 0xFFu, fn = uint(stt.z);
    uint a = ref & msk, b = sv & msk;
    bool pass = fn == 0u ? false : fn == 1u ? a < b : fn == 2u ? a == b : fn == 3u ? a <= b
              : fn == 4u ? a > b : fn == 5u ? a != b : fn == 6u ? a >= b : true;
    unlit = !pass;
  } else {
    unlit = abs(length(nw.xyz * 2.0 - 1.0) - 1.0) > 0.2 && nw.w > 254.5 / 255.0;
  }
  if (unlit) {
    vec4 r1 = texelFetch(relight, ivec2(5, 0), 0);
    if (r1.w < 0.5) {
      o = vec4(0.0);
      return;
    }
    vec4 r0 = texelFetch(relight, ivec2(4, 0), 0);
    vec4 r2 = texelFetch(relight, ivec2(6, 0), 0);
    vec4 r3 = texelFetch(relight, ivec2(7, 0), 0);
    vec3 side[4];
    float dist[4];
    const ivec2 kOff[4] = ivec2[4](ivec2(2, 0), ivec2(-2, 0), ivec2(0, 2), ivec2(0, -2));
    for (int k = 0; k < 4; ++k) {
      ivec2 q = clamp(ip + kOff[k], ivec2(0), sz - 1);
      float dq = texelFetch(depth, q, 0).r;
      vec4 wq = pc.m * vec4((vec2(q) + 0.5) / vec2(sz) * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - max(dq, 1e-7), 1.0);
      side[k] = wq.xyz / wq.w - P;
      dist[k] = dq <= 0.0 ? 1e9 : abs(dot(side[k], V));
    }
    vec3 dx = dist[0] < dist[1] ? side[0] : -side[1];
    vec3 dy = dist[2] < dist[3] ? side[2] : -side[3];
    vec3 Nb = cross(dx, dy);
    float nlen = length(Nb);
    Nb = nlen > 1e-6 ? Nb / nlen : V;
    if (dot(Nb, V) < 0.0) Nb = -Nb;
    float amb = r0.w;
    float e0 = amb + (1.0 - amb) * max(dot(Nb, r0.xyz), 0.0);
    // (The hour's sun through the mask: the clouds' shadows, the far
    // shadows; not at the mountain's own hour, its painted shade is in.)
    float sh = pc.cam.w > 0.5 ? mix(1.0, texelFetch(mask, ip, 0).r, r2.w) : 1.0;
    vec3 e1 = amb * r1.rgb + (1.0 - amb) * r2.rgb * max(dot(Nb, r3.xyz), 0.0) * sh;
    o = vec4(clamp(e1 / max(e0, 0.25), vec3(0.0), vec3(4.0)), -d);
    return;
  }
  if (texelFetch(relight, ivec2(7, 0), 0).w < 0.5) {
    // (The sheen's off: the backdrop only; the depth kept for the bounce.)
    o = texelFetch(relight, ivec2(9, 0), 0).x > 0.5 ? vec4(0.0, 0.0, 0.0, d) : vec4(0.0);
    return;
  }
  vec3 N = normalize(nw.xyz * 2.0 - 1.0);
  vec3 L = pc.L.xyz;
  float nl = dot(N, L), nv = dot(N, V);
  if (nv <= 0.0) {
    o = vec4(0.0, 0.0, 0.0, d);
    return;
  }
  float lit = pc.cam.w > 0.5 ? texelFetch(mask, ip, 0).r : 1.0;
  if (nl <= 0.0) lit = 0.0;  // (facing away: the sky's reflection only)
  nl = max(nl, 1e-4);
  vec3 alb = texelFetch(albedo, ip, 0).rgb;
  float lum = dot(alb, vec3(0.3, 0.59, 0.11));
  float sat = max(alb.r, max(alb.g, alb.b)) - min(alb.r, min(alb.g, alb.b));
  // The normals' alpha: the game's material in its low bits (3: foliage;
  // it reads them mod 4), our surface tag in the top three.
  uint mb = uint(nw.w * 255.0 + 0.5);
  bool foliage = (mb & 3u) == 3u;
  uint tag = mb >> 5u;
  // Snow: tagged, or by its color (fresh snow over anything).
  float snow = foliage ? 0.0 : max(tag == 1u ? 1.0 : 0.0, smoothstep(0.5, 0.7, lum) * smoothstep(0.2, 0.08, sat));
  // Roughness and reflectance by surface: snow glossier toward a low sun's
  // glare; metal (tinted by its color) and paint shinier; wood, rock and
  // cloth dull; foliage waxy, faint.
  float rough = 0.55, f0 = 0.04, metal = 0.0;
  if (foliage) { rough = 0.6; f0 = 0.03; }
  else if (tag == 2u) { rough = 0.32; f0 = 0.5; metal = 1.0; }
  else if (tag == 3u) { rough = 0.75; }
  else if (tag == 4u) { rough = 0.7; }
  else if (tag == 5u) { rough = 0.35; f0 = 0.05; }
  else if (tag == 6u) { rough = 0.9; f0 = 0.03; }
  // Snow: mostly a soft, wide sheen (not a gloss: it isn't plastic), its
  // sparkle the crystals' below.
  rough = mix(rough, 0.62, snow);
  f0 = mix(f0, 0.02, snow);
  metal *= 1.0 - snow;
  vec3 H = normalize(L + V);
  float nh = max(dot(N, H), 0.0), vh = max(dot(V, H), 0.0);
  float a = rough * rough;
  float fr = pow(1.0 - vh, 5.0);
  vec3 F = mix(vec3(f0 + (1.0 - f0) * fr), mix(alb, vec3(1.0), fr), metal);
  float spec = D_GGX(nh, a) * G_Smith(nv, nl, a) / (4.0 * nv * nl) * nl;
  spec = min(spec, 6.0);
  vec3 c = pc.col.rgb * F * spec * lit * (foliage ? 0.35 : 1.0);
  // The sky in it (what the game's ambient leaves out: a mirror's worth of
  // it), by how smooth: metal and paint show it, snow a faint sheen at a
  // glancing angle, the rough rest none. Below the horizon, the snow.
  if (pc.env.x > 0.0 && !foliage) {
    vec3 R = reflect(-V, N);
    vec3 zen = texelFetch(sky, ivec2(0, 0), 0).rgb, hor = texelFetch(sky, ivec2(1, 0), 0).rgb;
    vec3 env = R.y >= 0.0 ? mix(hor, zen, smoothstep(0.0, 0.6, R.y)) : hor * 0.8;
    float fv = pow(1.0 - nv, 5.0);
    float gloss = (1.0 - rough) * (1.0 - rough);
    vec3 Fe = mix(vec3(f0 + (max(1.0 - rough, f0) - f0) * fv), mix(alb, vec3(1.0), fv), metal);
    // (Out of the sun, less of it: in a shadow, under trees, indoors.)
    c += env * Fe * gloss * mix(0.5, 1.0, lit) * pc.env.x * mix(1.0, 0.5, snow);
  }
  // Snow toward a low sun: its glare, light scattered on through the
  // crystals (looking toward the sun across it), soft.
  if (snow > 0.0) {
    float toward = max(dot(-V, L), 0.0);
    c += pc.col.rgb * snow * lit * pow(toward, 6.0) * pow(1.0 - nv, 2.0) * 0.12;
  }
  // The crystals in the sunlit snow: fine, round, coming and going.
  if (snow > 0.0 && lit > 0.0) {
    vec2 uv1 = (gl_FragCoord.xy + vec2(1.0, 0.0)) / vec2(sz);
    vec4 w1 = pc.m * vec4(uv1.x * 2.0 - 1.0, 1.0 - uv1.y * 2.0, 1.0 - d, 1.0);
    float foot = length(w1.xyz / w1.w - P);
    c += pc.col.rgb * SnowGlint(P, N, H, foot) * snow * lit * nl * 6.0;
  }
  if (pc.L.w > 0.5) {
    // (Diagnostics: the surfaces: snow white, metal blue, wood brown, rock
    // grey, paint magenta, cloth green, foliage dark green, unknown black.)
    const vec3 kTag[7] = vec3[7](vec3(0.0), vec3(1.0), vec3(0.2, 0.4, 1.0), vec3(0.55, 0.3, 0.1),
                                 vec3(0.45), vec3(1.0, 0.1, 0.8), vec3(0.1, 0.9, 0.2));
    c = foliage ? vec3(0.0, 0.3, 0.0) : kTag[min(tag, 6u)];
  }
  o = vec4(c, d);
}
)", true);
  // The sun's sheen, 2: added to the scene where the depth still matches
  // (not over what was drawn after the lighting: riders, effects).
  // Light bounce (half size): the light from the lit surfaces round each
  // point (as the scene shows them: the sun's, the sky's, the lamps') onto
  // it - each of a spiral of points a few metres out, seen through the depth
  // on screen, facing it and in front of its surface, by how squarely they
  // face each other and how near. Its depth kept in alpha (the upsizing).
  copy_fs_[62] = CompileBuiltin("builtin_bounce", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;    // lit (linear)
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D normals;  // the lighting's
layout(push_constant) uniform Bounce {
  mat4 m;    // ndc (x, y, 1 - depth, 1) -> world
  vec4 cam;  // camera, radius (m)
  vec4 a;    // 1 / width, 1 / height (full size), pixels a metre 1 m away, farthest
  vec4 b;    // the frame (a turn of the spiral), largest radius (pixels), -, -
} pc;
layout(location = 0) out vec4 o;
ivec2 kSize;
vec3 World(ivec2 t, out float d) {
  d = texelFetch(depth, t, 0).r;
  vec2 uv = (vec2(t) + 0.5) * pc.a.xy;
  vec4 p = pc.m * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - max(d, 1e-7), 1.0);
  return p.xyz / p.w;
}
vec3 Normal(ivec2 t, out bool ok) {
  vec3 n = texelFetch(normals, t, 0).xyz * 2.0 - 1.0;
  float l = length(n);
  ok = abs(l - 1.0) < 0.2;
  return ok ? n / l : vec3(0.0, 1.0, 0.0);
}
void main() {
  kSize = textureSize(depth, 0);
  ivec2 c = min(ivec2(gl_FragCoord.xy) * 2, kSize - 1);
  float d;
  vec3 P = World(c, d);
  o = vec4(0.0, 0.0, 0.0, d);
  if (d <= 0.0) return;
  float dist = length(P - pc.cam.xyz);
  if (dist > pc.a.w) return;
  bool ok;
  vec3 N = Normal(c, ok);
  if (!ok) return;
  float R = pc.cam.w;
  float rpx = min(R * pc.a.z / dist, pc.b.y);
  if (rpx < 3.0) return;
  float spin = fract(52.9829189 * fract(dot(gl_FragCoord.xy + pc.b.x * vec2(5.588, 3.142), vec2(0.06711056, 0.00583715))));
  const int kTaps = 12;
  vec3 sum = vec3(0.0);
  for (int i = 0; i < kTaps; ++i) {
    float f = (float(i) + 0.5) / float(kTaps);
    float ang = (f * 7.0 + spin) * 6.2831853;
    // (Out to the radius, more of them farther: the area grows.)
    vec2 off = vec2(cos(ang), sin(ang)) * (sqrt(f) * rpx);
    ivec2 t = clamp(c + ivec2(round(off)), ivec2(0), kSize - 1);
    float dq;
    vec3 Q = World(t, dq);
    if (dq <= 0.0) continue;
    vec3 D = Q - P;
    float dl = length(D);
    if (dl < 0.05 || dl > R * 1.6) continue;
    vec3 w = D / dl;
    float cr = dot(N, w);           // facing it, here
    if (cr <= 0.08) continue;
    bool okq;
    vec3 Nq = Normal(t, okq);
    float ce = okq ? dot(Nq, -w) : 0.5;  // and it, here
    if (ce <= 0.0) continue;
    vec3 rad = textureLod(scene, (vec2(t) + 0.5) * pc.a.xy, 0.0).rgb;
    // (Fading out toward the radius, and not too much from right beside.)
    float fall = (1.0 - smoothstep(R * 0.6, R * 1.6, dl)) * smoothstep(0.0, 0.5, dl);
    sum += rad * cr * ce * fall;
  }
  // (Six over the taps: a wall beside a sunlit field gets about half its
  // light, a gentle slope beside it a little.)
  vec3 e = sum * (6.0 / float(kTaps));
  e *= 1.0 - smoothstep(pc.a.w * 0.6, pc.a.w, dist);
  o = vec4(e, d);
}
)", true);
  // World-space light, 1: a cascade's occupancy (see gi_): each texel a
  // column of voxels along y (128 bits), from the nearest surfaces seen
  // from above and from either side along x and z (each map twice the
  // cascade's size: four samples a voxel face), merged with the occupancy
  // before, moved to this box.
  copy_fs_[63] = CompileBuiltin("builtin_gi_occupancy", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D top;  // from above: u x, v z; depth down from the top
layout(set = 0, binding = 1) uniform sampler2D xp;   // from +x: u z, v y; depth in from the +x side
layout(set = 0, binding = 2) uniform sampler2D xn;   // from -x
layout(set = 0, binding = 3) uniform sampler2D zp;   // from +z: u x, v y
layout(set = 0, binding = 4) uniform sampler2D zn;   // from -z
layout(set = 0, binding = 5) uniform usampler2D old; // the occupancy before
layout(push_constant) uniform Occ {
  vec4 a;  // voxels a side, map texels a side, this box's origin minus the old one's (x, y)
  vec4 b;  // (z), the old one kept (0/1)
} pc;
layout(location = 0) out uvec4 o;
void Set(inout uvec4 b, int y) { b[y >> 5] |= 1u << uint(y & 31); }
void main() {
  int N = int(pc.a.x);
  int R = int(pc.a.y) / N;
  ivec2 c = ivec2(gl_FragCoord.xy);
  int x = c.x, z = c.y;
  uvec4 bits = uvec4(0u);
  for (int j = 0; j < R; ++j) {
    for (int i = 0; i < R; ++i) {
      float d = texelFetch(top, ivec2(x * R + i, z * R + j), 0).r;
      if (d < 1.0) Set(bits, clamp(int((1.0 - d) * float(N)), 0, N - 1));
    }
  }
  for (int y = 0; y < N; ++y) {
    bool hit = false;
    for (int j = 0; j < R && !hit; ++j) {
      for (int i = 0; i < R && !hit; ++i) {
        float d = texelFetch(xp, ivec2(z * R + i, y * R + j), 0).r;
        hit = hit || (d < 1.0 && int((1.0 - d) * float(N)) == x);
        d = texelFetch(xn, ivec2(z * R + i, y * R + j), 0).r;
        hit = hit || (d < 1.0 && int(d * float(N)) == x);
        d = texelFetch(zp, ivec2(x * R + i, y * R + j), 0).r;
        hit = hit || (d < 1.0 && int((1.0 - d) * float(N)) == z);
        d = texelFetch(zn, ivec2(x * R + i, y * R + j), 0).r;
        hit = hit || (d < 1.0 && int(d * float(N)) == z);
      }
    }
    if (hit) Set(bits, y);
  }
  if (pc.b.y > 0.5) {
    ivec3 sh = ivec3(int(pc.a.z), int(pc.a.w), int(pc.b.x));
    ivec2 oc = ivec2(x, z) + sh.xz;
    if (all(greaterThanEqual(oc, ivec2(0))) && all(lessThan(oc, ivec2(N)))) {
      uvec4 ob = texelFetch(old, oc, 0);
      for (int y = 0; y < N; ++y) {
        int yo = y + sh.y;
        if (yo >= 0 && yo < N && ((ob[yo >> 5] >> uint(yo & 31)) & 1u) != 0u) Set(bits, y);
      }
    }
  }
  o = bits;
}
)", true);
  // World-space light, 2 (half size): see GiTrace. Out: the light to add
  // (times the albedo; negative where the sky's is taken off), its depth.
  static const std::string kGiCommon = R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;    // lit (linear)
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D b2;  // trace: the lighting's normals; probes: the probes before
layout(set = 0, binding = 3) uniform usampler2D occ0;    // 1 m voxels
layout(set = 0, binding = 4) uniform usampler2D occ1;    // 4 m
layout(set = 0, binding = 5) uniform sampler2D params;
layout(push_constant) uniform Gi { vec4 a; vec4 b; } pc;  // 1 / width, 1 / height (full size), voxels a side, pixels a texel
layout(location = 0) out vec4 o;
mat4 M, VP;
vec4 kBox[2], kSun, kCam, kSh[7], kSet;
int N;
ivec2 kSize;
vec4 Param(int i) { return texelFetch(params, ivec2(i, 0), 0); }
bool Occ(int k, ivec3 v) {
  if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, ivec3(N)))) return false;
  uvec4 c = k == 0 ? texelFetch(occ0, v.xz, 0) : texelFetch(occ1, v.xz, 0);
  return ((c[v.y >> 5] >> uint(v.y & 31)) & 1u) != 0u;
}
uvec4 k0Col(ivec2 c) { return texelFetch(occ0, c, 0); }
// The sky's light from (or onto a surface facing) n: the lighting's own.
vec3 Sky(vec3 n) {
  vec4 n4 = vec4(n, 1.0);
  vec4 q = n.xyzz * n.yzzx;
  vec3 r = vec3(dot(kSh[0], n4), dot(kSh[1], n4), dot(kSh[2], n4)) +
           vec3(dot(kSh[3], q), dot(kSh[4], q), dot(kSh[5], q)) + kSh[6].xyz * (n.x * n.x - n.y * n.y);
  return max(r, vec3(0.0));
}
// Along P + t D through the cascades (the fine one, then on in the coarse
// one; from k0), at most tmax and budget[k] voxels in each: t of the first
// occupied voxel past tmin (-1: none), the face it was entered by. A
// column's 32 voxels of y that are all empty are crossed in one step (to
// where the ray leaves them, or leaves the column).
float Trace(vec3 P, vec3 D, float tmin, float tmax, int k0, ivec2 budget, out vec3 n) {
  n = vec3(0.0, 1.0, 0.0);
  vec3 Ds = D + vec3(equal(D, vec3(0.0))) * 1e-6;
  vec3 inv = 1.0 / Ds;
  ivec3 st = ivec3(sign(Ds));
  float t = 0.0;
  for (int k = k0; k < 2; ++k) {
    float vs = kBox[k].w;
    vec3 B = kBox[k].xyz;
    float S = float(N) * vs;
    vec3 t0 = (B - P) * inv, t1 = (B + vec3(S) - P) * inv;
    vec3 lo = min(t0, t1), hi = max(t0, t1);
    float tin = max(max(max(lo.x, lo.y), lo.z), t);
    float tout = min(min(min(hi.x, hi.y), hi.z), tmax);
    if (tout <= tin) continue;
    vec3 p = (P + D * (tin + 1e-3) - B) / vs;
    ivec3 v = clamp(ivec3(floor(p)), ivec3(0), ivec3(N - 1));
    vec3 tDelta = abs(vec3(vs) * inv);
    vec3 tMax = ((vec3(v) + max(vec3(st), vec3(0.0))) * vs + B - P) * inv;
    int axis = -1;
    float tc = tin;
    ivec2 col = ivec2(-1);
    uvec4 cw = uvec4(0u);
    for (int i = 0; i < budget[k]; ++i) {
      if (v.xz != col) {
        col = v.xz;
        cw = k == 0 ? texelFetch(occ0, col, 0) : texelFetch(occ1, col, 0);
      }
      uint w = cw[v.y >> 5];
      if (w == 0u) {
        // (Empty to the end of these 32: on to where the ray leaves them or
        // the column, whichever first.)
        float ty = ((st.y > 0 ? float((v.y | 31) + 1) : float(v.y & ~31)) * vs + B.y - P.y) * inv.y;
        float txz = min(tMax.x, tMax.z);
        if (ty < txz) {
          tc = ty;
          if (tc > tout) break;
          v.y = st.y > 0 ? (v.y | 31) + 1 : (v.y & ~31) - 1;
          if (v.y < 0 || v.y >= N) break;
          tMax.y = ty + tDelta.y;
          axis = 1;
          continue;
        }
        axis = tMax.x < tMax.z ? 0 : 2;
        tc = tMax[axis];
        if (tc > tout) break;
        v[axis] += st[axis];
        tMax[axis] += tDelta[axis];
        // (Where y is at that point.)
        v.y = clamp(int(floor((P.y + D.y * tc - B.y) / vs)), 0, N - 1);
        tMax.y = ((float(v.y) + (st.y > 0 ? 1.0 : 0.0)) * vs + B.y - P.y) * inv.y;
        if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, ivec3(N)))) break;
        continue;
      }
      if (((w >> uint(v.y & 31)) & 1u) != 0u && tc >= tmin) {
        if (axis >= 0) {
          n = vec3(0.0);
          n[axis] = -float(st[axis]);
        } else {
          n = -D;
        }
        return tc;
      }
      axis = tMax.x < tMax.y ? (tMax.x < tMax.z ? 0 : 2) : (tMax.y < tMax.z ? 1 : 2);
      tc = tMax[axis];
      if (tc > tout) break;
      v[axis] += st[axis];
      tMax[axis] += tDelta[axis];
      if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, ivec3(N)))) break;
    }
    t = tout;
    if (t >= tmax) break;
  }
  return -1.0;
}
vec3 World(vec2 uv, float d) {
  vec4 p = M * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - max(d, 1e-7), 1.0);
  return p.xyz / p.w;
}
// The light leaving a hit point toward the ray: as lit on screen, if it
// shows there; else worked out.
vec3 Radiance(vec3 H, vec3 n, float vs) {
  vec4 cl = VP * vec4(H, 1.0);
  if (cl.w > 0.0) {
    vec3 ndc = cl.xyz / cl.w;
    vec2 uv = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
    if (all(greaterThan(uv, vec2(0.0))) && all(lessThan(uv, vec2(1.0)))) {
      float ds = texelFetch(depth, ivec2(uv * vec2(kSize)), 0).r;
      if (ds > 0.0 && length(World(uv, ds) - H) < 1.5 * vs) return textureLod(scene, uv, 0.0).rgb;
    }
  }
  // (Facing up: snow; else darker: rock, wood, trees.)
  float alb = mix(0.3, 0.75, smoothstep(0.3, 0.8, n.y));
  vec3 e = Sky(n);
  // The night lights near it (no shadows: what's in the way left out).
  int lamps = int(Param(20).z);
  for (int i = 0; i < lamps; ++i) {
    vec4 lp = Param(26 + 2 * i);
    vec3 L = lp.xyz - H;
    float d = length(L);
    if (d >= lp.w) continue;
    float fall = 1.0 - d / lp.w;
    e += Param(27 + 2 * i).rgb * max(dot(n, L / max(d, 1e-3)), 0.0) * fall * fall;
  }
  float nl = dot(n, kSun.xyz);
  if (nl > 0.0) {
    vec3 nn;
    if (Trace(H + n * kBox[1].w * 1.01, kSun.xyz, 0.0, 300.0, 1, ivec2(0, 24), nn) < 0.0) e += vec3(kSun.w * nl);
  }
  return alb * e;
}
)";
  copy_fs_[64] = CompileBuiltin("builtin_gi_trace", kGiCommon + R"(void main() {
  N = int(pc.a.z);
  kSize = textureSize(depth, 0);
  for (int i = 0; i < 4; ++i) {
    M[i] = Param(i);
    VP[i] = Param(4 + i);
  }
  kBox[0] = Param(8);
  kBox[1] = Param(9);
  kSun = Param(10);
  kCam = Param(11);
  for (int i = 0; i < 7; ++i) kSh[i] = Param(12 + i);
  kSet = Param(19);
  int sc = int(pc.a.w);
  ivec2 c = min(ivec2(gl_FragCoord.xy) * sc + sc / 2, kSize - 1);
  float d = texelFetch(depth, c, 0).r;
  o = vec4(0.0, 0.0, 0.0, d);
  vec2 uv = (vec2(c) + 0.5) * pc.a.xy;
  if (Param(20).x > 0.5) {
    // (Diagnostics, "giview": the voxels seen from the camera, by the face
    // hit and how far; through gishow's 0.5 + 1.5 x.)
    vec3 Pf = World(uv, 0.5);
    vec3 D = normalize(Pf - kCam.xyz);
    vec3 n;
    float t = Trace(kCam.xyz, D, 0.0, 600.0, 0, ivec2(400, 400), n);
    vec3 col = t < 0.0 ? vec3(0.2, 0.2, 0.5) : (n * 0.5 + 0.5) * (1.0 - 0.7 * clamp(t / 300.0, 0.0, 1.0));
    o = vec4((col - 0.5) / 1.5, d);
    return;
  }
  if (d <= 0.0) return;
  // (Half the pixels a frame, a checkerboard turned each frame: the others
  // from the frames before.)
  if (Param(20).y > 0.5 && ((int(gl_FragCoord.x) + int(gl_FragCoord.y) + int(kSet.w)) & 1) != 0) {
    o = vec4(0.0, 0.0, 1.0, -d);
    return;
  }
  vec3 P = World(uv, d);
  float dist = length(P - kCam.xyz);
  if (dist > kCam.w) return;
  vec3 Nn = texelFetch(b2, c, 0).xyz * 2.0 - 1.0;
  float nl = length(Nn);
  if (abs(nl - 1.0) > 0.2) return;
  Nn /= nl;
  // (Off its own surface by a voxel and a half of the cascade it's in.)
  vec3 rel = P - kBox[0].xyz;
  bool fine = all(greaterThanEqual(rel, vec3(0.0))) && all(lessThan(rel, vec3(float(N) * kBox[0].w)));
  float vs = fine ? kBox[0].w : kBox[1].w;
  vec3 O = P + Nn * vs * 2.0;
  vec3 T = abs(Nn.y) < 0.9 ? normalize(cross(Nn, vec3(0.0, 1.0, 0.0))) : normalize(cross(Nn, vec3(1.0, 0.0, 0.0)));
  vec3 Bt = cross(Nn, T);
  float rnd = fract(52.9829189 * fract(dot(gl_FragCoord.xy + kSet.w * vec2(5.588, 3.142), vec2(0.06711056, 0.00583715))));
  int rays = int(kSet.x);
  vec3 sum = vec3(0.0);
  for (int i = 0; i < rays; ++i) {
    // Cosine-weighted round the normal (a spiral, turned per pixel).
    float u1 = (float(i) + 0.5) / float(rays);
    float ph = 6.2831853 * fract(float(i) * 0.618034 + rnd);
    float r = sqrt(u1);
    vec3 D = normalize(T * (r * cos(ph)) + Bt * (r * sin(ph)) + Nn * sqrt(max(1.0 - u1, 0.0)));
    vec3 n;
    // (Not the surface it starts from: the voxels' steps along a slope.)
    float t = Trace(O, D, vs * 2.0, 400.0, 0, ivec2(80, 48), n);
    if (t < 0.0) continue;
    vec3 H = O + D * t;
    float hv = t < float(N) * kBox[0].w * 0.5 ? kBox[0].w : kBox[1].w;
    sum += Radiance(H, n, hv) * kSet.z - Sky(D) * kSet.y;
  }
  vec3 e = sum / float(rays);
  e *= 1.0 - smoothstep(kCam.w * 0.7, kCam.w, dist);
  o = vec4(e, d);
}
)", true);
  copy_fs_[67] = CompileBuiltin("builtin_gi_probes", kGiCommon + R"(// The probe grid (see GiProbes): each texel one face (+x -x +y -y +z -z)
// of a probe, the light onto a surface facing that way there, traced again
// for an eighth of them a frame and blended into what it had.
void main() {
  N = int(pc.a.z);
  kSize = textureSize(depth, 0);
  for (int i = 0; i < 4; ++i) {
    M[i] = Param(i);
    VP[i] = Param(4 + i);
  }
  kBox[0] = Param(8);
  kBox[1] = Param(9);
  kSun = Param(10);
  kCam = Param(11);
  for (int i = 0; i < 7; ++i) kSh[i] = Param(12 + i);
  kSet = Param(19);
  ivec3 dims = ivec3(Param(22).xyz);
  int frame = int(Param(22).w);
  ivec2 t = ivec2(gl_FragCoord.xy);
  // (Rows past the coarse grid's: the fine one's.)
  bool fine = t.y >= dims.y * dims.z;
  vec4 g = fine ? Param(23) : Param(21);  // the grid's lowest probe (world index), spacing (m)
  int row = fine ? t.y - dims.y * dims.z : t.y;
  int face = t.x % 6;
  ivec3 slot = ivec3(t.x / 6, row / dims.z, row % dims.z);
  ivec3 lo = ivec3(g.xyz);
  ivec3 wi = lo + ((slot - lo) % dims + dims) % dims;
  // (Which probe this slot holds: tagged in alpha, so one scrolled in is
  // started afresh.)
  float tag = float((wi.x & 7) + 8 * (wi.y & 3) + 32 * (wi.z & 7)) + 1.0;
  vec4 old = texelFetch(b2, t, 0);
  bool same = abs(abs(old.a) - tag) < 0.25;
  // (Only this frame's band of rows is drawn: the rest copied as they were.
  // Picking the due ones per texel instead left every wave of the GPU's
  // waiting on its slowest, all of them busy every frame.)
  vec3 P = (vec3(wi) + 0.5) * g.w;
  vec3 Nn = vec3(0.0);
  Nn[face >> 1] = (face & 1) == 0 ? 1.0 : -1.0;
  // Inside something (its fine voxel occupied): no light of its own.
  vec3 rel = (P - kBox[0].xyz) / kBox[0].w;
  ivec3 v = ivec3(floor(rel));
  bool inside = all(greaterThanEqual(v, ivec3(0))) && all(lessThan(v, ivec3(N))) &&
                (((k0Col(v.xz)[v.y >> 5]) >> uint(v.y & 31)) & 1u) != 0u;
  // Or under the ground: the voxels are only the surfaces, so beneath the
  // snow is empty too, and such a probe's rays meet the snow's underside
  // and take it for lit snow (bright spots on the ground above). Told by
  // its column: something above it and nothing below.
  if (!inside && all(greaterThanEqual(v, ivec3(0))) && all(lessThan(v, ivec3(N)))) {
    uvec4 cw = k0Col(v.xz);
    bool below = false, above = false;
    for (int w = 0; w < 4; ++w) {
      int b0 = w * 32;
      uint m = cw[w];
      if (m == 0u) continue;
      uint lowm = v.y > b0 + 31 ? 0xFFFFFFFFu : v.y <= b0 ? 0u : ((1u << uint(v.y - b0)) - 1u);
      uint highm = v.y < b0 ? 0xFFFFFFFFu : v.y >= b0 + 31 ? 0u : ~((2u << uint(v.y - b0)) - 1u);
      below = below || (m & lowm) != 0u;
      above = above || (m & highm) != 0u;
    }
    inside = above && !below;
  }
  if (inside) {
    o = vec4(0.0, 0.0, 0.0, -tag);
    return;
  }
  vec3 T = abs(Nn.y) < 0.9 ? normalize(cross(Nn, vec3(0.0, 1.0, 0.0))) : vec3(1.0, 0.0, 0.0);
  vec3 Bt = cross(Nn, T);
  // (Each probe face's rays the same every time it's traced, turned its own
  // way: new rays each time made every probe's light wander a little at
  // each pass, and the shade on open snow swam and pulsed with the camera
  // still. Its neighbours' different rays average out in the reading.)
  float rnd = fract(52.9829189 * fract(dot(vec2(t), vec2(0.06711056, 0.00583715))));
  int rays = fine ? max(int(kSet.x) * 4 / 5, 4) : int(kSet.x);  // (the fine grid: 8 of 10)
  vec3 sum = vec3(0.0);
  for (int i = 0; i < rays; ++i) {
    float u1 = fract((float(i) + 0.5) / float(rays) + rnd * 0.37);
    float ph = 6.2831853 * fract(float(i) * 0.618034 + rnd);
    float r = sqrt(u1);
    vec3 D = normalize(T * (r * cos(ph)) + Bt * (r * sin(ph)) + Nn * sqrt(max(1.0 - u1, 0.0)));
    vec3 n;
    // (The fine grid's probes sit among things, near the ground: their rays
    // step voxel by voxel along it, about four times a coarse probe's work;
    // fewer steps for them.)
    float tt = Trace(P, D, 2.0 * kBox[0].w, 400.0, 0, fine ? ivec2(48, 32) : ivec2(80, 48), n);
    if (tt < 0.0) continue;
    vec3 H = P + D * tt;
    float hv = tt < float(N) * kBox[0].w * 0.5 ? kBox[0].w : kBox[1].w;
    sum += Radiance(H, n, hv) * kSet.z - Sky(D) * kSet.y;
  }
  vec3 e = sum / float(rays);
  // (Blended in still, for the light changing: the sun moving, the voxels
  // updated.)
  o = vec4(same && old.a > 0.0 ? mix(old.rgb, e, 0.2) : e, tag);
}
)", true);
  // World-space light, 3: its half-size result blurred one way (9 taps,
  // a.xy the step), only across what's at about the same depth.
  copy_fs_[65] = CompileBuiltin("builtin_gi_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;  // the light, its depth in alpha
layout(push_constant) uniform Blur { vec4 a; } pc;  // step (texels)
layout(location = 0) out vec4 o;
void main() {
  ivec2 sz = textureSize(src, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(src, p, 0);
  if (c.a <= 0.0) {
    o = c;
    return;
  }
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int i = -4; i <= 4; ++i) {
    vec4 b = texelFetch(src, clamp(p + ivec2(pc.a.xy * float(i)), ivec2(0), sz - 1), 0);
    if (b.a <= 0.0) continue;
    float w = exp(-float(i * i) / 10.0) * exp(-abs(b.a - c.a) / max(c.a * 0.02, 1e-6));
    sum += b.rgb * w;
    wsum += w;
  }
  o = vec4(wsum > 1e-5 ? sum / wsum : c.rgb, c.a);
}
)", true);
  // World-space light, 4: this frame's (half the pixels traced: the rest
  // marked, negative depth) blended into the history, which is reprojected:
  // each pixel's point (its depth in alpha) seen by last frame's camera,
  // taken where the history there has about that depth.
  copy_fs_[66] = CompileBuiltin("builtin_gi_temporal", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D cur;   // this frame's (quarter size)
layout(set = 0, binding = 1) uniform sampler2D hist;  // last frame's blended
layout(push_constant) uniform T {
  mat4 m;     // ndc (x, y, 1 - depth, 1) -> world (now)
  mat4 prev;  // world -> last frame's ndc (z: 1 - depth); [3][3] 0: no history
} pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 sz = textureSize(cur, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(cur, p, 0);
  bool traced = c.a >= 0.0;
  float d = abs(c.a);
  if (d <= 0.0) {
    o = vec4(0.0, 0.0, 0.0, 0.0);
    return;
  }
  // This one untraced: its traced neighbours (same depth) meanwhile.
  vec3 near = c.rgb;
  if (!traced) {
    vec3 sum = vec3(0.0);
    float ws = 0.0;
    for (int k = 0; k < 4; ++k) {
      ivec2 q = clamp(p + ivec2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0), ivec2(0), sz - 1);
      vec4 n = texelFetch(cur, q, 0);
      if (n.a <= 0.0) continue;
      float w = exp(-abs(n.a - d) / max(d * 0.02, 1e-6));
      sum += n.rgb * w;
      ws += w;
    }
    near = ws > 1e-4 ? sum / ws : vec3(0.0);
  }
  vec3 h = vec3(0.0);
  bool have = false;
  if (pc.prev[3][3] > 0.5) {
    vec2 uv = (vec2(p) + 0.5) / vec2(sz);
    vec4 w = pc.m * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - d, 1.0);
    vec3 P = w.xyz / w.w;
    vec4 q = pc.prev * vec4(P, 1.0);
    if (q.w > 0.0) {
      vec3 ndc = q.xyz / q.w;
      vec2 pu = vec2(ndc.x * 0.5 + 0.5, 0.5 - ndc.y * 0.5);
      if (all(greaterThan(pu, vec2(0.0))) && all(lessThan(pu, vec2(1.0)))) {
        vec4 hv = texelFetch(hist, ivec2(pu * vec2(sz)), 0);
        float dp = 1.0 - ndc.z;
        if (hv.a > 0.0 && abs(hv.a - dp) < dp * 0.03 + 1e-6) {
          h = hv.rgb;
          have = true;
        }
      }
    }
  }
  vec3 r = !have ? near : traced ? mix(h, c.rgb, 0.25) : h;
  o = vec4(r, d);
}
)", true);
  // World-space light, 7: the sun's shadows far off (past the sun maps)
  // from the voxels: toward the sun from each far point, through the coarse
  // cascade (4 m voxels, 512 m round the camera). What's off screen (a ridge
  // behind the camera, above the picture) shades too, which the march
  // through the depth on screen can't see; kept where the voxels reach, on
  // the mask before builtin_mask_finish (which marches the screen too).
  // Binding 0 (scene): the mask so far; 5 (params): the mask's settings
  // (builtin_mask_finish's), with 16, 17 the cascades and 18 strength,
  // frame.
  copy_fs_[70] = CompileBuiltin("builtin_gi_sun", kGiCommon + R"(void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  float lit = texelFetch(scene, ip, 0).r;
  o = vec4(lit);
  float d = texelFetch(depth, ip, 0).r;
  vec2 size = vec2(textureSize(depth, 0));
  vec2 uv = gl_FragCoord.xy / size;
  M = mat4(Param(0), Param(1), Param(2), Param(3));
  vec4 wp = M * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - max(d, 1e-7), 1.0);
  vec3 P = wp.xyz / (abs(wp.w) < 1e-9 ? 1.0 : wp.w);
  // (The surface's facing, from the depth: before anything branches.)
  vec3 Nf = cross(dFdx(P), dFdy(P));
  if (d <= 0.0 || abs(wp.w) < 1e-9 || lit < 0.02) return;
  N = int(pc.a.z);
  kBox[0] = Param(16);
  kBox[1] = Param(17);
  vec4 st = Param(18);  // strength, -, frame
  vec3 L = Param(7).xyz;
  vec4 f0 = Param(12), f1 = Param(13);  // camera, from (m); over (m)
  float k = st.w > 0.5 ? 1.0 : smoothstep(f0.w, f0.w + f1.x, length(P.xz - f0.xz));
  // (Diagnostic "gisunall": everywhere the voxels reach, not only far off.
  // Well inside the coarse cascade: fading out over its last voxels.)
  float vs = kBox[1].w;
  vec3 q = (P - kBox[1].xyz) / vs;
  vec3 m3 = min(q, vec3(float(N)) - q);
  float edge = smoothstep(2.0, 10.0, min(min(m3.x, m3.z), m3.y + 8.0));
  k *= edge * st.x;
  if (k <= 0.0) return;
  vec3 Nn = normalize(Nf);
  if (dot(Nn, P - f0.xyz) > 0.0) Nn = -Nn;  // (toward the camera)
  // (No skipping what faces away from the sun, dark by its light already:
  // the depth's facing is grainy, and single pixels of it left lit.)
  // Two rays, their directions jittered over a little of the sky round the
  // sun frame to frame (the mask's easing softens it into a penumbra; one
  // alone left single pixels of shade speckling the far slopes).
  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy + st.z * vec2(5.588, 3.142), vec2(0.06711056, 0.00583715))));
  vec3 T = normalize(cross(L, abs(L.y) < 0.9 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
  vec3 Bt = cross(L, T);
  // (From the fine cascade if it's in it (nearer than its edge by a few
  // voxels); off the surface by a voxel and more, the slope's own voxels
  // left.)
  vec3 q0 = (P - kBox[0].xyz) / kBox[0].w;
  int k0 = all(greaterThan(q0, vec3(4.0))) && all(lessThan(q0, vec3(float(N) - 4.0))) ? 0 : 1;
  float v0 = kBox[k0].w;
  vec3 S = P + Nn * v0 * 1.5;
  // (Toward a sun low over the slope the ray runs along the slope's own
  // voxels, a staircase it would meet: nothing counted nearer than a few
  // voxels, more the lower the sun is over it.)
  float nl = dot(Nn, L);
  float tmin = v0 * (2.0 + 1.5 / max(nl, 0.15));
  float shade = 0.0;
  for (int i = 0; i < 2; ++i) {
    float ph = 6.2831853 * fract(jit * 7.31 + st.z * 0.618034 + float(i) * 0.5);
    vec3 D = normalize(L + (T * cos(ph) + Bt * sin(ph)) * 0.015 * sqrt(fract(jit + float(i) * 0.5)));
    vec3 hn;
    if (Trace(S, D, tmin, 900.0, k0, ivec2(96, 140), hn) >= 0.0) shade += 0.5;
  }
  o = vec4(lit * (1.0 - k * shade));
  if (st.y > 0.5) o = vec4(0.5 - 0.5 * shade);  // (diagnostic "gisunview": where it reaches, what it shades)
}
)", true);
  // World-space light, 5: the probes read at each quarter-size pixel
  // (trilinear, the six faces by its normal, probes behind its surface
  // weighed less, those inside something or not yet traced left out).
  copy_fs_[68] = CompileBuiltin("builtin_gi_apply", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 0, binding = 1) uniform sampler2D normals;
layout(set = 0, binding = 2) uniform sampler2D probes;
layout(set = 0, binding = 5) uniform sampler2D params;
layout(push_constant) uniform A { vec4 a; } pc;  // 1 / width, 1 / height (full size), pixels a texel
layout(location = 0) out vec4 o;
vec4 Param(int i) { return texelFetch(params, ivec2(i, 0), 0); }
// The probes round P in one grid (its lowest probe and spacing g, its rows
// from row0): trilinear, for a surface facing Nn (each face by Nn squared,
// the probes it faces weighted up), or all six faces averaged (all6: a body
// facing every way). The weighted sum, the weights' total.
vec4 Gather(vec4 g, ivec3 dims, int row0, vec3 P, vec3 Nn, bool all6) {
  ivec3 lo = ivec3(g.xyz);
  vec3 q = P / g.w - 0.5;
  ivec3 base = ivec3(floor(q));
  vec3 f = q - vec3(base);
  vec3 n2 = Nn * Nn;
  ivec3 fc = ivec3(Nn.x >= 0.0 ? 0 : 1, Nn.y >= 0.0 ? 2 : 3, Nn.z >= 0.0 ? 4 : 5);
  vec3 sum = vec3(0.0);
  float ws = 0.0;
  for (int k = 0; k < 8; ++k) {
    ivec3 o3 = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
    ivec3 wi = base + o3;
    if (any(lessThan(wi, lo)) || any(greaterThanEqual(wi, lo + dims))) continue;
    ivec3 slot = ((wi % dims) + dims) % dims;
    ivec2 t0 = ivec2(slot.x * 6, row0 + slot.y * dims.z + slot.z);
    float tag = float((wi.x & 7) + 8 * (wi.y & 3) + 32 * (wi.z & 7)) + 1.0;
    vec3 tw = mix(1.0 - f, f, vec3(o3));
    float w = tw.x * tw.y * tw.z;
    vec3 val;
    if (all6) {
      vec4 a0 = texelFetch(probes, t0, 0);
      if (a0.a < 0.0 || abs(a0.a - tag) > 0.25) continue;
      val = a0.rgb;
      for (int c = 1; c < 6; ++c) val += texelFetch(probes, t0 + ivec2(c, 0), 0).rgb;
      val /= 6.0;
    } else {
      vec4 a = texelFetch(probes, t0 + ivec2(fc.x, 0), 0);
      if (a.a < 0.0 || abs(a.a - tag) > 0.25) continue;
      vec4 b = texelFetch(probes, t0 + ivec2(fc.y, 0), 0);
      vec4 cz = texelFetch(probes, t0 + ivec2(fc.z, 0), 0);
      val = a.rgb * n2.x + b.rgb * n2.y + cz.rgb * n2.z;
      vec3 dir = (vec3(wi) + 0.5) * g.w - P;
      float dl = length(dir);
      float facing = dl > 1e-3 ? dot(dir / dl, Nn) * 0.5 + 0.5 : 1.0;
      w *= facing * facing + 0.02;
    }
    sum += val * w;
    ws += w;
  }
  return vec4(sum, ws);
}
// Both grids: the fine one's where P is well inside it and it has probes
// round P (blended into the coarse one's over its last few), else the
// coarse one's. (Few or faint probes to go by: less of it, not theirs
// blown up.)
vec3 Probes(vec3 P, vec3 Nn, bool all6) {
  ivec3 dims = ivec3(Param(22).xyz);
  vec4 C = Gather(Param(21), dims, 0, P, Nn, all6);
  vec3 e = C.rgb / max(C.a, 0.3);
  vec4 gf = Param(23);
  if (gf.w <= 0.0) return e;
  vec3 q = P / gf.w - 0.5 - gf.xyz;
  vec3 m = min(q, vec3(dims) - 1.0 - q);
  float edge = smoothstep(1.0, 4.0, min(min(m.x, m.y), m.z));
  if (edge <= 0.0) return e;
  vec4 F = Gather(gf, dims, dims.y * dims.z, P, Nn, all6);
  return mix(e, F.rgb / max(F.a, 0.3), edge * smoothstep(0.1, 0.4, F.a));
}
void main() {
  mat4 M;
  for (int i = 0; i < 4; ++i) M[i] = Param(i);
  vec4 cam = Param(11);
  vec4 g = Param(21);
  ivec3 dims = ivec3(Param(22).xyz);
  ivec2 sz = textureSize(depth, 0);
  int sc = int(pc.a.z);
  ivec2 c = min(ivec2(gl_FragCoord.xy) * sc + sc / 2, sz - 1);
  float d = texelFetch(depth, c, 0).r;
  o = vec4(0.0, 0.0, 0.0, d);
  if (d <= 0.0) return;
  vec2 uv = (vec2(c) + 0.5) * pc.a.xy;
  vec4 w4 = M * vec4(uv * vec2(2.0, -2.0) + vec2(-1.0, 1.0), 1.0 - max(d, 1e-7), 1.0);
  vec3 P = w4.xyz / w4.w;
  vec3 Nn = texelFetch(normals, c, 0).xyz * 2.0 - 1.0;
  float nl = length(Nn);
  if (abs(nl - 1.0) > 0.2) return;
  Nn /= nl;
  vec3 e = Probes(P, Nn, false);
  float dist = length(P - cam.xyz);
  e *= 1.0 - smoothstep(float(dims.x) * g.w * 0.35, float(dims.x) * g.w * 0.5, dist);
  o = vec4(e, d);
}
)", true);
  // World-space light, 6 (half size): the riders' (and characters') share,
  // as a factor on their light: the probes round each one (all six faces:
  // they face every way), what they put on or take off against the sky's
  // light and a share of the sun's that the game gave them.
  copy_fs_[69] = CompileBuiltin("builtin_gi_movers", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D pos;  // the riders' points (w > 0)
layout(set = 0, binding = 2) uniform sampler2D probes;
layout(set = 0, binding = 5) uniform sampler2D params;
layout(push_constant) uniform GM { vec4 a; } pc;  // strength
layout(location = 0) out vec4 o;
vec4 Param(int i) { return texelFetch(params, ivec2(i, 0), 0); }
vec3 Sky(vec3 n) {
  vec4 n4 = vec4(n, 1.0);
  vec4 q = n.xyzz * n.yzzx;
  vec3 r = vec3(dot(Param(12), n4), dot(Param(13), n4), dot(Param(14), n4)) +
           vec3(dot(Param(15), q), dot(Param(16), q), dot(Param(17), q)) + Param(18).xyz * (n.x * n.x - n.y * n.y);
  return max(r, vec3(0.0));
}
// The probes round P in one grid (its lowest probe and spacing g, its rows
// from row0): trilinear, for a surface facing Nn (each face by Nn squared,
// the probes it faces weighted up), or all six faces averaged (all6: a body
// facing every way). The weighted sum, the weights' total.
vec4 Gather(vec4 g, ivec3 dims, int row0, vec3 P, vec3 Nn, bool all6) {
  ivec3 lo = ivec3(g.xyz);
  vec3 q = P / g.w - 0.5;
  ivec3 base = ivec3(floor(q));
  vec3 f = q - vec3(base);
  vec3 n2 = Nn * Nn;
  ivec3 fc = ivec3(Nn.x >= 0.0 ? 0 : 1, Nn.y >= 0.0 ? 2 : 3, Nn.z >= 0.0 ? 4 : 5);
  vec3 sum = vec3(0.0);
  float ws = 0.0;
  for (int k = 0; k < 8; ++k) {
    ivec3 o3 = ivec3(k & 1, (k >> 1) & 1, (k >> 2) & 1);
    ivec3 wi = base + o3;
    if (any(lessThan(wi, lo)) || any(greaterThanEqual(wi, lo + dims))) continue;
    ivec3 slot = ((wi % dims) + dims) % dims;
    ivec2 t0 = ivec2(slot.x * 6, row0 + slot.y * dims.z + slot.z);
    float tag = float((wi.x & 7) + 8 * (wi.y & 3) + 32 * (wi.z & 7)) + 1.0;
    vec3 tw = mix(1.0 - f, f, vec3(o3));
    float w = tw.x * tw.y * tw.z;
    vec3 val;
    if (all6) {
      vec4 a0 = texelFetch(probes, t0, 0);
      if (a0.a < 0.0 || abs(a0.a - tag) > 0.25) continue;
      val = a0.rgb;
      for (int c = 1; c < 6; ++c) val += texelFetch(probes, t0 + ivec2(c, 0), 0).rgb;
      val /= 6.0;
    } else {
      vec4 a = texelFetch(probes, t0 + ivec2(fc.x, 0), 0);
      if (a.a < 0.0 || abs(a.a - tag) > 0.25) continue;
      vec4 b = texelFetch(probes, t0 + ivec2(fc.y, 0), 0);
      vec4 cz = texelFetch(probes, t0 + ivec2(fc.z, 0), 0);
      val = a.rgb * n2.x + b.rgb * n2.y + cz.rgb * n2.z;
      vec3 dir = (vec3(wi) + 0.5) * g.w - P;
      float dl = length(dir);
      float facing = dl > 1e-3 ? dot(dir / dl, Nn) * 0.5 + 0.5 : 1.0;
      w *= facing * facing + 0.02;
    }
    sum += val * w;
    ws += w;
  }
  return vec4(sum, ws);
}
// Both grids: the fine one's where P is well inside it and it has probes
// round P (blended into the coarse one's over its last few), else the
// coarse one's. (Few or faint probes to go by: less of it, not theirs
// blown up.)
vec3 Probes(vec3 P, vec3 Nn, bool all6) {
  ivec3 dims = ivec3(Param(22).xyz);
  vec4 C = Gather(Param(21), dims, 0, P, Nn, all6);
  vec3 e = C.rgb / max(C.a, 0.3);
  vec4 gf = Param(23);
  if (gf.w <= 0.0) return e;
  vec3 q = P / gf.w - 0.5 - gf.xyz;
  vec3 m = min(q, vec3(dims) - 1.0 - q);
  float edge = smoothstep(1.0, 4.0, min(min(m.x, m.y), m.z));
  if (edge <= 0.0) return e;
  vec4 F = Gather(gf, dims, dims.y * dims.z, P, Nn, all6);
  return mix(e, F.rgb / max(F.a, 0.3), edge * smoothstep(0.1, 0.4, F.a));
}
void main() {
  vec4 P4 = texelFetch(pos, ivec2(gl_FragCoord.xy), 0);
  o = vec4(1.0, 1.0, 1.0, 0.0);
  if (P4.w <= 0.0) return;
  vec3 P = P4.xyz;
  vec3 e = Probes(P, vec3(0.0, 1.0, 0.0), true);
  vec3 sky = (Sky(vec3(1, 0, 0)) + Sky(vec3(-1, 0, 0)) + Sky(vec3(0, 1, 0)) + Sky(vec3(0, -1, 0)) +
              Sky(vec3(0, 0, 1)) + Sky(vec3(0, 0, -1))) / 6.0;
  // (The sun's share of their light, about: a quarter of it, averaged over
  // a body's sides.)
  vec3 lit = sky + vec3(0.25 * Param(10).w);
  vec3 fac = clamp(1.0 + pc.a.x * e / max(lit, vec3(0.05)), vec3(0.5), vec3(1.6));
  o = vec4(fac, 1.0);
}
)", true);
  copy_fs_[50] = CompileBuiltin("builtin_spec_add", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D spec;
layout(set = 0, binding = 1) uniform sampler2D scene;
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(set = 0, binding = 3) uniform sampler2D bounce;  // half size: the light bounced here, its depth
layout(set = 0, binding = 4) uniform sampler2D albedo;  // as the lighting had it
layout(push_constant) uniform Add { vec4 a; } pc;  // strength, diagnostics, the bounce's strength (0 none), world-space light (signed)
// The bounced light at this pixel: the half-size result's 3 x 3 round it,
// those at about this depth (not across an outline).
vec3 Bounced(ivec2 ip, float d) {
  ivec2 hs = textureSize(bounce, 0);
  ivec2 h = ip * hs / textureSize(scene, 0);
  vec3 sum = vec3(0.0);
  float wsum = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec4 b = texelFetch(bounce, clamp(h + ivec2(x, y), ivec2(0), hs - 1), 0);
      float w = (x == 0 && y == 0 ? 2.0 : 1.0) * exp(-abs(b.a - d) / max(d * 0.02, 1e-6));
      sum += b.rgb * w;
      wsum += w;
    }
  }
  return wsum > 1e-4 ? sum / wsum : vec3(0.0);
}
layout(location = 0) out vec4 o;
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(scene, ip, 0);
  vec4 s = texelFetch(spec, ip, 0);
  float d = texelFetch(depth, ip, 0).r;
  // The far-off scenery lit again for the hour (negative depth): a factor.
  if (s.a < 0.0) {
    o = abs(d + s.a) > 1e-6 - s.a * 0.015 ? c : vec4(c.rgb * s.rgb, c.a);
    return;
  }
  // (Within a percent or two: a rider in front differs far more.)
  if (s.a <= 0.0 || abs(d - s.a) > 1e-6 + s.a * 0.015) {
    o = c;
    return;
  }
  vec3 add = s.rgb * pc.a.x;
  if (pc.a.y > 0.5) {
    o = vec4(s.rgb, 1.0);
    return;
  }
  add /= 1.0 + dot(add, vec3(0.3, 0.59, 0.11));
  // The light bounced onto it, by its own color (the albedo the lighting
  // had: a rider in front fails the depth test above).
  if (pc.a.z > 0.0 && pc.a.w > 0.5) {
    // World-space light: signed (the sky's light taken off where it's
    // blocked, what's there put on).
    vec3 alb = texelFetch(albedo, ip, 0).rgb;
    vec3 g = Bounced(ip, d);
    if (pc.a.y < -0.5) {
      o = vec4(0.5 + g * 1.5, 1.0);  // (diagnostics: grey none, darker taken off, lighter put on)
      return;
    }
    o = vec4(max(c.rgb + add * (1.0 - 0.6 * clamp(c.rgb, 0.0, 1.0)) + alb * g * pc.a.z, 0.0), c.a);
    return;
  }
  if (pc.a.z > 0.0) {
    vec3 alb = texelFetch(albedo, ip, 0).rgb;
    vec3 b = alb * Bounced(ip, d) * pc.a.z;
    if (pc.a.y < -0.5) {
      o = vec4(Bounced(ip, d) * 3.0, 1.0);  // (diagnostics: the bounced light)
      return;
    }
    add += b / (1.0 + dot(b, vec3(0.3, 0.59, 0.11)) * 0.5);
  }
  o = vec4(c.rgb + add * (1.0 - 0.6 * clamp(c.rgb, 0.0, 1.0)), c.a);
}
)", true);
  // Snow tracks, 3: their own shadows in the sun's mask: a groove's wall
  // and a heap shading the snow beside them toward a low sun (marched over
  // the tracks' height toward it).
  // Lens flare. 0: how much of the sun's disc is seen (one texel, eased:
  // the sky's share of it, and how near white it is there, so a cloud over
  // it dims it too); 1: the night lights' ghosts, mirrored through the
  // middle from the bright parts (quarter size); 2: over the scene, those
  // and the sun's (glare, a six-bladed starburst with its fine rays, and
  // the aperture's ghosts along the line through the middle, each a little
  // apart in its colors).
  copy_fs_[53] = CompileBuiltin("builtin_flare", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;    // (linear) 0: the scene; 1: the bright parts; 2: the lights' ghosts
layout(set = 0, binding = 1) uniform sampler2D scene;  // 0: depth; 2: the scene
layout(set = 0, binding = 2) uniform sampler2D aux;    // 0: last frame's; 2: the sun seen
layout(push_constant) uniform Flare {
  vec4 a;  // mode, sun u, sun v, aspect (w / h)
  vec4 b;  // 0: ease, disc radius, near-white from, to; 1: strength, sun masked (0/1); 2: sun color, sun strength
  vec4 c;  // 2: lights' strength, aperture turn, lights' on (0/1), dirt on the lens
} pc;
layout(location = 0) out vec4 o;
const int kGhosts = 8;
// Where each ghost is (from the middle: the sun's offset times this), its
// size (of the screen's height), how bright and its coating's tint.
const float kK[kGhosts] = float[](-0.28, -0.55, -0.9, 0.42, -1.35, 0.22, -1.9, 1.35);
const float kSize[kGhosts] = float[](0.035, 0.06, 0.11, 0.025, 0.16, 0.015, 0.24, 0.05);
const float kBright[kGhosts] = float[](0.18, 0.09, 0.06, 0.14, 0.045, 0.2, 0.03, 0.05);
const vec3 kTint[kGhosts] = vec3[](vec3(0.9, 0.6, 1.0), vec3(0.5, 1.0, 0.6), vec3(1.0, 0.75, 0.4),
                                   vec3(0.6, 0.8, 1.0), vec3(0.7, 1.0, 0.8), vec3(1.0, 0.9, 0.7),
                                   vec3(0.8, 0.6, 1.0), vec3(1.0, 0.5, 0.4));
float Hash(vec2 p) {
  uvec2 q = uvec2(ivec2(floor(p)) + 32768);
  uint h = q.x * 1597334677u ^ q.y * 3812015801u;
  h = (h ^ (h >> 15)) * 2246822519u;
  return float(h ^ (h >> 13)) * (1.0 / 4294967296.0);
}
float Noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(Hash(i), Hash(i + vec2(1, 0)), f.x), mix(Hash(i + vec2(0, 1)), Hash(i + vec2(1, 1)), f.x), f.y);
}
// Dust and smudges on the front of the lens (out of focus: soft discs,
// a little brighter at the rim, and broad faint smears), in screen space.
float Dirt(vec2 uv, float aspect) {
  vec2 p = uv * vec2(aspect, 1.0) * 9.0;
  float d = 0.0;
  for (int l = 0; l < 2; ++l) {
    vec2 q = p * (l == 0 ? 1.0 : 2.3) + float(l) * 17.0;
    vec2 i = floor(q), f = fract(q);
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        vec2 o = vec2(x, y), c = i + o;
        if (Hash(c) < 0.6) continue;
        vec2 at = o + vec2(Hash(c + 31.0), Hash(c + 71.0));
        float rad = 0.15 + 0.3 * Hash(c + 5.0);
        float dd = length(f - at);
        d += (1.0 - smoothstep(rad * 0.8, rad, dd)) * (0.55 + 0.45 * smoothstep(rad * 0.4, rad, dd)) *
             (0.3 + 0.7 * Hash(c + 9.0)) * (l == 0 ? 1.0 : 0.5);
      }
    }
  }
  return d + 0.6 * smoothstep(0.55, 0.85, Noise(p * 0.3 + 3.0));
}
// The aperture (six blades): 1 on its edge.
float Hex(vec2 q) {
  q = abs(q);
  return max(q.x * 0.866025 + q.y * 0.5, q.y);
}
void main() {
  int mode = int(pc.a.x + 0.5);
  vec2 s = pc.a.yz;
  float aspect = pc.a.w;
  if (mode == 0) {
    float seen = 0.0, n = 0.0;
    for (int i = 0; i < 48; ++i) {
      float r = pc.b.y * sqrt((float(i) + 0.5) / 48.0), t = float(i) * 2.39996;
      vec2 uv = s + vec2(cos(t), sin(t)) * r * vec2(1.0 / aspect, 1.0);
      if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) continue;
      n += 1.0;
      if (textureLod(scene, uv, 0.0).r > 0.0) continue;  // something in front
      vec3 c = textureLod(src, uv, 0.0).rgb;
      seen += smoothstep(pc.b.z, pc.b.w, max(c.r, max(c.g, c.b)));
    }
    float prev = texelFetch(aux, ivec2(0), 0).r;
    // (Off the screen: what was seen, fading.)
    float v = n >= 4.0 ? mix(prev, seen / n, pc.b.x) : prev * (1.0 - pc.b.x * 0.5);
    o = vec4(v, n > 0.0 ? seen / n : 0.0, n, 1.0);
  } else if (mode == 1) {
    vec2 size = vec2(textureSize(src, 0));
    vec2 uv = gl_FragCoord.xy / size;
    vec3 acc = vec3(0.0);
    for (int i = 0; i < kGhosts; ++i) {
      for (int ch = 0; ch < 3; ++ch) {
        float k = kK[i] * (1.0 + float(ch - 1) * 0.03);
        vec2 from = 0.5 + (uv - 0.5) / k;
        if (any(lessThan(from, vec2(0.0))) || any(greaterThan(from, vec2(1.0)))) continue;
        // (Not the sun's: drawn whole below.)
        if (pc.b.y > 0.5 && length((from - s) * vec2(aspect, 1.0)) < 0.12) continue;
        // Off to the side, the barrel cuts it.
        float edge = 1.0 - smoothstep(0.35, 0.75, length(from - 0.5));
        acc[ch] += textureLod(src, from, 0.0)[ch] * kBright[i] * kTint[i][ch] * edge * 2.0;
      }
    }
    o = vec4(acc * pc.b.x, 1.0);
  } else {
    vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
    vec3 add = vec3(0.0);
    if (pc.c.z > 0.5) add += textureLod(src, uv, 0.0).rgb * pc.c.x;
    float sun = pc.b.w * texelFetch(aux, ivec2(0), 0).r;
    if (sun > 1e-4) {
      vec3 col = pc.b.rgb * sun;
      vec2 d = (uv - s) * vec2(aspect, 1.0);
      float r = length(d);
      // Glare: a bright core and a wide veil.
      vec3 glare = col * (0.35 * exp(-r * r / 0.00045) + 0.06 / (1.0 + r * r * 500.0));
      // The starburst: six spikes (the blades' edges), the far ends
      // reddening (longer waves spread wider), and fine rays.
      float t = atan(d.y, d.x) + pc.c.y;
      float spike = pow(abs(cos(3.0 * t)), 500.0);
      vec3 reach = vec3(0.11, 0.095, 0.08);
      vec3 burst = spike * exp(-r / reach) * 0.35;
      float fine = Noise(vec2(cos(t), sin(t)) * 26.0);
      burst += vec3(pow(fine, 7.0) * exp(-r / 0.05) * 0.5);
      add += glare + burst * col;
      // The ghosts: the aperture's shape, each a little apart in its colors
      // (an edge of fringes), brighter at the rim.
      vec2 off = s - 0.5;
      float cs = cos(pc.c.y), sn = sin(pc.c.y);
      for (int i = 0; i < kGhosts; ++i) {
        vec3 g;
        for (int ch = 0; ch < 3; ++ch) {
          vec2 p = 0.5 + off * kK[i] * (1.0 + float(ch - 1) * 0.025);
          vec2 q = (uv - p) * vec2(aspect, 1.0);
          q = vec2(cs * q.x - sn * q.y, sn * q.x + cs * q.y);
          float h = Hex(q) / kSize[i];
          g[ch] = (1.0 - smoothstep(0.9, 1.0, h)) * (0.55 + 0.45 * smoothstep(0.5, 1.0, h));
        }
        add += g * kTint[i] * kBright[i] * col;
      }
      // A wide ring round the middle, in rainbow colors, when the sun is
      // near the middle.
      float rc = length((uv - 0.5 + off * 0.1) * vec2(aspect, 1.0));
      vec3 ring = exp(-pow((rc - vec3(0.37, 0.355, 0.34)) / 0.012, vec3(2.0)));
      add += ring * col * 0.035 * (1.0 - smoothstep(0.1, 0.5, length(off * vec2(aspect, 1.0))));
      // The dirt on the lens catching the light: most near the sun.
      if (pc.c.w > 0.0) add += Dirt(uv, aspect) * col * pc.c.w * (0.04 + 0.4 * exp(-r / 0.25));
    }
    o = vec4(c.rgb + add, c.a);
  }
}
)", true);
  // Diamond dust: on a clear, calm, cold day, tiny ice crystals adrift in
  // the air near the camera, each a point that flashes as it tumbles and
  // catches the sun (brightest looking toward it: they scatter forward),
  // in a grid of cells drifting with the breeze, hidden behind what's drawn.
  copy_fs_[54] = CompileBuiltin("builtin_dust", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(push_constant) uniform Dust {
  mat4 m;     // ndc (x, y, 1 - depth, 1) -> world, from the camera
  vec4 t;     // seconds, how many (0..1), a pixel's angle (radians), -
  vec4 vel;   // their drift (m/s), camera x
  vec4 col;   // the sun's light on them, camera y
  vec4 sun;   // toward the sun, camera z
} pc;
layout(location = 0) out vec4 o;
vec3 Hash3(vec3 p) {
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yxz + 33.33);
  return fract((p.xxy + p.yxx) * p.zyx);
}
void main() {
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
  vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec4 mid = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(mid.xyz / mid.w);
  float d = textureLod(depth, uv, 0.0).r;
  float scene_dist = 1e9;
  if (d > 0.0) {
    vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
    scene_dist = length(wp.xyz / wp.w);
  }
  float tt = pc.t.x;
  vec3 vel = pc.vel.xyz;
  vec3 cam = vec3(pc.vel.w, pc.col.w, pc.sun.w);
  // Plates and columns: mostly forward toward the sun, a little all round.
  float mu = max(dot(dir, pc.sun.xyz), 0.0);
  float phase = 0.2 + 2.5 * pow(mu, 6.0) + 6.0 * pow(mu, 60.0);
  vec3 acc = vec3(0.0);
  for (int k = 0; k < 14; ++k) {
    float r = 0.4 * pow(1.28, float(k));  // 0.4 m .. about 10 m
    float cell = r * 0.12;
    if (r - cell > scene_dist) break;
    vec3 pm = cam + dir * r - vel * tt;
    vec3 ci = floor(pm / cell);
    vec3 h = Hash3(ci + float(k) * 17.1);
    if (h.x > pc.t.y * 0.65) continue;
    vec3 h2 = Hash3(ci.zxy * 1.3 + 5.7 + float(k));
    vec3 cm = (ci + 0.15 + 0.7 * h2) * cell;
    // (Wandering as they drift.)
    cm.x += 0.2 * cell * sin(tt * 0.6 + h.y * 30.0);
    cm.z += 0.2 * cell * cos(tt * 0.5 + h.z * 30.0);
    vec3 B = cm + vel * tt - cam;
    float tc = dot(B, dir);
    if (tc < 0.3 || abs(tc - r) > cell * 0.5 || tc > scene_dist) continue;
    float perp = length(B - dir * tc);
    float rad = max(0.0006, tc * pc.t.z * 0.75);
    float f = smoothstep(rad, rad * 0.3, perp);
    if (f <= 0.0) continue;
    // A flash as a face turns to the sun (each its own tumble).
    float g = pow(0.5 + 0.5 * sin(tt * (1.5 + 3.0 * h2.x) + h2.y * 40.0), 24.0);
    float fade = smoothstep(0.3, 0.8, tc) * (1.0 - smoothstep(7.0, 10.0, tc));
    acc += pc.col.rgb * f * (0.06 + 1.6 * g) * phase * fade;
  }
  o = vec4(c.rgb + min(acc, vec3(4.0)), c.a);
}
)", true);
  // Contact shadows: the sun's shadows of what's too small or too far for
  // the shadow maps (a rider's boots, bumps and lips in the snow, a rail's
  // underside), found in the depth: a short march toward the sun in screen
  // space, shadowed where it passes behind something drawn (not too far
  // behind: that's something else, farther off).
  // The sun's shadows eased over a step of the time of day's sun: when the
  // sun maps start over for a sun that's moved on (every degree and a half
  // or so), last frame's shadows, carried to where they are now on screen,
  // held while the maps are drawn again and then eased into the new.
  copy_fs_[56] = CompileBuiltin("builtin_mask_ease", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D prev;  // (linear) last frame's
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D cur;
layout(push_constant) uniform Ease {
  mat4 m;   // ndc (x, y, 1 - depth, 1) now -> last frame's clip
  vec4 p;   // how much of now's (0: last frame's held), 1: the world's shadows', offset x, y
} pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  // (The world's shadows baked for the sun: the old (prev) eased into the new
  // (cur) over the part baked, from its corner.)
  if (pc.p.y > 0.5) {
    ivec2 at = ip - ivec2(pc.p.zw);
    o = vec4(mix(texelFetch(prev, at, 0).r, texelFetch(cur, at, 0).r, pc.p.x));
    return;
  }
  float c = texelFetch(cur, ip, 0).r;
  o = vec4(c);
  float d = texelFetch(depth, ip, 0).r;
  if (d <= 0.0) return;
  vec2 uv = (gl_FragCoord.xy) / vec2(textureSize(depth, 0));
  vec4 q = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  if (q.w <= 1e-4) return;
  vec2 puv = vec2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);
  if (any(lessThan(puv, vec2(0.0))) || any(greaterThan(puv, vec2(1.0)))) return;
  float h = textureLod(prev, puv, 0.0).r;
  if (pc.p.z > 0.5) {
    // (Every frame: last frame's kept only within what's round about now,
    // so a moving shadow doesn't trail; the flicker of a shadow's edge
    // between frames smoothed.)
    float lo = c, hi = c;
    for (int y = -1; y <= 1; ++y) {
      for (int x = -1; x <= 1; ++x) {
        float v = texelFetch(cur, clamp(ip + ivec2(x, y), ivec2(0), textureSize(cur, 0) - 1), 0).r;
        lo = min(lo, v);
        hi = max(hi, v);
      }
    }
    h = clamp(h, lo - 0.04, hi + 0.04);
  }
  o = vec4(mix(h, c, pc.p.x));
}
)", true);
  // Time of day turned on or off: the last picture before eased into the new
  // (a.x: how far along).
  copy_fs_[57] = CompileBuiltin("builtin_tod_fade", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D before;
layout(set = 0, binding = 1) uniform sampler2D now;
layout(push_constant) uniform Fade { vec4 a; } pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  o = mix(texelFetch(before, p, 0), texelFetch(now, p, 0), pc.a.x);
}
)", true);
  // The event icons over the events, kept as the game drew them: where the
  // velocity image's w says an icon is the visible surface (2), the scene
  // from before the effects.
  copy_fs_[58] = CompileBuiltin("builtin_keep_icons", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D now;
layout(set = 0, binding = 1) uniform sampler2D before;
layout(set = 0, binding = 2) uniform sampler2D motion;
layout(location = 0) out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(now, p, 0);
  float icon = clamp(texelFetch(motion, p, 0).w - 1.0, 0.0, 1.0);
  o = icon > 0.0 ? mix(c, texelFetch(before, p, 0), icon) : c;
}
)", true);
  // Drifting snow: in a strong wind the loose snow streams along the slopes
  // in thin, winding ribbons a hand's height off the ground (spindrift),
  // racing with the wind, splitting and merging as they go, coming in gusts;
  // far off, where the ribbons are too fine to see, a low pale haze.
  copy_fs_[52] = CompileBuiltin("builtin_drift", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;  // (the render's size: the scene may be upscaled)
layout(push_constant) uniform Drift {
  mat4 m;    // ndc (x, y, 1 - depth, 1) -> world
  vec4 cam;  // camera, seconds
  vec4 w;    // the wind's direction (x, z), speed (m/s), how much (0..1)
} pc;
layout(location = 0) out vec4 o;
// (Whole-number cells hashed as integers: the same pattern however far out
// on the mountain, no float hash breaking up into blocks.)
uint hashu(ivec2 c) {
  uvec2 q = uvec2(c) * uvec2(1597334673u, 3812015801u);
  uint n = (q.x ^ q.y) * 1597334673u;
  n ^= n >> 16;
  n *= 2246822519u;
  n ^= n >> 13;
  return n;
}
vec2 grad(ivec2 c) {
  float a = float(hashu(c) & 0xFFFFu) * (6.2831853 / 65536.0);
  return vec2(cos(a), sin(a));
}
// Gradient noise, -0.7..0.7 or so (smooth ridges where it crosses zero, no
// value noise's diagonal blocks).
float gnoise(vec2 p) {
  vec2 fl = floor(p);
  ivec2 i = ivec2(fl);
  vec2 f = p - fl;
  vec2 u = f * f * f * (f * (f * 6.0 - 15.0) + 10.0);
  float a = dot(grad(i), f), b = dot(grad(i + ivec2(1, 0)), f - vec2(1, 0));
  float c = dot(grad(i + ivec2(0, 1)), f - vec2(0, 1)), d = dot(grad(i + ivec2(1, 1)), f - vec2(1, 1));
  return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}
vec3 World(vec2 uv, float d) {
  vec4 wp = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  return wp.xyz / wp.w;
}
// One layer of ribbons: thin lines where a stretched noise crosses zero
// (long along the wind, narrow across it), carried along at its speed,
// bent by a slower, broader noise so they wind and change as they go. Fades
// to its average where its ribbons get finer than a few pixels.
float Ribbons(float u, float v, float t, float speed, float across, float along, float fp, float seed) {
  float bend = gnoise(vec2((u - t * speed * 0.55) * 0.045, v * 0.09 + seed)) * 2.2 +
               gnoise(vec2((u - t * speed * 0.8) * 0.11, v * 0.25 + seed * 1.7)) * 0.7;
  float n = gnoise(vec2((u - t * speed) * along, (v + bend) * across + seed * 3.1));
  // (A ribbon's width: about a sixth of the spacing; softened by the pixel's
  // size so it doesn't shimmer.)
  float wdt = 0.14 + fp * across * 0.6;
  float r = 1.0 - smoothstep(0.0, wdt, abs(n));
  float seen = smoothstep(1.2, 4.0, 1.0 / (across * fp));
  return mix(0.3, r, seen);
}
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(scene, ip, 0);
  o = c;
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
  ivec2 ds = textureSize(depth, 0);
  ivec2 dp = clamp(ivec2(uv * vec2(ds)), ivec2(0), ds - 2);
  float d = texelFetch(depth, dp, 0).r;
  if (d <= 0.0) return;
  vec2 duv = (vec2(dp) + 0.5) / vec2(ds);
  vec3 P = World(duv, d);
  // Only on the ground: the depth's slope here near level (not a rider, a
  // tree, a wall or a lip's face).
  float dx = texelFetch(depth, dp + ivec2(1, 0), 0).r, dy = texelFetch(depth, dp + ivec2(0, 1), 0).r;
  if (dx <= 0.0 || dy <= 0.0) return;
  vec3 Px = World(duv + vec2(1.0 / float(ds.x), 0.0), dx), Py = World(duv + vec2(0.0, 1.0 / float(ds.y)), dy);
  vec3 n = cross(Px - P, Py - P);
  float nl = length(n);
  if (nl < 1e-8) return;
  n /= nl;
  float level = smoothstep(0.3, 0.55, abs(n.y));  // (steep slopes too; not walls, riders, trunks)
  // (And smooth: the ground a pixel to the other side in line with this
  // one, not the broken depth of a tree's needles or a rider's edge.)
  float dmx = texelFetch(depth, max(dp - ivec2(1, 0), ivec2(0)), 0).r;
  if (dmx <= 0.0) return;
  vec3 Pmx = World(duv - vec2(1.0 / float(ds.x), 0.0), dmx);
  level *= smoothstep(0.35, 0.15, length((Px - P) - (P - Pmx)) / max(length(Px - P), 1e-4));
  vec3 toP = P - pc.cam.xyz;
  float dist = length(toP);
  float lum = dot(c.rgb, vec3(0.3, 0.59, 0.11));
  // (On the snow: light, grey ground; not right underfoot or out at the haze.)
  float snowy = smoothstep(0.15, 0.35, lum) * smoothstep(0.25, 0.1, max(c.r, max(c.g, c.b)) - min(c.r, min(c.g, c.b)));
  float fade = smoothstep(1.5, 5.0, dist) * smoothstep(180.0, 90.0, dist);
  float where = snowy * fade * level;
  if (where <= 0.0) return;
  // (The ground a pixel covers, in metres, across the screen: the finest
  // ribbon worth drawing.)
  float fp = max(length(Px - P), length(Py - P));
  vec2 wd = pc.w.xy;
  vec2 wn = vec2(-wd.y, wd.x);
  float t = pc.cam.w;
  float u = dot(P.xz, wd), v = dot(P.xz, wn);
  float sp = pc.w.z;
  // Three layers: broad slow sheets, ribbons, and fine fast threads (the
  // grains nearest the ground hop slower than the air; the ones higher up
  // move nearer its speed).
  // (Under them, broad streaky sheets of it, thick and thin.)
  float sb = gnoise(vec2((u - t * sp * 0.3) * 0.03, v * 0.06 + 2.0)) * 1.6;
  float sheet = smoothstep(-0.15, 0.45, gnoise(vec2((u - t * sp * 0.45) * 0.06, (v + sb) * 0.45)));
  float r1 = Ribbons(u, v, t, sp * 0.6, 1.1, 0.13, fp, 11.0);
  float r2 = Ribbons(u, v, t, sp * 0.75, 3.0, 0.3, fp, 23.0);
  float rib = sheet * 0.55 + r1 * 0.45 * (0.4 + sheet) + r2 * 0.3;
  // Gusts: broad patches sweeping through at the wind's speed, the ribbons
  // thick in them and thin between.
  float gust = gnoise(vec2((u - t * sp) * 0.012, v * 0.02 + 5.0)) + 0.5 * gnoise(vec2((u - t * sp * 0.9) * 0.03, v * 0.05 + 9.0));
  gust = smoothstep(-0.5 + 0.25 * pc.w.w, 0.45, gust);
  float k = min(1.0, 0.35 + pc.w.w);
  float a = rib * mix(0.4, 1.0, gust) * k * 1.4;
  // (Seen along the ground, the drift layer's depth piles up: denser low
  // over the slope farther off and at a glancing view.)
  float glance = 1.0 - abs(dot(n, toP / max(dist, 1e-3)));
  a *= 0.55 + 0.6 * glance * glance;
  a = clamp(a * where, 0.0, 0.8);
  // (Lit as the snow is here and brighter: airborne grains catching the
  // light, the slope's texture softened under them (the scene a few pixels
  // round, blurred) but not painted over.)
  ivec2 ss = textureSize(scene, 0) - 1;
  vec3 soft = (texelFetch(scene, clamp(ip + ivec2(3, 1), ivec2(0), ss), 0).rgb +
               texelFetch(scene, clamp(ip + ivec2(-1, 3), ivec2(0), ss), 0).rgb +
               texelFetch(scene, clamp(ip + ivec2(-3, -1), ivec2(0), ss), 0).rgb +
               texelFetch(scene, clamp(ip + ivec2(1, -3), ivec2(0), ss), 0).rgb + c.rgb) * 0.2;
  float sl = max(dot(soft, vec3(0.3, 0.59, 0.11)), 1e-3);
  vec3 veil = mix(soft, vec3(sl), 0.5) * (1.2 + 0.1 * a) + vec3(0.07, 0.075, 0.08) * (0.3 + sl);
  o = vec4(mix(c.rgb, min(veil, vec3(max(lum * 1.35, 1.0))), a), c.a);
}
)", true);
  // The clouds' shadows: the sky's own cloud layer (the same noise, drifting
  // the same way), a few hundred metres up, over each point toward the sun:
  // the sun's shadow mask darkened under them.
  // The sun's shadow mask finished, in one pass (TodShadows): from the sun
  // maps' (kTodShadow), the clouds' shadows over it, the boards' tracks
  // shading the snow beside them toward a low sun, contact shadows (what the
  // maps are too coarse for, marched through the depth), and the far
  // slopes' shadows (past those baked round the camera: the game's painted
  // ones at the mountain's own hour, eased into ones marched toward the sun
  // through the depth on screen as the sun moves away). Its settings are a
  // row of texels (TodShadows lists them).
  copy_fs_[60] = CompileBuiltin("builtin_mask_finish", std::string(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth;
layout(set = 0, binding = 1) uniform sampler2D mask;     // the sun maps' shadows
layout(set = 0, binding = 2) uniform sampler2D cmap;     // the clouds' layer round about (builtin_cloud_map)
layout(set = 0, binding = 3) uniform sampler2D tracks;   // r groove, g heap
layout(set = 0, binding = 4) uniform sampler2D params;   // the settings
layout(set = 0, binding = 5) uniform sampler2D painted;  // the game's painted slope shadows
layout(location = 0) out vec4 o;
vec4 S(int i) { return texelFetch(params, ivec2(i, 0), 0); }
mat4 M;
vec4 R0, R1, R3;
vec3 World(vec2 uv, float d) {
  vec4 w = M * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  return w.xyz / w.w;
}
// The distance (clip w) of what's drawn at a pixel (far for the sky).
float Wat(ivec2 p, vec2 size) {
  float d = texelFetch(depth, clamp(p, ivec2(0), ivec2(size) - 1), 0).r;
  if (d <= 0.0) return 1e6;
  return dot(R3, vec4(World((vec2(p) + 0.5) / size, d), 1.0));
}
float hash(vec2 p) {
  p = fract(p * vec2(443.897, 441.423));
  p += dot(p, p.yx + 19.19);
  return fract((p.x + p.y) * p.x);
}
float noise(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  vec2 w = f * f * (3.0 - 2.0 * f);
  return mix(mix(hash(i), hash(i + vec2(1, 0)), w.x), mix(hash(i + vec2(0, 1)), hash(i + vec2(1, 1)), w.x), w.y);
}
float fbm(vec2 p) {
  float v = 0.0, a = 0.5;
  for (int i = 0; i < 5; ++i) {
    v += a * noise(p);
    p = p * 2.03 + vec2(17.1, 9.7);
    a *= 0.5;
  }
  return v;
}
vec4 TB;  // the tracks' groove depth, heap height
)") + kTracksClumps + R"(
float Ht(vec2 tc) {
  vec2 f = tc - 0.5;
  ivec2 i = ivec2(floor(f));
  vec2 w = f - floor(f);
  ivec2 n = textureSize(tracks, 0) - 1;
  vec2 a = Live(texelFetch(tracks, clamp(i, ivec2(0), n), 0));
  vec2 b = Live(texelFetch(tracks, clamp(i + ivec2(1, 0), ivec2(0), n), 0));
  vec2 c = Live(texelFetch(tracks, clamp(i + ivec2(0, 1), ivec2(0), n), 0));
  vec2 d = Live(texelFetch(tracks, clamp(i + ivec2(1, 1), ivec2(0), n), 0));
  vec2 t = mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
  float heap = t.g * (1.0 - t.r);
  return -TB.x * t.r + TB.y * heap + Clumps(tc, heap) * TB.y;
}
// (Soft over a texel or so, as the tracks are shaded.)
float Hs(vec2 tc) {
  return (Ht(tc + vec2(1.0, 1.0)) + Ht(tc + vec2(-1.0, 1.0)) + Ht(tc + vec2(1.0, -1.0)) + Ht(tc - vec2(1.0))) * 0.25;
}
// The painted map at world x, z (its four nearest texels weighed: it's
// coarse, a metre or so a texel).
float Painted(vec2 xz, vec4 c5) {
  vec2 n = vec2(textureSize(painted, 0));
  vec2 t = (xz * c5.xy + c5.zw) * n - 0.5;
  if (any(lessThan(t, vec2(0.0))) || any(greaterThan(t, n - 2.0))) return 1.0;
  ivec2 i = ivec2(floor(t));
  vec2 w = t - floor(t);
  float a = texelFetch(painted, i, 0).r, b = texelFetch(painted, i + ivec2(1, 0), 0).r;
  float c = texelFetch(painted, i + ivec2(0, 1), 0).r, d = texelFetch(painted, i + ivec2(1, 1), 0).r;
  return mix(mix(a, b, w.x), mix(c, d, w.x), w.y);
}
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  float lit = texelFetch(mask, ip, 0).r;
  o = vec4(lit);
  float d = texelFetch(depth, ip, 0).r;
  vec2 size = vec2(textureSize(depth, 0));
  vec2 uv = gl_FragCoord.xy / size;
  M = mat4(S(0), S(1), S(2), S(3));
  R0 = S(4);
  R1 = S(5);
  R3 = S(6);
  vec4 wp = M * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - max(d, 1e-7), 1.0);
  vec3 P = wp.xyz / (abs(wp.w) < 1e-9 ? 1.0 : wp.w);
  // (The surface's facing, from the depth: before anything branches.)
  vec3 Nf = cross(dFdx(P), dFdy(P));
  if (d <= 0.0 || abs(wp.w) < 1e-9) return;
  vec4 Ls = S(7);
  vec3 L = Ls.xyz;
  float w0 = dot(R3, vec4(P, 1.0));
  float jit = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));

  // The clouds' shadows.
  vec4 cc = S(8);
  if (cc.z > 0.0) {
    vec4 cm = S(9);
    // (Where its ray toward the sun meets the layer; the layer's scale as the
    // sky draws it: 0.9 a height.)
    vec2 q = P.xz + L.xz * (cc.y / max(L.y, 0.08));
    vec2 p = q / cc.y * 0.9 + vec2(cc.x * 0.006, cc.x * 0.0025);
    // (From the map round about; past it, the noise itself.)
    vec2 mu = (p - cm.xy) * cm.z;
    float n = all(greaterThan(mu, vec2(0.002))) && all(lessThan(mu, vec2(0.998))) ? textureLod(cmap, mu, 0.0).r : fbm(p);
    float cloud = smoothstep(1.0 - cc.w - 0.03, 1.0 - cc.w + 0.1, n);
    lit *= 1.0 - cc.z * cloud;
  }

  // The tracks toward a low sun: a groove's wall, a heap, a clump.
  vec4 ta = S(10);
  if (ta.w > 0.5 && lit > 0.01) {
    TB = S(11);
    vec2 tc = (P.xz - ta.xy) * ta.z;
    vec2 n2 = vec2(textureSize(tracks, 0));
    if (all(greaterThanEqual(tc, vec2(16.0))) && all(lessThanEqual(tc, n2 - 16.0))) {
      float h0 = Hs(tc);
      float occ = 0.0;
      for (int i = 0; i < 10; ++i) {
        float f = (float(i) + jit) / 10.0;
        float t = 0.02 + 0.9 * f * f;  // 2 cm .. ~1 m
        vec2 q = tc + L.xz * t * ta.z;
        float ray = h0 + L.y * t;
        occ = max(occ, smoothstep(0.0, 0.025, Hs(q) - ray));
      }
      lit *= 1.0 - 0.75 * occ;
    }
  }

  // Contact shadows: what the maps miss, near by.
  if (Ls.w > 0.0 && lit > 0.02 && w0 > 0.0 && w0 <= 120.0) {
    // (Reaching farther the farther off: about the same on screen.)
    float reach = clamp(w0 * 0.02, 0.2, 1.2);
    float bias = 0.03 + w0 * 0.002;
    // How fast the surface's distance changes across the screen here (a
    // slope seen edge-on: steps along it look 'behind' it): the smaller of
    // each side's, so an outline doesn't count.
    float gx = min(abs(Wat(ip + ivec2(1, 0), size) - w0), abs(Wat(ip - ivec2(1, 0), size) - w0));
    float gy = min(abs(Wat(ip + ivec2(0, 1), size) - w0), abs(Wat(ip - ivec2(0, 1), size) - w0));
    float grad = max(gx, gy);
    // (A little jitter only.)
    float cj = 0.35 + 0.3 * jit;
    float occ = 0.0;
    for (int i = 0; i < 12; ++i) {
      float f = (float(i) + cj) / 12.0;
      float t = reach * (0.02 + 0.98 * f * f);
      vec3 Q = P + L * t;
      float cw = dot(R3, vec4(Q, 1.0));
      if (cw <= 0.05) break;
      vec2 q = vec2(dot(R0, vec4(Q, 1.0)), dot(R1, vec4(Q, 1.0))) / cw;
      vec2 quv = vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
      if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
      ivec2 qp = ivec2(quv * size);
      if (qp == ip) continue;
      float ds = texelFetch(depth, qp, 0).r;
      if (ds <= 0.0) continue;
      float sw = dot(R3, vec4(World(quv, ds), 1.0));
      float behind = cw - sw;
      // Behind what's drawn there, but within a thickness (more for nearer
      // steps: a boot, a lip; not the slope far behind a rider's outline).
      float thick = 0.3 + 0.6 * t;
      // (Two steps behind for it all: one alone may be the depth's grain.)
      float px = length(vec2(qp - ip));
      if (behind > bias + t * 0.03 + grad * px * 0.6 && behind < thick) occ += 0.5 * (1.0 - f * 0.6);
    }
    occ = min(occ, 1.0);
    float fade = 1.0 - smoothstep(60.0, 120.0, w0);
    lit *= 1.0 - occ * fade * Ls.w;
  }

  // Far off: the slopes past those baked round the camera.
  vec4 f0 = S(12), f1 = S(13);
  if (f1.x > 0.0 && lit > 0.02) {
    float k = smoothstep(f0.w, f0.w + f1.x, length(P.xz - f0.xz));
    if (k > 0.0) {
      float away = f1.y;  // 0: the painted shadows (the mountain's hour) .. 1: the marched
      float pl = 1.0, ml = 1.0;
      if (f1.z > 0.5 && away < 0.999) pl = Painted(P.xz, S(14));
      // (Facing away from the sun: dark by its light already, nothing to find.)
      vec3 N = normalize(Nf);
      if (dot(N, P - f0.xyz) > 0.0) N = -N;  // (toward the camera)
      if (away > 0.001 && dot(N, L) > 0.03) {
        // Toward the sun in steps growing from a few metres to most of a
        // kilometre, against what's drawn on screen: behind it (by more than
        // the depth's grain), but not far behind (a nearer hill in front of
        // the ray, not between it and the sun). Jittered frame to frame (the
        // mask's easing smooths it).
        float fj = fract(jit + f1.w * 0.618034);
        float t0 = max(2.0, w0 * 0.006), t1 = clamp(w0 * 1.2, 250.0, 900.0);
        float occ = 0.0;
        for (int i = 0; i < 14; ++i) {
          float f = (float(i) + fj) / 14.0;
          float t = t0 * pow(t1 / t0, f);
          vec3 Q = P + L * t;
          float cw = dot(R3, vec4(Q, 1.0));
          if (cw <= 0.5) break;
          vec2 q = vec2(dot(R0, vec4(Q, 1.0)), dot(R1, vec4(Q, 1.0))) / cw;
          vec2 quv = vec2(q.x * 0.5 + 0.5, 0.5 - q.y * 0.5);
          if (any(lessThan(quv, vec2(0.0))) || any(greaterThan(quv, vec2(1.0)))) break;
          ivec2 qp = ivec2(quv * size);
          if (qp == ip) continue;
          float ds = texelFetch(depth, qp, 0).r;
          if (ds <= 0.0) continue;
          float behind = cw - dot(R3, vec4(World(quv, ds), 1.0));
          float bias = 0.5 + cw * 0.004 + t * 0.02;
          float thick = 12.0 + 0.3 * t;
          occ += smoothstep(bias, bias * 2.0 + 2.0, behind) * (1.0 - smoothstep(thick, thick * 1.4, behind));
          if (occ > 2.5) break;
        }
        // (Several steps behind for a shadow: a ridge holds many, a tree's
        // crown on the way one or two - left to the sun maps nearer, and
        // only speckle this far off.)
        ml = 1.0 - smoothstep(0.8, 2.5, occ);
      }
      lit *= mix(1.0, mix(pl, ml, away), k);
    }
  }
  o = vec4(lit);
}
)", true);
  copy_fs_[37] = CompileBuiltin("builtin_tod_shadow", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D depth;   // the scene's (0: nothing)
layout(set = 0, binding = 1) uniform sampler2D casters; // from the sun: depth, 1 = nothing
layout(set = 0, binding = 2) uniform sampler2D world;   // the world map (trees, props): the same
layout(set = 0, binding = 3) uniform sampler2D normals; // the lighting's (xyz * 2 - 1)
layout(push_constant) uniform Mask {
  mat4 m;  // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 o0; vec4 o1; vec4 o2;  // the casters' map's rows
  vec4 a;  // the world map's ndc from the casters' (x k + a.z, y k + a.w... ): offset x, y, k,
           // on (0.5) + how much an overcast sky softens them (thousandths)
} pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 sz = textureSize(depth, 0);
  vec2 uv = (gl_FragCoord.xy) / vec2(sz);
  float d = textureLod(depth, uv, 0.0).r;
  if (d <= 0.0) {
    o = vec4(1.0);
    return;
  }
  vec4 wp = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  if (abs(wp.w) < 1e-9) {
    o = vec4(1.0);
    return;
  }
  vec4 P = vec4(wp.xyz / wp.w, 1.0);
  float dpm = length(pc.o2.xyz);  // depth per metre along the sun
  vec3 fd = pc.o2.xyz / dpm;      // the sun's direction (travelling)
  float lit = 1.0;
  // Riders, rails, objects (fine).
  {
    vec2 st = vec2(dot(pc.o0, P), dot(pc.o1, P));
    float zr = dot(pc.o2, P);
    float n = float(textureSize(casters, 0).x);
    vec2 tc = (0.5 + 0.5 * st) * n;
    if (all(greaterThan(tc, vec2(2.0))) && all(lessThan(tc, vec2(n - 2.0)))) {
      float l = 0.0;
      for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
          float z = texelFetch(casters, ivec2(tc + vec2(x, y) - 0.5), 0).r;
          l += (z >= 1.0 || z >= zr - 0.06 * dpm) ? 1.0 : 0.0;
        }
      }
      lit = l / 16.0;
    }
  }
  // Trees and props (the world map: their trunks and the stumps under them,
  // which the lighting shades by this map, not the slopes' one), looked up
  // off the surface along its normal (the surface itself is in the map too).
  if (fract(pc.a.w) > 0.25) {
    vec3 N = normalize(textureLod(normals, uv, 0.0).xyz * 2.0 - 1.0);
    vec4 Q = vec4(P.xyz + N * 0.3 - fd * 0.15, 1.0);
    vec2 st = vec2(dot(pc.o0, Q), dot(pc.o1, Q)) * pc.a.z + pc.a.xy;
    float zr = dot(pc.o2, Q);
    float n = float(textureSize(world, 0).x);
    vec2 tc = (0.5 + 0.5 * st) * n;
    if (all(greaterThan(tc, vec2(3.0))) && all(lessThan(tc, vec2(n - 3.0)))) {
      // (Each of 3 x 3 taps a texel apart weighed between its four nearest
      // texels: the edge smooth, not in texel steps.)
      vec2 b = tc - 0.5;
      vec2 f = fract(b);
      ivec2 i0 = ivec2(floor(b));
      float l = 0.0;
      for (int y = -1; y <= 2; ++y) {
        for (int x = -1; x <= 2; ++x) {
          float z = texelFetch(world, i0 + ivec2(x, y), 0).r;
          float wx = x == -1 ? 1.0 - f.x : x == 2 ? f.x : 1.0;
          float wy = y == -1 ? 1.0 - f.y : y == 2 ? f.y : 1.0;
          l += ((z >= 1.0 || z >= zr - 0.25 * dpm) ? 1.0 : 0.0) * wx * wy;
        }
      }
      lit = min(lit, l / 9.0);
    }
  }
  // (A grey sky: the light all round, shadows faint.)
  lit = mix(lit, 1.0, floor(pc.a.w) / 1000.0);
  o = vec4(lit);
}
)", true);
  // Night lights: each lamp's light on the world (the lighting's albedo and
  // normals: as the game lights it) and on the riders (drawn after the
  // lighting: their surface from the depth, their color from the scene), a
  // glow round each lamp in the air.
  copy_fs_[38] = CompileBuiltin("builtin_night_lights", std::string(R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D world_depth;  // at the lighting (0: nothing)
layout(set = 0, binding = 2) uniform sampler2D lights;       // n x 3: pos, radius; color, intensity; dir, kind
                                                             // (row 3: world from the camera -> clip;
                                                             // rows 4, 5: points afar; 8..: the tiles)
layout(set = 0, binding = 3) uniform sampler2D albedo;
layout(set = 0, binding = 4) uniform sampler2D normals;      // xyz * 2 - 1
layout(set = 0, binding = 5) uniform sampler2D depth;        // now (with the riders)
layout(push_constant) uniform Lights {
  mat4 m;    // ndc (x, y, 1 - depth, 1) -> world (homogeneous)
  vec4 cam;  // camera position, tile size (pixels)
  vec4 a;    // 1 / width, 1 / height, how dark it is (0..1), riders told apart (0/1)
  vec4 b;    // seconds, the scene's light on the fittings (0..1), shadow steps (0: none), -
  vec4 c;    // the air's density for their light in it (volumetric: 0 none), how many
             // lamps cast shadows at each spot (the nearest; 0: all), -
} pc;
layout(location = 0) out vec4 o;

// A light's shadow: from P toward the light (at L), stepping through the
// depth on screen (riders and all); 1: nothing in the way.
vec4 g_vp[4];
float g_jitter;
vec4 g_dbg = vec4(0.0);
float g_noise;  // per pixel, 0..1 (the beams' steps)
float ShadowTo(vec3 P, vec3 N, vec3 L, int steps, bool riders) {
  vec3 start = P + N * 0.06;
  vec3 seg = L - start;
  float len = length(seg);
  if (len < 0.4) return 1.0;
  float vis = 1.0;
  // Up to 0.35 m short of the light (the surface it's mounted on).
  float stop = 1.0 - 0.35 / len;
  for (int i = 0; i < steps; ++i) {
    // (Closer together near the surface: the contact; jittered per pixel.)
    float f = (float(i) + g_jitter) / float(steps);
    f = f * f * stop;
    vec3 S = start + seg * f;
    vec4 sr = vec4(S - pc.cam.xyz, 1.0);
    vec4 cs = vec4(dot(g_vp[0], sr), dot(g_vp[1], sr), dot(g_vp[2], sr), dot(g_vp[3], sr));
    if (cs.w <= 1e-4) break;
    vec3 nd = cs.xyz / cs.w;
    vec2 suv = vec2(nd.x * 0.5 + 0.5, 0.5 - nd.y * 0.5);
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) break;
    float sd = riders ? textureLod(depth, suv, 0.0).r : textureLod(world_depth, suv, 0.0).r;
    if (sd <= 0.0) continue;  // (sky)
    vec4 wq = pc.m * vec4(nd.xy, 1.0 - sd, 1.0);
    float scene_dist = length(wq.xyz / wq.w - pc.cam.xyz);
    float s_dist = length(S - pc.cam.xyz);
    float gap = s_dist - scene_dist;  // > 0: something in front of the step
    float bias = 0.04 + 0.0025 * s_dist;
    float thick = 0.7 + 0.01 * s_dist;  // (no deeper than this: not what's behind)
    float occ = smoothstep(bias, bias * 2.5, gap) * smoothstep(thick, thick * 0.6, gap);
    vis = min(vis, 1.0 - occ);
    if (vis < 0.02) break;
  }
  return vis;
}

// The fittings, in each light's own frame (origin at the light; z along its
// direction for a floodlight): x distance, y material (0 dark metal, 1 lit
// glass / lens, 2 wood, 3 stone, 4 glowing embers, 5 charred, 6 bright
// metal).
float sdBox(vec3 p, vec3 b) {
  vec3 q = abs(p) - b;
  return length(max(q, 0.0)) + min(max(q.x, max(q.y, q.z)), 0.0);
}
float sdRoundBox(vec3 p, vec3 b, float r) { return sdBox(p, b - r) - r; }
float sdCapsule(vec3 p, vec3 a, vec3 b, float r) {
  vec3 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h) - r;
}
// A capsule thinning from ra (at a) to rb (at b).
float sdTaper(vec3 p, vec3 a, vec3 b, float ra, float rb) {
  vec3 pa = p - a, ba = b - a;
  float h = clamp(dot(pa, ba) / dot(ba, ba), 0.0, 1.0);
  return length(pa - ba * h) - mix(ra, rb, h);
}
float sdCylY(vec3 p, float y0, float y1, float r) {
  vec2 d = vec2(length(p.xz) - r, max(y0 - p.y, p.y - y1));
  return min(max(d.x, d.y), 0.0) + length(max(d, 0.0));
}
float sdCylX(vec3 p, float x0, float x1, float r) { return sdCylY(p.yxz, x0, x1, r); }
// A lamp shade: a cone's thin shell from radius r1 at y1 (top) to r0 at y0.
float sdShade(vec3 p, float y0, float y1, float r0, float r1, float t) {
  float h = clamp((p.y - y0) / (y1 - y0), 0.0, 1.0);
  float r = mix(r0, r1, h);
  float side = abs(length(p.xz) - r) - t;
  float cap = max(y0 - p.y, p.y - y1);
  return max(side, cap);
}
float smin(float a, float b, float k) {
  float h = clamp(0.5 + 0.5 * (b - a) / k, 0.0, 1.0);
  return mix(b, a, h) - k * h * (1.0 - h);
}
vec2 opU(vec2 a, vec2 b) { return a.x < b.x ? a : b; }
float Hash(float n) { return fract(sin(n) * 43758.5453); }
// (An integer hash: a cell's own, far from the origin too.)
float Hash3(ivec3 c, int k) {
  uint h = uint(c.x) * 73856093u ^ uint(c.y) * 19349663u ^ uint(c.z) * 83492791u ^ uint(k) * 2654435761u;
  h ^= h >> 16;
  h *= 0x7feb352du;
  h ^= h >> 15;
  h *= 0x846ca68bu;
  h ^= h >> 16;
  return float(h & 0xFFFFFFu) / 16777216.0;
}
)") + kSnowGlint + R"(vec3 g_arm;  // a bulb: from it back to where it's mounted (world axes)
vec2 Model(int kind, vec3 p) {
  if (kind == 0) {
    // The bulb: round, with its neck into a ribbed socket.
    float bulb = smin(length(p - vec3(0.0, -0.01, 0.0)) - 0.075, sdCylY(p, -0.01, 0.075, 0.032), 0.03);
    vec2 d = vec2(bulb, 1.0);
    float rib = 0.004 * sin(p.y * 140.0);
    d = opU(d, vec2(sdCylY(p, 0.07, 0.14, 0.038 + rib), 6.0));
    float L = length(g_arm);
    if (g_arm.y < -0.7 * L) {
      // A lamp post: a shade over the bulb, the pole down to a base plate.
      d = opU(d, vec2(sdShade(p, -0.02, 0.16, 0.2, 0.05, 0.006), 0.0));
      d = opU(d, vec2(sdCylY(p, 0.15, 0.2, 0.05), 0.0));
      vec3 foot = g_arm;
      d = opU(d, vec2(sdTaper(p, foot + vec3(0.0, 0.05, 0.0), vec3(0.0, 0.2, 0.0) + vec3(0.0, 0.0, 0.0), 0.06, 0.035), 0.0));
      d = opU(d, vec2(sdCylY(p - foot, 0.0, 0.05, 0.16), 0.0));
      // (The pole rises past the shade: a cap on top.)
      d = opU(d, vec2(length(p - vec3(0.0, 0.22, 0.0)) - 0.045, 6.0));
    } else if (g_arm.y > 0.7 * L) {
      // Hanging: a little shade and the cable up.
      d = opU(d, vec2(sdShade(p, 0.0, 0.15, 0.13, 0.045, 0.005), 0.0));
      d = opU(d, vec2(sdCapsule(p, vec3(0.0, 0.14, 0.0), g_arm, 0.007), 0.0));
    } else {
      // Off a wall or tower: a shade, a stem, a bracket curving back to a
      // plate on the wall.
      d = opU(d, vec2(sdShade(p, -0.02, 0.15, 0.17, 0.05, 0.006), 0.0));
      vec3 back = normalize(vec3(g_arm.x, 0.0, g_arm.z) + vec3(1e-5, 0.0, 0.0));
      vec3 top = vec3(0.0, 0.3, 0.0);
      vec3 wall = vec3(g_arm.x, 0.3, g_arm.z);
      d = opU(d, vec2(sdCapsule(p, vec3(0.0, 0.15, 0.0), top - vec3(0.0, 0.03, 0.0), 0.014), 0.0));
      d = opU(d, vec2(sdCapsule(p, top - vec3(0.0, 0.03, 0.0) + back * 0.03, wall, 0.02), 0.0));
      // The plate: flat against the wall.
      vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), back));
      vec3 q = p - wall;
      d = opU(d, vec2(sdRoundBox(vec3(dot(q, side), q.y, dot(q, back)), vec3(0.06, 0.1, 0.015), 0.01), 0.0));
    }
    return d;
  }
  if (kind == 4) {
    // A street light: a tapered pole on a concrete footing, an arm out at
    // the top to a long flat head with its lens underneath.
    vec3 foot = g_arm;
    vec3 back = normalize(vec3(foot.x, 0.0, foot.z) + vec3(1e-5, 0.0, 0.0));
    vec3 side = normalize(cross(vec3(0.0, 1.0, 0.0), back));
    vec3 top = vec3(foot.x, 0.3, foot.z);
    vec3 q = p - back * 0.1;
    vec3 hq = vec3(dot(q, side), q.y - 0.06, dot(q, back));
    vec2 d = vec2(sdRoundBox(hq, vec3(0.15, 0.05, 0.3), 0.05), 6.0);
    d = opU(d, vec2(sdRoundBox(hq - vec3(0.0, -0.06, -0.02), vec3(0.11, 0.012, 0.22), 0.01), 1.0));
    d = opU(d, vec2(sdCapsule(p, back * 0.38 + vec3(0.0, 0.08, 0.0), top + back * 0.05, 0.035), 6.0));
    d = opU(d, vec2(sdTaper(p, foot + vec3(0.0, 0.35, 0.0), top + vec3(0.0, 0.1, 0.0), 0.1, 0.06), 6.0));
    d = opU(d, vec2(length(p - top - vec3(0.0, 0.1, 0.0)) - 0.07, 0.0));
    d = opU(d, vec2(sdCylY(p - foot, -0.1, 0.35, 0.22), 3.0));
    return d;
  }
  if (kind == 5) {
    // A caged work / alarm light: a glass cylinder (its light) with a domed
    // cap, inside a metal cage of bars and rings, on a round base back to
    // where it's mounted (y: its axis, out from the mount).
    vec2 d = vec2(sdCylY(p, -0.085, 0.085, 0.062), 1.0);
    d = opU(d, vec2(length(p - vec3(0.0, 0.085, 0.0)) - 0.062, 1.0));
    d.x = smin(d.x, length(p - vec3(0.0, 0.085, 0.0)) - 0.062, 0.01);
    // The base: a collar and a plate on the mount.
    d = opU(d, vec2(sdCylY(p, -0.13, -0.085, 0.075), 0.0));
    d = opU(d, vec2(sdCylY(p, -0.16, -0.13, 0.1), 0.0));
    // The bars: six round it, out from the glass, meeting over the top.
    float a = atan(p.z, p.x);
    float seg = 6.2831853 / 6.0;
    float aa = mod(a + seg * 0.5, seg) - seg * 0.5;
    float rr = length(p.xz);
    vec3 b = vec3(rr * cos(aa), p.y, rr * sin(aa));
    float bars = sdCapsule(b, vec3(0.088, -0.09, 0.0), vec3(0.088, 0.09, 0.0), 0.0055);
    bars = min(bars, sdCapsule(b, vec3(0.088, 0.09, 0.0), vec3(0.0, 0.165, 0.0), 0.0055));
    d = opU(d, vec2(bars, 6.0));
    // The rings: round the middle and near the base.
    vec2 ring1 = vec2(rr - 0.09, p.y - 0.01);
    vec2 ring2 = vec2(rr - 0.09, p.y + 0.075);
    d = opU(d, vec2(min(length(ring1), length(ring2)) - 0.0065, 6.0));
    return d;
  }
  if (kind == 1) {
    // A floodlight: a rounded housing, a bright bezel round the lens, fins
    // behind, a yoke with its pivots, a stem back to the mount.
    vec2 d = vec2(sdRoundBox(p, vec3(0.21, 0.155, 0.1), 0.03), 0.0);
    float bezel = max(sdRoundBox(p - vec3(0.0, 0.0, 0.105), vec3(0.225, 0.17, 0.018), 0.02),
                      -sdBox(p - vec3(0.0, 0.0, 0.12), vec3(0.18, 0.125, 0.05)));
    d = opU(d, vec2(bezel, 6.0));
    // The lens (just inside the bezel).
    d = opU(d, vec2(sdBox(p - vec3(0.0, 0.0, 0.11), vec3(0.18, 0.125, 0.006)), 1.0));
    // Fins on the back.
    vec3 f = p - vec3(0.0, 0.0, -0.125);
    f.x = mod(f.x + 0.035, 0.07) - 0.035;
    d = opU(d, vec2(max(sdBox(f, vec3(0.006, 0.12, 0.03)), abs(p.x) - 0.19), 0.0));
    // The yoke: side arms with pivot bolts, joined below.
    d = opU(d, vec2(sdBox(vec3(abs(p.x) - 0.245, p.y + 0.06, p.z), vec3(0.01, 0.1, 0.035)), 0.0));
    d = opU(d, vec2(sdCylX(vec3(abs(p.x), p.y, p.z), 0.2, 0.27, 0.022), 6.0));
    d = opU(d, vec2(sdBox(p - vec3(0.0, -0.165, 0.0), vec3(0.255, 0.012, 0.035)), 0.0));
    d = opU(d, vec2(sdCapsule(p, vec3(0.0, -0.165, 0.0), vec3(0.0, -0.165, -0.32), 0.025), 0.0));
    d = opU(d, vec2(sdRoundBox(p - vec3(0.0, -0.165, -0.33), vec3(0.07, 0.07, 0.012), 0.008), 0.0));
    return d;
  }
  // A campfire: a ring of stones of all sizes, logs leaning in, two lying
  // across, an ember bed (the light 0.4 m up).
  vec3 q = p + vec3(0.0, 0.4, 0.0);
  vec2 d = vec2(1e9, 3.0);
  for (int i = 0; i < 9; ++i) {
    float t = float(i) * 0.6981 + 0.2 * Hash(float(i) * 3.7);
    float r = 0.065 + 0.04 * Hash(float(i) * 1.9);
    float rr = 0.36 + 0.04 * Hash(float(i) * 5.3);
    vec3 c = vec3(rr * cos(t), r * 0.55, rr * sin(t));
    d = opU(d, vec2((length((q - c) * vec3(1.0, 1.45, 1.0)) - r) / 1.45, 3.0));
  }
  for (int i = 0; i < 4; ++i) {
    float t = float(i) * 1.5708 + 0.4;
    vec3 a = vec3(0.32 * cos(t), 0.03, 0.32 * sin(t));
    float bark = 0.004 * sin(dot(q, vec3(40.0, 23.0, 31.0)));
    d = opU(d, vec2(sdTaper(q, a, vec3(0.02 * cos(t * 3.0), 0.36, 0.02 * sin(t * 3.0)), 0.048, 0.03) + bark, 2.0));
  }
  d = opU(d, vec2(sdCapsule(q, vec3(-0.28, 0.04, -0.06), vec3(0.26, 0.05, 0.1), 0.04), 2.0));
  d = opU(d, vec2(sdCapsule(q, vec3(-0.05, 0.05, -0.27), vec3(0.1, 0.06, 0.26), 0.038), 5.0));
  d = opU(d, vec2(sdCylY(q, 0.0, 0.035 + 0.01 * sin(q.x * 30.0) * sin(q.z * 27.0), 0.24), 4.0));
  return d;
}
vec3 ModelNormal(int kind, vec3 p) {
  const vec2 e = vec2(0.003, 0.0);
  return normalize(vec3(Model(kind, p + e.xyy).x - Model(kind, p - e.xyy).x,
                        Model(kind, p + e.yxy).x - Model(kind, p - e.yxy).x,
                        Model(kind, p + e.yyx).x - Model(kind, p - e.yyx).x));
}
void main() {
  vec2 uv = gl_FragCoord.xy * pc.a.xy;
  vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
  float k = pc.a.z;
  // This pixel's tile: the lights that can reach it.
  ivec2 tile = ivec2(gl_FragCoord.xy) / int(pc.cam.w + 0.5);
  ivec2 tile_base = ivec2(tile.x, 8 + tile.y * 9);
  int tile_n = int(texelFetch(lights, tile_base, 0).x + 0.5);
  if (tile_n == 0) {
    o = c;
    return;
  }
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  float dw = textureLod(world_depth, uv, 0.0).r;
  float dn = textureLod(depth, uv, 0.0).r;
  vec4 mid = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(mid.xyz / mid.w - pc.cam.xyz);
  // A rider: in front of the world by more than the snow trails lie above it
  // (in metres: the same up close as far off).
  bool rider = false;
  if (pc.a.w > 0.5 && dn > dw * 1.0005 + 1e-7) {
    vec4 pn = pc.m * vec4(ndc, 1.0 - dn, 1.0);
    float far_w = 1e9;
    if (dw > 0.0) {
      vec4 pw = pc.m * vec4(ndc, 1.0 - dw, 1.0);
      far_w = length(pw.xyz / pw.w - pc.cam.xyz);
    }
    rider = far_w - length(pn.xyz / pn.w - pc.cam.xyz) > 0.12;
  }
  float d = rider ? dn : dw;
  float pix_dist = 1e9;
  vec3 P = vec3(0.0);
  if (d > 0.0) {
    vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
    P = wp.xyz / wp.w;
    pix_dist = length(P - pc.cam.xyz);
  }
  vec4 nw4 = textureLod(normals, uv, 0.0);
  vec3 N = normalize(nw4.xyz * 2.0 - 1.0);
  // (The surface tagged in the normals' alpha: 2 metal, 5 paint: shinier.)
  uint surf_tag = rider ? 0u : (uint(nw4.w * 255.0 + 0.5) >> 5u);
  vec3 alb = textureLod(albedo, uv, 0.0).rgb;
  if (rider && d > 0.0) {
    // A rider (drawn after the lighting: no albedo or normals of its own):
    // its surface from the depth next to it (the nearer side: not across
    // its outline), its color from how the scene shows it under the
    // night's light.
    // (Three pixels out, each way: the depth's steps no grain in the light.)
    ivec2 ip = ivec2(gl_FragCoord.xy);
    const int R = 3;
    float dxp = texelFetch(depth, ip + ivec2(R, 0), 0).r, dxm = texelFetch(depth, ip - ivec2(R, 0), 0).r;
    float dyp = texelFetch(depth, ip + ivec2(0, R), 0).r, dym = texelFetch(depth, ip - ivec2(0, R), 0).r;
    bool use_xp = abs(dxp - dn) < abs(dxm - dn), use_yp = abs(dyp - dn) < abs(dym - dn);
    vec2 px = pc.a.xy * 2.0 * float(R);
    vec2 nx = ndc + vec2(use_xp ? px.x : -px.x, 0.0), ny = ndc + vec2(0.0, use_yp ? -px.y : px.y);
    vec4 wx = pc.m * vec4(nx, 1.0 - (use_xp ? dxp : dxm), 1.0);
    vec4 wy = pc.m * vec4(ny, 1.0 - (use_yp ? dyp : dym), 1.0);
    vec3 ex = (wx.xyz / wx.w - P) * (use_xp ? 1.0 : -1.0);
    vec3 ey = (wy.xyz / wy.w - P) * (use_yp ? 1.0 : -1.0);
    vec3 nr = cross(ey, ex);
    N = dot(nr, nr) > 1e-12 ? normalize(nr) : -dir;
    if (dot(N, dir) > 0.0) N = -N;  // (toward the camera)
    alb = min(c.rgb / max(pc.b.y, 0.12), vec3(0.85));
  }
  vec3 add = vec3(0.0);
  g_noise = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  int shadow_steps = int(pc.b.z + 0.5);
  if (shadow_steps > 0 && d > 0.0) {
    for (int r = 0; r < 4; ++r) g_vp[r] = texelFetch(lights, ivec2(r, 3), 0);
    // (Interleaved gradient noise: the steps' banding into fine grain.)
    g_jitter = fract(52.9829189 * fract(dot(gl_FragCoord.xy, vec2(0.06711056, 0.00583715))));
  }
  // The nearest fitting in front of what's drawn: its color (solid).
  float hit_t = pix_dist;
  vec3 hit_col = vec3(0.0);
  bool hit = false;
  vec3 surf = vec3(0.0);  // the lights on the surface drawn (hidden by a fitting)
  int shadowed = 0;       // lamps whose shadows were traced (the nearest first)
  for (int e = 0; e < tile_n; ++e) {
    int slot = e + 1;
    vec4 tv = texelFetch(lights, tile_base + ivec2(0, slot / 4), 0);
    int i = int(tv[slot % 4] + 0.5);
    if (i >= 256) {
      // A light afar: a point of light, hidden behind what's in front of it
      // (5 looks round its middle: no flicker at an edge).
      vec4 sa = texelFetch(lights, ivec2(i - 256, 4), 0);
      vec4 sc = texelFetch(lights, ivec2(i - 256, 5), 0);
      vec2 dpx = (ndc - sa.xy) * 0.5 / pc.a.xy;
      float q = dot(dpx, dpx) / (sa.w * sa.w);
      if (q > 16.0) continue;
      vec2 cuv = vec2(sa.x * 0.5 + 0.5, 0.5 - sa.y * 0.5);
      vec2 off = pc.a.xy * max(sa.w * 0.5, 1.0);
      float seen = 0.0;
      for (int s = 0; s < 5; ++s) {
        vec2 o2 = s == 0 ? vec2(0.0) : vec2(s == 1 ? 1.0 : s == 2 ? -1.0 : 0.0, s == 3 ? 1.0 : s == 4 ? -1.0 : 0.0);
        float sd = textureLod(depth, cuv + o2 * off, 0.0).r;
        seen += (sd <= 0.0 || sd < sa.z * 0.9995) ? 0.2 : 0.0;
      }
      // (What's drawn here in front of it: only a little of its halo over that.)
      if (dn > sa.z * 1.0005) seen *= 0.3;
      float g = 1.0 / (1.0 + q);
      add += sc.rgb * (g * g * 1.6 + 0.08 * g) * seen * (1.0 - smoothstep(9.0, 16.0, q));
      continue;
    }
    vec4 lp = texelFetch(lights, ivec2(i, 0), 0);
    vec4 lc = texelFetch(lights, ivec2(i, 1), 0);
    vec4 ld = texelFetch(lights, ivec2(i, 2), 0);
    // Its panel's width and height (a window or panel), how wide its light
    // is cast (half the cone, degrees: 0 the kind's own; 180 all round).
    vec4 lx = texelFetch(lights, ivec2(i, 6), 0);
    int code = int(ld.w + 0.5);
    int kind = code & 7;
    float spread = lx.z > 0.0 ? lx.z : (kind == 1 ? 35.0 : kind == 4 ? 90.0 : kind == 2 ? 75.0 : 180.0);
    vec2 panel = vec2(lx.x > 0.0 ? lx.x : 1.3, lx.y > 0.0 ? lx.y : 1.1) * 0.5;
    bool preview = (code & 8) != 0, selected = (code & 16) != 0;
    // How lit: after dark, each coming on at its own moment as it gets dark
    // (stuttering on as it warms up); a preview shows its light by day too.
    float hid = Hash(dot(lp.xyz, vec3(0.131, 0.713, 0.377)));
    float on_at = hid * 0.3;
    float kk = smoothstep(on_at, on_at + 0.1, k);
    if (kk > 0.0 && kk < 1.0) kk *= 0.55 + 0.45 * step(0.4, Hash(floor(pc.b.x * 14.0) + hid * 97.0));
    if (preview) kk = max(k, 0.6);
    vec3 axis = normalize(ld.xyz);
    // (A bulb: dir is its mounting, out from the surface: the arm back to it.)
    g_arm = -ld.xyz;
    float flick = 1.0;
    if (kind == 3) {  // a fire flickers
      float t = pc.b.x * 9.0 + float(i) * 3.1;
      flick = 0.82 + 0.1 * sin(t) + 0.08 * sin(t * 2.7 + 1.3) + 0.05 * Hash(floor(t * 1.5));
    }
    vec3 col = lc.rgb * lc.w * flick * kk;
    vec3 own = lc.rgb * lc.w * flick;  // (its glow at full)
    // On the world.
    if (d > 0.0 && kk > 0.001) {
      vec3 dl = lp.xyz - P;
      float dist = length(dl);
      // A window or panel: its own frame (facing the way it was placed,
      // tilted with it), the light from the nearest point of it.
      vec3 nrm = axis, rt = vec3(1.0, 0.0, 0.0), up = vec3(0.0, 1.0, 0.0);
      if (kind == 2) {
        rt = normalize(cross(abs(nrm.y) < 0.98 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), nrm));
        up = cross(nrm, rt);
        vec3 rel = P - lp.xyz;
        vec2 pl = vec2(dot(rel, rt), dot(rel, up));
        dl = lp.xyz + rt * clamp(pl.x, -panel.x, panel.x) + up * clamp(pl.y, -panel.y, panel.y) - P;
        dist = length(dl);
      }
      if (kind == 2 && !rider) {
        // The wall's own texture lit up within it (its color tinted,
        // glowing): as wide and high as it was sized.
        vec3 rel = P - lp.xyz;
        vec3 l3 = vec3(dot(rel, rt), dot(rel, up), dot(rel, nrm));
        vec2 soft = min(panel * 0.1, vec2(0.07));
        if (abs(l3.x) < panel.x + soft.x && abs(l3.y) < panel.y + soft.y && abs(l3.z) < 0.4) {
          float edge = smoothstep(panel.x + soft.x, panel.x - soft.x, abs(l3.x)) *
                       smoothstep(panel.y + soft.y, panel.y - soft.y, abs(l3.y));
          surf += (alb * col * 2.4 + col * 0.18) * edge;
          // (The panel's outline while previewed or picked.)
          if (preview || selected) {
            float rim = max(smoothstep(panel.x - soft.x * 1.5, panel.x + soft.x, abs(l3.x)),
                            smoothstep(panel.y - soft.y * 1.5, panel.y + soft.y, abs(l3.y)));
            surf += (preview ? vec3(0.3, 0.8, 1.0) : vec3(1.0, 0.85, 0.2)) * rim *
                   (0.35 + 0.2 * sin(pc.b.x * 6.0));
          }
        }
      }
      if (dist < lp.w) {
        float x = dist / lp.w;
        float fall = (1.0 - x * x) * (1.0 - x * x) / (1.0 + dist * dist * 0.06);
        float ndl = max(dot(N, dl / max(dist, 1e-3)), 0.0) * 0.8 + 0.2;
        vec3 out_dir = -dl / max(dist, 1e-3);  // (from the light to here)
        if (kind == 4) {  // a street light: down and round from its head
          fall *= smoothstep(cos(radians(min(spread + 20.0, 179.0))), cos(radians(max(spread - 15.0, 1.0))),
                             dot(out_dir, vec3(0.0, -1.0, 0.0))) * 1.25 + 0.04;
        } else if (kind == 1) {  // a floodlight: a cone along its direction
          fall *= smoothstep(cos(radians(min(spread, 179.0))), cos(radians(spread * 0.6)), dot(out_dir, axis)) * 1.4 +
                  0.04;
        } else if (kind == 2) {  // a window: out of it, spread from its face
          fall *= smoothstep(cos(radians(min(spread + 15.0, 179.0))), cos(radians(max(spread - 10.0, 1.0))),
                             dot(out_dir, nrm)) * (dot(P - lp.xyz, nrm) > -0.05 ? 1.0 : 0.0);
        } else if (spread < 179.0) {
          // A lamp: down from it; a caged light: along its axis.
          vec3 cone = kind == 5 ? axis : vec3(0.0, -1.0, 0.0);
          fall *= smoothstep(cos(radians(min(spread + 15.0, 179.0))), cos(radians(max(spread - 15.0, 1.0))),
                             dot(out_dir, cone)) * 1.2 + 0.03;
        }
        if (kind == 5) {
          // The cage's bars striping the light thrown round it.
          vec3 bx = normalize(cross(abs(axis.y) < 0.98 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), axis));
          vec3 bz = cross(axis, bx);
          float ang = atan(dot(out_dir, bz), dot(out_dir, bx));
          float side = 1.0 - abs(dot(out_dir, axis));
          fall *= 1.0 - 0.45 * side * smoothstep(0.9, 0.985, cos(ang * 6.0));
        }
        vec3 lit = alb * col * fall * ndl * 1.6;
        // A sheen toward the camera (snow and ice catch it), and on the snow
        // the odd crystal glinting, changing as the view moves.
        vec3 L = dl / max(dist, 1e-3);
        vec3 hv = normalize(L - dir);
        float facing = max(dot(N, L), 0.0);
        if (facing > 0.0) {
          float snow = rider ? 0.0 : smoothstep(0.45, 0.7, dot(alb, vec3(0.333)));
          float sheen = pow(max(dot(N, hv), 0.0), 16.0) * (0.12 + 0.12 * snow);
          if (snow < 0.5 && surf_tag == 2u) {
            // Metal: a tighter, brighter highlight, tinted by its color.
            lit += col * fall * facing * pow(max(dot(N, hv), 0.0), 70.0) * 1.6 * (alb + 0.25);
          } else if (snow < 0.5 && surf_tag == 5u) {
            sheen += pow(max(dot(N, hv), 0.0), 50.0) * 0.5;
          }
          vec4 w1 = pc.m * vec4(ndc + vec2(pc.a.x * 2.0, 0.0), 1.0 - d, 1.0);
          float glint = SnowGlint(P, N, hv, length(w1.xyz / w1.w - P)) * snow;
          lit += col * fall * facing * (sheen + glint * 5.0);
        }
        // Its shadows (what's lit enough to show one).
        if (shadow_steps > 0 && dot(lit, vec3(1.0)) > 0.004 &&
            (pc.c.y < 0.5 || float(shadowed) < pc.c.y)) {
          ++shadowed;
          // (Fewer steps where the shadow can't show much: a faint light, or
          // far off where a step covers few pixels.)
          int steps = shadow_steps;
          if (dot(lit, vec3(1.0)) < 0.06) steps = max(3, steps / 2);
          if (pix_dist > 40.0) steps = max(3, (steps * 2) / 3);
          float sv = ShadowTo(P, N, lp.xyz, steps, pc.a.w > 0.5);
          if (pc.b.w > 0.5) g_dbg = vec4(sv, rider ? 1.0 : 0.0, fall, 1.0);
          lit *= mix(0.08, 1.0, sv);
        }
        surf += lit;
      }
    }
    // The glow in the air round the bulb, the lens, the flames.
    vec3 to = lp.xyz - pc.cam.xyz;
    float t = dot(to, dir);
    if (kk > 0.001 && t > 0.3 && t < min(pix_dist, hit_t) + 0.5 && kind != 2) {
      float perp = length(to - dir * t);
      float size = (kind == 1 ? 0.18 : kind == 5 ? 0.1 : 0.12) + 0.004 * t;
      float g = size * size / (perp * perp + size * size);
      float facing = kind == 1 ? 0.25 + 0.75 * smoothstep(0.2, 0.9, dot(-dir, axis)) : 1.0;
      add += col * (0.55 * g * g + 0.12 * g) * facing * smoothstep(400.0, 150.0, t);
    }
    // Its light in the air (volumetric: haze, mist, falling snow), along the
    // ray through its reach: the in-scattering of a point light, worked out
    // (no steps): 1 / (1 + a d^2) along the ray (most near it), within its radius.
    if (pc.c.x > 0.0 && kk > 0.001 && kind != 2 && kind != 1) {
      float r = lp.w * 0.55;  // (the air near it: its far reach too faint to show)
      vec3 cl = dir * t - to;  // its nearest point on the ray, from the light
      float hq = length(cl);
      if (hq < r) {
        float half_len = sqrt(r * r - hq * hq);
        float s0 = max(0.0, t - half_len) - t;
        float s1 = min(min(pix_dist, hit_t), t + half_len) - t;
        if (s1 > s0) {
          const float a = 1.0;
          float bb = sqrt((1.0 + a * hq * hq) / a);
          float I = (atan(s1 / bb) - atan(s0 / bb)) / (a * bb);
          float x = hq / r;
          float win = (1.0 - x * x) * (1.0 - x * x);
          float dirf = 1.0;
          if (kind == 4) {
            // (A street light: down and round from its head.)
            vec3 v = dir * clamp(t, t + s0, t + s1) - to;
            dirf = smoothstep(-0.35, 0.25, dot(v / max(length(v), 1e-3), vec3(0.0, -1.0, 0.0))) * 1.25 + 0.04;
          }
          add += col * I * win * dirf * pc.c.x;
        }
      }
    }
    // A floodlight's beam in the air (night mist): along the ray where it
    // passes through the cone, out to the light's reach.
    if (kind == 1 && kk > 0.001) {
      vec3 oc = pc.cam.xyz - lp.xyz;
      float bq = dot(oc, dir), cq = dot(oc, oc) - lp.w * lp.w;
      float disc = bq * bq - cq;
      if (disc > 0.0) {
        float sq = sqrt(disc);
        float t0 = max(-bq - sq, 0.3), t1 = min(-bq + sq, min(pix_dist, hit_t));
        if (t1 > t0) {
          const int N_STEPS = 10;
          float stp = (t1 - t0) / float(N_STEPS);
          float acc = 0.0;
          for (int j = 0; j < N_STEPS; ++j) {
            vec3 v = pc.cam.xyz + dir * (t0 + (float(j) + g_noise) * stp) - lp.xyz;
            float dd = length(v);
            float cone = smoothstep(0.86, 0.95, dot(v / max(dd, 1e-3), axis));
            float x = dd / lp.w;
            acc += cone * (1.0 - x * x) * (1.0 - x * x) / (1.0 + dd * dd * 0.08);
          }
          add += col * acc * stp * (0.04 + pc.c.x * 0.25);
        }
      }
    }
    // The fitting itself (by day too), when the ray passes near it.
    // (How far the fitting reaches from its light: a lamp post down to the ground.)
    float reach = kind == 0 || kind == 4 ? max(0.5, length(g_arm) + 0.3) : (kind == 3 ? 0.8 : kind == 5 ? 0.35 : 0.6);
    if (kind != 2 && t > 0.2 && t < 150.0) {
      float perp = length(to - dir * t);
      if (perp < reach) {
        // Its frame: z along the axis for a floodlight; y along it for a
        // caged light.
        vec3 fz = kind == 1 ? axis : vec3(0.0, 0.0, 1.0);
        vec3 fx = normalize(cross(abs(fz.y) < 0.99 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0), fz));
        vec3 fy = cross(fz, fx);
        if (kind == 5) {
          fy = axis;
          fx = normalize(cross(fy, abs(fy.y) < 0.98 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0)));
          fz = cross(fx, fy);
        } else if (kind != 1) {
          fx = vec3(1.0, 0.0, 0.0);
          fy = vec3(0.0, 1.0, 0.0);
          fz = vec3(0.0, 0.0, 1.0);
        }
        float s = max(t - reach - 0.1, 0.0);
        for (int j = 0; j < 72; ++j) {
          vec3 wpos = pc.cam.xyz + dir * s;
          vec3 lpos = wpos - lp.xyz;
          vec3 q = vec3(dot(lpos, fx), dot(lpos, fy), dot(lpos, fz));
          vec2 md = Model(kind, q);
          if (md.x < 0.002) {
            if (s < hit_t) {
              vec3 nl = ModelNormal(kind, q);
              vec3 nw = nl.x * fx + nl.y * fy + nl.z * fz;
              int mat = int(md.y + 0.5);
              vec3 base = mat == 0 ? vec3(0.12, 0.12, 0.13)
                        : mat == 1 ? vec3(0.85, 0.85, 0.8)
                        : mat == 2 ? vec3(0.32, 0.2, 0.11)
                        : mat == 3 ? vec3(0.38, 0.37, 0.36)
                        : mat == 4 ? vec3(0.15, 0.06, 0.02)
                        : mat == 5 ? vec3(0.05, 0.04, 0.035) : vec3(0.45, 0.46, 0.48);
              // (Stones and bark a little mottled.)
              if (mat == 3 || mat == 2) base *= 0.8 + 0.4 * Hash(dot(floor(q * 40.0), vec3(1.0, 57.0, 113.0)));
              float amb = pc.b.y * (0.55 + 0.45 * max(nw.y, 0.0));
              // A sheen on the metal, from above and toward the camera.
              float sheen = (mat == 0 || mat == 6) ? pow(max(dot(reflect(dir, nw), vec3(0.0, 1.0, 0.0)), 0.0), 12.0) : 0.0;
              vec3 cc = base * amb + vec3(sheen * (mat == 6 ? 0.35 : 0.15)) * pc.b.y;
              // The inside of a shade or housing catches its own light.
              if ((kind <= 1 || kind == 5) && (mat == 0 || mat == 6)) cc += base * col * 0.9 * clamp(0.25 - length(q) , 0.0, 0.25) * 4.0;
              // (A caged light's bars painted: yellow, like a work light's.)
              if (kind == 5 && mat == 6) cc *= vec3(1.3, 1.1, 0.35);
              // Lit by its own light (the fire's logs and stones), glowing
              // where it's the light (the bulb, the lens, the embers).
              if (kind == 3 && mat != 4) cc += base * col * 0.8 * clamp(1.0 - length(q + vec3(0.0, 0.2, 0.0)) * 1.5, 0.0, 1.0);
              if (mat == 1 || mat == 4) cc = mix(cc, own * (mat == 4 ? 1.2 : 1.6), kk);
              // A preview: a ghostly blue; picked: pulsing gold.
              if (preview) cc = mix(cc, vec3(0.35, 0.8, 1.0), 0.4 + 0.15 * sin(pc.b.x * 6.0));
              if (selected) cc = mix(cc, vec3(1.0, 0.85, 0.2), 0.35 + 0.25 * sin(pc.b.x * 6.0));
              hit_col = cc;
              hit_t = s;
              hit = true;
            }
            break;
          }
          s += max(md.x, 0.0025);
          if (s > t + reach + 0.1 || s > hit_t) break;
        }
      }
      // The flames (a fire, after dark): glowing over the logs, flickering.
      if (kind == 3 && kk > 0.001 && perp < 0.8) {
        float glow = 0.0;
        for (int j = 0; j < 16; ++j) {
          float s = t - 0.6 + float(j) * 0.075;
          if (s > min(pix_dist, hit_t)) break;
          vec3 q = pc.cam.xyz + dir * s - lp.xyz + vec3(0.0, 0.4, 0.0);
          float h = q.y;
          if (h < 0.02 || h > 0.75) continue;
          float wob = 0.03 * sin(pc.b.x * 11.0 + h * 14.0 + float(i));
          float r = 0.2 * (1.0 - h / 0.75) * (0.85 + 0.15 * sin(pc.b.x * 7.0 + h * 9.0));
          float dd = length(q.xz + vec2(wob, 0.0));
          glow += smoothstep(r, r * 0.3, dd) * 0.09;
        }
        add += vec3(1.0, 0.55, 0.18) * glow * 2.2 * flick * kk;
      }
    }
  }
  if (pc.a.w > 1.5) {  // (diagnostics: riders, depth, albedo, normal)
    o = vec4(rider ? 1.0 : 0.0, d > 0.0 ? 0.5 : 0.0, dot(alb, vec3(0.333)), 1.0);
    if (uv.x > 0.5) o = vec4(N * 0.5 + 0.5, 1.0);
    return;
  }
  // A fitting in front: solid (the light on what's behind it hidden), the
  // glows in the air still over it.
  o = vec4(hit ? hit_col + add : c.rgb + surf + add, c.a);
  if (pc.b.w > 0.5) o = g_dbg;  // (diagnostics: shadow, rider, falloff)
}
)", true);
  // Falling snow: flakes in a grid of cells moving with the wind, met at
  // distances along each pixel's ray (nearer slices finer), hidden behind
  // what's drawn in front of them.
  copy_fs_[39] = CompileBuiltin("builtin_snow", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D scene;
layout(set = 0, binding = 1) uniform sampler2D depth;
layout(set = 0, binding = 2) uniform sampler2D lights;  // the night lights' (tiles of 32 px)
layout(set = 0, binding = 3) uniform sampler2D heights;  // the ground from above (the sun maps' height map): depth, 1 = nothing
layout(set = 0, binding = 4) uniform usampler2D occ;     // the world light's fine voxels (1 m), if on
layout(set = 0, binding = 5) uniform sampler2D prm;      // 0: the flakes' travel (gusts summed), turbulence (m);
                                                          // 1, 2: the height map's rows, its depth span, on, the ground under the camera;
                                                          // 3: the voxels' corner, size (0: none); 4: the wind's direction (x, z), speed
layout(push_constant) uniform Snow {
  mat4 m;     // ndc (x, y, 1 - depth, 1) -> world, from the camera
  vec4 t;     // seconds, how much snow (0..1), a pixel's angle (radians), flake size scale
              // (negative: the night lights are there to light the flakes)
  vec4 vel;   // the flakes' velocity (m/s; wind and fall), streak length (s)
  vec4 col;   // the flakes' color (lit), -
  vec4 a;     // 1 / width, 1 / height, -, -
} pc;
layout(location = 0) out vec4 o;

vec4 Prm(int i) { return texelFetch(prm, ivec2(i, 0), 0); }
// The ground's height at x, z (the height map softened over a couple of
// metres: the flakes' layers drape over it), or the ground under the camera.
float Ground(vec2 xz) {
  vec4 hm = Prm(1), h2 = Prm(2);
  if (h2.y < 0.5) return h2.z;
  ivec2 hn = textureSize(heights, 0);
  float sum = 0.0, wsum = 0.0;
  for (int i = 0; i < 5; ++i) {
    vec2 o = i == 0 ? vec2(0.0) : vec2(i == 1 ? 1.5 : (i == 2 ? -1.5 : 0.0), i == 3 ? 1.5 : (i == 4 ? -1.5 : 0.0));
    vec2 ht = (0.5 + 0.5 * ((xz + o) * hm.x + hm.yz)) * vec2(hn);
    if (any(lessThan(ht, vec2(0.0))) || any(greaterThanEqual(ht, vec2(hn)))) continue;
    float hz = texelFetch(heights, ivec2(ht), 0).r;
    if (hz >= 1.0) continue;
    sum += hm.w + (0.5 - hz) * h2.x;
    wsum += 1.0;
  }
  return wsum > 0.0 ? sum / wsum : h2.z;
}
bool Solid(vec3 w) {
  vec4 vb = Prm(3);
  if (vb.w <= 0.0) return false;
  ivec3 v = ivec3(floor((w - vb.xyz) / vb.w));
  if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, ivec3(128)))) return false;
  uvec4 c = texelFetch(occ, v.xz, 0);
  return ((c[v.y >> 5] >> uint(v.y & 31)) & 1u) != 0u;
}
vec3 Hash3(vec3 p) {
  p = fract(p * vec3(0.1031, 0.1030, 0.0973));
  p += dot(p, p.yxz + 33.33);
  return fract((p.xxy + p.yxx) * p.zyx);
}
void main() {
  vec2 uv = gl_FragCoord.xy * pc.a.xy;
  vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec4 mid = pc.m * vec4(ndc, 0.5, 1.0);
  vec3 dir = normalize(mid.xyz / mid.w);
  float d = textureLod(depth, uv, 0.0).r;
  float scene_dist = 1e9;
  if (d > 0.0) {
    vec4 wp = pc.m * vec4(ndc, 1.0 - d, 1.0);
    scene_dist = length(wp.xyz / wp.w);
  }
  float amount = pc.t.y;
  float tt = pc.t.x;
  vec3 vel = pc.vel.xyz;
  vec3 cam = vec3(pc.col.w, pc.a.z, pc.a.w);
  // A streak: where the flake was a moment ago (blurred in strong wind).
  vec3 streak = vel * pc.vel.w;
  float acc = 0.0;
  vec3 acc_col = vec3(0.0);
  // The night lights near this pixel (its tile's), lighting the flakes.
  bool lit_by_lamps = pc.t.w < 0.0;
  ivec2 tile_base = ivec2(ivec2(gl_FragCoord.xy) / 32) + ivec2(0, 8);
  tile_base.y = 8 + (ivec2(gl_FragCoord.xy) / 32).y * 9;
  int tile_n = lit_by_lamps ? int(texelFetch(lights, tile_base, 0).x + 0.5) : 0;
  float dark = lit_by_lamps ? texelFetch(lights, ivec2(4, 3), 0).x : 0.0;
  vec4 p0 = Prm(0), wv = Prm(4);
  vec3 travel = p0.xyz;  // (how far the snow has moved: the wind's gusts summed)
  float turb = p0.w;
  vec3 wd = vec3(wv.x, 0.0, wv.y);
  for (int k = 0; k < 16; ++k) {
    float r = 0.5 * pow(1.25, float(k));  // 0.5 m .. about 14 m
    float cell = r * 0.16;
    if (r - cell > scene_dist) break;
    // The cell (in the snow's own frame, moving with it, its layers draped
    // over the ground: the snow carried along the slopes, up over a rise
    // and down behind it) where the ray is r out, and the flake in it (if
    // any: fewer in light snow).
    vec3 W0 = cam + dir * r;
    float lift = Ground(W0.xz);
    vec3 pm = W0 - travel - vec3(0.0, lift, 0.0);
    vec3 ci = floor(pm / cell);
    vec3 h = Hash3(ci + float(k) * 31.7);
    // (Fewer up close: a few big flakes, not a curtain.)
    if (h.x > amount * (0.3 + 0.6 * smoothstep(1.0, 6.0, float(k)))) continue;
    vec3 h2 = Hash3(ci.yzx * 1.7 + 11.3 + float(k));
    vec3 cm = (ci + 0.2 + 0.6 * h2) * cell;
    // (Fluttering as it falls.)
    cm.x += 0.12 * cell * sin(tt * 1.7 + h.y * 40.0);
    cm.z += 0.12 * cell * cos(tt * 1.3 + h.z * 40.0);
    // (Eddies: the flakes of a patch swirled together, more in a strong
    // wind, the patches changing as they go.)
    vec3 sw = vec3(sin(cm.z * 0.45 + tt * 1.3 + 2.0 * sin(cm.y * 0.3 + tt * 0.5)),
                   0.6 * sin(cm.x * 0.5 - tt * 1.1),
                   cos(cm.x * 0.4 - tt * 0.9 + 2.0 * sin(cm.z * 0.25 - tt * 0.4)));
    cm += sw * turb;
    vec3 B = cm + travel + vec3(0.0, lift, 0.0) - cam;  // from the camera
    vec3 A = B - streak;
    // The nearest point of its streak to the ray.
    vec3 u = B - A;
    float uu = dot(u, u), sseg = 1.0;
    if (uu > 1e-8) {
      float bd = dot(dir, u), den = uu - bd * bd;
      sseg = den > 1e-8 ? clamp((bd * dot(dir, A) - dot(u, A)) / den, 0.0, 1.0) : 1.0;
    }
    vec3 X = A + u * sseg;
    float tc = dot(X, dir);
    if (tc < 0.4 || abs(tc - r) > cell * 0.5 || tc > scene_dist) continue;
    // Out of the snow: under something (a roof, a tree's crown) or in the
    // lee of what stands upwind (the voxels, with the world light on).
    if (Prm(3).w > 0.0) {
      vec3 Wf = cam + X;
      bool roof = Solid(Wf + vec3(0.0, 2.0, 0.0)) || Solid(Wf + vec3(0.0, 5.0, 0.0)) || Solid(Wf + vec3(0.0, 9.0, 0.0));
      if (roof && h.z < 0.85) continue;
      if (wv.z > 2.0) {
        float lee = 0.0;
        for (int j = 1; j <= 3; ++j) {
          if (Solid(Wf - wd * float(j) * 1.3)) lee = max(lee, 1.0 - float(j - 1) * 0.25);
        }
        if (h.z < lee * smoothstep(2.0, 10.0, wv.z) * 0.9) continue;
      }
    }
    float perp = length(X - dir * tc);
    float size = (0.006 + 0.006 * h.y) * abs(pc.t.w);
    // At least most of a pixel across (dimmer for it: as bright overall).
    float rad = max(size, tc * pc.t.z * 0.7);
    float f = smoothstep(rad, rad * 0.25, perp) * min(1.0, (size * size) / (rad * rad) * 1.5);
    f /= 1.0 + sqrt(uu) / (12.0 * size);  // (a streak: its light spread along it)
    f *= smoothstep(0.6, 1.4, tc) * (1.0 - smoothstep(10.0, 14.0, tc));
    if (f <= 0.0) continue;
    // Lit by the sky, and the lamps near it (each flake bright in a lamp's
    // light).
    vec3 fc = pc.col.rgb;
    if (dark > 0.001) {
      vec3 W = cam + X;
      for (int e = 0; e < tile_n; ++e) {
        int slot = e + 1;
        vec4 tv = texelFetch(lights, tile_base + ivec2(0, slot / 4), 0);
        int i = int(tv[slot % 4] + 0.5);
        if (i >= 256) continue;
        vec4 lp = texelFetch(lights, ivec2(i, 0), 0);
        vec3 dl = lp.xyz - W;
        float dist = length(dl);
        if (dist >= lp.w) continue;
        vec4 lc = texelFetch(lights, ivec2(i, 1), 0);
        vec4 ld = texelFetch(lights, ivec2(i, 2), 0);
        float x = dist / lp.w;
        float fall = (1.0 - x * x) * (1.0 - x * x) / (1.0 + dist * dist * 0.06);
        const int lk = int(ld.w + 0.5) & 7;
        if (lk == 4) fall *= smoothstep(-0.35, 0.25, dot(-dl / max(dist, 1e-3), vec3(0.0, -1.0, 0.0))) * 1.25 + 0.04;
        if (lk == 1) fall *= smoothstep(0.82, 0.93, dot(-dl / max(dist, 1e-3), normalize(ld.xyz))) * 1.4 + 0.04;
        fc += lc.rgb * lc.w * dark * fall * 1.2;
      }
    }
    acc += f;
    acc_col += fc * f;
  }
  float cover = clamp(acc, 0.0, 0.9);
  o = vec4(c.rgb * (1.0 - cover) + acc_col * (cover / max(acc, 1e-4)), c.a);
}
)", true);
  // Fresh snow: the lighting's albedo whitened where its surface faces up
  // (its normals), patchy where little has fallen (noise by where it is).
  copy_fs_[40] = CompileBuiltin("builtin_snow_cover", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D albedo;
layout(set = 0, binding = 1) uniform sampler2D normals;
layout(set = 0, binding = 2) uniform sampler2D depth;
layout(set = 0, binding = 3) uniform usampler2D occ;  // the world light's fine voxels (1 m), if on
layout(push_constant) uniform Cover {
  mat4 m;   // ndc (x, y, 1 - depth, 1) -> world
  vec4 a;   // how much (0..1), 1 / width, 1 / height, -
  vec4 b;   // the wind (m/s, x / z), the camera's x, z
  vec4 v;   // the voxels' corner (world), their size (0: none)
} pc;
layout(location = 0) out vec4 o;
// (Hashed as whole numbers: a sine of world-sized numbers is noise of its own
// on some GPUs - stripes.)
uint Hu(uvec3 q) {
  uint h = q.x * 1597334677u ^ q.y * 3812015801u ^ q.z * 2654435761u;
  h = (h ^ (h >> 15)) * 2246822519u;
  return h ^ (h >> 13);
}
float H(vec2 p) { return float(Hu(uvec3(ivec2(p) + 1048576, 7)) >> 8) * (1.0 / 16777216.0); }
// Something in the voxel at w (world).
bool Solid(vec3 w) {
  if (pc.v.w <= 0.0) return false;
  ivec3 v = ivec3(floor((w - pc.v.xyz) / pc.v.w));
  if (any(lessThan(v, ivec3(0))) || any(greaterThanEqual(v, ivec3(128)))) return false;
  uvec4 c = texelFetch(occ, v.xz, 0);
  return ((c[v.y >> 5] >> uint(v.y & 31)) & 1u) != 0u;
}
float N(vec2 p) {
  vec2 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  return mix(mix(H(i), H(i + vec2(1, 0)), f.x), mix(H(i + vec2(0, 1)), H(i + vec2(1, 1)), f.x), f.y);
}
float H3(vec3 p) { return float(Hu(uvec3(ivec3(p) + 1048576)) >> 8) * (1.0 / 16777216.0); }
float N3(vec3 p) {
  vec3 i = floor(p), f = fract(p);
  f = f * f * (3.0 - 2.0 * f);
  float a = mix(mix(H3(i), H3(i + vec3(1, 0, 0)), f.x), mix(H3(i + vec3(0, 1, 0)), H3(i + vec3(1, 1, 0)), f.x), f.y);
  float b = mix(mix(H3(i + vec3(0, 0, 1)), H3(i + vec3(1, 0, 1)), f.x), mix(H3(i + vec3(0, 1, 1)), H3(i + vec3(1, 1, 1)), f.x), f.y);
  return mix(a, b, f.z);
}
vec3 World(vec2 frag, float d) {
  vec2 uv = frag * pc.a.yz;
  vec4 w = pc.m * vec4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, 1.0 - d, 1.0);
  return w.xyz / w.w;
}
float Lum(vec3 c) { return dot(c, vec3(0.3, 0.59, 0.11)); }
void main() {
  ivec2 ip = ivec2(gl_FragCoord.xy);
  ivec2 sz = textureSize(depth, 0) - 1;
  vec4 c = texelFetch(albedo, ip, 0);
  float d = texelFetch(depth, ip, 0).r;
  if (d <= 0.0) {
    o = c;
    return;
  }
  if (pc.a.w > 0.5) {
    // (Diagnostics: the normals' w, the game's material.)
    float mw = texelFetch(normals, ip, 0).w;
    o = vec4(mw, fract(mw * 4.0), fract(mw * 16.0), 1.0);
    return;
  }
  vec3 n = normalize(texelFetch(normals, ip, 0).xyz * 2.0 - 1.0);
  vec3 P = World(gl_FragCoord.xy, d);
  float k = pc.a.x;
  float dist = length(P.xz - pc.b.zw);
  // The texture's own detail (against its neighbourhood): kept in the snow,
  // its bright parts (what's lit from above: the tops of needles, bark
  // ridges) catching it first, its dark gaps last.
  float lum = Lum(c.rgb);
  float mean = 0.0;
  for (int t = 0; t < 4; ++t) {
    ivec2 off = ivec2(t == 0 ? 2 : (t == 1 ? -2 : 0), t == 2 ? 2 : (t == 3 ? -2 : 0));
    mean += Lum(texelFetch(albedo, clamp(ip + off, ivec2(0), sz), 0).rgb);
  }
  mean = max(mean * 0.25, 0.02);
  float detail = clamp(lum / mean, 0.4, 1.7);
  // Foliage (green): its normals point up across whole boughs, so not by
  // them: by its top edges on screen (what's above it farther off, or sky:
  // the upper side of a bough) and its lit needles.
  float foliage = smoothstep(0.015, 0.06, c.g - max(c.r, c.b) * 0.92);
  float edge = 0.0;
  for (int t = 0; t < 3; ++t) {
    int s = t == 0 ? 1 : (t == 1 ? 3 : 6);
    ivec2 q = ivec2(ip.x, max(ip.y - s, 0));
    float dq = texelFetch(depth, q, 0).r;
    float gap = dq <= 0.0 ? 1e9 : length(World(vec2(q) + 0.5, dq).xz - pc.b.zw) - dist;
    edge = max(edge, smoothstep(0.25, 1.0, gap) * (t == 0 ? 1.0 : (t == 1 ? 0.75 : 0.45)));
  }
  // Patchy: by where it is (3D: no streaks down walls and trunks), two scales.
  float big = 0.65 * N(P.xz * 1.3) + 0.35 * N(P.xz * 5.1 + P.y);
  float clump = N3(P * 7.0) * 0.6 + N3(P * 19.0) * 0.4;
  // Plastered on the side the wind blows at (hard in a blizzard).
  float wind_v = length(pc.b.xy);
  vec3 wdir = wind_v > 0.1 ? vec3(-pc.b.x, 0.0, -pc.b.y) / wind_v : vec3(0.0);
  float windward = max(dot(n, wdir), 0.0) * smoothstep(2.0, 12.0, wind_v);
  // Where it lies on solid things: up-facing first, steeper as it deepens;
  // on walls and posts a patchy crust, more on the windward side.
  float up = smoothstep(0.75 - 0.35 * k, 0.98 - 0.25 * k, n.y + 0.25 * (big - 0.5));
  float side = smoothstep(0.55, 0.85, clump + 0.25 * k + 0.35 * windward - 0.35 * max(-n.y, 0.0)) *
               (0.35 + 0.65 * windward) * step(-0.6, n.y);
  float solid = max(up, side * k) + edge * 0.6 * step(-0.2, n.y);
  // On boughs: their top edges heavily, the rest in clumps on the lit
  // needles (the dark gaps between left green); farther off (a bough a
  // pixel or two) more evenly, still not all white.
  float near_f = smoothstep(60.0, 15.0, dist);
  float tufts = smoothstep(0.7 - 0.25 * k, 1.0 - 0.2 * k, clump * 0.7 + 0.45 * (detail - 1.0) + 0.2);
  float bough = max(edge * smoothstep(0.1, 0.5, k), tufts * 0.85) * mix(0.62, 1.0, near_f) + 0.15 * k * k;
  float lie = mix(solid, bough, foliage);
  float amount = clamp(lie, 0.0, 1.0) * smoothstep(0.0, 0.35, k + 0.3 * (big - 0.5)) *
                 min(0.92, k * 1.4);
  // Where the snow blows in (from the world light's voxels, when on): less
  // under a roof or a tree's crown, less in an obstacle's lee; drifted up
  // against the windward foot of whatever stands in the wind (a post, a
  // trunk, a wall), the drift deepening as it snows.
  if (pc.v.w > 0.0 && foliage < 0.5) {
    float vs = pc.v.w;
    // (Off the surface: out of its own voxel for a wall or a post.)
    vec3 Q = P + n * (n.y > 0.6 ? 0.3 : 1.2 * vs);
    // (Covered: something within 2..14 m above.)
    float roof = 0.0;
    for (int i = 0; i < 4; ++i) {
      if (Solid(Q + vec3(0.0, 2.0 + 4.0 * float(i), 0.0))) roof = 1.0;
    }
    amount *= 1.0 - 0.6 * roof;
    if (wind_v > 1.0) {
      vec3 wd = -wdir;  // (where the wind goes)
      // In the lee: something just upwind at this height.
      float lee = 0.0;
      for (int i = 1; i <= 3; ++i) {
        if (Solid(Q + vec3(0.0, 0.6, 0.0) - wd * (float(i) * 1.2 * vs))) lee = max(lee, 1.0 - float(i - 1) * 0.3);
      }
      amount *= 1.0 - 0.45 * lee * smoothstep(2.0, 10.0, wind_v);
      // At the windward foot of something: ground just upwind of it (or its
      // own windward side, low down). How high: the ground under the
      // windward side found down the column.
      float foot = 0.0;
      float deep = (0.25 + 0.9 * k) * smoothstep(1.5, 8.0, wind_v);
      if (n.y > 0.6) {
        for (int i = 1; i <= 3; ++i) {
          if (Solid(Q + vec3(0.0, 0.5, 0.0) + wd * (float(i) * 0.8 * vs))) foot = max(foot, 1.0 - float(i - 1) * 0.3);
        }
      } else if (windward > 0.15) {
        // (Its own side: how far above the ground just out in front of it.)
        vec3 F = P + wdir * 0.8 * vs;
        float above = 99.0;
        for (int i = 0; i < 4; ++i) {
          if (Solid(F - vec3(0.0, float(i) * vs, 0.0))) {
            above = float(i) * vs;
            break;
          }
        }
        foot = smoothstep(deep + 0.6, deep * 0.4, above) * smoothstep(0.15, 0.5, windward);
      }
      float pile = foot * deep * (0.7 + 0.6 * big);
      amount = max(amount, min(pile * 1.6, 0.95) * smoothstep(0.0, 0.2, k));
      if (pc.a.w < -0.5) {  // (diagnostics: the drifts red, the lee green, under cover blue)
        o = vec4(pile, lee, roof, 1.0);
        return;
      }
    }
  }
  // The snow itself: not flat white: shaded with the texture's detail, a
  // little blue in its hollows, brighter clumps.
  vec3 snow = vec3(0.86, 0.89, 0.93) * (0.9 + 0.12 * clump);
  snow *= mix(1.0, detail, 0.45);
  snow = mix(snow, snow * vec3(0.88, 0.93, 1.02), smoothstep(1.0, 0.6, detail) * 0.6);
  // (Already white: left as it is.)
  o = vec4(mix(c.rgb, max(c.rgb, snow), amount), c.a);
}
)", true);
  // Bloom: 0 the bright parts (soft threshold), 1 a 9-tap blur along a
  // direction, 2 added over the scene.
  copy_fs_[41] = CompileBuiltin("builtin_bloom", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;    // (linear)
layout(set = 0, binding = 1) uniform sampler2D scene;
layout(push_constant) uniform Bloom {
  vec4 a;  // mode, threshold or strength, step x, step y
  vec4 b;
} pc;
layout(location = 0) out vec4 o;
void main() {
  int mode = int(pc.a.x + 0.5);
  if (mode == 0) {
    // A quarter-size pixel: its 4x4 as 4 linear taps; what's over the
    // threshold, softly.
    vec2 uv = (floor(gl_FragCoord.xy) * 4.0 + 2.0) * pc.a.zw;
    vec3 c = vec3(0.0);
    c += textureLod(src, uv + vec2(-1.0, -1.0) * pc.a.zw, 0.0).rgb;
    c += textureLod(src, uv + vec2(1.0, -1.0) * pc.a.zw, 0.0).rgb;
    c += textureLod(src, uv + vec2(-1.0, 1.0) * pc.a.zw, 0.0).rgb;
    c += textureLod(src, uv + vec2(1.0, 1.0) * pc.a.zw, 0.0).rgb;
    c *= 0.25;
    // (Small bright things glow, broad lit ground not: what's over the
    // wider surroundings.)
    vec3 wide = vec3(0.0);
    for (int i = 0; i < 8; ++i) {
      float a = float(i) * 0.785398;
      wide += textureLod(src, uv + vec2(cos(a), sin(a)) * 40.0 * pc.a.zw, 0.0).rgb;
    }
    wide *= 0.125;
    c = max(c - wide * 0.7, vec3(0.0)) / 0.3 * pc.b.x + c * (1.0 - pc.b.x);
    float l = max(c.r, max(c.g, c.b));
    float knee = 0.12;
    float x = clamp(l - pc.a.y + knee, 0.0, 2.0 * knee);
    float over = max(l - pc.a.y, x * x / (4.0 * knee));
    o = vec4(c * over / max(l, 1e-4), 1.0);
  } else if (mode == 1) {
    vec2 uv = gl_FragCoord.xy / vec2(textureSize(src, 0));
    const float w[5] = float[](0.227, 0.195, 0.122, 0.054, 0.016);
    vec3 c = textureLod(src, uv, 0.0).rgb * w[0];
    for (int i = 1; i < 5; ++i) {
      c += textureLod(src, uv + pc.a.zw * float(i), 0.0).rgb * w[i];
      c += textureLod(src, uv - pc.a.zw * float(i), 0.0).rgb * w[i];
    }
    o = vec4(c, 1.0);
  } else if (mode == 3) {
    // Light pillars: what's bright drawn up in a column above it (and a
    // short way below), a little wider than it. (a.w: a step, of the height)
    vec2 size = vec2(textureSize(src, 0));
    vec2 uv = gl_FragCoord.xy / size;
    float dx = 1.0 / size.x;
    vec3 acc = vec3(0.0);
    for (int k = 1; k <= 48; ++k) {
      float f = float(k) / 48.0;
      vec2 at = uv + vec2(0.0, pc.a.w * float(k));
      vec3 s = textureLod(src, at, 0.0).rgb * 0.5 + textureLod(src, at - vec2(dx, 0.0), 0.0).rgb * 0.25 +
               textureLod(src, at + vec2(dx, 0.0), 0.0).rgb * 0.25;
      acc += s * pow(1.0 - f, 1.5);
      if (k <= 14) {
        at = uv - vec2(0.0, pc.a.w * float(k));
        acc += textureLod(src, at, 0.0).rgb * pow(1.0 - float(k) / 14.0, 1.5) * 0.5;
      }
    }
    o = vec4(acc * pc.a.y, 1.0);
  } else if (mode == 4) {
    // (Two the same size added: the glow and the pillars.)
    ivec2 p = ivec2(gl_FragCoord.xy);
    o = vec4(texelFetch(src, p, 0).rgb + texelFetch(scene, p, 0).rgb * pc.a.y, 1.0);
  } else {
    vec4 c = texelFetch(scene, ivec2(gl_FragCoord.xy), 0);
    vec2 uv = gl_FragCoord.xy * pc.a.zw;
    vec3 b = textureLod(src, uv, 0.0).rgb;
    o = vec4(c.rgb + b * pc.a.y * (1.0 - 0.6 * clamp(c.rgb, 0.0, 1.0)), c.a);
  }
}
)", true);
  copy_fs_[32] = CompileBuiltin("builtin_sun_tiles", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(push_constant) uniform Tiles { vec4 a; } pc;  // a.x: block size (8: from the map; 1: spread)
layout(location = 0) out vec4 o;
void main() {
  ivec2 size = textureSize(src, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  float m = 1.0;
  if (pc.a.x > 1.5) {
    for (int y = 0; y < 8; ++y)
      for (int x = 0; x < 8; ++x) m = min(m, texelFetch(src, min(p * 8 + ivec2(x, y), size - 1), 0).b);
  } else {
    for (int y = -4; y <= 4; ++y)
      for (int x = -4; x <= 4; ++x) m = min(m, texelFetch(src, clamp(p + ivec2(x, y), ivec2(0), size - 1), 0).r);
  }
  o = vec4(m);
}
)", true);
  // World shadows (BakeWorldShadows), 1: the slopes' painted shadow map
  // magnified (its x: 1 lit).
  copy_fs_[25] = CompileBuiltin("builtin_bake_init", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;  // linear
layout(push_constant) uniform Init { vec4 a; } pc;   // 1 / this image's size
layout(location = 0) out vec4 o;
void main() {
  o = vec4(textureLod(src, gl_FragCoord.xy * pc.a.xy, 0.0).r);
}
)", true);
  // World shadows, 2: each texel's surface (the height map) seen from the
  // sun through the world map: percentage-closer soft shadows, the casters
  // found against the ground's own slope (a receiver plane from the ground
  // map), so slopes don't shade themselves.
  // c5: world x, z -> u, v. hm: the height map (1 / R, -x0 / R, -z0 / R,
  // height at depth 0.5). o0-o2: the sun maps' rows. p: penumbra radius per
  // metre, smallest radius (m), metres per unit of depth, half width (m).
  // q: baked within (m) of the camera x, z. sz: this image's size.
  copy_fs_[26] = CompileBuiltin("builtin_bake", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D heights;  // from above: depth, 1 = nothing
layout(set = 0, binding = 1) uniform sampler2D world;    // from the sun: depth, 1 = nothing
layout(set = 0, binding = 2) uniform sampler2D seen;     // rays whose sunward side was in view
layout(push_constant) uniform Bake {
  vec4 c5; vec4 hm; vec4 o0; vec4 o1; vec4 o2; vec4 p; vec4 q; vec4 sz;
} pc;
layout(location = 0) out vec4 o;
vec2 Disc(int i, int n) {
  float r = sqrt((float(i) + 0.5) / float(n));
  float t = float(i) * 2.39996323;
  return r * vec2(cos(t), sin(t));
}
void main() {
  vec2 uv = gl_FragCoord.xy / pc.sz.xy;
  vec2 xz = (uv - pc.c5.zw) / pc.c5.xy;
  float from_cam = length(xz - pc.q.yz);
  if (from_cam > pc.q.x) discard;
  // (The time of day, near the camera: left lit - the slopes' shadows there
  // come sharper from the screen's mask, at the sun maps' detail rather than
  // this map's quarter metre. sz.z: that far all lit, sz.w: then fading in.)
  float near_lit = pc.sz.z > 0.0 ? 1.0 - smoothstep(pc.sz.z, pc.sz.z + pc.sz.w, from_cam) : 0.0;
  if (near_lit >= 0.999) {
    o = vec4(1.0);
    return;
  }
  ivec2 hn = textureSize(heights, 0);
  vec2 ht = (0.5 + 0.5 * (xz * pc.hm.x + pc.hm.yz)) * vec2(hn);
  if (any(lessThan(ht, vec2(0.0))) || any(greaterThanEqual(ht, vec2(hn)))) discard;
  float hz = texelFetch(heights, ivec2(ht), 0).r;
  if (hz >= 1.0) discard;  // not seen yet: as it was
  vec4 P = vec4(xz.x, pc.hm.w + (0.5 - hz) * pc.p.z, xz.y, 1.0);
  vec2 st = vec2(dot(pc.o0, P), dot(pc.o1, P));
  float zr = dot(pc.o2, P);
  ivec2 n = textureSize(world, 0);
  vec2 tc = (0.5 + 0.5 * st) * vec2(n);
  if (any(lessThan(tc, vec2(1.0))) || any(greaterThanEqual(tc, vec2(n) - 1.0))) discard;
  // Pass 1 (q.w = 0): rays whose sunward side has been in view (what casts
  // on them has been drawn): baked outright. Pass 2 (q.w = 1): the rest,
  // only ever darker than the painted map (blended by min): whatever casts
  // on them may not have been drawn yet (a tree's crown above the view).
  // (q.w = 2, the time of day: every ray, outright.)
  bool sunward_seen = texelFetch(seen, ivec2(tc), 0).r > 0.5;
  if (pc.q.w < 1.5 && sunward_seen == (pc.q.w > 0.5)) discard;
  float tm = 2.0 * pc.p.w / float(n.x);  // metres per texel
  float zpm = 1.0 / pc.p.z;               // depth per metre
  // The surface's slope (from the height map: the gentler side each way, so
  // edges don't tilt it) as a plane seen along the sun: how much nearer or
  // farther the neighbouring rays meet it (depth per texel, at most 2 m per
  // metre), so slopes don't shade themselves.
  ivec2 hc = ivec2(ht);
  float hstep = 2.0 / (pc.hm.x * float(hn.x));  // metres per height texel
  float hx0 = texelFetch(heights, clamp(hc - ivec2(1, 0), ivec2(0), hn - 1), 0).r;
  float hx1 = texelFetch(heights, clamp(hc + ivec2(1, 0), ivec2(0), hn - 1), 0).r;
  float hy0 = texelFetch(heights, clamp(hc - ivec2(0, 1), ivec2(0), hn - 1), 0).r;
  float hy1 = texelFetch(heights, clamp(hc + ivec2(0, 1), ivec2(0), hn - 1), 0).r;
  float dx = (hx0 < 1.0 && hx1 < 1.0) ? (abs(hx1 - hz) < abs(hz - hx0) ? hz - hx1 : hx0 - hz) : 0.0;
  float dz = (hy0 < 1.0 && hy1 < 1.0) ? (abs(hy1 - hz) < abs(hz - hy0) ? hz - hy1 : hy0 - hz) : 0.0;
  // (depth from above falls as the height rises: dh = -dz * D)
  vec3 nrm = normalize(vec3(-dx * pc.p.z / hstep, 1.0, -dz * pc.p.z / hstep));
  vec3 e1 = normalize(pc.o0.xyz), e2 = normalize(pc.o1.xyz), fd = normalize(pc.o2.xyz);
  float nf = dot(nrm, fd);
  vec2 grad = abs(nf) > 0.05 ? -vec2(dot(nrm, e1), dot(nrm, e2)) / nf : vec2(0.0);
  grad = clamp(grad, vec2(-2.0), vec2(2.0)) * tm * zpm;
  float bias = (0.25 + 1.5 * tm) * zpm;
  // (Searched a few metres around at least.)
  float reach = clamp(max(pc.p.x * 30.0, 4.0) / tm, 2.0, 64.0);
  float bs = 0.0, bn = 0.0;
  for (int i = 0; i < 24; ++i) {
    vec2 off = Disc(i, 24) * reach;
    float z = texelFetch(world, ivec2(tc + off), 0).r;
    if (z < 1.0 && z < zr + dot(grad, off) - bias) {
      bs += zr - z;
      bn += 1.0;
    }
  }
  if (bn == 0.0) {
    if (pc.q.w > 0.5 && pc.q.w < 1.5) discard;
    o = vec4(1.0);
    return;
  }
  float dist = min(bs / bn * pc.p.z, 60.0);
  float rad = clamp((pc.p.x * dist + pc.p.y) / tm, 0.75, 48.0);
  float sh = 0.0;
  for (int i = 0; i < 32; ++i) {
    vec2 off = Disc(i, 32) * rad;
    float z = texelFetch(world, ivec2(tc + off), 0).r;
    sh += (z < 1.0 && z < zr + dot(grad, off) - bias) ? 1.0 : 0.0;
  }
  o = vec4(mix(1.0 - sh / 32.0, 1.0, near_lit));
}
)", true);
  // Shaded riders, 1 (half size): the pixels drawn after the lighting pass
  // (riders and the like: the late depth differs from the world's, and from
  // what the depth-only passes left), as world positions (w = 1).
  copy_fs_[27] = CompileBuiltin("builtin_movers_pos", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D late;
layout(set = 0, binding = 1) uniform sampler2D world;
layout(set = 0, binding = 2) uniform sampler2D hidden;
layout(push_constant) uniform Pos { mat4 m; } pc;  // ndc (x, y, 1 - depth, 1) -> world
layout(location = 0) out vec4 o;
void main() {
  ivec2 size = textureSize(late, 0);
  // Of the 2 x 2 pixels, the one nearest the camera that's a mover.
  o = vec4(0.0);
  float best = 0.0;
  ivec2 bp = ivec2(0);
  for (int k = 0; k < 4; ++k) {
    ivec2 p = min(ivec2(gl_FragCoord.xy) * 2 + ivec2(k & 1, k >> 1), size - 1);
    float l = texelFetch(late, p, 0).r;
    if (l <= best || l == texelFetch(world, p, 0).r || l == texelFetch(hidden, p, 0).r) continue;
    best = l;
    bp = p;
  }
  if (best <= 0.0) return;
  vec2 uv = (vec2(bp) + 0.5) / vec2(size);
  vec2 ndc = vec2(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0);
  vec4 w = pc.m * vec4(ndc, 1.0 - best, 1.0);
  // w: 1e5 + the height of what's behind it (the ground near it, about).
  vec4 g = pc.m * vec4(ndc, 1.0 - texelFetch(world, bp, 0).r, 1.0);
  float gy = abs(g.w) > 1e-7 ? g.y / g.w : w.y / w.w;
  if (abs(w.w) > 1e-7) o = vec4(w.xyz / w.w, 1e5 + min(gy, w.y / w.w));
}
)", true);
  // Shaded riders, 2 (half size): each one's light seen from the sun,
  // through the world map (soft as the world's shadows) and through the
  // game's own shadow map (the riders themselves: an arm on the body, the
  // body on the legs), as a factor (1: in the sun). Only on the surfaces
  // facing the sun: those facing away are already dark.
  // o0-o2: the world map's rows (world -> ndc, depth). l: world -> the shadow
  // map's texel (u, v, depth). p: penumbra radius per metre, the shadow map's
  // depth per metre towards the sun (0: none this frame), how much light the
  // shade takes, diagnostics.
  copy_fs_[28] = CompileBuiltin("builtin_movers_shade", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D pos;
layout(set = 0, binding = 1) uniform sampler2D world;  // depth, 1 = nothing
layout(set = 0, binding = 2) uniform sampler2D zo;     // the shadow pass's depth (nearer the sun: more)
layout(set = 0, binding = 3) uniform sampler2D baked;  // the world shadows (p.w = 2)
layout(push_constant) uniform Shade { vec4 o0; vec4 o1; vec4 o2; mat4 l; vec4 p; } pc;
layout(location = 0) out vec4 o;
vec2 Disc(int i, int n) {
  float r = sqrt((float(i) + 0.5) / float(n));
  float t = float(i) * 2.39996323;
  return r * vec2(cos(t), sin(t));
}
void main() {
  ivec2 c = ivec2(gl_FragCoord.xy);
  vec4 q = texelFetch(pos, c, 0);
  o = vec4(1.0);
  if (q.w < 0.5) return;
  vec4 P = vec4(q.xyz, 1.0);
  vec3 fd = normalize(pc.p.w > 1.5 ? pc.o1.xyz : pc.o2.xyz);  // the sun's direction (travelling)
  float D = 1.0 / length(pc.o2.xyz), R = 1.0 / length(pc.o0.xyz);
  // The surface's normal (from its neighbours) and how much it faces the sun.
  // The neighbours a few pixels off, not the next ones: up close (a face in a
  // cutscene) the next pixel is a millimetre or two away, and the depth's
  // own steps tilted a normal from it this way and that from pixel to pixel,
  // which flipped the facing and showed as dark speckle in vertical runs.
  ivec2 ps = textureSize(pos, 0);
  float nd = 0.0;
  vec3 n = vec3(0.0);
  for (int k = 3; k >= 1; --k) {  // the widest that stays on the surface
    vec4 qx = texelFetch(pos, min(c + ivec2(k, 0), ps - 1), 0);
    vec4 qy = texelFetch(pos, min(c + ivec2(0, k), ps - 1), 0);
    vec4 qx0 = texelFetch(pos, max(c - ivec2(k, 0), ivec2(0)), 0);
    vec4 qy0 = texelFetch(pos, max(c - ivec2(0, k), ivec2(0)), 0);
    // Each side's neighbour, the nearer one in depth: across an edge (an
    // arm in front of the body) the other side's is a different surface.
    vec3 ex1 = qx.w > 0.5 ? qx.xyz - q.xyz : vec3(1e9), ex0 = qx0.w > 0.5 ? q.xyz - qx0.xyz : vec3(1e9);
    vec3 ey1 = qy.w > 0.5 ? qy.xyz - q.xyz : vec3(1e9), ey0 = qy0.w > 0.5 ? q.xyz - qy0.xyz : vec3(1e9);
    vec3 dx = dot(ex1, ex1) < dot(ex0, ex0) ? ex1 : ex0;
    vec3 dy = dot(ey1, ey1) < dot(ey0, ey0) ? ey1 : ey0;
    // (Screen right x screen down points away from the camera: the other way.)
    vec3 nk = cross(dy, dx);
    // A neighbour more than 15 cm a step away: an edge, try nearer.
    float lim = 0.0225 * float(k * k);
    if (dot(dx, dx) < lim && dot(dy, dy) < lim && dot(nk, nk) > 1e-14) {
      n = normalize(nk);
      nd = 1.0;
      break;
    }
  }
  // No normal (an edge, a thin part): half facing.
  bool normal_ok = nd > 0.5;
  float ndl = normal_ok ? dot(n, -fd) : 0.5;
  float facing = smoothstep(0.0, 0.5, ndl);
  // The world (trees, slopes): the world map; or, with only what's saved
  // (p.w = 2: o0 is c5, o1 the sun's direction), the baked shadows where the
  // sun's ray through it meets the ground.
  float world_sh = 0.0;
  ivec2 wn = textureSize(world, 0);
  vec2 tc = (0.5 + 0.5 * vec2(dot(pc.o0, P), dot(pc.o1, P))) * vec2(wn);
  if (pc.p.w > 1.5) {
    fd = normalize(pc.o1.xyz);
    // How high it is over the ground (what's behind it can be far down the
    // slope: at most 3 m, a rider's height and a jump's start), and the
    // ground the sun's ray through it meets there. The bake is finer than a
    // rider's shade needs (its thin tree shadows streaked them): a few taps
    // over 40 cm.
    float above = clamp(q.y - (q.w - 1e5), 0.0, 3.0);
    vec2 xz = q.xz + fd.xz * above / max(-fd.y, 0.25);
    vec2 uv = xz * pc.o0.xy + pc.o0.zw;
    vec2 r = 0.4 * abs(pc.o0.xy);
    float lit = 0.0;
    for (int i = 0; i < 8; ++i) lit += textureLod(baked, uv + Disc(i, 8) * r, 0.0).r;
    world_sh = 1.0 - lit / 8.0;
  } else if (all(greaterThanEqual(tc, vec2(1.0))) && all(lessThan(tc, vec2(wn) - 1.0))) {
    float zr = dot(pc.o2, P);
    float tm = 2.0 * R / float(wn.x);
    float bias = (0.3 + 1.5 * tm) / D;
    // The ground behind it along the sun tilts (the world map's own slope
    // there, the gentler side): neighbouring rays meet it nearer or farther.
    ivec2 t = ivec2(tc);
    float g0 = texelFetch(world, t, 0).r;
    float gx0 = texelFetch(world, t - ivec2(1, 0), 0).r, gx1 = texelFetch(world, t + ivec2(1, 0), 0).r;
    float gy0 = texelFetch(world, t - ivec2(0, 1), 0).r, gy1 = texelFetch(world, t + ivec2(0, 1), 0).r;
    vec2 grad = vec2(abs(gx1 - g0) < abs(g0 - gx0) ? gx1 - g0 : g0 - gx0,
                     abs(gy1 - g0) < abs(g0 - gy0) ? gy1 - g0 : g0 - gy0);
    grad = max(max(g0, max(gx0, gx1)), max(gy0, gy1)) < 1.0
               ? clamp(grad, vec2(-2.0 * tm / D), vec2(2.0 * tm / D)) : vec2(0.0);
    float reach = clamp(pc.p.x * 30.0 / tm, 2.0, 48.0);
    float bs = 0.0, bn = 0.0;
    for (int i = 0; i < 16; ++i) {
      vec2 off = Disc(i, 16) * reach;
      float z = texelFetch(world, ivec2(tc + off), 0).r;
      if (z < 1.0 && z < zr + dot(grad, off) - bias) {
        bs += zr - z;
        bn += 1.0;
      }
    }
    if (bn > 0.0) {
      if (pc.p.w > 0.5 && pc.p.w < 1.5) {  // diagnostics: the casters' distance (/ 30 m)
        o = vec4(min(bs / bn * D / 30.0, 1.0));
        return;
      }
      float rad = clamp((pc.p.x * min(bs / bn * D, 60.0) + 0.05) / tm, 0.75, 48.0);
      for (int i = 0; i < 24; ++i) {
        vec2 off = Disc(i, 24) * rad;
        float z = texelFetch(world, ivec2(tc + off), 0).r;
        world_sh += (z < 1.0 && z < zr + dot(grad, off) - bias) ? 1.0 / 24.0 : 0.0;
      }
    }
  }
  // Themselves (and the riders, rails, objects the game casts): its map.
  float self_sh = 0.0;
  // (p.y < 0: the map's depth grows away from the sun, 1 = nothing.)
  bool away_up = pc.p.y < 0.0;
  float dpm = abs(pc.p.y);
  if (dpm > 0.0) {
    // Looked up a little off the surface, along its normal (and towards
    // the sun): a surface steep to the sun crosses several of the map's
    // depths within a texel, and compared at the surface itself it shades
    // itself in stripes (shadow acne) that crawl as the rider moves.
    ivec2 zn = textureSize(zo, 0);
    vec4 m0 = pc.l * P;
    vec4 m1 = pc.l * vec4(P.xyz + vec3(1.0, 0.0, 0.0), 1.0);
    float texel_m = 1.0 / max(length((m1.xy / m1.w - m0.xy / m0.w) * vec2(zn)), 1e-3);  // metres per texel
    // (The time of day's map: texels of a few cm, finer than the
    // surface's normal from the depth is sure: a few cm off at least.)
    vec3 off = away_up ? (n * 1.0 - fd * 0.5) * max(texel_m, 0.05) : (n * 0.75 - fd * 0.25) * texel_m;
    vec4 m = pc.l * vec4(P.xyz + off, 1.0);
    vec3 s = m.xyz / m.w;  // texel u, v (0..1) and depth
    if (all(greaterThan(s.xy, vec2(0.0))) && all(lessThan(s.xy, vec2(1.0)))) {
      // (The time of day's map is finer: a little more, against stripes.)
      float bias = (away_up ? 0.12 + 0.3 * (1.0 - clamp(ndl, 0.0, 1.0))
                            : 0.08 + 0.25 * (1.0 - clamp(ndl, 0.0, 1.0))) * dpm;
      // Each tap the four texels around it, weighted by how near it is to
      // each (as a comparison sampler filters): one texel's yes or no per tap
      // flipped from pixel to pixel as the rider moved, and showed as
      // speckle in runs down the clothes and faces up close.
      for (int i = 0; i < 16; ++i) {
        vec2 f = (s.xy + Disc(i, 16) * 3.0 / vec2(zn)) * vec2(zn) - 0.5;
        ivec2 t = ivec2(floor(f));
        vec2 fr = f - floor(f);
        vec4 z4 = vec4(texelFetch(zo, clamp(t, ivec2(0), zn - 1), 0).r,
                       texelFetch(zo, clamp(t + ivec2(1, 0), ivec2(0), zn - 1), 0).r,
                       texelFetch(zo, clamp(t + ivec2(0, 1), ivec2(0), zn - 1), 0).r,
                       texelFetch(zo, clamp(t + ivec2(1, 1), ivec2(0), zn - 1), 0).r);
        vec4 hit = away_up ? step(z4, vec4(s.z - bias)) : step(vec4(s.z + bias), z4);
        self_sh += mix(mix(hit.x, hit.y, fr.x), mix(hit.z, hit.w, fr.x), fr.y) / 16.0;
      }
    }
  }
  // (Under a cloud: the sun's light gone all round.)
  float cloud_sh = fract(pc.p.w) / 0.4;
  o = vec4((1.0 - pc.p.z * max(world_sh, self_sh) * facing) * (1.0 - pc.p.z * cloud_sh * facing));
}
)", true);
  // The sun maps' rays whose sunward side (the ground and 45 m towards the
  // sun) is in view this frame, marked (what's left stays as it was).
  // a: the maps' ndc and depth -> world; vp: world -> the camera's clip.
  copy_fs_[31] = CompileBuiltin("builtin_sun_seen", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D ground;  // the world map (what each ray meets first)
layout(push_constant) uniform Seen { mat4 a; mat4 vp; } pc;
layout(location = 0) out vec4 o;
bool InView(vec4 w) {
  vec4 c = pc.vp * vec4(w.xyz / w.w, 1.0);
  return c.w > 0.1 && all(lessThan(abs(c.xy), vec2(0.97 * c.w)));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 n = textureSize(ground, 0);
  float g = texelFetch(ground, p, 0).r;
  if (g >= 1.0) discard;
  vec2 ndc = (vec2(p) + 0.5) / vec2(n) * 2.0 - 1.0;
  if (!InView(pc.a * vec4(ndc, g, 1.0)) || !InView(pc.a * vec4(ndc, g - 0.0225, 1.0))) discard;
  o = vec4(1.0);
}
)", true);
  // Shaded riders, 3: the scene times that factor (from the half size
  // one) where a mover is (the late depth differs from the world's).
  copy_fs_[29] = CompileBuiltin("builtin_movers_apply", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D shade;  // linear, half size
layout(set = 0, binding = 1) uniform sampler2D scene;
layout(set = 0, binding = 2) uniform sampler2D late;
layout(set = 0, binding = 3) uniform sampler2D world;
layout(set = 0, binding = 4) uniform sampler2D gi;  // the world light's factor (half size), if pc.a.x
layout(push_constant) uniform MA { vec4 a; } pc;
layout(location = 0) out vec4 o;
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(scene, p, 0);
  float l = texelFetch(late, p, 0).r;
  bool mover = l > 0.0 && l != texelFetch(world, p, 0).r;
  vec2 uv = gl_FragCoord.xy / vec2(textureSize(scene, 0));
  float f = mover ? textureLod(shade, uv, 0.0).r : 1.0;
  vec3 g = vec3(1.0);
  if (mover && pc.a.x > 0.5) {
    // (Its half-size pixel's, or a neighbour's where that one missed it.)
    ivec2 h = ivec2(gl_FragCoord.xy) / 2;
    ivec2 hs = textureSize(gi, 0);
    vec4 b = texelFetch(gi, min(h, hs - 1), 0);
    for (int k = 0; k < 4 && b.a <= 0.0; ++k)
      b = texelFetch(gi, clamp(h + ivec2(k == 0 ? 1 : k == 1 ? -1 : 0, k == 2 ? 1 : k == 3 ? -1 : 0), ivec2(0), hs - 1), 0);
    if (b.a > 0.0) g = b.rgb;
  }
  o = vec4(c.rgb * f * g, c.a);
}
)", true);
  // Sun shadows, 2: the map filtered with penumbras that widen with the
  // distance from the caster to the ground (percentage-closer soft shadows:
  // the casters' average distance found first, then a disc that size). The
  // stencil byte (b: 1 lit, 0 in shadow) is what the lighting reads.
  // a: penumbra radius per metre, search radius (m), radius min / max
  // (texels). h0-h2: map (u, v, 1) -> the world map's ndc (to measure the
  // map's texels in metres); h0.w: the world map's half width (m), h1.w:
  // bias (m), h2.w: 1 when the casters' heights aren't known this frame.
  copy_fs_[24] = CompileBuiltin("builtin_sun_pcss", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D raw;    // linear
layout(set = 0, binding = 1) uniform sampler2D prep;
layout(set = 0, binding = 2) uniform sampler2D tiles;  // casters nearby (1/8 size; 1: none)
layout(push_constant) uniform Pcss { vec4 a; vec4 h0; vec4 h1; vec4 h2; } pc;
layout(location = 0) out vec4 o;
vec2 Disc(int i, int n) {
  float r = sqrt((float(i) + 0.5) / float(n));
  float t = float(i) * 2.39996323;
  return r * vec2(cos(t), sin(t));
}
vec2 Plane(vec2 uv) {
  vec3 u1 = vec3(uv, 1.0);
  vec3 s3 = vec3(dot(pc.h0.xyz, u1), dot(pc.h1.xyz, u1), dot(pc.h2.xyz, u1));
  return s3.xy / s3.z;
}
void main() {
  ivec2 size = textureSize(prep, 0);
  ivec2 p = ivec2(gl_FragCoord.xy);
  vec4 c = texelFetch(raw, p, 0);
  // No caster within reach: the stencil as it is (lit).
  if (texelFetch(tiles, p / 8, 0).r > 0.999) {
    o = c;
    return;
  }
  vec2 inv = 1.0 / vec2(size);
  vec2 uv = (vec2(p) + 0.5) * inv;
  vec2 s0 = Plane(uv);
  float mpt = max(length(Plane(uv + vec2(inv.x, 0.0)) - s0), length(Plane(uv + vec2(0.0, inv.y)) - s0)) * pc.h0.w;
  mpt = clamp(mpt, 1e-3, 10.0);
  float dr = texelFetch(prep, p, 0).x;
  float bias = pc.h1.w + 2.0 * mpt;
  float reach = clamp(pc.a.y / mpt, 1.0, pc.a.w);
  float bs = 0.0, bn = 0.0;
  for (int i = 0; i < 16; ++i) {
    ivec2 s = clamp(p + ivec2(round(Disc(i, 16) * reach)), ivec2(0), size - 1);
    float od = texelFetch(prep, s, 0).y;
    if (od < dr - bias) {
      bs += dr - od;
      bn += 1.0;
    }
  }
  float rad = pc.a.z;
  if (bn > 0.0) rad = clamp(pc.a.x * min(bs / bn, 40.0) / mpt, pc.a.z, pc.a.w);
  // Without the casters' heights this frame: a rider's height, about.
  else if (pc.h2.w > 0.5) rad = clamp(pc.a.x * 1.5 / mpt, pc.a.z, pc.a.w);
  float lit = 0.0;
  for (int i = 0; i < 24; ++i) lit += texture(raw, (vec2(p) + 0.5 + Disc(i, 24) * rad) * inv).b;
  o = vec4(c.r, c.g, lit / 24.0, c.a);
}
)", true);
  copy_fs_[17] = CompileBuiltin("builtin_clouds", std::string(R"(#version 450
)") + clouds_common + R"(
layout(binding = 2) uniform sampler2D sprite;
layout(location = 0) flat in vec4 v_puff;
layout(location = 1) flat in vec3 v_turn;
layout(location = 0) out vec4 o;
// Signed distance to a puff (shape units, about 1 across): a body and six
// bumps around it.
float Puff(vec2 q, float id) {
  float d = length(q - vec2(0.0, 0.08)) - 0.56;
  for (int k = 0; k < 6; ++k) {
    float a = float(k) * 1.0472 + hash(id * 7.1 + float(k)) * 0.5;
    float r = 0.33 + 0.12 * hash(id * 3.7 + float(k) * 1.3);
    vec2 c = vec2(cos(a), sin(a)) * (0.52 + 0.08 * hash(id + float(k) * 9.1));
    d = min(d, length(q - c) - r);
  }
  return d;
}
void main() {
  float px = pc.a.z;
  vec2 uv = gl_FragCoord.xy * px;
  bool textured = pc.a.w > 0.5;
  float line = 0.0055;   // outline width (procedural)
  vec2 local = (uv - v_puff.xy) / v_puff.z;
  if (dot(local, local) > 2.6) discard;
  float id = v_turn.z;
  vec3 col;
  float cover;
  if (textured) {
    float ang = v_turn.x;
    vec2 l = mat2(cos(ang), -sin(ang), sin(ang), cos(ang)) * local;
    if (v_turn.y > 0.5) l.x = -l.x;
    vec2 suv = l / 2.3 + 0.5;
    if (any(lessThan(suv, vec2(0.0))) || any(greaterThan(suv, vec2(1.0)))) discard;
    vec4 t = textureLod(sprite, suv, 0.0);
    cover = t.a;
    col = t.rgb;
  } else {
    float d = Puff(local, id) * v_puff.z;  // screen heights
    cover = clamp(0.5 - d / px, 0.0, 1.0);
    // White, a touch blue toward the bottom, with a dark outline.
    vec3 fill = mix(vec3(1.0), vec3(0.86, 0.9, 0.98), clamp(local.y * 0.45 + 0.35, 0.0, 1.0) * 0.6);
    float edge = clamp(0.5 + (d + line) / px, 0.0, 1.0);
    col = mix(fill, vec3(0.06, 0.06, 0.08), edge);
  }
  cover *= v_puff.w;
  if (cover <= 0.0) discard;
  o = vec4(col, cover);
}
)", true);
  // Step 2: the largest motion in each tile (tile = the longest half blur),
  // in two passes: along rows (a.w = 0), then columns (a.w = 1).
  copy_fs_[7] = CompileBuiltin("builtin_motion_tile_max", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D motion;
layout(push_constant) uniform Tile { vec4 a; } pc;  // tile size, -, -, axis
layout(location = 0) out vec4 o;
void main() {
  ivec2 t = ivec2(gl_FragCoord.xy);
  int n = int(pc.a.x);
  ivec2 lim = textureSize(motion, 0) - 1;
  ivec2 step = pc.a.w > 0.5 ? ivec2(0, 1) : ivec2(1, 0);
  ivec2 start = pc.a.w > 0.5 ? ivec2(t.x, t.y * n) : ivec2(t.x * n, t.y);
  vec2 best = vec2(0.0);
  float bl = 0.0;
  for (int i = 0; i < n; ++i) {
    vec2 m = texelFetch(motion, min(start + step * i, lim), 0).xy;
    float l = dot(m, m);
    if (l > bl) {
      bl = l;
      best = m;
    }
  }
  o = vec4(best, 0.0, 0.0);
}
)", true);
  // Step 3: the largest motion among each tile and its 8 neighbours (what can
  // reach into the tile).
  copy_fs_[8] = CompileBuiltin("builtin_motion_neighbor_max", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D tiles;
layout(location = 0) out vec4 o;
void main() {
  ivec2 t = ivec2(gl_FragCoord.xy);
  ivec2 lim = textureSize(tiles, 0) - 1;
  vec2 best = vec2(0.0);
  float bl = 0.0;
  for (int y = -1; y <= 1; ++y) {
    for (int x = -1; x <= 1; ++x) {
      vec2 m = texelFetch(tiles, clamp(t + ivec2(x, y), ivec2(0), lim), 0).xy;
      float l = dot(m, m);
      if (l > bl) {
        bl = l;
        best = m;
      }
    }
  }
  o = vec4(best, 0.0, 0.0);
}
)", true);
  // Step 4: the scene sampled along the neighbourhood's largest motion, each
  // sample weighted by whether it reaches this pixel (McGuire et al. 2012,
  // "A reconstruction filter for plausible motion blur"): a moving object in
  // front smears over what's behind it as far as its own motion goes (past
  // its outline), what's behind shows through a blurred object's edges, and
  // nothing smears over a sharp object in front (the player).
  copy_fs_[5] = CompileBuiltin("builtin_motion_blur", R"(#version 450
layout(set = 0, binding = 0) uniform sampler2D src;
layout(set = 0, binding = 1) uniform sampler2D motion;
layout(set = 0, binding = 2) uniform sampler2D nmax;
layout(push_constant) uniform Blur {
  vec4 size;      // 1 / width, 1 / height, width, height
  vec4 params;    // tile size, taps, debug view
} pc;
layout(location = 0) out vec4 o;
float cone(float d, float h) { return h > 0.0 ? clamp(1.0 - d / h, 0.0, 1.0) : 0.0; }
float cylinder(float d, float h) { return 1.0 - smoothstep(0.95 * h, 1.05 * h + 0.01, d); }
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  ivec2 lim = ivec2(pc.size.zw) - 1;
  vec4 centre = texelFetch(src, p, 0);
  vec4 mc = texelFetch(motion, p, 0);
  if (pc.params.z > 0.5) {
    o = mc.w > 0.75 ? vec4(0.8, 0.8, 0.8, centre.a)
                    : vec4(clamp(abs(mc.xy) / 40.0, 0.0, 1.0), mc.w > 0.25 ? 0.6 : 0.0, centre.a);
    return;
  }
  if (mc.w > 0.75) {  // the player, event icons: never blurred
    o = centre;
    return;
  }
  vec2 vn = texelFetch(nmax, p / int(pc.params.x), 0).xy;
  if (dot(vn, vn) < 1.0) {  // under a pixel of motion around here
    o = centre;
    return;
  }
  float hc = length(mc.xy) * 0.5;  // half extents: the blur is centred
  float zc = mc.z;
  float wsum = 1.0 / max(hc, 1.0);
  vec3 sum = centre.rgb * wsum;
  int taps = int(pc.params.y);
  float jitter = fract(52.9829189 * fract(dot(vec2(p), vec2(0.06711056, 0.00583715))));
  for (int i = 0; i < taps; ++i) {
    float t = mix(-1.0, 1.0, (float(i) + jitter) / float(taps));
    vec2 off = vn * 0.5 * t;
    ivec2 q = clamp(ivec2(floor(vec2(p) + 0.5 + off)), ivec2(0), lim);
    vec4 mq = texelFetch(motion, q, 0);
    float hq = length(mq.xy) * 0.5;
    float d = length(off);
    float soft = 0.5 + 0.03 * min(zc, mq.z);
    float front = clamp(1.0 - (mq.z - zc) / soft, 0.0, 1.0);  // the sample is in front
    float back = clamp(1.0 - (zc - mq.z) / soft, 0.0, 1.0);   // this pixel is in front
    float w = front * cone(d, hq) + back * cone(d, hc) + cylinder(d, hq) * cylinder(d, hc) * 2.0;
    sum += texelFetch(src, q, 0).rgb * w;
    wsum += w;
  }
  o = vec4(sum / wsum, centre.a);
}
)", true);
  // Object motion: the screen motion of the surface between the velocity
  // vertex shader's two positions (this frame's, last frame's), in uv; z = 1
  // when last frame's constants were found (XE_FLAG_VELOCITY), else 0 (the
  // blur uses the camera's motion there); w = 1 on the player character, 0.5
  // on other characters (drawn after the lighting pass), 0 on the scenery.
  exempt_ps_.recipe = 5;
  vel_ps_.recipe = 4;
  exempt_ps_.module = CompileBuiltin("builtin_exempt", R"(#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, 0.0, 0.0, 2.0); }  // (2: an event icon; the player's is 1)
)", true);
  vel_ps_.module = CompileBuiltin("builtin_velocity", R"(#version 450
layout(set = 0, binding = 0, std140) uniform XeSystem {
  vec4 ndc_scale;
  vec4 ndc_offset;
  uint flags;
} xe_sys;
layout(location = 0) in vec4 cur;
layout(location = 1) in vec4 prev;
layout(location = 0) out vec4 o;
void main() {
  bool ok = (xe_sys.flags & 1024u) != 0u && cur.w > 1e-6 && prev.w > 1e-6;
  vec2 d = ok ? (cur.xy / cur.w - prev.xy / prev.w) * 0.5 : vec2(0.0);
  o = vec4(d, ok ? 1.0 : 0.0,
           (xe_sys.flags & 2048u) != 0u ? 1.0 : (xe_sys.flags & 4096u) != 0u ? 0.5 : 0.0);
}
)", true);
}

// Per-frame buffers and descriptors, sized for the frame (grown, never shrunk).
void Renderer::Impl::BeginFrame() {
  VkDevice dev = gpu_.device;
  const uint32_t draws = uint32_t(frame_->draws.size()) + 64;
  if (!pool_ || pool_draws_ < draws) {
    if (pool_) vk::vkDestroyDescriptorPool(dev, pool_, nullptr);
    pool_draws_ = draws + draws / 2;
    VkDescriptorPoolSize sizes[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC, 8},
                                    {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8},
                                    {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                                     pool_draws_ * 32 + 64}};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = pool_draws_ * 2 + 16;
    dpi.poolSizeCount = 3;
    dpi.pPoolSizes = sizes;
    vk::Check(vk::vkCreateDescriptorPool(dev, &dpi, nullptr, &pool_), "descriptor pool");
  } else {
    vk::vkResetDescriptorPool(dev, pool_, 0);
  }
  frame_tex_lookup_.clear();
  // Texture set pools retired 3+ frames ago: no frame in flight binds their
  // sets now.
  for (size_t i = 0; i < retired_tex_set_pools_.size();) {
    if (frame_number_ >= retired_tex_set_pools_[i].frame + 3) {
      vk::vkDestroyDescriptorPool(dev, retired_tex_set_pools_[i].pool, nullptr);
      retired_tex_set_pools_[i] = retired_tex_set_pools_.back();
      retired_tex_set_pools_.pop_back();
    } else {
      ++i;
    }
  }
  // Object motion (blur, upscaler) adds up to a system block and a constant
  // block a draw.
  const VkDeviceSize uniform_bytes =
      VkDeviceSize(draws) * (object_blur_ || upscaler_ ? 18 : 12) * 1024;
  if (uniforms_.size < uniform_bytes) {
    gpu_.DestroyBuffer(uniforms_);
    uniforms_ = gpu_.CreateBuffer(uniform_bytes + uniform_bytes / 2, VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);
  }
  uniform_used_ = 0;
  for (auto& c : const_uploads_) c = {};
  const_upload_next_[0] = const_upload_next_[1] = 0;
  for (auto& c : tod_uploads_) c = {};
  tod_upload_next_[0] = tod_upload_next_[1] = 0;
  bool_loop_version_ = 0;
  last_fetch_offset_ = UINT32_MAX;
  last_sys_offset_ = UINT32_MAX;
  uint64_t index_bytes = 0;
  for (const Draw& d : frame_->draws) index_bytes += uint64_t(d.count) * 6 + 256;
  if (indices_.size < index_bytes) {
    gpu_.DestroyBuffer(indices_);
    indices_ = gpu_.CreateBuffer(index_bytes + index_bytes / 2, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
  }
  index_used_ = 0;

  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layouts_[0];
  vk::Check(vk::vkAllocateDescriptorSets(dev, &ai, &set0_), "set0");
  ai.pSetLayouts = &set_layouts_[1];
  vk::Check(vk::vkAllocateDescriptorSets(dev, &ai, &set1_), "set1");
  const VkDeviceSize ranges[5] = {sizeof(SystemConstants), 4096, 4096, 160, 768};
  VkDescriptorBufferInfo infos[7];
  VkWriteDescriptorSet writes[7];
  for (uint32_t i = 0; i < 5; ++i) {
    infos[i] = {uniforms_.buffer, 0, ranges[i]};
    writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    writes[i].dstSet = set0_;
    writes[i].dstBinding = i;
    writes[i].descriptorCount = 1;
    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC;
    writes[i].pBufferInfo = &infos[i];
  }
  infos[5] = {arena_.buffer, 0, VK_WHOLE_SIZE};
  writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  writes[5].dstSet = set1_;
  writes[5].dstBinding = 0;
  writes[5].descriptorCount = 1;
  writes[5].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  writes[5].pBufferInfo = &infos[5];
  infos[6] = {debug_.buffer, 0, VK_WHOLE_SIZE};
  writes[6] = writes[5];
  writes[6].dstBinding = 1;
  writes[6].pBufferInfo = &infos[6];
  vk::vkUpdateDescriptorSets(dev, 7, writes, 0, nullptr);
}

// After the frame has finished on the GPU: free its uploads and replaced
// images, and textures that haven't been used for a while.
void Renderer::Impl::EndFrame() {
  // Async: uploads and replaced images go with the frame's slot instead, and
  // are freed when its fence signals (FinishSlot).
  if (!options_.async) {
    for (auto& b : staging_) gpu_.DestroyBuffer(b);
    staging_.clear();
    for (auto& im : retired_images_) gpu_.DestroyImage(im);
    retired_images_.clear();
  }
  for (auto it = spare_resolves_.begin(); it != spare_resolves_.end();) {
    if (frame_number_ - it->second.frame > 600) {
      Retire(it->second.image);
      it = spare_resolves_.erase(it);
    } else {
      ++it;
    }
  }
  // Textures stay while there is room: a texture out of view (behind the
  // camera while it turns) is not decoded and uploaded again when it comes
  // back. Over the budget, the least recently used go first; any unused for
  // texture_keep_frames go anyway. None used by a frame still in flight.
  {
    // Revamped Graphics turned on or off: the replaced textures (off) or
    // those a replacement could be for (on) loaded again.
    const bool drop = drop_replaced_;
    drop_replaced_ = false;
    std::vector<std::pair<uint64_t, std::string>> idle;
    for (auto it = textures_.begin(); it != textures_.end();) {
      const uint64_t unused = frame_number_ - it->second.last_used;
      if (unused > options_.texture_keep_frames || (drop && (it->second.replaced || (replace_on_ && it->second.may_replace)))) {
        texture_bytes_ -= std::min<uint64_t>(texture_bytes_, it->second.bytes);
        known_textures_.erase(it->second.hash);
        Retire(it->second.image);
        it = textures_.erase(it);
        ++stats_.textures_evicted;
        continue;
      }
      if (texture_bytes_ > texture_budget_ && unused > 4) {
        idle.emplace_back(it->second.last_used, it->first);
      }
      ++it;
    }
    if (texture_bytes_ > texture_budget_ && !idle.empty()) {
      std::sort(idle.begin(), idle.end());
      for (const auto& [used, key] : idle) {
        if (texture_bytes_ <= texture_budget_ * 9 / 10) break;
        auto it = textures_.find(key);
        texture_bytes_ -= std::min<uint64_t>(texture_bytes_, it->second.bytes);
        known_textures_.erase(it->second.hash);
        Retire(it->second.image);
        textures_.erase(it);
        ++stats_.textures_evicted;
      }
    }
    stats_.textures_cached = uint32_t(textures_.size());
    stats_.texture_cache_mb = double(texture_bytes_) / (1 << 20);
  }
  ++frame_number_;
}

void Renderer::Impl::DoClear(const Clear& c) {
  if (!c.has_targets) {
    stats_.skipped["clear without target registers (old capture)"]++;
    return;
  }
  const uint32_t surface = c.targets[0];
  if (c.flags & 0xF) {
    const uint32_t infos[4] = {c.targets[1], c.targets[3], c.targets[4], c.targets[5]};
    for (int i = 0; i < 4; ++i) {
      if (!(c.flags & (1u << i))) continue;
      if (i > 0 && infos[i] == 0) continue;
      ClearImage(GetTarget(ColorKey(surface, infos[i])), c.color, 0, 0, true, false, false);
    }
  }
  if (c.flags & 0x30) {
    ClearImage(GetTarget(DepthKey(surface, c.targets[2])), c.color, c.z, c.stencil, false,
               (c.flags & 0x10) != 0, (c.flags & 0x20) != 0);
  }
}

Image* Renderer::Impl::ResolveImage(std::map<uint32_t, Image>& images, uint32_t base, uint32_t w,
                              uint32_t h, VkFormat format, const char* suffix, float scale) {
  auto it = images.find(base);
  if (it == images.end() || it->second.width != w || it->second.height != h ||
      it->second.format != format) {
    // Games resolve different sizes to the same address within a frame (a
    // blur chain): the image of the other size is kept aside and taken back
    // next time instead of being made again every frame.
    if (it != images.end()) {
      const Image old = it->second;
      SpareResolve& spare = spare_resolves_[{&images, base, old.width, old.height, old.format}];
      if (spare.image.image) Retire(spare.image);
      spare = {old, frame_number_};
    }
    auto sp = spare_resolves_.find({&images, base, w, h, format});
    Image im;
    if (sp != spare_resolves_.end()) {
      im = sp->second.image;
      spare_resolves_.erase(sp);
    } else {
      char name[64];
      std::snprintf(name, sizeof(name), "resolve_%08X_%ux%u%s", base, w, h, suffix);
      im = gpu_.CreateImage(w, h, format, false, name);
      im.scaled = true;
      ToGeneral(cb_, im);
    }
    if (it != images.end()) {
      it->second = im;  // same node: pointers to it stay valid
    } else {
      it = images.emplace(base, im).first;
    }
  }
  it->second.scale = scale;  // the source's
  return &it->second;
}

VkPipeline Renderer::Impl::CopyPipeline(CopyKind kind, VkFormat format) {
  VkPipeline& p = copy_pipelines_[{int(kind), format}];
  if (!p) {
    VkPipelineShaderStageCreateInfo stages[2] = {
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO},
        {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO}};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = kind == CopyKind::kClouds ? clouds_vs_ : copy_vs_;
    stages[0].pName = "main";
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = copy_fs_[int(kind == CopyKind::kBakeMin ? CopyKind::kBake : kind)];
    stages[1].pName = "main";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState att{};
    att.colorWriteMask = 15;
    if (kind == CopyKind::kBakeMin) {  // only ever darker
      att.blendEnable = VK_TRUE;
      att.srcColorBlendFactor = att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
      att.srcAlphaBlendFactor = att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      att.colorBlendOp = att.alphaBlendOp = VK_BLEND_OP_MIN;
    }
    if (kind == CopyKind::kClouds) {  // drawn over the output
      att.blendEnable = VK_TRUE;
      att.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
      att.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
      att.colorBlendOp = VK_BLEND_OP_ADD;
      att.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
      att.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
      att.alphaBlendOp = VK_BLEND_OP_ADD;
    }
    VkPipelineColorBlendStateCreateInfo cbs{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cbs.attachmentCount = 1;
    cbs.pAttachments = &att;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dy{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dy.dynamicStateCount = 2;
    dy.pDynamicStates = dyn;
    VkPipelineRenderingCreateInfo pr{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
    pr.colorAttachmentCount = 1;
    pr.pColorAttachmentFormats = &format;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkGraphicsPipelineCreateInfo pci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    pci.pNext = &pr;
    pci.stageCount = 2;
    pci.pStages = stages;
    pci.pVertexInputState = &vi;
    pci.pInputAssemblyState = &ia;
    pci.pViewportState = &vp;
    pci.pRasterizationState = &rs;
    pci.pMultisampleState = &ms;
    pci.pDepthStencilState = &ds;
    pci.pColorBlendState = &cbs;
    pci.pDynamicState = &dy;
    pci.layout = kind == CopyKind::kClouds ? clouds_layout_ : copy_layout_;
    char what[64];
    std::snprintf(what, sizeof(what), "built-in pass %d (format %d)", int(kind), int(format));
    const amped3::crash_note::Scope note(what);
    const auto lock = PipeLock();
    vk::Check(vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &pci, nullptr, &p),
              "copy pipeline");
  }
  return p;
}

namespace {

// 4 x 4 inverse (row-major), false if singular.
bool Invert4(const double m[16], double out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      a[r][c] = m[r * 4 + c];
      a[r][c + 4] = r == c ? 1.0 : 0.0;
    }
  }
  for (int c = 0; c < 4; ++c) {
    int piv = c;
    for (int r = c + 1; r < 4; ++r) {
      if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
    }
    if (std::fabs(a[piv][c]) < 1e-30) return false;
    if (piv != c) {
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[piv][k]);
    }
    const double inv = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= inv;
    for (int r = 0; r < 4; ++r) {
      if (r == c) continue;
      const double f = a[r][c];
      if (f == 0.0) continue;
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) out[r * 4 + c] = a[r][c + 4];
  }
  return true;
}

}  // namespace

namespace {

void Mul4(const double a[16], const double b[16], double out[16]) {
  double t[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += a[r * 4 + k] * b[k * 4 + c];
      t[r * 4 + c] = v;
    }
  }
  std::memcpy(out, t, sizeof(t));
}

// Rotation by `a` radians about the unit axis (x, y, z) (row-major 3 x 3).
void AxisRotation(const double ax[3], double a, double m[9]) {
  const double c = std::cos(a), s = std::sin(a), t = 1.0 - c;
  const double x = ax[0], y = ax[1], z = ax[2];
  const double r[9] = {t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                       t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                       t * x * z - s * y, t * y * z + s * x, t * z * z + c};
  std::memcpy(m, r, sizeof(r));
}

void Apply3(const double m[9], const double v[3], double out[3]) {
  double t[3];
  for (int r = 0; r < 3; ++r) t[r] = m[r * 3] * v[0] + m[r * 3 + 1] * v[1] + m[r * 3 + 2] * v[2];
  std::memcpy(out, t, sizeof(t));
}

bool Normalize3(double v[3]) {
  const double n = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  if (!(n > 1e-9)) return false;
  for (int i = 0; i < 3; ++i) v[i] /= n;
  return true;
}

bool SameTarget(const TargetKey& a, const TargetKey& b) {
  return a.depth == b.depth && a.base == b.base && a.format == b.format && a.pitch == b.pitch &&
         a.samples == b.samples;
}

}  // namespace

// The freecam for this frame (FreecamInput): the game's camera from the
// frame's lighting pass (found before anything is drawn), the freecam's pose
// moved by the input since the last frame, and its view-projection.
void Renderer::Impl::FreecamFrame() {
  fc_on_ = false;
  const auto now = std::chrono::steady_clock::now();
  double dt = fc_last_ == std::chrono::steady_clock::time_point{}
                  ? 0.0
                  : std::chrono::duration<double>(now - fc_last_).count();
  fc_last_ = now;
  dt = std::clamp(dt, 0.0, 0.1);
  if (!fc_in_.active) {
    fc_have_entry_ = false;
    return;
  }
  // The lighting pass: its pixel shader by name once known (looked up by
  // its microcode's hash until then).
  const Draw* light = nullptr;
  std::unordered_set<std::string> tried;
  for (const Draw& d : frame_->draws) {
    if (d.ps.empty()) continue;
    if (!lighting_ps_.empty()) {
      if (d.ps == lighting_ps_) {
        light = &d;
        break;
      }
      continue;
    }
    if (!tried.insert(d.ps).second) continue;
    Shader* ps = GetShader(d.ps, ShaderStage::kPixel);
    if (ps && ps->error.empty() && ps->info.ucode_hash == kLightingPixelShader) {
      lighting_ps_ = d.ps;
      light = &d;
      break;
    }
  }
  if (!light) return;  // no 3D scene this frame (menus, loading, videos)
  const GpuState& st = *light->state;
  double ivpg[16], vpg[16];
  for (uint32_t i = 0; i < 16; ++i) {
    float f;
    const uint32_t bits = st.ps_consts[i];
    std::memcpy(&f, &bits, 4);
    ivpg[i] = f;
  }
  if (!Invert4(ivpg, vpg)) return;
  double cam[3];
  for (uint32_t i = 0; i < 3; ++i) {
    float f;
    const uint32_t bits = st.ps_consts[16 + i];
    std::memcpy(&f, &bits, 4);
    cam[i] = f;
  }
  std::memcpy(fc_ivpg_, ivpg, sizeof(ivpg));
  fc_depth_ = DepthKey(st.reg(0x2000), st.reg(0x2002));
  auto unproject = [&](double x, double y, double out[3]) {
    const double n[4] = {x, y, 0.5, 1.0};
    double w[4] = {};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) w[r] += ivpg[r * 4 + k] * n[k];
    }
    for (int r = 0; r < 3; ++r) out[r] = w[r] / w[3];
  };
  if (!fc_have_entry_ || fc_in_.reset) {
    // From the game's camera: its view-projection, position and axes (the
    // screen's middle and a step right of it, unprojected).
    double mid[3], side[3];
    unproject(0.0, 0.0, mid);
    unproject(0.5, 0.0, side);
    double fwd[3] = {mid[0] - cam[0], mid[1] - cam[1], mid[2] - cam[2]};
    double right[3] = {side[0] - mid[0], 0.0, side[2] - mid[2]};
    if (!Normalize3(fwd) || !Normalize3(right)) return;
    std::memcpy(fc_vpe_, vpg, sizeof(vpg));
    std::memcpy(fc_c0_, cam, sizeof(cam));
    std::memcpy(fc_p_, cam, sizeof(cam));
    std::memcpy(fc_fwd_, fwd, sizeof(fwd));
    std::memcpy(fc_right_, right, sizeof(right));
    // Which way a positive angle turns (the world's handedness): right and up.
    const double up[3] = {0, 1, 0};
    double m[9], v[3];
    AxisRotation(up, 0.1, m);
    Apply3(m, fwd, v);
    fc_yaw_sign_ = v[0] * right[0] + v[1] * right[1] + v[2] * right[2] > 0 ? 1.0 : -1.0;
    AxisRotation(right, 0.1, m);
    Apply3(m, fwd, v);
    fc_pitch_sign_ = v[1] > fwd[1] ? 1.0 : -1.0;
    fc_yaw_ = fc_pitch_ = 0.0;
    fc_zoom_ = 1.0;
    fc_have_entry_ = true;
    fc_in_.reset = false;
  }
  if (fc_in_.set_pose) {
    // Yaw and pitch from the entry that give this pose (both without roll:
    // turned about the world's up, tilted about the right axis).
    fc_in_.set_pose = false;
    double f[3] = {fc_in_.pose[3], fc_in_.pose[4], fc_in_.pose[5]};
    if (Normalize3(f)) {
      double he[3] = {fc_fwd_[0], 0.0, fc_fwd_[2]}, ht[3] = {f[0], 0.0, f[2]};
      if (Normalize3(he) && Normalize3(ht)) {
        const double c = he[0] * ht[0] + he[2] * ht[2];
        const double sn = he[2] * ht[0] - he[0] * ht[2];  // (he x ht) . up
        fc_yaw_ = std::atan2(sn, c) * fc_yaw_sign_;
      }
      fc_pitch_ = std::asin(std::clamp(f[1], -1.0, 1.0)) -
                  std::asin(std::clamp(fc_fwd_[1], -1.0, 1.0));
      for (int i = 0; i < 3; ++i) fc_p_[i] = fc_in_.pose[i];
    }
  }
  // The pose, moved.
  const double kTurn = 1.8;  // radians per second at full deflection
  fc_yaw_ += fc_in_.look[0] * kTurn * dt / fc_zoom_ + fc_in_.look_delta[0];
  fc_pitch_ += fc_in_.look[1] * kTurn * dt / fc_zoom_ + fc_in_.look_delta[1];
  fc_in_.look_delta[0] = fc_in_.look_delta[1] = 0.0f;
  const double e0 = std::asin(std::clamp(fc_fwd_[1], -1.0, 1.0));
  fc_pitch_ = std::clamp(fc_pitch_, -1.45 - e0, 1.45 - e0);
  fc_zoom_ = std::clamp(fc_zoom_ * std::exp(double(fc_in_.zoom) * 1.5 * dt), 1.0, 8.0);
  const double up[3] = {0, 1, 0};
  double ry[9], rp[9], R[9];
  AxisRotation(up, fc_yaw_sign_ * fc_yaw_, ry);
  AxisRotation(fc_right_, fc_pitch_sign_ * fc_pitch_, rp);
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) {
      R[r * 3 + c] = ry[r * 3] * rp[c] + ry[r * 3 + 1] * rp[3 + c] + ry[r * 3 + 2] * rp[6 + c];
    }
  }
  double fwd[3], right[3];
  Apply3(R, fc_fwd_, fwd);
  Apply3(R, fc_right_, right);
  const double step = double(fc_in_.speed) * dt;
  const float* md = fc_in_.move_delta;
  for (int i = 0; i < 3; ++i) {
    fc_p_[i] += (right[i] * fc_in_.move[0] + up[i] * fc_in_.move[1] + fwd[i] * fc_in_.move[2]) * step +
                right[i] * md[0] + up[i] * md[1] + fwd[i] * md[2];
  }
  fc_in_.move_delta[0] = fc_in_.move_delta[1] = fc_in_.move_delta[2] = 0.0f;
  // View-projection: a world point x is seen where the game's camera, at
  // entry, saw c0 + R^T (x - p).
  double T1[16] = {1, 0, 0, fc_c0_[0], 0, 1, 0, fc_c0_[1], 0, 0, 1, fc_c0_[2], 0, 0, 0, 1};
  double RT[16] = {R[0], R[3], R[6], 0, R[1], R[4], R[7], 0, R[2], R[5], R[8], 0, 0, 0, 0, 1};
  double T2[16] = {1, 0, 0, -fc_p_[0], 0, 1, 0, -fc_p_[1], 0, 0, 1, -fc_p_[2], 0, 0, 0, 1};
  double vpf[16];
  Mul4(fc_vpe_, T1, vpf);
  Mul4(vpf, RT, vpf);
  Mul4(vpf, T2, vpf);
  for (int c = 0; c < 4; ++c) {
    vpf[c] *= fc_zoom_;
    vpf[4 + c] *= fc_zoom_;
  }
  if (!Invert4(vpf, fc_ivpf_)) return;
  std::memcpy(fc_vpf_, vpf, sizeof(vpf));
  for (int i = 0; i < 3; ++i) {
    fc_pose_[i] = float(fc_p_[i]);
    fc_pose_[3 + i] = float(fwd[i]);
  }
  fc_on_ = true;
}

void Renderer::Impl::NoteLightingPass(const Draw& d, const GpuState& st) {
  if (Diag("lightconsts")) {
    for (uint32_t c : {11u, 12u, 13u, 39u, 40u, 41u, 42u, 43u, 44u, 45u, 46u, 47u, 253u, 254u, 255u}) {
      float v[4];
      std::memcpy(v, &st.ps_consts[c * 4], 16);
      std::fprintf(stderr, "lighting c%u = %g %g %g %g\n", c, v[0], v[1], v[2], v[3]);
    }
  }
  // Rows c0..c3: world (homogeneous) = rows * (ndc x, ndc y, 1 - depth, 1).
  for (uint32_t i = 0; i < 16; ++i) {
    float f;
    const uint32_t bits = st.ps_consts[i];
    std::memcpy(&f, &bits, 4);
    inv_vp_[i] = f;
  }
  for (uint32_t i = 0; i < 3; ++i) {
    const uint32_t bits = st.ps_consts[16 + i];  // c4: camera position
    std::memcpy(&cam_pos_[i], &bits, 4);
  }
  {
    uint64_t h = 1469598103934665603ull;  // FNV-1a over c0..c4
    for (uint32_t i = 0; i < 20; ++i) {
      h ^= st.ps_consts[i];
      h *= 1099511628211ull;
    }
    stats_.camera_signature = h | 1;
    // Position and axes (the screen's middle and a step right, unprojected).
    double ivp[16];
    for (uint32_t i = 0; i < 16; ++i) {
      float f;
      const uint32_t bits = st.ps_consts[i];
      std::memcpy(&f, &bits, 4);
      ivp[i] = f;
    }
    auto unproject = [&](double x, double y, double out[3]) {
      const double n[4] = {x, y, 0.5, 1.0};
      double w[4] = {};
      for (int r = 0; r < 4; ++r) {
        for (int k = 0; k < 4; ++k) w[r] += ivp[r * 4 + k] * n[k];
      }
      for (int r = 0; r < 3; ++r) out[r] = w[r] / w[3];
    };
    double mid[3], side[3];
    unproject(0.0, 0.0, mid);
    unproject(0.5, 0.0, side);
    double fwd[3] = {mid[0] - cam_pos_[0], mid[1] - cam_pos_[1], mid[2] - cam_pos_[2]};
    double right[3] = {side[0] - mid[0], side[1] - mid[1], side[2] - mid[2]};
    if (Normalize3(fwd) && Normalize3(right)) {
      for (int i = 0; i < 3; ++i) {
        stats_.game_camera[i] = cam_pos_[i];
        stats_.game_camera[3 + i] = float(fwd[i]);
        stats_.game_camera[6 + i] = float(right[i]);
      }
    }
  }
  {
    // c14: the light's direction (a unit vector toward the sun).
    float l[3];
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t bits = st.ps_consts[14 * 4 + i];
      std::memcpy(&l[i], &bits, 4);
    }
    const float len = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    sun_valid_ = std::isfinite(len) && std::fabs(len - 1.0f) < 0.05f;
    // Time of day: the day laid out around the mountain's own sun (again
    // when it changes: another mountain).
    tod_state_ = tod::State{};
    lm_on_ = false;
    std::memcpy(tod_ref_, st.ps_consts.data() + 14 * 4, sizeof(tod_ref_));
    tod_ref_ok_ = sun_valid_;
    if (sun_valid_) {
      const float dot = (l[0] * tod_game_light_[0] + l[1] * tod_game_light_[1] +
                         l[2] * tod_game_light_[2]) / len;
      if (!tod_layout_.valid || dot < 0.99999f) {
        for (int i = 0; i < 3; ++i) tod_game_light_[i] = l[i] / len;
        tod_layout_ = tod::MakeLayout({tod_game_light_[0], tod_game_light_[1], tod_game_light_[2]});
        tod_mountain_hour_ = float(tod_layout_.mountain_hour);
      }
      if (tod_.on && tod_.passive) {
        // Effects only: the mountain's own hour, its light exactly the game's.
        tod_state_ = tod::At(tod_layout_, tod_layout_.mountain_hour, tod_.moon_phase, tod_.night_light);
        tod_state_.light = tod_layout_.mountain_sun;
        tod_state_.light_scale = tod_state_.ambient_scale = {1.0, 1.0, 1.0};
        tod_state_.away = 0.0;
      } else if (tod_.on) {
        tod_state_ = tod::At(tod_layout_, tod_.hour, tod_.moon_phase, tod_.night_light);
        tod::ApplyWeather(&tod_state_, tod_.overcast);
      }
      EyeUpdate();
      CenterRead();
      ResolveInstances();
      lm_on_ = TodOn() || eye_strength_ > 0.0f;
      lm_set_dir_ = TodOn() && !tod_.passive;
      const double e = eye_strength_ > 0.0f ? exposure_ : 1.0;
      if (TodOn()) {
        lm_dir_ = tod_state_.light;
        const tod::Vec3& l = tod_state_.light_scale;
        const tod::Vec3& a = tod_state_.ambient_scale;
        lm_light_scale_ = {l.x * e, l.y * e, l.z * e};
        lm_ambient_scale_ = {a.x * e, a.y * e, a.z * e};
      } else {
        lm_light_scale_ = lm_ambient_scale_ = {e, e, e};
      }
      if (lm_on_) {
        std::vector<uint32_t> c(1024, 0u);
        std::memcpy(&c[14 * 4], tod_ref_, sizeof(tod_ref_));
        TodLightingBlock(c.data(), 14, 15, 39);
        std::memcpy(tod_new_, &c[14 * 4], sizeof(tod_new_));
      }
    }
    if (sun_valid_) {
      float painted[3];
      for (int i = 0; i < 3; ++i) painted[i] = l[i] / len;
      // Mountains whose sky panorama paints the sun somewhere other than
      // where the light comes from: the god rays and the fog's sunward glow
      // come from the painted sun instead (the game's own lighting is left
      // alone). Keyed by the mountain's light direction (constant per
      // mountain); the painted sun's direction is where the sky dome's
      // texture has it (dome vertices projected, the panorama's sun texel).
      struct SkySun {
        float light[3];
        float sun[3];
      };
      static const SkySun kSkySuns[] = {
          // Snowbird: light 22 deg up, 55 deg around; sun painted 12 deg up,
          // 40 deg around (panorama texel 868, 82 of 2048 x 256).
          {{0.7626f, 0.3727f, 0.5287f}, {0.6279f, 0.2120f, 0.7488f}},
      };
      for (const SkySun& k : kSkySuns) {
        const float dot = painted[0] * k.light[0] + painted[1] * k.light[1] +
                          painted[2] * k.light[2];
        if (dot > 0.9995f) {
          const float n = std::sqrt(k.sun[0] * k.sun[0] + k.sun[1] * k.sun[1] + k.sun[2] * k.sun[2]);
          for (int i = 0; i < 3; ++i) painted[i] = k.sun[i] / n;
          break;
        }
      }
      std::memcpy(sun_dir_painted_, painted, sizeof(painted));
      std::memcpy(sun_dir_, painted, sizeof(painted));
      if (TodOn() && !tod_.passive) {
        // The fog's glow and the god rays: from the sun, or the moon at night.
        const tod::Vec3 v =
            tod_state_.sun_elevation > -1.0 * tod::kDeg ? tod_state_.sun : tod_state_.moon;
        sun_dir_[0] = float(v.x);
        sun_dir_[1] = float(v.y);
        sun_dir_[2] = float(v.z);
      }
    }
  }
  if (speed_frame_ != frame_number_ && max_draws_ == UINT32_MAX) {
    speed_frame_ = frame_number_;
    UpdateSpeed();
  }
  // The game's camera kept; with the freecam on, what's drawn (the depth
  // the effects read) is seen from the freecam.
  std::memcpy(game_inv_vp_, inv_vp_, sizeof(inv_vp_));
  std::memcpy(game_cam_pos_, cam_pos_, sizeof(cam_pos_));
  if (fc_on_) {
    std::memcpy(inv_vp_, fc_ivpf_, sizeof(inv_vp_));
    for (int i = 0; i < 3; ++i) cam_pos_[i] = float(fc_p_[i]);
  }
  amped3::native::TextureFetch scene, depth;
  std::memcpy(scene.words, FetchWords(d, 3, st), sizeof(scene.words));
  std::memcpy(depth.words, FetchWords(d, 5, st), sizeof(depth.words));
  scene_base_ = scene.base_address() & 0x1FFFFFFF;
  depth_base_ = depth.base_address() & 0x1FFFFFFF;
  light_seen_ = true;
  UpdateShadowReach(st);
  DrawSunShadows(d, st);
  SunHoldApply();
  TodLightingInputs(d, st);
  // Ambient occlusion: the world's depth, kept before it's resolved again.
  ao_depth_ok_ = false;
  if (ao_strength_ > 0.0f || world_detail_ > 0) {
    auto dit = resolved_.find(depth_base_);
    if (dit != resolved_.end() && dit->second.format == VK_FORMAT_R32_SFLOAT) {
      Image& src = dit->second;
      EndRendering();
      if (!ao_depth_.image || ao_depth_.width != src.width || ao_depth_.height != src.height) {
        if (ao_depth_.image) Retire(ao_depth_);
        ao_depth_ = gpu_.CreateImage(src.width, src.height, VK_FORMAT_R32_SFLOAT, false, "ao_depth");
        ToGeneral(cb_, ao_depth_);
      }
      Barrier();
      VkImageCopy region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.extent = {src.width, src.height, 1};
      vk::vkCmdCopyImage(cb_, src.image, VK_IMAGE_LAYOUT_GENERAL, ao_depth_.image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
      Barrier();
      ao_depth_ok_ = true;
    }
  }
}

Image* Renderer::Impl::BlurredScene(Image* scene) {
  if (blur_done_) return blur_img_.image ? &blur_img_ : nullptr;
  blur_done_ = true;
  if (!vel_ready_ && (!vel_draws_.empty() || !mask_draws_.empty())) DrawVelocities();
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height ||
      scene->samples > 1 || scene->depth) {
    return nullptr;
  }
  Image* depth = &dit->second;
  double prev_vp[16], m[16];
  if (!Invert4(prev_inv_vp_, prev_vp)) return nullptr;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double v = 0.0;
      for (int k = 0; k < 4; ++k) v += prev_vp[r * 4 + k] * inv_vp_[k * 4 + c];
      m[r * 4 + c] = v;
    }
  }
  const uint32_t w = scene->width, h = scene->height;
  // Tiles as wide as the longest half blur (max length 6% of the screen).
  const float max_len = 0.06f;
  const uint32_t tile = std::max(8u, uint32_t(std::ceil(max_len * 0.5f * float(std::max(w, h)))));
  const uint32_t tw = (w + tile - 1) / tile, th = (h + tile - 1) / tile;
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  EndRendering();
  ensure(blur_img_, w, h, scene->format, "motion_blur");
  ensure(motion_img_, w, h, VK_FORMAT_R16G16B16A16_SFLOAT, "motion");
  ensure(tile_rows_img_, tw, h, VK_FORMAT_R16G16B16A16_SFLOAT, "motion_tile_rows");
  ensure(tile_img_, tw, th, VK_FORMAT_R16G16B16A16_SFLOAT, "motion_tiles");
  ensure(nmax_img_, tw, th, VK_FORMAT_R16G16B16A16_SFLOAT, "motion_neighbours");
  blur_img_.scaled = scene->scaled;
  blur_img_.scale = scene->scale;  // sampled like the scene (tex_scale)
  Barrier();
  Image* vel = object_blur_ || vel_mask_ ? Velocities() : nullptr;
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(m[r * 4 + c]);  // column-major
  }
  for (int k = 0; k < 4; ++k) pc[16 + k] = float(inv_vp_[12 + k]);
  pc[20] = blur_scale_;
  pc[21] = max_len;
  pc[22] = (camera_blur_ ? 1.0f : 0.0f) + (vel && object_blur_ ? 2.0f : 0.0f) +
            (vel && vel_mask_ ? 4.0f : 0.0f);
  pc[23] = 2.5f;    // player radius: character pixels this near the player are its
  pc[24] = 1.0f / float(w);
  pc[25] = 1.0f / float(h);
  // The player's position for the motion pass: rows of (inverse
  // view-projection - P * its last row), so rows * v / dot(w_row, v) = a
  // pixel's world position - P.
  ensure(player_rows_img_, 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, "player_rows");
  float rows[16] = {};
  if (player_known_) {
    for (int r = 0; r < 3; ++r) {
      for (int k = 0; k < 4; ++k) {
        rows[r * 4 + k] = float(inv_vp_[r * 4 + k] - double(player_pos_[r]) * inv_vp_[12 + k]);
      }
    }
    pc[28] = 1.0f;
  }
  FullscreenPass(CopyKind::kRows, &player_rows_img_, nullptr, nullptr, nullptr, false, rows, 16);
  FullscreenPass(CopyKind::kMotion, &motion_img_, &player_rows_img_, depth, vel, false, pc, 32);
  const float rows_pc[4] = {float(tile), 0.0f, 0.0f, 0.0f}, cols_pc[4] = {float(tile), 0.0f, 0.0f, 1.0f};
  FullscreenPass(CopyKind::kTileMax, &tile_rows_img_, &motion_img_, nullptr, nullptr, false, rows_pc,
                 4);
  FullscreenPass(CopyKind::kTileMax, &tile_img_, &tile_rows_img_, nullptr, nullptr, false, cols_pc,
                 4);
  FullscreenPass(CopyKind::kNeighborMax, &nmax_img_, &tile_img_, nullptr, nullptr, false, nullptr, 0);
  const float bpc[8] = {1.0f / float(w), 1.0f / float(h), float(w), float(h),
                        float(tile), 15.0f, blur_debug_ ? 1.0f : 0.0f, 0.0f};
  FullscreenPass(CopyKind::kMotionBlur, &blur_img_, scene, &motion_img_, &nmax_img_, true, bpc, 8);
  dyn_ = DynState{};
  ++stats_.motion_blurred;
  return &blur_img_;
}

void Renderer::Impl::Transition(const Image& im, VkImageLayout from, VkImageLayout to) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = im.image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, im.levels, 0, 1};
  vk::vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                           0, 0, nullptr, 0, nullptr, 1, &b);
}

// The scene (render resolution) through the upscaler into an output-sized
// image; null when it can't (the composite then stretches the scene).
// A character's world position: the first matrix among its vertex shader's
// constants with a (0, 0, 0, 1) last row that isn't the identity (the rider
// shaders' bone palettes are world space, 4 constants a bone); for skinned
// shaders from the palette's start (constants below it may hold another
// draw's leftovers).
bool Renderer::Impl::CharacterAnchor(const Shader& vsh, const uint32_t* src, float out[3]) const {
  const Shader* vs = &vsh;
  auto cf = [&](uint32_t c, uint32_t k) {
    float f;
    std::memcpy(&f, &src[c * 4 + k], 4);
    return f;
  };
  auto reads = [&](uint32_t c) {
    return vs->info.dynamic_constants || ((vs->info.float_constants[c >> 5] >> (c & 31)) & 1);
  };
  const uint32_t first = vs->info.relative_constant_base < 256 ? vs->info.relative_constant_base
                                                               : 4u;
  for (uint32_t c = first; c + 3 < 256; ++c) {
    if (!reads(c) || !reads(c + 3)) continue;
    if (cf(c + 3, 0) != 0.0f || cf(c + 3, 1) != 0.0f || cf(c + 3, 2) != 0.0f ||
        cf(c + 3, 3) != 1.0f) {
      continue;
    }
    float t[3] = {cf(c, 3), cf(c + 1, 3), cf(c + 2, 3)};
    bool plausible = std::fabs(t[0]) + std::fabs(t[1]) + std::fabs(t[2]) > 1e-3f;
    for (uint32_t r = 0; r < 3 && plausible; ++r) {
      const float len = std::sqrt(cf(c + r, 0) * cf(c + r, 0) + cf(c + r, 1) * cf(c + r, 1) +
                                  cf(c + r, 2) * cf(c + r, 2));
      plausible = len > 0.2f && len < 5.0f && std::isfinite(t[r]);
    }
    if (plausible) {
      std::memcpy(out, t, sizeof(t));
      return true;
    }
  }
  return false;
}

// The camera's speed and direction since the last frame with the 3D scene,
// smoothed (quicker to build than to fade). A jump (a cut, a respawn) counts
// as standing still.
void Renderer::Impl::UpdateSpeed() {
  const float dt = std::clamp(speed_dt_, 1.0f / 1000.0f, 0.25f);
  float v = 0.0f;
  if (prev_cam_valid_) {
    const float d[3] = {cam_pos_[0] - prev_cam_[0], cam_pos_[1] - prev_cam_[1],
                        cam_pos_[2] - prev_cam_[2]};
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    v = dist / dt;
    // No character near the camera last frame (the trail map, overviews):
    // its camera moves aren't riding speed.
    if (!riding_) v = 0.0f;
    if (!std::isfinite(v) || v > 120.0f) {
      v = 0.0f;
    } else if (dist > 1e-4f) {
      const float k = 1.0f - std::exp(-dt / 0.15f);
      for (int i = 0; i < 3; ++i) speed_dir_[i] += (d[i] / dist - speed_dir_[i]) * k;
    }
  }
  std::memcpy(prev_cam_, cam_pos_, sizeof(prev_cam_));
  prev_cam_valid_ = true;
  const float tau = v > speed_ ? 0.35f : 0.6f;
  speed_ += (v - speed_) * (1.0f - std::exp(-dt / tau));
  // Nothing while slow (walking, standing); full at a fast run downhill.
  const float x = std::clamp((speed_ - 4.0f) / (24.0f - 4.0f), 0.0f, 1.0f);
  speed_amount_ = speed_force_ >= 0.0f ? speed_force_ : x * x * (3.0f - 2.0f * x);
  // Where the travel direction is on screen: the streaks point there (kept
  // near the middle; the center when it is behind the camera).
  float cx = 0.5f, cy = 0.5f;
  double vp[16];
  const float n = std::sqrt(speed_dir_[0] * speed_dir_[0] + speed_dir_[1] * speed_dir_[1] +
                            speed_dir_[2] * speed_dir_[2]);
  if (n > 0.2f && Invert4(inv_vp_, vp)) {
    const double p[4] = {cam_pos_[0] + speed_dir_[0] / n * 1000.0,
                         cam_pos_[1] + speed_dir_[1] / n * 1000.0,
                         cam_pos_[2] + speed_dir_[2] / n * 1000.0, 1.0};
    double c[4];
    for (int r = 0; r < 4; ++r) {
      c[r] = 0.0;
      for (int k = 0; k < 4; ++k) c[r] += vp[r * 4 + k] * p[k];
    }
    if (c[3] > 1e-6) {
      cx = std::clamp(float(c[0] / c[3] * 0.5 + 0.5), 0.35f, 0.65f);
      cy = std::clamp(float(0.5 - c[1] / c[3] * 0.5), 0.35f, 0.65f);
    }
  }
  // Fog: only while riding (not over the trail map), faded in and out:
  // from the cut, as the map clouds part and on after them (kFogSeconds:
  // thinning steadily as the clouds clear the middle, the last of it
  // slowly), else gradually.
  if (map_clouds_ && fog_anim_) {
    const float t = std::chrono::duration<float>(std::chrono::steady_clock::now() -
                                                 fog_anim_start_).count() / kFogSeconds;
    if (t >= 1.0f) fog_anim_ = false;
    const float f = std::clamp(t, 0.0f, 1.0f);
    // Ease in-out, weighted early: about two thirds through as the clouds
    // finish parting.
    const float g = 1.0f - std::pow(1.0f - f, 1.6f);
    const float e = g * g * (3.0f - 2.0f * g);
    fog_presence_ = cloud_riding_ ? e : 1.0f - e;
  } else {
    const float target = riding_ ? 1.0f : 0.0f;
    fog_presence_ += (target - fog_presence_) * (1.0f - std::exp(-dt / 0.8f));
  }
  const float k = 1.0f - std::exp(-dt / 0.2f);
  speed_center_[0] += (cx - speed_center_[0]) * k;
  speed_center_[1] += (cy - speed_center_[1]) * k;
}

bool Renderer::Impl::SunOnScreen(float uv[2], float* visible) const {
  *visible = 0.0f;
  uv[0] = uv[1] = 0.5f;
  double vp[16];
  if (!sun_valid_ || !Invert4(inv_vp_, vp)) return false;
  const double p[4] = {cam_pos_[0] + sun_dir_[0] * 1000.0, cam_pos_[1] + sun_dir_[1] * 1000.0,
                       cam_pos_[2] + sun_dir_[2] * 1000.0, 1.0};
  double c[4];
  for (int r = 0; r < 4; ++r) {
    c[r] = 0.0;
    for (int k = 0; k < 4; ++k) c[r] += vp[r * 4 + k] * p[k];
  }
  if (c[3] <= 1e-6) return false;  // behind the camera
  const double x = c[0] / c[3], y = c[1] / c[3];
  uv[0] = float(x * 0.5 + 0.5);
  uv[1] = float(0.5 - y * 0.5);
  // Full on screen, fading out to nothing 60% of a screen beyond the edge.
  const double edge = std::max(std::fabs(x), std::fabs(y));
  *visible = float(std::clamp(1.0 - (edge - 1.0) / 0.6, 0.0, 1.0));
  return *visible > 0.0f;
}

// The shadow map's inputs, kept at its first blur (which blurs it in place):
// the unblurred map (b: the stencil byte, 0 where a caster is nearest the
// sun) and the shadow pass's depth (the casters' height, when the light's
// view-projection is found at the lighting pass).
void Renderer::Impl::PrepareSunShadows(const Draw& d, const GpuState& st) {
  std::string note;
  Image* raw = TextureFor(d, 0, st, &note);
  Image* depth = sun_depth_target_;
  if (Diag("sun")) {
    std::fprintf(stderr, "sun: prepare raw %d depth %ux%u\n", int(raw != nullptr),
                 depth ? depth->width : 0, depth ? depth->height : 0);
  }
  if (!raw || raw->depth) return;
  const uint32_t w = raw->width, h = raw->height;
  EndRendering();
  if (!sun_raw_.image || sun_raw_.width != w || sun_raw_.height != h ||
      sun_raw_.format != raw->format) {
    if (sun_raw_.image) Retire(sun_raw_);
    sun_raw_ = gpu_.CreateImage(w, h, raw->format, false, "sun_raw");
    ToGeneral(cb_, sun_raw_);
  }
  FlushPendingClears();
  const VkRect2D all{{0, 0}, {w, h}};
  ResolveBarrier(raw, all);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {w, h, 1};
  vk::vkCmdCopyImage(cb_, raw->image, VK_IMAGE_LAYOUT_GENERAL, sun_raw_.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  Barrier();
  sun_depth_ok_ = depth && depth->samples == 1 && depth->width == w && depth->height == h &&
                  SnapshotDepth(depth, sun_float_depth_, sun_zo_, "sun_depth");
  dyn_ = DynState{};
  sun_inputs_ok_ = true;
}

// Draws recorded from the scene pass into one of the sun maps: each
// reprojected in its vertex shader, camera clip -> world (the lighting's
// inverse view-projection, which takes x, y, 1 - depth) -> the map (rows O).
void Renderer::Impl::DrawIntoSunMap(std::vector<CasterDraw>& draws, Image* map,
                                    const double O[16], const double* clip_to_world) {
  if (draws.empty()) return;
  double OI[16];
  const double* inv = clip_to_world ? clip_to_world : game_inv_vp_;
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      double v = 0;
      for (int k = 0; k < 4; ++k) v += O[r * 4 + k] * inv[k * 4 + c];  // as the game drew them
      OI[r * 4 + c] = v;
    }
  }
  static const std::vector<Image*> kNoColors;
  BeginRendering(kNoColors, map);
  const float one[4] = {1, 1, 1, 1};
  vk::vkCmdSetBlendConstants(cb_, one);
  vk::vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetDepthBias(cb_, 0.0f, 0.0f, 0.0f);
  VkViewport vp{0, 0, float(map->width), float(map->height), 0.0f, 1.0f};
  vk::vkCmdSetViewport(cb_, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {map->width, map->height}};
  vk::vkCmdSetScissor(cb_, 0, 1, &sc);
  VkPipeline bound = VK_NULL_HANDLE;
  for (CasterDraw& c : draws) {
    // The draw's clip position as the game gave it (the viewport's z scale
    // and offset make its depth: 1 - depth = (1 - zo) w - zs z).
    const double zs = c.sys.ndc_scale[2], zo = c.sys.ndc_offset[2];
    const double K[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -zs, 1.0 - zo, 0, 0, 0, 1};
    static const double kI[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
    const double* Kp = clip_to_world ? kI : K;
    SystemConstants sys = c.sys;
    for (int r = 0; r < 4; ++r) {
      for (int col = 0; col < 4; ++col) {
        double v = 0;
        for (int k = 0; k < 4; ++k) v += OI[r * 4 + k] * Kp[k * 4 + col];
        sys.reproject[col * 4 + r] = float(v);  // column-major
      }
    }
    sys.ndc_scale[0] = sys.ndc_scale[1] = sys.ndc_scale[2] = 1.0f;
    sys.ndc_offset[0] = sys.ndc_offset[1] = sys.ndc_offset[2] = 0.0f;
    // No MSAA coverage, camera fade or dither here; the alpha test stays.
    sys.flags &= 0x7Fu | 128u;
    sys.flags |= 16384u;  // XE_FLAG_REPROJECT
    sys.res_scale = 1.0f;
    sys.rt_inv_size[0] = 1.0f / float(map->width);
    sys.rt_inv_size[1] = 1.0f / float(map->height);
    uint32_t offsets[5];
    std::memcpy(offsets, c.offsets, sizeof(offsets));
    offsets[0] = WriteUniform(&sys, sizeof(sys));
    if (c.pipeline != bound) {
      vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, c.pipeline);
      bound = c.pipeline;
    }
    VkDescriptorSet sets[4] = {set0_, set1_, c.tex_sets[0], c.tex_sets[1]};
    vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 5,
                                offsets);
    if (c.indexed) {
      vk::vkCmdBindIndexBuffer(cb_, c.index_buffer, 0, c.index_type);
      vk::vkCmdDrawIndexed(cb_, c.count, 1, c.first, 0, 0);
    } else {
      vk::vkCmdDraw(cb_, c.count, 1, 0, 0);
    }
  }
  EndRendering();
  Barrier();
  dyn_ = DynState{};
}

// A sun map's contents moved by (dx, dy) texels (through a scratch of the
// same format), what comes in from outside cleared (far / not seen).
void Renderer::Impl::ShiftMap(Image* map, Image* scratch, int64_t dx, int64_t dy) {
  const float zero[4] = {0, 0, 0, 0};
  const int64_t n = int64_t(map->width);
  const VkImageAspectFlags aspect = map->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  ClearImage(scratch, zero, 1.0f, 0, !map->depth, map->depth, map->depth);
  FlushPendingClears();
  if (std::llabs(dx) < n && std::llabs(dy) < n) {
    VkImageCopy r{};
    r.srcSubresource = {aspect, 0, 0, 1};
    r.dstSubresource = {aspect, 0, 0, 1};
    r.srcOffset = {int32_t(std::max<int64_t>(dx, 0)), int32_t(std::max<int64_t>(dy, 0)), 0};
    r.dstOffset = {int32_t(std::max<int64_t>(-dx, 0)), int32_t(std::max<int64_t>(-dy, 0)), 0};
    r.extent = {uint32_t(n - std::llabs(dx)), uint32_t(n - std::llabs(dy)), 1};
    Barrier();
    vk::vkCmdCopyImage(cb_, map->image, VK_IMAGE_LAYOUT_GENERAL, scratch->image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &r);
  }
  Barrier();
  VkImageCopy all{};
  all.srcSubresource = {aspect, 0, 0, 1};
  all.dstSubresource = {aspect, 0, 0, 1};
  all.extent = {map->width, map->height, 1};
  vk::vkCmdCopyImage(cb_, scratch->image, VK_IMAGE_LAYOUT_GENERAL, map->image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &all);
  Barrier();
}

// At the lighting pass:
// - the sun maps brought up to date (the world, the ground along the sun,
//   the heights from above), around the camera;
// - the world's shadows baked from them into the slopes' shadow map (the
//   lighting's tf2) at a higher resolution (World shadows);
// - the game's (blurred) shadow map of riders and objects replaced by the
//   unblurred one filtered with penumbras that widen away from the caster
//   (Shadow softness).
// Updates of the sun maps: when the camera has moved 2.5 m or turned 6
// degrees since the last one (or the maps started over), the world's draws
// are drawn into them over the next three frames, a third each (no frame
// pays for all of them), and the shadows baked from them on the third.
void Renderer::Impl::SunCycle(const double cam[3], const double dir[3]) {
  if (sun_phase_ >= 0 && sun_phase_ < 2) {
    sun_next_phase_ = sun_phase_ + 1;
    return;
  }
  // What's saved (and while it's being read). (Not for the time of day.)
  if (bake_saved_only_ && !TodOn() &&
      (bake_loaded_ || (bake_io_.upload && bake_io_.state.load() != 0)) &&
      !Diag("sunevery")) {
    return;
  }
  const double dx = cam[0] - sun_cycle_cam_[0], dy = cam[1] - sun_cycle_cam_[1],
               dz = cam[2] - sun_cycle_cam_[2];
  const double turned = dir[0] * sun_cycle_dir_[0] + dir[1] * sun_cycle_dir_[1] +
                        dir[2] * sun_cycle_dir_[2];
  if (!sun_map_ok_ || dx * dx + dy * dy + dz * dz > 2.5 * 2.5 || turned < 0.9945) {
    std::memcpy(sun_cycle_cam_, cam, sizeof(sun_cycle_cam_));
    std::memcpy(sun_cycle_dir_, dir, sizeof(sun_cycle_dir_));
    sun_next_phase_ = 0;
  }
}

// At the lighting pass:
// - the sun maps brought up to date (the world from the sun, the heights
//   from above), around the camera, when an update goes (SunCycle);
// - the world's shadows baked from them into the slopes' shadow map (the
//   lighting's tf2) at a higher resolution (World shadows), kept on disk;
// - the game's (blurred) shadow map of riders and objects replaced by the
//   unblurred one filtered with penumbras that widen away from the caster
//   (Shadow softness).
void Renderer::Impl::DrawSunShadows(const Draw& d, const GpuState& st) {
  // (Last frame's held maps put back: the new ones drawn on.)
  if (sun_hold_swapped_) {
    std::swap(sun_env_, sun_env_hold_);
    sun_hold_swapped_ = false;
  }
  std::vector<CasterDraw> env, height;
  env.swap(env_draws_);
  height.swap(height_draws_);
  std::vector<std::array<double, 16>> candidates;
  candidates.swap(light_candidates_);
  baked_ok_ = false;
  sun_frame_ok_ = false;
  sun_depth_per_m_ = 0.0;
  if (!SunShadowsOn()) return;
  // The lighting's lookup into the shadow map: u = c6.p / c9.p,
  // v = c7.p / c9.p (v remapped with the shadow distance).
  double c6[4], c7[4], c7o[4], c9[4];
  for (int i = 0; i < 4; ++i) {
    float a, b, c;
    std::memcpy(&a, &st.ps_consts[6 * 4 + i], 4);
    std::memcpy(&b, &st.ps_consts[7 * 4 + i], 4);
    std::memcpy(&c, &st.ps_consts[9 * 4 + i], 4);
    c6[i] = a;
    c9[i] = c;
    c7o[i] = b;
    c7[i] = shadow_av_ * (b - c) + c;
  }
  auto dot3 = [](const double* p, const double* q) { return p[0] * q[0] + p[1] * q[1] + p[2] * q[2]; };
  auto cross = [](const double* p, const double* q, double* o) {
    o[0] = p[1] * q[2] - p[2] * q[1];
    o[1] = p[2] * q[0] - p[0] * q[2];
    o[2] = p[0] * q[1] - p[1] * q[0];
  };
  // The sun's direction: u, v stay the same all along a ray of sunlight, so
  // the rows' xyz parts all take it to zero.
  double f[3];
  {
    double k1[3], k2[3], k3[3];
    // (From the game's own rows: the shadow distance's remap of v (c7) moves
    // with the reach each frame, and with it, a little, the direction worked
    // out from it.)
    cross(c6, c7o, k1);
    cross(c6, c9, k2);
    cross(c7o, c9, k3);
    const double* best = k1;
    if (dot3(k2, k2) > dot3(best, best)) best = k2;
    if (dot3(k3, k3) > dot3(best, best)) best = k3;
    const double n = std::sqrt(dot3(best, best));
    if (!(n > 1e-12)) return;
    for (int i = 0; i < 3; ++i) f[i] = best[i] / n;
    if (f[1] > 0.0) {  // travelling down from the sun
      for (double& v : f) v = -v;
    }
  }
  // The time of day: the hour's sun (or moon), kept at least a few degrees
  // up (lower, its light is all but gone and the shadows endless).
  if (Diag("sun")) std::fprintf(stderr, "sun: game f %.4f %.4f %.4f\n", f[0], f[1], f[2]);
  // The time of day: shadows from the hour's sun (or moon). Each mountain's
  // shadows come from higher than its light does (an artist's choice: the
  // game's shadow map and painted shadows are for a sun some 20 degrees
  // higher, a little round): the same lift and turn, the full amount from the
  // mountain's own sun's height up, none at the horizon (long low shadows at
  // dawn and dusk).
  if (TodOn()) {
    const double* L0 = nullptr;
    double l0[3] = {tod_game_light_[0], tod_game_light_[1], tod_game_light_[2]};
    L0 = l0;
    const double s0[3] = {-f[0], -f[1], -f[2]};
    auto elev = [](const double* v) { return std::asin(std::clamp(v[1], -1.0, 1.0)); };
    auto azim = [](const double* v) { return std::atan2(v[2], v[0]); };
    const double e0 = elev(L0), es0 = elev(s0);
    const double daz = std::remainder(azim(s0) - azim(L0), 2.0 * tod::kPi);
    const double l[3] = {lm_dir_.x, lm_dir_.y, lm_dir_.z};
    const double e = elev(l);
    const double k = e0 > 0.02 ? std::clamp(e / e0, 0.0, 1.0) : 1.0;
    const double es = std::clamp(e + (es0 - e0) * k, 0.06, 1.45);
    const double az = azim(l) + daz * k;
    const double ch = std::cos(es);
    f[0] = -ch * std::cos(az);
    f[1] = -std::sin(es);
    f[2] = -ch * std::sin(az);
  }
  // The light's view-projection: a shadow pass draw whose c0-c3 rows x, y, w
  // are the lighting's (x = 2 c6 - c9, y = c9 - 2 c7, w = c9, give or take
  // half a texel): one with world-space vertices.
  sun_light_ok_ = false;
  {
    double best = 1e30;
    for (const auto& m : candidates) {
      double err = 0, scale = 0;
      for (int i = 0; i < 4; ++i) {
        err += std::fabs(m[0 * 4 + i] - (2.0 * c6[i] - c9[i])) +
               std::fabs(m[1 * 4 + i] - (c9[i] - 2.0 * c7o[i])) + std::fabs(m[3 * 4 + i] - c9[i]);
        scale += std::fabs(c9[i]);
      }
      err /= std::max(scale, 1e-9);
      if (err < best) {
        best = err;
        if (err < 0.01) {
          std::memcpy(sun_light_vp_, m.data(), sizeof(sun_light_vp_));
          sun_light_ok_ = true;
        }
      }
    }
  }
  if (Diag("sunrec")) {
    for (auto& [h, n] : sun_rec_counts_) std::fprintf(stderr, "sun: world %016llX %u\n", (unsigned long long)h, n);
    for (uint64_t h : shadow_caster_vs_) std::fprintf(stderr, "sun: game caster %016llX\n", (unsigned long long)h);
    sun_rec_counts_.clear();
  }
  if (Diag("sun")) {
    std::fprintf(stderr, "sun: phase %d inputs %d light %d (of %zu) depth %d world %zu heights %zu\n",
                 sun_phase_, int(sun_inputs_ok_), int(sun_light_ok_), candidates.size(),
                 int(sun_depth_ok_), env.size(), height.size());
  }
  EndRendering();
  ++stats_.tree_shadow_draws;
  // The sun maps: the Shadow resolution setting's texels (2048 at 2x),
  // wide (100 m and more), centred ahead of the camera: trees are drawn only
  // while in view, and their crowns leave the top of the view well before
  // the camera reaches them.
  const double reach = shadow_distance_ > 0.0f ? double(shadow_distance_) : 45.0;
  const double R = std::clamp(std::max(reach * 1.15, 100.0), 100.0, 180.0);
  const float res = shadow_res_ > 0.0f ? shadow_res_ : 2.0f;
  const uint32_t n_texels =
      uint32_t(std::clamp(std::lround(1024.0f * res / 256.0f) * 256, 1024L, 4096L));
  const double N = double(n_texels);
  const double texel = 2.0 * R / N;
  const double cam[3] = {cam_pos_[0], cam_pos_[1], cam_pos_[2]};
  // Where the camera looks: the screen's centre, far.
  double ahead[3] = {cam[0], cam[1], cam[2]};
  double look[3] = {0.0, 0.0, 1.0};
  {
    double wp[4];
    const double ndc[4] = {0.0, 0.0, 0.5, 1.0};
    for (int r = 0; r < 4; ++r) {
      wp[r] = 0;
      for (int c = 0; c < 4; ++c) wp[r] += inv_vp_[r * 4 + c] * ndc[c];
    }
    if (std::fabs(wp[3]) > 1e-12) {
      double v[3] = {wp[0] / wp[3] - cam[0], wp[1] / wp[3] - cam[1], wp[2] / wp[3] - cam[2]};
      const double nl = std::sqrt(dot3(v, v));
      if (nl > 1e-6) {
        for (int i = 0; i < 3; ++i) look[i] = v[i] / nl;
      }
      const double nh = std::sqrt(v[0] * v[0] + v[2] * v[2]);
      if (nh > 1e-6) {
        ahead[0] += 0.35 * R * v[0] / nh;
        ahead[2] += 0.35 * R * v[2] / nh;
      }
    }
  }
  const bool world = world_detail_ > 0;
  if (world) {
    for (Image* im : {&sun_env_, &sun_height_, &sun_scratch_, &sun_seen_, &sun_seen_scratch_}) {
      const bool color = im == &sun_seen_ || im == &sun_seen_scratch_;
      if (!im->image || im->width != n_texels) {
        if (im->image) Retire(*im);
        *im = gpu_.CreateImage(n_texels, n_texels,
                               color ? VK_FORMAT_R8_UNORM : VK_FORMAT_D32_SFLOAT_S8_UINT, !color,
                               im == &sun_env_      ? "sun_world"
                               : im == &sun_height_ ? "sun_heights"
                               : im == &sun_seen_   ? "sun_seen"
                                                    : "sun_scratch");
        ToGeneral(cb_, *im);
        sun_map_ok_ = false;
      }
    }
    const double moved = std::sqrt((cam[0] - sun_last_cam_[0]) * (cam[0] - sun_last_cam_[0]) +
                                   (cam[1] - sun_last_cam_[1]) * (cam[1] - sun_last_cam_[1]) +
                                   (cam[2] - sun_last_cam_[2]) * (cam[2] - sun_last_cam_[2]));
    // Started over when the sun moves (another mountain: its direction is
    // fixed per mountain), the camera jumps (a restart, another run) or the
    // maps' size changes.
    // (The time of day's sun followed in small steps, about half a degree:
    // each eased into the next, nearly a glide; the maps are drawn again
    // every couple of metres the camera moves anyway.)
    const double step_cos = TodOn() && !Diag("bigsunsteps") ? 0.99996 : 0.9997;
    // (The time of day: a step only when its sun has moved, not for the
    // game's own shadow frustum wavering under it.)
    bool sun_moved = dot3(f, sun_f_) < step_cos;
    const double ld[3] = {lm_dir_.x, lm_dir_.y, lm_dir_.z};
    if (sun_moved && sun_map_ok_ && TodOn() && dot3(f, sun_f_) > 0.9997 && dot3(ld, sun_step_light_) > 0.999995 &&
        !Diag("nosunhyst")) {
      sun_moved = false;
    }
    const bool reset = !sun_map_ok_ || sun_moved || R != sun_r_ ||
                       moved > 0.5 * R ||
                       std::fabs(dot3(cam, sun_f_) - sun_ref_) > 0.3 * kSunDepth ||
                       std::fabs(cam[1] - sun_href_) > 0.3 * kSunDepth;
    const float far_depth[4] = {0, 0, 0, 0};
    if (reset) {
      // (The time of day's sun moved on, the camera where it was: eased over.)
      if (sun_map_ok_ && TodOn() && sun_moved && R == sun_r_ && moved <= 0.5 * R) {
        sun_turn_frame_ = frame_number_;
        sun_turned_ = true;
        bake_ease_pending_ = true;
        ++stats_.sun_steps;
        // What's there kept, for what reads it while the new are drawn.
        if (!Diag("nohold")) {
          if (!sun_env_hold_.image || sun_env_hold_.width != sun_env_.width ||
              sun_env_hold_.height != sun_env_.height || sun_env_hold_.format != sun_env_.format) {
            if (sun_env_hold_.image) Retire(sun_env_hold_);
            sun_env_hold_ = gpu_.CreateImage(sun_env_.width, sun_env_.height, sun_env_.format, true, "sun_world_held");
            ToGeneral(cb_, sun_env_hold_);
          }
          VkImageCopy all{};
          all.srcSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          all.dstSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
          all.extent = {sun_env_.width, sun_env_.height, 1};
          Barrier();
          vk::vkCmdCopyImage(cb_, sun_env_.image, VK_IMAGE_LAYOUT_GENERAL, sun_env_hold_.image,
                             VK_IMAGE_LAYOUT_GENERAL, 1, &all);
          Barrier();
          std::memcpy(sun_O_hold_, sun_O_, sizeof(sun_O_hold_));
          sun_hold_on_ = true;
          sun_hold_phase0_ = false;
          sun_hold_frame_ = frame_number_;
        }
        if (Diag("sun")) std::fprintf(stderr, "sun: the time of day's sun moved on: eased over\n");
      }
      const double up[3] = {0.0, 1.0, 0.0}, side[3] = {1.0, 0.0, 0.0};
      double e1[3], e2[3];
      cross(std::fabs(f[1]) < 0.9 ? up : side, f, e1);
      const double n1 = std::sqrt(dot3(e1, e1));
      for (double& v : e1) v /= n1;
      cross(f, e1, e2);
      std::memcpy(sun_f_, f, sizeof(f));
      std::memcpy(sun_step_light_, ld, sizeof(ld));
      std::memcpy(sun_e1_, e1, sizeof(e1));
      std::memcpy(sun_e2_, e2, sizeof(e2));
      std::memcpy(sun_c0_, ahead, sizeof(ahead));
      sun_ci_ = sun_cj_ = sun_hi_ = sun_hj_ = 0;
      sun_r_ = R;
      sun_ref_ = dot3(cam, f);
      sun_href_ = cam[1];
      for (Image* im : {&sun_env_, &sun_height_}) ClearImage(im, far_depth, 1.0f, 0, false, true, true);
      sun_next_phase_ = 0;  // an update from now (the world drawn into them again)
      ClearImage(&sun_seen_, far_depth, 0.0f, 0, true, false, false);
      FlushPendingClears();
      sun_map_ok_ = true;
      ++stats_.sun_map_resets;
      // What's recorded this frame was meant for the old maps' frame: drawn
      // all the same (reprojected: it lands right), the update starts now.
      if (Diag("sun")) std::fprintf(stderr, "sun: maps started over (%u texels)\n", n_texels);
    }
    std::memcpy(sun_last_cam_, cam, sizeof(cam));
    // Moved along when the camera is an eighth of the maps from their centre
    // (the contents shifted a whole number of texels).
    const int64_t limit = int64_t(n_texels) / 8;
    const double rel[3] = {ahead[0] - sun_c0_[0], ahead[1] - sun_c0_[1], ahead[2] - sun_c0_[2]};
    const int64_t ai = std::llround(dot3(rel, sun_e1_) / texel);
    const int64_t aj = std::llround(dot3(rel, sun_e2_) / texel);
    if (std::llabs(ai - sun_ci_) > limit || std::llabs(aj - sun_cj_) > limit) {
      ShiftMap(&sun_env_, &sun_scratch_, ai - sun_ci_, aj - sun_cj_);
      ShiftMap(&sun_seen_, &sun_seen_scratch_, ai - sun_ci_, aj - sun_cj_);
      sun_ci_ = ai;
      sun_cj_ = aj;
    }
    const int64_t hi = std::llround(rel[0] / texel), hj = std::llround(rel[2] / texel);
    if (std::llabs(hi - sun_hi_) > limit || std::llabs(hj - sun_hj_) > limit) {
      ShiftMap(&sun_height_, &sun_scratch_, hi - sun_hi_, hj - sun_hj_);
      sun_hi_ = hi;
      sun_hj_ = hj;
    }
  }
  std::memcpy(f, world ? sun_f_ : f, sizeof(f));
  // The sun maps' rows (world -> their ndc x, y and depth along the sun),
  // and the height map's (world -> x, z from above; depth from the top).
  // (Without World shadows: the same frame around the camera, to measure.)
  if (!world) {
    const double up[3] = {0.0, 1.0, 0.0}, side[3] = {1.0, 0.0, 0.0};
    cross(std::fabs(f[1]) < 0.9 ? up : side, f, sun_e1_);
    const double n1 = std::sqrt(dot3(sun_e1_, sun_e1_));
    for (double& v : sun_e1_) v /= n1;
    cross(f, sun_e1_, sun_e2_);
    std::memcpy(sun_f_, f, sizeof(f));
    std::memcpy(sun_c0_, cam, sizeof(cam));
    sun_ci_ = sun_cj_ = sun_hi_ = sun_hj_ = 0;
    sun_r_ = R;
    sun_ref_ = dot3(cam, f);
    sun_href_ = cam[1];
    sun_map_ok_ = false;
  }
  double C[3];
  for (int i = 0; i < 3; ++i) {
    C[i] = sun_c0_[i] + double(sun_ci_) * texel * sun_e1_[i] + double(sun_cj_) * texel * sun_e2_[i];
  }
  double* O = sun_O_;
  std::fill(O, O + 16, 0.0);
  for (int i = 0; i < 3; ++i) {
    O[0 * 4 + i] = sun_e1_[i] / R;
    O[1 * 4 + i] = sun_e2_[i] / R;
    O[2 * 4 + i] = sun_f_[i] / kSunDepth;
  }
  O[0 * 4 + 3] = -dot3(C, sun_e1_) / R;
  O[1 * 4 + 3] = -dot3(C, sun_e2_) / R;
  O[2 * 4 + 3] = 0.5 - sun_ref_ / kSunDepth;
  O[15] = 1.0;
  const double hx = sun_c0_[0] + double(sun_hi_) * texel, hz = sun_c0_[2] + double(sun_hj_) * texel;
  const double H[16] = {1.0 / R, 0, 0, -hx / R,
                        0, 0, 1.0 / R, -hz / R,
                        0, -1.0 / kSunDepth, 0, 0.5 + sun_href_ / kSunDepth,
                        0, 0, 0, 1};
  if (world) {
    DrawIntoSunMap(env, &sun_env_, O);
    DrawIntoSunMap(height, &sun_height_, H);
    stats_.tree_casters += uint32_t(env.size());
    stats_.sun_receivers += uint32_t(height.size());
    if (!env.empty()) ++sun_world_drawn_;
    // The rays whose sunward side is in view at the end of an update (with
    // the world drawn into the maps): what casts on them has been drawn.
    const bool update_done = sun_phase_ == 2 || sun_phase_ == 3;
    if (gi_ > 0.0f && !Diag("nogi")) {
      MarkPart("lighting: sun maps");
      GiVoxelize(env, sun_phase_ == 0 || sun_phase_ == 3, update_done);
      MarkPart("world light: voxelise");
    }
    // (Held until a whole update's drawn since the step.)
    if (sun_hold_on_ && frame_number_ != sun_hold_frame_) {
      if (sun_phase_ == 0 || sun_phase_ == 3) sun_hold_phase0_ = true;
      if ((update_done && sun_hold_phase0_) || frame_number_ > sun_hold_frame_ + 8) sun_hold_on_ = false;
    }
    if (update_done) {
      double Oinv[16], vp[16];
      if (Invert4(O, Oinv) && Invert4(inv_vp_, vp)) {
        float spc[32];
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            spc[c * 4 + r] = float(Oinv[r * 4 + c]);  // column-major
            spc[16 + c * 4 + r] = float(vp[r * 4 + c]);
          }
        }
        const VkRect2D all{{0, 0}, {n_texels, n_texels}};
        FullscreenPass(CopyKind::kSunSeen, &sun_seen_, &sun_env_, nullptr, nullptr, false, spc, 32,
                       &all);
      }
    }
    sun_frame_ok_ = true;
    // The world's shadows, baked into the slopes' shadow map around the
    // camera at the end of an update.
    BakeWorldShadows(d, st, H, update_done);
    SunCycle(cam, look);
  }
  if (!sun_inputs_ok_) return;
  std::string note;
  Image* shadow = TextureFor(d, 0, st, &note);
  if (!shadow || shadow->depth) return;
  const uint32_t w = shadow->width, h = shadow->height;
  if (w != sun_raw_.width || h != sun_raw_.height || shadow->format != sun_raw_.format) return;
  // Pass 1: per texel of the shadow map, the ground its ray lands on and the
  // caster's height when the light's view-projection is known.
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat fmt, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == fmt) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, fmt, false, name);
    ToGeneral(cb_, im);
  };
  ensure(sun_prep_, w, h, VK_FORMAT_R32G32_SFLOAT, "sun_prep");
  ensure(sun_out_, w, h, shadow->format, "sun_shadows");
  ensure(sun_tiles_, (w + 7) / 8, (h + 7) / 8, VK_FORMAT_R8_UNORM, "sun_tiles");
  ensure(sun_tiles2_, (w + 7) / 8, (h + 7) / 8, VK_FORMAT_R8_UNORM, "sun_tiles_spread");
  float prep_pc[32] = {};
  float hrows[12] = {};
  {
    // Plane points P(a, b) = C + a R e1 + b R e2 (a, b: the maps' ndc); the
    // lighting's rows on it, inverted: map (u, v) -> (a, b).
    const double E1[4] = {R * sun_e1_[0], R * sun_e1_[1], R * sun_e1_[2], 0.0};
    const double E2[4] = {R * sun_e2_[0], R * sun_e2_[1], R * sun_e2_[2], 0.0};
    const double Ch[4] = {C[0], C[1], C[2], 1.0};
    const double* rows[3] = {c6, c7, c9};
    double M[16] = {};
    auto dot4 = [](const double* p, const double* q) {
      return p[0] * q[0] + p[1] * q[1] + p[2] * q[2] + p[3] * q[3];
    };
    for (int r = 0; r < 3; ++r) {
      M[r * 4 + 0] = dot4(rows[r], E1);
      M[r * 4 + 1] = dot4(rows[r], E2);
      M[r * 4 + 2] = dot4(rows[r], Ch);
    }
    M[15] = 1.0;
    double Hm[16];
    if (!Invert4(M, Hm)) return;
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 3; ++c) prep_pc[r * 4 + c] = hrows[r * 4 + c] = float(Hm[r * 4 + c]);
    }
    prep_pc[3] = float(sun_ref_);
    prep_pc[7] = float(kSunDepth);
    prep_pc[11] = world && sun_map_ok_ ? 1.0f : 0.0f;
  }
  // The casters' height: map texel (u, v, depth) -> the light's ndc (its
  // viewport undone) -> world through the light's inverse view-projection.
  bool heights = false;
  double Vinv[16];
  if (sun_light_ok_ && sun_depth_ok_ && Invert4(sun_light_vp_, Vinv)) {
    const SystemConstants& ls = sun_light_sys_;
    const double D[16] = {2.0 / ls.ndc_scale[0], 0, 0, (-1.0 - ls.ndc_offset[0]) / ls.ndc_scale[0],
                          0, 2.0 / ls.ndc_scale[1], 0, (-1.0 - ls.ndc_offset[1]) / ls.ndc_scale[1],
                          0, 0, 1.0 / ls.ndc_scale[2], -ls.ndc_offset[2] / ls.ndc_scale[2],
                          0, 0, 0, 1};
    double A[16];
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        double v = 0;
        for (int k = 0; k < 4; ++k) v += Vinv[r * 4 + k] * D[k * 4 + c];
        A[r * 4 + c] = v;
        prep_pc[12 + c * 4 + r] = float(v);  // column-major
      }
    }
    heights = true;
    // For the riders' own shadows: world -> the map's texel and depth, and
    // its depth per metre towards the sun (at the camera: near enough).
    if (Invert4(A, sun_tex_from_world_)) {
      auto depth_at = [&](const double* p) {
        double z = 0, ww = 0;
        for (int k = 0; k < 3; ++k) {
          z += sun_tex_from_world_[2 * 4 + k] * p[k];
          ww += sun_tex_from_world_[3 * 4 + k] * p[k];
        }
        z += sun_tex_from_world_[2 * 4 + 3];
        ww += sun_tex_from_world_[3 * 4 + 3];
        return z / ww;
      };
      const double p1[3] = {cam[0] - f[0], cam[1] - f[1], cam[2] - f[2]};
      sun_depth_per_m_ = depth_at(p1) - depth_at(cam);
      if (!(sun_depth_per_m_ > 0.0)) sun_depth_per_m_ = 0.0;
      // (The game's map is for its own sun: not once the hour's is away.)
      if (TodNoGameShadows()) sun_depth_per_m_ = 0.0;
    }
  }
  for (int i = 0; i < 3; ++i) prep_pc[28 + i] = float(f[i]);
  prep_pc[31] = heights ? 1.0f : 0.0f;
  FullscreenPass(CopyKind::kSunPrep, &sun_prep_, heights ? &sun_zo_ : nullptr,
                 world ? &sun_env_ : nullptr, &sun_raw_, false, prep_pc, 32);
  // Where the casters are (coarse), spread over the filter's reach.
  const float eight[4] = {8.0f, 0, 0, 0}, one[4] = {1.0f, 0, 0, 0};
  FullscreenPass(CopyKind::kSunTiles, &sun_tiles_, &sun_raw_, nullptr, nullptr, false, eight, 4);
  FullscreenPass(CopyKind::kSunTiles, &sun_tiles2_, &sun_tiles_, nullptr, nullptr, false, one, 4);
  // Pass 2: the filter. Penumbra radius = k * (caster to ground) + the
  // smallest (about the game's blur, a texel or two of its map).
  const float k = 0.04f * shadow_softness_;
  const float search = std::max(k * 40.0f, 0.25f);
  const float min_texels = std::clamp(1.5f * float(w) / 1280.0f, 1.0f, 3.0f) * 0.75f;
  float pc[16] = {k, search, min_texels, 32.0f};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 3; ++c) pc[4 + r * 4 + c] = hrows[r * 4 + c];
  }
  pc[7] = float(R);
  pc[11] = 0.15f;
  pc[15] = heights ? 0.0f : 1.0f;
  FullscreenPass(CopyKind::kSunPcss, &sun_out_, &sun_raw_, &sun_prep_, &sun_tiles2_, true, pc, 16);
  VkImageCopy region{};
  region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.extent = {w, h, 1};
  vk::vkCmdCopyImage(cb_, sun_out_.image, VK_IMAGE_LAYOUT_GENERAL, shadow->image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  Barrier();
  dyn_ = DynState{};
}

uint32_t Renderer::Impl::BakeSignature() const {
  const uint32_t parts[3] = {uint32_t(world_detail_), uint32_t(std::lround(shadow_softness_ * 100.0f)),
                             uint32_t(std::lround(shadow_res_ * 100.0f))};
  uint32_t h = 2166136261u;
  for (uint32_t v : parts) {
    for (int b = 0; b < 4; ++b) {
      h ^= (v >> (8 * b)) & 0xFF;
      h *= 16777619u;
    }
  }
  return h ? h : 1;
}

// When the settings a bake depends on have changed (here, or since the saved
// bakes were made: bake_settings.txt beside them): the saved bakes go (they'd
// show shadows of another size and softness), the one in memory starts over
// from the painted map, and what's baked from now on is saved as the current
// bake. Files that couldn't be deleted are still never loaded (their header
// carries the settings they were made with).
void Renderer::Impl::CheckBakeSettings() {
  const uint32_t sig = BakeSignature();
  if (sig == bake_sig_applied_) return;
  const bool first = bake_sig_applied_ == 0;
  bake_sig_applied_ = sig;
  char text[64];
  std::snprintf(text, sizeof(text), "%08X detail %d softness %d resolution %d\n", sig, world_detail_,
                int(std::lround(shadow_softness_ * 100.0f)), int(std::lround(shadow_res_ * 100.0f)));
  if (!bake_dir_.empty()) {
    const std::string marker = bake_dir_ + "/bake_settings.txt";
    std::string old;
    if (FILE* fp = std::fopen(marker.c_str(), "rb")) {
      char buf[64] = {};
      old.assign(buf, std::fread(buf, 1, sizeof(buf) - 1, fp));
      std::fclose(fp);
    }
    if (old.compare(0, 8, std::string(text, 8)) != 0) {
      std::error_code ec;
      int removed = 0;
      for (const auto& e : std::filesystem::directory_iterator(bake_dir_, ec)) {
        const std::string ext = e.path().extension().string();
        if (ext == ".a3ws" || ext == ".tmp") removed += std::filesystem::remove(e.path(), ec) ? 1 : 0;
      }
      std::filesystem::create_directories(bake_dir_, ec);
      if (FILE* fp = std::fopen(marker.c_str(), "wb")) {
        std::fputs(text, fp);
        std::fclose(fp);
      }
      if (removed) {
        pending_notices_.push_back("world shadows: settings changed, " + std::to_string(removed) +
                                   " saved bake(s) cleared");
      }
    }
  }
  if (!first && baked_.image) {
    // What's in memory was made with the old settings: started over.
    Retire(baked_);
    baked_src_ = nullptr;
    bake_dirty_ = false;
    bake_loaded_ = false;
    baked_ok_ = false;
  }
}

std::string Renderer::Impl::BakePath(uint32_t w, uint32_t h) const {
  if (bake_dir_.empty() || !bake_key_) return {};
  char name[64];
  std::snprintf(name, sizeof(name), "%016llX_%ux%u.a3ws", (unsigned long long)bake_key_, w, h);
  return bake_dir_ + "/" + name;
}

// World shadows on disk: a saved bake read in the background when the
// mountain's painted map first shows (copied in when ready), and the bake
// written back now and then (read back from the GPU a few frames after the
// copy is recorded, compressed and written in the background).
// File: "A3WS", version (2), width, height, the settings it was made with
// (BakeSignature), then the texels deflated.
void Renderer::Impl::BakeIoTick(Image* src) {
  for (size_t i = 0; i < bake_retired_.size();) {
    if (frame_number_ >= bake_retired_[i].first + 4) {
      gpu_.DestroyBuffer(bake_retired_[i].second);
      bake_retired_[i] = bake_retired_.back();
      bake_retired_.pop_back();
    } else {
      ++i;
    }
  }
  BakeIo& io = bake_io_;
  const int state = io.state.load();
  if (state == 1 && frame_number_ >= io.frame + 4) {
    // The GPU's copy is done (frames in flight have come round): write it.
    io.state = 2;
    io.thread = std::thread([&io] {
      const size_t n = size_t(io.w) * io.h;
      mz_ulong packed = mz_compressBound(mz_ulong(n));
      std::vector<uint8_t> out(20 + packed);
      std::memcpy(out.data(), "A3WS", 4);
      const uint32_t head[4] = {2u, io.w, io.h, io.sig};
      std::memcpy(out.data() + 4, head, 16);
      // (From an ordinary copy: the compressor reads its input over and over,
      // slow from GPU-visible memory where it isn't cached.)
      std::vector<uint8_t> plain(io.buffer.mapped, io.buffer.mapped + n);
      io.ok = mz_compress2(out.data() + 20, &packed, plain.data(), mz_ulong(n), 1) == MZ_OK;
      if (io.ok) {
        const std::string tmp = io.path + ".tmp";
        if (FILE* fp = std::fopen(tmp.c_str(), "wb")) {
          io.ok = std::fwrite(out.data(), 1, 20 + packed, fp) == 20 + packed;
          io.ok = (std::fclose(fp) == 0) && io.ok;
          std::error_code ec;
          if (io.ok) std::filesystem::rename(tmp, io.path, ec);
          io.ok = io.ok && !ec;
        } else {
          io.ok = false;
        }
      }
      io.state = 3;
    });
    return;
  }
  if (state != 3) return;
  if (io.thread.joinable()) io.thread.join();
  io.state = 0;
  if (io.upload) {
    // A saved bake read: copied in (if it's still this mountain's).
    if (io.ok && baked_.image && baked_src_ == src && baked_.width == io.w &&
        baked_.height == io.h && io.sig == bake_sig_applied_ && (!baked_tod_ || tod_.passive)) {
      EndRendering();
      VkBufferImageCopy r{};
      r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      r.imageExtent = {io.w, io.h, 1};
      Barrier();
      vk::vkCmdCopyBufferToImage(cb_, io.buffer.buffer, baked_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                                 &r);
      Barrier();
      bake_loaded_ = true;
      // (The time of day's effects alone: the mountain's own sun, so what's
      // saved is right; round the camera baked again at once, left to the
      // screen's mask there.)
      if (baked_tod_) sun_cycle_cam_[0] = 1e30;
      pending_notices_.push_back("world shadows: loaded what's baked (" + io.path + ")");
      // (The buffer is in use by this frame: destroyed a few frames on.)
      bake_retired_.push_back({frame_number_, io.buffer});
    } else {
      gpu_.DestroyBuffer(io.buffer);
    }
  } else {
    if (!io.ok) pending_notices_.push_back("world shadows: couldn't save " + io.path);
    gpu_.DestroyBuffer(io.buffer);
  }
  io.buffer = Buffer{};
}

void Renderer::Impl::BakeSave() {
  BakeIo& io = bake_io_;
  if (io.state.load() != 0 || !baked_.image || !bake_dirty_) return;
  const std::string path = BakePath(baked_.width, baked_.height);
  if (path.empty()) return;
  std::error_code ec;
  std::filesystem::create_directories(bake_dir_, ec);
  EndRendering();
  io.buffer = gpu_.CreateBuffer(VkDeviceSize(baked_.width) * baked_.height,
                                VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  VkBufferImageCopy r{};
  r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.imageExtent = {baked_.width, baked_.height, 1};
  Barrier();
  vk::vkCmdCopyImageToBuffer(cb_, baked_.image, VK_IMAGE_LAYOUT_GENERAL, io.buffer.buffer, 1, &r);
  Barrier();
  io.upload = false;
  io.ok = false;
  io.w = baked_.width;
  io.h = baked_.height;
  io.sig = bake_sig_applied_;
  io.path = path;
  io.frame = frame_number_;
  io.state = 1;
  bake_dirty_ = false;
  bake_last_save_ = frame_number_;
}

// World shadows: the slopes' painted shadow map (the lighting's tf2, over
// the whole mountain: u, v = world x, z times c5.xy plus c5.zw) replaced
// for the lighting by a copy World shadows times larger: what was saved for
// this mountain, else the painted one magnified; then baked again around
// the camera as the camera goes: each texel's surface (the height map, from
// above) looked at from the sun through the world map, with penumbras that
// widen away from the caster. What's baked stays, and is saved.
void Renderer::Impl::BakeWorldShadows(const Draw& d, const GpuState& st, const double H[16],
                                      bool bake) {
  std::string note;
  Image* src = TextureFor(d, 2, st, &note);
  if (!src || src->depth || src->samples > 1) return;
  float c5[4];
  std::memcpy(c5, &st.ps_consts[5 * 4], 16);
  if (!(std::fabs(c5[0]) > 1e-9f) || !(std::fabs(c5[1]) > 1e-9f)) return;
  std::memcpy(bake_c5_, c5, sizeof(c5));
  CheckBakeSettings();
  BakeIoTick(src);
  const uint32_t mult = uint32_t(world_detail_);
  const uint32_t w = std::min(src->width * mult, 16384u), h = std::min(src->height * mult, 16384u);
  // (Time of day: baked for the hour's sun, from nothing; the mountain's
  // own again after.)
  if (!baked_.image || baked_.width != w || baked_.height != h || baked_src_ != src ||
      baked_tod_ != TodOn()) {
    // Another mountain (or size): what's baked for the last one saved first.
    if (baked_.image && bake_dirty_ && !baked_tod_ && bake_io_.state.load() == 0) BakeSave();
    if (baked_.image) Retire(baked_);
    baked_ = gpu_.CreateImage(w, h, VK_FORMAT_R8_UNORM, false, "world_shadows");
    // Read as all four channels (whichever the lighting's fetch picks).
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = baked_.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                      VK_COMPONENT_SWIZZLE_R};
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::Check(vk::vkCreateImageView(gpu_.device, &vci, nullptr, &baked_.sample_view),
              "world shadows view");
    ToGeneral(cb_, baked_);
    baked_src_ = src;
    bake_dirty_ = false;
    bake_loaded_ = false;
    bake_key_ = 0;
    for (const auto& [name, t] : textures_) {
      if (&t.image == src) bake_key_ = t.hash;
    }
    baked_tod_ = TodOn();
    // The painted map, magnified (until a saved one is in); for the time of
    // day: lit everywhere until baked.
    const float inv[4] = {1.0f / float(w), 1.0f / float(h), 0.0f, 0.0f};
    if (baked_tod_ && !tod_.passive) {
      VkClearColorValue one{};
      for (float& v : one.float32) v = 1.0f;
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk::vkCmdClearColorImage(cb_, baked_.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
      Barrier();
    } else {
      FullscreenPass(CopyKind::kBakeInit, &baked_, src, nullptr, nullptr, true, inv, 4);
    }
    if (Diag("sun")) {
      std::fprintf(stderr, "sun: world shadows %ux%u, texel %.3f x %.3f m; sun maps' reach %.1f m\n", w, h,
                   1.0 / (std::fabs(c5[0]) * w), 1.0 / (std::fabs(c5[1]) * h), sun_r_);
    }
    // A saved one: read in the background.
    const std::string path = BakePath(w, h);
    std::error_code ec;
    if ((!baked_tod_ || tod_.passive) && !path.empty() && bake_io_.state.load() == 0 &&
        std::filesystem::exists(path, ec)) {
      BakeIo& io = bake_io_;
      io.buffer = gpu_.CreateBuffer(VkDeviceSize(w) * h, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
      io.upload = true;
      io.ok = false;
      io.w = w;
      io.h = h;
      io.sig = bake_sig_applied_;
      io.path = path;
      io.state = 2;
      io.thread = std::thread([&io] {
        std::vector<uint8_t> data;
        if (FILE* fp = std::fopen(io.path.c_str(), "rb")) {
          std::fseek(fp, 0, SEEK_END);
          const long size = std::ftell(fp);
          std::fseek(fp, 0, SEEK_SET);
          if (size > 16) {
            data.resize(size_t(size));
            if (std::fread(data.data(), 1, data.size(), fp) != data.size()) data.clear();
          }
          std::fclose(fp);
        }
        // Version 2: made with these settings (version 1 files never are).
        uint32_t head[4] = {};
        if (data.size() > 20) std::memcpy(head, data.data() + 4, 16);
        if (data.size() > 20 && !std::memcmp(data.data(), "A3WS", 4) && head[0] == 2 &&
            head[1] == io.w && head[2] == io.h && head[3] == io.sig) {
          // Unpacked in ordinary memory, then copied over in one go: unpacking
          // reads back what it has written, and from the upload buffer
          // (GPU-visible, uncached) that took a minute and a half for a
          // 16384 x 16384 bake - and closing the renderer (an upscaler
          // switched) waited for it, the picture frozen meanwhile.
          const size_t total = size_t(io.w) * io.h;
          std::vector<uint8_t> plain(total);
          mz_ulong n = mz_ulong(total);
          io.ok = mz_uncompress(plain.data(), &n, data.data() + 20, mz_ulong(data.size() - 20)) ==
                      MZ_OK &&
                  n == mz_ulong(total);
          if (io.ok) std::memcpy(io.buffer.mapped, plain.data(), total);
        }
        io.state = 3;
      });
    }
  }
  // Baked again (at the end of an update): the texels within 80% of the sun
  // maps' reach of the camera.
  const double reach = 0.8 * sun_r_;
  const double cu = cam_pos_[0] * c5[0] + c5[2], cv = cam_pos_[2] * c5[1] + c5[3];
  const double du = reach * std::fabs(c5[0]) * w + 2.0, dv = reach * std::fabs(c5[1]) * h + 2.0;
  auto clampi = [](double v, uint32_t hi) { return int32_t(std::clamp(v, 0.0, double(hi))); };
  const int32_t x0 = clampi(cu * w - du, w), x1 = clampi(cu * w + du + 1.0, w);
  const int32_t y0 = clampi(cv * h - dv, h), y1 = clampi(cv * h + dv + 1.0, h);
  const VkRect2D rect{{x0, y0}, {uint32_t(std::max(x1 - x0, 0)), uint32_t(std::max(y1 - y0, 0))}};
  // (Not while a saved one is being read: it would be copied over this.)
  const bool reading = bake_io_.upload && bake_io_.state.load() != 0;
  if (bake && !reading && rect.extent.width && rect.extent.height && !Diag("nobake")) {
    // c5; the height map's rows x, z and its depth -> height; the world
    // map's rows; penumbra radius per metre, smallest radius (m), reach (m),
    // the bake's texel (m).
    float pc[32] = {};
    for (int i = 0; i < 4; ++i) pc[i] = c5[i];
    pc[4] = float(H[0 * 4 + 0]);  // 1 / R
    pc[5] = float(H[0 * 4 + 3]);  // -hx / R
    pc[6] = float(H[1 * 4 + 3]);  // -hz / R
    pc[7] = float(sun_href_);
    for (int r = 0; r < 3; ++r) {
      for (int c = 0; c < 4; ++c) pc[8 + r * 4 + c] = float(sun_O_[r * 4 + c]);
    }
    const float k = 0.04f * std::max(shadow_softness_, 0.25f);
    const float bake_texel = float(1.0 / (std::fabs(c5[0]) * double(w)));
    pc[20] = k;
    pc[21] = std::max(0.5f * bake_texel, 0.05f);
    pc[22] = float(kSunDepth);
    pc[23] = float(sun_r_);
    pc[24] = float(reach);
    pc[25] = float(cam_pos_[0]);
    pc[26] = float(cam_pos_[2]);
    pc[28] = float(w);
    pc[29] = float(h);
    // (The time of day with the screen's mask doing the slopes near by:
    // within 0.6 of the sun maps' reach left lit here, fading in to 0.7.)
    if (baked_tod_ && world_detail_ > 0 && sun_r_ > 0.0 && !Diag("nonearworld")) {
      pc[30] = float(0.6 * sun_r_);
      pc[31] = float(0.1 * sun_r_);
    }
    if (baked_tod_) {
      // The time of day: every ray baked outright (the sun moves on: what
      // was dark before mustn't stay dark), nothing saved. After a step of
      // the sun: what was there kept first, to ease from.
      const bool start_ease = bake_ease_pending_ && !Diag("noease");
      if (start_ease) {
        bake_ease_pending_ = false;
        bake_ease_rect_ = rect;
        bake_ease_frame_ = frame_number_;
        bake_ease_on_ = true;
        if (Diag("sun")) std::fprintf(stderr, "sun: world shadows eased over %ux%u texels\n", rect.extent.width, rect.extent.height);
        for (Image* im : {&bake_old_, &bake_new_}) {
          if (!im->image || im->width < rect.extent.width || im->height < rect.extent.height) {
            if (im->image) Retire(*im);
            const uint32_t ew = std::max(rect.extent.width, im->width), eh = std::max(rect.extent.height, im->height);
            *im = gpu_.CreateImage(ew, eh, VK_FORMAT_R8_UNORM, false, im == &bake_old_ ? "world_shadows_old" : "world_shadows_new");
            ToGeneral(cb_, *im);
          }
        }
        VkImageCopy part{};
        part.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        part.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        part.srcOffset = {rect.offset.x, rect.offset.y, 0};
        part.extent = {rect.extent.width, rect.extent.height, 1};
        Barrier();
        vk::vkCmdCopyImage(cb_, baked_.image, VK_IMAGE_LAYOUT_GENERAL, bake_old_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &part);
        Barrier();
      }
      pc[27] = 2.0f;
      FullscreenPass(CopyKind::kBake, &baked_, &sun_height_, &sun_env_, &sun_seen_, false, pc, 32,
                     &rect);
      // (While easing: what's newly baked over the eased part, kept as the new.)
      if (bake_ease_on_) {
        const VkRect2D& er = bake_ease_rect_;
        const int32_t ix0 = std::max(er.offset.x, rect.offset.x), iy0 = std::max(er.offset.y, rect.offset.y);
        const int32_t ix1 = std::min(er.offset.x + int32_t(er.extent.width), rect.offset.x + int32_t(rect.extent.width));
        const int32_t iy1 = std::min(er.offset.y + int32_t(er.extent.height), rect.offset.y + int32_t(rect.extent.height));
        if (ix1 > ix0 && iy1 > iy0) {
          VkImageCopy part{};
          part.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          part.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
          part.srcOffset = {ix0, iy0, 0};
          part.dstOffset = {ix0 - er.offset.x, iy0 - er.offset.y, 0};
          part.extent = {uint32_t(ix1 - ix0), uint32_t(iy1 - iy0), 1};
          Barrier();
          vk::vkCmdCopyImage(cb_, baked_.image, VK_IMAGE_LAYOUT_GENERAL, bake_new_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &part);
          Barrier();
        }
      }
    } else {
      pc[27] = 0.0f;
      FullscreenPass(CopyKind::kBake, &baked_, &sun_height_, &sun_env_, &sun_seen_, false, pc, 32,
                     &rect);
      pc[27] = 1.0f;
      FullscreenPass(CopyKind::kBakeMin, &baked_, &sun_height_, &sun_env_, &sun_seen_, false, pc,
                     32, &rect);
      bake_dirty_ = true;
    }
  }
  // Eased over a step of the sun (about a second): the part around the
  // camera, from what it was to what's baked for the new sun.
  if (bake_ease_on_) {
    const uint64_t since = frame_number_ - bake_ease_frame_;
    if (!baked_tod_ || since > 60) {
      bake_ease_on_ = false;
      // (The new, whole.)
      if (baked_tod_) {
        const VkRect2D& er = bake_ease_rect_;
        VkImageCopy part{};
        part.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        part.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        part.dstOffset = {er.offset.x, er.offset.y, 0};
        part.extent = {er.extent.width, er.extent.height, 1};
        Barrier();
        vk::vkCmdCopyImage(cb_, bake_new_.image, VK_IMAGE_LAYOUT_GENERAL, baked_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &part);
        Barrier();
      }
    } else {
      const double t = double(since) / 60.0;
      float ep[20] = {};
      ep[16] = float(t * t * (3.0 - 2.0 * t));
      ep[17] = 1.0f;
      ep[18] = float(bake_ease_rect_.offset.x);
      ep[19] = float(bake_ease_rect_.offset.y);
      Barrier();
      FullscreenPass(CopyKind::kMaskEase, &baked_, &bake_old_, nullptr, &bake_new_, false, ep, 20, &bake_ease_rect_);
      Barrier();
    }
  }
  // Saved every half a minute or so while there's something new.
  if (bake_dirty_ && (frame_number_ >= bake_last_save_ + 1800 || Diag("bakesave"))) BakeSave();
  dyn_ = DynState{};
  baked_ok_ = true;
}

// Riders and whatever else is drawn after the lighting pass (it doesn't
// light them: they're shaded by themselves) darkened where the world shades
// them, and where they shade themselves (the game's own shadow map: an arm
// on the body), on the surfaces facing the sun. Worked out at half size.
// While the time of day's sun maps are drawn again after a step: what reads
// them this frame gets the ones from before (and their rows).
void Renderer::Impl::SunHoldApply() {
  if (!sun_hold_on_ || sun_hold_swapped_ || !sun_env_hold_.image || !sun_env_.image ||
      sun_env_hold_.width != sun_env_.width || !TodOn()) {
    return;
  }
  std::swap(sun_env_, sun_env_hold_);
  std::memcpy(sun_O_, sun_O_hold_, sizeof(sun_O_));
  sun_hold_swapped_ = true;
  ++stats_.sun_held;
  if (Diag("sun")) std::fprintf(stderr, "sun: maps from before the step held\n");
}

Image* Renderer::Impl::ShadedMovers(Image* scene) {
  if (movers_done_) return movers_result_;
  movers_done_ = true;
  movers_result_ = nullptr;
  if (world_detail_ <= 0 || !sun_frame_ok_ || !sun_env_.image) return nullptr;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height ||
      scene->samples > 1 || !ao_depth_ok_ || ao_depth_.width != scene->width ||
      ao_depth_.height != scene->height) {
    return nullptr;
  }
  Image* depth = &dit->second;
  auto fits = [&](bool ok, const Image& im) {
    return ok && im.width == depth->width && im.height == depth->height;
  };
  // As the ambient occlusion: the riders' depth (late), told from what the
  // depth-only passes wrote where nothing shows.
  Image* late = fits(ao_late_ok_, ao_late_depth_) ? &ao_late_depth_ : depth;
  const bool hidden = fits(ao_hidden_ok_, ao_after_hidden_);
  Image* after_hidden = hidden ? &ao_after_hidden_ : &ao_depth_;
  const uint32_t w = scene->width, h = scene->height;
  const uint32_t hw = (w + 1) / 2, hh = (h + 1) / 2;
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  ensure(movers_pos_, hw, hh, VK_FORMAT_R32G32B32A32_SFLOAT, "movers_position");
  ensure(movers_shade_, hw, hh, VK_FORMAT_R8_UNORM, "movers_shade");
  ensure(movers_blur_, hw, hh, VK_FORMAT_R8_UNORM, "movers_shade_blur");
  ensure(movers_out_, w, h, scene->format, "movers_shaded");
  movers_out_.scaled = scene->scaled;
  movers_out_.scale = scene->scale;
  Barrier();
  float pc[16];
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // column-major
  }
  FullscreenPass(CopyKind::kMoverPos, &movers_pos_, late, &ao_depth_, after_hidden, false, pc, 16);
  float sp[32] = {};
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) sp[r * 4 + c] = float(sun_O_[r * 4 + c]);
  }
  // Only what's saved (no world map kept up): the baked shadows instead.
  const bool from_baked = bake_saved_only_ && bake_loaded_ && baked_ok_;
  if (from_baked) {
    for (int i = 0; i < 4; ++i) sp[i] = bake_c5_[i];
    for (int i = 0; i < 3; ++i) sp[4 + i] = float(sun_f_[i]);
  }
  // Themselves, and in the shade of other riders and objects: the game's
  // shadow map; with the time of day, the casters' map from the hour's sun
  // (its depth the other way: nearer the sun, less; told by a negative
  // depth per metre).
  const bool tod_self = TodOn() && tod_mask_ok_ && !Diag("noselfshadow");
  const bool self = tod_self || (sun_depth_per_m_ > 0.0 && sun_depth_ok_ && !Diag("noselfshadow"));
  double L[16] = {};
  if (tod_self) {
    for (int c = 0; c < 4; ++c) {
      L[0 * 4 + c] = 0.5 * tod_Om_[0 * 4 + c];
      L[1 * 4 + c] = 0.5 * tod_Om_[1 * 4 + c];
      L[2 * 4 + c] = tod_Om_[2 * 4 + c];
    }
    L[0 * 4 + 3] += 0.5;
    L[1 * 4 + 3] += 0.5;
    L[15] = 1.0;
  } else if (self) {
    std::memcpy(L, sun_tex_from_world_, sizeof(L));
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) sp[12 + c * 4 + r] = float(L[r * 4 + c]);
  }
  sp[28] = 0.04f * std::max(shadow_softness_, 0.25f);
  sp[29] = tod_self ? float(-1.0 / kSunDepth) : self ? float(sun_depth_per_m_) : 0.0f;
  sp[30] = 0.5f;  // how much of their light the shade takes
  sp[31] = from_baked ? 2.0f : Diag("moverdbg") ? 1.0f : 0.0f;
  // (The clouds' shade where they are, in its fraction: under 0.4.)
  sp[31] += 0.4f * std::clamp(cloud_here_, 0.0f, 0.99f);
  FullscreenPass(CopyKind::kMoverShade, &movers_shade_, &movers_pos_, &sun_env_,
                 tod_self ? &tod_movers_map_ : self ? &sun_zo_ : nullptr, false, sp, 32, nullptr,
                 from_baked ? &baked_ : nullptr);
  // Evened out across each surface (both ways), then applied.
  const float bx[4] = {1, 0, 0, 0}, by[4] = {0, 1, 0, 0};
  FullscreenPass(CopyKind::kMoverBlur, &movers_blur_, &movers_shade_, &movers_pos_, nullptr, false, bx, 4);
  FullscreenPass(CopyKind::kMoverBlur, &movers_shade_, &movers_blur_, &movers_pos_, nullptr, false, by, 4);
  // The world light on them too (from the probes where they are: the snow's
  // light from below, the sky's taken off under a roof or the trees), as a
  // factor on their light.
  const bool gi = gi_probes_now_ && GiReady() && gi_probe_[gi_probe_cur_].image && !Diag("nogimovers");
  if (gi) {
    ensure(gi_movers_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, "world_light_riders");
    float gp[4] = {gi_, 0, 0, 0};
    FullscreenPass(CopyKind::kGiMovers, &gi_movers_, &movers_pos_, nullptr, &gi_probe_[gi_probe_cur_], false,
                   gp, 4, nullptr, nullptr, nullptr, &gi_params_);
    Barrier();
  }
  float ap[4] = {gi ? 1.0f : 0.0f, 0, 0, 0};
  FullscreenPass(CopyKind::kMoverApply, &movers_out_, &movers_shade_, scene, late, true, ap, 4,
                 nullptr, &ao_depth_, gi ? &gi_movers_ : nullptr);
  dyn_ = DynState{};
  movers_result_ = &movers_out_;
  return movers_result_;
}

void Renderer::Impl::UpdateShadowReach(const GpuState& st) {
  next_shadow_av_ = 1.0f;
  next_shadow_fade_ = 1.0f;
  if (shadow_distance_ <= 0.0f) return;
  auto c = [&](int i, float* v) { std::memcpy(v, st.ps_consts.data() + i * 4, 16); };
  float c6[4], c7[4], c9[4], c10[4];
  c(6, c6);
  c(7, c7);
  c(9, c9);
  c(10, c10);
  double vp[16];
  if (!Invert4(inv_vp_, vp)) return;
  // The camera's view direction, level.
  double mid[4] = {0, 0, 0, 0};
  for (int r = 0; r < 4; ++r) {
    const double ndc[4] = {0.0, 0.0, 0.5, 1.0};
    for (int k = 0; k < 4; ++k) mid[r] += inv_vp_[r * 4 + k] * ndc[k];
  }
  if (std::fabs(mid[3]) < 1e-12) return;
  double f[3];
  for (int i = 0; i < 3; ++i) f[i] = mid[i] / mid[3] - cam_pos_[i];
  const double fh = std::sqrt(f[0] * f[0] + f[2] * f[2]);
  if (fh < 1e-6) return;
  // Points out to the distance, ahead and to the sides along the view (its
  // slope as the camera looks down the hill): the map must reach them all.
  const double d = shadow_distance_;
  double v_min = 1.0, depth_far = 1.0;
  bool any = false;
  for (int side = -1; side <= 1; ++side) {
    const double p[4] = {cam_pos_[0] + (f[0] - side * 0.6 * f[2]) / fh * d,
                         cam_pos_[1] + f[1] / fh * d,
                         cam_pos_[2] + (f[2] + side * 0.6 * f[0]) / fh * d, 1.0};
    auto dot = [&](const float* r) { return r[0] * p[0] + r[1] * p[1] + r[2] * p[2] + r[3]; };
    const double w = dot(c9);
    if (w <= 1e-6) continue;
    v_min = std::min(v_min, dot(c7) / w);
    double clip[4] = {0, 0, 0, 0};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) clip[r] += vp[r * 4 + k] * p[k];
    }
    if (clip[3] > 1e-6) depth_far = std::min(depth_far, 1.0 - clip[2] / clip[3]);
    any = true;
  }
  if (!any) return;
  // Only ever farther than the game's own (av <= 1), and not past four times
  // its extent.
  const double v0 = std::min(v_min - 0.02, 0.0);
  next_shadow_av_ = float(std::clamp(1.0 / (1.0 - v0), 0.25, 1.0));
  // The fade where the reach ends (raw depth: larger is nearer).
  if (c10[0] > 0.0f && depth_far > 0.0 && depth_far < c10[0]) {
    next_shadow_fade_ = float(depth_far / c10[0]);
  }
}

bool Renderer::Impl::SnapshotDepth(Image* depth, bool float_depth, Image& out,
                                   const char* name) {
  EndRendering();
  if (!out.image || out.width != depth->width || out.height != depth->height) {
    if (out.image) Retire(out);
    out = gpu_.CreateImage(depth->width, depth->height, VK_FORMAT_R32_SFLOAT, false, name);
    ToGeneral(cb_, out);
  }
  const VkRect2D read{{0, 0}, {depth->width, depth->height}};
  ResolveBarrier(depth, read);
  CopyDepth(depth, &out, 0, float_depth, 0, 0, 0, 0, 0, depth->width, depth->height);
  Barrier();
  dyn_ = DynState{};
  return true;
}

Image* Renderer::Impl::OccludedScene(Image* scene) {
  if (ao_done_) return ao_result_;
  ao_done_ = true;
  ao_result_ = nullptr;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height ||
      scene->samples > 1) {
    return nullptr;
  }
  // The camera's vertical field of view from the inverse view-projection:
  // the rays through the middle and the top of the screen.
  float px_per_unit = 0.0f;
  {
    double mid[3], top[3];
    for (int k = 0; k < 2; ++k) {
      const double ndc[4] = {0.0, k ? 1.0 : 0.0, 0.5, 1.0};
      double wp[4];
      for (int r = 0; r < 4; ++r) {
        wp[r] = 0;
        for (int c = 0; c < 4; ++c) wp[r] += inv_vp_[r * 4 + c] * ndc[c];
      }
      if (std::fabs(wp[3]) < 1e-12) return nullptr;
      double* out = k ? top : mid;
      for (int r = 0; r < 3; ++r) out[r] = wp[r] / wp[3] - cam_pos_[r];
    }
    const double dm = std::sqrt(mid[0] * mid[0] + mid[1] * mid[1] + mid[2] * mid[2]);
    const double dt = std::sqrt(top[0] * top[0] + top[1] * top[1] + top[2] * top[2]);
    if (dm < 1e-9 || dt < 1e-9) return nullptr;
    const double cos_a =
        std::clamp((mid[0] * top[0] + mid[1] * top[1] + mid[2] * top[2]) / (dm * dt), -1.0, 1.0);
    const double tan_half = std::tan(std::acos(cos_a));
    if (!(tan_half > 1e-4) || !std::isfinite(tan_half)) return nullptr;
    px_per_unit = float(0.5 * double(scene->height) / tan_half);
  }
  Image* depth = &dit->second;
  // The occlusion from the world's depth (see ao_depth_); the final depth
  // only keeps it off what was drawn in front afterwards (riders, spray).
  Image* world_depth = ao_depth_ok_ && ao_depth_.width == depth->width &&
                               ao_depth_.height == depth->height
                           ? &ao_depth_
                           : depth;
  auto fits = [&](bool ok, const Image& im) {
    return ok && im.width == depth->width && im.height == depth->height;
  };
  // With the riders (kept at the scene's size), else the world again; and
  // the depth before and after the depth-only passes (the world twice when
  // there were none).
  Image* late_depth = fits(ao_late_ok_, ao_late_depth_) ? &ao_late_depth_ : world_depth;
  const bool hidden = fits(ao_before_ok_, ao_before_hidden_) &&
                      fits(ao_hidden_ok_, ao_after_hidden_) && late_depth != world_depth;
  Image* before_hidden = hidden ? &ao_before_hidden_ : world_depth;
  Image* after_hidden = hidden ? &ao_after_hidden_ : world_depth;
  const uint32_t w = scene->width, h = scene->height;
  const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  // The smaller depths for the far taps (builtin_ao_prep).
  {
    uint32_t lw = w, lh = h;
    for (int l = 0; l < 3; ++l) {
      lw = std::max((lw + 1) / 2, 1u);
      lh = std::max((lh + 1) / 2, 1u);
      ensure(ao_prep_[l], lw, lh, VK_FORMAT_R32_SFLOAT, "ao_depths");
    }
    Barrier();
    float first[4] = {1, 0, 0, 0}, next[4] = {0, 0, 0, 0};
    FullscreenPass(CopyKind::kAoPrep, &ao_prep_[0], late_depth, before_hidden, after_hidden, false, first, 4);
    for (int l = 1; l < 3; ++l) {
      Barrier();
      FullscreenPass(CopyKind::kAoPrep, &ao_prep_[l], &ao_prep_[l - 1], nullptr, nullptr, false, next, 4);
    }
  }
  ensure(ao_img_[0], hw, hh, VK_FORMAT_R16G16_SFLOAT, "ao");
  ensure(ao_img_[1], hw, hh, VK_FORMAT_R16G16_SFLOAT, "ao_blur");
  ensure(ao_out_img_, w, h, scene->format, "ambient_occlusion");
  ao_out_img_.scaled = scene->scaled;
  ao_out_img_.scale = scene->scale;
  Barrier();
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // column-major
  }
  pc[16] = cam_pos_[0];
  pc[17] = cam_pos_[1];
  pc[18] = cam_pos_[2];
  // A few metres: where the snow meets trees, rails, jumps and buildings.
  pc[19] = 2.5f;
  // (With the world light on, the shade from what's near by is in its
  // probes: the occlusion kept to the small, close-up shade under that.)
  if (GiReady() && !Diag("gifullao")) pc[19] = 1.25f;
  pc[20] = 1.0f / float(w);
  pc[21] = 1.0f / float(h);
  pc[22] = px_per_unit;
  pc[23] = 150.0f;  // none farther away (the fog takes over)
  pc[24] = 1.0f / float(hw);
  pc[25] = 1.0f / float(hh);
  pc[26] = 9.0f * ao_strength_;
  pc[27] = 0.25f * float(h);  // the screen radius, at most
  pc[28] = Diag("aofull") ? 1.0f : 0.0f;
  Barrier();
  FullscreenPass(CopyKind::kAo, &ao_img_[0], late_depth, before_hidden, after_hidden, false, pc,
                 32, nullptr, &ao_prep_[0], &ao_prep_[1], &ao_prep_[2]);
  float bx[4] = {1, 0, 0, 0}, by[4] = {0, 1, 0, 0};
  FullscreenPass(CopyKind::kAoBlur, &ao_img_[1], &ao_img_[0], nullptr, nullptr, false, bx, 4);
  FullscreenPass(CopyKind::kAoBlur, &ao_img_[0], &ao_img_[1], nullptr, nullptr, false, by, 4);
  float ap[28] = {};
  std::memcpy(ap, pc, sizeof(float) * 20);
  ap[20] = 1.0f / float(w);
  ap[21] = 1.0f / float(h);
  ap[22] = ao_debug_ ? 1.0f : 0.0f;
  // (With the time of day: the sun's share left unoccluded.)
  Image* sunlit = nullptr;
  Image* normals = nullptr;
  if (TodOn() && tod_mask_ok_ && tod_normal_ && tod_mask_.width == w && tod_mask_.height == h &&
      tod_normal_->width == w && tod_normal_->height == h && tod_normal_->samples == 1 && !tod_normal_->depth &&
      !Diag("aoall")) {
    const tod::State& t = tod_state_;
    const double day = tod::SmoothStep(-2.0 * tod::kDeg, 3.0 * tod::kDeg, t.sun_elevation);
    const double cover = std::clamp(double(tod_.clouds) + (1.0 - tod_.clouds) * tod_.overcast, 0.0, 1.0);
    const double ratio = 2.2 * day * (1.0 - 0.85 * cover) + 0.6 * (1.0 - day) * t.moon_bright;
    ap[24] = float(t.light.x);
    ap[25] = float(t.light.y);
    ap[26] = float(t.light.z);
    ap[27] = float(ratio);
    if (ratio > 0.01) {
      sunlit = &tod_mask_;
      normals = tod_normal_;
    }
  }
  if (!sunlit) ap[27] = 0.0f;
  FullscreenPass(CopyKind::kAoApply, &ao_out_img_, scene, depth, &ao_img_[0], false, ap, 28, nullptr,
                 sunlit ? sunlit : &dummy_, normals ? normals : &dummy_);
  dyn_ = DynState{};
  ++stats_.ambient_occlusion;
  ao_result_ = &ao_out_img_;
  return ao_result_;
}

void Renderer::Impl::SetWind(SystemConstants& sys, const GpuState& st, int mode) {
  // c0..c3: rows of the draw's own space -> clip. (4: a prop, its c0..c3
  // its world matrix: the world -> clip from the game's view instead.)
  double M[16], Mi[16];
  if (mode == 4) {
    const double zs = sys.ndc_scale[2], zo = sys.ndc_offset[2];
    const double K[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -zs, 1.0 - zo, 0, 0, 0, 1};
    Mul4(game_inv_vp_, K, Mi);
    if (!Invert4(Mi, M)) return;
    mode = 3;
  } else {
    for (int i = 0; i < 16; ++i) {
      float f;
      std::memcpy(&f, &st.vs_consts[i], 4);
      M[i] = f;
    }
    if (!Invert4(M, Mi)) return;
  }
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      sys.wind_m[c * 4 + r] = float(M[r * 4 + c]);  // column-major
      sys.wind_minv[c * 4 + r] = float(Mi[r * 4 + c]);
    }
  }
  const double speed = std::sqrt(double(wind_vel_[0]) * wind_vel_[0] + double(wind_vel_[1]) * wind_vel_[1]);
  if (speed < 0.05) return;
  double dir[2] = {wind_vel_[0] / speed, wind_vel_[1] / speed};
  double units = 1.0, seed = 0.0;
  if (mode == 2) {
    // Placed by c0..c3: its space -> the world (last frame's view; only its
    // axes and scale are wanted), the wind into its axes.
    const double zs = sys.ndc_scale[2], zo = sys.ndc_offset[2];
    const double K[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -zs, 1.0 - zo, 0, 0, 0, 1};
    double KM[16], W[16];
    Mul4(K, M, KM);
    Mul4(game_inv_vp_, KM, W);
    const double w = W[15];
    if (std::fabs(w) < 1e-9) return;
    double ax[3][3];  // the object's axes in the world (columns)
    for (int j = 0; j < 3; ++j) {
      for (int r = 0; r < 3; ++r) ax[j][r] = (W[r * 4 + j] - W[r * 4 + 3] * W[12 + j] / w) / w;
    }
    const double len_y = std::sqrt(ax[1][0] * ax[1][0] + ax[1][1] * ax[1][1] + ax[1][2] * ax[1][2]);
    if (!(len_y > 1e-6)) return;
    units = 1.0 / len_y;  // its units a metre
    // The wind along its own x and z (as a direction).
    double dx = (ax[0][0] * dir[0] + ax[0][2] * dir[1]), dz = (ax[2][0] * dir[0] + ax[2][2] * dir[1]);
    const double lx = ax[0][0] * ax[0][0] + ax[0][1] * ax[0][1] + ax[0][2] * ax[0][2];
    const double lz = ax[2][0] * ax[2][0] + ax[2][1] * ax[2][1] + ax[2][2] * ax[2][2];
    dx /= std::max(lx, 1e-12);
    dz /= std::max(lz, 1e-12);
    const double l = std::sqrt(dx * dx + dz * dz);
    if (l < 1e-9) return;
    dir[0] = dx / l;
    dir[1] = dz / l;
    // (Each tree its own phase: by where it stands.)
    seed = std::fmod(std::fabs(W[3] / w * 0.37 + W[11] / w * 0.53), 6.2832);
  }
  sys.wind[0] = float(dir[0]);
  sys.wind[1] = float(dir[1]);
  sys.wind[2] = float(std::min(speed, 16.0) * 0.25);  // how hard
  sys.wind[3] = float(std::fmod(wind_t_, 1000.0));
  sys.wind2[0] = float(mode);
  sys.wind2[1] = float(units);
  sys.wind2[2] = float(seed);
  sys.wind2[3] = float(std::clamp(wind_t_ - wind_prev_t_, 0.0, 0.25));
}

Image* Renderer::Impl::BloomScene(Image* scene) {
  if (scene->samples > 1 || Diag("nobloom")) return nullptr;
  const uint32_t w = scene->width, h = scene->height;
  const uint32_t qw = std::max(1u, w / 4), qh = std::max(1u, h / 4);
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  ensure(bloom_a_, qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, "bloom_a");
  ensure(bloom_b_, qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, "bloom_b");
  ensure(bloom_out_, w, h, scene->format, "bloom_out");
  bloom_out_.scaled = scene->scaled;
  bloom_out_.scale = scene->scale;
  // At night the lamps and fires glow; by day only what's near white (the
  // sun on fresh snow, a little).
  const float dark = float(LightsDark());
  const float thr = 0.9f - 0.3f * dark;
  const float strength = bloom_ * (0.25f + 0.75f * dark);
  float pc[8] = {};
  // Bright parts, a quarter the size (4 linear taps a pixel).
  pc[0] = 0.0f;
  pc[1] = thr;
  pc[2] = 1.0f / float(w);
  pc[3] = 1.0f / float(h);
  pc[4] = 0.85f;  // how much the surroundings are taken off
  Barrier();
  FullscreenPass(CopyKind::kBloom, &bloom_a_, scene, nullptr, nullptr, true, pc, 8);
  // The lights' ghosts (from the bright parts before they're blurred).
  const bool flare = flare_ > 0.0f && !Diag("noflare");
  // (The lamps' ghosts: off; the night's flare is the moon's. Diagnostic
  // "lampflare" brings them back.)
  const bool ghosts = flare && dark > 0.02f && Diag("lampflare");
  if (ghosts) FlareGhosts(&bloom_a_, scene);
  // Light pillars: at night in still air with ice crystals about (light
  // snow, or a clear frost), the lamps' light in columns above them.
  // (Off: the lamps' columns aren't wanted; diagnostic "lamppillar" for them.)
  float pillar = 0.0f;
  if (bloom_ > 0.0f && Diag("lamppillar")) {
    const double wind = std::hypot(tod_.wind[0], tod_.wind[1]), snow = std::clamp(double(tod_.snow), 0.0, 1.0);
    pillar = float(halo_ * tod::SmoothStep(0.3, 0.8, dark) * (1.0 - tod::SmoothStep(3.0, 7.0, wind)) *
                   (0.35 + 0.65 * tod::SmoothStep(0.02, 0.2, snow)) * (1.0 - tod::SmoothStep(0.5, 0.8, snow)));
    if (Diag("pillar")) pillar = 1.0f;
  }
  if (pillar > 0.01f) {
    ensure(pillar_img_, qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, "pillars");
    float pp[8] = {3.0f, 0.045f, 0.0f, 0.24f / 48.0f, 0, 0, 0, 0};
    Barrier();
    FullscreenPass(CopyKind::kBloom, &pillar_img_, &bloom_a_, nullptr, nullptr, true, pp, 8);
  }
  if (!(bloom_ > 0.0f)) {
    Image* f = flare ? FlareScene(scene, ghosts) : nullptr;
    dyn_ = DynState{};
    return f;
  }
  // Blurred: across, then down, twice (wider the second time).
  for (int pass = 0; pass < 2; ++pass) {
    const float spread = pass == 0 ? 1.0f : 2.5f;
    pc[0] = 1.0f;
    pc[1] = 0.0f;
    pc[2] = spread / float(qw);
    pc[3] = 0.0f;
    Barrier();
    FullscreenPass(CopyKind::kBloom, &bloom_b_, &bloom_a_, nullptr, nullptr, true, pc, 8);
    pc[2] = 0.0f;
    pc[3] = spread / float(qh);
    Barrier();
    FullscreenPass(CopyKind::kBloom, &bloom_a_, &bloom_b_, nullptr, nullptr, true, pc, 8);
  }
  // (The pillars with the glow, as strong whatever the glow's strength.)
  Image* glow = &bloom_a_;
  if (pillar > 0.01f) {
    float pa[8] = {4.0f, pillar / std::max(strength, 0.05f), 0, 0, 0, 0, 0, 0};
    Barrier();
    FullscreenPass(CopyKind::kBloom, &bloom_b_, &bloom_a_, &pillar_img_, nullptr, false, pa, 8);
    glow = &bloom_b_;
  }
  // Over the scene.
  pc[0] = 2.0f;
  pc[1] = strength;
  pc[2] = 1.0f / float(w);
  pc[3] = 1.0f / float(h);
  Barrier();
  FullscreenPass(CopyKind::kBloom, &bloom_out_, glow, scene, nullptr, true, pc, 8);
  Barrier();
  Image* out = &bloom_out_;
  if (flare) {
    if (Image* f = FlareScene(out, ghosts)) out = f;
  }
  dyn_ = DynState{};
  return out;
}

// The night lights' ghosts (builtin_flare 1): mirrored through the middle
// from the bright parts, a quarter the size, then blurred a little.
void Renderer::Impl::FlareGhosts(Image* bright, Image* scene) {
  const uint32_t qw = bright->width, qh = bright->height;
  for (Image* im : {&flare_img_, &flare_tmp_}) {
    if (im->image && im->width == qw && im->height == qh) continue;
    if (im->image) Retire(*im);
    *im = gpu_.CreateImage(qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, false, im == &flare_img_ ? "flare" : "flare_tmp");
    ToGeneral(cb_, *im);
  }
  float sun_uv[2], edge = 0.0f;
  const bool sun = SunOnScreen(sun_uv, &edge);
  float pc[12] = {1.0f, sun_uv[0], sun_uv[1], float(scene->width) / float(scene->height),
                  1.0f, sun && tod_state_.sun_elevation > -1.0 * tod::kDeg ? 1.0f : 0.0f, 0, 0};
  Barrier();
  FullscreenPass(CopyKind::kFlare, &flare_img_, bright, nullptr, nullptr, true, pc, 12);
  float bl[8] = {1.0f, 0.0f, 1.5f / float(qw), 0.0f, 0, 0, 0, 0};
  Barrier();
  FullscreenPass(CopyKind::kBloom, &flare_tmp_, &flare_img_, nullptr, nullptr, true, bl, 8);
  bl[2] = 0.0f;
  bl[3] = 1.5f / float(qh);
  Barrier();
  FullscreenPass(CopyKind::kBloom, &flare_img_, &flare_tmp_, nullptr, nullptr, true, bl, 8);
}

// The lens flare over the scene (builtin_flare 0 and 2): the sun's, as much
// as its disc is seen, and the lights' ghosts (FlareGhosts).
Image* Renderer::Impl::FlareScene(Image* scene, bool ghosts_ready) {
  const uint32_t w = scene->width, h = scene->height;
  auto dit = resolved_.find(depth_base_);
  Image* depth = dit != resolved_.end() && dit->second.format == VK_FORMAT_R32_SFLOAT &&
                         dit->second.samples == 1 && dit->second.width == w && dit->second.height == h
                     ? &dit->second
                     : nullptr;
  float sun_uv[2], edge = 0.0f;
  bool on = SunOnScreen(sun_uv, &edge);
  // The sun, up; or at night the moon (fainter: as bright as its phase,
  // up), through the eye's exposure.
  const tod::State& t = tod_state_;
  double day = 1.0, moon = 0.0;
  tod::Vec3 col{1.0, 0.97, 0.9};
  float seen_from = 0.8f, seen_to = 0.97f;  // how near white its disc is, seen
  if (TodOn()) {
    day = tod::SmoothStep(-1.0 * tod::kDeg, 4.0 * tod::kDeg, t.sun_elevation);
    col = t.sun_color;
    if (day < 0.01) {
      moon = 0.35 * t.moon_bright * tod::SmoothStep(0.0, 4.0 * tod::kDeg, t.moon_elevation);
      col = t.moon_color;
      seen_from = 0.45f;
      seen_to = 0.8f;
      // (Where the moon is on screen.)
      on = false;
      edge = 0.0f;
      double vp[16];
      if (moon > 0.001 && Invert4(inv_vp_, vp)) {
        const double p[4] = {cam_pos_[0] + t.moon.x * 1000.0, cam_pos_[1] + t.moon.y * 1000.0,
                             cam_pos_[2] + t.moon.z * 1000.0, 1.0};
        double c[4];
        for (int r = 0; r < 4; ++r) {
          c[r] = 0.0;
          for (int k = 0; k < 4; ++k) c[r] += vp[r * 4 + k] * p[k];
        }
        if (c[3] > 1e-6) {
          const double x = c[0] / c[3], y = c[1] / c[3];
          sun_uv[0] = float(x * 0.5 + 0.5);
          sun_uv[1] = float(0.5 - y * 0.5);
          edge = float(std::clamp(1.0 - (std::max(std::fabs(x), std::fabs(y)) - 1.0) / 0.6, 0.0, 1.0));
          on = edge > 0.0f;
        }
      }
    }
  }
  const double e = eye_strength_ > 0.0f ? std::min(exposure_, 2.0) : 1.0;
  const float sun_strength = float(flare_ * std::max(day, moon) * e);
  const bool sun = depth && sun_strength > 0.001f && (sun_valid_ || moon > 0.0);
  const float dark = float(LightsDark());
  if (!sun && !ghosts_ready) return nullptr;
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return false;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
    return true;
  };
  ensure(flare_out_, w, h, scene->format, "flare_out");
  flare_out_.scaled = scene->scaled;
  flare_out_.scale = scene->scale;
  const float aspect = float(w) / float(h);
  if (sun) {
    for (Image& v : flare_vis_) {
      if (ensure(v, 1, 1, VK_FORMAT_R16G16B16A16_SFLOAT, "flare_vis")) {
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vk::vkCmdClearColorImage(cb_, v.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
      }
    }
    const uint32_t prev = flare_vis_cur_, cur = prev ^ 1;
    const float ease = 1.0f - std::exp(-std::clamp(speed_dt_, 0.001f, 0.25f) / 0.06f);
    float pc[12] = {0.0f, on ? sun_uv[0] : -9.0f, on ? sun_uv[1] : -9.0f, aspect,
                    ease, 0.012f, seen_from, seen_to, 0, 0, 0, 0};
    Barrier();
    FullscreenPass(CopyKind::kFlare, &flare_vis_[cur], scene, depth, &flare_vis_[prev], true, pc, 12);
    flare_vis_cur_ = cur;
  }
  float pc[12] = {2.0f, sun_uv[0], sun_uv[1], aspect,
                  float(col.x), float(col.y), float(col.z), sun ? sun_strength * edge : 0.0f,
                  flare_ * dark, 0.26f, ghosts_ready ? 1.0f : 0.0f, lens_dirt_};
  Barrier();
  FullscreenPass(CopyKind::kFlare, &flare_out_, ghosts_ready ? &flare_img_ : nullptr, scene,
                 sun ? &flare_vis_[flare_vis_cur_] : nullptr, true, pc, 12);
  Barrier();
  return &flare_out_;
}

Image* Renderer::Impl::SnowScene(Image* scene) {
  if (snow_done_) return snow_result_;
  snow_done_ = true;
  snow_result_ = nullptr;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height || scene->samples > 1) {
    return nullptr;
  }
  // (Not on the trail map and the like: no rider about.)
  const float amount = std::clamp(tod_.snow, 0.0f, 1.0f) * fog_presence_;
  if (amount < 0.002f) return nullptr;
  Image* depth = &dit->second;
  const uint32_t w = scene->width, h = scene->height;
  EndRendering();
  if (!snow_img_.image || snow_img_.width != w || snow_img_.height != h || snow_img_.format != scene->format) {
    if (snow_img_.image) Retire(snow_img_);
    snow_img_ = gpu_.CreateImage(w, h, scene->format, false, "snow");
    ToGeneral(cb_, snow_img_);
  }
  snow_img_.scaled = scene->scaled;
  snow_img_.scale = scene->scale;
  float pc[32] = {};
  // ndc -> world, less the camera (the flakes are placed from it).
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      double v = inv_vp_[r * 4 + c];
      if (r < 3) v -= cam_pos_[r] * inv_vp_[12 + c];
      pc[c * 4 + r] = float(v);
    }
  }
  const double pix = PixelAngle(h);
  pc[16] = float(std::fmod(double(tod_.seconds), 600.0));
  pc[17] = amount;
  pc[18] = float(pix);
  pc[19] = 1.0f + 0.2f * std::clamp(tod_.snow - 0.6f, 0.0f, 0.4f) / 0.4f;  // (bigger flakes in a heavy fall)
  // Falling about 1 m/s, carried by the wind; streaked over 1/30 s.
  pc[20] = tod_.wind[0];
  pc[21] = -1.1f - 0.3f * tod_.snow;
  pc[22] = tod_.wind[1];
  // Streaked over a short exposure (shorter in strong wind: thin lines, not dashes).
  {
    const float ws = std::sqrt(tod_.wind[0] * tod_.wind[0] + tod_.wind[1] * tod_.wind[1]);
    pc[23] = 1.0f / (90.0f + 12.0f * ws);
  }
  {
    // Lit by the sky (and a little the sun or moon), as the snow on the
    // ground: about its brightness by day, dim at night.
    auto lum = [](const tod::Vec3& v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
    const double e = eye_strength_ > 0.0f ? exposure_ : 1.0;
    const double k = std::clamp((0.75 * lum(tod_state_.ambient_scale) + 0.35 * lum(tod_state_.light_scale)) * e, 0.03, 1.2);
    pc[24] = float(0.86 * k);
    pc[25] = float(0.89 * k);
    pc[26] = float(0.95 * k);
  }
  pc[27] = cam_pos_[0];
  pc[28] = 1.0f / float(w);
  pc[29] = 1.0f / float(h);
  pc[30] = cam_pos_[1];
  pc[31] = cam_pos_[2];
  Barrier();
  // The night lights (their tiles), when drawn this frame.
  const bool lamps = lights_result_ != nullptr && lights_img_.image && lights_img_.width >= 256;
  if (lamps) pc[19] = -pc[19];
  // The flakes' travel: the wind summed over time, in gusts (its speed
  // rising and falling, its direction wandering a little), so a gust
  // carries them faster without the whole fall jumping; eddies by the wind.
  float prm[20] = {};
  {
    static double travel[3] = {0, 0, 0};
    static double last = -1.0;
    const double now = double(tod_.seconds);
    double dt = last < 0.0 ? 0.0 : now - last;
    if (dt < 0.0 || dt > 0.5) dt = 0.0;
    last = now;
    const double ws = std::hypot(double(tod_.wind[0]), double(tod_.wind[1]));
    const bool still = Diag("nogusts");
    const double gust = still ? 1.0 : WindGust(now);
    const double turn = still ? 0.0 : 0.3 * TimeNoise(now * 0.15 + 5.0);
    const double cs = std::cos(turn), sn = std::sin(turn);
    const double wx = (tod_.wind[0] * cs - tod_.wind[1] * sn) * gust, wz = (tod_.wind[0] * sn + tod_.wind[1] * cs) * gust;
    travel[0] += wx * dt;
    travel[1] += double(pc[21]) * dt;
    travel[2] += wz * dt;
    // (Kept within a few kilometres: float precision.)
    for (double& v : travel) v = std::fmod(v, 4000.0);
    for (int i = 0; i < 3; ++i) prm[i] = float(travel[i]);
    prm[3] = float(std::min(0.05 * ws * gust, 0.9));  // turbulence (m)
    pc[20] = float(wx);  // (the streaks along the gust)
    pc[22] = float(wz);
    const double wl = std::hypot(wx, wz);
    if (wl > 0.1) {
      prm[16] = float(wx / wl);
      prm[17] = float(wz / wl);
    }
    prm[18] = float(wl);
  }
  Image* heights = &dummy_;
  prm[8] = float(kSunDepth);
  prm[10] = cam_pos_[1] - 1.5f;
  if (sun_map_ok_ && sun_height_.image && sun_r_ > 0.0 && !Diag("nosnowground")) {
    const double R = sun_r_, texel = 2.0 * R / double(sun_height_.width);
    const double hx = sun_c0_[0] + double(sun_hi_) * texel, hz = sun_c0_[2] + double(sun_hj_) * texel;
    prm[4] = float(1.0 / R);
    prm[5] = float(-hx / R);
    prm[6] = float(-hz / R);
    prm[7] = float(sun_href_);
    prm[9] = 1.0f;
    heights = &sun_height_;
  }
  Image* occ = UintDummy();
  if (GiReady() && !Diag("nosnowvox")) {
    const GiCascade& g = gi_c_[0];
    for (int i = 0; i < 3; ++i) prm[12 + i] = float(double(g.org[i]) * g.voxel);
    prm[15] = float(g.voxel);
    occ = const_cast<Image*>(&g.occ[g.cur]);
  }
  if (!snow_params_.image) {
    snow_params_ = gpu_.CreateImage(5, 1, VK_FORMAT_R32G32B32A32_SFLOAT, false, "snow_params");
    ToGeneral(cb_, snow_params_);
  }
  {
    Buffer staging = gpu_.CreateBuffer(sizeof(prm), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, prm, sizeof(prm));
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {5, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, snow_params_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    staging_.push_back(staging);
    Barrier();
  }
  FullscreenPass(CopyKind::kSnow, &snow_img_, scene, depth, lamps ? &lights_img_ : &dummy_, false, pc, 32, nullptr,
                 heights, occ, &snow_params_);
  Barrier();
  dyn_ = DynState{};
  snow_result_ = &snow_img_;
  return snow_result_;
}

Image* Renderer::Impl::FoggedScene(Image* scene) {
  if (fog_done_) return fog_result_;
  fog_done_ = true;
  fog_result_ = nullptr;
  fog_merged_ = false;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height ||
      scene->samples > 1) {
    return nullptr;
  }
  Image* depth = &dit->second;
  const uint32_t w = scene->width, h = scene->height;
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name, bool clear) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
    if (clear) {
      VkClearColorValue zero{};
      VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      vk::vkCmdClearColorImage(cb_, im.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
    }
  };
  for (Image& c : fog_color_img_) ensure(c, 2, 1, VK_FORMAT_R16G16B16A16_SFLOAT, "fog_color", true);
  ensure(fog_img_, w, h, scene->format, "fog", false);
  fog_img_.scaled = scene->scaled;
  fog_img_.scale = scene->scale;
  Barrier();
  float sun_uv[2];
  float sun_visible = 0.0f;
  const bool sun_seen = SunOnScreen(sun_uv, &sun_visible) && sun_visible >= 1.0f;
  // 1: the fog's colors from the sky (ambient, and the sun's), eased over
  // about a third of a second.
  const uint32_t prev = fog_color_cur_, cur = fog_color_cur_ ^ 1;
  const float ease = 1.0f - std::exp(-std::clamp(speed_dt_, 0.001f, 0.25f) / 0.3f);
  float pc0[8] = {ease, sun_uv[0], sun_uv[1], sun_seen ? 1.0f : 0.0f, float(w) / float(h), 0, 0, 0};
  FullscreenPass(CopyKind::kFogColor, &fog_color_img_[cur], scene, depth, &fog_color_img_[prev],
                 false, pc0, 8);
  fog_color_cur_ = cur;
  Barrier();
  Image* out = scene;
  // 2: the fog.
  if (fog_density_ > 0.0f) {
    float pc[32] = {};
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // column-major
    }
    pc[16] = cam_pos_[0];
    pc[17] = cam_pos_[1];
    pc[18] = cam_pos_[2];
    if (sun_valid_) {
      pc[20] = sun_dir_[0];
      pc[21] = sun_dir_[1];
      pc[22] = sun_dir_[2];
      // (At night the moon's glow, faint.)
      pc[23] = TodOn() ? float(1.0 - tod_state_.night) : 1.0f;
    }
    // Density 1: about half fogged 2 km away level with the camera; thinner
    // higher up (halving every ~230 m), up to twice as thick in the valleys.
    pc[24] = 0.0004f * fog_density_ * fog_presence_;
    pc[25] = fog_start_;
    pc[26] = 0.003f;
    pc[27] = cam_pos_[1] - 50.0f;
    pc[28] = fog_most_;  // never opaque (far mountains stay visible), but in a whiteout
    pc[29] = 1.0f / float(w);
    pc[30] = 1.0f / float(h);
    if (VolumeOn() && VolumeReady(scene, depth) && !Diag("nofogmerge")) {
      // (Left to the volumetric light's pass: one pass for both.)
      std::memcpy(fog_pc_, pc, sizeof(pc));
      fog_colors_now_ = &fog_color_img_[cur];
      fog_merged_ = true;
    } else {
      FullscreenPass(CopyKind::kFog, &fog_img_, scene, depth, &fog_color_img_[cur], true, pc, 32);
      Barrier();
      out = &fog_img_;
    }
    ++stats_.fogged;
  }
  // 3: god rays, from the sky around the sun while it's in (or near) view.
  if (Diag("sun")) {
    std::fprintf(stderr, "sun: valid %d dir (%.3f %.3f %.3f) uv (%.3f %.3f) visible %.2f cam (%.1f %.1f %.1f)\n",
                 sun_valid_ ? 1 : 0, sun_dir_[0], sun_dir_[1], sun_dir_[2], sun_uv[0], sun_uv[1],
                 sun_visible, cam_pos_[0], cam_pos_[1], cam_pos_[2]);
  }
  // (No god rays from the moon, nor a sun below the horizon.)
  if (TodOn()) sun_visible *= float(tod_state_.sun_disc * (1.0 - tod_state_.night));
  // (The volumetric light's shafts instead, while it's on.)
  if (VolumeOn() && SunShadowsOn()) sun_visible = 0.0f;
  if (rays_strength_ > 0.0f && sun_visible > 0.0f) {
    const uint32_t hw = std::max(w / 2, 1u), hh = std::max(h / 2, 1u);
    ensure(ray_mask_img_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, "ray_mask", false);
    ensure(ray_img_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, "rays", false);
    ensure(rays_out_img_, w, h, scene->format, "god_rays", false);
    rays_out_img_.scaled = scene->scaled;
    rays_out_img_.scale = scene->scale;
    Barrier();
    float m[8] = {sun_uv[0], sun_uv[1], float(w) / float(h), 0.35f,
                  1.0f / float(hw), 1.0f / float(hh), 0, 0};
    FullscreenPass(CopyKind::kRayMask, &ray_mask_img_, scene, depth, nullptr, true, m, 8);
    Barrier();
    float b[8] = {sun_uv[0], sun_uv[1], 0.9f, 0.965f, 1.0f / float(hw), 1.0f / float(hh), 0, 0};
    FullscreenPass(CopyKind::kRayBlur, &ray_img_, &ray_mask_img_, nullptr, nullptr, true, b, 8);
    Barrier();
    float a[8] = {3.0f * rays_strength_ * sun_visible * fog_presence_, 1.0f / float(w),
                  1.0f / float(h), float(w) / float(h), sun_uv[0], sun_uv[1], 0, 0};
    // The rays (half size) filtered in binding 0; the scene point-sampled.
    FullscreenPass(CopyKind::kRayAdd, &rays_out_img_, &ray_img_, out, nullptr, true, a, 8);
    Barrier();
    out = &rays_out_img_;
    ++stats_.god_rays;
  }
  dyn_ = DynState{};
  fog_result_ = out == scene ? nullptr : out;
  return fog_result_;
}

// The surface picker: the normals' alpha at the screen's middle (this
// frame's digit) copied to its read-back slot.
void Renderer::Impl::PickProbe(const Draw& d, const GpuState& st) {
  std::string n0;
  Image* n4 = TextureFor(d, 4, st, &n0);
  if (!n4 || n4->depth || n4->samples != 1 || n4->format != VK_FORMAT_R8G8B8A8_UNORM || n4->width < 4) return;
  if (!pick_rb_.buffer) pick_rb_ = gpu_.CreateBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  EndRendering();
  Barrier();
  const int slot = int(frame_number_ % 4);
  VkBufferImageCopy region{};
  region.bufferOffset = VkDeviceSize(slot) * 16;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(n4->width / 2), int32_t(n4->height / 2), 0};
  region.imageExtent = {1, 1, 1};
  vk::vkCmdCopyImageToBuffer(cb_, n4->image, VK_IMAGE_LAYOUT_GENERAL, pick_rb_.buffer, 1, &region);
  Barrier();
  pick_slot_digit_[slot] = pick_cur_;
  pick_slot_frame_[slot] = frame_number_;
  dyn_ = DynState{};
}

// The picker, a frame on: the listing done (digits next); a digit read back.
void Renderer::Impl::PickRead() {
  if (pick_state_ == 1) {
    if (++pick_frames_ < 2) return;  // (a whole frame listed)
    if (pick_hashes_.empty()) {
      pick_state_ = 0;
      pick_result_ = 2;
      return;
    }
    pick_digits_ = 1;
    for (size_t n = 7; n < pick_hashes_.size() && pick_digits_ < 8; n *= 7) ++pick_digits_;
    for (int& h : pick_have_) h = -1;
    for (int& sd : pick_slot_digit_) sd = -1;
    pick_state_ = 2;
    pick_cur_ = 0;
    pick_frames_ = 0;
    pick_misses_ = 0;
    return;
  }
  if (pick_state_ != 2) return;
  // Each digit in turn, round again until all are read.
  pick_cur_ = (pick_cur_ + 1) % pick_digits_;
  if (++pick_frames_ > 120) {  // (given up: nothing tagged there)
    pick_state_ = 0;
    pick_result_ = 2;
    return;
  }
  for (int slot = 0; slot < 4; ++slot) {
    if (pick_slot_digit_[slot] < 0 || frame_number_ < pick_slot_frame_[slot] + 4) continue;
    const uint8_t w = pick_rb_.mapped[slot * 16 + 3];
    const int digit = pick_slot_digit_[slot];
    pick_slot_digit_[slot] = -1;
    const int tag = w >> 5;
    if (Diag("pickdbg")) std::fprintf(stderr, "pick: slot %d digit %d byte %u (of %zu, %d digits)\n", slot, digit, w, pick_hashes_.size(), pick_digits_);
    if (tag == 0) {
      if (++pick_misses_ >= 6) {
        pick_state_ = 0;
        pick_result_ = 2;
        return;
      }
      continue;
    }
    pick_have_[digit] = tag - 1;
  }
  uint64_t index = 0, place = 1;
  for (int k = 0; k < pick_digits_; ++k) {
    if (pick_have_[k] < 0) return;
    index += uint64_t(pick_have_[k]) * place;
    place *= 7;
  }
  pick_state_ = 0;
  if (index < pick_hashes_.size()) {
    pick_hash_ = pick_hashes_[index];
    pick_found_guess_ = pick_guess_[index];
    pick_result_ = 1;
  } else {
    pick_result_ = 2;
  }
}

// The sun's sheen, at the lighting pass (builtin_sun_spec): into spec_img_.
void Renderer::Impl::SunSpecular(const Draw& d, const GpuState& st) {
  spec_ok_ = false;
  // (The sheen, and the far-off scenery lit for the hour: both or either.)
  const bool sheen = specular_ > 0.0f && !Diag("nospec");
  const bool backdrop = !Diag("nobackdrop");
  const bool bounce = (bounce_ > 0.0f && !Diag("nobounce")) || GiReady();
  bounce_ok_ = false;
  if (!(sheen || backdrop || bounce) || !TodOn() || !tod_normal_) return;
  std::string n0;
  Image* a3 = TextureFor(d, 3, st, &n0);
  auto dit = resolved_.find(depth_base_);
  if (!a3 || a3->depth || a3->samples != 1 || dit == resolved_.end() ||
      dit->second.format != VK_FORMAT_R32_SFLOAT || dit->second.samples != 1 ||
      dit->second.width != a3->width || dit->second.height != a3->height ||
      tod_normal_->width != a3->width || tod_normal_->height != a3->height) {
    return;
  }
  Image* albedo = tracks_ok_ && a3 == tracks_albedo_src_ ? &tracks_albedo_
                  : snow_cover_ok_ && a3 == snow_albedo_src_ ? &snow_albedo_ : a3;
  const tod::State& t = tod_state_;
  double fd[3] = {sun_O_[8], sun_O_[9], sun_O_[10]};
  const double fl = std::sqrt(fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2]);
  if (!(fl > 1e-12)) return;
  // The light: the sun by day, the moon (faint) at night, dimmed through a
  // grey sky (little sheen then).
  const double day = 1.0 - t.night;
  const double dim = 1.0 - 0.85 * std::clamp(double(tod_.overcast), 0.0, 1.0);
  const double e = eye_strength_ > 0.0f ? exposure_ : 1.0;
  double col[3] = {t.sun_color.x * day + t.moon_color.x * t.night * t.moon_bright * 0.12,
                   t.sun_color.y * day + t.moon_color.y * t.night * t.moon_bright * 0.12,
                   t.sun_color.z * day + t.moon_color.z * t.night * t.moon_bright * 0.14};
  for (double& c : col) c *= dim * e * 0.9;
  if (col[0] + col[1] + col[2] < 0.002 && !backdrop && !bounce) return;
  EndRendering();
  // The albedo as the lighting has it, kept for the light bounced onto it
  // later (SpecularScene).
  if (bounce && albedo->samples == 1 && !albedo->depth) {
    if (!bounce_albedo_.image || bounce_albedo_.width != albedo->width || bounce_albedo_.height != albedo->height ||
        bounce_albedo_.format != albedo->format) {
      if (bounce_albedo_.image) Retire(bounce_albedo_);
      bounce_albedo_ = gpu_.CreateImage(albedo->width, albedo->height, albedo->format, false, "bounce_albedo");
      ToGeneral(cb_, bounce_albedo_);
    }
    Barrier();
    VkImageCopy ic{};
    ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    ic.extent = {albedo->width, albedo->height, 1};
    vk::vkCmdCopyImage(cb_, albedo->image, VK_IMAGE_LAYOUT_GENERAL, bounce_albedo_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                       &ic);
    Barrier();
    bounce_ok_ = true;
  }
  // The backdrop's light: the mountain's sun it was painted with and the
  // sky's share of that light (the rest the sun's); the hour's sky and sun,
  // as the lighting has them (scaled per color), its direction; how far the
  // sun is from the mountain's (its shadows then from the mask).
  // (The lighting's depth target, for its stencil: which pixels it lights.)
  Image* zt = nullptr;
  float stt[4] = {};
  {
    const uint32_t depth_ctl = st.reg(0x2200), srm = st.reg(0x210D);
    if ((depth_ctl & 1) && !Diag("nostenciltest")) {
      zt = GetTarget(DepthKey(st.reg(0x2000), st.reg(0x2002)));
      if (zt && (!zt->depth || !zt->stencil_view || zt->width != a3->width || zt->height != a3->height)) zt = nullptr;
    }
    if (zt) {
      stt[0] = float(srm & 0xFF);
      stt[1] = float((srm >> 8) & 0xFF);
      stt[2] = float((depth_ctl >> 8) & 7);
      stt[3] = 1.0f;
    }
    if (Diag("sun")) {
      std::fprintf(stderr, "spec: lighting stencil %s ref %u mask %u func %u\n", zt ? "on" : "off", srm & 0xFF,
                   (srm >> 8) & 0xFF, (depth_ctl >> 8) & 7);
    }
  }
  {
    float f[24] = {};
    for (int i = 0; i < 3; ++i) f[i] = tod_game_light_[i];
    f[3] = 0.4f;
    f[4] = float(lm_ambient_scale_.x);
    f[5] = float(lm_ambient_scale_.y);
    f[6] = float(lm_ambient_scale_.z);
    f[7] = backdrop ? 1.0f : 0.0f;
    f[8] = float(lm_light_scale_.x);
    f[9] = float(lm_light_scale_.y);
    f[10] = float(lm_light_scale_.z);
    f[11] = float(std::clamp(tod_state_.away, 0.0, 1.0));
    f[12] = float(lm_dir_.x);
    f[13] = float(lm_dir_.y);
    f[14] = float(lm_dir_.z);
    f[15] = sheen ? 1.0f : 0.0f;
    if (!lm_set_dir_) {
      for (int i = 0; i < 3; ++i) f[12 + i] = tod_game_light_[i];
    }
    for (int i = 0; i < 4; ++i) f[16 + i] = stt[i];
    f[20] = bounce ? 1.0f : 0.0f;  // (texel 9: the bounce on: the depth kept where there's no sheen)
    // One row: the sky's colors (texels 0-3, copied), these (4-8).
    if (!relight_params_.image) {
      relight_params_ = gpu_.CreateImage(12, 1, VK_FORMAT_R32G32B32A32_SFLOAT, false, "sheen_settings");
      ToGeneral(cb_, relight_params_);
    }
    Buffer staging = gpu_.CreateBuffer(sizeof(f), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, f, sizeof(f));
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {4, 0, 0};
    region.imageExtent = {6, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, relight_params_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    staging_.push_back(staging);
    if (tod_colors_.image && tod_colors_.width >= 4) {
      VkImageCopy ic{};
      ic.srcSubresource = ic.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      ic.extent = {4, 1, 1};
      vk::vkCmdCopyImage(cb_, tod_colors_.image, VK_IMAGE_LAYOUT_GENERAL, relight_params_.image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &ic);
    }
    Barrier();
  }
  if (!spec_img_.image || spec_img_.width != a3->width || spec_img_.height != a3->height) {
    if (spec_img_.image) Retire(spec_img_);
    spec_img_ = gpu_.CreateImage(a3->width, a3->height, VK_FORMAT_R16G16B16A16_SFLOAT, false, "sun_spec");
    ToGeneral(cb_, spec_img_);
  }
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);
  }
  for (int i = 0; i < 3; ++i) pc[16 + i] = float(-fd[i] / fl);
  pc[19] = Diag("specdbg") ? 1.0f : 0.0f;
  // The sky's reflection (its colors as the sky was last drawn).
  pc[28] = tod_colors_.image && !Diag("noenvspec") ? float(e) * 0.8f : 0.0f;
  for (int i = 0; i < 3; ++i) pc[20 + i] = float(col[i]);
  pc[24] = cam_pos_[0];
  pc[25] = cam_pos_[1];
  pc[26] = cam_pos_[2];
  pc[27] = tod_mask_ok_ ? 1.0f : 0.0f;
  Barrier();
  // (Its depth: a copy-sized float would lose nothing; R16F alpha keeps the
  // match test's tolerance relative.)
  // (The stencil read as the depth target's stencil aspect; without one, a
  // stand-in the shader doesn't read.)
  Image stencil_in = zt ? *zt : spec_stencil_dummy_;
  if (zt) stencil_in.sample_view = zt->stencil_view;
  FullscreenPass(zt && zt->samples > 1 ? CopyKind::kSunSpecMs : CopyKind::kSunSpec, &spec_img_, albedo, tod_normal_,
                 &dit->second, false, pc, 32, nullptr, tod_mask_ok_ ? &tod_mask_ : &tod_white_, &relight_params_,
                 &stencil_in);
  Barrier();
  dyn_ = DynState{};
  spec_ok_ = true;
}

// The sun's sheen added (builtin_spec_add), after the scene.
// World-space light, 1: the world voxelised (see gi_). At each sun map
// update's start the cascades' boxes are put round the camera and their
// maps cleared; each part of the update draws its share of the world into
// them; at its end the occupancy is built from them, merged with what was
// there (shifted to the new box).
void Renderer::Impl::GiVoxelize(std::vector<CasterDraw>& env, bool start, bool done) {
  if (Diag("gidbg")) std::fprintf(stderr, "gi: voxelize %zu draws start %d done %d\n", env.size(), start, done);
  const VkFormat fmt = sun_env_.format;
  for (int k = 0; k < 2; ++k) {
    GiCascade& g = gi_c_[k];
    g.voxel = k == 0 ? 1.0 : 4.0;
    for (int m = 0; m < 5; ++m) {
      Image& im = g.maps[m];
      if (!im.image || im.width != kGiMap || im.format != fmt) {
        if (im.image) Retire(im);
        im = gpu_.CreateImage(kGiMap, kGiMap, fmt, true, "gi_map");
        ToGeneral(cb_, im);
        g.ok = false;
        g.drawing = false;
      }
    }
    for (Image& im : g.occ) {
      if (!im.image || im.width != kGiN) {
        if (im.image) Retire(im);
        im = gpu_.CreateImage(kGiN, kGiN, VK_FORMAT_R32G32B32A32_UINT, false, "gi_occupancy");
        ToGeneral(cb_, im);
        g.ok = false;
      }
    }
    if (start || !g.drawing) {
      // Round the camera (a little more of it below: the ground), whole voxels.
      int64_t want[3];
      for (int i = 0; i < 3; ++i) {
        const double c = std::floor(cam_pos_[i] / g.voxel);
        want[i] = int64_t(c) - int64_t(i == 1 ? kGiN * 5 / 8 : kGiN / 2);
      }
      // (Only once the camera's an eighth of the box from where it was put:
      // each update draws the world five times a cascade.)
      int64_t moved = 0;
      for (int i = 0; i < 3; ++i) moved = std::max<int64_t>(moved, std::llabs(want[i] - g.org[i]));
      g.skip = g.ok && moved < int64_t(kGiN / 8) && !Diag("gievery");
      if (g.skip) {
        g.drawing = true;
        continue;
      }
      std::memcpy(g.pending, want, sizeof(want));
      const float zero[4] = {0, 0, 0, 0};
      for (Image& im : g.maps) ClearImage(&im, zero, 1.0f, 0, false, true, true);
      FlushPendingClears();
      g.drawing = true;
    }
    if (g.skip) {
      if (done) g.drawing = false;
      continue;
    }
    const double S = double(kGiN) * g.voxel;
    const double B[3] = {double(g.pending[0]) * g.voxel, double(g.pending[1]) * g.voxel,
                         double(g.pending[2]) * g.voxel};
    // Rows world -> clip: (u, v, depth) for each view: from above (u x,
    // v z), from +x and -x (u z, v y), from +z and -z (u x, v y).
    for (int m = 0; m < 5; ++m) {
      double O[16] = {};
      const int ua = m == 0 ? 0 : (m <= 2 ? 2 : 0);
      const int va = m == 0 ? 2 : 1;
      const int da = m == 0 ? 1 : (m <= 2 ? 0 : 2);
      const bool from_max = m == 0 || m == 1 || m == 3;
      O[0 * 4 + ua] = 2.0 / S;
      O[0 * 4 + 3] = -2.0 * B[ua] / S - 1.0;
      O[1 * 4 + va] = 2.0 / S;
      O[1 * 4 + 3] = -2.0 * B[va] / S - 1.0;
      if (from_max) {
        O[2 * 4 + da] = -1.0 / S;
        O[2 * 4 + 3] = (B[da] + S) / S;
      } else {
        O[2 * 4 + da] = 1.0 / S;
        O[2 * 4 + 3] = -B[da] / S;
      }
      O[15] = 1.0;
      DrawIntoSunMap(env, &g.maps[m], O);
    }
    if (done) {
      const int nxt = 1 - g.cur;
      float pc[8] = {float(kGiN), float(kGiMap),
                     float(g.pending[0] - g.org[0]), float(g.pending[1] - g.org[1]),
                     float(g.pending[2] - g.org[2]), g.ok ? 1.0f : 0.0f, 0, 0};
      Barrier();
      // (Five maps and the occupancy before: the sixth input.)
      FullscreenPass(CopyKind::kGiOcc, &g.occ[nxt], &g.maps[0], &g.maps[1], &g.maps[2], false, pc, 8,
                     nullptr, &g.maps[3], &g.maps[4], &g.occ[g.cur]);
      Barrier();
      g.cur = nxt;
      for (int i = 0; i < 3; ++i) g.org[i] = g.pending[i];
      g.ok = true;
      g.drawing = false;
      ++stats_.gi_updates;
    }
  }
  dyn_ = DynState{};
}

// World-space light, 2 (half size, into bounce_img_ for the bounce's
// upsizing and adding): each pixel's rays (cosine-weighted round its
// normal) traced through the cascades. What a ray hits: lit as the screen
// shows it, where it's on screen and the depth there agrees; else its
// light worked out (a guess at its color from which way it faces, the
// sky's light from the lighting's own spherical harmonics, the sun's where
// a ray toward it gets out). Each hit takes off the sky's light the game
// gave from that way and puts its own on.
void Renderer::Impl::GiTrace(Image* scene, Image* depth) {
  gi_probes_now_ = false;
  if (Diag("gidbg")) std::fprintf(stderr, "gi: trace\n");
  const uint32_t* blk = lm_on_ ? tod_new_ : tod_ref_;
  float f[42 * 4] = {};
  f[20 * 4] = Diag("giview") ? 1.0f : 0.0f;
  double vp[16];
  if (!Invert4(inv_vp_, vp)) return;
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      f[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // texels 0-3: columns
      f[16 + c * 4 + r] = float(vp[r * 4 + c]);   // 4-7
    }
  }
  for (int k = 0; k < 2; ++k) {
    const GiCascade& g = gi_c_[k];
    for (int i = 0; i < 3; ++i) f[(8 + k) * 4 + i] = float(double(g.org[i]) * g.voxel);
    f[(8 + k) * 4 + 3] = float(g.voxel);
  }
  // Toward the sun (the lighting's c14), how bright its light is on a
  // surface facing it (about what lit snow shows).
  for (int i = 0; i < 3; ++i) std::memcpy(&f[10 * 4 + i], &blk[i], 4);
  const tod::Vec3 ls = lm_light_scale_;
  f[10 * 4 + 3] = float(0.9 * (ls.x + ls.y + ls.z) / 3.0);
  for (int i = 0; i < 3; ++i) f[11 * 4 + i] = float(cam_pos_[i]);
  f[11 * 4 + 3] = 600.0f;
  // The sky's light: the lighting's spherical harmonics (its first set):
  // cAr, cAg, cAb (c15, c19, c23), cBr, cBg, cBb (c27, c31, c35), cC (c39).
  const int regs[7] = {15, 19, 23, 27, 31, 35, 39};
  for (int j = 0; j < 7; ++j) std::memcpy(&f[(12 + j) * 4], &blk[(regs[j] - 14) * 4], 16);
  // (With the frames before: half the pixels, fewer rays each, blended.)
  const bool probes = !Diag("gipixel") && !Diag("giview");
  const bool temporal = !probes && !Diag("ginotemporal") && !Diag("giview");
  int64_t plo[3];
  for (int i = 0; i < 3; ++i) {
    plo[i] = int64_t(std::floor(cam_pos_[i] / kProbeSpacing)) - (i == 1 ? kProbeDims[i] * 3 / 8 : kProbeDims[i] / 2);
    f[21 * 4 + i] = float(plo[i]);
    f[22 * 4 + i] = float(kProbeDims[i]);
  }
  f[21 * 4 + 3] = float(kProbeSpacing);
  // (The fine grid: texels 23, 24.)
  for (int i = 0; i < 3; ++i) {
    f[23 * 4 + i] = float(int64_t(std::floor(cam_pos_[i] / kProbeFine)) -
                          (i == 1 ? kProbeDims[i] * 3 / 8 : kProbeDims[i] / 2));
    f[24 * 4 + i] = float(kProbeDims[i]);
  }
  f[23 * 4 + 3] = Diag("ginofine") ? 0.0f : float(kProbeFine);
  // The night lights nearest the camera (after dark), for what the rays hit
  // off screen (on screen their light is in the picture already): texels
  // 26 + 2 i (where, how far it reaches), 27 + 2 i (its light), up to 8.
  {
    const double dark = LightsDark();
    std::vector<std::pair<double, size_t>> close_by;
    if (dark > 0.001 && !Diag("ginolamps")) {
      for (size_t i = 0; i < lights_.size(); ++i) {
        const Renderer::NightLight& l = lights_[i];
        if (l.preview) continue;
        double d2 = 0;
        for (int k = 0; k < 3; ++k) d2 += (l.pos[k] - cam_pos_[k]) * (l.pos[k] - cam_pos_[k]);
        if (d2 < 150.0 * 150.0) close_by.push_back({d2, i});
      }
      std::sort(close_by.begin(), close_by.end());
    }
    const int n = int(std::min<size_t>(close_by.size(), 8));
    for (int j = 0; j < n; ++j) {
      const Renderer::NightLight& l = lights_[close_by[size_t(j)].second];
      for (int k = 0; k < 3; ++k) {
        f[(26 + 2 * j) * 4 + k] = l.pos[k];
        f[(27 + 2 * j) * 4 + k] = float(l.color[k] * l.intensity * dark);
      }
      f[(26 + 2 * j) * 4 + 3] = std::max(l.radius, 0.5f);
    }
    f[20 * 4 + 2] = float(n);
  }
  f[22 * 4 + 3] = float(frame_number_ % 4096);
  f[20 * 4 + 1] = temporal ? 1.0f : 0.0f;
  f[19 * 4 + 0] = Diag("gimany") ? 32.0f : probes ? 10.0f : temporal ? 6.0f : 8.0f;  // rays
  f[19 * 4 + 1] = Diag("ginosky") ? 0.0f : 1.0f;    // the sky's light taken off
  f[19 * 4 + 2] = Diag("ginoput") ? 0.0f : 1.0f;    // the hits' light put on
  f[19 * 4 + 3] = float(frame_number_ % 64);
  if (!gi_params_.image) {
    gi_params_ = gpu_.CreateImage(42, 1, VK_FORMAT_R32G32B32A32_SFLOAT, false, "gi_params");
    ToGeneral(cb_, gi_params_);
  }
  Buffer staging = gpu_.CreateBuffer(sizeof(f), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
  std::memcpy(staging.mapped, f, sizeof(f));
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {42, 1, 1};
  Barrier();
  vk::vkCmdCopyBufferToImage(cb_, staging.buffer, gi_params_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  staging_.push_back(staging);
  Barrier();
  const GiCascade& c0 = gi_c_[0];
  const GiCascade& c1 = gi_c_[1];
  // (A quarter of the size each way: the light round a point changes slowly;
  // blurred, then read with its depth when added.)
  // (The probes read at every pixel: cheap, and a quarter-size pixel's one
  // sample of a tree, leaf one frame and sky behind the next, flickered.)
  const uint32_t qs = probes ? (Diag("gihalf") ? 2u : 1u) : Diag("gihalf") ? 2u : 4u;
  const uint32_t qw = std::max(scene->width / qs, 1u), qh = std::max(scene->height / qs, 1u);
  if (!gi_img_.image || gi_img_.width != qw || gi_img_.height != qh) {
    if (gi_img_.image) Retire(gi_img_);
    gi_img_ = gpu_.CreateImage(qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, false, "world_light");
    ToGeneral(cb_, gi_img_);
  }
  if (probes) {
    const uint32_t pw = uint32_t(6 * kProbeDims[0]), rows = uint32_t(kProbeDims[1] * kProbeDims[2]),
                   ph = rows * 2;
    for (Image& im : gi_probe_) {
      if (!im.image || im.width != pw || im.height != ph) {
        if (im.image) Retire(im);
        im = gpu_.CreateImage(pw, ph, VK_FORMAT_R16G16B16A16_SFLOAT, false, "world_light_probes");
        ToGeneral(cb_, im);
        const float zero[4] = {0, 0, 0, 0};
        ClearImage(&im, zero, 1.0f, 0, true, false, false);
        FlushPendingClears();
      }
    }
    const int nxt = 1 - gi_probe_cur_;
    float pp[8] = {1.0f / float(scene->width), 1.0f / float(scene->height), float(kGiN), 1.0f, 0, 0, 0, 0};
    // An eighth of the rows (two layers of probes) traced again a frame, the
    // rest copied over.
    Barrier();
    VkImageCopy all{};
    all.srcSubresource = all.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    all.extent = {pw, ph, 1};
    vk::vkCmdCopyImage(cb_, gi_probe_[gi_probe_cur_].image, VK_IMAGE_LAYOUT_GENERAL, gi_probe_[nxt].image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &all);
    Barrier();
    // (The coarse grid's a sixteenth of its rows (one layer), the fine
    // one's an eighth (two).)
    // (The coarse grid a sixteenth, the fine one a thirty-second: the fine
    // grid's eighth with the coarse one's sixteenth cost 4.6 ms on an Intel
    // Arc iGPU, its probes' rays four times a coarse one's work.)
    const uint32_t cb = rows / 16, fb = rows / 32;
    const VkRect2D band{{0, int32_t((frame_number_ % 16) * cb)}, {pw, cb}};
    FullscreenPass(CopyKind::kGiProbes, &gi_probe_[nxt], scene, depth, &gi_probe_[gi_probe_cur_], true, pp, 8,
                   &band, const_cast<Image*>(&c0.occ[c0.cur]), const_cast<Image*>(&c1.occ[c1.cur]),
                   &gi_params_);
    if (!Diag("ginofine")) {
      const VkRect2D fband{{0, int32_t(rows + ((frame_number_ + 8) % 32) * fb)}, {pw, fb}};
      FullscreenPass(CopyKind::kGiProbes, &gi_probe_[nxt], scene, depth, &gi_probe_[gi_probe_cur_], true, pp, 8,
                     &fband, const_cast<Image*>(&c0.occ[c0.cur]), const_cast<Image*>(&c1.occ[c1.cur]),
                     &gi_params_);
    }
    Barrier();
    gi_probe_cur_ = nxt;
    MarkPart("world light: probes");
    float ap[4] = {1.0f / float(scene->width), 1.0f / float(scene->height), float(qs), 0};
    FullscreenPass(CopyKind::kGiApply, &gi_img_, depth, tod_normal_, &gi_probe_[nxt], false, ap, 4, nullptr,
                   nullptr, nullptr, &gi_params_);
    Barrier();
    std::memcpy(gi_prev_vp_, vp, sizeof(vp));
    gi_prev_frame_ = frame_number_;
    ++stats_.gi_traces;
    gi_probes_now_ = true;
    return;
  }
  float pc[4] = {1.0f / float(scene->width), 1.0f / float(scene->height), float(kGiN), float(qs)};
  FullscreenPass(CopyKind::kGiTrace, &gi_img_, scene, depth, tod_normal_, true, pc, 4, nullptr,
                 const_cast<Image*>(&c0.occ[c0.cur]), const_cast<Image*>(&c1.occ[c1.cur]), &gi_params_);
  if (temporal) {
    for (Image& im : gi_hist_) {
      if (!im.image || im.width != qw || im.height != qh) {
        if (im.image) Retire(im);
        im = gpu_.CreateImage(qw, qh, VK_FORMAT_R16G16B16A16_SFLOAT, false, "world_light_history");
        ToGeneral(cb_, im);
        gi_hist_ok_ = false;
      }
    }
    // (Only straight after the frame before: not over a jump or a pause.)
    const bool valid = gi_hist_ok_ && frame_number_ == gi_prev_frame_ + 1;
    const int nxt = 1 - gi_hist_cur_;
    float tp[32];
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) {
        tp[c * 4 + r] = float(inv_vp_[r * 4 + c]);
        tp[16 + c * 4 + r] = float(gi_prev_vp_[r * 4 + c]);
      }
    }
    // (The history's validity rides in the last matrix's unused corner:
    // prev vp's [3][3] stays 1 for a valid one, 0 otherwise.)
    if (!valid) tp[31] = 0.0f;
    Barrier();
    FullscreenPass(CopyKind::kGiTemporal, &gi_hist_[nxt], &gi_img_, &gi_hist_[gi_hist_cur_], nullptr, false, tp,
                   32);
    Barrier();
    gi_hist_cur_ = nxt;
    gi_hist_ok_ = true;
    // (Blurred from the history into gi_img_, the history kept unblurred.)
    VkImageCopy all{};
    all.srcSubresource = all.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    all.extent = {qw, qh, 1};
    vk::vkCmdCopyImage(cb_, gi_hist_[nxt].image, VK_IMAGE_LAYOUT_GENERAL, gi_img_.image, VK_IMAGE_LAYOUT_GENERAL,
                       1, &all);
    Barrier();
  } else {
    gi_hist_ok_ = false;
  }
  std::memcpy(gi_prev_vp_, vp, sizeof(vp));
  gi_prev_frame_ = frame_number_;
  if (!Diag("ginoblur") && !Diag("giview")) {
    if (!gi_blur_.image || gi_blur_.width != gi_img_.width || gi_blur_.height != gi_img_.height) {
      if (gi_blur_.image) Retire(gi_blur_);
      gi_blur_ = gpu_.CreateImage(gi_img_.width, gi_img_.height, gi_img_.format, false, "gi_blur");
      ToGeneral(cb_, gi_blur_);
    }
    // (Twice each way, the second wider; once with the frames before.)
    for (int pass = 0; pass < (temporal ? 1 : 2); ++pass) {
      const float st = pass == 0 ? 1.0f : 2.0f;
      float h[4] = {st, 0, 0, 0}, v[4] = {0, st, 0, 0};
      Barrier();
      FullscreenPass(CopyKind::kGiBlur, &gi_blur_, &gi_img_, nullptr, nullptr, false, h, 4);
      Barrier();
      FullscreenPass(CopyKind::kGiBlur, &gi_img_, &gi_blur_, nullptr, nullptr, false, v, 4);
    }
  }
  ++stats_.gi_traces;
}

Image* Renderer::Impl::SpecularScene(Image* scene) {
  if (!spec_ok_) return nullptr;
  spec_ok_ = false;
  if (pick_state_ == 2) return nullptr;  // (the tags are the picker's now)
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT || dit->second.samples != 1 ||
      dit->second.width != scene->width || dit->second.height != scene->height || scene->samples > 1 ||
      spec_img_.width != scene->width || spec_img_.height != scene->height) {
    return nullptr;
  }
  EndRendering();
  if (!spec_out_.image || spec_out_.width != scene->width || spec_out_.height != scene->height ||
      spec_out_.format != scene->format) {
    if (spec_out_.image) Retire(spec_out_);
    spec_out_ = gpu_.CreateImage(scene->width, scene->height, scene->format, false, "sun_spec_lit");
    ToGeneral(cb_, spec_out_);
  }
  spec_out_.scaled = scene->scaled;
  spec_out_.scale = scene->scale;
  // The light bounced onto the world (half size), from the scene as lit.
  const bool bounce = bounce_ok_ && bounce_albedo_.width == scene->width && bounce_albedo_.height == scene->height &&
                      tod_normal_ && tod_normal_->width == scene->width && tod_normal_->height == scene->height &&
                      tod_normal_->samples == 1 && !tod_normal_->depth;
  bounce_ok_ = false;
  if (bounce) {
    const uint32_t hw = std::max(scene->width / 2, 1u), hh = std::max(scene->height / 2, 1u);
    if (!bounce_img_.image || bounce_img_.width != hw || bounce_img_.height != hh) {
      if (bounce_img_.image) Retire(bounce_img_);
      bounce_img_ = gpu_.CreateImage(hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, false, "light_bounce");
      ToGeneral(cb_, bounce_img_);
    }
    // Pixels a metre, a metre away (the vertical field of view).
    float px_per_unit = 0.0f;
    {
      double mid[3], top[3];
      bool okv = true;
      for (int k = 0; k < 2 && okv; ++k) {
        const double ndc[4] = {0.0, k ? 1.0 : 0.0, 0.5, 1.0};
        double wp[4];
        for (int r = 0; r < 4; ++r) {
          wp[r] = 0;
          for (int c = 0; c < 4; ++c) wp[r] += inv_vp_[r * 4 + c] * ndc[c];
        }
        okv = std::fabs(wp[3]) > 1e-12;
        double* out = k ? top : mid;
        for (int r = 0; r < 3 && okv; ++r) out[r] = wp[r] / wp[3] - cam_pos_[r];
      }
      const double dm = okv ? std::sqrt(mid[0] * mid[0] + mid[1] * mid[1] + mid[2] * mid[2]) : 0.0;
      const double dt = okv ? std::sqrt(top[0] * top[0] + top[1] * top[1] + top[2] * top[2]) : 0.0;
      if (dm > 1e-9 && dt > 1e-9) {
        const double ca = std::clamp((mid[0] * top[0] + mid[1] * top[1] + mid[2] * top[2]) / (dm * dt), -1.0, 1.0);
        const double th = std::tan(std::acos(ca));
        if (th > 1e-4 && std::isfinite(th)) px_per_unit = float(0.5 * double(scene->height) / th);
      }
    }
    float bp[28] = {};
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) bp[c * 4 + r] = float(inv_vp_[r * 4 + c]);
    }
    bp[16] = cam_pos_[0];
    bp[17] = cam_pos_[1];
    bp[18] = cam_pos_[2];
    bp[19] = 3.0f;  // a few metres round
    bp[20] = 1.0f / float(scene->width);
    bp[21] = 1.0f / float(scene->height);
    bp[22] = px_per_unit;
    bp[23] = 120.0f;
    bp[24] = float(frame_number_ % 64);
    bp[25] = 0.12f * float(scene->height);
    Barrier();
    if (GiReady()) {
      MarkPart("effect: sun sheen (before the world light)");
      GiTrace(scene, &dit->second);
      MarkPart("world light: trace");
    } else {
      FullscreenPass(CopyKind::kBounce, &bounce_img_, scene, &dit->second, tod_normal_, true, bp, 28);
    }
    Barrier();
  }
  const bool gi = bounce && GiReady();
  float pc[4] = {specular_,
                 Diag("specdbg") || Diag("specshow") ? 1.0f
                 : Diag("bounceshow") || Diag("gishow") ? -1.0f
                                                       : 0.0f,
                 bounce ? (gi ? gi_ : bounce_) : 0.0f, gi ? 1.0f : 0.0f};
  Barrier();
  FullscreenPass(CopyKind::kSpecAdd, &spec_out_, &spec_img_, scene, &dit->second, false, pc, 4, nullptr,
                 gi ? &gi_img_ : bounce ? &bounce_img_ : &dummy_, bounce ? &bounce_albedo_ : &dummy_);
  Barrier();
  dyn_ = DynState{};
  return &spec_out_;
}

// The air's density for the volumetric light (per metre, at the camera's
// height): a little haze always; more under a grey sky, in mist (the fog),
// and in falling snow.
float Renderer::Impl::VolumeDensity() const {
  const float overcast = std::clamp(tod_.overcast, 0.0f, 1.0f);
  const float snow = std::clamp(tod_.snow, 0.0f, 1.0f);
  const float mist = std::max(fog_density_, 0.0f) * fog_presence_;
  return volumetric_ * (0.0004f + 0.0015f * overcast + 0.008f * snow + 0.003f * std::min(mist, 4.0f));
}

bool Renderer::Impl::VolumeReady(const Image* scene, const Image* depth) const {
  return vol_shadow_ok_ && tod_movers_map_.image && depth && depth->format == VK_FORMAT_R32_SFLOAT &&
         depth->samples == 1 && depth->width == scene->width && depth->height == scene->height &&
         scene->samples == 1;
}

// A pixel's angle: the view's height in ndc over the screen's pixels.
double Renderer::Impl::PixelAngle(uint32_t h) const {
  double pix = 0.0015;
  auto ray = [&](double y, double out[3]) {
    double v[4] = {};
    const double n[4] = {0.0, y, 0.5, 1.0};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) v[r] += inv_vp_[r * 4 + k] * n[k];
    }
    for (int r = 0; r < 3; ++r) out[r] = v[r] / v[3] - cam_pos_[r];
  };
  double a[3], b[3];
  ray(0.0, a);
  ray(1.0, b);
  const double la = std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
  const double d = std::sqrt((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) +
                             (b[2] - a[2]) * (b[2] - a[2]));
  if (la > 1e-9 && d > 0) pix = d / la / (h * 0.5);
  return pix;
}

// Time of day turned on or off (builtin_tod_fade): the last picture shown
// before eased into the new over 0.7 s (the camera moves a little meanwhile:
// too short to tell). Each frame's kept for that.
Image* Renderer::Impl::TodFade(Image* scene) {
  if (scene->samples > 1 || scene->depth) return nullptr;
  const bool on = TodOn() && !tod_.passive;  // (the look changing, not the effects)
  const bool kept = fade_copy_frame_ + 1 == frame_number_ && fade_img_.image &&
                    fade_img_.width == scene->width && fade_img_.height == scene->height &&
                    fade_img_.format == scene->format;
  if (on != fade_last_on_) {
    fade_last_on_ = on;
    if (kept && !Diag("nofade")) {
      fade_active_ = true;
      fade_t_ = 0.0f;
    }
  }
  EndRendering();
  auto ensure = [&](Image& im, const char* name) {
    if (im.image && im.width == scene->width && im.height == scene->height && im.format == scene->format) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(scene->width, scene->height, scene->format, false, name);
    ToGeneral(cb_, im);
  };
  Image* out = nullptr;
  if (fade_active_ && kept) {
    fade_t_ += std::clamp(speed_dt_, 0.001f, 0.1f) / 0.7f;
    if (fade_t_ >= 1.0f) {
      fade_active_ = false;
    } else {
      ensure(fade_out_, "tod_fade");
      fade_out_.scaled = scene->scaled;
      fade_out_.scale = scene->scale;
      const float t = fade_t_ * fade_t_ * (3.0f - 2.0f * fade_t_);
      float pc[4] = {t, 0, 0, 0};
      Barrier();
      FullscreenPass(CopyKind::kTodFade, &fade_out_, &fade_img_, scene, nullptr, false, pc, 4);
      Barrier();
      out = &fade_out_;
    }
  } else {
    fade_active_ = false;
  }
  // (What's shown kept for the next frame.)
  ensure(fade_img_, "tod_fade_before");
  Image* shown = out ? out : scene;
  VkImageCopy all{};
  all.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  all.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  all.extent = {scene->width, scene->height, 1};
  Barrier();
  vk::vkCmdCopyImage(cb_, shown->image, VK_IMAGE_LAYOUT_GENERAL, fade_img_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &all);
  Barrier();
  fade_copy_frame_ = frame_number_;
  dyn_ = DynState{};
  return out;
}

// Diamond dust (builtin_dust): on a clear, calm day, ice crystals in the air
// glinting in the sun, the most with it low (mornings and evenings).
Image* Renderer::Impl::DustScene(Image* scene) {
  if (!TodOn() || dust_ <= 0.0f || scene->samples > 1 || Diag("nodust")) return nullptr;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT || dit->second.samples != 1 ||
      dit->second.width != scene->width || dit->second.height != scene->height) {
    return nullptr;
  }
  const tod::State& t = tod_state_;
  const double el = t.sun_elevation;
  const double day = tod::SmoothStep(0.0, 3.0 * tod::kDeg, el);
  const double low = 0.35 + 0.65 * (1.0 - tod::SmoothStep(15.0 * tod::kDeg, 40.0 * tod::kDeg, el));
  const double cover = std::clamp(double(tod_.clouds) + (1.0 - tod_.clouds) * tod_.overcast, 0.0, 1.0);
  const double clear = 1.0 - tod::SmoothStep(0.25, 0.6, cover);
  const double dry = 1.0 - tod::SmoothStep(0.02, 0.15, double(tod_.snow));
  const double speed = std::hypot(tod_.wind[0], tod_.wind[1]);
  const double calm = 1.0 - tod::SmoothStep(4.0, 9.0, speed);
  const float amount = float(std::min(1.0, dust_ * day * low * clear * dry * calm));
  if (amount < 0.01f) return nullptr;
  const uint32_t w = scene->width, h = scene->height;
  EndRendering();
  if (!dust_out_.image || dust_out_.width != w || dust_out_.height != h || dust_out_.format != scene->format) {
    if (dust_out_.image) Retire(dust_out_);
    dust_out_ = gpu_.CreateImage(w, h, scene->format, false, "dust");
    ToGeneral(cb_, dust_out_);
  }
  dust_out_.scaled = scene->scaled;
  dust_out_.scale = scene->scale;
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) {
      double v = inv_vp_[r * 4 + c];
      if (r < 3) v -= cam_pos_[r] * inv_vp_[12 + c];
      pc[c * 4 + r] = float(v);
    }
  }
  pc[16] = float(std::fmod(double(tod_.seconds), 600.0));
  pc[17] = amount;
  pc[18] = float(PixelAngle(h));
  pc[20] = tod_.wind[0] * 0.6f;
  pc[21] = -0.12f;
  pc[22] = tod_.wind[1] * 0.6f;
  pc[23] = cam_pos_[0];
  const double e = eye_strength_ > 0.0f ? std::min(exposure_, 2.0) : 1.0;
  pc[24] = float(t.sun_color.x * e);
  pc[25] = float(t.sun_color.y * e);
  pc[26] = float(t.sun_color.z * e);
  pc[27] = cam_pos_[1];
  pc[28] = float(t.sun.x);
  pc[29] = float(t.sun.y);
  pc[30] = float(t.sun.z);
  pc[31] = cam_pos_[2];
  Barrier();
  FullscreenPass(CopyKind::kDust, &dust_out_, scene, &dit->second, nullptr, false, pc, 32);
  Barrier();
  dyn_ = DynState{};
  return &dust_out_;
}

// Drifting snow over the ground in a strong wind (builtin_drift).
Image* Renderer::Impl::DriftScene(Image* scene) {
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT || dit->second.samples != 1 ||
      scene->samples > 1) {
    return nullptr;
  }
  const float speed = std::hypot(tod_.wind[0], tod_.wind[1]);
  // (From a stiff breeze to a gale: 5 .. 18 m/s.)
  const float amount = std::clamp((speed - 4.5f) / 12.0f, 0.0f, 1.0f);
  if (amount <= 0.0f) return nullptr;
  if (Diag("driftdiag")) std::fprintf(stderr, "drift: amount %.2f scene %ux%u\n", amount, scene->width, scene->height);
  EndRendering();
  if (!drift_out_.image || drift_out_.width != scene->width || drift_out_.height != scene->height ||
      drift_out_.format != scene->format) {
    if (drift_out_.image) Retire(drift_out_);
    drift_out_ = gpu_.CreateImage(scene->width, scene->height, scene->format, false, "drift");
    ToGeneral(cb_, drift_out_);
  }
  drift_out_.scaled = scene->scaled;
  drift_out_.scale = scene->scale;
  float pc[24] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);
  }
  pc[16] = cam_pos_[0];
  pc[17] = cam_pos_[1];
  pc[18] = cam_pos_[2];
  // (The clock the streams move by runs faster in a gust, slower between:
  // the same gusts as the falling snow's.)
  {
    static double gust_clock = 0.0, last = -1.0;
    const double now = double(tod_.seconds);
    double dt = last < 0.0 ? 0.0 : now - last;
    if (dt < 0.0 || dt > 0.5) dt = 0.0;
    last = now;
    gust_clock = std::fmod(gust_clock + dt * (Diag("nogusts") ? 1.0 : WindGust(now)), 3600.0);
    pc[19] = float(gust_clock);
  }
  pc[20] = tod_.wind[0] / speed;
  pc[21] = tod_.wind[1] / speed;
  pc[22] = speed;
  pc[23] = amount;
  Barrier();
  FullscreenPass(CopyKind::kDrift, &drift_out_, scene, &dit->second, nullptr, false, pc, 24);
  Barrier();
  dyn_ = DynState{};
  return &drift_out_;
}

// Volumetric light: the sun's (or moon's) in the air, marched at half size
// through the shadow maps, then added (builtin_volume / builtin_volume_add;
// with the fog when it was left to this pass: builtin_fog_volume).
Image* Renderer::Impl::VolumeScene(Image* scene) {
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end()) return nullptr;
  Image* depth = &dit->second;
  const bool fog = fog_merged_;
  fog_merged_ = false;
  if (!VolumeReady(scene, depth)) return nullptr;
  const tod::State& t = tod_state_;
  // The light: the sun by day; the moon's, faint, at night.
  const double day = 1.0 - t.night;
  double col[3] = {t.sun_color.x * day + t.moon_color.x * t.night * t.moon_bright * 0.04,
                   t.sun_color.y * day + t.moon_color.y * t.night * t.moon_bright * 0.04,
                   t.sun_color.z * day + t.moon_color.z * t.night * t.moon_bright * 0.05};
  // (Through a grey sky the sun's dimmed: little to make shafts.)
  const double dim = 1.0 - 0.8 * std::clamp(double(tod_.overcast), 0.0, 1.0);
  const double e = eye_strength_ > 0.0f ? exposure_ : 1.0;
  // (A low sun lights the haze far more: shafts at either end of the day,
  // little at noon.)
  const double low = 1.0 - tod::SmoothStep(0.06, 0.5, t.sun_elevation);
  for (double& c : col) c *= dim * e * (0.45 + 1.5 * low);
  double fd[3] = {sun_O_[8], sun_O_[9], sun_O_[10]};
  const double fl = std::sqrt(fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2]);
  const bool lit = col[0] + col[1] + col[2] >= 0.002 && fl > 1e-12;
  if (!lit && !fog) return nullptr;
  const uint32_t w = scene->width, h = scene->height;
  const uint32_t div = volumetric_quality_ == 0 ? 4 : 2;
  const uint32_t hw = std::max(w / div, 1u), hh = std::max(h / div, 1u);
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  ensure(vol_img_, hw, hh, VK_FORMAT_R16G16B16A16_SFLOAT, "volume");
  ensure(vol_params_, 8, 1, VK_FORMAT_R32G32B32A32_SFLOAT, "volume_params");
  ensure(vol_out_, w, h, scene->format, "volume_lit");
  vol_out_.scaled = scene->scaled;
  vol_out_.scale = scene->scale;
  // (Toward the light: back along the sun map's depth axis, fd.)
  const float far_m = 220.0f;
  const int steps = volumetric_quality_ == 0 ? 24 : volumetric_quality_ == 1 ? 28 : 48;
  {
    Buffer staging = gpu_.CreateBuffer(8 * 16, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    float* f = reinterpret_cast<float*>(staging.mapped);
    std::memset(f, 0, 8 * 16);
    f[0] = cam_pos_[0];
    f[1] = cam_pos_[1];
    f[2] = cam_pos_[2];
    f[3] = far_m;
    f[4] = float(col[0]);
    f[5] = float(col[1]);
    f[6] = float(col[2]);
    f[7] = VolumeDensity();
    for (int i = 0; i < 3; ++i) f[8 + i] = lit ? float(-fd[i] / fl) : (i == 1 ? 1.0f : 0.0f);
    f[11] = 0.55f;
    f[12] = cam_pos_[1] - 30.0f;  // densest a little below the camera (in the valleys)
    f[13] = 1.0f / 160.0f;
    f[14] = float(steps);
    f[15] = float(1 | (vol_world_ok_ ? 2 : 0));
    std::memcpy(f + 16, vol_cloud_, sizeof(vol_cloud_));
    f[20] = 1.0f / float(hw);
    f[21] = 1.0f / float(hh);
    f[22] = float(w);
    f[23] = float(h);
    std::memcpy(f + 24, cloud_map_at_, sizeof(float) * 4);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {8, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, vol_params_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    staging_.push_back(staging);
  }
  Barrier();
  if (lit) {
    FullscreenPass(CopyKind::kVolume, &vol_img_, depth, &tod_movers_map_,
                   vol_world_ok_ ? &sun_env_ : &tod_movers_map_, false, vol_shadow_pc_, 32, nullptr, &vol_params_,
                   cloud_map_.image ? &cloud_map_ : &vol_params_);
    Barrier();
  }
  if (fog) {
    // The fog first, then the air's light: one pass.
    float fp[32];
    std::memcpy(fp, fog_pc_, sizeof(fp));
    fp[31] = lit ? 1.0f : 0.0f;
    FullscreenPass(CopyKind::kFogVolume, &vol_out_, scene, depth, fog_colors_now_, true, fp, 32, nullptr,
                   &vol_img_);
    Barrier();
    dyn_ = DynState{};
    return &vol_out_;
  }
  float pc[24] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);
  }
  pc[16] = 1.0f / float(w);
  pc[17] = 1.0f / float(h);
  pc[18] = 1.0f;
  pc[19] = far_m;
  pc[20] = cam_pos_[0];
  pc[21] = cam_pos_[1];
  pc[22] = cam_pos_[2];
  FullscreenPass(CopyKind::kVolumeAdd, &vol_out_, &vol_img_, scene, depth, false, pc, 24);
  Barrier();
  dyn_ = DynState{};
  return &vol_out_;
}

// Time of day, at the lighting pass: its sky-light table (tf1) tinted for
// the hour; the white stand-in for the game's fixed shadows made.
// Snow tracks, kept: this frame's trail draws drawn again from above into
// the stamp (round the camera), pressed into the map (its window), the map
// moved along with the camera (a quarter of it at a time) and filling in
// while it snows.
void Renderer::Impl::TracksUpdate() {
  std::vector<CasterDraw> draws;
  draws.swap(tracks_draws_);
  tracks_seen_.clear();
  if (tracks_ <= 0.0f || Diag("notracks")) {
    tracks_valid_ = false;
    return;
  }
  auto make = [&](Image& im, uint32_t n, VkFormat f, bool depth, const char* name) {
    if (im.image && im.width == n && im.format == f) return false;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(n, n, f, depth, name);
    ToGeneral(cb_, im);
    return true;
  };
  EndRendering();
  bool fresh = make(tracks_map_, kTracksN, VK_FORMAT_R8G8B8A8_UNORM, false, "snow_tracks");
  make(tracks_scratch_, kTracksN, VK_FORMAT_R8G8B8A8_UNORM, false, "snow_tracks_scratch");
  make(tracks_stamp_, kTracksW, VK_FORMAT_D32_SFLOAT_S8_UINT, true, "snow_tracks_stamp");
  make(tracks_stamp_more_[0], kTracksW, VK_FORMAT_D32_SFLOAT_S8_UINT, true, "snow_tracks_stamp_2");
  make(tracks_stamp_more_[1], kTracksW, VK_FORMAT_D32_SFLOAT_S8_UINT, true, "snow_tracks_stamp_3");
  make(tracks_tmp_, kTracksW, VK_FORMAT_R8G8B8A8_UNORM, false, "snow_tracks_window");
  const int64_t tx = std::llround(cam_pos_[0] / kTracksTexel), tz = std::llround(cam_pos_[2] / kTracksTexel);
  const double jx = cam_pos_[0] - tracks_last_cam_[0], jy = cam_pos_[1] - tracks_last_cam_[1],
               jz = cam_pos_[2] - tracks_last_cam_[2];
  const bool jump = jx * jx + jy * jy + jz * jz > 150.0 * 150.0;
  for (int i = 0; i < 3; ++i) tracks_last_cam_[i] = double(cam_pos_[i]);
  const int64_t N = kTracksN, W = kTracksW;
  if (fresh || !tracks_valid_ || jump) {
    // (A new place: another mountain, a restart.)
    const float zero[4] = {0, 0, 0, 0};
    ClearImage(&tracks_map_, zero, 1.0f, 0, true, false, false);
    FlushPendingClears();
    tracks_cx_ = tx;
    tracks_cz_ = tz;
    tracks_valid_ = true;
  } else if (std::llabs(tx - tracks_cx_) > N / 4 || std::llabs(tz - tracks_cz_) > N / 4) {
    ShiftMap(&tracks_map_, &tracks_scratch_, tx - tracks_cx_, tz - tracks_cz_);
    tracks_cx_ = tx;
    tracks_cz_ = tz;
  }
  // Falling snow fills them in (a heavy fall: in a couple of minutes), and
  // the wind wears them away: the loose heaps blown off (a gale: in a minute
  // or so), the grooves drifting in more slowly (a few minutes), the shapes
  // softening and creeping downwind as the loose snow moves. The whole map
  // every so often.
  {
    static auto last = std::chrono::steady_clock::now();
    static float pending = 0.0f, scour = 0.0f, drift = 0.0f, waited = 0.0f, aged = 0.0f;
    static double carried[2] = {0.0, 0.0};
    const auto now = std::chrono::steady_clock::now();
    const float dt = std::min(std::chrono::duration<float>(now - last).count(), 0.25f);
    last = now;
    const bool test_wind = std::getenv("A3_TEST_WIND") != nullptr;
    if (TodOn() || test_wind) {
      pending += std::clamp(tod_.snow, 0.0f, 1.0f) * dt / 90.0f;
      double wx = tod_.wind[0], wz = tod_.wind[1];
      if (test_wind) {
        wx = std::atof(std::getenv("A3_TEST_WIND"));  // (m/s, along x)
        wz = 0.0;
      }
      const double speed = std::hypot(wx, wz);
      if (speed > 0.1) {
        scour += float(tod::SmoothStep(1.5, 14.0, speed)) * dt / 40.0f;
        drift += float(tod::SmoothStep(2.5, 15.0, speed)) * dt / 150.0f;
        // (The loose snow at the surface: about a hundredth of the wind's
        // speed, in texels.)
        const double moved = tod::SmoothStep(2.0, 10.0, speed) * 0.012 * speed * dt / kTracksTexel;
        carried[0] += wx / speed * moved;
        carried[1] += wz / speed * moved;
      }
      waited += dt;
    }
    // Wind or none, the tracks wear away with time: each texel's freshness
    // runs down over kTracksLife seconds from when it was last pressed, the
    // track fading over the last third of that (the oldest parts first).
    if (!Diag("tracksforever")) aged += dt / float(kTracksLife);
    // (In whole 255ths: the freshness is 8-bit.)
    const float age_step = std::floor(aged * 255.0f) / 255.0f;
    if (pending > 0.03f || ((scour > 0.02f || drift > 0.02f) && waited > 0.5f) || age_step >= 2.0f / 255.0f) {
      // (Carried at most two texels a pass: the rest next time.)
      const float cx = float(std::clamp(carried[0], -2.0, 2.0)), cz = float(std::clamp(carried[1], -2.0, 2.0));
      carried[0] -= cx;
      carried[1] -= cz;
      float pc[12] = {0.0f, -1.0f, std::min(pending, 1.0f), 0.0f,
                      cx, cz, std::min(scour, 0.5f), std::min(drift, 0.5f), age_step};
      aged -= age_step;
      scour = drift = waited = 0.0f;
      Barrier();
      FullscreenPass(CopyKind::kTracksMerge, &tracks_scratch_, &tracks_stamp_, &tracks_map_, nullptr,
                     false, pc, 12);
      Barrier();
      VkImageCopy all{};
      all.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      all.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      all.extent = {uint32_t(N), uint32_t(N), 1};
      vk::vkCmdCopyImage(cb_, tracks_scratch_.image, VK_IMAGE_LAYOUT_GENERAL, tracks_map_.image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &all);
      Barrier();
      pending = 0.0f;
    }
  }
  // (Test: a board ridden ahead of the camera, curving, then skidding to a
  // stop: A3_TEST_TRACKS, the frames counted.)
  static int test_frame = 0;
  const bool test = std::getenv("A3_TEST_TRACKS") != nullptr;
  if (draws.empty() && !test) {
    dyn_ = DynState{};
    return;
  }
  // The window: round the camera, in the map.
  const int64_t ox = std::clamp<int64_t>(tx - tracks_cx_ + N / 2 - W / 2, 0, N - W);
  const int64_t oz = std::clamp<int64_t>(tz - tracks_cz_ + N / 2 - W / 2, 0, N - W);
  const double x0 = double(tracks_cx_ - N / 2 + ox) * kTracksTexel;
  const double z0 = double(tracks_cz_ - N / 2 + oz) * kTracksTexel;
  const double span = double(W) * kTracksTexel;
  const float far_depth[4] = {0, 0, 0, 0};
  ClearImage(&tracks_stamp_, far_depth, 1.0f, 0, false, true, true);
  for (Image& im : tracks_stamp_more_) ClearImage(&im, far_depth, 1.0f, 0, false, true, true);
  FlushPendingClears();
  float tb[8] = {};
  if (test) {
    // The camera's forward on the ground, and right.
    double f[2] = {0, 1};
    {
      double v[4] = {};
      const double n[4] = {0, 0, 0.5, 1};
      for (int r = 0; r < 4; ++r) for (int k = 0; k < 4; ++k) v[r] += inv_vp_[r * 4 + k] * n[k];
      const double fx = v[0] / v[3] - cam_pos_[0], fz = v[2] / v[3] - cam_pos_[2];
      const double fl = std::sqrt(fx * fx + fz * fz);
      if (fl > 1e-6) f[0] = fx / fl, f[1] = fz / fl;
    }
    const double rgt[2] = {f[1], -f[0]};
    const int k = test_frame++;
    // 0..34 a curve away (0.4 m a frame), then 12 frames skidding sideways
    // to a stop.
    const int kturn = std::getenv("A3_TEST_TRACKS")[0] == 's' ? 16 : 34;
    const int kc = std::min(k, kturn);
    double along = 3.0 + 0.4 * kc, side = 3.0 * std::sin(kc * 0.18) + (std::getenv("A3_TEST_TRACKS")[0] == 's' ? -4.5 : 0.0);
    const double heading = std::atan2(0.4, 3.0 * 0.18 * std::cos(kc * 0.18));  // from the side axis
    double board = heading;  // along its run
    if (k > kturn) {
      const int ks = std::min(k - kturn, 12);
      along += 0.35 * ks * (1.0 - ks / 24.0);
      side += 0.35 * ks * (1.0 - ks / 24.0) / std::tan(heading);
      board = heading + 1.45;  // across its run
    }
    // The board's middle and direction in the world, then the window's texels.
    const double wx = cam_pos_[0] + f[0] * along + rgt[0] * side;
    const double wz = cam_pos_[2] + f[1] * along + rgt[1] * side;
    const double dside = std::cos(board), dalong = std::sin(board);
    double dx = f[0] * dalong + rgt[0] * dside, dz = f[1] * dalong + rgt[1] * dside;
    tb[0] = float((wx - x0) / kTracksTexel);
    tb[1] = float((wz - z0) / kTracksTexel);
    tb[2] = float(dx);
    tb[3] = float(dz);
    tb[4] = 0.75f / float(kTracksTexel);
    tb[5] = (k > kturn ? 0.2f : 0.14f) / float(kTracksTexel);
    tb[6] = 1.0f;
  }
  const double O[16] = {2.0 / span, 0, 0, -1.0 - 2.0 * x0 / span,
                        0, 0, 2.0 / span, -1.0 - 2.0 * z0 / span,
                        0, -1.0 / 1000.0, 0, 0.5 + cam_pos_[1] / 1000.0,
                        0, 0, 0, 1};
  DrawIntoSunMap(draws, &tracks_stamp_, O, nullptr);
  // (Again where the trail's texture is stronger: pressed deeper there. The
  // whole of a trail's strip pressed alike made wide flat patches with hard
  // edges where the game's mark is faint, heaps lit all along them.)
  const bool graded = !Diag("tracksflat");
  if (graded && !draws.empty()) {
    for (int k = 0; k < 2; ++k) {
      std::vector<CasterDraw> more = draws;
      for (CasterDraw& c : more) {
        if (c.sys.alpha_func == 6) c.sys.alpha_ref = k == 0 ? 0.15f : 0.3f;
      }
      DrawIntoSunMap(more, &tracks_stamp_more_[k], O, nullptr);
    }
  }
  // (How much each cut pushes up beside it.)
  float pc[12] = {float(ox), float(oz), 0.0f, 0.035f};
  std::memcpy(pc + 4, tb, sizeof(tb));
  Barrier();
  pc[11] = graded && !test ? 1.0f : 0.0f;
  FullscreenPass(CopyKind::kTracksMerge, &tracks_tmp_, &tracks_stamp_, &tracks_map_, graded ? &tracks_stamp_more_[0] : nullptr,
                 false, pc, 12, nullptr, graded ? &tracks_stamp_more_[1] : nullptr);
  Barrier();
  VkImageCopy r{};
  r.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  r.dstOffset = {int32_t(ox), int32_t(oz), 0};
  r.extent = {uint32_t(W), uint32_t(W), 1};
  vk::vkCmdCopyImage(cb_, tracks_tmp_.image, VK_IMAGE_LAYOUT_GENERAL, tracks_map_.image,
                     VK_IMAGE_LAYOUT_GENERAL, 1, &r);
  Barrier();
  dyn_ = DynState{};
}

// Snow tracks, at the lighting pass: its albedo (after the fresh snow) and
// normals with the tracks in them, given to it instead of its own.
void Renderer::Impl::TracksInputs(const Draw& d, const GpuState& st, Image* albedo_in) {
  tracks_ok_ = false;
  tracks_albedo_src_ = tracks_normal_src_ = nullptr;
  if (tracks_ <= 0.0f || !tracks_valid_ || !tracks_map_.image) return;
  std::string n0;
  Image* a3 = TextureFor(d, 3, st, &n0);
  Image* n4 = TextureFor(d, 4, st, &n0);
  auto dit = resolved_.find(depth_base_);
  if (!a3 || !n4 || a3->depth || n4->depth || a3->samples != 1 || n4->samples != 1 || n4 == &dummy_ ||
      dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != a3->width || dit->second.height != a3->height ||
      n4->width != a3->width || n4->height != a3->height) {
    return;
  }
  if (!albedo_in) albedo_in = a3;
  EndRendering();
  auto ensure = [&](Image& im, const Image* like, const char* name) {
    if (im.image && im.width == like->width && im.height == like->height && im.format == like->format) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(like->width, like->height, like->format, false, name);
    ToGeneral(cb_, im);
  };
  ensure(tracks_albedo_, a3, "snow_tracks_albedo");
  ensure(tracks_normal_, n4, "snow_tracks_normals");
  // (Sampled as the lighting samples its own: at the same scale.)
  tracks_albedo_.scaled = a3->scaled;
  tracks_albedo_.scale = a3->scale;
  tracks_normal_.scaled = n4->scaled;
  tracks_normal_.scale = n4->scale;
  float pc[28] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);
  }
  const int64_t N = kTracksN;
  pc[16] = float(double(tracks_cx_ - N / 2) * kTracksTexel);
  pc[17] = float(double(tracks_cz_ - N / 2) * kTracksTexel);
  pc[18] = float(1.0 / kTracksTexel);
  pc[20] = 0.06f * tracks_;  // a groove's depth (m)
  pc[21] = 0.15f * tracks_;  // a heap's height (all piled up)
  pc[22] = 1.0f / float(a3->width);
  pc[23] = 1.0f / float(a3->height);
  pc[24] = cam_pos_[0];
  pc[25] = cam_pos_[1];
  pc[26] = cam_pos_[2];
  Barrier();
  pc[19] = Diag("matid") ? 3.0f : Diag("tracksshow") ? 2.0f : 0.0f;
  FullscreenPass(CopyKind::kTracksApply, &tracks_albedo_, albedo_in, n4, &dit->second, false, pc, 28,
                 nullptr, &tracks_map_);
  pc[19] = 1.0f;
  FullscreenPass(CopyKind::kTracksApply, &tracks_normal_, n4, n4, &dit->second, false, pc, 28, nullptr,
                 &tracks_map_);
  Barrier();
  dyn_ = DynState{};
  tracks_albedo_src_ = a3;
  tracks_normal_src_ = n4;
  tracks_ok_ = true;
}

// The lighting's albedo and normals with the fresh snow and the boards'
// tracks in them (its own replaced by these). Again for each of the game's
// two lighting passes (its two samples, with MSAA: each resolved anew).
void Renderer::Impl::SceneInputs(const Draw& d, const GpuState& st) {
  std::string n0;
  // Fresh snow: on what faces up (the albedo whitened, by its normals),
  // patchy at first, given to the lighting instead of its own.
  snow_cover_ok_ = false;
  snow_albedo_src_ = nullptr;
  if (TodOn() && tod_.snow_cover > 0.01f && !Diag("nosnowcover")) {
    Image* a3 = TextureFor(d, 3, st, &n0);
    Image* n4 = TextureFor(d, 4, st, &n0);
    auto dit = resolved_.find(depth_base_);
    if (a3 && n4 && !a3->depth && a3->samples == 1 && n4 != &dummy_ && dit != resolved_.end() &&
        dit->second.format == VK_FORMAT_R32_SFLOAT && dit->second.width == a3->width &&
        dit->second.height == a3->height) {
      EndRendering();
      if (!snow_albedo_.image || snow_albedo_.width != a3->width || snow_albedo_.height != a3->height ||
          snow_albedo_.format != a3->format) {
        if (snow_albedo_.image) Retire(snow_albedo_);
        snow_albedo_ = gpu_.CreateImage(a3->width, a3->height, a3->format, false, "snow_cover_albedo");
        ToGeneral(cb_, snow_albedo_);
      }
      float pc[28] = {};
      for (int c = 0; c < 4; ++c) {
        for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);
      }
      // (The world light's fine voxels: where the snow blows in, drifts.)
      Image* occ = UintDummy();
      if (GiReady() && !Diag("nosnowvox")) {
        const GiCascade& g = gi_c_[0];
        for (int i = 0; i < 3; ++i) pc[24 + i] = float(double(g.org[i]) * g.voxel);
        pc[27] = float(g.voxel);
        occ = const_cast<Image*>(&g.occ[g.cur]);
      }
      pc[16] = std::clamp(tod_.snow_cover, 0.0f, 1.0f);
      pc[17] = 1.0f / float(a3->width);
      pc[18] = 1.0f / float(a3->height);
      pc[19] = Diag("matid") ? 1.0f : Diag("snowdrifts") ? -1.0f : 0.0f;
      pc[20] = tod_.wind[0];
      pc[21] = tod_.wind[1];
      pc[22] = float(cam_pos_[0]);
      pc[23] = float(cam_pos_[2]);
      Barrier();
      FullscreenPass(CopyKind::kSnowCover, &snow_albedo_, a3, n4, &dit->second, false, pc, 28, nullptr, occ);
      Barrier();
      dyn_ = DynState{};
      snow_albedo_src_ = a3;
      snow_cover_ok_ = true;
    }
  }
  // The boards' tracks pressed into it (over the fresh snow).
  TracksInputs(d, st, snow_cover_ok_ ? &snow_albedo_ : nullptr);
}

void Renderer::Impl::TodLightingInputs(const Draw& d, const GpuState& st) {
  tod_lut_ok_ = false;
  {
    std::string n0;
    tod_game_map_ = TodOn() ? TextureFor(d, 0, st, &n0) : nullptr;
    SceneInputs(d, st);
    // (The albedo is the scene's own image, lit over by this pass: a copy.)
    tod_albedo_ = nullptr;
    bool preview = false;
    for (const auto& l : lights_) preview |= l.preview || l.selected;
    // (Copies only with a light in view to use them.)
    Image* alb = TodOn() && !lights_.empty() && (LightsDark() > 0.001 || preview) &&
                         !VisibleLights().empty()
                     ? TextureFor(d, 3, st, &n0)
                     : nullptr;
    if (alb && !alb->depth && alb->samples == 1) {
      EndRendering();
      if (!lights_albedo_.image || lights_albedo_.width != alb->width ||
          lights_albedo_.height != alb->height) {
        if (lights_albedo_.image) Retire(lights_albedo_);
        lights_albedo_ = gpu_.CreateImage(alb->width, alb->height, VK_FORMAT_R8G8B8A8_UNORM, false,
                                          "night_lights_albedo");
        ToGeneral(cb_, lights_albedo_);
      }
      Barrier();
      const float one[4] = {1, 1, 1, 0};
      FullscreenPass(CopyKind::kTodTint, &lights_albedo_,
                     tracks_ok_ && alb == tracks_albedo_src_ ? &tracks_albedo_
                     : snow_cover_ok_ && alb == snow_albedo_src_ ? &snow_albedo_ : alb,
                     nullptr, nullptr, false, one, 4);
      Barrier();
      tod_albedo_ = &lights_albedo_;
      // The world's depth as the lighting has it (riders, drawn after, told
      // apart by it): a copy.
      lights_wdepth_ok_ = false;
      auto dit = resolved_.find(depth_base_);
      if (dit != resolved_.end() && dit->second.format == VK_FORMAT_R32_SFLOAT &&
          dit->second.samples == 1) {
        Image* wd = &dit->second;
        if (!lights_wdepth_.image || lights_wdepth_.width != wd->width ||
            lights_wdepth_.height != wd->height) {
          if (lights_wdepth_.image) Retire(lights_wdepth_);
          lights_wdepth_ = gpu_.CreateImage(wd->width, wd->height, VK_FORMAT_R32_SFLOAT, false,
                                            "night_lights_world_depth");
          ToGeneral(cb_, lights_wdepth_);
        }
        Barrier();
        FullscreenPass(CopyKind::kTodTint, &lights_wdepth_, wd, nullptr, nullptr, false, one, 4);
        Barrier();
        lights_wdepth_ok_ = true;
      }
      dyn_ = DynState{};
    }
    tod_normal_ = TodOn() ? TextureFor(d, 4, st, &n0) : nullptr;
    if (tod_normal_ && tracks_ok_ && tod_normal_ == tracks_normal_src_) tod_normal_ = &tracks_normal_;
  }
  if (!LightModOn()) return;
  auto ensure = [&](Image& im, uint32_t w, uint32_t h, VkFormat f, const char* name) {
    if (im.image && im.width == w && im.height == h && im.format == f) return false;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(w, h, f, false, name);
    ToGeneral(cb_, im);
    return true;
  };
  EndRendering();
  if (ensure(tod_white_, 4, 4, VK_FORMAT_R8G8B8A8_UNORM, "tod_white")) {
    VkClearColorValue one{};
    for (float& v : one.float32) v = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::vkCmdClearColorImage(cb_, tod_white_.image, VK_IMAGE_LAYOUT_GENERAL, &one, 1, &range);
  }
  if (ensure(spec_stencil_dummy_, 1, 1, VK_FORMAT_R8_UINT, "sheen_stencil_none")) {
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::vkCmdClearColorImage(cb_, spec_stencil_dummy_.image, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
  }
  TodShadows();
  SunSpecular(d, st);
  if (pick_state_ == 2) PickProbe(d, st);
  std::string note;
  Image* src = TextureFor(d, 1, st, &note);
  if (!src || src->depth || src->samples > 1 || src->width > 4096 || src->height > 4096) {
    Barrier();
    return;
  }
  ensure(tod_lut_, src->width, src->height, VK_FORMAT_R16G16B16A16_SFLOAT, "tod_sky_light");
  Barrier();
  // What the air adds: as bright and as blue as the sky's light.
  const tod::Vec3& a = lm_ambient_scale_;
  float pc[4] = {float(a.x), float(a.y), float(a.z), 0.0f};
  FullscreenPass(CopyKind::kTodTint, &tod_lut_, src, nullptr, nullptr, false, pc, 4);
  Barrier();
  tod_lut_src_ = src;
  tod_lut_ok_ = true;
  dyn_ = DynState{};
}

// Eye adaptation, at the lighting pass: the measure from 4 frames back (the
// GPU long done with it) and the exposure eased toward what it calls for:
// brighter in the dark (slowly, as eyes do), darker in bright snow (faster).
void Renderer::Impl::EyeUpdate() {
  if (eye_strength_ <= 0.0f) {
    exposure_ = 1.0;
    return;
  }
  const int slot = int(frame_number_ % 4);
  if (eye_rb_.buffer && eye_rb_frame_[slot] && frame_number_ >= eye_rb_frame_[slot] + 4) {
    float mean_log;
    std::memcpy(&mean_log, eye_rb_.mapped + slot * 16, 4);
    eye_rb_frame_[slot] = 0;
    const double shown = std::exp(double(mean_log));
    const double scene = shown / std::max(eye_rb_exposure_[slot], 1e-3);
    if (std::isfinite(scene) && scene > 1e-5) {
      // What the scene's average brightness calls for (0.42: about the
      // game's own look by day), part of the way (the strength), within
      // what eyes do: down a little in glare, up to 2x in the dark.
      const double target =
          std::clamp(std::pow(0.42 / scene, double(eye_strength_)), 0.75, 2.0);
      const double dt = std::clamp(double(speed_dt_), 0.001, 0.25);
      const double tau = target > exposure_ ? 2.0 : 0.7;
      exposure_ += (target - exposure_) * (1.0 - std::exp(-dt / tau));
      if (Diag("eyesnap")) exposure_ = target;  // (the replayer: no easing)
    }
  }
}

// Eye adaptation, after the scene: its average brightness (log, center
// weighted) to a 1 x 1 image, copied to this frame's read-back slot.
void Renderer::Impl::EyeMeasure(Image* scene) {
  if (eye_done_) return;
  eye_done_ = true;
  if (scene->samples > 1) return;
  EndRendering();
  if (!eye_img_.image) {
    eye_img_ = gpu_.CreateImage(1, 1, VK_FORMAT_R32_SFLOAT, false, "eye_measure");
    ToGeneral(cb_, eye_img_);
  }
  if (!eye_rb_.buffer) {
    eye_rb_ = gpu_.CreateBuffer(64, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  }
  Barrier();
  float pc[4] = {0, 0, 0, 0};
  FullscreenPass(CopyKind::kEyeMeasure, &eye_img_, scene, nullptr, nullptr, true, pc, 4);
  Barrier();
  const int slot = int(frame_number_ % 4);
  VkBufferImageCopy region{};
  region.bufferOffset = VkDeviceSize(slot) * 16;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {1, 1, 1};
  vk::vkCmdCopyImageToBuffer(cb_, eye_img_.image, VK_IMAGE_LAYOUT_GENERAL, eye_rb_.buffer, 1, &region);
  Barrier();
  eye_rb_frame_[slot] = frame_number_;
  eye_rb_exposure_[slot] = eye_strength_ > 0.0f ? exposure_ : 1.0;
  dyn_ = DynState{};
}

// Time of day, at the lighting pass: the riders', rails' and objects'
// shadows from the hour's sun (the game's shadow pass's casters drawn again
// into a map like the world's, its frame), as a screen mask.
// The cloud layer's noise as the shaders have it (builtin_cloud_shadow).
namespace {
float CloudHash(float x, float y) {
  auto fr = [](float v) { return v - std::floor(v); };
  x = fr(x * 443.897f);
  y = fr(y * 441.423f);
  const float d = x * (y + 19.19f) + y * (x + 19.19f);
  x += d;
  y += d;
  return fr((x + y) * x);
}
float CloudNoise(float x, float y) {
  const float ix = std::floor(x), iy = std::floor(y), fx = x - ix, fy = y - iy;
  const float wx = fx * fx * (3.0f - 2.0f * fx), wy = fy * fy * (3.0f - 2.0f * fy);
  const float a = CloudHash(ix, iy), b = CloudHash(ix + 1, iy), c = CloudHash(ix, iy + 1), d = CloudHash(ix + 1, iy + 1);
  return (a + (b - a) * wx) + ((c + (d - c) * wx) - (a + (b - a) * wx)) * wy;
}
float CloudFbm(float x, float y) {
  float v = 0.0f, a = 0.5f;
  for (int i = 0; i < 5; ++i) {
    v += a * CloudNoise(x, y);
    x = x * 2.03f + 17.1f;
    y = y * 2.03f + 9.7f;
    a *= 0.5f;
  }
  return v;
}
}  // namespace

void Renderer::Impl::TodShadows() {
  std::vector<CasterDraw> draws;
  draws.swap(tod_movers_draws_);
  stats_.tod_casters = uint32_t(draws.size());
  tod_mask_ok_ = false;
  cloud_here_ = 0.0f;
  vol_shadow_ok_ = false;
  if (!TodOn() || !SunShadowsOn() || !sun_env_.image) return;
  // Its own frame: the world map's axes, but only ~35 m each way, centred
  // a little ahead of the camera, 2048 texels (a couple of cm each: limbs
  // and rails cast).
  const uint32_t n = 2048;
  const double Rm = 35.0;
  if (!tod_movers_map_.image || tod_movers_map_.width != n) {
    if (tod_movers_map_.image) Retire(tod_movers_map_);
    tod_movers_map_ = gpu_.CreateImage(n, n, VK_FORMAT_D32_SFLOAT_S8_UINT, true, "tod_casters");
    ToGeneral(cb_, tod_movers_map_);
    return;  // (recorded into from the next frame)
  }
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.samples > 1) {
    return;
  }
  Image* depth = &dit->second;
  double Vinv[16], vp[16];
  if (!Invert4(inv_vp_, vp)) return;
  const float far_depth[4] = {0, 0, 0, 0};
  ClearImage(&tod_movers_map_, far_depth, 1.0f, 0, false, true, true);
  FlushPendingClears();
  double Om[16] = {};
  {
    double ahead[3] = {cam_pos_[0], cam_pos_[1], cam_pos_[2]};
    double mid[4] = {};
    const double ndc[4] = {0.0, 0.0, 0.5, 1.0};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) mid[r] += inv_vp_[r * 4 + c] * ndc[c];
    }
    if (std::fabs(mid[3]) > 1e-12) {
      const double vx = mid[0] / mid[3] - cam_pos_[0], vz = mid[2] / mid[3] - cam_pos_[2];
      const double nh = std::sqrt(vx * vx + vz * vz);
      if (nh > 1e-6) {
        ahead[0] += 0.4 * Rm * vx / nh;
        ahead[2] += 0.4 * Rm * vz / nh;
      }
    }
    const double texel = 2.0 * Rm / double(n);
    auto dot3 = [](const double* a, const double* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
    // (Whole texels: the shadows don't crawl as the camera moves.)
    const double a1 = std::round(dot3(ahead, sun_e1_) / texel) * texel;
    const double a2 = std::round(dot3(ahead, sun_e2_) / texel) * texel;
    for (int i = 0; i < 3; ++i) {
      Om[0 * 4 + i] = sun_e1_[i] / Rm;
      Om[1 * 4 + i] = sun_e2_[i] / Rm;
    }
    Om[0 * 4 + 3] = -a1 / Rm;
    Om[1 * 4 + 3] = -a2 / Rm;
    for (int i = 0; i < 4; ++i) Om[2 * 4 + i] = sun_O_[2 * 4 + i];
    Om[15] = 1.0;
  }
  std::memcpy(tod_Om_, Om, sizeof(Om));
  if (sun_light_ok_ && Invert4(sun_light_vp_, Vinv)) {
    DrawIntoSunMap(draws, &tod_movers_map_, Om, Vinv);
    EndRendering();
  }
  // The masks: the sun's (kTodShadow) and then the finished one (the
  // clouds, the tracks, contact and far shadows, in one pass), eased against
  // last frame's. Passed from one image to the next rather than copied: this
  // frame's finished mask is the next frame's last one (swapped).
  std::swap(tod_mask_, tod_mask_prev_);
  const bool prev_fresh = tod_mask_prev_ok_ && tod_mask_prev_.image && tod_mask_prev_.width == depth->width &&
                          tod_mask_prev_.height == depth->height;
  auto mask_image = [&](Image& im, const char* name) {
    if (im.image && im.width == depth->width && im.height == depth->height) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(depth->width, depth->height, VK_FORMAT_R8_UNORM, false, name);
    // Read as all four channels (whichever the lighting's fetch picks).
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = im.image;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R8_UNORM;
    vci.components = {VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R, VK_COMPONENT_SWIZZLE_R,
                      VK_COMPONENT_SWIZZLE_R};
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::Check(vk::vkCreateImageView(gpu_.device, &vci, nullptr, &im.sample_view), "time of day shadow mask view");
    ToGeneral(cb_, im);
  };
  mask_image(tod_mask_, "tod_shadow_mask");
  mask_image(tod_mask_raw_, "tod_shadow_mask_sun");
  mask_image(tod_mask_prev_, "tod_shadow_mask_prev");
  Barrier();
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // column-major
  }
  for (int r = 0; r < 3; ++r) {
    for (int c = 0; c < 4; ++c) pc[16 + r * 4 + c] = float(Om[r * 4 + c]);
  }
  // The world map's frame from the casters' (same axes, its own centre and
  // size): x_w = x_m Rm / R + (a1 - c1) / R.
  const bool world = world_detail_ > 0 && sun_map_ok_ && sun_r_ > 0.0 && tod_normal_;
  if (world) {
    const double k = Rm / sun_r_;
    pc[28] = float(-Om[0 * 4 + 3] * k + sun_O_[0 * 4 + 3]);
    pc[29] = float(-Om[1 * 4 + 3] * k + sun_O_[1 * 4 + 3]);
    pc[30] = float(k);
    pc[31] = 0.5f;
  }
  pc[31] += std::floor(std::clamp(0.85f * tod_.overcast, 0.0f, 1.0f) * 1000.0f);
  // The clouds' shadows over it (a second pass: from a copy).
  const float cover = std::clamp(tod_.clouds + (1.0f - tod_.clouds) * tod_.overcast, 0.0f, 1.0f);
  // (Under a grey sky the sun's already dimmed: its clouds' shadows fainter.)
  const float cloud_dark = cloud_shadows_ * (1.0f - 0.8f * std::clamp(tod_.overcast, 0.0f, 1.0f));
  const bool clouds = cloud_dark > 0.01f && cover > 0.02f && !Diag("nocloudshadows");
  const bool tracks = tracks_ok_ && tracks_map_.image && !Diag("notrackshadow");
  const bool contact = contact_ > 0.0f && !Diag("nocontact");
  // Far off (past the slopes' shadows baked round the camera, which reach
  // 80% of the sun maps'): at the mountain's own hour the game's painted
  // shadows, as the game has them; as the sun moves away from it, the
  // shadows found by marching toward the sun through the depth on screen
  // (ridges and peaks shading the slopes behind them), so the far slopes
  // aren't left lit all over.
  const bool painted = baked_ok_ && baked_src_ && baked_tod_ && !tod_.passive && bake_c5_[0] != 0.0f &&
                       !Diag("nopainted");
  const bool far_off = !tod_.passive && sun_r_ > 0.0 && !Diag("nofar");
  // Eased (after a step of the sun) or smoothed (always) against last frame's.
  const uint64_t since = frame_number_ - sun_turn_frame_;
  const double jump = std::hypot(cam_pos_[0] - tod_mask_prev_cam_[0], cam_pos_[1] - tod_mask_prev_cam_[1],
                                 cam_pos_[2] - tod_mask_prev_cam_[2]);
  if (sun_turned_ && since > 45) sun_turned_ = false;
  const bool steady = !sun_turned_ && !Diag("nomasksmooth");
  double prev_vp[16];
  const bool ease = (sun_turned_ || steady) && prev_fresh && jump < 5.0 && !Diag("noease") &&
                    Invert4(tod_mask_prev_inv_vp_, prev_vp);
  const bool finish = clouds || tracks || contact || far_off;
  // sun -> (finish ->) (ease ->) tod_mask_
  Image* finished = ease ? &tod_mask_raw_ : &tod_mask_;
  Image* sun_out = finish ? (ease ? &tod_mask_ : &tod_mask_raw_) : finished;
  FullscreenPass(CopyKind::kTodShadow, sun_out, depth, &tod_movers_map_, world ? &sun_env_ : nullptr, false, pc,
                 32, nullptr, world ? tod_normal_ : nullptr);
  // (For the air's light later: the same maps.)
  std::memcpy(vol_shadow_pc_, pc, sizeof(pc));
  vol_shadow_ok_ = true;
  vol_world_ok_ = world;
  vol_cloud_[0] = cover;
  vol_cloud_[1] = tod_.cloud_clock > 0.0f ? tod_.cloud_clock : tod_.seconds;
  vol_cloud_[2] = 600.0f;
  vol_cloud_[3] = clouds ? std::min(1.0f, 0.4f + cloud_dark) : 0.0f;
  Barrier();
  if (finish) {
    // Its settings, a row of texels (more than the push constants hold).
    float f[20 * 4] = {};
    for (int c = 0; c < 4; ++c) {
      for (int r = 0; r < 4; ++r) f[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // 0-3: ndc -> world
    }
    for (int c = 0; c < 4; ++c) {
      f[16 + c] = float(vp[0 * 4 + c]);  // 4-6: world -> clip x, y, w
      f[20 + c] = float(vp[1 * 4 + c]);
      f[24 + c] = float(vp[3 * 4 + c]);
    }
    double fd[3] = {sun_O_[8], sun_O_[9], sun_O_[10]};
    const double fl = std::sqrt(fd[0] * fd[0] + fd[1] * fd[1] + fd[2] * fd[2]);
    for (int i = 0; i < 3; ++i) f[28 + i] = fl > 1e-12 ? float(-fd[i] / fl) : (i == 1 ? 1.0f : 0.0f);
    f[31] = contact ? contact_ : 0.0f;  // 7: toward the sun, contact shadows
    if (clouds) {
      f[32] = tod_.cloud_clock > 0.0f ? tod_.cloud_clock : tod_.seconds;
      f[33] = 600.0f;
      // The layer round about once (512 texels over ~600 m of it: a metre or
      // so each), for this and the volumetric light.
      {
        if (!cloud_map_.image) {
          cloud_map_ = gpu_.CreateImage(512, 512, VK_FORMAT_R16_SFLOAT, false, "cloud_map");
          ToGeneral(cb_, cloud_map_);
        }
        const float H = f[33], ly = std::max(f[29], 0.08f);
        const float px = (float(cam_pos_[0]) + f[28] * H / ly) / H * 0.9f + f[32] * 0.006f;
        const float pz = (float(cam_pos_[2]) + f[30] * H / ly) / H * 0.9f + f[32] * 0.0025f;
        const float span = 600.0f / H * 0.9f;
        float mp[4] = {px - span * 0.5f, pz - span * 0.5f, span, 0.0f};
        FullscreenPass(CopyKind::kCloudMap, &cloud_map_, nullptr, nullptr, nullptr, false, mp, 4);
        Barrier();
        cloud_map_at_[0] = mp[0];
        cloud_map_at_[1] = mp[1];
        cloud_map_at_[2] = 1.0f / span;
        f[36] = mp[0];
        f[37] = mp[1];
        f[38] = 1.0f / span;
      }
      // (At the camera, for the riders.)
      {
        const float H = f[33], ly = std::max(f[29], 0.08f);
        const float qx = float(cam_pos_[0]) + f[28] * H / ly, qz = float(cam_pos_[2]) + f[30] * H / ly;
        const float n = CloudFbm(qx / H * 0.9f + f[32] * 0.006f, qz / H * 0.9f + f[32] * 0.0025f);
        const float lo = 1.0f - cover - 0.03f, hi = 1.0f - cover + 0.1f;
        const float t = std::clamp((n - lo) / (hi - lo), 0.0f, 1.0f);
        cloud_here_ = cloud_dark * t * t * (3.0f - 2.0f * t);
      }
      // (8: the clouds' clock, height, darkness, cover; 9: the map's corner, 1 / span.)
      f[34] = cloud_dark;
      f[35] = cover;
    }
    if (tracks) {
      const int64_t N = kTracksN;
      f[40] = float(double(tracks_cx_ - N / 2) * kTracksTexel);  // 10: the map's texel 0, texels a metre, on
      f[41] = float(double(tracks_cz_ - N / 2) * kTracksTexel);
      f[42] = float(1.0 / kTracksTexel);
      f[43] = 1.0f;
      f[44] = 0.06f * tracks_;  // 11: groove depth, heap height
      f[45] = 0.15f * tracks_;
    }
    if (far_off) {
      for (int i = 0; i < 3; ++i) f[48 + i] = float(cam_pos_[i]);  // 12: camera, from (m)
      f[51] = float(0.8 * sun_r_);
      f[52] = float(0.2 * sun_r_);  // 13: over (m), how far the sun's from the mountain's, painted, jitter
      f[53] = painted ? float(std::clamp(tod_state_.away, 0.0, 1.0)) : 1.0f;
      f[54] = painted ? 1.0f : 0.0f;
      f[55] = float(frame_number_ % 64);
      for (int i = 0; i < 4; ++i) f[56 + i] = bake_c5_[i];  // 14: the painted map's world x, z -> u, v
    }
    // (16, 17: the world light's cascades, 18: their strength here, the
    // frame; for builtin_gi_sun.)
    const bool vox_sun = far_off && GiReady() && !Diag("nogisun");
    if (vox_sun) {
      for (int k = 0; k < 2; ++k) {
        const GiCascade& g = gi_c_[k];
        for (int i = 0; i < 3; ++i) f[(16 + k) * 4 + i] = float(double(g.org[i]) * g.voxel);
        f[(16 + k) * 4 + 3] = float(g.voxel);
      }
      f[18 * 4 + 0] = painted ? float(std::clamp(tod_state_.away, 0.0, 1.0)) : 1.0f;
      f[18 * 4 + 1] = Diag("gisunview") ? 1.0f : 0.0f;
      f[18 * 4 + 2] = float(frame_number_ % 64);
      f[18 * 4 + 3] = Diag("gisunall") ? 1.0f : 0.0f;  // (everywhere it reaches, not only far off)
    }
    if (!tod_params_.image || tod_params_.width != 20) {
      if (tod_params_.image) Retire(tod_params_);
      tod_params_ = gpu_.CreateImage(20, 1, VK_FORMAT_R32G32B32A32_SFLOAT, false, "tod_mask_settings");
      ToGeneral(cb_, tod_params_);
    }
    Buffer staging = gpu_.CreateBuffer(sizeof(f), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    std::memcpy(staging.mapped, f, sizeof(f));
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {20, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, tod_params_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    staging_.push_back(staging);
    Barrier();
    // The sun's shadows far off from the world light's voxels (what's off
    // screen too), on the mask before the rest.
    Image* masked = sun_out;
    if (vox_sun) {
      if (!gi_sun_.image || gi_sun_.width != sun_out->width || gi_sun_.height != sun_out->height ||
          gi_sun_.format != sun_out->format) {
        if (gi_sun_.image) Retire(gi_sun_);
        gi_sun_ = gpu_.CreateImage(sun_out->width, sun_out->height, sun_out->format, false, "world_light_sun");
        ToGeneral(cb_, gi_sun_);
      }
      MarkPart("lighting: sun mask");
      float gp[8] = {0, 0, float(kGiN), 0, 0, 0, 0, 0};
      FullscreenPass(CopyKind::kGiSun, &gi_sun_, sun_out, depth, nullptr, false, gp, 8, nullptr,
                     const_cast<Image*>(&gi_c_[0].occ[gi_c_[0].cur]),
                     const_cast<Image*>(&gi_c_[1].occ[gi_c_[1].cur]), &tod_params_);
      Barrier();
      masked = &gi_sun_;
      MarkPart("world light: sun");
    }
    FullscreenPass(CopyKind::kMaskFinish, finished, depth, masked, clouds ? &cloud_map_ : nullptr, false, nullptr,
                   0, nullptr, tracks ? &tracks_map_ : nullptr, &tod_params_, painted ? baked_src_ : nullptr);
    Barrier();
  }
  if (ease) {
    float ep[20] = {};
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        double v = 0.0;
        for (int k = 0; k < 4; ++k) v += prev_vp[r * 4 + k] * inv_vp_[k * 4 + c];
        ep[c * 4 + r] = float(v);
      }
    }
    ep[16] = steady ? 0.4f : since < 3 ? 0.0f : 0.12f;
    ep[18] = steady ? 1.0f : 0.0f;  // (the neighbourhood clamp)
    if (Diag("sun") && !steady) std::fprintf(stderr, "sun: shadows eased (%llu frames on)\n", (unsigned long long)since);
    FullscreenPass(CopyKind::kMaskEase, &tod_mask_, &tod_mask_prev_, depth, finished, true, ep, 20);
    Barrier();
  }
  // (Kept for the next frame: tod_mask_ itself, swapped in as the last.)
  tod_mask_prev_ok_ = true;
  std::memcpy(tod_mask_prev_inv_vp_, inv_vp_, sizeof(tod_mask_prev_inv_vp_));
  for (int i = 0; i < 3; ++i) tod_mask_prev_cam_[i] = cam_pos_[i];
  // The lighting's lookup u = c6.p / c9.p, v = c7.p / c9.p: the screen's.
  for (int i = 0; i < 4; ++i) {
    tod_mask_rows_[0 + i] = 0.5 * vp[0 * 4 + i] + 0.5 * vp[3 * 4 + i];
    tod_mask_rows_[4 + i] = -0.5 * vp[1 * 4 + i] + 0.5 * vp[3 * 4 + i];
    tod_mask_rows_[8 + i] = vp[3 * 4 + i];
  }
  tod_mask_ok_ = true;
  dyn_ = DynState{};
}

void Renderer::Impl::ResolveInstances() {
  std::vector<PendingInstance> pend;
  pend.swap(pending_instances_);
  if (!collect_instances_) {
    instances_.clear();
    return;
  }
  instances_.clear();
  size_t affine = 0;
  // (The pick's queries: each one's object, none for the world itself.)
  if (cur_slot_ && cur_slot_->pick_count) cur_slot_->pick_objs.assign(cur_slot_->pick_count, Renderer::Instance{});
  for (const PendingInstance& p : pend) {
    const double K[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -p.zs, 1.0 - p.zo, 0, 0, 0, 1};
    double KM[16], W[16];
    Mul4(K, p.m, KM);
    Mul4(inv_vp_, KM, W);  // (this frame's, the game's camera: at the lighting pass before the freecam)
    // Affine (an object placed in the world): the bottom row 0 0 0 w; not
    // the view's own (the world drawn as it is: the placement is elsewhere).
    Renderer::Instance in;
    in.key = p.key;
    const double w = W[15];
    bool placed = std::fabs(w) > 1e-9 &&
                  std::fabs(W[12]) + std::fabs(W[13]) + std::fabs(W[14]) <= 1e-4 * std::fabs(w);
    if (placed) {
      double id = 0.0;
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) id += std::fabs(W[r * 4 + c] / w - (r == c ? 1.0 : 0.0));
      }
      if (id < 1e-2) placed = false;  // the identity: world space
    }
    if (placed) {
      for (int r = 0; r < 3; ++r) {
        for (int c = 0; c < 4; ++c) in.m[r * 4 + c] = float(W[r * 4 + c] / w);
      }
    } else if (p.has_world) {
      for (int i = 0; i < 12; ++i) in.m[i] = float(p.world[i]);
    } else {
      // (For the pick: why it has no copies.)
      if (p.query >= 0 && cur_slot_ && size_t(p.query) < cur_slot_->pick_objs.size()) {
        Renderer::Instance& o = cur_slot_->pick_objs[size_t(p.query)];
        o.why = std::fabs(w) > 1e-9 ? 1 : 2;
        if (o.why == 1) {
          bool ident = true;
          for (int r = 0; r < 3 && ident; ++r) {
            for (int c = 0; c < 4; ++c) ident = ident && std::fabs(W[r * 4 + c] / w - (r == c ? 1.0 : 0.0)) < 1e-2;
          }
          if (!ident) o.why = 2;
        }
        if (o.why == 2) {
          char buf[512];
          std::snprintf(buf, sizeof(buf),
                        "vs %016llX key %016llX c0 %.3g %.3g %.3g %.3g c1 %.3g %.3g %.3g %.3g c2 %.3g %.3g %.3g "
                        "%.3g c3 %.3g %.3g %.3g %.3g",
                        (unsigned long long)p.vs, (unsigned long long)p.key, p.m[0], p.m[1], p.m[2], p.m[3],
                        p.m[4], p.m[5], p.m[6], p.m[7], p.m[8], p.m[9], p.m[10], p.m[11], p.m[12], p.m[13],
                        p.m[14], p.m[15]);
          if (cur_slot_->pick_why.size() < cur_slot_->pick_objs.size()) cur_slot_->pick_why.resize(cur_slot_->pick_objs.size());
          cur_slot_->pick_why[size_t(p.query)] = buf;
        }
      }
      continue;
    }
    if (p.query >= 0 && cur_slot_ && size_t(p.query) < cur_slot_->pick_objs.size()) {
      cur_slot_->pick_objs[size_t(p.query)] = in;
    }
    // (Each object once: some are drawn in several parts.)
    bool dup = false;
    for (auto it = instances_.rbegin(); it != instances_.rend() && it - instances_.rbegin() < 8; ++it) {
      if (it->key == in.key && std::memcmp(it->m, in.m, sizeof(in.m)) == 0) dup = true;
    }
    ++affine;
    if (!dup) instances_.push_back(in);
  }
  if (Diag("instances")) std::fprintf(stderr, "instances: %zu drawn, %zu placed, %zu kept\n", pend.size(), affine, instances_.size());
}

// The screen's middle: its depth copied to this frame's read-back slot (with
// the view it goes with), read 4 frames later (CenterRead).
void Renderer::Impl::CenterProbe(Image* depth) {
  // 9 x 9 texels round the middle (its depth, and the surface's tilt there).
  if (!center_rb_.buffer) center_rb_ = gpu_.CreateBuffer(4 * 512, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true);
  if (depth->width < 16 || depth->height < 16) return;
  EndRendering();
  Barrier();
  const int slot = int(frame_number_ % 4);
  VkBufferImageCopy region{};
  region.bufferOffset = VkDeviceSize(slot) * 512;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageOffset = {int32_t(depth->width / 2) - 4, int32_t(depth->height / 2) - 4, 0};
  region.imageExtent = {9, 9, 1};
  center_size_[slot][0] = depth->width;
  center_size_[slot][1] = depth->height;
  vk::vkCmdCopyImageToBuffer(cb_, depth->image, VK_IMAGE_LAYOUT_GENERAL, center_rb_.buffer, 1, &region);
  Barrier();
  center_frame_[slot] = frame_number_;
  std::memcpy(center_inv_[slot], inv_vp_, sizeof(inv_vp_));
}

void Renderer::Impl::CenterRead() {
  const int slot = int(frame_number_ % 4);
  if (!center_rb_.buffer || !center_frame_[slot] || frame_number_ < center_frame_[slot] + 4) return;
  float blk[81];
  std::memcpy(blk, center_rb_.mapped + slot * 512, sizeof(blk));
  center_frame_[slot] = 0;
  const double W = center_size_[slot][0], H = center_size_[slot][1];
  // The world point at texel (dx, dy) from the middle (false: nothing there).
  auto at = [&](int dx, int dy, double out[3]) {
    const float d = blk[(dy + 4) * 9 + (dx + 4)];
    if (!(d > 0.0f) || !std::isfinite(d)) return false;
    const double px = std::floor(W / 2) + dx + 0.5, py = std::floor(H / 2) + dy + 0.5;
    const double n[4] = {px / W * 2.0 - 1.0, 1.0 - py / H * 2.0, 1.0 - double(d), 1.0};
    double w[4] = {};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) w[r] += center_inv_[slot][r * 4 + k] * n[k];
    }
    if (std::fabs(w[3]) < 1e-12) return false;
    for (int i = 0; i < 3; ++i) out[i] = w[i] / w[3];
    return true;
  };
  double c[3];
  if (!at(0, 0, c)) {
    center_ok_ = false;
    return;
  }
  for (int i = 0; i < 3; ++i) center_pt_[i] = float(c[i]);
  center_ok_ = true;
  // The surface's normal: across and down, each side the nearer one (an edge
  // is a different surface), facing the camera.
  double e[4][3];
  const bool ok[4] = {at(4, 0, e[0]), at(-4, 0, e[1]), at(0, 4, e[2]), at(0, -4, e[3])};
  auto dist2 = [&](const double* p) {
    return (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]);
  };
  double dx[3], dy[3];
  const int ix = ok[0] && (!ok[1] || dist2(e[0]) < dist2(e[1])) ? 0 : (ok[1] ? 1 : -1);
  const int iy = ok[2] && (!ok[3] || dist2(e[2]) < dist2(e[3])) ? 2 : (ok[3] ? 3 : -1);
  center_normal_ok_ = false;
  if (ix < 0 || iy < 0) return;
  for (int i = 0; i < 3; ++i) {
    dx[i] = (e[ix][i] - c[i]) * (ix == 0 ? 1.0 : -1.0);
    dy[i] = (e[iy][i] - c[i]) * (iy == 2 ? 1.0 : -1.0);
  }
  double n[3] = {dy[1] * dx[2] - dy[2] * dx[1], dy[2] * dx[0] - dy[0] * dx[2], dy[0] * dx[1] - dy[1] * dx[0]};
  const double nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
  if (!(nl > 1e-9)) return;
  // Toward the camera (its position: the view's at w = 0... the middle's
  // ray backwards).
  double near_pt[3];
  {
    const double nn[4] = {0.0, 0.0, 1.0, 1.0};
    double w[4] = {};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) w[r] += center_inv_[slot][r * 4 + k] * nn[k];
    }
    for (int i = 0; i < 3; ++i) near_pt[i] = std::fabs(w[3]) > 1e-12 ? w[i] / w[3] : c[i];
  }
  const double to_cam = (near_pt[0] - c[0]) * n[0] + (near_pt[1] - c[1]) * n[1] + (near_pt[2] - c[2]) * n[2];
  for (int i = 0; i < 3; ++i) center_normal_[i] = float(n[i] / nl * (to_cam < 0 ? -1.0 : 1.0));
  center_normal_ok_ = true;
}

// Night lights, after the scene (builtin_night_lights): on as it gets dark.
std::vector<std::pair<float, size_t>> Renderer::Impl::VisibleLights(bool afar) const {
  std::vector<std::pair<float, size_t>> out;
  // The view's side planes and its front (Gribb / Hartmann, from world ->
  // clip), each normalised: a sphere wholly outside one is out of view.
  double vp[16];
  double planes[5][4];
  const bool have = Invert4(inv_vp_, vp);
  if (have) {
    const int rows[5][2] = {{0, 1}, {0, -1}, {1, 1}, {1, -1}, {-1, 0}};
    for (int k = 0; k < 5; ++k) {
      for (int j = 0; j < 4; ++j) {
        const double w = vp[12 + j];
        planes[k][j] = rows[k][0] < 0 ? w : w + rows[k][1] * vp[rows[k][0] * 4 + j];
      }
      const double n = std::sqrt(planes[k][0] * planes[k][0] + planes[k][1] * planes[k][1] +
                                 planes[k][2] * planes[k][2]);
      if (n > 1e-12) {
        for (double& v : planes[k]) v /= n;
      }
    }
  }
  for (size_t i = 0; i < lights_.size(); ++i) {
    const auto& l = lights_[i];
    const float* p = l.pos;
    const float dx = p[0] - cam_pos_[0], dy = p[1] - cam_pos_[1], dz = p[2] - cam_pos_[2];
    const float d2 = dx * dx + dy * dy + dz * dz;
    if (afar ? (d2 < 150.0f * 150.0f || d2 >= 2500.0f * 2500.0f) : d2 >= 400.0f * 400.0f) continue;
    if (afar && l.preview) continue;
    if (have && afar) {
      bool in = true;
      for (int k = 0; k < 5 && in; ++k) {
        in = planes[k][0] * p[0] + planes[k][1] * p[1] + planes[k][2] * p[2] + planes[k][3] > -2.0;
      }
      if (!in) continue;
    } else if (have) {
      // (Its light's reach; its glow and fitting a few metres round it,
      // more far off; a lamp post down to the ground.)
      const double reach = std::max<double>({l.radius, 4.0 + 0.03 * std::sqrt(double(d2)),
                                             std::sqrt(double(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] +
                                                              l.dir[2] * l.dir[2])) + 1.0});
      bool in = true;
      for (int k = 0; k < 5 && in; ++k) {
        in = planes[k][0] * p[0] + planes[k][1] * p[1] + planes[k][2] * p[2] + planes[k][3] > -reach;
      }
      if (!in) continue;
    }
    out.push_back({d2, i});
  }
  std::sort(out.begin(), out.end());
  if (out.size() > (afar ? 256u : 128u)) out.resize(afar ? 256 : 128);
  return out;
}

Image* Renderer::Impl::NightLights(Image* scene, Image* depth) {
  if (lights_done_) return lights_result_;
  lights_done_ = true;
  lights_result_ = nullptr;
  // (By day too: the fittings show; their light only after dark.)
  const double dark = LightsDark();
  if (lights_.empty() || !TodOn() || !tod_normal_ ||
      depth->width != scene->width || depth->height != scene->height || scene->samples > 1) {
    return nullptr;
  }
  // Those that can show (in view), the nearest 128.
  const std::vector<std::pair<float, size_t>> nearby = VisibleLights();
  // Afar, after dark: each a point of light.
  std::vector<std::pair<float, size_t>> distant;
  if (dark > 0.001) distant = VisibleLights(true);
  if (Diag("lightcull")) {
    std::fprintf(stderr, "night lights: %zu placed, %zu in view, %zu afar\n", lights_.size(), nearby.size(),
                 distant.size());
  }
  if (nearby.empty() && distant.empty()) return nullptr;
  double vp[16];
  if (!Invert4(inv_vp_, vp)) return nullptr;
  // The screen in tiles, each with the lights that can reach its pixels
  // (nearest first): rows 8.. of the lights image, 9 a tile (its count and
  // 3 entries, then 4 a texel; 35 at most). An entry: a light (0..127) or a
  // point afar (256 + n).
  constexpr int kTile = 32, kTileRows = 9, kPerTile = 35;
  const int W = int(scene->width), H = int(scene->height);
  const int tiles_x = (W + kTile - 1) / kTile, tiles_y = (H + kTile - 1) / kTile;
  const uint32_t img_w = uint32_t(std::max(256, tiles_x));
  const uint32_t img_h = uint32_t(8 + tiles_y * kTileRows);
  std::vector<std::vector<uint16_t>> tiles(size_t(tiles_x) * tiles_y);
  // World -> pixels (x, y), with w (false: behind the camera).
  auto project = [&](double x, double y, double z, double* px, double* py, double* ndz) {
    const double c[4] = {vp[0] * x + vp[1] * y + vp[2] * z + vp[3], vp[4] * x + vp[5] * y + vp[6] * z + vp[7],
                         vp[8] * x + vp[9] * y + vp[10] * z + vp[11], vp[12] * x + vp[13] * y + vp[14] * z + vp[15]};
    if (c[3] <= 1e-6) return false;
    *px = (c[0] / c[3] * 0.5 + 0.5) * W;
    *py = (0.5 - c[1] / c[3] * 0.5) * H;
    if (ndz) *ndz = c[2] / c[3];
    return true;
  };
  auto bin = [&](double x0, double y0, double x1, double y1, uint16_t entry) {
    const int tx0 = std::clamp(int(std::floor(x0 / kTile)), 0, tiles_x - 1);
    const int tx1 = std::clamp(int(std::floor(x1 / kTile)), 0, tiles_x - 1);
    const int ty0 = std::clamp(int(std::floor(y0 / kTile)), 0, tiles_y - 1);
    const int ty1 = std::clamp(int(std::floor(y1 / kTile)), 0, tiles_y - 1);
    if (x1 < 0 || y1 < 0 || x0 >= W || y0 >= H) return;
    for (int ty = ty0; ty <= ty1; ++ty) {
      for (int tx = tx0; tx <= tx1; ++tx) {
        auto& t = tiles[size_t(ty) * tiles_x + tx];
        if (t.size() < kPerTile) t.push_back(entry);
      }
    }
  };
  for (size_t j = 0; j < nearby.size(); ++j) {
    const Renderer::NightLight& l = lights_[nearby[j].second];
    const double dist = std::sqrt(double(nearby[j].first));
    // What it reaches: its light; its glow, fitting (a lamp post's pole).
    const double arm = std::sqrt(double(l.dir[0] * l.dir[0] + l.dir[1] * l.dir[1] + l.dir[2] * l.dir[2]));
    const double R = std::max({double(std::max(l.radius, 0.5f)), 4.0 * (0.18 + 0.004 * dist), arm + 1.0}) +
                     0.5 * std::max(l.size[0], l.size[1]) * (l.kind == 2 ? 1.0 : 0.0);
    double x0 = 1e30, y0 = 1e30, x1 = -1e30, y1 = -1e30;
    bool whole = dist < R * 1.2;
    for (int c = 0; c < 8 && !whole; ++c) {
      double px, py;
      if (!project(l.pos[0] + (c & 1 ? R : -R), l.pos[1] + (c & 2 ? R : -R), l.pos[2] + (c & 4 ? R : -R), &px,
                   &py, nullptr)) {
        whole = true;
        break;
      }
      x0 = std::min(x0, px);
      x1 = std::max(x1, px);
      y0 = std::min(y0, py);
      y1 = std::max(y1, py);
    }
    if (whole) {
      x0 = y0 = 0;
      x1 = W;
      y1 = H;
    }
    bin(x0, y0, x1, y1, uint16_t(j));
  }
  struct Sprite {
    float x, y, d, r, rgb[3];
  };
  std::vector<Sprite> sprites;
  const double e = eye_strength_ > 0.0f ? exposure_ : 1.0;
  for (const auto& [d2, i] : distant) {
    const Renderer::NightLight& l = lights_[i];
    const double dist = std::sqrt(double(d2));
    double px, py, ndz;
    if (!project(l.pos[0], l.pos[1], l.pos[2], &px, &py, &ndz)) continue;
    // Fades in as the glow up close fades out, and away again at the far end.
    const double fade = dark * tod::SmoothStep(150.0, 400.0, dist) * (1.0 - tod::SmoothStep(1800.0, 2500.0, dist));
    if (fade < 0.002) continue;
    // A few pixels across (more for a bright one), a little smaller far off.
    const double r = H / 720.0 * 2.2 * std::sqrt(std::max(double(l.intensity), 0.2)) *
                     (1.25 - 0.45 * tod::SmoothStep(150.0, 2500.0, dist));
    Sprite sp{float(px / W * 2.0 - 1.0), float(1.0 - py / H * 2.0), float(1.0 - ndz), float(r), {}};
    const double k = l.intensity * e * fade * (l.kind == 1 ? 1.4 : 1.0);
    for (int c = 0; c < 3; ++c) sp.rgb[c] = float(l.color[c] * k);
    const uint16_t entry = uint16_t(256 + sprites.size());
    sprites.push_back(sp);
    bin(px - r * 4.0, py - r * 4.0, px + r * 4.0, py + r * 4.0, entry);
  }
  if (nearby.empty() && sprites.empty()) return nullptr;
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  ensure(lights_img_, img_w, img_h, VK_FORMAT_R32G32B32A32_SFLOAT, "night_lights");
  ensure(lights_out_, scene->width, scene->height, scene->format, "night_lit");
  lights_out_.scaled = scene->scaled;
  lights_out_.scale = scene->scale;
  {
    const size_t bytes = size_t(img_w) * img_h * 16;
    Buffer staging = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    float* f = reinterpret_cast<float*>(staging.mapped);
    std::memset(f, 0, bytes);
    const size_t row = size_t(img_w) * 4;  // floats a row
    {
      // Row 3: world (from the camera) -> clip, its rows (the shadows' steps
      // onto the screen; worked out here in double, from the camera so
      // nothing's lost far from the origin).
      {
        float* r = f + 3 * row;
        for (int i = 0; i < 4; ++i) {
          r[i * 4 + 0] = float(vp[i * 4 + 0]);
          r[i * 4 + 1] = float(vp[i * 4 + 1]);
          r[i * 4 + 2] = float(vp[i * 4 + 2]);
          r[i * 4 + 3] = float(vp[i * 4 + 0] * cam_pos_[0] + vp[i * 4 + 1] * cam_pos_[1] +
                               vp[i * 4 + 2] * cam_pos_[2] + vp[i * 4 + 3]);
        }
        r[16] = float(dark);  // (texel 4: how dark, for the snow's flakes)
      }
    }
    for (size_t j = 0; j < nearby.size(); ++j) {
      const Renderer::NightLight& l = lights_[nearby[j].second];
      float* a = f + j * 4;
      float* b = f + row + j * 4;
      a[0] = l.pos[0];
      a[1] = l.pos[1];
      a[2] = l.pos[2];
      a[3] = std::max(l.radius, 0.5f);
      b[0] = l.color[0];
      b[1] = l.color[1];
      b[2] = l.color[2];
      b[3] = float(l.intensity * e);
      float* c = f + 2 * row + j * 4;
      c[0] = l.dir[0];
      c[1] = l.dir[1];
      c[2] = l.dir[2];
      c[3] = float((l.kind & 7) | (l.preview ? 8 : 0) | (l.selected ? 16 : 0));
      // Row 6: a panel's size, its spread.
      float* x = f + 6 * row + j * 4;
      x[0] = l.size[0];
      x[1] = l.size[1];
      x[2] = l.spread;
    }
    // Rows 4, 5: the points afar (ndc x, y, depth, radius in pixels; color).
    for (size_t n = 0; n < sprites.size(); ++n) {
      float* a = f + 4 * row + n * 4;
      float* b = f + 5 * row + n * 4;
      a[0] = sprites[n].x;
      a[1] = sprites[n].y;
      a[2] = sprites[n].d;
      a[3] = sprites[n].r;
      b[0] = sprites[n].rgb[0];
      b[1] = sprites[n].rgb[1];
      b[2] = sprites[n].rgb[2];
    }
    // The tiles.
    for (int ty = 0; ty < tiles_y; ++ty) {
      for (int tx = 0; tx < tiles_x; ++tx) {
        const auto& t = tiles[size_t(ty) * tiles_x + tx];
        float* base = f + size_t(8 + ty * kTileRows) * row + size_t(tx) * 4;
        base[0] = float(t.size());
        for (size_t k = 0; k < t.size(); ++k) {
          const size_t slot = k + 1;
          base[(slot / 4) * row + slot % 4] = float(t[k]);
        }
      }
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {img_w, img_h, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, lights_img_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
    staging_.push_back(staging);
  }
  Barrier();
  // The world's depth as the lighting had it (riders told apart), when kept.
  const bool have_world = lights_wdepth_ok_ && tod_albedo_ && lights_wdepth_.width == depth->width &&
                          lights_wdepth_.height == depth->height;
  float pc[32] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(inv_vp_[r * 4 + c]);  // column-major
  }
  // Their light in the air (volumetric).
  pc[28] = VolumeOn() && !Diag("novolumelights") ? VolumeDensity() * 3.0f : 0.0f;
  pc[29] = float(light_shadow_lamps_);
  {
    // The light on the fittings: the sky's and the sun's (or moon's), as the
    // hour has them; the exposure.
    auto lum = [](const tod::Vec3& v) { return 0.2126 * v.x + 0.7152 * v.y + 0.0722 * v.z; };
    const double amb = std::clamp(0.6 * lum(lm_ambient_scale_) + 0.35 * lum(lm_light_scale_), 0.03, 1.2);
    pc[24] = tod_.seconds;
    pc[25] = float(amb);
  }
  pc[16] = cam_pos_[0];
  pc[17] = cam_pos_[1];
  pc[18] = cam_pos_[2];
  pc[19] = float(kTile);
  pc[20] = 1.0f / float(scene->width);
  pc[21] = 1.0f / float(scene->height);
  pc[22] = float(dark);
  pc[23] = Diag("lightsdbg") ? 2.0f : have_world ? 1.0f : 0.0f;
  // Shadows: steps along each ray toward a light (0: none).
  pc[27] = Diag("lightshadowdbg") ? 1.0f : 0.0f;
  pc[26] = light_shadows_ == 0 || Diag("nolightshadows") ? 0.0f : light_shadows_ == 1 ? 10.0f : 20.0f;
  FullscreenPass(CopyKind::kTodLights, &lights_out_, scene, have_world ? &lights_wdepth_ : depth,
                 &lights_img_, false, pc, 32, nullptr, tod_albedo_ ? tod_albedo_ : &dummy_,
                 tod_normal_, depth);
  Barrier();
  dyn_ = DynState{};
  lights_result_ = &lights_out_;
  return lights_result_;
}

// Time of day, after the scene: the sky drawn for the hour (builtin_tod_sky).
Image* Renderer::Impl::TodSky(Image* scene) {
  if (tod_sky_done_) return tod_sky_result_;
  tod_sky_done_ = true;
  tod_sky_result_ = nullptr;
  if (!TodOn() || tod_.passive) return nullptr;  // (effects only: the game's own sky)
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != scene->width || dit->second.height != scene->height ||
      scene->samples > 1) {
    return nullptr;
  }
  Image* depth = &dit->second;
  const uint32_t w = scene->width, h = scene->height;
  EndRendering();
  auto ensure = [&](Image& im, uint32_t iw, uint32_t ih, VkFormat f, const char* name) {
    if (im.image && im.width == iw && im.height == ih && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(iw, ih, f, false, name);
    ToGeneral(cb_, im);
  };
  ensure(tod_sky_img_, w, h, scene->format, "tod_sky");
  tod_sky_img_.scaled = scene->scaled;
  tod_sky_img_.scale = scene->scale;
  ensure(tod_colors_, 4, 1, VK_FORMAT_R32G32B32A32_SFLOAT, "tod_sky_colors");
  const tod::State& t = tod_state_;
  {
    Buffer staging = gpu_.CreateBuffer(64, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    float* f = reinterpret_cast<float*>(staging.mapped);
    const tod::Vec3* cols[4] = {&t.zenith, &t.horizon, &t.mountain_zenith, &t.mountain_horizon};
    for (int i = 0; i < 4; ++i) {
      f[i * 4] = float(cols[i]->x);
      f[i * 4 + 1] = float(cols[i]->y);
      f[i * 4 + 2] = float(cols[i]->z);
      f[i * 4 + 3] = 1.0f;
    }
    // (Texel 0's alpha: the ice halo's strength: thin high cloud (some
    // cover, not a grey sky), the sun up but not high.)
    {
      const double cover = tod_.clouds, ov = std::clamp(double(tod_.overcast), 0.0, 1.0);
      const double halo = tod::SmoothStep(0.08, 0.22, cover) * (1.0 - tod::SmoothStep(0.45, 0.7, cover)) *
                          (1.0 - tod::SmoothStep(0.15, 0.45, ov)) *
                          tod::SmoothStep(2.0 * tod::kDeg, 8.0 * tod::kDeg, t.sun_elevation) *
                          (1.0 - tod::SmoothStep(45.0 * tod::kDeg, 60.0 * tod::kDeg, t.sun_elevation)) *
                          (1.0 - t.night);
      f[3] = Diag("nohalo") ? 0.0f : Diag("halo") ? 1.0f : float(halo) * halo_;
      // (Texel 1's alpha: the sun pillar's: the sun at the horizon, the air
      // still, the sky clear to thinly clouded, no more than light snow.)
      const double wind = std::hypot(tod_.wind[0], tod_.wind[1]);
      const double pillar = tod::SmoothStep(-4.0 * tod::kDeg, -0.5 * tod::kDeg, t.sun_elevation) *
                            (1.0 - tod::SmoothStep(4.0 * tod::kDeg, 9.0 * tod::kDeg, t.sun_elevation)) *
                            (1.0 - tod::SmoothStep(0.35, 0.65, cover)) * (1.0 - tod::SmoothStep(0.15, 0.45, ov)) *
                            (1.0 - tod::SmoothStep(4.0, 8.0, wind)) *
                            (1.0 - tod::SmoothStep(0.3, 0.6, double(tod_.snow)));
      f[7] = Diag("nopillar") ? 0.0f : Diag("pillar") ? 1.0f : float(pillar) * halo_;
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {4, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, tod_colors_.image, VK_IMAGE_LAYOUT_GENERAL, 1,
                               &region);
    staging_.push_back(staging);
  }
  Barrier();
  // The view rays (linear in ndc for a perspective view): through the middle
  // and their change across the screen, from the inverse view-projection.
  auto unproject = [&](double x, double y, double out[3]) {
    const double n[4] = {x, y, 0.5, 1.0};
    double v[4] = {};
    for (int r = 0; r < 4; ++r) {
      for (int k = 0; k < 4; ++k) v[r] += inv_vp_[r * 4 + k] * n[k];
    }
    for (int r = 0; r < 3; ++r) out[r] = v[r] / v[3] - cam_pos_[r];
  };
  double c[3], rx[3], uy[3];
  unproject(0, 0, c);
  unproject(1, 0, rx);
  unproject(0, 1, uy);
  // Normalized so the middle ray is unit length (the others scale alike).
  const double cl = std::sqrt(c[0] * c[0] + c[1] * c[1] + c[2] * c[2]);
  if (!(cl > 1e-9)) return nullptr;
  float pc[32] = {};
  for (int i = 0; i < 3; ++i) {
    pc[0 + i] = float(c[i] / cl);
    pc[4 + i] = float((rx[i] - c[i]) / cl);
    pc[8 + i] = float((uy[i] - c[i]) / cl);
    pc[12 + i] = float(t.sun.x * (i == 0) + t.sun.y * (i == 1) + t.sun.z * (i == 2));
    pc[16 + i] = float(t.moon.x * (i == 0) + t.moon.y * (i == 1) + t.moon.z * (i == 2));
    pc[20 + i] = sun_dir_painted_[i];
  }
  // (The clouds drift with the wind: their own clock.)
  pc[3] = tod_.cloud_clock > 0.0f ? tod_.cloud_clock : tod_.seconds;
  pc[7] = float(t.night);
  pc[11] = float(t.stars);
  pc[15] = float(t.sun_disc);
  pc[19] = float(t.moon_bright);
  pc[23] = float(t.away);
  pc[24] = float(t.sun_color.x);
  pc[25] = float(t.sun_color.y);
  pc[26] = float(t.sun_color.z);
  // (An overcast sky: clouded over.)
  pc[27] = std::clamp(tod_.clouds + (1.0f - tod_.clouds) * tod_.overcast, 0.0f, 1.0f);
  pc[28] = 1.0f / float(w);
  pc[29] = 1.0f / float(h);
  pc[31] = eye_strength_ > 0.0f ? float(exposure_) : 1.0f;
  pc[30] = float(1.0 - tod::SmoothStep(2.0 * tod::kDeg, 20.0 * tod::kDeg, t.sun_elevation)) *
           float(tod::SmoothStep(-8.0 * tod::kDeg, -1.0 * tod::kDeg, t.sun_elevation));
  FullscreenPass(CopyKind::kTodSky, &tod_sky_img_, scene, depth, &tod_colors_, false, pc, 32);
  Barrier();
  dyn_ = DynState{};
  tod_sky_result_ = &tod_sky_img_;
  return tod_sky_result_;
}

Image* Renderer::Impl::RadialScene(Image* scene) {
  if (radial_done_) return radial_img_.image ? &radial_img_ : nullptr;
  radial_done_ = true;
  const uint32_t w = scene->width, h = scene->height;
  EndRendering();
  if (!radial_img_.image || radial_img_.width != w || radial_img_.height != h ||
      radial_img_.format != scene->format) {
    if (radial_img_.image) Retire(radial_img_);
    radial_img_ = gpu_.CreateImage(w, h, scene->format, false, "speed_blur");
    ToGeneral(cb_, radial_img_);
  }
  radial_img_.scaled = scene->scaled;
  radial_img_.scale = scene->scale;
  Barrier();
  const float amount = std::clamp(speed_amount_, 0.0f, 1.0f) * speed_strength_;
  float pc[8] = {speed_center_[0], speed_center_[1],
                 0.08f * amount,  // up to 8% of the way to the center
                 float(w) / float(h),
                 // The ring: from 55% of the way to the corners (50% at full
                 // speed) to the corners; the rider, below the middle, stays
                 // clear.
                 0.55f - 0.05f * std::min(amount, 1.0f), 1.0f, 1.0f / float(w),
                 1.0f / float(h)};
  FullscreenPass(CopyKind::kRadialBlur, &radial_img_, scene, nullptr, nullptr, true, pc, 8);
  Barrier();
  dyn_ = DynState{};
  ++stats_.speed_blurred;
  return &radial_img_;
}

Image* Renderer::Impl::UpscaledScene(Image* src) {
  if (up_done_) return up_result_;
  up_done_ = true;
  if (!upscaler_) return nullptr;
  auto dit = resolved_.find(depth_base_);
  if (dit == resolved_.end() || dit->second.format != VK_FORMAT_R32_SFLOAT ||
      dit->second.width != src->width || dit->second.height != src->height || src->samples > 1) {
    return nullptr;
  }
  Image* depth = &dit->second;
  const uint32_t rw = src->width, rh = src->height;
  // The guest size from the scene's own scale (resolves are made at the
  // scene's scale_; an image derived from it, like the motion blur's, is the
  // same size).
  const float in_scale = src->scaled ? src->scale : 1.0f;
  const float gw = float(rw) / in_scale, gh = float(rh) / in_scale;
  const uint32_t ow = std::max(uint32_t(std::lround(gw * out_scale_)), 1u);
  const uint32_t oh = std::max(uint32_t(std::lround(gh * out_scale_)), 1u);
  std::string error;
  if (!up_inited_ || rw > up_in_w_ || rh > up_in_h_ || ow != up_out_w_ || oh != up_out_h_) {
    PhaseScope phase("starting the upscaler");
    if (up_inited_) {
      // A new size: a new context (none of the old one's work in flight).
      vk::vkDeviceWaitIdle(gpu_.device);
      upscaler_ = amped3::native::Upscaler::Create(
          amped3::native::UpscalerKind(options_.upscaler), &error);
      if (!upscaler_) {
        upscaler_status_ = error;
        return nullptr;
      }
    }
    if (!upscaler_->Init(gpu_.instance, gpu_.physical, gpu_.device, vk::GetInstanceProcAddr(),
                         vk::vkGetDeviceProcAddr, ow, oh, rw, rh, &error)) {
      upscaler_status_ = error;
      upscaler_.reset();
      return nullptr;
    }
    upscaler_status_ = upscaler_->description();
    up_inited_ = true;
    up_in_w_ = rw;
    up_in_h_ = rh;
    up_out_w_ = ow;
    up_out_h_ = oh;
    up_reset_ = true;
    jitter_phases_ = upscaler_->JitterPhases(rw, ow);
  }
  EndRendering();
  auto ensure = [&](Image& im, uint32_t w, uint32_t h, VkFormat f, const char* name, bool storage) {
    if (im.image && im.width == w && im.height == h && im.format == f) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(w, h, f, false, name, 1, 1, storage);
    ToGeneral(cb_, im);
  };
  ensure(mv_img_, rw, rh, VK_FORMAT_R16G16_SFLOAT, "motion_vectors", false);
  // The output in the input's format (XeSS requires it; with a different
  // one its upscaling modes write black), when it can be a storage image.
  VkFormat out_format = VK_FORMAT_R16G16B16A16_SFLOAT;
  {
    VkFormatProperties fp{};
    vk::vkGetPhysicalDeviceFormatProperties(gpu_.physical, src->format, &fp);
    if (fp.optimalTilingFeatures & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) out_format = src->format;
  }
  ensure(up_out_, ow, oh, out_format, "upscaled", true);
  up_out_.scaled = true;
  up_out_.scale = out_scale_;
  Barrier();

  // Motion vectors (none, and a reset, without last frame's camera).
  const bool have_prev = prev_valid_;
  double prev_vp[16], m[16];
  if (!have_prev || !Invert4(prev_inv_vp_, prev_vp)) {
    for (int i = 0; i < 16; ++i) m[i] = (i % 5) ? 0.0 : 1.0;
  } else {
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        double v = 0.0;
        for (int k = 0; k < 4; ++k) v += prev_vp[r * 4 + k] * inv_vp_[k * 4 + c];
        m[r * 4 + c] = v;
      }
    }
  }
  Image* vel = Velocities();
  float pc[28] = {};
  for (int c = 0; c < 4; ++c) {
    for (int r = 0; r < 4; ++r) pc[c * 4 + r] = float(m[r * 4 + c]);
  }
  for (int k = 0; k < 4; ++k) pc[16 + k] = float(inv_vp_[12 + k]);
  pc[20] = jitter_x_;
  pc[21] = jitter_y_;
  pc[22] = vel ? 1.0f : 0.0f;
  pc[24] = 1.0f / float(rw);
  pc[25] = 1.0f / float(rh);
  pc[26] = float(rw);
  pc[27] = float(rh);
  FullscreenPass(CopyKind::kMotionVectors, &mv_img_, nullptr, depth, vel, false, pc, 28);

  // The camera, for FSR: near plane and vertical field of view from the
  // inverse view-projection (depth is inverted and infinite).
  auto unproject = [&](double nx, double ny, double nz, double out[3]) {
    double hv[4];
    const double in[4] = {nx, ny, nz, 1.0};
    for (int r = 0; r < 4; ++r) {
      hv[r] = 0.0;
      for (int k = 0; k < 4; ++k) hv[r] += inv_vp_[r * 4 + k] * in[k];
    }
    for (int k = 0; k < 3; ++k) out[k] = hv[k] / hv[3];
    return 1.0 / hv[3];
  };
  double a[3], b[3];
  const double da = unproject(0.0, 0.0, 0.5, a);
  unproject(0.0, 1.0, 0.5, b);
  const double half_h = std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) +
                                  (a[2] - b[2]) * (a[2] - b[2]));
  double nearest[3];
  const double near_dist = unproject(0.0, 0.0, 0.0, nearest);  // v.z = 1 - depth(1)

  amped3::native::UpscaleInputs in;
  auto image = [](const Image& im) {
    return amped3::native::UpscaleImage{im.image, im.sample_view, im.format, im.width, im.height,
                                        im.usage};
  };
  in.color = image(*src);
  in.depth = image(*depth);
  in.motion = image(mv_img_);
  Image* reactive = DrawReactive(rw, rh, &mv_img_);
  if (reactive) in.reactive = image(*reactive);
  in.output = image(up_out_);
  in.render_width = rw;
  in.render_height = rh;
  in.jitter_x = jitter_x_;
  in.jitter_y = jitter_y_;
  in.reset = up_reset_ || !have_prev;
  in.frame_ms = frame_ms_;
  in.sharpness = up_sharpness_;
  in.camera_near = near_dist > 1e-4 && std::isfinite(near_dist) ? float(near_dist) : 0.1f;
  in.camera_far = 100000.0f;
  in.fov_y = da > 1e-6 ? float(2.0 * std::atan(half_h / da)) : 1.0f;

  const VkImageLayout ro = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, gen = VK_IMAGE_LAYOUT_GENERAL;
  Transition(*src, gen, ro);
  Transition(*depth, gen, ro);
  Transition(mv_img_, gen, ro);
  if (reactive) Transition(*reactive, gen, ro);
  // The upscaler records into the command buffer itself: everything before
  // it first.
  bool ok;
  {
    PhaseScope phase("the upscaler (recording its work)");
    if (stream_.active()) stream_.Drain();
    ok = upscaler_->Dispatch(cb_, in, &error);
  }
  Transition(*src, ro, gen);
  Transition(*depth, ro, gen);
  Transition(mv_img_, ro, gen);
  if (reactive) Transition(*reactive, ro, gen);
  Barrier();
  dyn_ = DynState{};
  if (!ok) {
    upscaler_status_ = error;
    vk::vkDeviceWaitIdle(gpu_.device);
    upscaler_.reset();
    return nullptr;
  }
  up_reset_ = false;
  if (reactive_debug_ && reactive) {
    // Diagnostics: the scene stretched (not upscaled), red where the mask
    // tells FSR to trust this frame.
    const float view_pc[2] = {1.0f / float(up_out_.width), 1.0f / float(up_out_.height)};
    FullscreenPass(CopyKind::kReactiveView, &up_out_, src, reactive, nullptr, true, view_pc, 2);
    dyn_ = DynState{};
  }
  up_result_ = &up_out_;
  return up_result_;
}

// A full-screen pass of a copy shader into `dst` (bindings 0..2 = b0..b2,
// missing ones the dummy image; binding 0 filtered when `linear0`).
void Renderer::Impl::FullscreenPass(CopyKind kind, Image* dst, Image* b0, Image* b1, Image* b2,
                                    bool linear0, const float* pc, uint32_t floats,
                                    const VkRect2D* rect, Image* b3, Image* b4, Image* b5) {
  FlushPendingClears();  // what it reads or writes may have a clear waiting
  VkPipeline p = CopyPipeline(kind, dst->format);
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &copy_set_layout_;
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &set), "fullscreen pass set");
  VkDescriptorImageInfo ii[6] = {
      {linear0 ? linear_sampler_ : point_sampler_, (b0 ? b0 : &dummy_)->sample_view,
       VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b1 ? b1 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b2 ? b2 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b3 ? b3 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b4 ? b4 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b5 ? b5 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wr.dstSet = set;
  wr.descriptorCount = 6;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wr.pImageInfo = ii;
  vk::vkUpdateDescriptorSets(gpu_.device, 1, &wr, 0, nullptr);
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = dst->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  // (A part of the image: what the shader leaves (discard) stays.)
  att.loadOp = rect ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  const VkRect2D area = rect ? *rect : VkRect2D{{0, 0}, {dst->width, dst->height}};
  ri.renderArea = area;
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vk::vkCmdBeginRendering(cb_, &ri);
  vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_layout_, 0, 1, &set, 0,
                              nullptr);
  if (floats) {
    vk::vkCmdPushConstants(cb_, copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, floats * 4, pc);
  }
  VkViewport v{0, 0, float(dst->width), float(dst->height), 0, 1};
  vk::vkCmdSetViewport(cb_, 0, 1, &v);
  VkRect2D sc = area;
  vk::vkCmdSetScissor(cb_, 0, 1, &sc);
  vk::vkCmdDraw(cb_, 3, 1, 0, 0);
  vk::vkCmdEndRendering(cb_);
  Barrier();
}

void Renderer::Impl::CopyPass(CopyKind kind, Image* src, Image* dst, uint32_t mode,
                              bool float_depth, uint32_t first, uint32_t count, uint32_t sx0,
                              uint32_t sy0, uint32_t dx0, uint32_t dy0, uint32_t w, uint32_t h) {
  {
    // What it reads or writes may have a clear waiting (not the source's
    // other tiles).
    const VkRect2D read{{int32_t(sx0), int32_t(sy0)}, {w, h}};
    FlushPendingClearsExcept(src, &read, dst);
  }
  VkPipeline p = CopyPipeline(kind, dst->format);
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &copy_set_layout_;
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &set), "copy set");
  VkDescriptorImageInfo ii[2] = {
      {point_sampler_, src->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, src->depth ? src->stencil_view : src->sample_view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet w2{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w2.dstSet = set;
  w2.descriptorCount = 2;
  w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w2.pImageInfo = ii;
  vk::vkUpdateDescriptorSets(gpu_.device, 1, &w2, 0, nullptr);
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = dst->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {dst->width, dst->height}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vk::vkCmdBeginRendering(cb_, &ri);
  vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_layout_, 0, 1, &set, 0,
                              nullptr);
  int32_t delta[8] = {int32_t(sx0) - int32_t(dx0), int32_t(sy0) - int32_t(dy0), int32_t(mode),
                     float_depth ? 1 : 0, int32_t(first), int32_t(count), 0, 0};
  vk::vkCmdPushConstants(cb_, copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, delta);
  VkViewport v{0, 0, float(dst->width), float(dst->height), 0, 1};
  vk::vkCmdSetViewport(cb_, 0, 1, &v);
  VkRect2D sc{{int32_t(dx0), int32_t(dy0)}, {w, h}};
  vk::vkCmdSetScissor(cb_, 0, 1, &sc);
  vk::vkCmdDraw(cb_, 3, 1, 0, 0);
  vk::vkCmdEndRendering(cb_);
}

void Renderer::Impl::MarkPart(const std::string& name) {
  FrameSlot* fs = cur_slot_;
  if (!fs || !fs->timed || !part_pool_ || rendering_) return;
  if (fs->part_names.size() + 1 >= kMaxParts) return;
  const uint32_t q = uint32_t(fs - slots_) * kMaxParts + uint32_t(fs->part_names.size()) + 1;
  vk::vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, part_pool_, q);
  fs->part_names.push_back(name);
}

void Renderer::Impl::DoResolve(const Resolve& r, uint32_t number) {
  // Object motion: the scene pass's draws are done once a resolve follows the
  // characters (drawn after the lighting); later resolves may clear the depth.
  if (post_light_draws_ && !vel_ready_ && (!vel_draws_.empty() || !mask_draws_.empty())) {
    DrawVelocities();
  }
  frame_tex_lookup_.clear();  // textures at the destination now show the resolve
  if (!r.has_targets || !r.has_dest) {
    Skip(number, "resolve without target registers (old capture)");
    return;
  }
  EndRendering();
  const uint32_t source = r.flags & 7;

  const uint32_t surface = r.targets[0];
  Image* src = nullptr;
  if (source == 4) {
    src = GetTarget(DepthKey(surface, r.targets[2]), true);
  } else {
    const uint32_t infos[4] = {r.targets[1], r.targets[3], r.targets[4], r.targets[5]};
    src = GetTarget(ColorKey(surface, infos[source & 3]), true);
  }
  // GPU time by part: what drew into it, and which of the frame's parts this
  // is (a surface drawn to more than once a frame shows each time apart).
  MarkPart(src->name + " #" + std::to_string(cur_slot_ ? cur_slot_->part_names.size() + 1 : 0));
  amped3::native::TextureFetch dest;
  std::memcpy(dest.words, r.dest_raw.data(), sizeof(dest.words));
  const uint32_t base = dest.base_address() & 0x1FFFFFFF;
  // At the source target's scale (the scene's or the output's).
  const float rs = src->scale;
  const uint32_t dw = std::max(Scaled(dest.width(), rs), 1u),
                 dh = std::max(Scaled(dest.height(), rs), 1u);
  VkFormat format = source == 4 ? VK_FORMAT_R32_SFLOAT : src->format;
  Image* dst = ResolveImage(resolved_, base, dw, dh, format, "", rs);
  Image* packed = nullptr;
  if (source == 4) {
    packed = ResolveImage(resolved_packed_, base, dw, dh, VK_FORMAT_R8G8B8A8_UNORM, "_raw", rs);
  } else {
    auto pit = resolved_packed_.find(base);  // overwritten by color
    if (pit != resolved_packed_.end()) {
      // Kept aside, not destroyed: games resolve depth and then color to the
      // same address every frame (a new image each frame also dropped the
      // cached texture descriptor sets: TextureSet).
      const Image old = pit->second;
      SpareResolve& spare =
          spare_resolves_[{&resolved_packed_, base, old.width, old.height, old.format}];
      if (spare.image.image) Retire(spare.image);
      spare = {old, frame_number_};
      resolved_packed_.erase(pit);
    }
  }
  resolved_object_[r.dest_object] = base;
  resolved_format_[base] = dest.format();
  resolved_info_[base] = {frame_number_, {}};

  // Rectangles.
  uint32_t sx0 = 0, sy0 = 0, sx1 = src->width, sy1 = src->height;
  if (!r.full) {
    sx0 = Scaled(r.rect[0], rs);
    sy0 = Scaled(r.rect[1], rs);
    sx1 = Scaled(r.rect[2], rs);
    sy1 = Scaled(r.rect[3], rs);
  }
  uint32_t dx0 = r.has_dest_point ? Scaled(r.dest_point[0], rs) : (r.full ? 0 : sx0);
  uint32_t dy0 = r.has_dest_point ? Scaled(r.dest_point[1], rs) : (r.full ? 0 : sy0);
  uint32_t w = std::min(sx1, src->width) - std::min(sx0, src->width);
  uint32_t h = std::min(sy1, src->height) - std::min(sy0, src->height);
  w = std::min(w, dst->width > dx0 ? dst->width - dx0 : 0);
  h = std::min(h, dst->height > dy0 ? dst->height - dy0 : 0);
  // MSAA: the samples resolved (D3DRESOLVE_FRAGMENT*, flags bits 4..6: all,
  // one of 0..3, the pairs 01 / 23, or 0123), in host samples.
  uint32_t first_sample = 0, sample_count = 1;
  if (src->samples > 1) {
    const uint32_t scale = std::max(src->sample_scale, 1u);
    const uint32_t guest_samples = src->samples / scale;
    const uint32_t sel = (r.flags >> 4) & 7;
    uint32_t first = 0, count = guest_samples;
    if (sel >= 1 && sel <= 4) {
      first = sel - 1;
      count = 1;
    } else if (sel == 5 || sel == 6) {
      first = sel == 5 ? 0 : 2;
      count = 2;
    }
    first = std::min(first, guest_samples - 1);
    count = std::min(count, guest_samples - first);
    first_sample = first * scale;
    sample_count = count * scale;
  }
  // One sample for the guest's two (see DoDraw): a resolve of guest samples
  // other than 0 copies what sample 0 held before the draws that wrote it
  // alone. When this destination already got sample 0 of the same source
  // and rectangle, with no draw to every sample since, it holds exactly that:
  // the copy is skipped (copying now would bring the sample-0 lighting into
  // the sample-1 layer).
  bool skip_copy = false;
  if (src->samples == 1 && src->guest_samples > 1) {
    const uint32_t sel = (r.flags >> 4) & 7;
    const bool sample0 = sel == 0 || sel == 1 || sel == 5 || sel == 7;
    const ResolveRecord rec{src, src->full_draws, sx0, sy0, w, h};
    auto& last = last_resolve_[dst];
    if (!sample0 && last == rec) skip_copy = true;
    if (sample0) last = rec;
  }
  if (w && h && !skip_copy) {
    // (clears waiting for the other tiles of the source may wait on)
    const VkRect2D read{{int32_t(sx0), int32_t(sy0)}, {w, h}};
    ResolveBarrier(src, read);
    if (source == 4) {
      // Depth -> R32F, and the raw words -> 8_8_8_8, through a fragment shader
      // (multisampled depth: the first resolved sample, depth isn't averaged).
      const bool float_depth = (r.targets[2] >> 16) & 1;
      CopyDepth(src, dst, 0, float_depth, first_sample, sx0, sy0, dx0, dy0, w, h);
      CopyDepth(src, packed, 1, float_depth, first_sample, sx0, sy0, dx0, dy0, w, h);
    } else if (src->samples > 1) {
      CopyPass(CopyKind::kColorMs, src, dst, 0, false, first_sample, sample_count, sx0, sy0, dx0,
               dy0, w, h);
    } else {
      VkImageCopy region{};
      region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
      region.srcOffset = {int32_t(sx0), int32_t(sy0), 0};
      region.dstOffset = {int32_t(dx0), int32_t(dy0), 0};
      region.extent = {w, h, 1};
      vk::vkCmdCopyImage(cb_, src->image, VK_IMAGE_LAYOUT_GENERAL, dst->image,
                         VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    }
    ResolveBarrier(src, read);
  }
  // Clear after resolve, of the resolved rectangle only (with EDRAM tiling
  // this clears one tile; here all tiles share one full-size image).
  const VkRect2D clear_rect{{int32_t(sx0), int32_t(sy0)},
                            {std::min(sx1, src->width) - std::min(sx0, src->width),
                             std::min(sy1, src->height) - std::min(sy0, src->height)}};
  if ((r.flags & 0x100) && source != 4) {
    ClearImage(src, r.clear_rgba, 0, 0, true, false, false, &clear_rect);
  }
  if (r.flags & 0x200) {
    Image* d = GetTarget(DepthKey(surface, r.targets[2]), true);
    ClearImage(d, r.clear_rgba, r.clear_z, 0, false, true, true, &clear_rect);
  }
}

void Renderer::Impl::DropTextureSets() {
  if (tex_set_pool_) retired_tex_set_pools_.push_back({frame_number_, tex_set_pool_});
  tex_set_pool_ = VK_NULL_HANDLE;
  tex_set_pool_left_ = 0;
  tex_sets_.clear();
  tex_set_views_.clear();
  for (int i = 0; i < 2; ++i) {
    last_tex_key_[i].clear();
    last_tex_set_[i] = VK_NULL_HANDLE;
  }
}

VkDescriptorSet Renderer::Impl::TextureSet(uint32_t set, const std::vector<TexDesc>& descs) {
  // A view a cached set uses was destroyed: none of them can be trusted.
  if (!gpu_.destroyed_views.empty()) {
    for (VkImageView v : gpu_.destroyed_views) {
      if (tex_set_views_.count(v)) {
        DropTextureSets();
        ++stats_.texture_set_resets;
        break;
      }
    }
    gpu_.destroyed_views.clear();
  }
  char key_buf[1 + 16 * (4 + 2 * sizeof(void*))];
  size_t key_len = 0;
  key_buf[key_len++] = char(set);
  for (const auto& td : descs) {
    if (td.set != set || key_len + 4 + 2 * sizeof(void*) > sizeof(key_buf)) continue;
    std::memcpy(key_buf + key_len, &td.binding, 4);
    std::memcpy(key_buf + key_len + 4, &td.info.sampler, sizeof(void*));
    std::memcpy(key_buf + key_len + 4 + sizeof(void*), &td.info.imageView, sizeof(void*));
    key_len += 4 + 2 * sizeof(void*);
  }
  const std::string_view key(key_buf, key_len);
  if (last_tex_set_[set - 2] && key == last_tex_key_[set - 2]) return last_tex_set_[set - 2];
  auto it = tex_sets_.find(key);
  if (it != tex_sets_.end()) {
    last_tex_key_[set - 2].assign(key);
    return last_tex_set_[set - 2] = it->second;
  }
  if (!tex_set_pool_left_) {
    DropTextureSets();
    VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kTexSetPoolSets * 16};
    VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpi.maxSets = kTexSetPoolSets;
    dpi.poolSizeCount = 1;
    dpi.pPoolSizes = &size;
    vk::Check(vk::vkCreateDescriptorPool(gpu_.device, &dpi, nullptr, &tex_set_pool_),
              "texture set pool");
    tex_set_pool_left_ = kTexSetPoolSets;
  }
  VkDescriptorSet ds;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = tex_set_pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &set_layouts_[set];
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &ds), "texture set");
  --tex_set_pool_left_;
  VkWriteDescriptorSet writes[16];
  uint32_t n = 0;
  for (const auto& td : descs) {
    if (td.set != set || n >= 16) continue;
    VkWriteDescriptorSet& w = writes[n++];
    w = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstSet = ds;
    w.dstBinding = td.binding;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w.pImageInfo = &td.info;
    tex_set_views_.insert(td.info.imageView);
  }
  if (n) vk::vkUpdateDescriptorSets(gpu_.device, n, writes, 0, nullptr);
  ++stats_.texture_sets_written;
  tex_sets_.emplace(std::string(key), ds);
  last_tex_key_[set - 2].assign(key);
  return last_tex_set_[set - 2] = ds;
}

void Renderer::Impl::DoDraw(const Draw& d, uint32_t number) {
  // Sampled frames (every 30th): where a draw's CPU time goes, by part
  // (RenderStats::draw_part_ms).
  const bool prof = profile_frame_;
  auto prof_t = prof ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
  auto mark = [&](int part) {
    if (!prof) return;
    const auto t = std::chrono::steady_clock::now();
    draw_part_ms_[part] += std::chrono::duration<double, std::milli>(t - prof_t).count();
    prof_t = t;
  };
  const GpuState& st = *d.state;
  // After the composite (the HUD, menus, the save notice) while the map
  // clouds part: the save notice held back, drawn over the clouds once the
  // rest is drawn (the clouds cover the rest).
  if (composite_rt_ && !replaying_notice_ && !clouds_drawn_) {
    UpdateClouds();
    if (map_clouds_ && cloud_active_ && !cloud_closing_ && !hold_picture_ && !Diag("cloudsontop") &&
        IsSaveNotice(d, *d.state)) {
      notice_draws_.push_back({&d, number});
      return;
    }
  }
  const bool composite_before = composite_seen_;
  if ((hide_interface_ || fc_on_ || fc_in_.hide_hud) && composite_seen_) return;  // the HUD / menus, left out
  if (d.vs.empty() || d.ps.empty()) {
    Skip(number, "no microcode recorded (capture from an older build)");
    return;
  }
  if (d.prim == 8 || (d.prim > 6 && d.prim != 13)) {
    Skip(number, "primitive type " + std::to_string(d.prim) + " not supported yet");
    return;
  }
  Shader* vs = GetShader(d.vs, ShaderStage::kVertex);
  Shader* ps = GetShader(d.ps, ShaderStage::kPixel);
  if (!vs->error.empty() || !ps->error.empty()) {
    Skip(number, !vs->error.empty() ? vs->error : ps->error);
    return;
  }
  if (ps->info.ucode_hash == kLightingPixelShader) {
    if (!light_seen_) {
      lighting_aa_ = st.reg(0x2312) & 0xF;
      NoteLightingPass(d, st);
    } else if ((snow_cover_ok_ || tracks_ok_) && (st.reg(0x2312) & 0xF) != lighting_aa_) {
      // (The next sample's lighting: its own albedo and normals resolved.)
      lighting_aa_ = st.reg(0x2312) & 0xF;
      SceneInputs(d, st);
    }
  }
  // Diagnostics: A3_LOG_DRAWS=1 lists each draw (its shaders, whether
  // after the lighting pass, its first texture, its count) on stderr.
  if (static const char* ld = std::getenv("A3_LOG_DRAWS"); ld) {
    std::fprintf(stderr, "draw %u vs %016llx ps %016llx lit %d tex %016llx idx %u\n", number,
                 (unsigned long long)vs->info.ucode_hash, (unsigned long long)ps->info.ucode_hash, int(light_seen_),
                 (unsigned long long)DrawTextureHash(d, st, 0), d.count);
  }
  // Diagnostics: A3_SKIP_VS=<hash>[,<hash>...] not drawn (which draws are
  // what); A3_SKIP_TEX the same by the first texture's hash.
  if (static const char* skip = std::getenv("A3_SKIP_VS"); skip) {
    char hx[24];
    std::snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)vs->info.ucode_hash);
    if (std::strstr(skip, hx)) return;
  }
  if (static const char* skip = std::getenv("A3_SKIP_TEX"); skip) {
    char hx[24];
    std::snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)DrawTextureHash(d, st, 0));
    if (std::strstr(skip, hx)) return;
  }
  mark(0);

  // Render targets.
  const uint32_t surface = st.reg(0x2000);
  const uint32_t mode_control = st.reg(0x2208) & 7;  // 4 = color + depth, 5 = depth only
  const uint32_t color_mask = st.reg(0x2104);
  std::vector<Image*>& colors = colors_scratch_;
  colors.clear();
  uint32_t mask_bits = 0;
  if (mode_control != 5) {
    const uint32_t infos[4] = {st.reg(0x2001), st.reg(0x2003), st.reg(0x2004), st.reg(0x2005)};
    for (uint32_t i = 0; i < 4; ++i) {
      if (i > 0 && !(ps->info.colors_written & (1u << i))) break;
      colors.push_back(GetTarget(ColorKey(surface, infos[i])));
      mask_bits |= ((ps->info.colors_written & (1u << i)) ? (color_mask >> (4 * i)) & 15 : 0)
                   << (4 * i);
    }
  }
  Image* depth = GetTarget(DepthKey(surface, st.reg(0x2002)));
  // Attachments must be the same size: drop the depth buffer if it differs.
  if (!colors.empty() && (depth->width != colors[0]->width || depth->height != colors[0]->height ||
                          depth->samples != colors[0]->samples)) {
    depth = nullptr;
  }
  for (const Image* c : colors) {
    if (c->samples != colors[0]->samples) {
      Skip(number, "render targets with different sample counts");
      return;
    }
  }
  if (colors.empty() && !depth) {
    Skip(number, "no render target");
    return;
  }
  // One sample for the guest's two (MSAA off): the one sample stands for
  // guest sample 0. Amped 3 uses its two samples as two layers for the
  // scene's lighting: it resolves sample 0, lights sample 0 alone
  // (PA_SC_AA_MASK 0x1111), resolves sample 1, lights sample 1 alone
  // (0x2222), and the final resolve averages the two. Without MSAA both
  // samples hold the same scene, so both lit layers are the same: drawing
  // only the sample-0 pass (and skipping the resolve of sample 1, see
  // DoResolve) gives the average exactly. Drawing both lit the scene twice
  // (much darker).
  {
    Image* att = colors.empty() ? depth : colors[0];
    if (att->samples == 1 && att->guest_samples > 1) {
      const uint32_t all = (1u << std::min(att->guest_samples, 4u)) - 1;
      const uint32_t aa_mask = st.reg(0x2312) & all;
      if (!(aa_mask & 1u)) return;  // other samples only
      if (aa_mask == all) {
        for (Image* c : colors) ++c->full_draws;
        if (depth) ++depth->full_draws;
      }
    }
  }

  mark(1);
  // Texture descriptors (prepared before rendering starts: uploads end it).

  std::vector<TexDesc>& descs = descs_scratch_;
  descs.clear();
  uint32_t scaled_textures = 0;  // bindings showing rendered (scaled) images
  // Upscaled scene: textures sampled as sharp as at the output resolution
  // (the vendors' recommendation, log2 of the render / output size).
  const float lod_bias =
      upscaler_ && tiling_.width && out_scale_ > tiled_scale_ ? std::log2(tiled_scale_ / out_scale_)
                                                              : 0.0f;
  float tex_scales[32];
  std::fill(std::begin(tex_scales), std::end(tex_scales), 1.0f);
  for (int stage = 0; stage < 2; ++stage) {
    const Shader& s = stage == 0 ? *vs : *ps;
    for (const auto& b : s.info.textures) {
      if (b.binding >= 16) continue;
      // Image and sampler, looked up once per frame for each distinct fetch
      // constant + data + filter override (until a resolve changes things).
      const uint32_t* words = FetchWords(d, b.fetch_constant, st);
      const std::string* data_name = nullptr;
      for (const auto& t : d.textures) {
        if (t.slot == b.fetch_constant) data_name = &t.data;
      }
      uint64_t lk = 0x84222325CBF29CE4ull;
      for (int w = 0; w < 6; ++w) lk = (lk ^ words[w]) * 0x100000001B3ull;
      lk ^= data_name ? std::hash<std::string>{}(*data_name) * 31 : 7;
      lk ^= lod_bias != 0.0f ? 0x5BD1E995ull : 0;
      lk = (lk ^ uint64_t(uint32_t(b.mag_filter + 1) | (uint32_t(b.min_filter + 1) << 8) |
                          (uint32_t(b.aniso_filter + 1) << 16) |
                          (uint32_t(b.mip_filter + 1) << 24))) *
           0x9E3779B97F4A7C15ull;
      Image* im;
      VkSampler cached_sampler;
      auto hit = frame_tex_lookup_.find(lk);
      if (hit == frame_tex_lookup_.end()) {
        std::string why;
        Image* found = TextureFor(d, b.fetch_constant, st, &why);
        VkSampler sampler;
        if (found && found->replaced) {
          // Every level of the replacement, filtered: it is usually drawn
          // smaller than its size (a 4x font at 1080p).
          uint32_t rwords[6];
          std::memcpy(rwords, words, sizeof(rwords));
          rwords[4] = (rwords[4] & ~0x3FCu) | (15u << 6);  // mip levels 0..15
          amped3::native::TextureBinding rb = b;
          rb.mag_filter = rb.min_filter = rb.mip_filter = 1;
          sampler = SamplerFor(rwords, rb, lod_bias);
        } else {
          sampler = SamplerFor(words, b, lod_bias);
        }
        hit = frame_tex_lookup_.emplace(lk, TexLookup{found, sampler, std::move(why)}).first;
      }
      im = hit->second.image;
      cached_sampler = hit->second.sampler;
      // The lighting's painted slope shadows: the copy baked again around the
      // camera (BakeWorldShadows).
      if (baked_ok_ && stage == 1 && b.fetch_constant == 2 && im == baked_src_ &&
          ps->info.ucode_hash == kLightingPixelShader) {
        im = &baked_;
      }
      // Time of day: the sky-light table tinted; the game's fixed shadows
      // (the painted slopes, tf2; the shadow map, tf0) gone once the sun has
      // moved well away from where they were made for.
      // (The snow trails' lookup into the game's shadow map: the mask.)
      if (stage == 1 && TodOn() && tod_mask_ok_ && light_seen_ && tod_game_map_ &&
          hit->second.image == tod_game_map_ && ps->info.ucode_hash != kLightingPixelShader) {
        im = &tod_mask_;
      }
      if (stage == 1 && LightModOn() && ps->info.ucode_hash == kLightingPixelShader) {
        if (b.fetch_constant == 1 && tod_lut_ok_ && hit->second.image == tod_lut_src_) im = &tod_lut_;
        // (Fresh snow on its albedo.)
        if (b.fetch_constant == 3 && snow_cover_ok_ && hit->second.image == snow_albedo_src_) im = &snow_albedo_;
        // (World shadows: the slopes' map baked for the hour's sun, kept.)
        if (b.fetch_constant == 0 && tod_mask_ok_) {
          im = &tod_mask_;
        } else if ((b.fetch_constant == 2 || b.fetch_constant == 0) && TodNoGameShadows() &&
            tod_white_.image && !(im == &baked_ && baked_tod_)) {
          im = &tod_white_;
        }
      }
      // (The boards' tracks pressed into the snow.)
      if (stage == 1 && tracks_ok_ && ps->info.ucode_hash == kLightingPixelShader) {
        if (b.fetch_constant == 3 && hit->second.image == tracks_albedo_src_) im = &tracks_albedo_;
        if (b.fetch_constant == 4 && hit->second.image == tracks_normal_src_) im = &tracks_normal_;
      }
      const std::string& note = hit->second.note;
      // Diagnostics: vertex textures replaced ("novtex") or sampled plainly
      // ("vtexplain") in the --diag-draws range.
      if (stage == 0 && DiagRange() && Diag("novtex")) im = &dummy_;
      // Depth-only draws (Direct3D's null pixel shader is a texture copy with
      // no color writes and no alpha test) don't need the texture.
      const uint32_t colorctl = st.reg(0x2202);
      const bool output_unused = stage == 1 && st.reg(0x2104) == 0 && !(colorctl & 0x18) &&
                                 !ps->info.kills && !ps->info.writes_depth;
      if (!note.empty() && !output_unused) {
        if (++stats_.skipped["texture: " + note] <= 3) {
          log_ << "[" << number << "] texture: " << note << "\n";
        }
      }
      VkSampler sampler = (stage == 0 && DiagRange() && Diag("vtexplain"))
                              ? point_sampler_
                              : cached_sampler;
      VkDescriptorImageInfo ii{sampler, im->sample_view, VK_IMAGE_LAYOUT_GENERAL};
      descs.push_back({uint32_t(2 + stage), b.binding, ii});
      if (im->scaled) scaled_textures |= 1u << ((b.binding & 15) + (stage ? 16 : 0));
      tex_scales[(b.binding & 15) + (stage ? 16 : 0)] =
          im->scaled || im->replaced ? im->scale : 1.0f;
    }
  }

  mark(2);
  VkPipeline pipeline = PipelineFor(d, st, *vs, *ps, colors, depth, mask_bits);
  mark(3);
  if (!pipeline) {
    if (last_pipeline_pending_) {
      ++stats_.draws_waiting;
      return;
    }
    Skip(number, "pipeline creation failed");
    return;
  }

  // Uniforms.
  const uint32_t rt_w = colors.empty() ? depth->width : colors[0]->width;
  const uint32_t rt_h = colors.empty() ? depth->height : colors[0]->height;
  SystemConstants sys{};
  const uint32_t vte = st.reg(0x2206);
  float xs = (vte & 1) ? st.regf(0x210F) : 1.0f, xo = (vte & 2) ? st.regf(0x2110) : 0.0f;
  float ys = (vte & 4) ? st.regf(0x2111) : 1.0f, yo = (vte & 8) ? st.regf(0x2112) : 0.0f;
  float zs = (vte & 16) ? st.regf(0x2113) : 1.0f, zo = (vte & 32) ? st.regf(0x2114) : 0.0f;
  if ((st.reg(0x2302) & 1) == 0) {  // PA_SU_VTX_CNTL pix_center: Direct3D, centres at .0
    xo += 0.5f;
    yo += 0.5f;
  }
  // The viewport transform is in guest pixels: render targets are their
  // scale times larger.
  const float ts = (colors.empty() ? depth : colors[0])->scale;
  const float guest_w = float(rt_w) / ts, guest_h = float(rt_h) / ts;
  sys.ndc_scale[0] = 2.0f * xs / guest_w;
  sys.ndc_scale[1] = 2.0f * ys / guest_h;
  sys.ndc_scale[2] = zs;
  sys.ndc_offset[0] = 2.0f * xo / guest_w - 1.0f;
  sys.ndc_offset[1] = 2.0f * yo / guest_h - 1.0f;
  sys.res_scale = ts;
  sys.scaled_textures = scaled_textures;
  std::memcpy(sys.tex_scale, tex_scales, sizeof(tex_scales));
  sys.ndc_offset[2] = zo;
  // Shadow reach: the map's v remapped (v' = av (v - 1) + 1, the near edge
  // kept) to take in more of the slope ahead (see ShadowReach).
  // (Its geometry only: not the blur passes' full-screen quads.)
  if (shadow_av_ != 1.0f && (st.reg(0x2200) & 2) &&
      ShadowMapTarget(DepthKey(surface, st.reg(0x2002)))) {
    // ndc = 2 v - 1  ->  ndc' = av (ndc - 1) + 1
    sys.ndc_scale[1] *= shadow_av_;
    sys.ndc_offset[1] = shadow_av_ * (sys.ndc_offset[1] - 1.0f) + 1.0f;
  }
  // Upscaler jitter: the scene's depth-tested geometry, on its full-size
  // targets (not shadow maps or full-screen passes).
  if (upscaler_ && tiling_.width && (st.reg(0x2200) & 2) &&
      tiled_scale_ == scale_ && rt_w == Scaled(tiling_.width, scale_) &&
      rt_h == Scaled(tiling_.height, scale_)) {
    sys.ndc_offset[0] += 2.0f * jitter_x_ / float(rt_w);
    sys.ndc_offset[1] += 2.0f * jitter_y_ / float(rt_h);
  }
  sys.flags = ((vte >> 8) & 1 ? 1u : 0u) | ((vte >> 9) & 1 ? 2u : 0u) | ((vte >> 10) & 1 ? 0u : 4u);
  const uint32_t index_offset_reg = st.reg(0x2102);
  sys.vertex_index_offset = index_offset_reg ? index_offset_reg
                            : d.kind == Draw::Kind::kIndexed ? uint32_t(d.base_vertex)
                            : d.kind == Draw::Kind::kVertices ? d.start
                                                              : 0;
  sys.vertex_index_mask = 0x00FFFFFF;
  const uint32_t colorctl = st.reg(0x2202);
  sys.alpha_func = (colorctl & 8) ? (colorctl & 7) : 7;
  sys.alpha_ref = st.regf(0x210E);
  // alpha_to_mask_enable: hardware alpha to coverage on multisampled
  // targets (PipelineFor), else emulated in the shader (discard below 0.5).
  const Image* att0 = colors.empty() ? depth : colors[0];
  if ((colorctl & 0x10) && att0->samples == 1) sys.flags |= 256u;
  // Multisampled: alpha to coverage, and alpha tests that cut (greater /
  // greater-equal), as a sample mask from the shader (smooth, anti-aliased
  // foliage edges instead of the hardware's few coverage steps).
  // One sample for the guest's two: the same, dithered over neighbouring
  // pixels instead of samples (the upscaler's accumulation smooths it), so
  // foliage edges and the camera fade match the multisampled look.
  if ((att0->samples > 1 || att0->guest_samples > 1) && smooth_alpha_ &&
      ((colorctl & 0x10) || ((colorctl & 8) && ((colorctl & 7) == 4 || (colorctl & 7) == 6)))) {
    sys.flags |= 8192u | (std::min(att0->samples, 31u) << 16) | (((colorctl >> 24) & 0xFF) << 21);
    if (colorctl & 0x10) sys.flags |= 256u;  // alpha to mask
    // Scene foliage near the camera and toward the middle of the screen
    // fades out (XE_FLAG_CAMERA_FADE) so the rider and the way ahead stay
    // in view. Only alpha-to-coverage surfaces: the terrain and riders use a
    // plain alpha test.
    if (foliage_fade_ && tiling_.width && (colorctl & 0x10)) sys.flags |= 1u << 29;
    // One sample per pixel: the dither's per-frame phase (the shader).
    if (att0->samples == 1) sys.flags |= uint32_t(frame_number_ & 3) << 30;
  }
  // Freecam: the scene's depth-tested geometry (on the scene's depth target:
  // not the shadow maps, the lighting pass or other full-screen passes)
  // reprojected, the game's clip position -> (x, y, 1 - depth, 1) -> world
  // (the game's inverse) -> the freecam's -> back to a clip position with
  // the draw's own depth scale.
  if (fc_on_ && !composite_seen_ && (st.reg(0x2200) & 2) &&
      ps->info.ucode_hash != kLightingPixelShader &&
      SameTarget(DepthKey(surface, st.reg(0x2002)), fc_depth_)) {
    const double dzs = st.reg(0x2206) & 16 ? st.regf(0x2113) : 1.0;
    const double dzo = st.reg(0x2206) & 32 ? st.regf(0x2114) : 0.0;
    if (std::fabs(dzs) > 1e-12) {
      const double K[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -dzs, 1.0 - dzo, 0, 0, 0, 1};
      const double Ki[16] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1.0 / dzs, (1.0 - dzo) / dzs, 0, 0, 0, 1};
      double m[16];
      Mul4(fc_ivpg_, K, m);
      Mul4(fc_vpf_, m, m);
      Mul4(Ki, m, m);
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) sys.reproject[c * 4 + r] = float(m[r * 4 + c]);  // column-major
      }
      sys.flags |= 16384u;  // XE_FLAG_REPROJECT
    }
  }
  // Wind: trees, foliage and cloth swaying (WindMode).
  if (wind_on_) {
    int wm = WindMode(vs->info.ucode_hash);
    const uint64_t vh = vs->info.ucode_hash;
    if (wm == 1) {
      // (The trees' textures: their trunks are drawn from the same.)
      const uint64_t th = DrawTextureHash(d, st, 0);
      if (th && tree_textures_.size() < 256) tree_textures_.insert(th);
    }
    if (!wm && vh == kWorldMeshVs && !tree_textures_.empty() && tree_textures_.count(DrawTextureHash(d, st, 0)) &&
        !Diag("notrunks")) {
      wm = 5;  // a trunk: bent with its leaves
    }
    if (!wm && (vh == kWorldMeshVs || vh == kPropVs) && IsCloth(DrawContentHash(d, st, 0), cloth_extra_)) {
      wm = vh == kWorldMeshVs ? 3 : 4;
    }
    if (wm) SetWind(sys, st, wm);
  }
  // The surface, tagged into the normals' alpha (the scene's draws before
  // the lighting that write normals): from its albedo texture (fetch 0), or
  // the player's list (native_materials) by its content hash.
  // (Only where the normals are written whole: not blended over, which
  // would mix the tag into the game's bits.)
  const uint32_t blend1 = st.reg(0x2209);
  const bool rt1_opaque = ((blend1 >> 16) & 31u) == 1u && ((blend1 >> 24) & 31u) == 0u && ((blend1 >> 21) & 7u) == 0u;
  if ((specular_ > 0.0f || pick_state_) && !light_seen_ && (ps->info.colors_written & 2u) && rt1_opaque &&
      surfaces_on_) {
    std::string n0;
    uint8_t tag = 0;
    Image* t0 = TextureFor(d, 0, st, &n0);
    if (t0) tag = t0->surface;
    if (!material_overrides_.empty() || pick_state_) {
      const uint64_t h = DrawContentHash(d, st, 0);
      auto it = material_overrides_.find(h);
      if (it != material_overrides_.end()) tag = it->second;
      if (pick_state_ == 1 && h && !pick_index_.count(h)) {
        pick_index_[h] = uint32_t(pick_hashes_.size());
        pick_hashes_.push_back(h);
        pick_guess_.push_back(t0 ? t0->surface : 0);
      } else if (pick_state_ == 2) {
        auto pi = pick_index_.find(h);
        uint32_t v = pi != pick_index_.end() ? pi->second : 0;
        for (int k = 0; k < pick_cur_; ++k) v /= 7;
        tag = pi != pick_index_.end() ? uint8_t(1 + v % 7) : 0;
      }
    }
    sys.material[0] = tag;
  }
  sys.rt_inv_size[0] = 1.0f / float(rt_w);
  sys.rt_inv_size[1] = 1.0f / float(rt_h);
  sys.param_gen_register = (st.reg(0x2180) & (1u << 18)) ? ((st.reg(0x2181) >> 8) & 0xFF) : 255;
  // PA_SU_POINT_SIZE: half width (bits 16..31) in 12.4 fixed point.
  sys.point_size = std::max(float(st.reg(0x2280) >> 16) * (2.0f / 16.0f), 1.0f);
  if (d.prim == 1) sys.flags |= 128u;  // XE_FLAG_POINT (ps_param_gen point coordinates)
  if (debug_stores_ && DiagRange()) sys.flags |= 512u;  // XE_FLAG_DEBUG_STORE
  // Polygon offset: front values (PA_SU_POLY_OFFSET_FRONT_SCALE/OFFSET), or
  // the back ones when only back faces are offset.
  const uint32_t su_mode = st.reg(0x2205);
  const bool offset_front = (su_mode >> 11) & 1, offset_back = (su_mode >> 12) & 1;
  float poly_scale = 0.0f, poly_offset = 0.0f;
  if (offset_front || offset_back) {
    const uint32_t r = offset_front ? 0x2380 : 0x2382;
    poly_scale = st.regf(r);
    poly_offset = st.regf(r + 1);
  }
  // Constant part in the depth format's units, as the SDK's emulation
  // converts it (util/draw.h GetD3D10IntegerPolygonOffset): for float24 depth
  // the offset counts float24 ULPs of the primitive's largest depth, which on
  // our float32 buffer is 8 float32 ULPs each.
  float bias_constant = 0.0f;
  if (poly_offset != 0.0f) {
    const bool float24 = depth && ((st.reg(0x2002) >> 16) & 1);
    float units = std::ceil(std::fabs(poly_offset) *
                            (float24 ? float(1u << 21) : float(1u << 24)));
    if (float24) units *= 8.0f;
    bias_constant = poly_offset < 0 ? -units : units;
  }
  mark(12);
  uint32_t fetch[192];
  std::memcpy(fetch, st.fetch.data(), sizeof(fetch));
  for (const auto& [vf, vmem_name] : d.vmem) {
    // UP draws: Direct3D copies the user's vertices into the command buffer and
    // points the fetch constant there without updating its copy; use the
    // saved user data instead.
    const std::string& name = d.up_vertices.empty() ? vmem_name : d.up_vertices;
    auto it = mem_offsets_.find(name);
    if (vf >= 96) continue;
    if (it == mem_offsets_.end() || it->second == UINT64_MAX) {
      // The shader would read far outside the vertex buffer (some drivers,
      // SwiftShader among them, crash on that).
      Skip(number, "vertex data missing");
      return;
    }
    fetch[vf * 2] = uint32_t(it->second) | (fetch[vf * 2] & 3);
  }
  if (!d.up_vertices.empty()) {
    // The shadow constant may never have held a vertex buffer at all (the
    // intro videos' quad is the first thing drawn), so there is no vmem entry
    // for it: point every vertex fetch constant the shader reads at the user
    // data, as Direct3D does (type 3, 8-in-32 like its vertex buffers).
    auto it = mem_offsets_.find(d.up_vertices);
    if (it != mem_offsets_.end() && it->second != UINT64_MAX) {
      const uint64_t data_size = mem_sizes_[d.up_vertices];
      for (uint32_t vf = 0; vf < 96; ++vf) {
        if (!(vs->info.vertex_fetch_constants[vf >> 5] & (1u << (vf & 31)))) continue;
        if (d.vmem.count(vf)) continue;  // done above
        fetch[vf * 2] = uint32_t(it->second) | 3;
        fetch[vf * 2 + 1] = (uint32_t(data_size / 4) << 2) | 2;
      }
    }
  } else {
    // Every vertex fetch constant the shader reads must point into the
    // vertex data: one that wasn't recorded would make it read far outside.
    for (const uint32_t vf : vs->vfetch) {
      if (!d.vmem.count(vf)) {
        Skip(number, "vertex fetch constant vf" + std::to_string(vf) + " not recorded");
        return;
      }
    }
  }
  mark(9);  // (the draw's system constants and vertex data)
  // Uniform blocks: each written only when it differs from what an earlier
  // draw this frame already uploaded.
  uint32_t offsets[5];
  if (last_sys_offset_ == UINT32_MAX || std::memcmp(&sys, &last_sys_, sizeof(sys)) != 0) {
    last_sys_ = sys;
    last_sys_offset_ = WriteUniform(&sys, sizeof(sys));
  }
  offsets[0] = last_sys_offset_;
  offsets[1] = ConstantsFor(0, st.vs_consts, *vs);
  if (shadow_av_ != 1.0f && vs->info.ucode_hash == kTrailVertexShader) {
    // The snow trails take the riders' shadows from the map too (rows c9,
    // c10 over c12: u, v): the same remap, v' = av (v - 1) + 1.
    uint8_t* out;
    offsets[1] = ReserveConstants(ConstBytes(*vs), &out);
    const uint32_t* src = st.vs_consts.data();
    std::memcpy(out, src, ConstBytes(*vs));
    float c10[4], c12[4];
    std::memcpy(c10, src + 10 * 4, 16);
    std::memcpy(c12, src + 12 * 4, 16);
    for (int i = 0; i < 4; ++i) c10[i] = shadow_av_ * (c10[i] - c12[i]) + c12[i];
    std::memcpy(out + 10 * 16, c10, 16);
  }
  // (The snow trails take the riders' shadows from the shadow map, rows
  // c9, c10 over c12: the time of day's mask, through the screen's rows.)
  const bool trail_tod = TodOn() && tod_mask_ok_ && vs->info.ucode_hash == kTrailVertexShader;
  const bool vs_needs = LightModOn() && TodNeeds(0, *vs);
  if (vs_needs && !trail_tod && vs->info.ucode_hash != kTrailVertexShader && !Diag("nconstcache")) {
    offsets[1] = TodConstants(0, st.vs_consts, *vs);
  } else if (vs_needs || trail_tod) {
    uint32_t vc[1024];
    std::memcpy(vc, st.vs_consts.data(), sizeof(vc));
    bool changed = vs_needs && TodApply(0, *vs, vc);
    if (trail_tod) {
      for (int r = 0; r < 3; ++r) {
        const int reg = r == 0 ? 9 : r == 1 ? 10 : 12;
        float v[4];
        for (int i = 0; i < 4; ++i) v[i] = float(tod_mask_rows_[r * 4 + i]);
        std::memcpy(&vc[reg * 4], v, 16);
      }
      changed = true;
    }
    if (changed) {
      uint8_t* out;
      offsets[1] = ReserveConstants(ConstBytes(*vs), &out);
      std::memcpy(out, vc, ConstBytes(*vs));
    }
  }
  offsets[2] = ConstantsFor(1, st.ps_consts, *ps);
  // Freecam: the lighting pass works out each pixel's world position from
  // the depth with its inverse view-projection (c0..c3) and the view from
  // the camera's position (c4): the freecam's, the depth being seen from it.
  const uint32_t* lit_src = st.ps_consts.data();
  uint32_t fc_consts[1024];
  if (fc_on_ && ps->info.ucode_hash == kLightingPixelShader) {
    std::memcpy(fc_consts, st.ps_consts.data(), sizeof(fc_consts));
    for (int i = 0; i < 16; ++i) {
      const float f = float(fc_ivpf_[i]);
      std::memcpy(&fc_consts[i], &f, 4);
    }
    for (int i = 0; i < 3; ++i) {
      const float f = float(fc_p_[i]);
      std::memcpy(&fc_consts[16 + i], &f, 4);
    }
    lit_src = fc_consts;
    uint8_t* out;
    offsets[2] = ReserveConstants(ConstBytes(*ps), &out);
    std::memcpy(out, fc_consts, ConstBytes(*ps));
  }
  // Time of day: the light from the hour's sun (or moon), its color and the
  // sky's light scaled from the mountain's. The game's lighting block: toward
  // the sun; the sky's light as spherical harmonics (24 registers: 4 sets of
  // red, then green, then blue, the first band; then the second band the
  // same); the sun's color (4 sets). The lighting pass has it at c14, c15,
  // c39; vertex-lit things (riders, characters) at c238, c206, c230.
  if (LightModOn() && ps->info.ucode_hash == kLightingPixelShader) {
    if (lit_src != fc_consts) std::memcpy(fc_consts, lit_src, sizeof(fc_consts));
    TodLightingBlock(fc_consts, 14, 15, 39);
    if (tod_mask_ok_) {
      for (int r = 0; r < 3; ++r) {
        const int reg = r == 0 ? 6 : r == 1 ? 7 : 9;
        float v[4];
        for (int i = 0; i < 4; ++i) v[i] = float(tod_mask_rows_[r * 4 + i]);
        std::memcpy(&fc_consts[reg * 4], v, 16);
      }
    }
    lit_src = fc_consts;
    uint8_t* out;
    offsets[2] = ReserveConstants(ConstBytes(*ps), &out);
    std::memcpy(out, fc_consts, ConstBytes(*ps));
  } else if (LightModOn() && TodNeeds(1, *ps)) {
    if (lit_src == st.ps_consts.data() && !Diag("nconstcache")) {
      offsets[2] = TodConstants(1, st.ps_consts, *ps);
    } else {
      if (lit_src != fc_consts) std::memcpy(fc_consts, lit_src, sizeof(fc_consts));
      if (TodApply(1, *ps, fc_consts)) {
        lit_src = fc_consts;
        uint8_t* out;
        offsets[2] = ReserveConstants(ConstBytes(*ps), &out);
        std::memcpy(out, fc_consts, ConstBytes(*ps));
      }
    }
  }
  if (shadow_res_ > 1.0f && ps->info.ucode_hash == kShadowBlurPixelShader &&
      ShadowMapTarget(DepthKey(surface, st.reg(0x2002)))) {
    // The shadow map's blur (5 taps 1.25 texels apart, c255.xy = 1 / size):
    // kept to the same number of texels of the sharper map, so its edges
    // get sharper rather than staying as soft as the game's.
    uint8_t* out;
    offsets[2] = ReserveConstants(ConstBytes(*ps), &out);
    std::memcpy(out, st.ps_consts.data(), ConstBytes(*ps));
    float c255[4];
    std::memcpy(c255, st.ps_consts.data() + 255 * 4, 16);
    c255[0] /= shadow_res_;
    c255[1] /= shadow_res_;
    std::memcpy(out + 255 * 16, c255, 16);
  }
  // (The time of day's mask holds the sun's shadows all over the screen -
  // near by from the sun maps, the clouds', far off the far slopes' - so
  // the game's fade of its shadow map with distance is left out: whole
  // everywhere.)
  const bool mask_all = TodOn() && tod_mask_ok_ && !Diag("maskfade");
  if ((shadow_av_ != 1.0f || shadow_fade_ != 1.0f || mask_all) && ps->info.ucode_hash == kLightingPixelShader) {
    // The lighting pass's lookup into the shadow map (rows c6, c7 over c9:
    // u, v), remapped the same way: v' = av (v - 1) + 1.
    uint8_t* out;
    offsets[2] = ReserveConstants(ConstBytes(*ps), &out);
    const uint32_t* src = lit_src;
    std::memcpy(out, src, ConstBytes(*ps));
    float c7[4], c9[4];
    std::memcpy(c7, src + 7 * 4, 16);
    std::memcpy(c9, src + 9 * 4, 16);
    for (int i = 0; i < 4; ++i) c7[i] = shadow_av_ * (c7[i] - c9[i]) + c9[i];
    if (!(TodOn() && tod_mask_ok_)) std::memcpy(out + 7 * 16, c7, 16);  // (the time of day's: the screen's)
    // The fade (shadows gone at raw scene depth c10.x, whole from c11.x)
    // moved out with the reach.
    float c10[4], c11[4];
    std::memcpy(c10, src + 10 * 4, 16);
    std::memcpy(c11, src + 11 * 4, 16);
    c10[0] *= shadow_fade_;
    c11[0] *= shadow_fade_;
    if (mask_all) {
      // (fade = saturate((depth - c10) / (c11 - c10)): 1 for any depth.)
      c10[0] = -2.0f;
      c11[0] = -1.0f;
    }
    std::memcpy(out + 10 * 16, c10, 16);
    std::memcpy(out + 11 * 16, c11, 16);
  }
  // Riders' and characters' shine (SetRiderShine): the game's character
  // pixel shaders add a broad Phong highlight, pow(R.L, c32.x) with L in c31
  // and an exponent of about 7, which reads as plastic on cloth and skin.
  // Scaled by s and tightened by moving the constants alone: the exponent
  // raised to p', L shortened to s^(1/p') (so the highlight is s times
  // as bright). (Told by what they read: c29 / c30 as relative bases, c31,
  // c32; nothing else of the game's reads that set.)
  if (rider_shine_ != 1.0f && ps->info.dynamic_constants && !Diag("ridershine")) {
    auto reads = [&](uint32_t r) { return (ps->info.float_constants[r >> 5] >> (r & 31)) & 1u; };
    if (reads(29) && reads(30) && reads(31) && reads(32) && ConstBytes(*ps) >= 33 * 16) {
      const size_t n = ConstBytes(*ps);
      const uint8_t* src = uniforms_.mapped + offsets[2];
      uint8_t* out;
      const uint32_t off = ReserveConstants(n, &out);
      std::memmove(out, src, n);
      float L[4], P[4];
      std::memcpy(L, out + 31 * 16, 16);
      std::memcpy(P, out + 32 * 16, 16);
      if (P[0] > 0.5f && P[0] < 200.0f) {
        const float sh = std::max(rider_shine_, 1e-4f);
        // (Tighter as it's turned down: rough cloth, not a plastic coat.)
        const float p2 = P[0] * (sh < 1.0f ? 1.0f + 1.5f * (1.0f - sh) : 1.0f);
        const float k = std::pow(sh, 1.0f / p2);
        for (int i = 0; i < 3; ++i) L[i] *= k;
        P[0] = p2;
        std::memcpy(out + 31 * 16, L, 16);
        std::memcpy(out + 32 * 16, P, 16);
        offsets[2] = off;
      } else {
        uniform_used_ -= n;  // (not that shader after all: handed back)
      }
    }
  }
  if (st.bool_loop.version() != bool_loop_version_) {
    bool_loop_version_ = st.bool_loop.version();
    bool_loop_offset_ = WriteUniform(st.bool_loop.data(), 160);
  }
  offsets[3] = bool_loop_offset_;
  if (last_fetch_offset_ == UINT32_MAX || std::memcmp(fetch, last_fetch_, sizeof(last_fetch_)) != 0) {
    std::memcpy(last_fetch_, fetch, sizeof(last_fetch_));
    last_fetch_offset_ = WriteUniform(fetch, 768);
  }
  offsets[4] = last_fetch_offset_;

  mark(4);
  // Texture sets, shared by draws binding the same textures and samplers
  // (cached from frame to frame: TextureSet).
  VkDescriptorSet tex_sets[2] = {TextureSet(2, descs), TextureSet(3, descs)};

  mark(5);
  // Indices (host-converted to 32-bit). Quad lists become triangle lists.
  const bool quads = d.prim == 13;
  const bool indexed_source = d.kind == Draw::Kind::kIndexed || d.kind == Draw::Kind::kIndexedUP;
  // Diagnostics ("list"): non-indexed triangle strips drawn as lists.
  const bool strip_list = Diag("list") && d.prim == 6 && !indexed_source;
  const bool indexed = indexed_source || quads || strip_list;
  uint32_t first_index = 0, index_count = d.count;
  VkIndexType index_type = VK_INDEX_TYPE_UINT32;
  cur_index_buffer_ = indices_.buffer;
  auto cached_ix = indexed && indexed_source && !quads && !strip_list &&
                           d.kind == Draw::Kind::kIndexed
                       ? ix_offsets_.find(d.index_data)
                       : ix_offsets_.end();
  if (cached_ix != ix_offsets_.end() && cached_ix->second != UINT64_MAX) {
    // The index buffer, already in the vertex arena (byte-swapped once).
    const uint32_t isz = d.index_32bit ? 4 : 2;
    const auto size_it = ix_sizes_.find(d.index_data);
    if (size_it == ix_sizes_.end() || (size_t(d.start) + d.count) * isz > size_it->second) {
      Skip(number, "index range outside the saved index buffer");
      return;
    }
    cur_index_buffer_ = arena_.buffer;
    index_type = isz == 2 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
    first_index = uint32_t(cached_ix->second / isz) + d.start;
    index_count = d.count;
  } else if (indexed && indexed_source && !quads && !strip_list) {
    // The common case: the guest's indices byte-swapped straight into the
    // ring, 16-bit ones kept 16-bit (restart index 0xFFFF either way).
    const auto& data = Bytes(d.index_data);
    if (data.empty()) {
      Skip(number, "index data missing");
      return;
    }
    const uint32_t isz = d.index_32bit ? 4 : 2;
    const size_t start = d.kind == Draw::Kind::kIndexed ? size_t(d.start) * isz : 0;
    if (start + size_t(d.count) * isz > data.size()) {
      Skip(number, "index range outside the saved index buffer");
      return;
    }
    index_used_ = (index_used_ + 3) & ~size_t(3);
    const size_t bytes = size_t(d.count) * isz;
    if (index_used_ + bytes > indices_.size) {
      Skip(number, "index ring full");
      return;
    }
    const uint8_t* p = data.data() + start;
    uint8_t* out = indices_.mapped + index_used_;
    if (isz == 2) {
      for (uint32_t i = 0; i < d.count; ++i) {
        uint16_t v;
        std::memcpy(&v, p + i * 2, 2);
        v = uint16_t((v >> 8) | (v << 8));
        std::memcpy(out + i * 2, &v, 2);
      }
      index_type = VK_INDEX_TYPE_UINT16;
    } else {
      for (uint32_t i = 0; i < d.count; ++i) {
        uint32_t v;
        std::memcpy(&v, p + i * 4, 4);
        v = (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
        std::memcpy(out + i * 4, &v, 4);
      }
    }
    first_index = uint32_t(index_used_ / isz);
    index_used_ += bytes;
    index_count = d.count;
  } else if (indexed) {
    std::vector<uint32_t> src;
    if (indexed_source) {
      const auto& data = Bytes(d.index_data);
      if (data.empty()) {
        Skip(number, "index data missing");
        return;
      }
      const uint32_t isz = d.index_32bit ? 4 : 2;
      const size_t start = d.kind == Draw::Kind::kIndexed ? size_t(d.start) * isz : 0;
      if (start + size_t(d.count) * isz > data.size()) {
        Skip(number, "index range outside the saved index buffer");
        return;
      }
      const uint8_t* p = data.data() + start;
      src.resize(d.count);
      for (uint32_t i = 0; i < d.count; ++i) {
        uint32_t v = isz == 4 ? (uint32_t(p[i * 4]) << 24) | (uint32_t(p[i * 4 + 1]) << 16) |
                                    (uint32_t(p[i * 4 + 2]) << 8) | p[i * 4 + 3]
                              : (uint32_t(p[i * 2]) << 8) | p[i * 2 + 1];
        if (isz == 2 && v == 0xFFFF) v = 0xFFFFFFFF;
        src[i] = v;
      }
    } else {
      // Auto-indexed: the vertex index offset (start vertex) is added in the shader.
      src.resize(d.count);
      for (uint32_t i = 0; i < d.count; ++i) src[i] = i;
    }
    std::vector<uint32_t> out_indices;
    if (strip_list) {
      for (size_t t = 0; t + 2 < src.size(); ++t) {
        if (t & 1) {
          out_indices.insert(out_indices.end(), {src[t + 1], src[t], src[t + 2]});
        } else {
          out_indices.insert(out_indices.end(), {src[t], src[t + 1], src[t + 2]});
        }
      }
    } else if (quads) {
      for (size_t q = 0; q + 3 < src.size(); q += 4) {
        const uint32_t a = src[q], b = src[q + 1], c = src[q + 2], e = src[q + 3];
        out_indices.insert(out_indices.end(), {a, b, c, a, c, e});
      }
    } else {
      out_indices = std::move(src);
    }
    index_used_ = (index_used_ + 3) & ~size_t(3);
    if (index_used_ + out_indices.size() * 4 > indices_.size) {
      Skip(number, "index ring full");
      return;
    }
    first_index = uint32_t(index_used_ / 4);
    std::memcpy(indices_.mapped + index_used_, out_indices.data(), out_indices.size() * 4);
    index_used_ += out_indices.size() * 4;
    index_count = uint32_t(out_indices.size());
  }

  // Ambient occlusion: the scene's depth with the riders. Kept before the
  // first see-through draw that writes depth after the lighting (snow
  // spray, in-world labels); around the character passes that write depth
  // but no picture (render target 0 untouched: the riders drawn through
  // another view, whose depth lands where nothing shows), the depth before
  // and after them, to tell what they wrote from the riders drawn later.
  if ((ao_strength_ > 0.0f || world_detail_ > 0) && light_seen_ && !ao_late_done_ && depth &&
      tiling_.width &&
      ps->info.ucode_hash != kLightingPixelShader && ((st.reg(0x2200) & 6) == 6) &&
      max_draws_ == UINT32_MAX) {
    const bool float_depth = (st.reg(0x2002) >> 16) & 1;
    const uint32_t blend0 = st.reg(0x2201);
    // (Characters draw see-through parts too, hair: those stay.)
    const bool blended = (blend0 & 0x1FFF1FFF) != 0x00010001 && mask_bits &&
                         !vs->info.dynamic_constants;
    const bool label_or_icon =
        (!d.up_vertices.empty() && d.prim == 13) ||
        (vs->info.ucode_hash == 0x6DA245E02A477C07ull && ps->info.ucode_hash == 0x2C0355A9D2589300ull);
    const bool no_picture = (mask_bits & 0xF) == 0;
    if (blended || label_or_icon) {
      if (Diag("ao")) std::fprintf(stderr, "ao: late depth before draw %u\n", number);
      ao_late_done_ = true;
      if (ao_hidden_state_ == 1) {  // (still in them: what they wrote so far)
        ao_hidden_ok_ = SnapshotDepth(depth, float_depth, ao_after_hidden_, "ao_after_hidden");
        ao_hidden_state_ = 2;
      }
      ao_late_ok_ = SnapshotDepth(depth, float_depth, ao_late_depth_, "ao_late_depth");
    } else if (no_picture && ao_hidden_state_ == 0) {
      if (Diag("ao")) std::fprintf(stderr, "ao: depth-only passes from draw %u\n", number);
      ao_hidden_state_ = 1;
      ao_before_ok_ = SnapshotDepth(depth, float_depth, ao_before_hidden_, "ao_before_hidden");
    } else if (!no_picture && ao_hidden_state_ == 1) {
      if (Diag("ao")) std::fprintf(stderr, "ao: depth-only passes end before draw %u\n", number);
      ao_hidden_state_ = 2;
      ao_hidden_ok_ = SnapshotDepth(depth, float_depth, ao_after_hidden_, "ao_after_hidden");
    }
  }
  // Sun shadows: the inputs kept at the shadow map's first blur (it blurs
  // the map in place).
  if (SunShadowsOn() && !sun_prepared_ && max_draws_ == UINT32_MAX && !light_seen_ &&
      ps->info.ucode_hash == kShadowBlurPixelShader &&
      ShadowMapTarget(DepthKey(surface, st.reg(0x2002)))) {
    sun_prepared_ = true;
    PrepareSunShadows(d, st);
  }
  mark(6);
  keep_target_clears_ = true;
  BeginRendering(colors, depth);
  keep_target_clears_ = false;
  // Commands only for state that differs from the previous draw in this
  // render pass (each Vulkan call costs driver time; draws mostly repeat).
  DynState& ds = dyn_;
  if (!ds.valid || ds.pipeline != pipeline) {
    vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    ds.pipeline = pipeline;
  }
  VkDescriptorSet sets[4] = {set0_, set1_, tex_sets[0], tex_sets[1]};
  if (!ds.valid || std::memcmp(ds.sets, sets, sizeof(sets)) != 0 ||
      std::memcmp(ds.offsets, offsets, sizeof(offsets)) != 0) {
    vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 5,
                                offsets);
    std::memcpy(ds.sets, sets, sizeof(sets));
    std::memcpy(ds.offsets, offsets, sizeof(offsets));
  }
  if (!ds.valid || ds.rt_w != rt_w || ds.rt_h != rt_h) {
    VkViewport vp{0, 0, float(rt_w), float(rt_h), 0.0f, 1.0f};
    vk::vkCmdSetViewport(cb_, 0, 1, &vp);
    ds.rt_w = rt_w;
    ds.rt_h = rt_h;
  }
  // Screen scissor (PA_SC_SCREEN_SCISSOR_TL / BR).
  const uint32_t tl = st.reg(0x200E), br = st.reg(0x200F);
  int32_t x0 = int32_t(Scaled(tl & 0x7FFF, ts)), y0 = int32_t(Scaled((tl >> 16) & 0x7FFF, ts));
  int32_t x1 = std::min<int32_t>(int32_t(Scaled(br & 0x7FFF, ts)), int32_t(rt_w));
  int32_t y1 = std::min<int32_t>(int32_t(Scaled((br >> 16) & 0x7FFF, ts)), int32_t(rt_h));
  VkRect2D sc{{x0, y0}, {uint32_t(std::max(0, x1 - x0)), uint32_t(std::max(0, y1 - y0))}};
  if (!ds.valid || std::memcmp(&ds.scissor, &sc, sizeof(sc)) != 0) {
    vk::vkCmdSetScissor(cb_, 0, 1, &sc);
    ds.scissor = sc;
  }
  float blend_constants[4] = {st.regf(0x2105), st.regf(0x2106), st.regf(0x2107), st.regf(0x2108)};
  if (!ds.valid || std::memcmp(ds.blend, blend_constants, sizeof(blend_constants)) != 0) {
    vk::vkCmdSetBlendConstants(cb_, blend_constants);
    std::memcpy(ds.blend, blend_constants, sizeof(blend_constants));
  }
  // Slope scale is in 1/16-pixel units (as the SDK's emulation treats it).
  const float bias[2] = {bias_constant * options_.depth_bias_scale,
                         poly_scale * (1.0f / 16.0f) * options_.depth_bias_scale};
  if (!ds.valid || std::memcmp(ds.bias, bias, sizeof(bias)) != 0) {
    vk::vkCmdSetDepthBias(cb_, bias[0], 0.0f, bias[1]);
    std::memcpy(ds.bias, bias, sizeof(bias));
  }
  const uint32_t srm = st.reg(0x210D), srm_bf = st.reg(0x210C);
  const bool bf = (st.reg(0x2200) >> 7) & 1;
  const uint32_t back = bf ? srm_bf : srm;
  if (!ds.valid || ds.stencil_front != srm || ds.stencil_back != back) {
    vk::vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_BIT, srm & 0xFF);
    vk::vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_BIT, (srm >> 8) & 0xFF);
    vk::vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_BIT, (srm >> 16) & 0xFF);
    vk::vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_BACK_BIT, back & 0xFF);
    vk::vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_BACK_BIT, (back >> 8) & 0xFF);
    vk::vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_BACK_BIT, (back >> 16) & 0xFF);
    ds.stencil_front = srm;
    ds.stencil_back = back;
  }
  if (indexed) {
    if (!ds.valid || !ds.index_bound || ds.index_type != index_type ||
        ds.index_buffer != cur_index_buffer_) {
      vk::vkCmdBindIndexBuffer(cb_, cur_index_buffer_, 0, index_type);
      ds.index_bound = true;
      ds.index_type = index_type;
      ds.index_buffer = cur_index_buffer_;
    }
    vk::vkCmdDrawIndexed(cb_, index_count, 1, first_index, 0, 0);
  } else {
    vk::vkCmdDraw(cb_, d.count, 1, 0, 0);
  }
  // The light editor's pick: the same again over the middle pixel alone, in
  // an occlusion query (passing: in front there, so far) - what the scene
  // pass's objects are kept for (the same draws as below).
  pick_q_ = -1;
  if (pick_queries_ && cur_slot_ && pick_used_ < kPickMax && collect_instances_ && !light_seen_ && tiling_.width &&
      depth && (st.reg(0x2200) & 6) == 6 && max_draws_ == UINT32_MAX && pending_instances_.size() < 8192) {
    const int32_t cx = int32_t(rt_w / 2), cy = int32_t(rt_h / 2);
    if (cx >= sc.offset.x && cy >= sc.offset.y && cx < sc.offset.x + int32_t(sc.extent.width) &&
        cy < sc.offset.y + int32_t(sc.extent.height)) {
      const uint32_t q = uint32_t(cur_slot_ - slots_) * kPickMax + pick_used_;
      const VkRect2D one{{cx, cy}, {1, 1}};
      vk::vkCmdSetScissor(cb_, 0, 1, &one);
      vk::vkCmdBeginQuery(cb_, pick_pool_, q, 0);
      if (indexed) {
        vk::vkCmdDrawIndexed(cb_, index_count, 1, first_index, 0, 0);
      } else {
        vk::vkCmdDraw(cb_, d.count, 1, 0, 0);
      }
      vk::vkCmdEndQuery(cb_, pick_pool_, q);
      vk::vkCmdSetScissor(cb_, 0, 1, &sc);
      pick_q_ = int(pick_used_++);
      cur_slot_->pick_count = pick_used_;
    }
  }
  ds.valid = true;
  if (!colors.empty()) last_color_ = colors[0];
  if (!composite_before && composite_seen_ && !colors.empty() && colors[0]->samples == 1) composite_rt_ = colors[0];
  ++stats_.draws_done;
  mark(7);
  struct AfterMark {
    decltype(mark)& m;
    ~AfterMark() { m(8); }
  } after_mark{mark};

  // Object motion blur: this draw's constants for the next frame, and its
  // velocity copy (solid geometry of the scene pass: depth written, not drawn
  // from user-pointer data; the scenery before the lighting pass, the
  // characters after it).
  // For the upscaler alone (object blur off) only the draws after the
  // lighting pass (characters, the rider and board: what moves by itself)
  // need their own motion; the scenery before it is static, and the
  // camera's motion from the depth is exact for it. That skips nearly all
  // of the scene's draws (recording each one twice cost a few ms a frame).
  // Sun shadows: the scene pass's trees and props (before the lighting),
  // kept to be drawn again from the sun into the tree map; the shadow
  // pass's terrain, to be drawn again alone (the ground under the casters).
  // Snow tracks: the trails' draws (each trail once: the game draws it in
  // several passes), kept to be drawn again from above.
  // (Only its colored pass: the game's earlier ones mark the stencil and
  // alpha with the same strip; RB_COLOR_MASK's RGB on.)
  if (tracks_ > 0.0f && light_seen_ && max_draws_ == UINT32_MAX && vs->info.ucode_hash == kTrailVertexShader &&
      (d.prim == 4 || d.prim == 5 || d.prim == 6) && tracks_draws_.size() < 64 && (st.reg(0x2104) & 7u) != 0) {
    uint64_t key = uint64_t(d.count) << 40;
    if (!d.up_vertices.empty()) {
      key ^= std::hash<std::string>{}(d.up_vertices);
    } else {
      key ^= uint64_t(first_index) * 2654435761ull ^ offsets[1];
    }
    if (tracks_seen_.insert(key).second) {
      if (!tracks_stamp_.image) {
        tracks_stamp_ = gpu_.CreateImage(kTracksW, kTracksW, VK_FORMAT_D32_SFLOAT_S8_UINT, true, "snow_tracks_stamp");
        ToGeneral(cb_, tracks_stamp_);
      }
      static const std::vector<Image*> kNoColors;
      const VkPipeline tp = PipelineFor(d, st, *vs, *ps, kNoColors, &tracks_stamp_, 0, false, false, false, true);
      if (tp) {
        CasterDraw c{};
        c.pipeline = tp;
        c.tex_sets[0] = tex_sets[0];
        c.tex_sets[1] = tex_sets[1];
        std::memcpy(c.offsets, offsets, sizeof(offsets));
        c.indexed = indexed;
        c.index_type = index_type;
        c.index_buffer = cur_index_buffer_;
        c.first = first_index;
        c.count = indexed ? index_count : d.count;
        c.sys = sys;
        // (Only where the trail shows: its texture's soft edges left out.)
        if (!Diag("tracksnoalpha")) {
          c.sys.alpha_func = 6;  // greater or equal
          c.sys.alpha_ref = 0.05f;
        }
        tracks_draws_.push_back(c);
      }
    }
  }
  // The game's own trail, drawn over the lit snow round the rider (it fades
  // a few metres back, and the game culls it farther out): flat, it covered
  // the pressed grooves near the rider (they went flat as the camera turned
  // close by). Pressed into the snow already, its colored pass is left out.
  if (tracks_ > 0.0f && tracks_valid_ && light_seen_ && max_draws_ == UINT32_MAX &&
      vs->info.ucode_hash == kTrailVertexShader && (st.reg(0x2104) & 7u) != 0 && !Diag("gametrails")) {
    return;
  }
  if (SunShadowsOn() && max_draws_ == UINT32_MAX && depth && d.up_vertices.empty() &&
      (d.prim == 4 || d.prim == 5 || d.prim == 6 || d.prim == 13)) {
    auto keep = [&](VkPipeline pl, std::vector<CasterDraw>& to, bool alpha_test) {
      CasterDraw c{};
      c.pipeline = pl;
      c.tex_sets[0] = tex_sets[0];
      c.tex_sets[1] = tex_sets[1];
      std::memcpy(c.offsets, offsets, sizeof(offsets));
      c.indexed = indexed;
      c.index_type = index_type;
      c.index_buffer = cur_index_buffer_;
      c.first = first_index;
      c.count = indexed ? index_count : d.count;
      c.sys = sys;
      // (The sun maps hold still: drawn a third at a time, every few metres,
      // trees caught mid-sway made their shadows shimmer and break up as the
      // parts were drawn again at other moments. The casters' map, drawn
      // whole every frame, sways with them.)
      if ((&to == &env_draws_ || &to == &height_draws_) && !Diag("windshadows")) c.sys.wind2[0] = 0.0f;
      // Cut-outs by alpha to coverage (MSAA) become an alpha test: the
      // maps have one sample.
      if (alpha_test && (st.reg(0x2202) & 0x10)) {
        c.sys.alpha_func = 6;  // greater or equal
        c.sys.alpha_ref = 0.5f;
      }
      to.push_back(c);
    };
    // The scene pass (before the lighting): what stays put, drawn again
    // into the sun maps. Into the world map: everything opaque that the
    // game doesn't already cast from (its shadow pass's casters: riders,
    // rails, objects), so trees, rocks, props and the terrain itself. Into
    // the height map (from above): the same without the trees (the surfaces
    // shadows lie on). Only while an update goes (the camera has moved or
    // turned enough since the last), a third of the draws each frame over
    // three frames (SunCycle).
    // (Diagnostics, "sunkeep": the world drawn into the maps once only, to
    // check that its shadows stay.)
    bool scene_pass = !light_seen_ && tiling_.width && (st.reg(0x2200) & 6) == 6 &&
                      sun_phase_ >= 0 && world_detail_ > 0 && sun_env_.image &&
                      !(Diag("sunkeep") && sun_world_drawn_ > 0);
    if (scene_pass) scene_pass = sun_phase_ == 3 || (sun_draw_seq_++ % 3) == uint32_t(sun_phase_);
    const uint64_t vh = vs->info.ucode_hash;
    static const std::vector<Image*> kNoColors;
    if (scene_pass && !shadow_caster_vs_.count(vh)) {
      const VkPipeline cp = DerivedPipeline(sun_env_pipes_, pipeline, [&] {
        return PipelineFor(d, st, *vs, *ps, kNoColors, &sun_env_, 0, false, false, false, true);
      });
      if (cp) keep(cp, env_draws_, true);
      if (Diag("sunrec")) ++sun_rec_counts_[vh];
      if (!IsShadowCaster(vh) && sun_height_.image) {
        const VkPipeline hp = DerivedPipeline(sun_height_pipes_, pipeline, [&] {
          return PipelineFor(d, st, *vs, *ps, kNoColors, &sun_height_, 0, false, false, false, true);
        });
        if (hp) keep(hp, height_draws_, true);
      }
    }
    if (!light_seen_ && (st.reg(0x2200) & 2) &&
        ShadowMapTarget(DepthKey(surface, st.reg(0x2002)))) {
      sun_depth_target_ = depth;
      sun_float_depth_ = (st.reg(0x2002) >> 16) & 1;
      sun_light_sys_ = sys;
      // Its casters' shaders (not the terrain's): the game casts those.
      if (ps->info.ucode_hash != kTerrainShadowPixelShader) shadow_caster_vs_.insert(vh);
      // The time of day: the same casters (riders, rails, objects) drawn
      // again from the hour's sun (TodLightingInputs).
      if (TodOn() && ps->info.ucode_hash != kTerrainShadowPixelShader && tod_movers_map_.image) {
        const VkPipeline mp = DerivedPipeline(tod_mover_pipes_, pipeline, [&] {
          return PipelineFor(d, st, *vs, *ps, kNoColors, &tod_movers_map_, 0, false, false, false, true);
        });
        if (mp) keep(mp, tod_movers_draws_, true);
      }
      // The light's view-projection: c0-c3 of a draw with world-space
      // vertices (found at the lighting pass among these).
      if (light_candidates_.size() < 256) {
        std::array<double, 16> m;
        for (int k = 0; k < 16; ++k) {
          float f;
          std::memcpy(&f, &st.vs_consts[k], 4);
          m[k] = f;
        }
        if (std::find(light_candidates_.begin(), light_candidates_.end(), m) ==
            light_candidates_.end()) {
          light_candidates_.push_back(m);
        }
      }
    }
  }
  mark(10);
  if (Diag("winddiag") && !light_seen_ && tiling_.width && depth && (st.reg(0x2200) & 6) == 6) {
    float c[16];
    std::memcpy(c, st.vs_consts.data(), 64);
    std::fprintf(stderr, "wind: content %016llx vs %016llx ps %016llx tex %016llx a2c %d at %d count %u caster %d c0 %.3f %.3f %.3f %.3f c3 %.3f %.3f %.3f %.3f\n",
                 (unsigned long long)DrawContentHash(d, st, 0),
                 (unsigned long long)vs->info.ucode_hash, (unsigned long long)ps->info.ucode_hash,
                 (unsigned long long)DrawTextureHash(d, st, 0),
                 int((st.reg(0x2202) >> 4) & 1), int((st.reg(0x2202) >> 3) & 1), d.count,
                 int(IsShadowCaster(vs->info.ucode_hash)), c[0], c[1], c[2], c[3], c[12], c[13], c[14], c[15]);
  }
  // Night lights on every copy of an object (SetCollectInstances): the scene
  // pass's draws with an object -> clip matrix in c0..c3, kept to be turned
  // into object -> world at the lighting pass (its view then known).
  // (Shaders that take the view-projection in c0..c3 and the object's
  // placement elsewhere: three rows at right angles, of about the same
  // length, kept too.)
  if (collect_instances_ && !light_seen_ && tiling_.width && depth && (st.reg(0x2200) & 6) == 6 &&
      max_draws_ == UINT32_MAX && pending_instances_.size() < 8192) {
    PendingInstance pi;
    uint64_t key = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; key ^= key >> 29; };
    const std::hash<std::string_view> hs;
    mix(hs(d.vs));
    mix(hs(d.index_data));
    for (const auto& [vf, name] : d.vmem) mix(hs(name));
    mix((uint64_t(d.start) << 32) | d.count);
    pi.key = key;
    for (int i = 0; i < 16; ++i) {
      float f;
      std::memcpy(&f, &st.vs_consts[i], 4);
      pi.m[i] = f;
    }
    pi.query = pick_q_;
    pi.vs = vs->info.ucode_hash;
    pi.zs = sys.ndc_scale[2];
    pi.zo = sys.ndc_offset[2];
    pi.has_world = false;
    {
      const uint32_t* c = st.vs_consts.data();
      const uint32_t last = vs->info.dynamic_constants ? std::min(vs->info.relative_constant_base, 253u) : 253u;
      auto used = [&](uint32_t r) { return (vs->info.float_constants[r >> 5] >> (r & 31)) & 1u; };
      for (uint32_t r = 4; r + 2 <= last && !pi.has_world; ++r) {
        if (!used(r) || !used(r + 1) || !used(r + 2)) continue;
        double row[3][4];
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 4; ++j) {
            float f;
            std::memcpy(&f, &c[(r + i) * 4 + j], 4);
            row[i][j] = f;
          }
        }
        // Columns of the 3 x 3 part (the axes): about equally long,
        // at right angles; the translation finite.
        double len[3];
        bool ok = true;
        for (int j = 0; j < 3 && ok; ++j) {
          len[j] = std::sqrt(row[0][j] * row[0][j] + row[1][j] * row[1][j] + row[2][j] * row[2][j]);
          ok = len[j] > 0.02 && len[j] < 200.0;
        }
        if (!ok) continue;
        for (int a = 0; a < 3 && ok; ++a) {
          for (int b = a + 1; b < 3 && ok; ++b) {
            const double dt = row[0][a] * row[0][b] + row[1][a] * row[1][b] + row[2][a] * row[2][b];
            ok = std::fabs(dt) < 0.02 * len[a] * len[b];
          }
        }
        // (Scaled unevenly too - a stretched building or panel - up to 8:1.)
        if (!ok || std::max({len[0], len[1], len[2]}) > 8.0 * std::min({len[0], len[1], len[2]})) continue;
        for (int i = 0; i < 3 && ok; ++i) ok = std::isfinite(row[i][3]) && std::fabs(row[i][3]) < 1e5;
        // (A matrix with no translation is a rotation for normals: not the placement.)
        if (!ok || std::fabs(row[0][3]) + std::fabs(row[1][3]) + std::fabs(row[2][3]) < 1e-3) continue;
        for (int i = 0; i < 3; ++i) {
          for (int j = 0; j < 4; ++j) pi.world[i * 4 + j] = row[i][j];
        }
        pi.has_world = true;
      }
    }
    pending_instances_.push_back(pi);
  }
  mark(11);
  // Speed blur: how close the nearest character is (the rider, when riding).
  if (light_seen_ && tiling_.width && depth && vs->info.dynamic_constants &&
      max_draws_ == UINT32_MAX) {
    float a[3];
    if (CharacterAnchor(*vs, st.vs_consts.data(), a)) {
      const float dx = a[0] - game_cam_pos_[0], dy = a[1] - game_cam_pos_[1],
                  dz = a[2] - game_cam_pos_[2];
      const float dist = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (dist < nearest_character_) {
        nearest_character_ = dist;
        std::memcpy(nearest_character_pos_, a, sizeof(a));
      }
    }
  }
  const uint32_t depth_ctl = st.reg(0x2200);
  // Object blur: the characters (after the lighting pass); the scenery too
  // only with scenery_blur_ (each of the scene's draws matched to last
  // frame's and drawn again: a few ms of this thread per frame; static
  // scenery moves with the camera, which the depth already gives).
  if (((object_blur_ && blur_scale_ > 0.0f && (light_seen_ || scenery_blur_)) ||
       (upscaler_ && light_seen_)) &&
      depth && tiling_.width && !vel_ready_ &&
      (depth_ctl & 6) == 6 && !ps->info.writes_depth && d.up_vertices.empty() &&
      d.kind != Draw::Kind::kIndexedUP && d.kind != Draw::Kind::kVerticesUP &&
      (d.prim == 4 || d.prim == 5 || d.prim == 6 || d.prim == 13) && max_draws_ == UINT32_MAX) {
    // The mesh: shader, index and vertex data, range.
    uint64_t key = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { key = (key ^ v) * 0x100000001B3ull; key ^= key >> 31; };
    const std::hash<std::string_view> hs;
    mix(hs(d.vs));
    mix(hs(d.index_data));
    for (const auto& [vf, name] : d.vmem) {
      mix(vf);
      mix(hs(name));
    }
    mix((uint64_t(d.start) << 32) | d.count);
    mix((uint64_t(uint32_t(d.base_vertex)) << 32) | d.prim);
    // The constants the shader reads (all of them when it indexes them), in
    // a vector reused from two frames back.
    std::vector<uint32_t> words;
    if (!words_pool_.empty()) {
      words = std::move(words_pool_.back());
      words_pool_.pop_back();
      words.clear();
    }
    const uint32_t* src = st.vs_consts.data();
    if (vs->info.dynamic_constants) {
      words.assign(src, src + 1024);
    } else {
      for (uint32_t w = 0; w < 8; ++w) {
        for (uint32_t bits = vs->info.float_constants[w]; bits; bits &= bits - 1) {
          const uint32_t c = w * 32 + uint32_t(std::countr_zero(bits));
          words.insert(words.end(), src + c * 4, src + c * 4 + 4);
        }
      }
    }
    // (Diagnostic: as if everything had moved a little since last frame.)
    if (Diag("veljitter")) {
      for (auto& w : words) {
        float f;
        std::memcpy(&f, &w, 4);
        if (std::isfinite(f) && f != 0.0f) f *= 1.0f + 1e-5f * float(frame_number_ & 1);
        std::memcpy(&w, &f, 4);
      }
    }
    auto& cur_list = vel_cur_[key];
    Shader* vvs = nullptr;
    if (prev_valid_) {
      // By the draw's shader (cheaper than its name).
      auto vit = vel_by_vs_.find(vs);
      if (vit == vel_by_vs_.end()) vit = vel_by_vs_.emplace(vs, VelocityShader(d.vs)).first;
      vvs = vit->second;
      if (!vvs) vel_by_vs_.erase(vit);  // microcode not there yet: try again later
    }
    if (vvs && vvs->error.empty() && vel_ps_.module) {
      if (depth != vel_depth_) {
        vel_draws_.clear();  // the scene's depth is the last one drawn to
        vel_depth_ = depth;
      }
      EnsureVelocityImage(depth);
      // The velocity pipeline follows from the draw's own (same shaders,
      // geometry and depth state; a fixed velocity target).
      VkPipeline vp = VK_NULL_HANDLE;
      if (auto pit = vel_pipelines_.find(pipeline); pit != vel_pipelines_.end()) {
        vp = pit->second;
      } else {
        vel_colors_.assign(1, &vel_img_);
        vp = PipelineFor(d, st, *vvs, vel_ps_, vel_colors_, depth, 15, true);
        if (vp) vel_pipelines_.emplace(pipeline, vp);
      }
      if (vp) {
        // Last frame's draw of this mesh: when it was drawn more than once
        // (copies placed by their constants), the one with the nearest
        // constants. Drawing order isn't stable (culling, sorting), so the
        // n-th copy last frame may be another tree.
        const std::vector<uint32_t>* prevw = nullptr;
        const auto match_t0 = std::chrono::steady_clock::now();
        if (auto pit = vel_prev_.find(key); pit != vel_prev_.end()) {
          const auto& cands = pit->second;
          size_t nth = cur_list.size();
          auto distance = [&](const std::vector<uint32_t>& c, float limit) {
            float sum = 0.0f;
            for (size_t k = 0; k < words.size() && sum < limit; ++k) {
              float a, b;
              std::memcpy(&a, &words[k], 4);
              std::memcpy(&b, &c[k], 4);
              const float dd = std::fabs(a - b);
              sum += std::isfinite(dd) ? dd : 0.0f;
            }
            return sum;
          };
          float best = std::numeric_limits<float>::infinity();
          // Many copies (a forest of one tree), the same-numbered one not an
          // exact match (the camera moved; drawing order shifts as copies
          // come into and out of view): a first guess from the leading
          // constants (the transform, which differs most between copies), so
          // the full comparisons below stop early.
          const bool nth_exact = !Diag("velnoexact") && nth < cands.size() && cands[nth] == words;
          // (Where the copies differ: worked out once a frame for the mesh,
          // from a few of them, at most 48 constants' words.)
          const std::vector<uint32_t>* differ = nullptr;
          if (!nth_exact && cands.size() > 4) {
            auto dit = vel_prev_differ_.find(key);
            if (dit == vel_prev_differ_.end()) {
              std::vector<uint32_t> idx;
              const auto& c0 = cands[0];
              for (size_t k = 0; k < c0.size() && idx.size() < 48; ++k) {
                for (size_t i = 1; i < cands.size() && i < 12; ++i) {
                  if (cands[i].size() == c0.size() && cands[i][k] != c0[k]) {
                    idx.push_back(uint32_t(k));
                    break;
                  }
                }
              }
              dit = vel_prev_differ_.emplace(key, std::move(idx)).first;
            }
            differ = &dit->second;
          }
          auto word_f = [](const std::vector<uint32_t>& c, uint32_t k) {
            float f;
            std::memcpy(&f, &c[k], 4);
            return std::isfinite(f) ? f : 0.0f;
          };
          if (!nth_exact && cands.size() > 16 && differ && !differ->empty()) {
            // Many copies: sorted by the constant that differs most among
            // them; the nearest found by looking either side of this one's
            // place, until that constant alone is further than the best.
            auto sit = vel_prev_sorted_.find(key);
            if (sit == vel_prev_sorted_.end()) {
              VelSorted vs_sorted;
              std::vector<std::pair<float, uint32_t>> spread;
              for (uint32_t k : *differ) {
                float lo = std::numeric_limits<float>::infinity(), hi = -lo;
                for (const auto& c : cands) {
                  if (k >= c.size()) continue;
                  const float f = word_f(c, k);
                  lo = std::min(lo, f);
                  hi = std::max(hi, f);
                }
                if (hi > lo) spread.emplace_back(hi - lo, k);
              }
              std::sort(spread.begin(), spread.end(), [](auto& a, auto& b) { return a.first > b.first; });
              for (size_t i = 0; i < spread.size() && i < 3; ++i) vs_sorted.dims[vs_sorted.n_dims++] = spread[i].second;
              if (vs_sorted.n_dims) {
                for (uint32_t i = 0; i < cands.size(); ++i) {
                  if (cands[i].size() == words.size()) vs_sorted.order.emplace_back(word_f(cands[i], vs_sorted.dims[0]), i);
                }
                std::sort(vs_sorted.order.begin(), vs_sorted.order.end());
              }
              sit = vel_prev_sorted_.emplace(key, std::move(vs_sorted)).first;
            }
            const VelSorted& so = sit->second;
            if (so.n_dims && !so.order.empty()) {
              float q[3];
              for (uint32_t d = 0; d < so.n_dims; ++d) q[d] = word_f(words, so.dims[d]);
              const auto at = std::lower_bound(so.order.begin(), so.order.end(), std::make_pair(q[0], 0u));
              float guess = std::numeric_limits<float>::infinity();
              auto consider = [&](const std::pair<float, uint32_t>& e) {
                float sum = 0.0f;
                for (uint32_t d = 0; d < so.n_dims; ++d) sum += std::fabs(word_f(cands[e.second], so.dims[d]) - q[d]);
                if (sum < guess) {
                  guess = sum;
                  nth = e.second;
                }
              };
              for (auto it = at; it != so.order.end() && std::fabs(it->first - q[0]) < guess; ++it) consider(*it);
              for (auto it = at; it != so.order.begin();) {
                --it;
                if (std::fabs(it->first - q[0]) >= guess) break;
                consider(*it);
              }
            }
          } else if (!nth_exact && cands.size() > 4) {
            float guess = std::numeric_limits<float>::infinity();
            auto probe_sum = [&](const std::vector<uint32_t>& c, size_t k) {
              float a, b;
              std::memcpy(&a, &words[k], 4);
              std::memcpy(&b, &c[k], 4);
              const float dd = std::fabs(a - b);
              return dd == dd && dd < 1e30f ? dd : 0.0f;
            };
            for (size_t i = 0; i < cands.size(); ++i) {
              if (cands[i].size() != words.size()) continue;
              float sum = 0.0f;
              if (differ && !differ->empty()) {
                for (uint32_t k : *differ) {
                  if (k < words.size()) sum += probe_sum(cands[i], k);
                }
              } else {
                const size_t probe = std::min<size_t>(words.size(), 16);
                for (size_t k = 0; k < probe; ++k) sum += probe_sum(cands[i], k);
              }
              if (sum < guess) {
                guess = sum;
                nth = i;
              }
            }
          }
          // The guess (else the same-numbered copy) first: the others then
          // stop comparing as soon as they're further.
          if (nth < cands.size() && cands[nth].size() == words.size()) {
            best = distance(cands[nth], best);
            prevw = &cands[nth];
          }
          // (Many copies: the guess from where they differ stands; the full
          // comparison only among a few.)
          for (size_t i = 0; i < cands.size() && best > 0.0f && (cands.size() <= 16 || !differ || differ->empty() || Diag("velfull")); ++i) {
            if (i == nth || cands[i].size() != words.size()) continue;
            const float dd = distance(cands[i], best);
            if (dd < best) {
              best = dd;
              prevw = &cands[i];
            }
          }
        }
        vel_match_ms_ += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - match_t0).count();
        const bool matched = prevw != nullptr;
        VelDraw v{};
        v.pipeline = vp;
        std::memcpy(v.offsets, offsets, sizeof(offsets));
        v.sys = sys;
        v.matched = matched;
        v.post_light = light_seen_;
        v.vs = vs;
        v.skinned = vs->info.dynamic_constants;
        if (light_seen_) {
          v.has_anchor = CharacterAnchor(*vs, src, v.anchor);
          vel_post_light_ = true;
        }
        if (matched) {
          uint8_t* out;
          v.offsets[2] = ReserveConstants(ConstBytes(*vs), &out);
          const std::vector<uint32_t>& pw = *prevw;
          if (vs->info.dynamic_constants) {
            std::memcpy(out, pw.data(), 4096);
          } else {
            size_t at = 0;
            for (uint32_t w = 0; w < 8; ++w) {
              for (uint32_t bits = vs->info.float_constants[w]; bits; bits &= bits - 1) {
                const uint32_t c = w * 32 + uint32_t(std::countr_zero(bits));
                std::memcpy(out + c * 16, pw.data() + at, 16);
                at += 4;
              }
            }
          }
          ++stats_.velocity_matched;
        } else {
          v.offsets[2] = offsets[1];
        }
        v.tex_sets[0] = tex_sets[0];
        v.tex_sets[1] = tex_sets[1];
        v.indexed = indexed;
        v.index_type = index_type;
        v.index_buffer = cur_index_buffer_;
        v.first = first_index;
        v.count = indexed ? index_count : d.count;
        v.scissor = sc;
        v.bias[0] = bias[0];
        v.bias[1] = bias[1];
        v.rt_w = rt_w;
        v.rt_h = rt_h;
        vel_draws_.push_back(v);
        ++stats_.velocity_draws;
      }
    }
    cur_list.push_back(std::move(words));
  }

  // Event icons aren't blurred: masked where visible. The floating icons
  // (shader pair below: meshes drawn after the characters) and their
  // labels (quads from user data written into the scene with depth, as the
  // game draws in-world text; its soft particles don't write depth).
  // By microcode hash (the stems' hex part), as the live recorder names
  // microcode by short aliases.
  const bool icon_mesh = vs->info.ucode_hash == 0x6DA245E02A477C07ull &&
                         ps->info.ucode_hash == 0x2C0355A9D2589300ull;
  const bool label = !d.up_vertices.empty() && d.prim == 13 && (depth_ctl & 6) == 6;
  if (light_seen_ && tiling_.width && ps->info.ucode_hash != kLightingPixelShader) {
    ++post_light_draws_;
    const uint32_t blend0 = st.reg(0x2201);
    const bool blended = (blend0 & 0x1FFF1FFF) != 0x00010001;
    const bool writes_depth = depth && (depth_ctl & 6) == 6;
    if (upscaler_ && blended && !writes_depth && (mask_bits & 7) && max_draws_ == UINT32_MAX) {
      QueueReactive(d, st, *vs, *ps, tex_sets, offsets, indexed, index_type, first_index,
                    index_count, sc, bias, rt_w, rt_h);
    }
  }
  // (Also with the time of day: where they are, kept clear of its lighting
  // effects (KeepIcons).)
  if ((blur_scale_ > 0.0f || TodOn()) && depth && tiling_.width && max_draws_ == UINT32_MAX &&
      (icon_mesh || label) && (depth_ctl & 6) == 6 && !ps->info.writes_depth) {
    MaskExempt(d, st, *vs, *ps, depth, tex_sets, offsets, indexed, index_type, first_index,
               index_count, sc, bias, rt_w, rt_h);
  }
}

void Renderer::Impl::QueueReactive(const Draw& d, const GpuState& st, const Shader& vs,
                                   const Shader& ps, const VkDescriptorSet* tex_sets,
                                   const uint32_t* offsets, bool indexed, VkIndexType index_type,
                                   uint32_t first, uint32_t count, const VkRect2D& sc,
                                   const float* bias, uint32_t rt_w, uint32_t rt_h) {
  // The pipeline's format and sample count only (the image comes later).
  static Image proto = [] {
    Image im;
    im.format = VK_FORMAT_R8_UNORM;
    return im;
  }();
  std::vector<Image*> colors{&proto};
  VkPipeline p = PipelineFor(d, st, vs, ps, colors, nullptr, 0x1, false, false, true);
  if (!p) return;
  MaskDraw m{};
  m.pipeline = p;
  m.tex_sets[0] = tex_sets[0];
  m.tex_sets[1] = tex_sets[1];
  std::memcpy(m.offsets, offsets, sizeof(m.offsets));
  m.indexed = indexed;
  m.index_type = index_type;
  m.index_buffer = cur_index_buffer_;
  m.first = first;
  m.count = indexed ? count : d.count;
  m.scissor = sc;
  m.bias[0] = bias[0];
  m.bias[1] = bias[1];
  m.rt_w = rt_w;
  m.rt_h = rt_h;
  reactive_draws_.push_back(m);
}

// The reactive mask for the upscale (null: no blended draws this frame or
// last). A flake's trail is where it was: FSR's history there still holds
// the flake, reprojected with the scenery's motion, while this frame shows
// scenery (reactive 0), and over textured scenery its history clamp doesn't
// catch it (over the plain sky it did). So the mask is this frame's draws
// and last frame's mask carried along the motion vectors (fading).
Image* Renderer::Impl::DrawReactive(uint32_t w, uint32_t h, Image* motion) {
  const bool had_last = reactive_hist_[0].image != nullptr;
  if (reactive_draws_.empty() && !had_last) return nullptr;
  EndRendering();
  const float zero[4] = {0, 0, 0, 0};
  auto ensure = [&](Image& im, const char* name) {
    if (im.image && im.width == w && im.height == h) return;
    if (im.image) Retire(im);
    im = gpu_.CreateImage(w, h, VK_FORMAT_R8_UNORM, false, name);
    ToGeneral(cb_, im);
    ClearImage(&im, zero, 0.0f, 0, true, false, false);
  };
  ensure(reactive_img_, "reactive");
  ensure(reactive_hist_[0], "reactive_a");
  ensure(reactive_hist_[1], "reactive_b");
  ClearImage(&reactive_img_, zero, 0.0f, 0, true, false, false);
  if (!reactive_draws_.empty()) {
    std::vector<Image*> colors{&reactive_img_};
    BeginRendering(colors, nullptr);
    DrawMasks(reactive_draws_.data(), reactive_draws_.size());
    EndRendering();
    reactive_draws_.clear();
  }
  Barrier();
  Image* out = &reactive_hist_[reactive_flip_ & 1];
  Image* last = &reactive_hist_[(reactive_flip_ + 1) & 1];
  ++reactive_flip_;
  FullscreenPass(CopyKind::kReactiveMerge, out, &reactive_img_, last, motion, false, nullptr, 0);
  dyn_ = DynState{};
  return out;
}

// The velocity image, sized and sampled like the scene's depth.
bool Renderer::Impl::EnsureVelocityImage(Image* depth) {
  if (vel_img_.image && vel_img_.width == depth->width && vel_img_.height == depth->height &&
      vel_img_.samples == depth->samples) {
    return false;
  }
  EndRendering();
  if (vel_img_.image) Retire(vel_img_);
  if (vel_single_.image) Retire(vel_single_);
  vel_img_ = gpu_.CreateImage(depth->width, depth->height, VK_FORMAT_R16G16B16A16_SFLOAT, false,
                              "velocity", 1, depth->samples);
  vel_img_.sample_scale = depth->sample_scale;
  ToGeneral(cb_, vel_img_);
  if (depth->samples > 1) {
    vel_single_ = gpu_.CreateImage(depth->width, depth->height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                   false, "velocity_1x");
    ToGeneral(cb_, vel_single_);
  }
  return true;
}

// Blur exemption (event icons): the draw again with its vertex shader, w = 1
// in the velocity image where it is the visible surface (not blurred).
void Renderer::Impl::MaskExempt(const Draw& d, const GpuState& st, const Shader& vs,
                                const Shader& ps, Image* depth, const VkDescriptorSet* tex_sets,
                                const uint32_t* offsets, bool indexed, VkIndexType index_type,
                                uint32_t first, uint32_t count, const VkRect2D& sc,
                                const float* bias, uint32_t rt_w, uint32_t rt_h) {
  EnsureVelocityImage(depth);
  if (vel_ready_ && (vel_img_.width != depth->width || vel_img_.height != depth->height ||
                     vel_img_.samples != depth->samples)) {
    return;
  }
  std::vector<Image*> colors{&vel_img_};
  VkPipeline p = PipelineFor(d, st, vs, exempt_ps_, colors, depth, 0x8, false, true);
  if (!p) return;
  MaskDraw m{};
  m.pipeline = p;
  m.tex_sets[0] = tex_sets[0];
  m.tex_sets[1] = tex_sets[1];
  std::memcpy(m.offsets, offsets, sizeof(m.offsets));
  m.indexed = indexed;
  m.index_type = index_type;
  m.index_buffer = cur_index_buffer_;
  m.first = first;
  m.count = indexed ? count : d.count;
  m.scissor = sc;
  m.bias[0] = bias[0];
  m.bias[1] = bias[1];
  m.rt_w = rt_w;
  m.rt_h = rt_h;
  if (!vel_ready_) {
    // With the velocities, against the finished scene depth.
    mask_draws_.push_back(m);
    if (!mask_depth_) mask_depth_ = depth;
    return;
  }
  BeginRendering(colors, depth);
  DrawMasks(&m, 1);
  EndRendering();  // the next draw starts its own pass (state rebound)
  vel_mask_ = true;
  vel_resolved_ = false;
}

// Mask draws into the velocity image (in a rendering scope on it).
void Renderer::Impl::DrawMasks(const MaskDraw* m, size_t n) {
  const float blend[4] = {0, 0, 0, 0};
  vk::vkCmdSetBlendConstants(cb_, blend);
  vk::vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  for (size_t k = 0; k < n; ++k) {
    const MaskDraw& d = m[k];
    vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, d.pipeline);
    VkDescriptorSet sets[4] = {set0_, set1_, d.tex_sets[0], d.tex_sets[1]};
    vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 5,
                                d.offsets);
    VkViewport vp{0, 0, float(d.rt_w), float(d.rt_h), 0.0f, 1.0f};
    vk::vkCmdSetViewport(cb_, 0, 1, &vp);
    vk::vkCmdSetScissor(cb_, 0, 1, &d.scissor);
    vk::vkCmdSetDepthBias(cb_, d.bias[0], 0.0f, d.bias[1]);
    if (d.indexed) {
      vk::vkCmdBindIndexBuffer(cb_, d.index_buffer, 0, d.index_type);
      vk::vkCmdDrawIndexed(cb_, d.count, 1, d.first, 0, 0);
    } else {
      vk::vkCmdDraw(cb_, d.count, 1, 0, 0);
    }
  }
}

// The velocity variant of a vertex shader (TranslateOptions::velocity).
Shader* Renderer::Impl::VelocityShader(const std::string& stem) {
  auto it = vel_shaders_.find(stem);
  if (it != vel_shaders_.end()) return &it->second;
  Shader& s = vel_shaders_[stem];
  auto bytes = res_->Read(stem);
  if (bytes.size() < 12) {
    vel_shaders_.erase(stem);  // may come with a later frame
    return nullptr;
  }
  std::vector<uint32_t> ucode(bytes.size() / 4);
  for (size_t i = 0; i < ucode.size(); ++i) {
    ucode[i] = (uint32_t(bytes[i * 4]) << 24) | (uint32_t(bytes[i * 4 + 1]) << 16) |
               (uint32_t(bytes[i * 4 + 2]) << 8) | bytes[i * 4 + 3];
  }
  amped3::native::TranslateOptions topt;
  topt.velocity = true;
  topt.swap_variant = Diag("swap0") ? 0 : Diag("swap2") ? 2 : Diag("swap3") ? 3 : 1;
  topt.bit_zero_mul = bit_zero_mul_;
  topt.safe_level = SafeLevel(ucode.data(), ucode.size());
  s.safe_level = topt.safe_level;
  if (s.safe_level) {
    char m[160];
    std::snprintf(m, sizeof(m), "vertex shader %016llX made plainer (level %d): it crashed the driver before",
                  (unsigned long long)amped3::native::HashMicrocode(ucode.data(), ucode.size()), s.safe_level);
    pending_notices_.push_back(m);
  }
  auto r = amped3::native::TranslateShader(ShaderStage::kVertex, ucode.data(), ucode.size(), topt);
  if (!r.ok) {
    s.error = "velocity translation failed: " + r.error;
    return &s;
  }
  s.info = r.info;
  if (!precompiling_) {
    s.recipe = 3;
    s.recipe_hash = KeepMicrocode(bytes);
  }
  std::vector<uint32_t> spirv;
  std::string error;
  if (!options_.compile || !options_.compile(false, stem + "_velocity", r.glsl, &spirv, &error)) {
    s.error = "velocity GLSL compile failed: " + error;
    log_ << s.error << "\n";
    return &s;
  }
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = spirv.size() * 4;
  ci.pCode = spirv.data();
  if (vk::vkCreateShaderModule(gpu_.device, &ci, nullptr, &s.module) != VK_SUCCESS) {
    s.error = "vkCreateShaderModule failed";
  }
  return &s;
}

// The recorded velocity copies, in one pass against the finished scene depth.
void Renderer::Impl::DrawVelocities() {
  vel_ready_ = false;
  Image* depth = vel_depth_ ? vel_depth_ : mask_depth_;
  if ((vel_draws_.empty() && mask_draws_.empty()) || !depth || !vel_img_.image) {
    vel_draws_.clear();
    mask_draws_.clear();
    return;
  }
  // The player character: the character nearest the camera, preferring the
  // one near last frame's player (another rider passing close by doesn't take
  // over). Its draws, and any character draw within a few metres of it (board,
  // limbs drawn separately), get no motion of their own.
  auto dist = [](const float* a, const float* b) {
    const float x = a[0] - b[0], y = a[1] - b[1], z = a[2] - b[2];
    return std::sqrt(x * x + y * y + z * z);
  };
  // Character draws without a matrix of their own (a board, parts drawn
  // whole) belong to the character drawn just before them: a skinned draw, or
  // one with the same shader.
  const VelDraw* last = nullptr;
  for (VelDraw& v : vel_draws_) {
    if (!v.post_light) continue;
    if (!v.has_anchor && last && (v.skinned || v.vs == last->vs)) {
      v.has_anchor = true;
      std::memcpy(v.anchor, last->anchor, sizeof(v.anchor));
    }
    last = v.has_anchor ? &v : nullptr;
  }
  const VelDraw* player = nullptr;
  float best = 1e30f;
  for (const VelDraw& v : vel_draws_) {
    if (!v.post_light || !v.has_anchor) continue;
    const float dc = dist(v.anchor, game_cam_pos_);
    if (dc > 15.0f) continue;
    float score = dc;
    if (player_known_ && dist(v.anchor, player_pos_) < 4.0f) score -= 100.0f;
    if (score < best) {
      best = score;
      player = &v;
    }
  }
  player_known_ = player != nullptr;
  if (player) std::memcpy(player_pos_, player->anchor, sizeof(player_pos_));
  player_draws_ = 0;
  for (VelDraw& v : vel_draws_) {
    // (Freecam: the game's camera moves under the draws; camera motion only.)
    if (v.matched && !fc_on_) v.sys.flags |= 1024u;  // XE_FLAG_VELOCITY: last frame's constants found
    if (v.post_light) v.sys.flags |= 4096u;  // XE_FLAG_VELOCITY_CHARACTER
    if (player && v.post_light && v.has_anchor && dist(v.anchor, player_pos_) < 2.5f) {
      v.sys.flags |= 2048u;  // XE_FLAG_VELOCITY_PLAYER
      ++player_draws_;
    }
    v.offsets[0] = WriteUniform(&v.sys, sizeof(v.sys));
  }
  stats_.velocity_player = player_draws_;

  EndRendering();
  const float zero[4] = {0, 0, 0, 0};
  ClearImage(&vel_img_, zero, 0, 0, true, false, false);
  BeginRendering({&vel_img_}, depth);
  const float blend[4] = {0, 0, 0, 0};
  vk::vkCmdSetBlendConstants(cb_, blend);
  vk::vkCmdSetStencilReference(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilCompareMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  vk::vkCmdSetStencilWriteMask(cb_, VK_STENCIL_FACE_FRONT_AND_BACK, 0);
  VkPipeline bound = VK_NULL_HANDLE;
  VkIndexType bound_type = VK_INDEX_TYPE_UINT32;
  VkBuffer bound_buffer = VK_NULL_HANDLE;
  uint32_t vw = 0, vh = 0;
  for (const VelDraw& v : vel_draws_) {
    if (v.pipeline != bound) {
      vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipeline);
      bound = v.pipeline;
    }
    VkDescriptorSet sets[4] = {set0_, set1_, v.tex_sets[0], v.tex_sets[1]};
    vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, layout_, 0, 4, sets, 5,
                                v.offsets);
    if (v.rt_w != vw || v.rt_h != vh) {
      VkViewport vp{0, 0, float(v.rt_w), float(v.rt_h), 0.0f, 1.0f};
      vk::vkCmdSetViewport(cb_, 0, 1, &vp);
      vw = v.rt_w;
      vh = v.rt_h;
    }
    vk::vkCmdSetScissor(cb_, 0, 1, &v.scissor);
    vk::vkCmdSetDepthBias(cb_, v.bias[0], 0.0f, v.bias[1]);
    if (v.indexed) {
      if (v.index_type != bound_type || v.index_buffer != bound_buffer) {
        vk::vkCmdBindIndexBuffer(cb_, v.index_buffer, 0, v.index_type);
        bound_type = v.index_type;
        bound_buffer = v.index_buffer;
      }
      vk::vkCmdDrawIndexed(cb_, v.count, 1, v.first, 0, 0);
    } else {
      vk::vkCmdDraw(cb_, v.count, 1, 0, 0);
    }
  }
  // The blur-exempt draws over the velocities.
  if (!mask_draws_.empty()) {
    DrawMasks(mask_draws_.data(), mask_draws_.size());
    vel_mask_ = true;
  }
  EndRendering();
  vel_draws_.clear();
  mask_draws_.clear();
  vel_depth_ = depth;
  Barrier();
  dyn_ = DynState{};
  vel_ready_ = true;
  vel_resolved_ = false;
}

Image Renderer::Impl::Downsample(Image* im) {
  Image tmp = gpu_.CreateImage(im->width, im->height, im->depth ? VK_FORMAT_R32_SFLOAT : im->format,
                               false, im->name + "_downsampled");
  // Its own command buffer and descriptor pool: the frame's are finished.
  VkDescriptorPool frame_pool = pool_;
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 6};  // copy_set_layout_
  VkDescriptorPoolCreateInfo dpi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  dpi.maxSets = 1;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = &size;
  vk::Check(vk::vkCreateDescriptorPool(gpu_.device, &dpi, nullptr, &pool_), "readback pool");
  cb_ = gpu_.BeginCommands();
  ToGeneral(cb_, tmp);
  Barrier();
  const uint32_t scale = std::max(im->sample_scale, 1u);
  if (im->depth) {
    CopyPass(CopyKind::kDepthMs, im, &tmp, 0, false, 0, 1, 0, 0, 0, 0, im->width, im->height);
  } else {
    CopyPass(CopyKind::kColorMs, im, &tmp, 0, false, 0, im->samples / scale * scale, 0, 0, 0, 0,
             im->width, im->height);
  }
  Barrier();
  gpu_.Submit(cb_, TakeUploads());
  cb_ = VK_NULL_HANDLE;
  vk::vkDestroyDescriptorPool(gpu_.device, pool_, nullptr);
  pool_ = frame_pool;
  return tmp;
}

bool Renderer::Impl::ReadRGBA(Image* im, std::vector<uint8_t>* out) {
  if (im->samples > 1) {
    Image tmp = Downsample(im);
    const bool ok = ReadRGBA(&tmp, out);
    gpu_.DestroyImage(tmp);
    return ok;
  }
  const uint32_t w = im->width, h = im->height;
  const bool depth = im->depth;
  size_t texel = 4;
  if (im->format == VK_FORMAT_R16G16B16A16_SFLOAT || im->format == VK_FORMAT_R16G16B16A16_SNORM ||
      im->format == VK_FORMAT_R32G32_SFLOAT) {
    texel = 8;
  }
  const VkDeviceSize bytes = VkDeviceSize(w) * h * texel;
  if (readback_.size < bytes) {
    gpu_.DestroyBuffer(readback_);
    readback_ = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  Buffer& rb = readback_;
  VkCommandBuffer cb = gpu_.BeginCommands();
  VkBufferImageCopy region{};
  region.imageSubresource = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT)
                                   : VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT),
                             0, 0, 1};
  region.imageExtent = {w, h, 1};
  vk::vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, rb.buffer, 1, &region);
  gpu_.Submit(cb);
  std::vector<uint8_t>& rgba = *out;
  rgba.resize(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    uint8_t* o = &rgba[i * 4];
    const uint8_t* s = rb.mapped + i * texel;
    uint32_t v;
    std::memcpy(&v, s, 4);
    switch (im->format) {
      case VK_FORMAT_R8G8B8A8_UNORM:
      case VK_FORMAT_R8G8B8A8_SRGB:
        std::memcpy(o, s, 4);
        break;
      case VK_FORMAT_A2B10G10R10_UNORM_PACK32:
        o[0] = uint8_t((v & 1023) >> 2);
        o[1] = uint8_t(((v >> 10) & 1023) >> 2);
        o[2] = uint8_t(((v >> 20) & 1023) >> 2);
        o[3] = uint8_t((v >> 30) * 85);
        break;
      case VK_FORMAT_R16G16B16A16_SFLOAT: {
        uint16_t c[4];
        std::memcpy(c, s, 8);
        for (int k = 0; k < 4; ++k) o[k] = ToByte(HalfToFloat(c[k]));
        break;
      }
      case VK_FORMAT_R32_SFLOAT:
      case VK_FORMAT_D32_SFLOAT_S8_UINT: {
        float f;
        std::memcpy(&f, s, 4);
        o[0] = o[1] = o[2] = ToByte(f);
        o[3] = 255;
        break;
      }
      case VK_FORMAT_D24_UNORM_S8_UINT: {
        const float f = float(v & 0xFFFFFF) / 16777215.0f;
        o[0] = o[1] = o[2] = ToByte(f);
        o[3] = 255;
        break;
      }
      default:
        std::memcpy(o, s, 4);
        break;
    }
    if (!depth && im->format != VK_FORMAT_R32_SFLOAT) o[3] = 255;  // PNGs without alpha holes
  }
  return true;
}

bool Renderer::Impl::ReadDepth(Image* im, std::vector<float>* out) {
  if (!im->depth) return false;
  if (im->samples > 1) {  // sample 0, through an R32F copy
    Image tmp = Downsample(im);
    const VkDeviceSize bytes = VkDeviceSize(tmp.width) * tmp.height * 4;
    if (readback_.size < bytes) {
      gpu_.DestroyBuffer(readback_);
      readback_ = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
    }
    VkCommandBuffer cb = gpu_.BeginCommands();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {tmp.width, tmp.height, 1};
    vk::vkCmdCopyImageToBuffer(cb, tmp.image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer, 1, &region);
    gpu_.Submit(cb);
    out->resize(size_t(tmp.width) * tmp.height);
    std::memcpy(out->data(), readback_.mapped, out->size() * 4);
    gpu_.DestroyImage(tmp);
    return true;
  }
  const uint32_t w = im->width, h = im->height;
  const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
  if (readback_.size < bytes) {
    gpu_.DestroyBuffer(readback_);
    readback_ = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  VkCommandBuffer cb = gpu_.BeginCommands();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vk::vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer, 1, &region);
  gpu_.Submit(cb);
  out->resize(size_t(w) * h);
  for (size_t i = 0; i < out->size(); ++i) {
    uint32_t v;
    std::memcpy(&v, readback_.mapped + i * 4, 4);
    if (im->format == VK_FORMAT_D24_UNORM_S8_UINT) {
      (*out)[i] = float(v & 0xFFFFFF) / 16777215.0f;
    } else {
      std::memcpy(&(*out)[i], &v, 4);
    }
  }
  return true;
}

// Stencil of a depth image as a picture: red = bit 0, green = bit 1, blue =
// the higher bits.
bool Renderer::Impl::ReadStencil(Image* im, std::vector<uint8_t>* out) {
  if (!im->depth || im->samples > 1) return false;  // (not for multisampled images yet)
  const uint32_t w = im->width, h = im->height;
  const VkDeviceSize bytes = VkDeviceSize(w) * h;
  if (readback_.size < bytes) {
    gpu_.DestroyBuffer(readback_);
    readback_ = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT);
  }
  VkCommandBuffer cb = gpu_.BeginCommands();
  VkBufferImageCopy region{};
  region.imageSubresource = {VK_IMAGE_ASPECT_STENCIL_BIT, 0, 0, 1};
  region.imageExtent = {w, h, 1};
  vk::vkCmdCopyImageToBuffer(cb, im->image, VK_IMAGE_LAYOUT_GENERAL, readback_.buffer, 1, &region);
  gpu_.Submit(cb);
  out->resize(size_t(w) * h * 4);
  for (size_t i = 0; i < size_t(w) * h; ++i) {
    const uint8_t v = readback_.mapped[i];
    uint8_t* o = &(*out)[i * 4];
    o[0] = (v & 1) ? 255 : 0;
    o[1] = (v & 2) ? 255 : 0;
    o[2] = uint8_t(v & 0xFC);
    o[3] = 255;
  }
  return true;
}


std::string Renderer::Impl::CloudSpritePath() const {
  if (!options_.cloud_sprite_path.empty()) return options_.cloud_sprite_path;
  const std::string& pc = options_.pipeline_cache_path;
  if (pc.empty()) return {};
  const size_t slash = pc.find_last_of("/\\");
  return (slash == std::string::npos ? std::string() : pc.substr(0, slash + 1)) +
         "native_cloud_sprite.bin";
}

// The file: "A3CS", width, height (little-endian 32-bit), then RGBA rows.
void Renderer::Impl::LoadCloudSprite() {
  const std::string path = CloudSpritePath();
  if (path.empty()) return;
  FILE* f = std::fopen(path.c_str(), "rb");
  if (!f) return;
  uint32_t head[3] = {};
  const size_t n = size_t(kCloudSpriteSize) * kCloudSpriteSize * 4;
  std::vector<uint8_t> rgba(n);
  const bool ok = std::fread(head, 4, 3, f) == 3 && std::memcmp(head, "A3CS", 4) == 0 &&
                  head[1] == kCloudSpriteSize && head[2] == kCloudSpriteSize &&
                  std::fread(rgba.data(), 1, n, f) == n;
  std::fclose(f);
  if (!ok) return;
  cloud_sprite_pending_ = std::move(rgba);
  cloud_sprite_found_ = true;
}

// A new texture the game uses: the cloud sprite? (128x128 DXT2/3, by a hash
// of its guest data.) Decoded to RGBA for the clouds and kept on disk.
void Renderer::Impl::FindCloudSprite(const std::string& data,
                                     const amped3::native::TextureFetch& tf) {
  if (tf.width() != kCloudSpriteSize || tf.height() != kCloudSpriteSize || tf.format() != 0x13 ||
      tf.dimension() != 1) {
    return;
  }
  const size_t n = size_t(kCloudSpriteSize / 4) * (kCloudSpriteSize / 4) * 16;
  auto bytes = res_->Read(data + ".base");
  if (bytes.size() < n) return;
  uint64_t h = 0xCBF29CE484222325ull;
  for (size_t i = 0; i < n; ++i) h = (h ^ bytes[i]) * 0x100000001B3ull;
  if (h != kCloudSpriteHash) return;
  auto levels = amped3::native::DecodeTextureMips(tf, bytes.data(), bytes.size(), nullptr, 0, false);
  if (levels.empty() || levels[0].width != kCloudSpriteSize ||
      levels[0].height != kCloudSpriteSize ||
      levels[0].rgba.size() != size_t(kCloudSpriteSize) * kCloudSpriteSize * 4) {
    return;
  }
  cloud_sprite_found_ = true;
  cloud_sprite_pending_ = std::move(levels[0].rgba);
  const std::string path = CloudSpritePath();
  if (path.empty()) return;
  const std::string tmp = path + ".tmp";
  if (FILE* f = std::fopen(tmp.c_str(), "wb")) {
    const uint32_t head[3] = {0x53433341u, kCloudSpriteSize, kCloudSpriteSize};  // "A3CS"
    const bool ok = std::fwrite(head, 4, 3, f) == 3 &&
                    std::fwrite(cloud_sprite_pending_.data(), 1, cloud_sprite_pending_.size(), f) ==
                        cloud_sprite_pending_.size();
    if (std::fclose(f) == 0 && ok) {
      std::remove(path.c_str());
      std::rename(tmp.c_str(), path.c_str());
    } else {
      std::remove(tmp.c_str());
    }
  }
}

// Into its image, recorded into cb_ (outside rendering).
void Renderer::Impl::UploadCloudSprite() {
  if (cloud_sprite_pending_.empty()) return;
  if (!cloud_sprite_.image) {
    cloud_sprite_ = gpu_.CreateTexture(kCloudSpriteSize, kCloudSpriteSize,
                                       VK_FORMAT_R8G8B8A8_UNORM, 1, "cloud_sprite",
                                       VkComponentMapping{});
  }
  VkBuffer buffer;
  VkDeviceSize offset;
  uint8_t* mapped;
  UploadSpace(cloud_sprite_pending_.size(), &buffer, &offset, &mapped);
  std::memcpy(mapped, cloud_sprite_pending_.data(), cloud_sprite_pending_.size());
  VkBufferImageCopy region{};
  region.bufferOffset = offset;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {kCloudSpriteSize, kCloudSpriteSize, 1};
  ToGeneral(cb_, cloud_sprite_);
  vk::vkCmdCopyBufferToImage(cb_, buffer, cloud_sprite_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
  Barrier();
  cloud_sprite_pending_.clear();
}

// The save notice's draws (identified by their texture; the hashes logged
// with diagnostic "noticedbg" or the A3_NOTICE_TEX list, hex).
bool Renderer::Impl::IsSaveNotice(const Draw& d, const GpuState& st) {
  // The notice's own puffs of cloud (its first draws; not the HUD's): from
  // them on, all of it (the puffs, its black box, the text). The screen's
  // dimming just before them stays under the map clouds.
  static const uint64_t kNoticeTextures[] = {0xd3694399f575a068ull};
  if (notice_started_) return true;
  const uint64_t h = DrawTextureHash(d, st, 0);
  if (Diag("noticedbg")) std::fprintf(stderr, "notice? texture %016llx count %u\n", (unsigned long long)h, d.count);
  for (uint64_t k : kNoticeTextures) {
    if (k && h == k) return notice_started_ = true;
  }
  if (static const char* env = std::getenv("A3_NOTICE_TEX"); env) {
    char hx[24];
    std::snprintf(hx, sizeof(hx), "%016llx", (unsigned long long)h);
    if (std::strstr(env, hx)) return true;
  }
  return false;
}

// The map clouds under the interface: at the first draw after the composite
// (the HUD, menus, the save notice on the way into the trail map), drawn
// into the composite's picture while they part, so what's drawn after them
// shows over them. (Closing in over the held frame: at the output, over it
// all, as before.)
void Renderer::Impl::CloudsUnderInterface() {
  auto flush_notice = [&] {
    // (Not drawn under the interface after all: the held draws as they were.)
    clouds_drawn_ = true;
    replaying_notice_ = true;
    for (const auto& nd : notice_draws_) DoDraw(*nd.first, nd.second);
    replaying_notice_ = false;
    notice_draws_.clear();
  };
  if (Diag("cloudsontop") || !map_clouds_ || hold_picture_) {
    composite_rt_ = nullptr;
    flush_notice();
    clouds_drawn_ = false;
    return;
  }
  UpdateClouds();
  if (!cloud_active_ || cloud_closing_ || !composite_rt_ || !composite_rt_->image || composite_rt_->depth) {
    flush_notice();
    clouds_drawn_ = false;
    composite_rt_ = nullptr;
    return;
  }
  Image* rt = composite_rt_;
  EndRendering();
  UploadCloudSprite();
  if (Diag("cloudsdbg")) std::fprintf(stderr, "clouds: under the interface at %.2f, %zu notice draws held\n", cloud_p_, notice_draws_.size());
  Barrier();
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &copy_set_layout_;
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &set), "clouds set");
  const bool sprite = cloud_sprite_.image != VK_NULL_HANDLE;
  VkDescriptorImageInfo ii[3] = {{point_sampler_, dummy_.sample_view, VK_IMAGE_LAYOUT_GENERAL},
                                 {point_sampler_, dummy_.sample_view, VK_IMAGE_LAYOUT_GENERAL},
                                 {linear_sampler_, sprite ? cloud_sprite_.view : dummy_.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wr.dstSet = set;
  wr.descriptorCount = 3;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wr.pImageInfo = ii;
  vk::vkUpdateDescriptorSets(gpu_.device, 1, &wr, 0, nullptr);
  const uint32_t w = rt->width, h = rt->height;
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = rt->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vk::vkCmdBeginRendering(cb_, &ri);
  vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, CopyPipeline(CopyKind::kClouds, rt->format));
  vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, clouds_layout_, 0, 1, &set, 0, nullptr);
  VkViewport v{0, 0, float(w), float(h), 0, 1};
  vk::vkCmdSetViewport(cb_, 0, 1, &v);
  VkRect2D sc{{0, 0}, {w, h}};
  vk::vkCmdSetScissor(cb_, 0, 1, &sc);
  // (As at the output: the puffs' grid, rows by the sprite, columns across.)
  const float aspect = float(w) / float(h);
  const int rows = sprite ? 10 : 7;
  const int cols = int(std::ceil(aspect / (sprite ? 0.14f : 0.2f))) + 2;
  const float cpc[8] = {cloud_p_, aspect, 1.0f / float(h), sprite ? 1.0f : 0.0f, float(cols), 0.0f, 0.0f, 0.0f};
  vk::vkCmdPushConstants(cb_, clouds_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                         sizeof(cpc), cpc);
  vk::vkCmdDraw(cb_, 6, uint32_t(rows * cols), 0, 0);
  vk::vkCmdEndRendering(cb_);
  Barrier();
  dyn_ = DynState{};
  clouds_drawn_ = true;
  // The save notice over them.
  replaying_notice_ = true;
  for (const auto& nd : notice_draws_) DoDraw(*nd.first, nd.second);
  replaying_notice_ = false;
  notice_draws_.clear();
}

// The map clouds' state at this frame's output: a cut between riding and the
// trail map (judged as EndFrame does, from this frame's characters) starts
// them, covering the screen, and they part over kCloudSeconds.
void Renderer::Impl::UpdateClouds() {
  if (clouds_updated_) return;  // (once a frame: under the interface, or at the output)
  clouds_updated_ = true;
  const auto now = std::chrono::steady_clock::now();
  if (!light_seen_) {
    // A loading screen (or anything else without the scene) for long enough:
    // what comes next is judged afresh.
    if (++cloud_dark_ >= 20) {
      cloud_after_load_ = cloud_dark_ >= 90;
      cloud_known_ = false;
      cloud_active_ = cloud_closing_ = false;
      cloud_held_valid_ = false;
    }
  } else {
    cloud_dark_ = 0;
    bool riding_now = nearest_character_ <= kRidingCameraDistance;
    if (!riding_now && rider_known_) {
      const float r[3] = {game_cam_pos_[0] - rider_pos_[0], game_cam_pos_[1] - rider_pos_[1],
                          game_cam_pos_[2] - rider_pos_[2]};
      riding_now = r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < 60.0f * 60.0f;
    }
    if (in_scene_) {
      // A cutscene: no clouds, and what follows it is judged afresh.
      cloud_known_ = false;
      cloud_active_ = cloud_closing_ = false;
      fog_anim_ = false;
      cloud_held_valid_ = false;
      cloud_after_load_ = false;
    } else if (!cloud_known_) {
      // Into a level (or the first scene): its trail map is revealed.
      cloud_known_ = true;
      cloud_riding_ = riding_now;
      cloud_settle_ = 0;
      // Only onto the trail map itself (game state 7): a far camera while
      // riding (13) is also the new game's opening shot, between its video and
      // its first cutscene.
      const bool on_map = game_state_ < 0 || game_state_ == 7;
      if (!riding_now && on_map && (!map_known_ || map_open_ || cloud_after_load_)) {
        StartCloudsParting(now);
      }
      cloud_after_load_ = false;
    } else if (riding_now != cloud_riding_) {
      cloud_riding_ = riding_now;
      if (!MapChangedRecently(now)) {
        // Not the trail map opening or closing (a restart's camera cut, say):
        // no clouds; the fog follows on its own.
        cloud_active_ = cloud_closing_ = false;
        fog_anim_ = false;
      } else if (cloud_settle_ < 10) {
        // Still settling: the state was misjudged, nothing was cut.
        cloud_active_ = cloud_closing_ = false;
        fog_anim_ = false;
      } else if (cloud_held_valid_) {
        // To or from the map: close in over the last frame before the cut
        // first (the pause menu, or the map).
        cloud_active_ = true;
        cloud_closing_ = true;
        cloud_start_ = now;
        cloud_p_ = 1.0f;
        fog_anim_ = false;
        fog_presence_ = riding_now ? 0.0f : 1.0f;
      } else {
        StartCloudsParting(now);
      }
    }
    ++cloud_settle_;
  }
  if (!cloud_known_) cloud_held_valid_ = false;
  // Diagnostics: "cloudhold" closes them halfway over the held map frame
  // once there is one (replay --repeat).
  if (Diag("cloudhold") && cloud_held_valid_) {
    cloud_active_ = true;
    cloud_closing_ = true;
    cloud_p_ = 0.5f;
    return;
  }
  // "clouds" holds them a third of the way through (replay).
  // "cloudsNN" holds them NN% through.
  for (int pct = 0; pct <= 100; pct += 5) {
    const std::string name = "clouds" + std::to_string(pct);
    if (Diag("clouds") || Diag(name.c_str())) {
      cloud_active_ = true;
      cloud_closing_ = false;
      cloud_p_ = Diag("clouds") ? 0.3f : pct / 100.0f;
      return;
    }
  }
  if (cloud_active_ && cloud_closing_) {
    const float t = std::chrono::duration<float>(now - cloud_start_).count() / kCloseSeconds;
    if (t >= 1.0f) {
      cloud_held_valid_ = false;
      StartCloudsParting(now);
    } else {
      cloud_p_ = 1.0f - t;
    }
  }
  if (cloud_active_ && !cloud_closing_) {
    cloud_p_ = std::chrono::duration<float>(now - cloud_start_).count() / kCloudSeconds;
    if (cloud_p_ >= 1.0f) {
      cloud_p_ = 1.0f;
      cloud_active_ = false;
    }
  }
}

// Records the output pass (front buffer -> gamma table -> A2B10G10R10) into
// cb_: into shared slot `slot`, or into the local output image copied to
// `readback`. Upload staging goes to staging_.
bool Renderer::Impl::RecordOutput(Image* src, const uint8_t* lut, bool share, uint32_t slot,
                                  OutputInfo* info, Buffer* readback) {
  if (!src || !src->image || !src->width || src->depth || !pool_) return false;
  if (share && (!gpu_.can_export || slot >= Renderer::kOutputSlots)) return false;
  const uint32_t w = src->width, h = src->height;
  Image* dst;
  if (share) {
    OutputSlot& os = out_slots_[slot];
    if (os.image.width != w || os.image.height != h) {
      if (os.image.image) {
        vk::vkDeviceWaitIdle(gpu_.device);
        DestroyOutputSlot(os);
      }
      os.image = gpu_.CreateExportImage(w, h, &os.handle, &os.memory_size);
      // Unique across renderers (the presenter re-imports on a new value; a
      // renderer recreated for the MSAA setting must not reuse old ones).
      static std::atomic<uint64_t> generations{0};
      os.generation = ++generations;
      out_generation_ = os.generation;
    }
    dst = &os.image;
  } else {
    if (out_local_.width != w || out_local_.height != h) {
      vk::vkDeviceWaitIdle(gpu_.device);
      gpu_.DestroyImage(out_local_);
      out_local_ = gpu_.CreateImage(w, h, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, "output");
    }
    dst = &out_local_;
  }
  EndRendering();
  if (!lut_.image) {
    lut_ = gpu_.CreateImage(256, 1, VK_FORMAT_R8G8B8A8_UNORM, false, "gamma");
    ToGeneral(cb_, lut_);
  }
  // Uploaded every frame (1 KB): the table can never go stale.
  if (lut) {
    std::memcpy(lut_data_, lut, sizeof(lut_data_));
    lut_set_ = true;
    Buffer staging = gpu_.CreateBuffer(1024, VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    for (uint32_t v = 0; v < 256; ++v) {
      staging.mapped[v * 4] = lut[v];
      staging.mapped[v * 4 + 1] = lut[256 + v];
      staging.mapped[v * 4 + 2] = lut[512 + v];
      staging.mapped[v * 4 + 3] = 255;
    }
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {256, 1, 1};
    Barrier();
    vk::vkCmdCopyBufferToImage(cb_, staging.buffer, lut_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
    staging_.push_back(staging);
  }
  // The output's old contents don't matter (and a shared image comes back
  // from the other device this way).
  ToGeneral(cb_, *dst);
  UploadCloudSprite();
  Barrier();

  // Map clouds: their state this frame; while they close in on leaving the
  // map, its last frame is shown (already through the gamma table) instead.
  if (!(hold_picture_ && light_seen_ && hold_valid_)) UpdateClouds();
  const bool clouds = map_clouds_ && cloud_active_ && !(hold_picture_ && light_seen_ && hold_valid_) &&
                      !(clouds_drawn_ && !cloud_closing_);
  if (cloud_closing_ && (!cloud_held_valid_ || cloud_held_.width != w || cloud_held_.height != h)) {
    cloud_closing_ = false;  // nothing to show: part over the riding view now
    StartCloudsParting(std::chrono::steady_clock::now());
  }
  bool held = clouds && cloud_closing_;
  if (held) {
    src = &cloud_held_;
    lut = nullptr;
  }
  // Holding the picture (loading into a mountain): the scene there, the
  // loading screen's last picture shown instead.
  const bool hold_show = hold_picture_ && light_seen_ && hold_valid_ && hold_img_.width == w && hold_img_.height == h;
  if (hold_show) {
    src = &hold_img_;
    lut = nullptr;
    held = true;
  }
  const CopyKind kind = src->samples > 1 ? CopyKind::kOutputMs : CopyKind::kOutput;
  VkPipeline p = CopyPipeline(kind, dst->format);
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &copy_set_layout_;
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &set), "output set");
  // Binding 2: the cloud sprite (for the map clouds).
  const bool sprite = cloud_sprite_.image != VK_NULL_HANDLE;
  VkDescriptorImageInfo ii[3] = {
      {point_sampler_, src->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, lut_.view, VK_IMAGE_LAYOUT_GENERAL},
      {linear_sampler_, sprite ? cloud_sprite_.view : dummy_.view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet w2{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  w2.dstSet = set;
  w2.descriptorCount = 3;
  w2.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w2.pImageInfo = ii;
  vk::vkUpdateDescriptorSets(gpu_.device, 1, &w2, 0, nullptr);
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = dst->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
  ri.renderArea = {{0, 0}, {w, h}};
  ri.layerCount = 1;
  ri.colorAttachmentCount = 1;
  ri.pColorAttachments = &att;
  vk::vkCmdBeginRendering(cb_, &ri);
  vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, p);
  vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, copy_layout_, 0, 1, &set, 0,
                              nullptr);
  const bool srgb = src->format == VK_FORMAT_R8G8B8A8_SRGB;
  int32_t pc[8] = {0, 0, lut && !held ? 1 : 0, srgb ? 1 : 0, 0, int32_t(src->samples), 0, 0};
  vk::vkCmdPushConstants(cb_, copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32, pc);
  VkViewport v{0, 0, float(w), float(h), 0, 1};
  vk::vkCmdSetViewport(cb_, 0, 1, &v);
  VkRect2D sc{{0, 0}, {w, h}};
  vk::vkCmdSetScissor(cb_, 0, 1, &sc);
  vk::vkCmdDraw(cb_, 3, 1, 0, 0);
  // Map clouds over the picture while they part (or close in).
  if (clouds) {
    vk::vkCmdBindPipeline(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                          CopyPipeline(CopyKind::kClouds, dst->format));
    vk::vkCmdBindDescriptorSets(cb_, VK_PIPELINE_BIND_POINT_GRAPHICS, clouds_layout_, 0, 1, &set,
                                0, nullptr);
    // The puffs' grid (as the shader lays it out): rows by the sprite,
    // columns across the width.
    const float aspect = float(w) / float(h);
    const int rows = sprite ? 10 : 7;
    const int cols = int(std::ceil(aspect / (sprite ? 0.14f : 0.2f))) + 2;
    const float cpc[8] = {cloud_p_, aspect, 1.0f / float(h), sprite ? 1.0f : 0.0f,
                          float(cols), 0.0f, 0.0f, 0.0f};
    vk::vkCmdPushConstants(cb_, clouds_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(cpc), cpc);
    vk::vkCmdDraw(cb_, 6, uint32_t(rows * cols), 0, 0);
  }
  vk::vkCmdEndRendering(cb_);
  dyn_ = DynState{};  // all of it: a new pass or command buffer binds nothing yet
  // Holding: the loading screen's picture kept (each frame without the scene).
  if (hold_picture_ && !light_seen_) {
    if (hold_img_.width != w || hold_img_.height != h) {
      if (hold_img_.image) Retire(hold_img_);
      hold_img_ = gpu_.CreateImage(w, h, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, "load_held");
      ToGeneral(cb_, hold_img_);
    }
    Barrier();
    VkImageCopy hr{};
    hr.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    hr.dstSubresource = hr.srcSubresource;
    hr.extent = {w, h, 1};
    vk::vkCmdCopyImage(cb_, dst->image, VK_IMAGE_LAYOUT_GENERAL, hold_img_.image, VK_IMAGE_LAYOUT_GENERAL, 1, &hr);
    Barrier();
    hold_valid_ = true;
  }
  // Clouds done: keep the frame, for closing in over it at a cut to or from
  // the map.
  if (map_clouds_ && cloud_known_ && !cloud_active_ && light_seen_) {
    if (cloud_held_.width != w || cloud_held_.height != h) {
      if (cloud_held_.image) Retire(cloud_held_);
      cloud_held_ = gpu_.CreateImage(w, h, VK_FORMAT_A2B10G10R10_UNORM_PACK32, false, "map_held");
      ToGeneral(cb_, cloud_held_);
    }
    Barrier();
    VkImageCopy cr{};
    cr.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    cr.dstSubresource = cr.srcSubresource;
    cr.extent = {w, h, 1};
    vk::vkCmdCopyImage(cb_, dst->image, VK_IMAGE_LAYOUT_GENERAL, cloud_held_.image,
                       VK_IMAGE_LAYOUT_GENERAL, 1, &cr);
    Barrier();
    cloud_held_valid_ = true;
  }

  if (share) {
    // Hand the image to the other device (queue family ownership release).
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    b.dstAccessMask = 0;
    b.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.srcQueueFamilyIndex = gpu_.family;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
    b.image = dst->image;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vk::vkCmdPipelineBarrier(cb_, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                             VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 1,
                             &b);
  } else if (readback) {
    const VkDeviceSize bytes = VkDeviceSize(w) * h * 4;
    if (readback->size < bytes) {
      gpu_.DestroyBuffer(*readback);
      *readback = gpu_.CreateBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_DST_BIT, /*cached=*/true);
    }
    Barrier();
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vk::vkCmdCopyImageToBuffer(cb_, dst->image, VK_IMAGE_LAYOUT_GENERAL, readback->buffer, 1,
                               &region);
  }
  Barrier();
  if (info) {
    *info = OutputInfo{};
    info->width = w;
    info->height = h;
    if (share) {
      const OutputSlot& os = out_slots_[slot];
      info->slot = slot;
      info->generation = os.generation;
      info->handle = os.handle;
      info->memory_size = os.memory_size;
    }
  }
  return true;
}

// Standalone output (synchronous renderers: the replayer's --output-test).
bool Renderer::Impl::DrawOutput(Image* src, const uint8_t* lut, bool share, uint32_t slot,
                                OutputInfo* info, std::vector<uint8_t>* packed) {
  if (!pool_) return false;
  cb_ = gpu_.BeginCommands();
  const bool ok = RecordOutput(src, lut, share, slot, info, share ? nullptr : &out_readback_);
  gpu_.Submit(cb_, TakeUploads());
  cb_ = VK_NULL_HANDLE;
  for (auto& b : staging_) gpu_.DestroyBuffer(b);
  staging_.clear();
  if (!ok) return false;
  if (!share && packed) {
    packed->resize(size_t(src->width) * src->height * 4);
    std::memcpy(packed->data(), out_readback_.mapped, packed->size());
  }
  return true;
}

Renderer::Impl::~Impl() {
  // (Each step named for the stall watch: a renderer that never finished
  // closing froze the picture when the upscaler was switched.)
  PhaseScope phase("closing the old renderer: its command stream");
  if (stream_.active()) stream_.End();  // a frame left unfinished (an error)
  if (!gpu_.device) return;
  g_phase = "closing the old renderer: stopping the pipeline builders";
  StopPrecompile();  // (its thread makes shader modules)
  StopPipelineBuilders();
  g_phase = "closing the old renderer: saving the pipeline cache";
  if (cache_save_.valid()) cache_save_.wait();
  if (pipelines_unsaved_) gpu_.SavePipelineCache(options_.pipeline_cache_path);
  g_phase = "closing the old renderer: waiting for the GPU (vkDeviceWaitIdle)";
  vk::vkDeviceWaitIdle(gpu_.device);
  g_phase = "closing the old renderer: the world shadows' file thread";
  if (bake_io_.thread.joinable()) bake_io_.thread.join();
  gpu_.DestroyBuffer(bake_io_.buffer);
  for (auto& r : bake_retired_) gpu_.DestroyBuffer(r.second);
  VkDevice dev = gpu_.device;
  g_phase = "closing the old renderer: the upscaler's context";
  upscaler_.reset();  // its context uses the device
  g_phase = "closing the old renderer: freeing its images and buffers";
  gpu_.DestroyImage(up_out_);
  gpu_.DestroyImage(mv_img_);
  for (auto& fs : slots_) {
    FinishSlot(fs);
    gpu_.DestroyBuffer(fs.uniforms);
    gpu_.DestroyBuffer(fs.memory);
    gpu_.DestroyBuffer(fs.indices);
    gpu_.DestroyBuffer(fs.readback);
    gpu_.DestroyBuffer(fs.upload);
    if (fs.pool) vk::vkDestroyDescriptorPool(dev, fs.pool, nullptr);
    if (fs.fence) vk::vkDestroyFence(dev, fs.fence, nullptr);
  }
  for (auto& [k, t] : textures_) gpu_.DestroyImage(t.image);
  for (auto& [k, im] : targets_) gpu_.DestroyImage(im);
  for (auto& [k, im] : resolved_) gpu_.DestroyImage(im);
  for (auto& [k, im] : resolved_packed_) gpu_.DestroyImage(im);
  for (auto& [k, sp] : spare_resolves_) gpu_.DestroyImage(sp.image);
  for (auto& im : retired_images_) gpu_.DestroyImage(im);
  for (auto& b : staging_) gpu_.DestroyBuffer(b);
  gpu_.DestroyBuffer(upload_);
  for (auto& os : out_slots_) DestroyOutputSlot(os);
  gpu_.DestroyImage(out_local_);
  gpu_.DestroyImage(lut_);
  gpu_.DestroyImage(blur_img_);
  gpu_.DestroyImage(radial_img_);
  gpu_.DestroyImage(fog_img_);
  if (eye_rb_.buffer) gpu_.DestroyBuffer(eye_rb_);
  if (center_rb_.buffer) gpu_.DestroyBuffer(center_rb_);
  if (pick_rb_.buffer) gpu_.DestroyBuffer(pick_rb_);
  for (Image* im : {&tod_lut_, &tod_white_, &tod_colors_, &tod_sky_img_, &eye_img_,
                    &tod_movers_map_, &tod_mask_, &lights_img_, &lights_out_,
                    &lights_albedo_, &lights_wdepth_, &snow_img_, &snow_albedo_, &bloom_a_, &bloom_b_, &bloom_out_, &pillar_img_, &flare_img_, &flare_tmp_, &flare_out_, &dust_out_, &fade_img_, &fade_out_, &icons_out_, &flare_vis_[0], &flare_vis_[1], &tod_mask_raw_, &tod_mask_prev_, &tracks_map_, &tracks_scratch_, &tracks_stamp_, &tracks_stamp_more_[0], &tracks_stamp_more_[1],
                    &tracks_tmp_, &tracks_albedo_, &tracks_normal_, &vol_img_, &vol_params_, &vol_out_, &cloud_map_, &spec_img_, &spec_out_, &drift_out_, &relight_params_, &tod_params_, &spec_stencil_dummy_}) {
    gpu_.DestroyImage(*im);
  }
  gpu_.DestroyImage(gi_params_);
  gpu_.DestroyImage(gi_blur_);
  gpu_.DestroyImage(gi_img_);
  for (Image& im : gi_hist_) gpu_.DestroyImage(im);
  for (Image& im : gi_probe_) gpu_.DestroyImage(im);
  gpu_.DestroyImage(gi_sun_);
  gpu_.DestroyImage(uint_dummy_);
  gpu_.DestroyImage(snow_params_);
  gpu_.DestroyImage(gi_movers_);
  for (GiCascade& g : gi_c_) {
    for (Image& im : g.maps) gpu_.DestroyImage(im);
    for (Image& im : g.occ) gpu_.DestroyImage(im);
  }
  gpu_.DestroyImage(fog_color_img_[0]);
  gpu_.DestroyImage(fog_color_img_[1]);
  gpu_.DestroyImage(ray_mask_img_);
  gpu_.DestroyImage(ray_img_);
  gpu_.DestroyImage(rays_out_img_);
  gpu_.DestroyImage(ao_img_[0]);
  gpu_.DestroyImage(ao_img_[1]);
  gpu_.DestroyImage(ao_out_img_);
  for (Image& im : ao_prep_) gpu_.DestroyImage(im);
  gpu_.DestroyImage(ao_depth_);
  gpu_.DestroyImage(ao_late_depth_);
  for (Image* im : {&sun_raw_, &sun_zo_, &sun_prep_, &sun_out_, &sun_env_,
                    &sun_height_, &sun_scratch_, &sun_seen_, &sun_seen_scratch_, &sun_env_hold_, &baked_, &bake_old_, &bake_new_,
                    &sun_tiles_, &sun_tiles2_, &movers_pos_, &movers_shade_, &movers_blur_,
                    &movers_out_}) {
    gpu_.DestroyImage(*im);
  }
  gpu_.DestroyImage(ao_before_hidden_);
  gpu_.DestroyImage(ao_after_hidden_);
  gpu_.DestroyImage(vel_img_);
  gpu_.DestroyImage(motion_img_);
  gpu_.DestroyImage(tile_rows_img_);
  gpu_.DestroyImage(player_rows_img_);
  gpu_.DestroyImage(tile_img_);
  gpu_.DestroyImage(nmax_img_);
  gpu_.DestroyImage(vel_single_);
  for (auto& [k, sh] : vel_shaders_) {
    if (sh.module) vk::vkDestroyShaderModule(dev, sh.module, nullptr);
  }
  if (vel_ps_.module) vk::vkDestroyShaderModule(dev, vel_ps_.module, nullptr);
  if (exempt_ps_.module) vk::vkDestroyShaderModule(dev, exempt_ps_.module, nullptr);
  gpu_.DestroyImage(reactive_img_);
  gpu_.DestroyImage(reactive_hist_[0]);
  gpu_.DestroyImage(reactive_hist_[1]);
  gpu_.DestroyBuffer(out_readback_);
  gpu_.DestroyImage(dummy_);
  gpu_.DestroyImage(cloud_sprite_);
  gpu_.DestroyImage(cloud_held_);
  gpu_.DestroyBuffer(uniforms_);
  gpu_.DestroyBuffer(memory_);
  gpu_.DestroyBuffer(arena_);
  gpu_.DestroyBuffer(indices_);
  gpu_.DestroyBuffer(readback_);
  gpu_.DestroyBuffer(debug_);
  for (auto& [k, p] : pipelines_) {
    if (p) vk::vkDestroyPipeline(dev, p, nullptr);
  }
  for (auto& [k, p] : copy_pipelines_) {
    if (p) vk::vkDestroyPipeline(dev, p, nullptr);
  }
  for (auto& [k, sh] : precompile_shaders_) {
    if (sh.module) vk::vkDestroyShaderModule(dev, sh.module, nullptr);
  }
  precompile_shaders_.clear();
  gpu_.DestroyImage(hold_img_);
  for (auto& [k, s] : shaders_) {
    if (s.module) vk::vkDestroyShaderModule(dev, s.module, nullptr);
  }
  for (auto& [k, s] : samplers_) vk::vkDestroySampler(dev, s, nullptr);
  if (point_sampler_) vk::vkDestroySampler(dev, point_sampler_, nullptr);
  if (linear_sampler_) vk::vkDestroySampler(dev, linear_sampler_, nullptr);
  if (copy_vs_) vk::vkDestroyShaderModule(dev, copy_vs_, nullptr);
  for (auto m : copy_fs_) {
    if (m) vk::vkDestroyShaderModule(dev, m, nullptr);
  }
  if (pool_) vk::vkDestroyDescriptorPool(dev, pool_, nullptr);
  if (tex_set_pool_) vk::vkDestroyDescriptorPool(dev, tex_set_pool_, nullptr);
  for (auto& r : retired_tex_set_pools_) vk::vkDestroyDescriptorPool(dev, r.pool, nullptr);
  if (layout_) vk::vkDestroyPipelineLayout(dev, layout_, nullptr);
  if (copy_layout_) vk::vkDestroyPipelineLayout(dev, copy_layout_, nullptr);
  if (clouds_layout_) vk::vkDestroyPipelineLayout(dev, clouds_layout_, nullptr);
  if (clouds_vs_) vk::vkDestroyShaderModule(dev, clouds_vs_, nullptr);
  for (auto& l : set_layouts_) {
    if (l) vk::vkDestroyDescriptorSetLayout(dev, l, nullptr);
  }
  if (copy_set_layout_) vk::vkDestroyDescriptorSetLayout(dev, copy_set_layout_, nullptr);
  if (ts_pool_) vk::vkDestroyQueryPool(dev, ts_pool_, nullptr);
  if (pick_pool_) vk::vkDestroyQueryPool(dev, pick_pool_, nullptr);
  g_phase = "closing the old renderer: its Vulkan device";
  gpu_.Shutdown();
}

RenderStats Renderer::Impl::Render(const Frame& frame, FrameResources& resources,
                                   uint32_t max_draws, const Renderer::OutputRequest* output,
                                   OutputInfo* info) {
  PhaseScope phase("recording the frame");
  FrameSlot* fs = nullptr;
  if (options_.async) {
    // The slot two frames back: wait for it (normally long done), then this
    // frame records into its buffers.
    fs = &slots_[next_slot_];
    next_slot_ ^= 1;
    FinishSlot(*fs);
    if (!fs->fence) {
      VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
      vk::Check(vk::vkCreateFence(gpu_.device, &fci, nullptr, &fs->fence), "vkCreateFence");
    }
    SwapSlot(*fs);
  }
  upload_used_ = 0;  // this slot's last frame has finished with it
  frame_ = &frame;
  res_ = &resources;
  stats_ = RenderStats{};
  stats_.scene_lit = light_seen_;
  stats_.tod_mask = tod_mask_ok_;
  stats_.notices = std::move(pending_notices_);
  pending_notices_.clear();
  profile_frame_ = frame_number_ % 30 == 0 || Diag("profile");
  std::fill(std::begin(draw_part_ms_), std::end(draw_part_ms_), 0.0);
  stats_.draws = uint32_t(frame.draws.size());
  log_.str({});
  last_color_ = nullptr;
  final_ = nullptr;
  tiling_ = {};
  light_seen_ = false;
  scene_begun_ = false;
  last_resolve_.clear();
  blur_done_ = false;
  composite_seen_ = false;
  clouds_updated_ = clouds_drawn_ = false;
  composite_rt_ = nullptr;
  notice_started_ = false;
  max_draws_ = max_draws;
  up_done_ = false;
  up_result_ = nullptr;
  radial_done_ = false;
  fog_done_ = false;
  snow_done_ = false;
  tod_sky_done_ = false;
  eye_done_ = false;
  lights_done_ = false;
  surfaces_on_ = !Diag("nosurfaces");
  PickRead();  // (before this frame's draws are tagged)
  ao_done_ = false;
  ao_depth_ok_ = false;
  ao_late_ok_ = false;
  ao_late_done_ = false;
  ao_hidden_state_ = 0;
  ao_before_ok_ = ao_hidden_ok_ = false;
  env_draws_.clear();
  height_draws_.clear();
  shadow_caster_vs_.clear();
  light_candidates_.clear();
  movers_done_ = false;
  movers_result_ = nullptr;
  sun_frame_ok_ = false;
  if (sun_hold_swapped_) {
    std::swap(sun_env_, sun_env_hold_);  // (the held maps were for last frame's reading only)
    sun_hold_swapped_ = false;
  }
  sun_phase_ = Diag("sunevery") ? 3 : sun_next_phase_;
  sun_next_phase_ = -1;
  sun_draw_seq_ = 0;
  sun_depth_target_ = nullptr;
  sun_light_ok_ = sun_prepared_ = sun_inputs_ok_ = sun_depth_ok_ = false;
  baked_ok_ = false;
  shadow_av_ = shadow_distance_ > 0.0f ? next_shadow_av_ : 1.0f;
  shadow_fade_ = shadow_distance_ > 0.0f ? next_shadow_fade_ : 1.0f;
  nearest_character_ = 1e9f;
  if (upscaler_) {
    ++jitter_index_;
    amped3::native::UpscaleJitter(jitter_index_, jitter_phases_, &jitter_x_, &jitter_y_);
  } else {
    jitter_x_ = jitter_y_ = 0.0f;
  }
  vel_ready_ = false;
  vel_draws_.clear();
  vel_depth_ = nullptr;
  vel_post_light_ = false;
  player_draws_ = 0;
  vel_mask_ = false;
  vel_resolved_ = false;
  mask_draws_.clear();
  mask_depth_ = nullptr;
  post_light_draws_ = 0;
  reactive_draws_.clear();
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t0 = Clock::now();
  frame_bytes_.clear();
  dyn_ = DynState{};
  CollectPipelines();
  pipeline_sync_ms_ = 0;
  if (pc_thread_.joinable()) PrecompileStep(3.0);
  Prepass();
  FreecamFrame();
  // The frame's commands: recorded on the command stream's thread (from a
  // pool of their own) while this one prepares the next draws.
  if (stream_.active()) stream_.End();  // the last frame stopped by an error
  const bool streamed = parallel_commands_;
  const VkCommandPool cb_pool = streamed ? gpu_.frame_pool : gpu_.pool;
  cb_ = gpu_.BeginCommands(cb_pool);
  if (streamed) stream_.Begin(cb_);
  // GPU time of the frame (the F3 overlay): timestamps at its start and end.
  if (fs) {
    fs->timed = false;
    if (gpu_.timestamp_bits && !ts_pool_) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = 4;
      if (vk::vkCreateQueryPool(gpu_.device, &qi, nullptr, &ts_pool_) != VK_SUCCESS) {
        ts_pool_ = VK_NULL_HANDLE;
        gpu_.timestamp_bits = 0;
      }
    }
    if (ts_pool_) {
      const uint32_t q = uint32_t(fs - slots_) * 2;
      vk::vkCmdResetQueryPool(cb_, ts_pool_, q, 2);
      vk::vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, ts_pool_, q);
      fs->timed = true;
    }
    // The parts: a timestamp at the start, then one after each part.
    fs->part_names.clear();
    if (ts_pool_ && !part_pool_) {
      VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
      qi.queryType = VK_QUERY_TYPE_TIMESTAMP;
      qi.queryCount = 2 * kMaxParts;
      if (vk::vkCreateQueryPool(gpu_.device, &qi, nullptr, &part_pool_) != VK_SUCCESS) {
        part_pool_ = VK_NULL_HANDLE;
      }
    }
    if (part_pool_ && fs->timed) {
      const uint32_t q = uint32_t(fs - slots_) * kMaxParts;
      vk::vkCmdResetQueryPool(cb_, part_pool_, q, kMaxParts);
      vk::vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, part_pool_, q);
    }
  }
  // The light editor's pick: the slot's last results (its frame is done),
  // then its queries reset for this frame.
  pick_queries_ = false;
  pick_used_ = 0;
  if (fs) {
    const uint32_t base = uint32_t(fs - slots_) * kPickMax;
    if (pick_pool_ && fs->pick_count) {
      std::vector<uint32_t> r(fs->pick_count, 0);
      if (vk::vkGetQueryPoolResults(gpu_.device, pick_pool_, base, fs->pick_count, r.size() * 4, r.data(), 4, 0) ==
          VK_SUCCESS) {
        int last = -1;
        for (int q = 0; q < int(r.size()); ++q) {
          if (r[size_t(q)]) last = q;
        }
        if (last < 0) {
          picked_state_ = 0;
        } else if (size_t(last) < fs->pick_objs.size() && fs->pick_objs[size_t(last)].key) {
          picked_ = fs->pick_objs[size_t(last)];
          picked_state_ = 1;
        } else {
          picked_state_ = 2;
          picked_ = size_t(last) < fs->pick_objs.size() ? fs->pick_objs[size_t(last)] : Renderer::Instance{};
          // (One the light editor can't place on every copy, named once.)
          if (picked_.why == 2 && size_t(last) < fs->pick_why.size() && fs->pick_why[size_t(last)] != picked_why_) {
            picked_why_ = fs->pick_why[size_t(last)];
            pending_notices_.push_back("light editor: object placement not recognised: " + picked_why_);
          }
        }
      }
    }
    fs->pick_count = 0;
    fs->pick_objs.clear();
    fs->pick_why.clear();
    if (collect_instances_ && center_probe_ && !Diag("nopick")) {
      if (!pick_pool_) {
        VkQueryPoolCreateInfo qi{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qi.queryType = VK_QUERY_TYPE_OCCLUSION;
        qi.queryCount = kPickMax * 2;
        if (vk::vkCreateQueryPool(gpu_.device, &qi, nullptr, &pick_pool_) != VK_SUCCESS) pick_pool_ = VK_NULL_HANDLE;
      }
      if (pick_pool_) {
        vk::vkCmdResetQueryPool(cb_, pick_pool_, base, kPickMax);
        pick_queries_ = true;
      }
    } else {
      picked_state_ = 0;
    }
  }
  cur_slot_ = fs;
  // Clears carried over from the last frame (render targets cleared after
  // their last use) stay waiting for this frame's first pass on them.
  if (!merge_clears_) FlushPendingClears();
  BeginFrame();
  const auto t1 = Clock::now();
  uint32_t draw_events = 0;
  for (const Event& e : frame.events) {
    switch (e.type) {
      case EventType::kDraw:
        if (draw_events++ >= max_draws) break;
        current_draw_ = draw_events;  // 1-based draw index
        DoDraw(frame.draws[e.index], e.number);
        break;
      case EventType::kClear:
        DoClear(frame.clears[e.index]);
        break;
      case EventType::kResolve:
        if (draw_events > max_draws) break;
        // (The picture after the composite resolved out: the clouds over it
        // first, the save notice over them.)
        if (composite_rt_ && !clouds_drawn_) CloudsUnderInterface();
        DoResolve(frame.resolves[e.index], e.number);
        break;
      case EventType::kBeginTiling:
        tiling_ = frame.tilings[e.index];
        tiled_scale_ = last_frame_lit_ ? scale_ : out_scale_;
        scene_begun_ = true;
        last_tiling_ = tiling_;
        break;
      case EventType::kEndTiling:
        if (!vel_ready_ && (!vel_draws_.empty() || !mask_draws_.empty())) DrawVelocities();
        tiling_ = {};
        break;
      case EventType::kSwap:
        break;
    }
  }
  if (composite_rt_ && !clouds_drawn_) CloudsUnderInterface();
  // Output: the front buffer's picture, else the last render target. Found
  // before submitting: a front buffer decoded from memory is uploaded here.
  final_front_ = max_draws == UINT32_MAX ? FrontBuffer(frame) : nullptr;
  if (final_front_) {
    const float fs = final_front_->scaled ? final_front_->scale : 1.0f;
    stats_.guest_output_width = uint32_t(std::lround(final_front_->width / fs));
    stats_.guest_output_height = uint32_t(std::lround(final_front_->height / fs));
  }
  EndRendering();
  MarkPart("after the last resolve");
  // Render targets cleared after their last use this frame keep their clear
  // for the next frame's first pass on them; the picture shown can't wait.
  FlushPendingClearsKeepTargets(final_front_, last_color_);
  FullBarrier(cb_);
  if (output && final_front_) {
    Buffer* readback = output->share ? nullptr : fs ? &fs->readback : &out_readback_;
    stats_.output_drawn =
        RecordOutput(final_front_, output->lut, output->share, output->slot, info, readback);
    EndRendering();
    MarkPart("output (post effects, upscaler, HUD)");
  }
  FlushPendingClearsKeepTargets(final_front_, last_color_);
  cur_slot_ = nullptr;
  const auto t2 = Clock::now();
  if (fs) {
    if (fs->timed) {
      vk::vkCmdWriteTimestamp(cb_, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, ts_pool_,
                              uint32_t(fs - slots_) * 2 + 1);
    }
    if (stream_.active()) stream_.End();
    fs->upload_cb = TakeUploads();
    gpu_.SubmitFenced(cb_, fs->fence, fs->upload_cb);
    fs->cb = cb_;
    fs->cb_pool = cb_pool;
    fs->pending = true;
    fs->ticket = ++next_ticket_;
    stats_.ticket = fs->ticket;
  } else {
    if (stream_.active()) stream_.End();
    gpu_.Submit(cb_, TakeUploads(), cb_pool);
  }
  const auto t3 = Clock::now();
  cb_ = VK_NULL_HANDLE;
  EndFrame();
  if (fs) SwapSlot(*fs);  // the frame's buffers stay with it until its fence
  final_ = final_front_ ? final_front_ : last_color_;
  frame_bytes_.clear();
  // The next frame's motion is measured from this one's camera (only frames
  // with the lighting pass: a menu in between starts over).
  prev_valid_ = light_seen_ && max_draws == UINT32_MAX;
  if (max_draws == UINT32_MAX) {
    last_frame_lit_ = light_seen_;
    if (light_seen_) {
      last_nearest_character_ = nearest_character_;
      // Riding: the rider close by, or (looking up or away, the rider not
      // drawn) the camera still near where they were last seen, carried
      // along by the camera's own small steps. The trail map's camera
      // jumps far away from them.
      if (nearest_character_ <= kRidingCameraDistance) {
        std::memcpy(rider_pos_, nearest_character_pos_, sizeof(rider_pos_));
        rider_known_ = true;
      } else if (rider_known_) {
        const float d[3] = {game_cam_pos_[0] - riding_prev_cam_[0],
                            game_cam_pos_[1] - riding_prev_cam_[1],
                            game_cam_pos_[2] - riding_prev_cam_[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 3.0f * 3.0f) {
          for (int i = 0; i < 3; ++i) rider_pos_[i] += d[i];
        }
      }
      const float r[3] = {game_cam_pos_[0] - rider_pos_[0], game_cam_pos_[1] - rider_pos_[1],
                          game_cam_pos_[2] - rider_pos_[2]};
      riding_ = nearest_character_ <= kRidingCameraDistance ||
                (rider_known_ && r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < 60.0f * 60.0f);
      std::memcpy(riding_prev_cam_, game_cam_pos_, sizeof(riding_prev_cam_));
    }
    stats_.nearest_character = nearest_character_;
    if (!light_seen_) {
      // No 3D scene (menus, loading): start over when it comes back.
      prev_cam_valid_ = false;
      speed_ = speed_amount_ = 0.0f;
    }
  }
  if (prev_valid_) std::memcpy(prev_inv_vp_, inv_vp_, sizeof(inv_vp_));
  // Object motion: this frame's constants are the next one's previous.
  if (max_draws == UINT32_MAX) {
    if (Diag("velstat")) {
      size_t total = 0, most = 0, meshes = 0;
      for (auto& [k, list] : vel_cur_) {
        total += list.size();
        most = std::max(most, list.size());
        ++meshes;
      }
      std::fprintf(stderr, "velstat: %zu meshes, %zu draws, most copies of one %zu; matching %.2f ms\n", meshes, total, most, vel_match_ms_);
    }
    vel_prev_.swap(vel_cur_);
    vel_prev_differ_.clear();
    vel_prev_sorted_.clear();
    vel_match_ms_ = 0.0;
    if (!prev_valid_) vel_prev_.clear();
    // Two frames back: its vectors are reused for the next frame's constants.
    if (words_pool_.size() < 16384) {
      for (auto& [k, list] : vel_cur_) {
        for (auto& w : list) words_pool_.push_back(std::move(w));
      }
    }
    vel_cur_.clear();
  }
  // New pipelines reach the disk cache within a minute (in the background:
  // reading the cache out can take a few milliseconds).
  if (pipelines_unsaved_ && !options_.pipeline_cache_path.empty() &&
      (!cache_save_.valid() ||
       cache_save_.wait_for(std::chrono::seconds(0)) == std::future_status::ready) &&
      Clock::now() - cache_saved_at_ > std::chrono::seconds(60)) {
    if (cache_save_.valid()) cache_save_.get();
    pipelines_unsaved_ = 0;
    cache_saved_at_ = Clock::now();
    cache_save_ = std::async(std::launch::async, [this] {
      return gpu_.SavePipelineCache(options_.pipeline_cache_path);
    });
  }
  stats_.upscaler = upscaler_status_;
  stats_.prepass_ms = ms(t0, t1);
  stats_.record_ms = ms(t1, t2);
  if (profile_frame_) {
    stats_.draw_parts_sampled = true;
    std::copy(std::begin(draw_part_ms_), std::end(draw_part_ms_), stats_.draw_part_ms);
  }
  stats_.gpu_wait_ms = ms(t2, t3);
  stats_.details = log_.str();
  frame_ = nullptr;
  res_ = nullptr;
  return stats_;
}

// The swapped front buffer: a resolve's image (by the destination texture
// object this frame, else by address, from any frame), or the texture decoded
// from guest memory when the CPU filled it (a disclaimer or boot screen).
Image* Renderer::Impl::FrontBuffer(const Frame& frame) {
  auto fb = resolved_object_.find(frame.swap_front_buffer);
  if (fb != resolved_object_.end()) {
    auto it = resolved_.find(fb->second);
    if (it != resolved_.end() && resolved_info_.count(fb->second) &&
        resolved_info_[fb->second].frame == frame_number_) {
      return &it->second;
    }
  }
  if (!frame.has_swap_fetch) {
    if (fb == resolved_object_.end()) return nullptr;
    auto it = resolved_.find(fb->second);
    return it != resolved_.end() ? &it->second : nullptr;
  }
  // As a texture: TextureFor finds resolves by address (dropping ones the
  // memory has changed under), else decodes the memory.
  Draw d;
  FrameTexture t;
  t.slot = 0;
  t.data = frame.swap_data;
  std::copy(frame.swap_fetch.begin(), frame.swap_fetch.end(), t.raw.begin());
  d.textures.push_back(t);
  auto st = std::make_unique<GpuState>();
  std::string note;
  Image* im = TextureFor(d, 0, *st, &note);
  if (im == &dummy_ || !im->width) return nullptr;
  return im;
}

namespace {
RenderedImage Describe(const Image* im) {
  RenderedImage r;
  if (!im) return r;
  r.width = im->width;
  r.height = im->height;
  r.name = im->name;
  r.handle = im;
  return r;
}
}  // namespace

Renderer::Renderer(RendererOptions options) : impl_(std::make_unique<Impl>(std::move(options))) {}
Renderer::~Renderer() = default;

const std::string& Renderer::device_name() const { return impl_->gpu_.device_name; }

void Renderer::SetMapClouds(bool on) { impl_->map_clouds_ = on; }
void Renderer::SetTimeOfDay(const TimeOfDay& tod) {
  auto& im = *impl_;
  im.tod_ = tod;
  // The wind on the trees: its clock (this frame's and last's, for the
  // motion vectors).
  im.wind_on_ = tod.on && tod.sway && !im.Diag("nowind");
  im.wind_vel_[0] = tod.wind[0];
  im.wind_vel_[1] = tod.wind[1];
  im.wind_prev_t_ = im.wind_t_;
  im.wind_t_ = tod.seconds;
  if (im.wind_t_ < im.wind_prev_t_ || im.wind_t_ - im.wind_prev_t_ > 0.25) im.wind_prev_t_ = im.wind_t_;
}
void Renderer::SetNightLights(const std::vector<NightLight>& lights) { impl_->lights_ = lights; }
void Renderer::StartPrecompile() {
  auto& im = *impl_;
  if (im.precompile_dir_.empty() || !im.options_.async_pipelines || im.Diag("noprecompile")) return;
  im.ReadList();
  // (Again from the list: what's not built this session yet.)
  std::lock_guard lock(im.pc_mutex_);
  im.precompile_todo_.clear();
  im.pc_ready_.clear();
  im.precompile_keys_.clear();
  im.precompile_failed_ = 0;
  im.precompile_handled_ = 0;
  for (const std::string& r : im.listed_) {
    if (im.precompiled_.insert(r).second) im.precompile_todo_.push_back(r);
  }
  im.precompile_total_ = uint32_t(im.precompile_todo_.size());
  if (!im.precompile_total_) return;
  im.pending_notices_.push_back("precompiling " + std::to_string(im.precompile_total_) +
                                " pipelines (" + im.ListPath() + ")");
  if (!im.pc_thread_.joinable()) im.pc_thread_ = std::thread([&im] { im.PrecompileMain(); });
  im.pc_cv_.notify_all();
}
void Renderer::PrecompileProgress(uint32_t* done, uint32_t* total) const {
  auto& im = *impl_;
  uint32_t finished = 0;
  {
    std::lock_guard lock(im.pc_mutex_);
    finished = im.precompile_failed_ + im.precompile_handled_;
  }
  for (const std::string& k : im.precompile_keys_) finished += im.pipelines_pending_.count(k) ? 0u : 1u;
  *total = im.precompile_total_;
  *done = std::min(finished, im.precompile_total_);
}
void Renderer::SetHoldPicture(bool hold) {
  auto& im = *impl_;
  if (hold && !im.hold_picture_) im.hold_valid_ = false;
  im.hold_picture_ = hold;
}
uint64_t Renderer::MountainKey() const {
  if (!impl_->tod_layout_.valid) return 0;
  uint64_t h = 1469598103934665603ull;
  for (float f : impl_->tod_game_light_) {
    // (Rounded: the same mountain gives the same key.)
    const int32_t q = int32_t(std::lround(double(f) * 10000.0));
    h = (h ^ uint32_t(q)) * 1099511628211ull;
  }
  return h;
}
void Renderer::SetCollectInstances(bool on) { impl_->collect_instances_ = on; }
void Renderer::SetCenterProbe(bool on) { impl_->center_probe_ = on; }
int Renderer::PickedObject(Instance* out) const {
  if (impl_->picked_state_ != 0 && out) *out = impl_->picked_;
  return impl_->picked_state_;
}
const std::vector<Renderer::Instance>& Renderer::Instances() const { return impl_->instances_; }
bool Renderer::CenterPoint(float out[3], float normal[3]) const {
  if (!impl_->center_ok_) return false;
  std::memcpy(out, impl_->center_pt_, sizeof(impl_->center_pt_));
  if (normal) {
    if (impl_->center_normal_ok_) {
      std::memcpy(normal, impl_->center_normal_, sizeof(impl_->center_normal_));
    } else {
      normal[0] = normal[2] = 0.0f;
      normal[1] = 1.0f;
    }
  }
  return true;
}
void Renderer::SetBloom(float strength) { impl_->bloom_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetCloudShadows(float darkness) { impl_->cloud_shadows_ = std::clamp(darkness, 0.0f, 1.0f); }
void Renderer::SetSnowTracks(float depth) { impl_->tracks_ = std::clamp(depth, 0.0f, 2.0f); }
void Renderer::SetDriftingSnow(bool on) { impl_->drift_ = on; }
void Renderer::SetLensFlare(float strength) { impl_->flare_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetContactShadows(float strength) { impl_->contact_ = std::clamp(strength, 0.0f, 1.0f); }
void Renderer::SetDiamondDust(float strength) { impl_->dust_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetLensDirt(float strength) { impl_->lens_dirt_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetIceHalo(float strength) { impl_->halo_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetSpecular(float strength) { impl_->specular_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetGi(float strength) { impl_->gi_ = std::clamp(strength, 0.0f, 2.0f); }
void Renderer::SetRiderShine(float shine) { impl_->rider_shine_ = std::clamp(shine, 0.0f, 2.0f); }
void Renderer::StartSurfacePick() {
  impl_->pick_state_ = 1;
  impl_->pick_frames_ = 0;
  impl_->pick_result_ = 0;
  impl_->pick_hashes_.clear();
  impl_->pick_guess_.clear();
  impl_->pick_index_.clear();
}
int Renderer::SurfacePick(uint64_t* hash, int* guess) const {
  if (impl_->pick_result_ == 1) {
    *hash = impl_->pick_hash_;
    *guess = impl_->pick_found_guess_;
  }
  return impl_->pick_result_;
}
void Renderer::SetMaterials(const std::vector<std::pair<uint64_t, int>>& list) {
  impl_->material_overrides_.clear();
  for (const auto& [h, m] : list) impl_->material_overrides_[h] = uint8_t(std::clamp(m, 0, 7));
}
void Renderer::SetVolumetric(float strength, int quality) {
  impl_->volumetric_ = std::clamp(strength, 0.0f, 2.0f);
  impl_->volumetric_quality_ = std::clamp(quality, 0, 2);
}
void Renderer::SetClothTextures(const std::vector<uint64_t>& hashes) { impl_->cloth_extra_ = hashes; }
void Renderer::SetLightShadows(int quality, int lamps) {
  impl_->light_shadows_ = std::clamp(quality, 0, 2);
  impl_->light_shadow_lamps_ = std::clamp(lamps, 0, 35);
}
void Renderer::SetEyeAdaptation(float strength) { impl_->eye_strength_ = std::clamp(strength, 0.0f, 1.0f); }
float Renderer::Exposure() const { return float(impl_->eye_strength_ > 0.0f ? impl_->exposure_ : 1.0); }
float Renderer::MountainHour() const { return impl_->tod_mountain_hour_; }
void Renderer::SetInCutscene(bool scene) { impl_->in_scene_ = scene; }
void Renderer::SetHideInterface(bool hide) { impl_->hide_interface_ = hide; }
void Renderer::SetFreecam(const FreecamInput& input) {
  // Steps (the mouse, reset) not yet used by a frame are kept and added to.
  const FreecamInput old = impl_->fc_in_;
  FreecamInput& in = impl_->fc_in_;
  in = input;
  in.reset = old.reset || input.reset;
  if (!input.set_pose && old.set_pose) {
    in.set_pose = true;
    std::memcpy(in.pose, old.pose, sizeof(in.pose));
  }
  for (int i = 0; i < 2; ++i) in.look_delta[i] += old.look_delta[i];
  for (int i = 0; i < 3; ++i) in.move_delta[i] += old.move_delta[i];
}
bool Renderer::FreecamActive() const { return impl_->fc_on_; }
bool Renderer::FreecamPose(float out[6]) const {
  if (!impl_->fc_on_) return false;
  std::memcpy(out, impl_->fc_pose_, sizeof(impl_->fc_pose_));
  return true;
}
void Renderer::SetGameState(int state) { impl_->game_state_ = state; }
void Renderer::SetTrailMapOpen(bool open) {
  if (!impl_->map_known_ || open != impl_->map_open_) {
    impl_->map_change_ = std::chrono::steady_clock::now();
  }
  impl_->map_known_ = true;
  impl_->map_open_ = open;
}

void Renderer::SetRevamped(bool on) {
  auto& im = *impl_;
  im.smooth_alpha_ = on && im.options_.smooth_alpha && !im.Diag("hardalpha");
  if (on != im.replace_on_) {
    im.replace_on_ = on;
    // (Only if there's anything to replace with.)
    if (im.replacer_) {
      im.drop_replaced_ = true;
    }
  }
}
void Renderer::SetFoliageFade(bool on) {
  impl_->foliage_fade_ = on && !impl_->Diag("nofade");
}
void Renderer::SetGodRays(float strength) { impl_->rays_strength_ = std::max(strength, 0.0f); }

void Renderer::SetWorldShadows(int detail, bool saved_only) {
  impl_->world_detail_ = std::clamp(detail, 0, 4);
  impl_->bake_saved_only_ = saved_only;
}

void Renderer::SetWorldShadowsFolder(const std::string& folder) { impl_->bake_dir_ = folder; }

void Renderer::SetShadowSoftness(float softness) {
  impl_->shadow_softness_ = std::max(softness, 0.0f);
}

void Renderer::SetShadows(float resolution, float distance) {
  impl_->shadow_res_ = resolution > 0.0f ? std::clamp(resolution, 0.5f, 4.0f) : 0.0f;
  impl_->shadow_distance_ = std::max(distance, 0.0f);
}

void Renderer::SetAmbientOcclusion(float strength, bool debug) {
  impl_->ao_strength_ = std::max(strength, 0.0f);
  impl_->ao_debug_ = debug && strength > 0.0f;
}

void Renderer::SetLightBounce(float strength) { impl_->bounce_ = std::max(strength, 0.0f); }
void Renderer::SetFog(float density, float start, float most) {
  impl_->fog_density_ = std::max(density, 0.0f);
  impl_->fog_start_ = std::max(start, 0.0f);
  impl_->fog_most_ = std::clamp(most, 0.0f, 0.98f);
}

void Renderer::SetSpeedBlur(float strength, float dt, float force_amount) {
  impl_->speed_strength_ = std::max(strength, 0.0f);
  impl_->speed_dt_ = dt;
  impl_->speed_force_ = force_amount;
  if (force_amount >= 0.0f) impl_->speed_amount_ = force_amount;
}

void Renderer::SetUpscaleParams(float frame_ms, float sharpness, bool reactive_debug) {
  impl_->reactive_debug_ = reactive_debug;
  impl_->frame_ms_ = std::clamp(frame_ms, 1.0f, 250.0f);
  impl_->up_sharpness_ = std::clamp(sharpness, 0.0f, 1.0f);
}

void Renderer::SetMotionBlur(float scale, bool camera, bool objects, bool debug) {
  impl_->blur_scale_ = (camera || objects) ? std::max(scale, 0.0f) : 0.0f;
  impl_->camera_blur_ = camera;
  impl_->object_blur_ = objects;
  impl_->blur_debug_ = debug;
  if (!objects && !impl_->upscaler_) {  // the upscaler's motion vectors use them too
    impl_->vel_prev_.clear();
    impl_->vel_cur_.clear();
  }
}

void Renderer::SetSceneryObjectBlur(bool on) { impl_->scenery_blur_ = on; }
void Renderer::SetMergeClears(bool on) { impl_->merge_clears_ = on; }
void Renderer::SetParallelCommands(bool on) { impl_->parallel_commands_ = on; }

RenderStats Renderer::Render(const Frame& frame, FrameResources& resources, uint32_t max_draws,
                             const OutputRequest* output, OutputInfo* info) {
  return impl_->Render(frame, resources, max_draws, output, info);
}

double Renderer::WaitFrame(uint64_t ticket) {
  for (auto& fs : impl_->slots_) {
    if (fs.pending && fs.ticket == ticket) {
      const auto t0 = std::chrono::steady_clock::now();
      PhaseScope phase("waiting for the frame's GPU work (its fence)");
      vk::Check(vk::vkWaitForFences(impl_->gpu_.device, 1, &fs.fence, VK_TRUE, UINT64_MAX),
                "fence");
      return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
          .count();
    }
  }
  return 0;
}

double Renderer::GpuFrameMs(uint64_t ticket) {
  auto& im = *impl_;
  for (auto& fs : im.slots_) {
    if (fs.ticket != ticket || !fs.timed || !im.ts_pool_) continue;
    uint64_t t[2] = {};
    const uint32_t q = uint32_t(&fs - im.slots_) * 2;
    if (vk::vkGetQueryPoolResults(im.gpu_.device, im.ts_pool_, q, 2, sizeof(t), t,
                                  sizeof(uint64_t), VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      return -1;
    }
    const uint32_t bits = im.gpu_.timestamp_bits;
    const uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    const uint64_t ticks = (t[1] - t[0]) & mask;
    return double(ticks) * double(im.gpu_.props.limits.timestampPeriod) * 1e-6;
  }
  return -1;
}

std::vector<GpuPart> Renderer::GpuParts(uint64_t ticket) {
  auto& im = *impl_;
  std::vector<GpuPart> parts;
  for (auto& fs : im.slots_) {
    if (fs.ticket != ticket || !fs.timed || !im.part_pool_ || fs.part_names.empty()) continue;
    const uint32_t n = uint32_t(fs.part_names.size()) + 1;
    std::vector<uint64_t> t(n);
    const uint32_t q = uint32_t(&fs - im.slots_) * Impl::kMaxParts;
    if (vk::vkGetQueryPoolResults(im.gpu_.device, im.part_pool_, q, n, n * sizeof(uint64_t),
                                  t.data(), sizeof(uint64_t),
                                  VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) {
      return {};
    }
    const uint32_t bits = im.gpu_.timestamp_bits;
    const uint64_t mask = bits >= 64 ? ~0ull : ((1ull << bits) - 1);
    const double period = double(im.gpu_.props.limits.timestampPeriod) * 1e-6;
    for (uint32_t i = 1; i < n; ++i) {
      parts.push_back({fs.part_names[i - 1], double((t[i] - t[i - 1]) & mask) * period});
    }
  }
  return parts;
}

bool Renderer::ReadOutput(uint64_t ticket, std::vector<uint8_t>* packed) {
  if (!impl_->options_.async) return false;
  for (auto& fs : impl_->slots_) {
    if (fs.ticket == ticket && fs.readback.mapped && impl_->out_local_.width) {
      const size_t bytes = size_t(impl_->out_local_.width) * impl_->out_local_.height * 4;
      if (fs.readback.size < bytes) return false;
      packed->resize(bytes);
      std::memcpy(packed->data(), fs.readback.mapped, bytes);
      return true;
    }
  }
  return false;
}

bool Renderer::CanShareOutput() const { return impl_->gpu_.can_export; }

bool Renderer::DrawOutput(const RenderedImage& src, const uint8_t* lut, bool share, uint32_t slot,
                          OutputInfo* info, std::vector<uint8_t>* packed) {
  return impl_->DrawOutput(const_cast<Image*>(static_cast<const Image*>(src.handle)), lut, share,
                           slot, info, packed);
}

RenderedImage Renderer::FinalImage() const {
  RenderedImage r = Describe(impl_->final_);
  r.front = impl_->final_ && impl_->final_ == impl_->final_front_;
  return r;
}

std::vector<RenderedImage> Renderer::AllImages() const {
  std::vector<RenderedImage> out;
  for (auto& [k, im] : impl_->targets_) out.push_back(Describe(&im));
  for (auto& [k, im] : impl_->resolved_) out.push_back(Describe(&im));
  for (auto& [k, im] : impl_->resolved_packed_) out.push_back(Describe(&im));
  if (impl_->reactive_img_.image) out.push_back(Describe(&impl_->reactive_img_));
  if (impl_->sun_out_.image) out.push_back(Describe(&impl_->sun_out_));
  if (impl_->baked_.image && impl_->Diag("dumpbake")) out.push_back(Describe(&impl_->baked_));
  if (impl_->Diag("dumptod")) {
    out.clear();
    for (auto* im : {&impl_->tod_mask_, &impl_->tod_movers_map_, &impl_->movers_shade_}) {
      if (im->image) out.push_back(Describe(im));
    }
  }
  if (impl_->Diag("dumpbake")) {
    for (auto* im : {&impl_->sun_height_, &impl_->sun_env_, &impl_->movers_shade_}) {
      if (im->image) out.push_back(Describe(im));
    }
  }
  return out;
}

bool Renderer::ReadRGBA(const RenderedImage& image, std::vector<uint8_t>* rgba) {
  if (!image.handle) return false;
  return impl_->ReadRGBA(const_cast<Image*>(static_cast<const Image*>(image.handle)), rgba);
}

bool Renderer::ReadDebugStores(std::vector<float>* values) {
  if (!impl_->debug_stores_) return false;
  const float* f = reinterpret_cast<const float*>(impl_->debug_.mapped);
  values->assign(f, f + Impl::kDebugBytes / sizeof(float));
  return true;
}

bool Renderer::ReadDepth(const RenderedImage& image, std::vector<float>* depth) {
  if (!image.handle) return false;
  return impl_->ReadDepth(const_cast<Image*>(static_cast<const Image*>(image.handle)), depth);
}

bool Renderer::ReadStencil(const RenderedImage& image, std::vector<uint8_t>* rgba) {
  if (!image.handle) return false;
  return impl_->ReadStencil(const_cast<Image*>(static_cast<const Image*>(image.handle)), rgba);
}

}  // namespace amped3::native
