#include <gracia_demo/frame_ring.hpp>

namespace gvk {

void FrameRing::init(VkDevice device, uint32_t graphicsFamily, uint32_t count) {
  VkCommandPoolCreateInfo pi{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
  pi.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  pi.queueFamilyIndex = graphicsFamily;
  VK_CHECK(vkCreateCommandPool(device, &pi, nullptr, &pool_));

  cmds_.resize(count);
  VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
  ai.commandPool = pool_;
  ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ai.commandBufferCount = count;
  VK_CHECK(vkAllocateCommandBuffers(device, &ai, cmds_.data()));

  // Signaled: the first frame in each slot must not wait.
  fences_.resize(count);
  VkFenceCreateInfo fi{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, nullptr,
                       VK_FENCE_CREATE_SIGNALED_BIT};
  for (uint32_t i = 0; i < count; ++i)
    VK_CHECK(vkCreateFence(device, &fi, nullptr, &fences_[i]));
}

void FrameRing::destroy(VkDevice device) {
  for (auto f : fences_) vkDestroyFence(device, f, nullptr);
  fences_.clear();
  cmds_.clear();
  if (pool_) vkDestroyCommandPool(device, pool_, nullptr);
  pool_ = VK_NULL_HANDLE;
  slot_ = 0;
}

void FrameRing::waitSlot(VkDevice device) const {
  VK_CHECK(vkWaitForFences(device, 1, &fences_[slot_], VK_TRUE, UINT64_MAX));
}

VkCommandBuffer FrameRing::beginCmd(VkDevice device) {
  VK_CHECK(vkResetFences(device, 1, &fences_[slot_]));
  VkCommandBuffer cmd = cmds_[slot_];
  VK_CHECK(vkResetCommandBuffer(cmd, 0));
  VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
  VK_CHECK(vkBeginCommandBuffer(cmd, &bi));
  return cmd;
}

}  // namespace gvk
