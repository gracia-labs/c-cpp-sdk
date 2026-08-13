#pragma once

#include <volk.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

#define VK_CHECK(expr)                                                        \
  do {                                                                        \
    VkResult vk_check_result_ = (expr);                                       \
    if (vk_check_result_ != VK_SUCCESS) {                                     \
      std::fprintf(stderr, "Vulkan error %d at %s:%d\n",                      \
                   (int)vk_check_result_, __FILE__, __LINE__);                \
      std::abort();                                                           \
    }                                                                         \
  } while (0)

namespace gvk {

// A real transfer queue matters: the SDK streams uploads on it while we render.
// Compute is unused, so it stays ~0u and the SDK leaves that queue null.
struct QueueFamilies {
  uint32_t graphics = ~0u;
  uint32_t compute = ~0u;  // unused, deliberately
  uint32_t transfer = ~0u;

  bool valid() const { return graphics != ~0u; }
  bool transferSharesGraphics() const { return transfer == graphics; }
};

QueueFamilies pickQueueFamilies(VkPhysicalDevice gpu);
void printQueueFamilies(const QueueFamilies& q);

// What a context hands the SDK. Both the window and the headset path fill it.
struct Gpu {
  VkInstance instance = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
  VkDevice device = VK_NULL_HANDLE;
  VkPipelineCache pipelineCache = VK_NULL_HANDLE;
  QueueFamilies queues;
};

// A VkDeviceCreateInfo plus everything it points at, which must outlive the
// create call. vkCreateDevice takes it on the desktop, OpenXR on the headset.
class DeviceRequest {
 public:
  DeviceRequest(VkPhysicalDevice gpu, const QueueFamilies& families,
                bool wantSwapchain);

  DeviceRequest(const DeviceRequest&) = delete;
  DeviceRequest& operator=(const DeviceRequest&) = delete;

  const VkDeviceCreateInfo& info() const { return ci_; }

 private:
  bool has(const char* name) const;

  std::vector<VkExtensionProperties> available_;
  std::vector<const char*> extensions_;
  std::vector<VkDeviceQueueCreateInfo> queues_;
  float priority_ = 1.0f;

  VkPhysicalDeviceVulkan13Features f13_{};
  VkPhysicalDeviceVulkan12Features f12_{};
  VkPhysicalDeviceVulkan11Features f11_{};
  VkPhysicalDeviceFeatures2 f2_{};
  VkDeviceCreateInfo ci_{};
};

// Loads the Vulkan entry points through volk, which owns them for the whole
// process: the SDK, Dear ImGui and GLFW all resolve through this one loader.
// Safe to call more than once; the first call decides. Call it before anything
// that touches Vulkan, including glfwInitVulkanLoader.
bool initVulkanLoader();

VkPipelineCache createPipelineCache(VkDevice device);
VkImageView createColorView(VkDevice device, VkImage image, VkFormat format);

// Color only. finalLayout is PRESENT_SRC_KHR for a window,
// COLOR_ATTACHMENT_OPTIMAL for OpenXR.
//
// `viewMask` turns the subpass into a multiview one: 0b11 makes every draw run
// twice and write layer 0 and layer 1 of an array attachment, which is stereo in
// one pass. Give render() the stereo mode that matches
// (GRACIA_STEREO_MODE_MULTIVIEW); the pass and the mode must describe the same
// thing.
VkRenderPass createColorRenderPass(VkDevice device, VkFormat format,
                                   VkImageLayout finalLayout,
                                   uint32_t viewMask = 0);

// Makes the SDK's compute prep visible to the splat draws.
void barrierComputeToGraphics(VkCommandBuffer cmd);

}  // namespace gvk
