// Minimal Vulkan loader for the native renderer (game and replayer): loads
// vulkan-1.dll / libvulkan.so.1 at run time and fetches the functions it uses.

#pragma once

#define VK_NO_PROTOTYPES
#include <vulkan/vulkan_core.h>

#include <stdexcept>
#include <string>

#define AMPED3_VK_GLOBAL_FUNCS(X) \
  X(vkCreateInstance)             \
  X(vkEnumerateInstanceVersion)   \
  X(vkEnumerateInstanceExtensionProperties)

#define AMPED3_VK_INSTANCE_FUNCS(X)              \
  X(vkDestroyInstance)                           \
  X(vkEnumeratePhysicalDevices)                  \
  X(vkGetPhysicalDeviceProperties)               \
  X(vkGetPhysicalDeviceFeatures2)                \
  X(vkGetPhysicalDeviceProperties2)              \
  X(vkEnumerateDeviceExtensionProperties)        \
  X(vkGetPhysicalDeviceQueueFamilyProperties)    \
  X(vkGetPhysicalDeviceMemoryProperties)         \
  X(vkGetPhysicalDeviceFormatProperties)         \
  X(vkGetPhysicalDeviceImageFormatProperties)    \
  X(vkCreateDevice)                              \
  X(vkGetDeviceProcAddr)

#define AMPED3_VK_DEVICE_FUNCS(X)          \
  X(vkDestroyDevice)                       \
  X(vkCreatePipelineCache)                 \
  X(vkCmdClearColorImage)                  \
  X(vkCreateQueryPool)                     \
  X(vkDestroyQueryPool)                    \
  X(vkCmdResetQueryPool)                   \
  X(vkCmdWriteTimestamp)                   \
  X(vkGetQueryPoolResults)                 \
  X(vkDestroyPipelineCache)                \
  X(vkGetPipelineCacheData)                \
  X(vkGetDeviceQueue)                      \
  X(vkQueueSubmit)                         \
  X(vkQueueWaitIdle)                       \
  X(vkDeviceWaitIdle)                      \
  X(vkCreateCommandPool)                   \
  X(vkDestroyCommandPool)                  \
  X(vkAllocateCommandBuffers)              \
  X(vkBeginCommandBuffer)                  \
  X(vkEndCommandBuffer)                    \
  X(vkAllocateMemory)                      \
  X(vkFreeMemory)                          \
  X(vkMapMemory)                           \
  X(vkUnmapMemory)                         \
  X(vkCreateBuffer)                        \
  X(vkDestroyBuffer)                       \
  X(vkGetBufferMemoryRequirements)         \
  X(vkBindBufferMemory)                    \
  X(vkCreateImage)                         \
  X(vkDestroyImage)                        \
  X(vkGetImageMemoryRequirements)          \
  X(vkBindImageMemory)                     \
  X(vkCreateImageView)                     \
  X(vkDestroyImageView)                    \
  X(vkCreateSampler)                       \
  X(vkDestroySampler)                      \
  X(vkCreateShaderModule)                  \
  X(vkDestroyShaderModule)                 \
  X(vkCreateDescriptorSetLayout)           \
  X(vkCreateDescriptorPool)                \
  X(vkAllocateDescriptorSets)              \
  X(vkUpdateDescriptorSets)                \
  X(vkCreatePipelineLayout)                \
  X(vkCreateGraphicsPipelines)             \
  X(vkDestroyPipeline)                     \
  X(vkCmdPipelineBarrier)                  \
  X(vkCmdCopyBufferToImage)                \
  X(vkCmdCopyImageToBuffer)                \
  X(vkCmdCopyImage)                        \
  X(vkCmdBeginRendering)                   \
  X(vkCmdEndRendering)                     \
  X(vkCmdBindPipeline)                     \
  X(vkCmdBindDescriptorSets)               \
  X(vkCmdPushConstants)                    \
  X(vkCmdBindIndexBuffer)                  \
  X(vkCmdSetViewport)                      \
  X(vkCmdSetScissor)                       \
  X(vkCmdSetBlendConstants)                \
  X(vkCmdSetDepthBias)                     \
  X(vkCmdSetStencilReference)              \
  X(vkCmdSetStencilCompareMask)            \
  X(vkCmdSetStencilWriteMask)              \
  X(vkCmdDraw)                             \
  X(vkCmdDrawIndexed)                      \
  X(vkCmdClearAttachments)                 \
  X(vkCreateFence)                         \
  X(vkDestroyFence)                        \
  X(vkResetFences)                         \
  X(vkWaitForFences)                       \
  X(vkResetCommandPool)                    \
  X(vkFreeCommandBuffers)                  \
  X(vkDestroyDescriptorPool)               \
  X(vkResetDescriptorPool)                 \
  X(vkDestroyDescriptorSetLayout)          \
  X(vkDestroyPipelineLayout)

namespace vk {
#define AMPED3_VK_DECLARE(name) extern PFN_##name name;
AMPED3_VK_GLOBAL_FUNCS(AMPED3_VK_DECLARE)
AMPED3_VK_INSTANCE_FUNCS(AMPED3_VK_DECLARE)
AMPED3_VK_DEVICE_FUNCS(AMPED3_VK_DECLARE)
#undef AMPED3_VK_DECLARE

void LoadGlobal();                   // throws on failure
void LoadInstance(VkInstance instance);
void LoadDevice(VkDevice device);
// The loader's vkGetInstanceProcAddr (after LoadGlobal).
PFN_vkGetInstanceProcAddr GetInstanceProcAddr();

inline void Check(VkResult r, const char* what) {
  if (r != VK_SUCCESS) throw std::runtime_error(std::string(what) + " failed (" + std::to_string(r) + ")");
}
}  // namespace vk
