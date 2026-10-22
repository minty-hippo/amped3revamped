// Command streams: one thread prepares a frame, another records its Vulkan
// commands.
//
// The renderer's worker spends a good part of each frame inside the driver
// recording commands (vkCmd*: binding pipelines and descriptor sets, setting
// state, drawing): about 3 ms a frame on an ARM handheld. With a Stream
// begun for a command buffer, the worker's vkCmd* calls on that command
// buffer are copied (arguments and the arrays they point to) into a queue,
// and a recording thread makes the real calls, in the same order, while the
// worker goes on with the next draws.
//
// It works under the existing calls: Install() replaces the vk:: command
// functions (vk.h) with ones that check whether the calling thread has a
// stream for that command buffer; every other call goes straight to the
// driver. Code that records into the command buffer by other means (a
// library such as an upscaler) calls Drain() first.

#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <new>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "vk.h"

namespace amped3::native::cmd {

// After vk::LoadDevice (each time: it sets the driver's functions again):
// the vk:: command functions a stream handles become stubs that defer or pass
// through.
void Install();

class Stream {
 public:
  Stream();
  ~Stream();
  Stream(const Stream&) = delete;
  Stream& operator=(const Stream&) = delete;

  // The calling thread's commands to `cb` go to the recording thread.
  void Begin(VkCommandBuffer cb);
  // Waits until everything deferred so far is recorded.
  void Drain();
  // Drain, and the calling thread records directly again.
  void End();

  bool active() const { return cb_ != VK_NULL_HANDLE; }
  VkCommandBuffer command_buffer() const { return cb_; }

  // A command: any callable taking the command buffer (copied into the queue).
  template <typename F>
  void Push(F&& f) {
    using T = std::decay_t<F>;
    static_assert(alignof(T) <= kAlign, "command over-aligned");
    const size_t size = (sizeof(Entry) + sizeof(T) + kAlign - 1) & ~(kAlign - 1);
    Entry* e = static_cast<Entry*>(Reserve(size));
    e->size = uint32_t(size);
    e->run = [](void* p, VkCommandBuffer cb) {
      T* t = static_cast<T*>(p);
      (*t)(cb);
      t->~T();
    };
    new (e + 1) T(std::forward<F>(f));
    Commit(size);
  }

  // The stream of the calling thread, if it has begun one.
  static Stream* Current();

 private:
  static constexpr size_t kAlign = 16;
  struct alignas(kAlign) Entry {
    void (*run)(void* object, VkCommandBuffer cb);
    uint32_t size;
  };
  static_assert(sizeof(Entry) % kAlign == 0, "entry header size");
  struct Chunk {
    std::unique_ptr<unsigned char[]> data;
    size_t capacity = 0;
    size_t used = 0;
  };

  void* Reserve(size_t size);
  void Commit(size_t size);
  void HandOver();  // the current chunk to the recording thread
  void Main();

  VkCommandBuffer cb_ = VK_NULL_HANDLE;
  Chunk* current_ = nullptr;

  std::mutex mutex_;
  std::condition_variable work_cv_, idle_cv_;
  std::vector<Chunk*> queue_, free_;
  std::vector<std::unique_ptr<Chunk>> chunks_;
  bool busy_ = false;
  bool stop_ = false;
  std::thread thread_;
};

}  // namespace amped3::native::cmd
