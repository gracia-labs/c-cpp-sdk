#include <gracia_demo/vk_common.hpp>

#include <cstring>
#include <set>

namespace gvk {

QueueFamilies pickQueueFamilies(VkPhysicalDevice gpu) {
  uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> q(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(gpu, &qn, q.data());

  auto pick = [&](VkQueueFlags need, VkQueueFlags avoid) -> uint32_t {
    for (uint32_t i = 0; i < qn; ++i)
      if (q[i].queueCount && (q[i].queueFlags & need) && !(q[i].queueFlags & avoid))
        return i;
    return ~0u;
  };

  QueueFamilies f;
  f.graphics = pick(VK_QUEUE_GRAPHICS_BIT, 0);
  // Prefer a pure-transfer family, then transfer-without-graphics, then fall back.
  f.transfer = pick(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT);
  if (f.transfer == ~0u) f.transfer = pick(VK_QUEUE_TRANSFER_BIT, VK_QUEUE_GRAPHICS_BIT);
  if (f.transfer == ~0u) f.transfer = f.graphics;
  return f;
}

void printQueueFamilies(const QueueFamilies& q) {
  std::printf("Queues: graphics=%u transfer=%u%s\n", q.graphics, q.transfer,
              q.transferSharesGraphics() ? "  (transfer shares graphics!)" : "");
}

DeviceRequest::DeviceRequest(VkPhysicalDevice gpu, const QueueFamilies& families,
                             bool wantSwapchain) {
  // ~0u means "no such queue" and must not reach vkCreateDevice.
  const std::set<uint32_t> unique{families.graphics, families.compute,
                                  families.transfer};
  for (uint32_t f : unique) {
    if (f == ~0u) continue;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = f;
    qci.queueCount = 1;
    qci.pQueuePriorities = &priority_;
    queues_.push_back(qci);
  }

  uint32_t availCount = 0;
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &availCount, nullptr);
  available_.resize(availCount);
  vkEnumerateDeviceExtensionProperties(gpu, nullptr, &availCount, available_.data());

  // The SDK resolves these by their KHR names, so the promoted core
  // versions alone are not enough. Everything else it uses is a feature below.
  static const char* const kWanted[] = {
      VK_KHR_DYNAMIC_RENDERING_EXTENSION_NAME,
      VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
      VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
  };
  if (wantSwapchain && has(VK_KHR_SWAPCHAIN_EXTENSION_NAME))
    extensions_.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
  for (const char* e : kWanted)
    if (has(e)) extensions_.push_back(e);

  f13_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES;
  f12_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES;
  f11_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
  f2_.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
  f12_.pNext = &f13_;
  f11_.pNext = &f12_;
  f2_.pNext = &f11_;
  vkGetPhysicalDeviceFeatures2(gpu, &f2_);

  ci_.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  ci_.pNext = &f2_;
  ci_.queueCreateInfoCount = (uint32_t)queues_.size();
  ci_.pQueueCreateInfos = queues_.data();
  ci_.enabledExtensionCount = (uint32_t)extensions_.size();
  ci_.ppEnabledExtensionNames = extensions_.data();
}

bool DeviceRequest::has(const char* name) const {
  for (const auto& e : available_)
    if (std::strcmp(e.extensionName, name) == 0) return true;
  return false;
}

VkPipelineCache createPipelineCache(VkDevice device) {
  VkPipelineCacheCreateInfo pci{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
  VkPipelineCache cache = VK_NULL_HANDLE;
  VK_CHECK(vkCreatePipelineCache(device, &pci, nullptr, &cache));
  return cache;
}

VkImageView createColorView(VkDevice device, VkImage image, VkFormat format) {
  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = image;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vi.format = format;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  VK_CHECK(vkCreateImageView(device, &vi, nullptr, &view));
  return view;
}

namespace {

void colorImageBarrier(VkCommandBuffer cmd, VkImage image, VkImageLayout from,
                       VkImageLayout to, VkAccessFlags srcAccess,
                       VkAccessFlags dstAccess, VkPipelineStageFlags srcStage,
                       VkPipelineStageFlags dstStage) {
  VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
  b.srcAccessMask = srcAccess;
  b.dstAccessMask = dstAccess;
  b.oldLayout = from;
  b.newLayout = to;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = image;
  b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdPipelineBarrier(cmd, srcStage, dstStage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

void beginColorRendering(VkCommandBuffer cmd, VkImage image, VkImageView view,
                         VkExtent2D extent, const VkClearValue& clear) {
  // A render pass did this through initialLayout and a subpass dependency.
  colorImageBarrier(cmd, image, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                    VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  VkRenderingAttachmentInfoKHR color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO_KHR};
  color.imageView = view;
  color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.clearValue = clear;

  VkRenderingInfoKHR info{VK_STRUCTURE_TYPE_RENDERING_INFO_KHR};
  info.renderArea = {{0, 0}, extent};
  info.layerCount = 1;
  info.colorAttachmentCount = 1;
  info.pColorAttachments = &color;
  vkCmdBeginRenderingKHR(cmd, &info);
}

void endColorRendering(VkCommandBuffer cmd, VkImage image,
                       VkImageLayout finalLayout) {
  vkCmdEndRenderingKHR(cmd);
  if (finalLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL) return;
  colorImageBarrier(cmd, image, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    finalLayout, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, 0,
                    VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void barrierComputeToGraphics(VkCommandBuffer cmd) {
  VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
  mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
  mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_UNIFORM_READ_BIT |
                     VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                     VK_ACCESS_INDIRECT_COMMAND_READ_BIT;
  vkCmdPipelineBarrier(
      cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
      VK_PIPELINE_STAGE_VERTEX_INPUT_BIT | VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
          VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_DRAW_INDIRECT_BIT,
      0, 1, &mb, 0, nullptr, 0, nullptr);
}

}  // namespace gvk
