#include "vk.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace vk {
#define AMPED3_VK_DEFINE(name) PFN_##name name = nullptr;
AMPED3_VK_GLOBAL_FUNCS(AMPED3_VK_DEFINE)
AMPED3_VK_INSTANCE_FUNCS(AMPED3_VK_DEFINE)
AMPED3_VK_DEVICE_FUNCS(AMPED3_VK_DEFINE)
#undef AMPED3_VK_DEFINE

namespace {
PFN_vkGetInstanceProcAddr g_get_instance_proc = nullptr;
}

void LoadGlobal() {
#ifdef _WIN32
  HMODULE lib = LoadLibraryA("vulkan-1.dll");
  if (!lib) throw std::runtime_error("vulkan-1.dll not found");
  g_get_instance_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(lib, "vkGetInstanceProcAddr"));
#else
  void* lib = dlopen("libvulkan.so.1", RTLD_NOW);
  if (!lib) throw std::runtime_error("libvulkan.so.1 not found");
  g_get_instance_proc =
      reinterpret_cast<PFN_vkGetInstanceProcAddr>(dlsym(lib, "vkGetInstanceProcAddr"));
#endif
  if (!g_get_instance_proc) throw std::runtime_error("vkGetInstanceProcAddr missing");
#define AMPED3_VK_LOAD(name) name = reinterpret_cast<PFN_##name>(g_get_instance_proc(nullptr, #name));
  AMPED3_VK_GLOBAL_FUNCS(AMPED3_VK_LOAD)
#undef AMPED3_VK_LOAD
  if (!vkCreateInstance) throw std::runtime_error("vkCreateInstance missing");
}

PFN_vkGetInstanceProcAddr GetInstanceProcAddr() { return g_get_instance_proc; }

void LoadInstance(VkInstance instance) {
#define AMPED3_VK_LOAD(name)                                                       \
  name = reinterpret_cast<PFN_##name>(g_get_instance_proc(instance, #name));       \
  if (!name) throw std::runtime_error("missing Vulkan function " #name);
  AMPED3_VK_INSTANCE_FUNCS(AMPED3_VK_LOAD)
#undef AMPED3_VK_LOAD
}

void LoadDevice(VkDevice device) {
#define AMPED3_VK_LOAD(name)                                                       \
  name = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));         \
  if (!name) throw std::runtime_error("missing Vulkan function " #name);
  AMPED3_VK_DEVICE_FUNCS(AMPED3_VK_LOAD)
#undef AMPED3_VK_LOAD
}
}  // namespace vk
