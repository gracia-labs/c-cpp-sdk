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

  // Dropping an entry here leaves volk with null entry points for the SDK.
  static const char* const kWanted[] = {
      VK_KHR_DEPTH_STENCIL_RESOLVE_EXTENSION_NAME,
      VK_KHR_CREATE_RENDERPASS_2_EXTENSION_NAME,
      VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
      VK_KHR_MULTIVIEW_EXTENSION_NAME,
      VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
      VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
      VK_EXT_SHADER_VIEWPORT_INDEX_LAYER_EXTENSION_NAME,
      VK_EXT_SAMPLER_FILTER_MINMAX_EXTENSION_NAME,
      VK_KHR_SHADER_SUBGROUP_EXTENDED_TYPES_EXTENSION_NAME,
      VK_EXT_SCALAR_BLOCK_LAYOUT_EXTENSION_NAME,
      VK_EXT_TOOLING_INFO_EXTENSION_NAME,
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

bool initVulkanLoader() {
  static const bool ok = volkInitialize() == VK_SUCCESS;
  if (!ok)
    std::fprintf(stderr, "Vulkan loader not found (install a Vulkan runtime)\n");
  return ok;
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

VkRenderPass createColorRenderPass(VkDevice device, VkFormat format,
                                   VkImageLayout finalLayout,
                                   uint32_t viewMask) {
  VkAttachmentDescription color{};
  color.format = format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = finalLayout;

  VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription subpass{};
  subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpass.colorAttachmentCount = 1;
  subpass.pColorAttachments = &colorRef;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;

  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.attachmentCount = 1;
  rp.pAttachments = &color;
  rp.subpassCount = 1;
  rp.pSubpasses = &subpass;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;

  // The correlation mask tells the driver the views run together on one GPU,
  // which is what lets it merge them instead of scheduling two passes.
  VkRenderPassMultiviewCreateInfo mv{
      VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
  mv.subpassCount = 1;
  mv.pViewMasks = &viewMask;
  mv.correlationMaskCount = 1;
  mv.pCorrelationMasks = &viewMask;
  if (viewMask) rp.pNext = &mv;

  VkRenderPass pass = VK_NULL_HANDLE;
  VK_CHECK(vkCreateRenderPass(device, &rp, nullptr, &pass));
  return pass;
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
