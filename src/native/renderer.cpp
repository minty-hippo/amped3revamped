// The native renderer: see renderer.h. Grew out of the step 4 stage 1
// replayer (tools/replay), which is now a thin front end to this.

#include "renderer.h"

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
#include <thread>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <vector>

#include "vk.h"
#include "cmd_stream.h"
#include "xenos_shader.h"
#include "xenos_texture.h"
#include "upscaler.h"
#include "texture_replace.h"

namespace amped3::native {
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
  // A render target (GetTarget): draws never sample one directly.
  bool target = false;
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
};

class Gpu {
 public:
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physical = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkQueue queue = VK_NULL_HANDLE;
  uint32_t family = 0;
  uint32_t timestamp_bits = 0;  // the queue's valid timestamp bits (0: no timestamps)
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
    vk::Check(vk::vkQueueSubmit(queue, 1, &si, fence), "vkQueueSubmit");
  }

  void Submit(VkCommandBuffer cb, VkCommandBuffer first = VK_NULL_HANDLE,
              VkCommandPool cb_pool = VK_NULL_HANDLE) {
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
};
static_assert(offsetof(SystemConstants, tex_scale) == 80, "XeSystem std140 layout");

// --- Replayer -------------------------------------------------------------------


// Amped 3's deferred lighting pixel shader (camera motion blur reads its
// constants and textures).
constexpr uint64_t kLightingPixelShader = 0xC17CD159D811B298ull;

struct Shader {
  VkShaderModule module = VK_NULL_HANDLE;
  ShaderInfo info;
  std::string error;
  std::vector<uint8_t> vfetch;  // the vertex fetch constants it reads (info's bits as a list)
};

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
  Image* TextureFor(const Draw& draw, uint32_t fetch_constant, const GpuState& st,
                    std::string* note);
  // `lod_bias`: added to the mip level (sharper textures for a scene drawn
  // below the output resolution and upscaled).
  VkSampler SamplerFor(const uint32_t* fetch, const amped3::native::TextureBinding& b,
                       float lod_bias = 0.0f);
  VkPipeline PipelineFor(const Draw& d, const GpuState& st, const Shader& vs, const Shader& ps,
                         const std::vector<Image*>& colors, Image* depth, uint32_t color_mask_bits,
                         bool velocity = false, bool mask = false);
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
  // pass, else the output's. Screen-sized passes drawn before the scene
  // (Amped 3's depth pre-pass, about 500 draws, blurred and sampled by the
  // scene) are at the scene's scale too: the scene samples them at its own
  // resolution, so drawing them at the output's (2x the scene's each way with
  // an upscaler) only cost GPU time.
  float TargetScale(const TargetKey& k) const {
    if (tiling_.width) return tiled_scale_;
    if (!scene_begun_ && last_frame_lit_ && last_tiling_.width && k.samples == 1 &&
        k.pitch == last_tiling_.width) {
      TargetKey hk = k;
      hk.scale_id = 0;
      auto h = target_heights_.find(hk);
      if (h != target_heights_.end() && h->second == last_tiling_.height) return scale_;
    }
    return out_scale_;
  }
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
  double draw_part_ms_[9] = {};  // off with the diagnostic "noixcache"
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
    kClouds = 17
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
  bool last_pipeline_pending_ = false;
  std::unordered_set<std::string> pipelines_pending_;
  std::mutex pq_mutex_;
  std::condition_variable pq_cv_;
  std::deque<std::unique_ptr<PipelineBuild>> pq_;
  std::vector<std::pair<std::string, VkPipeline>> pq_done_;
  std::vector<std::thread> pq_threads_;
  bool pq_stop_ = false;
  void QueuePipeline(std::unique_ptr<PipelineBuild> build);
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
  VkShaderModule copy_fs_[18] = {};  // by CopyKind
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
                      bool linear0, const float* pc, uint32_t floats);
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
  float cam_pos_[3] = {};        // this frame's camera (lighting pass c4)
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
  float fog_density_ = 0.0f, fog_start_ = 150.0f;
  float fog_presence_ = 1.0f;  // 0..1: fades out without a rider close by
  float sun_dir_[3] = {};
  bool sun_valid_ = false;
  Image fog_color_img_[2];
  uint32_t fog_color_cur_ = 0;
  Image fog_img_;
  bool fog_done_ = false;
  Image* FoggedScene(Image* scene);
  // God rays (SetGodRays): strength, the light mask and its shafts (half
  // size) and the scene with them.
  float rays_strength_ = 0.0f;
  // Anti-aliased alpha test / alpha to coverage on multisampled targets
  // (RendererOptions::smooth_alpha).
  bool smooth_alpha_ = true;
  bool foliage_fade_ = true;  // RendererOptions::foliage_fade (with smooth_alpha_)
  Image* fog_result_ = nullptr;  // this frame's fogged / rayed scene
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
  std::vector<std::vector<uint32_t>> words_pool_;  // vel_prev_'s vectors, reused
  std::unordered_map<const Shader*, Shader*> vel_by_vs_;    // draw shader -> velocity shader
  std::unordered_map<VkPipeline, VkPipeline> vel_pipelines_;  // draw pipeline -> velocity one
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

uint32_t Renderer::Impl::WriteUniform(const void* data, size_t size) {
  const size_t align = std::max<size_t>(gpu_.props.limits.minUniformBufferOffsetAlignment, 16);
  uniform_used_ = (uniform_used_ + align - 1) & ~(align - 1);
  if (uniform_used_ + size > uniforms_.size) throw std::runtime_error("uniform ring full");
  std::memcpy(uniforms_.mapped + uniform_used_, data, size);
  const uint32_t offset = uint32_t(uniform_used_);
  uniform_used_ += size;
  return offset;
}

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
  auto r = amped3::native::TranslateShader(stage, ucode.data(), ucode.size(), topt);
  if (!r.ok) {
    s.error = "translation failed: " + r.error;
    return &s;
  }
  s.info = r.info;
  for (uint32_t vf = 0; vf < 96; ++vf) {
    if (s.info.vertex_fetch_constants[vf >> 5] & (1u << (vf & 31))) s.vfetch.push_back(uint8_t(vf));
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
  if (light_seen_ && !tiling_.width && tex_base == scene_base_) composite_seen_ = true;
  if ((blur || upscaler_ || radial || fog) && light_seen_ && !tiling_.width &&
      tex_base == scene_base_) {
    auto sit = resolved_.find(tex_base);
    if (sit != resolved_.end()) {
      Image* scene = &sit->second;
      if (fog) {
        if (Image* fogged = FoggedScene(scene)) scene = fogged;
      }
      if (blur) {
        if (Image* blurred = BlurredScene(scene)) scene = blurred;
      }
      if (radial) {
        if (Image* streaked = RadialScene(scene)) scene = streaked;
      }
      if (upscaler_) {
        if (Image* up = UpscaledScene(scene)) scene = up;
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
  if (replacer_ && replacer_->MayReplace(tf)) {
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
  known_textures_.insert(cached.hash);
  texture_bytes_ += total;
  ++stats_.new_textures;
  stats_.texture_mb += double(total) / (1 << 20);
  stats_.texture_ms += MsSince(t_tex);
  return &cached.image;
}

VkPipeline Renderer::Impl::PipelineFor(const Draw& d, const GpuState& st, const Shader& vs,
                                 const Shader& ps, const std::vector<Image*>& colors, Image* depth,
                                 uint32_t color_mask_bits, bool velocity, bool mask) {
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
    alpha_to_coverage = !velocity && !mask && !smooth_alpha_ && (st.reg(0x2202) & 0x10) != 0;
    put(0x4D530000u | host_samples);
    put(sample_mask);
    put(alpha_to_coverage ? 1u : 0u);
  }
  const bool diag_range = DiagRange() && !velocity;
  if (diag_range) put(0x44494147u);
  if (velocity) put(0x56454Cu);
  if (mask) put(0x4D41534Bu);
  const std::string_view key(key_buf, std::min(key_len, sizeof(key_buf)));
  last_pipeline_pending_ = false;
  auto it = pipelines_.find(key);
  if (it != pipelines_.end()) return it->second;
  if (pipelines_pending_.count(std::string(key))) {
    last_pipeline_pending_ = true;
    return VK_NULL_HANDLE;
  }

  // Everything the create info points at lives in `build`, so it can be
  // compiled on another thread.
  auto build = std::make_unique<PipelineBuild>();
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
  rs.frontFace = (mode & 4) ? VK_FRONT_FACE_CLOCKWISE : VK_FRONT_FACE_COUNTER_CLOCKWISE;
  // Polygon offset (PA_SU_SC_MODE_CNTL poly_offset_front/back_enable); the
  // values are dynamic (DoDraw).
  rs.depthBiasEnable = (mode & (3u << 11)) != 0;
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
  // Scene geometry (and the motion blur's extra passes) can wait: once this
  // frame has spent its budget building pipelines, the rest are built in the
  // background and the draw is left out until its pipeline is ready (a
  // frame or two), instead of the frame stalling on the driver's compiler.
  const bool deferrable = options_.async_pipelines && (tiling_.width != 0 || velocity || mask);
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
  if (vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &pci, nullptr, &p) !=
      VK_SUCCESS) {
    p = VK_NULL_HANDLE;
  }
  const double took = MsSince(t_pipe);
  ++stats_.new_pipelines;
  ++pipelines_unsaved_;
  stats_.pipeline_ms += took;
  pipeline_sync_ms_ += took;
  pipelines_.emplace(std::string(key), p);
  return p;
}

void Renderer::Impl::QueuePipeline(std::unique_ptr<PipelineBuild> build) {
  pipelines_pending_.insert(build->key);
  ++stats_.deferred_pipelines;
  std::lock_guard lock(pq_mutex_);
  if (pq_threads_.empty()) {
    // Two builders: drivers compile pipelines on one thread each.
    for (int i = 0; i < 2; ++i) {
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
          if (vk::vkCreateGraphicsPipelines(gpu_.device, gpu_.pipeline_cache, 1, &b->pci, nullptr,
                                            &p) != VK_SUCCESS) {
            p = VK_NULL_HANDLE;
          }
          std::lock_guard lock(pq_mutex_);
          pq_done_.emplace_back(std::move(b->key), p);
        }
      });
    }
  }
  pq_.push_back(std::move(build));
  pq_cv_.notify_one();
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
  VkDescriptorSetLayoutBinding cbnd[3] = {
      {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
      {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
  lci.bindingCount = 3;  // binding 2: motion blur only
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
  exempt_ps_.module = CompileBuiltin("builtin_exempt", R"(#version 450
layout(location = 0) out vec4 o;
void main() { o = vec4(0.0, 0.0, 0.0, 1.0); }
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
    std::vector<std::pair<uint64_t, std::string>> idle;
    for (auto it = textures_.begin(); it != textures_.end();) {
      const uint64_t unused = frame_number_ - it->second.last_used;
      if (unused > options_.texture_keep_frames) {
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
    stages[1].module = copy_fs_[int(kind)];
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

void Renderer::Impl::NoteLightingPass(const Draw& d, const GpuState& st) {
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
    // c14: the light's direction (a unit vector toward the sun).
    float l[3];
    for (uint32_t i = 0; i < 3; ++i) {
      const uint32_t bits = st.ps_consts[14 * 4 + i];
      std::memcpy(&l[i], &bits, 4);
    }
    const float len = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    sun_valid_ = std::isfinite(len) && std::fabs(len - 1.0f) < 0.05f;
    if (sun_valid_) {
      for (int i = 0; i < 3; ++i) sun_dir_[i] = l[i] / len;
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
        const float dot = sun_dir_[0] * k.light[0] + sun_dir_[1] * k.light[1] +
                          sun_dir_[2] * k.light[2];
        if (dot > 0.9995f) {
          const float n = std::sqrt(k.sun[0] * k.sun[0] + k.sun[1] * k.sun[1] + k.sun[2] * k.sun[2]);
          for (int i = 0; i < 3; ++i) sun_dir_[i] = k.sun[i] / n;
          break;
        }
      }
    }
  }
  if (speed_frame_ != frame_number_ && max_draws_ == UINT32_MAX) {
    speed_frame_ = frame_number_;
    UpdateSpeed();
  }
  amped3::native::TextureFetch scene, depth;
  std::memcpy(scene.words, FetchWords(d, 3, st), sizeof(scene.words));
  std::memcpy(depth.words, FetchWords(d, 5, st), sizeof(depth.words));
  scene_base_ = scene.base_address() & 0x1FFFFFFF;
  depth_base_ = depth.base_address() & 0x1FFFFFFF;
  light_seen_ = true;
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

Image* Renderer::Impl::FoggedScene(Image* scene) {
  if (fog_done_) return fog_result_;
  fog_done_ = true;
  fog_result_ = nullptr;
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
      pc[23] = 1.0f;
    }
    // Density 1: about half fogged 2 km away level with the camera; thinner
    // higher up (halving every ~230 m), up to twice as thick in the valleys.
    pc[24] = 0.0004f * fog_density_ * fog_presence_;
    pc[25] = fog_start_;
    pc[26] = 0.003f;
    pc[27] = cam_pos_[1] - 50.0f;
    pc[28] = 0.75f;  // never opaque: far mountains stay visible
    pc[29] = 1.0f / float(w);
    pc[30] = 1.0f / float(h);
    FullscreenPass(CopyKind::kFog, &fog_img_, scene, depth, &fog_color_img_[cur], true, pc, 32);
    Barrier();
    out = &fog_img_;
    ++stats_.fogged;
  }
  // 3: god rays, from the sky around the sun while it's in (or near) view.
  if (Diag("sun")) {
    std::fprintf(stderr, "sun: valid %d dir (%.3f %.3f %.3f) uv (%.3f %.3f) visible %.2f cam (%.1f %.1f %.1f)\n",
                 sun_valid_ ? 1 : 0, sun_dir_[0], sun_dir_[1], sun_dir_[2], sun_uv[0], sun_uv[1],
                 sun_visible, cam_pos_[0], cam_pos_[1], cam_pos_[2]);
  }
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
  // The upscaler records into the command buffer itself: everything before
  // it first.
  if (stream_.active()) stream_.Drain();
  const bool ok = upscaler_->Dispatch(cb_, in, &error);
  Transition(*src, ro, gen);
  Transition(*depth, ro, gen);
  Transition(mv_img_, ro, gen);
  Barrier();
  dyn_ = DynState{};
  if (!ok) {
    upscaler_status_ = error;
    vk::vkDeviceWaitIdle(gpu_.device);
    upscaler_.reset();
    return nullptr;
  }
  up_reset_ = false;
  up_result_ = &up_out_;
  return up_result_;
}

// A full-screen pass of a copy shader into `dst` (bindings 0..2 = b0..b2,
// missing ones the dummy image; binding 0 filtered when `linear0`).
void Renderer::Impl::FullscreenPass(CopyKind kind, Image* dst, Image* b0, Image* b1, Image* b2,
                                    bool linear0, const float* pc, uint32_t floats) {
  FlushPendingClears();  // what it reads or writes may have a clear waiting
  VkPipeline p = CopyPipeline(kind, dst->format);
  VkDescriptorSet set;
  VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  ai.descriptorPool = pool_;
  ai.descriptorSetCount = 1;
  ai.pSetLayouts = &copy_set_layout_;
  vk::Check(vk::vkAllocateDescriptorSets(gpu_.device, &ai, &set), "fullscreen pass set");
  VkDescriptorImageInfo ii[3] = {
      {linear0 ? linear_sampler_ : point_sampler_, (b0 ? b0 : &dummy_)->sample_view,
       VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b1 ? b1 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL},
      {point_sampler_, (b2 ? b2 : &dummy_)->sample_view, VK_IMAGE_LAYOUT_GENERAL}};
  VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  wr.dstSet = set;
  wr.descriptorCount = 3;
  wr.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  wr.pImageInfo = ii;
  vk::vkUpdateDescriptorSets(gpu_.device, 1, &wr, 0, nullptr);
  VkRenderingAttachmentInfo att{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
  att.imageView = dst->view;
  att.imageLayout = VK_IMAGE_LAYOUT_GENERAL;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
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
  if (floats) {
    vk::vkCmdPushConstants(cb_, copy_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, floats * 4, pc);
  }
  VkViewport v{0, 0, float(dst->width), float(dst->height), 0, 1};
  vk::vkCmdSetViewport(cb_, 0, 1, &v);
  VkRect2D sc{{0, 0}, {dst->width, dst->height}};
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
  MarkPart(src->name);  // GPU time by part: what drew into it
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
  if (w && h) {
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
  if (hide_interface_ && composite_seen_) return;  // the HUD / menus, left out
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
  if (ps->info.ucode_hash == kLightingPixelShader && !light_seen_) NoteLightingPass(d, st);
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
      tex_scales[(b.binding & 15) + (stage ? 16 : 0)] = im->scaled ? im->scale : 1.0f;
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
  if (att0->samples > 1 && smooth_alpha_ &&
      ((colorctl & 0x10) || ((colorctl & 8) && ((colorctl & 7) == 4 || (colorctl & 7) == 6)))) {
    sys.flags |= 8192u | (std::min(att0->samples, 31u) << 16) | (((colorctl >> 24) & 0xFF) << 21);
    if (colorctl & 0x10) sys.flags |= 256u;  // alpha to mask
    // Scene foliage near the camera and toward the middle of the screen
    // fades out (XE_FLAG_CAMERA_FADE) so the rider and the way ahead stay
    // in view. Only alpha-to-coverage surfaces: the terrain and riders use a
    // plain alpha test.
    if (foliage_fade_ && tiling_.width && (colorctl & 0x10)) sys.flags |= 1u << 29;
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
  // Uniform blocks: each written only when it differs from what an earlier
  // draw this frame already uploaded.
  uint32_t offsets[5];
  if (last_sys_offset_ == UINT32_MAX || std::memcmp(&sys, &last_sys_, sizeof(sys)) != 0) {
    last_sys_ = sys;
    last_sys_offset_ = WriteUniform(&sys, sizeof(sys));
  }
  offsets[0] = last_sys_offset_;
  offsets[1] = ConstantsFor(0, st.vs_consts, *vs);
  offsets[2] = ConstantsFor(1, st.ps_consts, *ps);
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
  ds.valid = true;
  if (!colors.empty()) last_color_ = colors[0];
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
  // Speed blur: how close the nearest character is (the rider, when riding).
  if (light_seen_ && tiling_.width && depth && vs->info.dynamic_constants &&
      max_draws_ == UINT32_MAX) {
    float a[3];
    if (CharacterAnchor(*vs, st.vs_consts.data(), a)) {
      const float dx = a[0] - cam_pos_[0], dy = a[1] - cam_pos_[1], dz = a[2] - cam_pos_[2];
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
          const bool nth_exact = nth < cands.size() && cands[nth] == words;
          if (!nth_exact && cands.size() > 4) {
            const size_t probe = std::min<size_t>(words.size(), 16);
            float guess = std::numeric_limits<float>::infinity();
            for (size_t i = 0; i < cands.size(); ++i) {
              if (cands[i].size() != words.size()) continue;
              float sum = 0.0f;
              for (size_t k = 0; k < probe; ++k) {
                float a, b;
                std::memcpy(&a, &words[k], 4);
                std::memcpy(&b, &cands[i][k], 4);
                const float dd = std::fabs(a - b);
                sum += dd == dd && dd < 1e30f ? dd : 0.0f;
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
          for (size_t i = 0; i < cands.size() && best > 0.0f; ++i) {
            if (i == nth || cands[i].size() != words.size()) continue;
            const float dd = distance(cands[i], best);
            if (dd < best) {
              best = dd;
              prevw = &cands[i];
            }
          }
        }
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
  }
  if (blur_scale_ > 0.0f && depth && tiling_.width && max_draws_ == UINT32_MAX &&
      (icon_mesh || label) && (depth_ctl & 6) == 6 && !ps->info.writes_depth) {
    MaskExempt(d, st, *vs, *ps, depth, tex_sets, offsets, indexed, index_type, first_index,
               index_count, sc, bias, rt_w, rt_h);
  }
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
  auto r = amped3::native::TranslateShader(ShaderStage::kVertex, ucode.data(), ucode.size(), topt);
  if (!r.ok) {
    s.error = "velocity translation failed: " + r.error;
    return &s;
  }
  s.info = r.info;
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
    const float dc = dist(v.anchor, cam_pos_);
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
    if (v.matched) v.sys.flags |= 1024u;  // XE_FLAG_VELOCITY: last frame's constants found
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
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3};  // copy_set_layout_
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

// The map clouds' state at this frame's output: a cut between riding and the
// trail map (judged as EndFrame does, from this frame's characters) starts
// them, covering the screen, and they part over kCloudSeconds.
void Renderer::Impl::UpdateClouds() {
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
      const float r[3] = {cam_pos_[0] - rider_pos_[0], cam_pos_[1] - rider_pos_[1],
                          cam_pos_[2] - rider_pos_[2]};
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
  UpdateClouds();
  const bool clouds = map_clouds_ && cloud_active_;
  if (cloud_closing_ && (!cloud_held_valid_ || cloud_held_.width != w || cloud_held_.height != h)) {
    cloud_closing_ = false;  // nothing to show: part over the riding view now
    StartCloudsParting(std::chrono::steady_clock::now());
  }
  const bool held = clouds && cloud_closing_;
  if (held) {
    src = &cloud_held_;
    lut = nullptr;
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
  if (stream_.active()) stream_.End();  // a frame left unfinished (an error)
  if (!gpu_.device) return;
  StopPipelineBuilders();
  if (cache_save_.valid()) cache_save_.wait();
  if (pipelines_unsaved_) gpu_.SavePipelineCache(options_.pipeline_cache_path);
  vk::vkDeviceWaitIdle(gpu_.device);
  VkDevice dev = gpu_.device;
  upscaler_.reset();  // its context uses the device
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
  gpu_.DestroyImage(fog_color_img_[0]);
  gpu_.DestroyImage(fog_color_img_[1]);
  gpu_.DestroyImage(ray_mask_img_);
  gpu_.DestroyImage(ray_img_);
  gpu_.DestroyImage(rays_out_img_);
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
  gpu_.Shutdown();
}

RenderStats Renderer::Impl::Render(const Frame& frame, FrameResources& resources,
                                   uint32_t max_draws, const Renderer::OutputRequest* output,
                                   OutputInfo* info) {
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
  blur_done_ = false;
  composite_seen_ = false;
  max_draws_ = max_draws;
  up_done_ = false;
  up_result_ = nullptr;
  radial_done_ = false;
  fog_done_ = false;
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
  using Clock = std::chrono::steady_clock;
  auto ms = [](Clock::time_point a, Clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
  };
  const auto t0 = Clock::now();
  frame_bytes_.clear();
  dyn_ = DynState{};
  CollectPipelines();
  pipeline_sync_ms_ = 0;
  Prepass();
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
        const float d[3] = {cam_pos_[0] - riding_prev_cam_[0], cam_pos_[1] - riding_prev_cam_[1],
                            cam_pos_[2] - riding_prev_cam_[2]};
        if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] < 3.0f * 3.0f) {
          for (int i = 0; i < 3; ++i) rider_pos_[i] += d[i];
        }
      }
      const float r[3] = {cam_pos_[0] - rider_pos_[0], cam_pos_[1] - rider_pos_[1],
                          cam_pos_[2] - rider_pos_[2]};
      riding_ = nearest_character_ <= kRidingCameraDistance ||
                (rider_known_ && r[0] * r[0] + r[1] * r[1] + r[2] * r[2] < 60.0f * 60.0f);
      std::memcpy(riding_prev_cam_, cam_pos_, sizeof(riding_prev_cam_));
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
    vel_prev_.swap(vel_cur_);
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
void Renderer::SetInCutscene(bool scene) { impl_->in_scene_ = scene; }
void Renderer::SetHideInterface(bool hide) { impl_->hide_interface_ = hide; }
void Renderer::SetGameState(int state) { impl_->game_state_ = state; }
void Renderer::SetTrailMapOpen(bool open) {
  if (!impl_->map_known_ || open != impl_->map_open_) {
    impl_->map_change_ = std::chrono::steady_clock::now();
  }
  impl_->map_known_ = true;
  impl_->map_open_ = open;
}

void Renderer::SetFoliageFade(bool on) {
  impl_->foliage_fade_ = on && !impl_->Diag("nofade");
}
void Renderer::SetGodRays(float strength) { impl_->rays_strength_ = std::max(strength, 0.0f); }

void Renderer::SetFog(float density, float start) {
  impl_->fog_density_ = std::max(density, 0.0f);
  impl_->fog_start_ = std::max(start, 0.0f);
}

void Renderer::SetSpeedBlur(float strength, float dt, float force_amount) {
  impl_->speed_strength_ = std::max(strength, 0.0f);
  impl_->speed_dt_ = dt;
  impl_->speed_force_ = force_amount;
  if (force_amount >= 0.0f) impl_->speed_amount_ = force_amount;
}

void Renderer::SetUpscaleParams(float frame_ms, float sharpness) {
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
