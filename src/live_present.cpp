#include "live_present.h"

#include <cstring>
#include <mutex>
#include <vector>

#include <rex/ui/presenter.h>
// The SDK's Vulkan headers include renderdoc_app.h, which the SDK install
// lacks; CMake adds the SDK checkout's copy and defines this when found.
#if defined(AMPED3_VULKAN_PRESENT)
#include <rex/ui/vulkan/presenter.h>
#endif
#if defined(_WIN32)
#include <rex/ui/d3d12/d3d12_presenter.h>
#endif

#include "native/renderer.h"
#include "presenter_external.h"

namespace amped3::live {
namespace {

// The guest output formats of both backends pack R in the low bits
// (DXGI_FORMAT_R10G10B10A2_UNORM / VK_FORMAT_A2B10G10R10_UNORM_PACK32), as the
// native renderer's output does: rows are copied as they are.
void CopyPacked(const uint8_t* packed, uint32_t width, uint32_t height, uint8_t* dst,
                size_t row_pitch) {
  const size_t row = size_t(width) * 4;
  if (row_pitch == row) {
    std::memcpy(dst, packed, row * height);
    return;
  }
  for (uint32_t y = 0; y < height; ++y) std::memcpy(dst + row_pitch * y, packed + row * y, row);
}

#if defined(AMPED3_VULKAN_PRESENT)
// --- Vulkan ---------------------------------------------------------------------

struct VulkanUpload {
  const rex::ui::vulkan::VulkanDevice* device = nullptr;
  VkBuffer buffer = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  void* mapped = nullptr;
  VkDeviceSize size = 0;
  VkCommandPool pool = VK_NULL_HANDLE;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool fence_pending = false;
};
VulkanUpload g_vk;

// Shared output images (the native renderer's), imported into the
// presenter's device, one per renderer slot.
struct SharedSlot {
  uint64_t generation = 0;
  VkImage image = VK_NULL_HANDLE;
  VkDeviceMemory memory = VK_NULL_HANDLE;
  uint32_t width = 0, height = 0;
  VkCommandBuffer cb = VK_NULL_HANDLE;
  VkFence fence = VK_NULL_HANDLE;
  bool fence_pending = false;
};
SharedSlot g_shared[native::Renderer::kOutputSlots];
PFN_vkCmdCopyImage g_cmd_copy_image = nullptr;

void ReleaseSharedImage(SharedSlot& slot) {
  if (!g_vk.device) return;
  const auto& dfn = g_vk.device->functions();
  const VkDevice device = g_vk.device->device();
  if (slot.fence_pending) {
    dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    slot.fence_pending = false;
  }
  if (slot.image) dfn.vkDestroyImage(device, slot.image, nullptr);
  if (slot.memory) dfn.vkFreeMemory(device, slot.memory, nullptr);
  slot.image = VK_NULL_HANDLE;
  slot.memory = VK_NULL_HANDLE;
  slot.generation = 0;
}

void ReleaseVulkan() {
  if (!g_vk.device) return;
  const auto& dfn = g_vk.device->functions();
  const VkDevice device = g_vk.device->device();
  for (auto& slot : g_shared) {
    ReleaseSharedImage(slot);
    if (slot.fence) dfn.vkDestroyFence(device, slot.fence, nullptr);
    slot = SharedSlot{};
  }
  if (g_vk.fence_pending) dfn.vkWaitForFences(device, 1, &g_vk.fence, VK_TRUE, UINT64_MAX);
  if (g_vk.fence) dfn.vkDestroyFence(device, g_vk.fence, nullptr);
  if (g_vk.pool) dfn.vkDestroyCommandPool(device, g_vk.pool, nullptr);
  if (g_vk.buffer) dfn.vkDestroyBuffer(device, g_vk.buffer, nullptr);
  if (g_vk.memory) dfn.vkFreeMemory(device, g_vk.memory, nullptr);
  g_vk = VulkanUpload{};
}

// Command objects on the presenter's device (recreated if it changes).
bool EnsureVulkan(const rex::ui::vulkan::VulkanDevice* vulkan_device, std::string* error) {
  if (g_vk.device == vulkan_device) return true;
  ReleaseVulkan();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  g_vk.device = vulkan_device;
  VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pci.queueFamilyIndex = vulkan_device->queue_family_graphics_compute();
  bool ok = dfn.vkCreateCommandPool(device, &pci, nullptr, &g_vk.pool) == VK_SUCCESS;
  VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  cai.commandPool = g_vk.pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
  if (ok) ok = dfn.vkAllocateCommandBuffers(device, &cai, &g_vk.cb) == VK_SUCCESS;
  if (ok) ok = dfn.vkCreateFence(device, &fci, nullptr, &g_vk.fence) == VK_SUCCESS;
  for (auto& slot : g_shared) {
    if (ok) ok = dfn.vkAllocateCommandBuffers(device, &cai, &slot.cb) == VK_SUCCESS;
    if (ok) ok = dfn.vkCreateFence(device, &fci, nullptr, &slot.fence) == VK_SUCCESS;
  }
  g_cmd_copy_image = reinterpret_cast<PFN_vkCmdCopyImage>(
      vulkan_device->vulkan_instance()->functions().vkGetDeviceProcAddr(device, "vkCmdCopyImage"));
  if (!ok) {
    *error = "Vulkan command objects";
    ReleaseVulkan();
    return false;
  }
  return true;
}

bool PresentVulkan(rex::ui::Presenter* presenter, uint32_t width, uint32_t height,
                   const uint8_t* packed, std::string* error) {
  using rex::ui::vulkan::VulkanPresenter;
  auto* vk_presenter = static_cast<VulkanPresenter*>(presenter);
  const rex::ui::vulkan::VulkanDevice* vulkan_device = vk_presenter->vulkan_device();
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  const VkDeviceSize bytes = VkDeviceSize(width) * height * 4;

  if (!EnsureVulkan(vulkan_device, error)) return false;
  if (g_vk.fence_pending) {
    dfn.vkWaitForFences(device, 1, &g_vk.fence, VK_TRUE, UINT64_MAX);
    dfn.vkResetFences(device, 1, &g_vk.fence);
    g_vk.fence_pending = false;
  }
  if (g_vk.size < bytes) {
    if (g_vk.buffer) dfn.vkDestroyBuffer(device, g_vk.buffer, nullptr);
    if (g_vk.memory) dfn.vkFreeMemory(device, g_vk.memory, nullptr);
    g_vk.buffer = VK_NULL_HANDLE;
    g_vk.memory = VK_NULL_HANDLE;
    g_vk.size = 0;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (dfn.vkCreateBuffer(device, &bci, nullptr, &g_vk.buffer) != VK_SUCCESS) {
      *error = "Vulkan upload buffer";
      return false;
    }
    VkMemoryRequirements req;
    dfn.vkGetBufferMemoryRequirements(device, g_vk.buffer, &req);
    const auto& types = vulkan_device->memory_types();
    uint32_t candidates = req.memoryTypeBits & types.host_visible & types.host_coherent;
    if (!candidates) {
      *error = "no host-visible coherent memory";
      return false;
    }
    uint32_t type = 0;
    while (!(candidates & (1u << type))) ++type;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = req.size;
    mai.memoryTypeIndex = type;
    if (dfn.vkAllocateMemory(device, &mai, nullptr, &g_vk.memory) != VK_SUCCESS ||
        dfn.vkBindBufferMemory(device, g_vk.buffer, g_vk.memory, 0) != VK_SUCCESS ||
        dfn.vkMapMemory(device, g_vk.memory, 0, VK_WHOLE_SIZE, 0, &g_vk.mapped) != VK_SUCCESS) {
      *error = "Vulkan upload memory";
      return false;
    }
    g_vk.size = bytes;
  }
  CopyPacked(packed, width, height, static_cast<uint8_t*>(g_vk.mapped), size_t(width) * 4);

  return RefreshGuestOutputExternal(
      presenter, width, height, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto& vc = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        // (Begin resets it: the pool allows resetting single buffers.)
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dfn.vkBeginCommandBuffer(g_vk.cb, &bi);
        VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        b.srcAccessMask = vc.image_ever_written_previously()
                              ? VulkanPresenter::kGuestOutputInternalAccessMask
                              : 0;
        b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = vc.image();
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(g_vk.cb,
                                 vc.image_ever_written_previously()
                                     ? VkPipelineStageFlags(
                                           VulkanPresenter::kGuestOutputInternalStageMask)
                                     : VkPipelineStageFlags(VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT),
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {width, height, 1};
        dfn.vkCmdCopyBufferToImage(g_vk.cb, g_vk.buffer, vc.image(),
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
        dfn.vkCmdPipelineBarrier(g_vk.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                                 nullptr, 1, &b);
        dfn.vkEndCommandBuffer(g_vk.cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &g_vk.cb;
        VkResult r;
        {
          auto queue = vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
          r = dfn.vkQueueSubmit(queue.queue(), 1, &si, g_vk.fence);
        }
        if (r != VK_SUCCESS) {
          *error = "vkQueueSubmit";
          return false;
        }
        g_vk.fence_pending = true;
        return true;
      });
}

// --- Shared images (zero copy) ---------------------------------------------------

template <typename E>
bool HasExternalMemory(const E& ext) {
  if constexpr (requires { ext.ext_KHR_external_memory_win32; ext.ext_KHR_external_memory_fd; }) {
#if defined(_WIN32)
    return ext.ext_KHR_external_memory_win32;
#else
    return ext.ext_KHR_external_memory_fd;
#endif
  } else {
    return false;  // SDK without patch 0010
  }
}

bool UuidVulkan(rex::ui::Presenter* presenter, uint8_t uuid[16]) {
  using rex::ui::vulkan::VulkanPresenter;
  const auto* d = static_cast<VulkanPresenter*>(presenter)->vulkan_device();
  const auto& ifn = d->vulkan_instance()->functions();
  if (!ifn.vkGetPhysicalDeviceProperties2) return false;
  VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
  VkPhysicalDeviceProperties2 p2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
  p2.pNext = &id;
  ifn.vkGetPhysicalDeviceProperties2(d->physical_device(), &p2);
  std::memcpy(uuid, id.deviceUUID, VK_UUID_SIZE);
  return true;
}

// Imports the renderer's image for `info.slot` into the presenter's device.
bool ImportShared(const rex::ui::vulkan::VulkanDevice* vulkan_device,
                  const native::OutputInfo& info, SharedSlot& slot, std::string* error) {
  ReleaseSharedImage(slot);
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
#if defined(_WIN32)
  constexpr auto kType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
  constexpr auto kType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
  // The same creation parameters as the renderer's image (Gpu::CreateExportImage).
  VkExternalMemoryImageCreateInfo emi{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
  emi.handleTypes = kType;
  VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ici.pNext = &emi;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32;
  ici.extent = {info.width, info.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  if (dfn.vkCreateImage(device, &ici, nullptr, &slot.image) != VK_SUCCESS) {
    *error = "shared image: vkCreateImage";
    return false;
  }
  VkMemoryRequirements req;
  dfn.vkGetImageMemoryRequirements(device, slot.image, &req);
  const auto& types = vulkan_device->memory_types();
  uint32_t candidates = req.memoryTypeBits & types.device_local;
  if (!candidates) candidates = req.memoryTypeBits;
  uint32_t type = 0;
  while (type < 32 && !(candidates & (1u << type))) ++type;
  VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
  dedicated.image = slot.image;
#if defined(_WIN32)
  VkImportMemoryWin32HandleInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
  import_info.handleType = kType;
  import_info.handle = reinterpret_cast<HANDLE>(uintptr_t(info.handle));
#else
  VkImportMemoryFdInfoKHR import_info{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
  import_info.handleType = kType;
  import_info.fd = int(info.handle);
#endif
  import_info.pNext = &dedicated;
  VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  mai.pNext = &import_info;
  mai.allocationSize = info.memory_size;
  mai.memoryTypeIndex = type;
  if (dfn.vkAllocateMemory(device, &mai, nullptr, &slot.memory) != VK_SUCCESS ||
      dfn.vkBindImageMemory(device, slot.image, slot.memory, 0) != VK_SUCCESS) {
    *error = "shared image: import failed";
    ReleaseSharedImage(slot);
    return false;
  }
  slot.generation = info.generation;
  slot.width = info.width;
  slot.height = info.height;
  return true;
}

bool PresentSharedVulkan(rex::ui::Presenter* presenter, const native::OutputInfo& info,
                         std::string* error) {
  using rex::ui::vulkan::VulkanPresenter;
  auto* vk_presenter = static_cast<VulkanPresenter*>(presenter);
  const rex::ui::vulkan::VulkanDevice* vulkan_device = vk_presenter->vulkan_device();
  if (!EnsureVulkan(vulkan_device, error)) return false;
  if (!g_cmd_copy_image || info.slot >= native::Renderer::kOutputSlots) {
    *error = "shared image: vkCmdCopyImage";
    return false;
  }
  const auto& dfn = vulkan_device->functions();
  const VkDevice device = vulkan_device->device();
  SharedSlot& slot = g_shared[info.slot];
  if (slot.fence_pending) {
    dfn.vkWaitForFences(device, 1, &slot.fence, VK_TRUE, UINT64_MAX);
    slot.fence_pending = false;
  }
  if (slot.generation != info.generation || !slot.image) {
    if (!ImportShared(vulkan_device, info, slot, error)) return false;
  }
  dfn.vkResetFences(device, 1, &slot.fence);
  const uint32_t width = info.width, height = info.height;
  return RefreshGuestOutputExternal(
      presenter, width, height, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto& vc = static_cast<VulkanPresenter::VulkanGuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        dfn.vkBeginCommandBuffer(slot.cb, &bi);
        VkImageMemoryBarrier b[2] = {{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER},
                                     {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER}};
        // The renderer released the image to the external queue family in
        // GENERAL layout: acquire it.
        b[0].srcAccessMask = 0;
        b[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
        b[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b[0].srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
        b[0].dstQueueFamilyIndex = vulkan_device->queue_family_graphics_compute();
        b[0].image = slot.image;
        b[0].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        b[1].srcAccessMask = vc.image_ever_written_previously()
                                 ? VulkanPresenter::kGuestOutputInternalAccessMask
                                 : 0;
        b[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b[1].srcQueueFamilyIndex = b[1].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b[1].image = vc.image();
        b[1].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(slot.cb,
                                 VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT |
                                     (vc.image_ever_written_previously()
                                          ? VkPipelineStageFlags(
                                                VulkanPresenter::kGuestOutputInternalStageMask)
                                          : VkPipelineStageFlags(0)),
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, b);
        VkImageCopy region{};
        region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.extent = {width, height, 1};
        g_cmd_copy_image(slot.cb, slot.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, vc.image(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
        VkImageMemoryBarrier a{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        a.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        a.dstAccessMask = VulkanPresenter::kGuestOutputInternalAccessMask;
        a.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        a.newLayout = VulkanPresenter::kGuestOutputInternalLayout;
        a.srcQueueFamilyIndex = a.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        a.image = vc.image();
        a.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        dfn.vkCmdPipelineBarrier(slot.cb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                                 VulkanPresenter::kGuestOutputInternalStageMask, 0, 0, nullptr, 0,
                                 nullptr, 1, &a);
        dfn.vkEndCommandBuffer(slot.cb);
        VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
        si.commandBufferCount = 1;
        si.pCommandBuffers = &slot.cb;
        VkResult r;
        {
          auto queue = vulkan_device->AcquireQueue(vulkan_device->queue_family_graphics_compute(), 0);
          r = dfn.vkQueueSubmit(queue.queue(), 1, &si, slot.fence);
        }
        if (r != VK_SUCCESS) {
          *error = "vkQueueSubmit";
          return false;
        }
        slot.fence_pending = true;
        return true;
      });
}

#endif  // AMPED3_VULKAN_PRESENT

// --- Direct3D 12 ------------------------------------------------------------------

#if defined(_WIN32)
struct D3D12Upload {
  ID3D12Device* device = nullptr;
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  UINT64 size = 0;
  Microsoft::WRL::ComPtr<ID3D12CommandAllocator> allocator;
  Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> list;
  Microsoft::WRL::ComPtr<ID3D12Fence> fence;
  HANDLE event = nullptr;
  UINT64 fence_value = 0;
};
D3D12Upload g_d3d;

void WaitD3D12() {
  if (g_d3d.fence && g_d3d.fence->GetCompletedValue() < g_d3d.fence_value) {
    g_d3d.fence->SetEventOnCompletion(g_d3d.fence_value, g_d3d.event);
    WaitForSingleObject(g_d3d.event, INFINITE);
  }
}

void ReleaseD3D12() {
  WaitD3D12();
  if (g_d3d.event) CloseHandle(g_d3d.event);
  g_d3d = D3D12Upload{};
}

bool PresentD3D12(rex::ui::Presenter* presenter, uint32_t width, uint32_t height,
                  const uint8_t* packed, std::string* error) {
  using rex::ui::d3d12::D3D12Presenter;
  auto* d3d_presenter = static_cast<D3D12Presenter*>(presenter);
  const auto& provider = d3d_presenter->provider();
  ID3D12Device* device = provider.GetDevice();
  ID3D12CommandQueue* queue = provider.GetDirectQueue();
  const UINT row_pitch = (width * 4 + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
                         ~UINT(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  const UINT64 bytes = UINT64(row_pitch) * height;

  if (g_d3d.device != device) ReleaseD3D12();
  if (!g_d3d.device) {
    g_d3d.device = device;
    if (FAILED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                              IID_PPV_ARGS(&g_d3d.allocator))) ||
        FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_d3d.allocator.Get(),
                                         nullptr, IID_PPV_ARGS(&g_d3d.list))) ||
        FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_d3d.fence)))) {
      *error = "D3D12 command objects";
      ReleaseD3D12();
      return false;
    }
    g_d3d.list->Close();
    g_d3d.event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
  }
  WaitD3D12();
  if (g_d3d.size < bytes) {
    g_d3d.buffer.Reset();
    D3D12_HEAP_PROPERTIES heap{};
    heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC desc{};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = bytes;
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                               IID_PPV_ARGS(&g_d3d.buffer)))) {
      *error = "D3D12 upload buffer";
      return false;
    }
    g_d3d.size = bytes;
  }
  void* mapped = nullptr;
  D3D12_RANGE none{0, 0};
  if (FAILED(g_d3d.buffer->Map(0, &none, &mapped))) {
    *error = "D3D12 Map";
    return false;
  }
  CopyPacked(packed, width, height, static_cast<uint8_t*>(mapped), row_pitch);
  g_d3d.buffer->Unmap(0, nullptr);

  return RefreshGuestOutputExternal(
      presenter, width, height, [&](rex::ui::Presenter::GuestOutputRefreshContext& context) {
        auto& dc = static_cast<D3D12Presenter::D3D12GuestOutputRefreshContext&>(context);
        context.SetIs8bpc(true);
        ID3D12Resource* target = dc.resource_uav_capable();
        g_d3d.allocator->Reset();
        g_d3d.list->Reset(g_d3d.allocator.Get(), nullptr);
        D3D12_RESOURCE_BARRIER barrier{};
        barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barrier.Transition.pResource = target;
        barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barrier.Transition.StateBefore = D3D12Presenter::kGuestOutputInternalState;
        barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_DEST;
        g_d3d.list->ResourceBarrier(1, &barrier);
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = target;
        dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        dst.SubresourceIndex = 0;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = g_d3d.buffer.Get();
        src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        src.PlacedFootprint.Offset = 0;
        src.PlacedFootprint.Footprint.Format = D3D12Presenter::kGuestOutputFormat;
        src.PlacedFootprint.Footprint.Width = width;
        src.PlacedFootprint.Footprint.Height = height;
        src.PlacedFootprint.Footprint.Depth = 1;
        src.PlacedFootprint.Footprint.RowPitch = row_pitch;
        g_d3d.list->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
        g_d3d.list->ResourceBarrier(1, &barrier);
        if (FAILED(g_d3d.list->Close())) {
          *error = "D3D12 command list";
          return false;
        }
        ID3D12CommandList* lists[] = {g_d3d.list.Get()};
        queue->ExecuteCommandLists(1, lists);
        queue->Signal(g_d3d.fence.Get(), ++g_d3d.fence_value);
        return true;
      });
}
#endif

std::mutex g_present_mutex;

}  // namespace

bool PresentPacked(rex::ui::Presenter* presenter, const std::string& backend, uint32_t width,
                   uint32_t height, const uint8_t* packed, std::string* error) {
  std::lock_guard lock(g_present_mutex);
  if (!presenter || !width || !height) {
    *error = "nothing to present";
    return false;
  }
  // The presenter's type says which API the GPU plugin actually uses (it may
  // have fallen back from the one asked for).
#if defined(AMPED3_VULKAN_PRESENT)
  if (dynamic_cast<rex::ui::vulkan::VulkanPresenter*>(presenter)) {
    return PresentVulkan(presenter, width, height, packed, error);
  }
#endif
#if defined(_WIN32)
  if (dynamic_cast<rex::ui::d3d12::D3D12Presenter*>(presenter)) {
    return PresentD3D12(presenter, width, height, packed, error);
  }
#endif
  *error = "unsupported presenter (GPU backend '" + backend + "')";
  return false;
}

bool PresenterDeviceUuid(rex::ui::Presenter* presenter, uint8_t uuid[16]) {
#if defined(AMPED3_VULKAN_PRESENT)
  if (presenter && dynamic_cast<rex::ui::vulkan::VulkanPresenter*>(presenter)) {
    return UuidVulkan(presenter, uuid);
  }
#endif
  (void)presenter;
  (void)uuid;
  return false;
}

bool CanPresentShared(rex::ui::Presenter* presenter) {
#if defined(AMPED3_VULKAN_PRESENT)
  if (auto* p = presenter ? dynamic_cast<rex::ui::vulkan::VulkanPresenter*>(presenter) : nullptr) {
    return HasExternalMemory(p->vulkan_device()->extensions());
  }
#endif
  (void)presenter;
  return false;
}

void WaitSharedSlot(uint32_t slot) {
#if defined(AMPED3_VULKAN_PRESENT)
  std::lock_guard lock(g_present_mutex);
  if (slot >= native::Renderer::kOutputSlots || !g_vk.device) return;
  SharedSlot& s = g_shared[slot];
  if (s.fence_pending) {
    const auto& dfn = g_vk.device->functions();
    dfn.vkWaitForFences(g_vk.device->device(), 1, &s.fence, VK_TRUE, UINT64_MAX);
    s.fence_pending = false;
  }
#else
  (void)slot;
#endif
}

bool PresentShared(rex::ui::Presenter* presenter, const native::OutputInfo& info,
                   std::string* error) {
  std::lock_guard lock(g_present_mutex);
#if defined(AMPED3_VULKAN_PRESENT)
  if (presenter && dynamic_cast<rex::ui::vulkan::VulkanPresenter*>(presenter)) {
    return PresentSharedVulkan(presenter, info, error);
  }
#endif
  (void)presenter;
  (void)info;
  *error = "shared images need the Vulkan presenter";
  return false;
}

void ReleasePresentResources() {
  std::lock_guard lock(g_present_mutex);
#if defined(AMPED3_VULKAN_PRESENT)
  ReleaseVulkan();
#endif
#if defined(_WIN32)
  ReleaseD3D12();
#endif
}

}  // namespace amped3::live
