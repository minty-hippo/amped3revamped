// Linux build of the FidelityFX API runtime: the few Vulkan functions the
// SDK's Vulkan backend calls directly (AMD's Windows DLL links the Vulkan
// loader for them), taken from the loader the game has already loaded, and
// the frame generation swapchain hook, which this build leaves out.

#include <dlfcn.h>

#include <FidelityFX/host/backends/vk/ffx_vk.h>

namespace {

void* Loader() {
  static void* handle = [] {
    void* h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL | RTLD_NOLOAD);
    if (!h) h = dlopen("libvulkan.so.1", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libvulkan.so", RTLD_NOW | RTLD_LOCAL);
    return h;
  }();
  return handle;
}

template <typename T>
T Resolve(const char* name) {
  void* h = Loader();
  return h ? reinterpret_cast<T>(dlsym(h, name)) : nullptr;
}

}  // namespace

#define FORWARD(ret, name, params, args)                                      \
  extern "C" ret VKAPI_CALL name params {                                     \
    static auto fn = Resolve<PFN_##name>(#name);                              \
    return fn args;                                                           \
  }

FORWARD(VkResult, vkCreateBuffer,
        (VkDevice d, const VkBufferCreateInfo* i, const VkAllocationCallbacks* a, VkBuffer* b),
        (d, i, a, b))
FORWARD(VkResult, vkEnumerateDeviceExtensionProperties,
        (VkPhysicalDevice p, const char* l, uint32_t* c, VkExtensionProperties* e), (p, l, c, e))
FORWARD(PFN_vkVoidFunction, vkGetDeviceProcAddr, (VkDevice d, const char* n), (d, n))
FORWARD(void, vkGetPhysicalDeviceFeatures, (VkPhysicalDevice p, VkPhysicalDeviceFeatures* f),
        (p, f))
FORWARD(void, vkGetPhysicalDeviceFeatures2, (VkPhysicalDevice p, VkPhysicalDeviceFeatures2* f),
        (p, f))
FORWARD(void, vkGetPhysicalDeviceMemoryProperties,
        (VkPhysicalDevice p, VkPhysicalDeviceMemoryProperties* m), (p, m))
FORWARD(void, vkGetPhysicalDeviceProperties, (VkPhysicalDevice p, VkPhysicalDeviceProperties* r),
        (p, r))
FORWARD(void, vkGetPhysicalDeviceProperties2,
        (VkPhysicalDevice p, VkPhysicalDeviceProperties2* r), (p, r))

FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) {
  return FFX_ERROR_INVALID_ARGUMENT;
}
