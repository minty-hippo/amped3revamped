// Shows the native renderer's image in the game window, through the
// presenter's guest output (SDK patch 0008) with the GPU plugin's own API
// (Direct3D 12 or Vulkan): copied on the GPU from an image shared between the
// two Vulkan devices when possible, else uploaded from the CPU.

#pragma once

#include <cstdint>
#include <string>

namespace rex::ui {
class Presenter;
}

namespace amped3::native {
struct OutputInfo;
}

namespace amped3::live {

// Packed A2B10G10R10 pixels (red in the low bits, the guest output format of
// both presenters), top row first, width * 4 bytes per row. Goes through the
// CPU. One thread at a time. Returns false with *error set on failure.
bool PresentPacked(rex::ui::Presenter* presenter, const std::string& backend, uint32_t width,
                   uint32_t height, const uint8_t* packed, std::string* error);

// Zero-copy presentation (Vulkan presenter on a device with
// VK_KHR_external_memory_win32 / _fd, SDK patch 0010): the native renderer
// draws into images shared with the presenter's device.
// The presenter's GPU (VkPhysicalDeviceIDProperties::deviceUUID); false if
// unknown.
bool PresenterDeviceUuid(rex::ui::Presenter* presenter, uint8_t uuid[16]);
// Whether the presenter's device can import the native renderer's images.
bool CanPresentShared(rex::ui::Presenter* presenter);
// Waits until the presenter has finished copying from shared slot `slot`, so
// the renderer can draw into it again.
void WaitSharedSlot(uint32_t slot);
// Shows shared image `info` (imported once per generation). The image must be
// finished (DrawOutput returned).
bool PresentShared(rex::ui::Presenter* presenter, const native::OutputInfo& info,
                   std::string* error);

// Frees the presentation resources (waits for the GPU).
void ReleasePresentResources();

}  // namespace amped3::live
