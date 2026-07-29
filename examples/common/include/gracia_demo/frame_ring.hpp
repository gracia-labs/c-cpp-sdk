#pragma once

#include <gracia_demo/vk_common.hpp>

#include <vector>

namespace gvk {

// Command buffers and fences for the frames in flight. Stay at or below the
// SDK's ring depth of 3, and submit on the one queue the SDK tracks.
class FrameRing {
 public:
  void init(VkDevice device, uint32_t graphicsFamily, uint32_t count);
  void destroy(VkDevice device);

  uint32_t slot() const { return slot_; }
  uint32_t count() const { return (uint32_t)fences_.size(); }
  VkFence fence() const { return fences_[slot_]; }
  VkCommandBuffer cmd() const { return cmds_[slot_]; }

  // Draw calls in this slot are safe to free only after this returns.
  void waitSlot(VkDevice device) const;
  // Separate from waitSlot so a frame can be abandoned in between.
  VkCommandBuffer beginCmd(VkDevice device);
  void advance() { slot_ = (slot_ + 1) % (uint32_t)fences_.size(); }

 private:
  VkCommandPool pool_ = VK_NULL_HANDLE;
  std::vector<VkCommandBuffer> cmds_;
  std::vector<VkFence> fences_;
  uint32_t slot_ = 0;
};

}  // namespace gvk
