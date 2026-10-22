// Command streams (cmd_stream.h).

#include "cmd_stream.h"

#include <algorithm>
#include <array>
#include <cstring>

namespace amped3::native::cmd {

namespace {

thread_local Stream* t_stream = nullptr;

// The driver's functions, saved by Install.
struct Real {
#define AMPED3_CMD_FUNCS(X)                                                                      \
  X(vkCmdBindPipeline) X(vkCmdBindDescriptorSets) X(vkCmdPushConstants) X(vkCmdBindIndexBuffer) \
  X(vkCmdSetViewport) X(vkCmdSetScissor) X(vkCmdSetBlendConstants) X(vkCmdSetDepthBias)        \
  X(vkCmdSetStencilReference) X(vkCmdSetStencilCompareMask) X(vkCmdSetStencilWriteMask)        \
  X(vkCmdDraw) X(vkCmdDrawIndexed) X(vkCmdBeginRendering) X(vkCmdEndRendering)                 \
  X(vkCmdPipelineBarrier) X(vkCmdCopyImage) X(vkCmdCopyBufferToImage)                         \
  X(vkCmdCopyImageToBuffer) X(vkCmdClearColorImage) X(vkCmdClearAttachments)                  \
  X(vkCmdResetQueryPool) X(vkCmdWriteTimestamp)
#define AMPED3_CMD_REAL(name) PFN_##name name = nullptr;
  AMPED3_CMD_FUNCS(AMPED3_CMD_REAL)
#undef AMPED3_CMD_REAL
};
Real real;

// The stream deferring this call, if any.
inline Stream* For(VkCommandBuffer cb) {
  Stream* s = t_stream;
  return s && s->command_buffer() == cb ? s : nullptr;
}

template <typename T>
std::vector<T> Copy(const T* p, uint32_t n) {
  return p && n ? std::vector<T>(p, p + n) : std::vector<T>();
}

// Small arrays inline (the per-draw calls), larger ones on the heap.
template <typename T, size_t N>
struct SmallArray {
  std::array<T, N> inline_{};
  std::vector<T> heap;
  uint32_t n = 0;
  SmallArray(const T* p, uint32_t count) : n(count) {
    if (!p || !count) {
      n = 0;
    } else if (count <= N) {
      std::copy(p, p + count, inline_.begin());
    } else {
      heap.assign(p, p + count);
    }
  }
  const T* data() const { return n == 0 ? nullptr : n <= N ? inline_.data() : heap.data(); }
};

// --- Stubs: defer for the calling thread's stream, else the driver. ---

VKAPI_ATTR void VKAPI_CALL BindPipeline(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                         VkPipeline pipeline) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdBindPipeline(c, bp, pipeline); });
  } else {
    real.vkCmdBindPipeline(cb, bp, pipeline);
  }
}

VKAPI_ATTR void VKAPI_CALL BindDescriptorSets(VkCommandBuffer cb, VkPipelineBindPoint bp,
                                               VkPipelineLayout layout, uint32_t first,
                                               uint32_t count, const VkDescriptorSet* sets,
                                               uint32_t dyn_count, const uint32_t* dyn) {
  if (Stream* s = For(cb)) {
    s->Push([bp, layout, first, sets = SmallArray<VkDescriptorSet, 4>(sets, count),
             dyn = SmallArray<uint32_t, 8>(dyn, dyn_count)](VkCommandBuffer c) {
      real.vkCmdBindDescriptorSets(c, bp, layout, first, sets.n, sets.data(), dyn.n, dyn.data());
    });
  } else {
    real.vkCmdBindDescriptorSets(cb, bp, layout, first, count, sets, dyn_count, dyn);
  }
}

VKAPI_ATTR void VKAPI_CALL PushConstants(VkCommandBuffer cb, VkPipelineLayout layout,
                                          VkShaderStageFlags stages, uint32_t offset,
                                          uint32_t size, const void* values) {
  if (Stream* s = For(cb)) {
    s->Push([layout, stages, offset,
             bytes = SmallArray<uint8_t, 128>(static_cast<const uint8_t*>(values), size)](
                VkCommandBuffer c) {
      real.vkCmdPushConstants(c, layout, stages, offset, bytes.n, bytes.data());
    });
  } else {
    real.vkCmdPushConstants(cb, layout, stages, offset, size, values);
  }
}

VKAPI_ATTR void VKAPI_CALL BindIndexBuffer(VkCommandBuffer cb, VkBuffer buffer,
                                            VkDeviceSize offset, VkIndexType type) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdBindIndexBuffer(c, buffer, offset, type); });
  } else {
    real.vkCmdBindIndexBuffer(cb, buffer, offset, type);
  }
}

VKAPI_ATTR void VKAPI_CALL SetViewport(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                        const VkViewport* v) {
  if (Stream* s = For(cb)) {
    s->Push([first, v = SmallArray<VkViewport, 1>(v, count)](VkCommandBuffer c) {
      real.vkCmdSetViewport(c, first, v.n, v.data());
    });
  } else {
    real.vkCmdSetViewport(cb, first, count, v);
  }
}

VKAPI_ATTR void VKAPI_CALL SetScissor(VkCommandBuffer cb, uint32_t first, uint32_t count,
                                       const VkRect2D* r) {
  if (Stream* s = For(cb)) {
    s->Push([first, r = SmallArray<VkRect2D, 1>(r, count)](VkCommandBuffer c) {
      real.vkCmdSetScissor(c, first, r.n, r.data());
    });
  } else {
    real.vkCmdSetScissor(cb, first, count, r);
  }
}

VKAPI_ATTR void VKAPI_CALL SetBlendConstants(VkCommandBuffer cb, const float constants[4]) {
  if (Stream* s = For(cb)) {
    std::array<float, 4> k{constants[0], constants[1], constants[2], constants[3]};
    s->Push([k](VkCommandBuffer c) { real.vkCmdSetBlendConstants(c, k.data()); });
  } else {
    real.vkCmdSetBlendConstants(cb, constants);
  }
}

VKAPI_ATTR void VKAPI_CALL SetDepthBias(VkCommandBuffer cb, float constant, float clamp,
                                         float slope) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdSetDepthBias(c, constant, clamp, slope); });
  } else {
    real.vkCmdSetDepthBias(cb, constant, clamp, slope);
  }
}

VKAPI_ATTR void VKAPI_CALL SetStencilReference(VkCommandBuffer cb, VkStencilFaceFlags f,
                                                uint32_t v) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdSetStencilReference(c, f, v); });
  } else {
    real.vkCmdSetStencilReference(cb, f, v);
  }
}

VKAPI_ATTR void VKAPI_CALL SetStencilCompareMask(VkCommandBuffer cb, VkStencilFaceFlags f,
                                                  uint32_t v) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdSetStencilCompareMask(c, f, v); });
  } else {
    real.vkCmdSetStencilCompareMask(cb, f, v);
  }
}

VKAPI_ATTR void VKAPI_CALL SetStencilWriteMask(VkCommandBuffer cb, VkStencilFaceFlags f,
                                                uint32_t v) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdSetStencilWriteMask(c, f, v); });
  } else {
    real.vkCmdSetStencilWriteMask(cb, f, v);
  }
}

VKAPI_ATTR void VKAPI_CALL Draw(VkCommandBuffer cb, uint32_t vertices, uint32_t instances,
                                 uint32_t first_vertex, uint32_t first_instance) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) {
      real.vkCmdDraw(c, vertices, instances, first_vertex, first_instance);
    });
  } else {
    real.vkCmdDraw(cb, vertices, instances, first_vertex, first_instance);
  }
}

VKAPI_ATTR void VKAPI_CALL DrawIndexed(VkCommandBuffer cb, uint32_t indices, uint32_t instances,
                                        uint32_t first_index, int32_t vertex_offset,
                                        uint32_t first_instance) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) {
      real.vkCmdDrawIndexed(c, indices, instances, first_index, vertex_offset, first_instance);
    });
  } else {
    real.vkCmdDrawIndexed(cb, indices, instances, first_index, vertex_offset, first_instance);
  }
}

VKAPI_ATTR void VKAPI_CALL BeginRendering(VkCommandBuffer cb, const VkRenderingInfo* info) {
  if (Stream* s = For(cb)) {
    // The renderer chains nothing (pNext) onto these structures.
    struct Copied {
      VkRenderingInfo info;
      std::vector<VkRenderingAttachmentInfo> colors;
      bool has_depth, has_stencil;
      VkRenderingAttachmentInfo depth, stencil;
    };
    Copied c{*info, Copy(info->pColorAttachments, info->colorAttachmentCount),
             info->pDepthAttachment != nullptr, info->pStencilAttachment != nullptr,
             info->pDepthAttachment ? *info->pDepthAttachment : VkRenderingAttachmentInfo{},
             info->pStencilAttachment ? *info->pStencilAttachment : VkRenderingAttachmentInfo{}};
    s->Push([c = std::move(c)](VkCommandBuffer cmd) mutable {
      c.info.pColorAttachments = c.colors.empty() ? nullptr : c.colors.data();
      c.info.pDepthAttachment = c.has_depth ? &c.depth : nullptr;
      c.info.pStencilAttachment = c.has_stencil ? &c.stencil : nullptr;
      real.vkCmdBeginRendering(cmd, &c.info);
    });
  } else {
    real.vkCmdBeginRendering(cb, info);
  }
}

VKAPI_ATTR void VKAPI_CALL EndRendering(VkCommandBuffer cb) {
  if (Stream* s = For(cb)) {
    s->Push([](VkCommandBuffer c) { real.vkCmdEndRendering(c); });
  } else {
    real.vkCmdEndRendering(cb);
  }
}

VKAPI_ATTR void VKAPI_CALL PipelineBarrier(VkCommandBuffer cb, VkPipelineStageFlags src,
                                            VkPipelineStageFlags dst, VkDependencyFlags dep,
                                            uint32_t mem_count, const VkMemoryBarrier* mem,
                                            uint32_t buf_count,
                                            const VkBufferMemoryBarrier* buf,
                                            uint32_t img_count,
                                            const VkImageMemoryBarrier* img) {
  if (Stream* s = For(cb)) {
    s->Push([=, mem = SmallArray<VkMemoryBarrier, 1>(mem, mem_count),
             buf = Copy(buf, buf_count),
             img = SmallArray<VkImageMemoryBarrier, 1>(img, img_count)](VkCommandBuffer c) {
      real.vkCmdPipelineBarrier(c, src, dst, dep, mem.n, mem.data(), uint32_t(buf.size()),
                                buf.empty() ? nullptr : buf.data(), img.n, img.data());
    });
  } else {
    real.vkCmdPipelineBarrier(cb, src, dst, dep, mem_count, mem, buf_count, buf, img_count, img);
  }
}

VKAPI_ATTR void VKAPI_CALL CopyImage(VkCommandBuffer cb, VkImage src, VkImageLayout src_layout,
                                      VkImage dst, VkImageLayout dst_layout, uint32_t count,
                                      const VkImageCopy* regions) {
  if (Stream* s = For(cb)) {
    s->Push([=, r = Copy(regions, count)](VkCommandBuffer c) {
      real.vkCmdCopyImage(c, src, src_layout, dst, dst_layout, uint32_t(r.size()), r.data());
    });
  } else {
    real.vkCmdCopyImage(cb, src, src_layout, dst, dst_layout, count, regions);
  }
}

VKAPI_ATTR void VKAPI_CALL CopyBufferToImage(VkCommandBuffer cb, VkBuffer src, VkImage dst,
                                              VkImageLayout layout, uint32_t count,
                                              const VkBufferImageCopy* regions) {
  if (Stream* s = For(cb)) {
    s->Push([=, r = Copy(regions, count)](VkCommandBuffer c) {
      real.vkCmdCopyBufferToImage(c, src, dst, layout, uint32_t(r.size()), r.data());
    });
  } else {
    real.vkCmdCopyBufferToImage(cb, src, dst, layout, count, regions);
  }
}

VKAPI_ATTR void VKAPI_CALL CopyImageToBuffer(VkCommandBuffer cb, VkImage src,
                                              VkImageLayout layout, VkBuffer dst, uint32_t count,
                                              const VkBufferImageCopy* regions) {
  if (Stream* s = For(cb)) {
    s->Push([=, r = Copy(regions, count)](VkCommandBuffer c) {
      real.vkCmdCopyImageToBuffer(c, src, layout, dst, uint32_t(r.size()), r.data());
    });
  } else {
    real.vkCmdCopyImageToBuffer(cb, src, layout, dst, count, regions);
  }
}

VKAPI_ATTR void VKAPI_CALL ClearColorImage(VkCommandBuffer cb, VkImage image,
                                            VkImageLayout layout, const VkClearColorValue* color,
                                            uint32_t count,
                                            const VkImageSubresourceRange* ranges) {
  if (Stream* s = For(cb)) {
    s->Push([=, k = *color, r = Copy(ranges, count)](VkCommandBuffer c) {
      real.vkCmdClearColorImage(c, image, layout, &k, uint32_t(r.size()), r.data());
    });
  } else {
    real.vkCmdClearColorImage(cb, image, layout, color, count, ranges);
  }
}

VKAPI_ATTR void VKAPI_CALL ClearAttachments(VkCommandBuffer cb, uint32_t count,
                                             const VkClearAttachment* atts, uint32_t rect_count,
                                             const VkClearRect* rects) {
  if (Stream* s = For(cb)) {
    s->Push([a = Copy(atts, count), r = Copy(rects, rect_count)](VkCommandBuffer c) {
      real.vkCmdClearAttachments(c, uint32_t(a.size()), a.data(), uint32_t(r.size()), r.data());
    });
  } else {
    real.vkCmdClearAttachments(cb, count, atts, rect_count, rects);
  }
}

VKAPI_ATTR void VKAPI_CALL ResetQueryPool(VkCommandBuffer cb, VkQueryPool pool, uint32_t first,
                                           uint32_t count) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdResetQueryPool(c, pool, first, count); });
  } else {
    real.vkCmdResetQueryPool(cb, pool, first, count);
  }
}

VKAPI_ATTR void VKAPI_CALL WriteTimestamp(VkCommandBuffer cb, VkPipelineStageFlagBits stage,
                                           VkQueryPool pool, uint32_t query) {
  if (Stream* s = For(cb)) {
    s->Push([=](VkCommandBuffer c) { real.vkCmdWriteTimestamp(c, stage, pool, query); });
  } else {
    real.vkCmdWriteTimestamp(cb, stage, pool, query);
  }
}

constexpr size_t kChunkBytes = 64 * 1024;
// A chunk goes to the recording thread once it holds this much (a few dozen
// draws), so both threads work at once.
constexpr size_t kHandOverBytes = 8 * 1024;

}  // namespace

void Install() {
  // After every vk::LoadDevice (which sets the driver's functions again).
  static std::mutex mutex;
  std::lock_guard lock(mutex);
  if (vk::vkCmdDraw == Draw) return;  // already ours
  {
#define AMPED3_CMD_SAVE(name) real.name = vk::name;
    AMPED3_CMD_FUNCS(AMPED3_CMD_SAVE)
#undef AMPED3_CMD_SAVE
    vk::vkCmdBindPipeline = BindPipeline;
    vk::vkCmdBindDescriptorSets = BindDescriptorSets;
    vk::vkCmdPushConstants = PushConstants;
    vk::vkCmdBindIndexBuffer = BindIndexBuffer;
    vk::vkCmdSetViewport = SetViewport;
    vk::vkCmdSetScissor = SetScissor;
    vk::vkCmdSetBlendConstants = SetBlendConstants;
    vk::vkCmdSetDepthBias = SetDepthBias;
    vk::vkCmdSetStencilReference = SetStencilReference;
    vk::vkCmdSetStencilCompareMask = SetStencilCompareMask;
    vk::vkCmdSetStencilWriteMask = SetStencilWriteMask;
    vk::vkCmdDraw = Draw;
    vk::vkCmdDrawIndexed = DrawIndexed;
    vk::vkCmdBeginRendering = BeginRendering;
    vk::vkCmdEndRendering = EndRendering;
    vk::vkCmdPipelineBarrier = PipelineBarrier;
    vk::vkCmdCopyImage = CopyImage;
    vk::vkCmdCopyBufferToImage = CopyBufferToImage;
    vk::vkCmdCopyImageToBuffer = CopyImageToBuffer;
    vk::vkCmdClearColorImage = ClearColorImage;
    vk::vkCmdClearAttachments = ClearAttachments;
    vk::vkCmdResetQueryPool = ResetQueryPool;
    vk::vkCmdWriteTimestamp = WriteTimestamp;
  }
}

Stream* Stream::Current() { return t_stream; }

Stream::Stream() = default;

Stream::~Stream() {
  if (active()) End();
  {
    std::lock_guard lock(mutex_);
    stop_ = true;
  }
  work_cv_.notify_all();
  if (thread_.joinable()) thread_.join();
}

void Stream::Begin(VkCommandBuffer cb) {
  if (!thread_.joinable()) thread_ = std::thread([this] { Main(); });
  cb_ = cb;
  t_stream = this;
}

void Stream::Drain() {
  if (current_ && current_->used) HandOver();
  std::unique_lock lock(mutex_);
  idle_cv_.wait(lock, [this] { return queue_.empty() && !busy_; });
}

void Stream::End() {
  Drain();
  if (t_stream == this) t_stream = nullptr;
  cb_ = VK_NULL_HANDLE;
}

void* Stream::Reserve(size_t size) {
  if (!current_ || current_->used + size > current_->capacity) {
    if (current_ && current_->used) HandOver();
    if (!current_) {
      std::lock_guard lock(mutex_);
      if (!free_.empty() && free_.back()->capacity >= size) {
        current_ = free_.back();
        free_.pop_back();
      } else {
        auto c = std::make_unique<Chunk>();
        c->capacity = std::max(kChunkBytes, size);
        c->data.reset(new unsigned char[c->capacity + kAlign]);
        current_ = c.get();
        chunks_.push_back(std::move(c));
      }
      current_->used = 0;
    }
  }
  // 16-byte aligned start (the block itself may not be).
  unsigned char* base = current_->data.get();
  const size_t pad = (kAlign - (reinterpret_cast<uintptr_t>(base) & (kAlign - 1))) & (kAlign - 1);
  return base + pad + current_->used;
}

void Stream::Commit(size_t size) {
  current_->used += size;
  if (current_->used >= kHandOverBytes) HandOver();
}

void Stream::HandOver() {
  Chunk* c = current_;
  current_ = nullptr;
  {
    std::lock_guard lock(mutex_);
    queue_.push_back(c);
  }
  work_cv_.notify_one();
}

void Stream::Main() {
  std::unique_lock lock(mutex_);
  for (;;) {
    work_cv_.wait(lock, [this] { return stop_ || !queue_.empty(); });
    if (queue_.empty()) {
      if (stop_) return;
      continue;
    }
    Chunk* c = queue_.front();
    queue_.erase(queue_.begin());
    busy_ = true;
    const VkCommandBuffer cb = cb_;
    lock.unlock();
    unsigned char* base = c->data.get();
    base += (kAlign - (reinterpret_cast<uintptr_t>(base) & (kAlign - 1))) & (kAlign - 1);
    for (size_t at = 0; at < c->used;) {
      Entry* e = reinterpret_cast<Entry*>(base + at);
      e->run(e + 1, cb);
      at += e->size;
    }
    c->used = 0;
    lock.lock();
    free_.push_back(c);
    busy_ = false;
    if (queue_.empty()) idle_cv_.notify_all();
  }
}

}  // namespace amped3::native::cmd
