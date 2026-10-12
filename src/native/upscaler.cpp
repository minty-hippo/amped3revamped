// Temporal upscalers (upscaler.h): XeSS and FSR 3.1 through their runtime
// libraries, loaded on demand.

#include "upscaler.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

#if !defined(_WIN32) && !defined(__declspec)
#define __declspec(x)  // the FidelityFX header assumes Windows (Linux replayer build)
#endif
#include <ffx_api/ffx_api.h>
#include <ffx_api/ffx_upscale.h>
#include <ffx_api/vk/ffx_api_vk.h>
// XeSS's headers are Intel's (not redistributable with this project's
// sources): setup.ps1 takes them from the XeSS SDK download into
// third_party/xess. Without them XeSS is left out of the build.
#if __has_include(<xess/xess_vk.h>)
#include <xess/xess_vk.h>
#define AMPED3_HAVE_XESS 1
#else
#define AMPED3_HAVE_XESS 0
#endif

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace amped3::native {
namespace {

// --- Library loading ---------------------------------------------------------

struct Library {
  void* handle = nullptr;
  bool Open(const char* name) {
#ifdef _WIN32
    handle = reinterpret_cast<void*>(LoadLibraryA(name));
#else
    (void)name;  // the vendors ship Windows libraries only
    handle = nullptr;
#endif
    return handle != nullptr;
  }
  template <typename T>
  bool Get(const char* name, T* out) const {
#ifdef _WIN32
    *out = reinterpret_cast<T>(
        reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name)));
#else
    *out = reinterpret_cast<T>(dlsym(handle, name));
#endif
    return *out != nullptr;
  }
  ~Library() {
#ifdef _WIN32
    if (handle) FreeLibrary(static_cast<HMODULE>(handle));
#else
    if (handle) dlclose(handle);
#endif
  }
};

uint32_t HaltonBits(uint32_t index, uint32_t base, float* out) {
  float f = 1.0f, r = 0.0f;
  for (uint32_t i = index; i > 0; i /= base) {
    f /= float(base);
    r += f * float(i % base);
  }
  *out = r;
  return 0;
}

// --- XeSS ----------------------------------------------------------------------

#if AMPED3_HAVE_XESS
class Xess final : public Upscaler {
 public:
  bool Load(std::string* error) {
    if (!lib_.Open("libxess.dll")) {
      *error = "libxess.dll not found (run setup.ps1 to fetch the XeSS runtime)";
      return false;
    }
    bool ok = lib_.Get("xessGetVersion", &get_version_) &&
              lib_.Get("xessVKGetRequiredInstanceExtensions", &instance_ext_) &&
              lib_.Get("xessVKGetRequiredDeviceExtensions", &device_ext_) &&
              lib_.Get("xessVKGetRequiredDeviceFeatures", &device_features_) &&
              lib_.Get("xessVKCreateContext", &create_) && lib_.Get("xessVKInit", &init_) &&
              lib_.Get("xessVKExecute", &execute_) &&
              lib_.Get("xessDestroyContext", &destroy_) &&
              lib_.Get("xessGetOptimalInputResolution", &optimal_) &&
              lib_.Get("xessSetVelocityScale", &velocity_scale_);
    if (!ok) {
      *error = "libxess.dll has no Vulkan support (XeSS SDK 1.3 or newer needed)";
      return false;
    }
    xess_version_t v{};
    if (get_version_(&v) == XESS_RESULT_SUCCESS) {
      version_ = "XeSS " + std::to_string(v.major) + "." + std::to_string(v.minor) + "." +
                 std::to_string(v.patch);
    }
    description_ = version_;
    return true;
  }
  ~Xess() override {
    if (context_) destroy_(context_);
  }
  const char* name() const override { return "XeSS"; }

  void InstanceRequirements(std::vector<const char*>* extensions, uint32_t* api) override {
    uint32_t n = 0, version = 0;
    const char* const* names = nullptr;
    if (instance_ext_(&n, &names, &version) != XESS_RESULT_SUCCESS) return;
    for (uint32_t i = 0; i < n; ++i) extensions->push_back(names[i]);
    *api = std::max(*api, version);
  }
  bool DeviceRequirements(VkInstance instance, VkPhysicalDevice physical,
                          std::vector<const char*>* extensions, void* features,
                          std::string* error) override {
    uint32_t n = 0;
    const char* const* names = nullptr;
    xess_result_t r = device_ext_(instance, physical, &n, &names);
    if (r != XESS_RESULT_SUCCESS) {
      *error = "XeSS doesn't support this GPU or driver (" + std::to_string(int(r)) + ")";
      return false;
    }
    for (uint32_t i = 0; i < n; ++i) extensions->push_back(names[i]);
    void* chain = features;
    r = device_features_(instance, physical, &chain);
    if (r != XESS_RESULT_SUCCESS) {
      *error = "XeSS: required device features missing (" + std::to_string(int(r)) + ")";
      return false;
    }
    return true;
  }
  bool Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            PFN_vkGetInstanceProcAddr, PFN_vkGetDeviceProcAddr, uint32_t ow, uint32_t oh,
            uint32_t rw, uint32_t rh, std::string* error) override {
    xess_result_t r = create_(instance, physical, device, &context_);
    if (r != XESS_RESULT_SUCCESS) {
      *error = "XeSS context creation failed (" + std::to_string(int(r)) + ")";
      context_ = nullptr;
      return false;
    }
    // The quality preset whose input range holds the render size (any size
    // in the range can be used each frame).
    const xess_2d_t out{ow, oh};
    const xess_quality_settings_t presets[] = {
        XESS_QUALITY_SETTING_AA, XESS_QUALITY_SETTING_ULTRA_QUALITY_PLUS,
        XESS_QUALITY_SETTING_ULTRA_QUALITY, XESS_QUALITY_SETTING_QUALITY,
        XESS_QUALITY_SETTING_BALANCED, XESS_QUALITY_SETTING_PERFORMANCE,
        XESS_QUALITY_SETTING_ULTRA_PERFORMANCE};
    static const char* const kPresetNames[] = {"Native AA", "Ultra Quality Plus",
                                               "Ultra Quality", "Quality", "Balanced",
                                               "Performance", "Ultra Performance"};
    int pick = -1;
    xess_2d_t lowest{rw, rh};
    for (int i = 0; i < 7; ++i) {
      xess_2d_t opt{}, lo{}, hi{};
      if (optimal_(context_, &out, presets[i], &opt, &lo, &hi) != XESS_RESULT_SUCCESS) continue;
      // Fixed presets report no range: just their own size.
      if (!lo.x || !hi.x) lo = hi = opt;
      if (lo.x < lowest.x) lowest = lo;
      if (rw >= lo.x && rw <= hi.x && rh >= lo.y && rh <= hi.y) {
        pick = i;
        break;
      }
    }
    if (pick < 0) {
      *error = "XeSS can't upscale " + std::to_string(rw) + "x" + std::to_string(rh) + " to " +
               std::to_string(ow) + "x" + std::to_string(oh) + " (smallest input " +
               std::to_string(lowest.x) + "x" + std::to_string(lowest.y) +
               "): raise the render scale";
      return false;
    }
    xess_vk_init_params_t p{};
    p.outputResolution = out;
    p.qualitySetting = presets[pick];
    p.initFlags = XESS_INIT_FLAG_INVERTED_DEPTH | XESS_INIT_FLAG_LDR_INPUT_COLOR;
    r = init_(context_, &p);
    if (r != XESS_RESULT_SUCCESS) {
      *error = "XeSS initialisation failed (" + std::to_string(int(r)) + ")";
      return false;
    }
    velocity_scale_(context_, 1.0f, 1.0f);
    description_ = version_ + ", " + kPresetNames[pick] + " preset, " + std::to_string(rw) + "x" +
                   std::to_string(rh) + " to " + std::to_string(ow) + "x" + std::to_string(oh);
    return true;
  }
  bool Dispatch(VkCommandBuffer cb, const UpscaleInputs& in, std::string* error) override {
    auto view = [](const UpscaleImage& im) {
      xess_vk_image_view_info v{};
      v.imageView = im.view;
      v.image = im.image;
      v.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      v.format = im.format;
      v.width = im.width;
      v.height = im.height;
      return v;
    };
    xess_vk_execute_params_t e{};
    e.colorTexture = view(in.color);
    e.velocityTexture = view(in.motion);
    e.depthTexture = view(in.depth);
    e.outputTexture = view(in.output);
    e.jitterOffsetX = in.jitter_x;
    e.jitterOffsetY = in.jitter_y;
    e.exposureScale = 1.0f;
    e.resetHistory = in.reset ? 1 : 0;
    e.inputWidth = in.render_width;
    e.inputHeight = in.render_height;
    const xess_result_t r = execute_(context_, cb, &e);
    if (r != XESS_RESULT_SUCCESS) {
      *error = "XeSS execute failed (" + std::to_string(int(r)) + ")";
      return false;
    }
    return true;
  }

 private:
  Library lib_;
  std::string version_ = "XeSS";
  xess_context_handle_t context_ = nullptr;
  decltype(&xessGetVersion) get_version_ = nullptr;
  decltype(&xessVKGetRequiredInstanceExtensions) instance_ext_ = nullptr;
  decltype(&xessVKGetRequiredDeviceExtensions) device_ext_ = nullptr;
  decltype(&xessVKGetRequiredDeviceFeatures) device_features_ = nullptr;
  decltype(&xessVKCreateContext) create_ = nullptr;
  decltype(&xessVKInit) init_ = nullptr;
  decltype(&xessVKExecute) execute_ = nullptr;
  decltype(&xessDestroyContext) destroy_ = nullptr;
  decltype(&xessGetOptimalInputResolution) optimal_ = nullptr;
  decltype(&xessSetVelocityScale) velocity_scale_ = nullptr;
};
#endif  // AMPED3_HAVE_XESS

// --- FSR (FidelityFX API) --------------------------------------------------------

// FidelityFX's Vulkan backend looks up instance functions too (e.g.
// vkGetPhysicalDeviceFeatures2) through the vkGetDeviceProcAddr it is given,
// which by the spec returns null for them (AMD's driver is lenient, Intel's
// isn't: a call through null). Give it one that falls back to the instance.
// One FSR context at a time, so plain statics.
VkInstance g_ffx_instance = VK_NULL_HANDLE;
PFN_vkGetInstanceProcAddr g_ffx_instance_proc = nullptr;
PFN_vkGetDeviceProcAddr g_ffx_device_proc = nullptr;

void VKAPI_PTR FfxNoLabel(VkCommandBuffer, const VkDebugUtilsLabelEXT*) {}
void VKAPI_PTR FfxNoLabelEnd(VkCommandBuffer) {}

PFN_vkVoidFunction VKAPI_PTR FfxGetDeviceProcAddr(VkDevice device, const char* name) {
  auto device_fn = [device](const char* n) {
    return g_ffx_device_proc ? g_ffx_device_proc(device, n) : nullptr;
  };
  PFN_vkVoidFunction f = device_fn(name);
  if (f) return f;
  // An extension's name for a function that is core now (e.g.
  // vkGetBufferMemoryRequirements2KHR, core since 1.1, which Intel's driver
  // doesn't give out under the extension's name unless it is enabled).
  std::string core = name;
  if (core.size() > 3 && core.compare(core.size() - 3, 3, "KHR") == 0) {
    core.resize(core.size() - 3);
    if ((f = device_fn(core.c_str()))) return f;
  }
  // Instance-level functions only: for a device function the instance hands
  // out the loader's trampoline, which jumps through null when the device
  // doesn't have it (the crash this replaces).
  if (std::strncmp(name, "vkGetPhysicalDevice", 19) == 0 ||
      std::strncmp(name, "vkEnumerate", 11) == 0) {
    if (g_ffx_instance_proc && (f = g_ffx_instance_proc(g_ffx_instance, name))) return f;
  }
  // Debug labels without VK_EXT_debug_utils: do nothing.
  if (std::strcmp(name, "vkCmdBeginDebugUtilsLabelEXT") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&FfxNoLabel);
  }
  if (std::strcmp(name, "vkCmdEndDebugUtilsLabelEXT") == 0) {
    return reinterpret_cast<PFN_vkVoidFunction>(&FfxNoLabelEnd);
  }
  return nullptr;
}

class Fsr final : public Upscaler {
 public:
  bool Load(std::string* error) {
    if (!lib_.Open("amd_fidelityfx_vk.dll")) {
      *error = "amd_fidelityfx_vk.dll not found (run setup.ps1 to fetch the FidelityFX runtime)";
      return false;
    }
    bool ok = lib_.Get("ffxCreateContext", &create_) && lib_.Get("ffxDestroyContext", &destroy_) &&
              lib_.Get("ffxQuery", &query_) && lib_.Get("ffxDispatch", &dispatch_);
    if (!ok) {
      *error = "amd_fidelityfx_vk.dll is not a FidelityFX API runtime";
      return false;
    }
    description_ = "FSR";
    return true;
  }
  ~Fsr() override {
    if (context_) destroy_(&context_, nullptr);
  }
  const char* name() const override { return "FSR"; }

  bool Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
            PFN_vkGetInstanceProcAddr get_instance_proc, PFN_vkGetDeviceProcAddr get_device_proc,
            uint32_t ow, uint32_t oh, uint32_t rw, uint32_t rh, std::string* error) override {
    g_ffx_instance = instance;
    g_ffx_instance_proc = get_instance_proc;
    g_ffx_device_proc = get_device_proc;
    ffxCreateBackendVKDesc backend{};
    backend.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_VK;
    backend.vkDevice = device;
    backend.vkPhysicalDevice = physical;
    backend.vkDeviceProcAddr = FfxGetDeviceProcAddr;
    ffxCreateContextDescUpscale desc{};
    desc.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    desc.flags = FFX_UPSCALE_ENABLE_DEPTH_INVERTED | FFX_UPSCALE_ENABLE_DEPTH_INFINITE |
                 FFX_UPSCALE_ENABLE_DYNAMIC_RESOLUTION;
    desc.maxRenderSize = {rw, rh};
    desc.maxUpscaleSize = {ow, oh};
    // The newest upscaler the runtime has (the list is newest first).
    ffxOverrideVersion override_version{};
    override_version.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    {
      uint64_t count = 8;
      uint64_t ids[8] = {};
      const char* names[8] = {};
      ffxQueryDescGetVersions q{};
      q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
      q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
      q.outputCount = &count;
      q.versionIds = ids;
      q.versionNames = names;
      if (query_(nullptr, &q.header) == FFX_API_RETURN_OK && count > 0) {
        override_version.versionId = ids[0];
        description_ = std::string("FSR ") + (names[0] ? names[0] : "");
        backend.header.pNext = &override_version.header;
      }
    }
    desc.header.pNext = &backend.header;
    if (create_(&context_, &desc.header, nullptr) != FFX_API_RETURN_OK) {
      context_ = nullptr;
      *error = "FSR context creation failed";
      return false;
    }
    return true;
  }
  bool Dispatch(VkCommandBuffer cb, const UpscaleInputs& in, std::string* error) override {
    auto resource = [](const UpscaleImage& im, uint32_t usage, uint32_t state) {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D;
      ci.format = im.format;
      ci.extent = {im.width, im.height, 1};
      ci.mipLevels = 1;
      ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT;
      ci.usage = im.usage;
      return ffxApiGetResourceVK(reinterpret_cast<void*>(im.image),
                                 ffxApiGetImageResourceDescriptionVK(im.image, ci, usage), state);
    };
    ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cb;
    d.color = resource(in.color, FFX_API_RESOURCE_USAGE_READ_ONLY,
                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth = resource(in.depth, FFX_API_RESOURCE_USAGE_READ_ONLY,
                       FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = resource(in.motion, FFX_API_RESOURCE_USAGE_READ_ONLY,
                               FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.output = resource(in.output, FFX_API_RESOURCE_USAGE_UAV, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset = {in.jitter_x, in.jitter_y};
    d.motionVectorScale = {1.0f, 1.0f};
    d.renderSize = {in.render_width, in.render_height};
    d.upscaleSize = {in.output.width, in.output.height};
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = std::clamp(in.sharpness, 0.0f, 1.0f);
    d.frameTimeDelta = in.frame_ms;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.camera_near;
    d.cameraFar = in.camera_far;
    d.cameraFovAngleVertical = in.fov_y;
    d.viewSpaceToMetersFactor = 1.0f;
    if (dispatch_(&context_, &d.header) != FFX_API_RETURN_OK) {
      *error = "FSR dispatch failed";
      return false;
    }
    return true;
  }
  int JitterPhases(uint32_t rw, uint32_t ow) const override {
    int32_t phases = 0;
    ffxQueryDescUpscaleGetJitterPhaseCount q{};
    q.header.type = FFX_API_QUERY_DESC_TYPE_UPSCALE_GETJITTERPHASECOUNT;
    q.renderWidth = rw;
    q.displayWidth = ow;
    q.pOutPhaseCount = &phases;
    ffxContext ctx = context_;
    if (query_(&ctx, &q.header) == FFX_API_RETURN_OK && phases > 0) return phases;
    return Upscaler::JitterPhases(rw, ow);
  }

 private:
  Library lib_;
  ffxContext context_ = nullptr;
  PfnFfxCreateContext create_ = nullptr;
  PfnFfxDestroyContext destroy_ = nullptr;
  PfnFfxQuery query_ = nullptr;
  PfnFfxDispatch dispatch_ = nullptr;
};

// --- Test (a stretch) ---------------------------------------------------------------

class TestUpscaler final : public Upscaler {
 public:
  TestUpscaler() { description_ = "test stretch"; }
  const char* name() const override { return "test"; }
  bool Init(VkInstance, VkPhysicalDevice, VkDevice device, PFN_vkGetInstanceProcAddr,
            PFN_vkGetDeviceProcAddr get, uint32_t, uint32_t, uint32_t, uint32_t,
            std::string* error) override {
    blit_ = reinterpret_cast<PFN_vkCmdBlitImage>(get(device, "vkCmdBlitImage"));
    barrier_ = reinterpret_cast<PFN_vkCmdPipelineBarrier>(get(device, "vkCmdPipelineBarrier"));
    if (!blit_ || !barrier_) *error = "vkCmdBlitImage missing";
    return blit_ && barrier_;
  }
  bool Dispatch(VkCommandBuffer cb, const UpscaleInputs& in, std::string*) override {
    auto layout = [&](VkImage im, VkImageLayout from, VkImageLayout to) {
      VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
      b.srcAccessMask = b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
      b.oldLayout = from;
      b.newLayout = to;
      b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      b.image = im;
      b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      barrier_(cb, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
               nullptr, 0, nullptr, 1, &b);
    };
    layout(in.color.image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
           VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkImageBlit r{};
    r.srcSubresource = r.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    r.srcOffsets[1] = {int32_t(in.render_width), int32_t(in.render_height), 1};
    r.dstOffsets[1] = {int32_t(in.output.width), int32_t(in.output.height), 1};
    blit_(cb, in.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, in.output.image,
          VK_IMAGE_LAYOUT_GENERAL, 1, &r, VK_FILTER_LINEAR);
    layout(in.color.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
           VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    return true;
  }

 private:
  PFN_vkCmdBlitImage blit_ = nullptr;
  PFN_vkCmdPipelineBarrier barrier_ = nullptr;
};

}  // namespace

int Upscaler::JitterPhases(uint32_t render_width, uint32_t output_width) const {
  // 8 per input pixel, times the area ratio (XeSS's guidance).
  const float ratio = float(output_width) / float(std::max(render_width, 1u));
  return std::max(8, int(std::ceil(8.0f * ratio * ratio)));
}

void UpscaleJitter(uint32_t index, int phases, float* x, float* y) {
  const uint32_t i = (index % uint32_t(std::max(phases, 1))) + 1;  // Halton skips 0
  HaltonBits(i, 2, x);
  HaltonBits(i, 3, y);
  *x -= 0.5f;
  *y -= 0.5f;
}

std::unique_ptr<Upscaler> Upscaler::Create(UpscalerKind kind, std::string* error) {
  if (kind == UpscalerKind::kXess) {
#if AMPED3_HAVE_XESS
    auto u = std::make_unique<Xess>();
    if (u->Load(error)) return u;
#else
    *error = "this build has no XeSS (its headers come with the XeSS SDK; run setup.ps1)";
    return nullptr;
#endif
  } else if (kind == UpscalerKind::kFsr) {
    auto u = std::make_unique<Fsr>();
    if (u->Load(error)) return u;
  } else if (kind == UpscalerKind::kTest) {
    return std::make_unique<TestUpscaler>();
  }
  return nullptr;
}

}  // namespace amped3::native
