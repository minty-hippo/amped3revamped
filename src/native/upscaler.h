// Temporal upscalers for the native renderer: Intel XeSS (XMX on Intel Arc,
// DP4a elsewhere, chosen by the library) and AMD FSR 3.1 (FidelityFX API,
// compute shaders on any GPU; the newest provider the runtime offers).
//
// Both libraries are loaded at run time (libxess.dll, amd_fidelityfx_vk.dll
// next to the exe; setup.ps1 fetches them. On Linux FSR only, as
// libamd_fidelityfx_vk.so, which setup.sh builds from AMD's sources:
// linux/fidelityfx): without them, or on a GPU they don't support, Create
// returns null with the reason and the renderer draws without upscaling.
// The headers are vendored in third_party/ (unmodified).

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace amped3::native {

// kTest: a plain stretch through the same path (replayer tests: runs the
// motion vectors, layouts and dispatch without the vendors' libraries).
enum class UpscalerKind : int { kNone = 0, kXess = 1, kFsr = 2, kTest = 9 };

struct UpscaleImage {
  VkImage image = VK_NULL_HANDLE;
  VkImageView view = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  uint32_t width = 0, height = 0;
  VkImageUsageFlags usage = 0;
};

struct UpscaleInputs {
  UpscaleImage color;   // render resolution, sampled
  UpscaleImage depth;   // render resolution, R32F (inverted: 1 near, 0 far)
  UpscaleImage motion;  // render resolution, RG16F: pixels, current -> previous, no jitter
  UpscaleImage output;  // output resolution, storage
  UpscaleImage reactive;  // render resolution, R8 (optional): 1 = trust this frame (FSR)
  uint32_t render_width = 0, render_height = 0;
  float jitter_x = 0, jitter_y = 0;  // pixels, as added to the projection (+x right, +y down)
  bool reset = false;
  float frame_ms = 16.7f;
  float camera_near = 0.1f, camera_far = 10000.0f, fov_y = 1.0f;  // radians
  float sharpness = 0.0f;  // 0..1 (FSR's own sharpening)
};

class Upscaler {
 public:
  virtual ~Upscaler() = default;
  // Loads the library; null (and *error) when it isn't there.
  static std::unique_ptr<Upscaler> Create(UpscalerKind kind, std::string* error);

  virtual const char* name() const = 0;
  // Before the instance: extensions to enable and the API version it needs.
  virtual void InstanceRequirements(std::vector<const char*>* extensions, uint32_t* api) {}
  // Before the device: extensions, and features chained into `features`
  // (a VkPhysicalDeviceFeatures2 chain, which it may extend).
  virtual bool DeviceRequirements(VkInstance instance, VkPhysicalDevice physical,
                                  std::vector<const char*>* extensions, void* features,
                                  std::string* error) {
    return true;
  }
  // The device is made: create the context for this output size (render
  // sizes up to max_render).
  virtual bool Init(VkInstance instance, VkPhysicalDevice physical, VkDevice device,
                    PFN_vkGetInstanceProcAddr get_instance_proc,
                    PFN_vkGetDeviceProcAddr get_device_proc, uint32_t output_width,
                    uint32_t output_height, uint32_t max_render_width, uint32_t max_render_height,
                    std::string* error) = 0;
  // Records the upscale. Inputs must be in SHADER_READ_ONLY_OPTIMAL layout and
  // the output in GENERAL (the caller transitions them).
  virtual bool Dispatch(VkCommandBuffer cb, const UpscaleInputs& in, std::string* error) = 0;
  // The library's own idea of the jitter sequence length for this ratio.
  virtual int JitterPhases(uint32_t render_width, uint32_t output_width) const;
  // Last reported description (version, path: e.g. "XeSS 3.0.2 (XMX)").
  const std::string& description() const { return description_; }

 protected:
  std::string description_;
};

// Halton(2, 3) jitter in [-0.5, 0.5] pixels for frame `index` of `phases`.
void UpscaleJitter(uint32_t index, int phases, float* x, float* y);

}  // namespace amped3::native
