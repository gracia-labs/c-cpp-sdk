#pragma once

#include <gracia_demo/frame_ring.hpp>
#include <gracia_demo/vk_common.hpp>

#include <functional>
#include <optional>
#include <vector>

struct GLFWwindow;

struct Frame {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint32_t imageIndex = 0;
  uint32_t slot = 0;
  VkExtent2D extent{};
};

// Minimal Vulkan device + swapchain: the splats and the ImGui overlay render
// straight into the swapchain image with dynamic rendering, so there is no
// VkRenderPass and no framebuffer. The device setup and the frame ring come
// from gracia_demo.
class VulkanContext {
 public:
  // Matches the SDK's per-frame ring depth (fuji uses 3); stay at or below it and
  // submit every frame on the one queue the SDK tracks.
  static constexpr uint32_t kFramesInFlight = 3;

  bool init(GLFWwindow* window, const char* appName);
  void shutdown();

  gvk::Gpu gpu() const {
    return {instance_, physicalDevice_, device_, pipelineCache_, queues_};
  }
  VkQueue graphicsQueue() const { return graphicsQueue_; }
  VkFormat colorFormat() const { return colorFormat_; }
  uint32_t minImageCount() const { return minImageCount_; }
  uint32_t imageCount() const { return (uint32_t)swapchainImages_.size(); }

  // beginFrame returns nullopt when the frame should be skipped (minimized /
  // out-of-date swapchain). render() runs `body` inside a cleared color
  // rendering scope with viewport/scissor set.
  std::optional<Frame> beginFrame();
  void render(const Frame& frame, const std::function<void(VkCommandBuffer)>& body);
  void endFrame(const Frame& frame);

  void requestResize() { resized_ = true; }
  void waitIdle() const {
    if (device_) vkDeviceWaitIdle(device_);
  }

 private:
  bool createInstance(GLFWwindow* window, const char* appName);
  bool pickPhysicalDevice();
  bool createDevice();
  void createSwapchain();
  void destroySwapchainResources();
  void recreateSwapchain();

  GLFWwindow* window_ = nullptr;
  VkInstance instance_ = VK_NULL_HANDLE;
  VkSurfaceKHR surface_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;

  gvk::QueueFamilies queues_;
  VkQueue graphicsQueue_ = VK_NULL_HANDLE;  // shared: SDK graphics + our present

  VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
  VkFormat colorFormat_ = VK_FORMAT_UNDEFINED;
  VkExtent2D extent_{};
  uint32_t minImageCount_ = 2;
  std::vector<VkImage> swapchainImages_;
  std::vector<VkImageView> swapchainViews_;

  gvk::FrameRing frames_;
  std::vector<VkSemaphore> imageAvailable_;  // per in-flight frame
  std::vector<VkSemaphore> renderFinished_;  // per swapchain image
  std::vector<VkFence> imagesInFlight_;
  bool resized_ = false;
};
