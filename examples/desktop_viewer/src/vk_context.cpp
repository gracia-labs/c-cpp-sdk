#include "vk_context.hpp"

#include <GLFW/glfw3.h>

#include <algorithm>

namespace {

VkSurfaceFormatKHR chooseSurfaceFormat(VkPhysicalDevice gpu, VkSurfaceKHR surface) {
  uint32_t n = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, nullptr);
  std::vector<VkSurfaceFormatKHR> formats(n);
  vkGetPhysicalDeviceSurfaceFormatsKHR(gpu, surface, &n, formats.data());
  for (const auto& f : formats)
    if ((f.format == VK_FORMAT_B8G8R8A8_UNORM ||
         f.format == VK_FORMAT_R8G8B8A8_UNORM) &&
        f.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
      return f;
  return formats.empty()
             ? VkSurfaceFormatKHR{VK_FORMAT_B8G8R8A8_UNORM,
                                  VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}
             : formats[0];
}

}  // namespace

bool VulkanContext::init(GLFWwindow* window, const char* appName) {
  window_ = window;
  if (!createInstance(window, appName)) return false;
  if (!pickPhysicalDevice()) return false;
  if (!createDevice()) return false;
  createSwapchain();
  frames_.init(device_, queues_.graphics, kFramesInFlight);

  imageAvailable_.resize(kFramesInFlight);
  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < kFramesInFlight; ++i)
    VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &imageAvailable_[i]));
  return true;
}

bool VulkanContext::createInstance(GLFWwindow* window, const char* appName) {
  if (volkInitialize() != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan loader not found (install a Vulkan runtime)\n");
    return false;
  }

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = appName;
  app.pEngineName = "gracia-demo";
  app.apiVersion = VK_API_VERSION_1_3;

  uint32_t extCount = 0;
  const char** exts = glfwGetRequiredInstanceExtensions(&extCount);
  VkInstanceCreateInfo ci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  ci.pApplicationInfo = &app;
  ci.enabledExtensionCount = extCount;
  ci.ppEnabledExtensionNames = exts;
  VK_CHECK(vkCreateInstance(&ci, nullptr, &instance_));
  volkLoadInstanceOnly(instance_);

  VK_CHECK(glfwCreateWindowSurface(instance_, window, nullptr, &surface_));
  return true;
}

bool VulkanContext::pickPhysicalDevice() {
  uint32_t n = 0;
  vkEnumeratePhysicalDevices(instance_, &n, nullptr);
  if (n == 0) {
    std::fprintf(stderr, "No Vulkan-capable GPU found\n");
    return false;
  }
  std::vector<VkPhysicalDevice> devices(n);
  vkEnumeratePhysicalDevices(instance_, &n, devices.data());

  physicalDevice_ = devices[0];
  for (auto d : devices) {
    VkPhysicalDeviceProperties p{};
    vkGetPhysicalDeviceProperties(d, &p);
    if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) {
      physicalDevice_ = d;
      break;
    }
  }

  queues_ = gvk::pickQueueFamilies(physicalDevice_);

  // The graphics family must also present, or the swapchain is not usable.
  VkBool32 present = VK_FALSE;
  vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, queues_.graphics, surface_,
                                       &present);
  if (!present) {
    uint32_t qn = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qn, nullptr);
    std::vector<VkQueueFamilyProperties> q(qn);
    vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice_, &qn, q.data());
    for (uint32_t i = 0; i < qn; ++i) {
      VkBool32 p = VK_FALSE;
      vkGetPhysicalDeviceSurfaceSupportKHR(physicalDevice_, i, surface_, &p);
      if (p && (q[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)) {
        queues_.graphics = i;
        break;
      }
    }
  }
  if (!queues_.valid()) {
    std::fprintf(stderr, "No graphics+present queue family\n");
    return false;
  }

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &props);
  std::printf("GPU: %s\n", props.deviceName);
  gvk::printQueueFamilies(queues_);
  return true;
}

bool VulkanContext::createDevice() {
  // Present on the SAME queue the SDK uses (graphics family, index 0): the SDK
  // tracks its per-frame buffer ring against that queue, so submitting our frames
  // elsewhere corrupts its accounting (splats flicker).
  const gvk::DeviceRequest request(physicalDevice_, queues_, /*wantSwapchain=*/true);
  VK_CHECK(vkCreateDevice(physicalDevice_, &request.info(), nullptr, &device_));
  volkLoadDevice(device_);
  vkGetDeviceQueue(device_, queues_.graphics, 0, &graphicsQueue_);
  return true;
}

void VulkanContext::createSwapchain() {
  VkSurfaceCapabilitiesKHR caps{};
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physicalDevice_, surface_, &caps);
  const VkSurfaceFormatKHR fmt = chooseSurfaceFormat(physicalDevice_, surface_);
  colorFormat_ = fmt.format;

  if (caps.currentExtent.width != UINT32_MAX) {
    extent_ = caps.currentExtent;
  } else {
    int w = 0, h = 0;
    glfwGetFramebufferSize(window_, &w, &h);
    extent_.width = std::clamp((uint32_t)w, caps.minImageExtent.width,
                               caps.maxImageExtent.width);
    extent_.height = std::clamp((uint32_t)h, caps.minImageExtent.height,
                                caps.maxImageExtent.height);
  }

  minImageCount_ = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && minImageCount_ > caps.maxImageCount)
    minImageCount_ = caps.maxImageCount;

  VkSwapchainCreateInfoKHR ci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
  ci.surface = surface_;
  ci.minImageCount = minImageCount_;
  ci.imageFormat = colorFormat_;
  ci.imageColorSpace = fmt.colorSpace;
  ci.imageExtent = extent_;
  ci.imageArrayLayers = 1;
  ci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  ci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ci.preTransform = caps.currentTransform;
  ci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  ci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  ci.clipped = VK_TRUE;
  VK_CHECK(vkCreateSwapchainKHR(device_, &ci, nullptr, &swapchain_));

  uint32_t count = 0;
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
  swapchainImages_.resize(count);
  vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchainImages_.data());

  swapchainViews_.resize(count);
  renderFinished_.resize(count);
  imagesInFlight_.assign(count, VK_NULL_HANDLE);

  VkSemaphoreCreateInfo si{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
  for (uint32_t i = 0; i < count; ++i) {
    swapchainViews_[i] = gvk::createColorView(device_, swapchainImages_[i], colorFormat_);
    VK_CHECK(vkCreateSemaphore(device_, &si, nullptr, &renderFinished_[i]));
  }
}

std::optional<Frame> VulkanContext::beginFrame() {
  int w = 0, h = 0;
  glfwGetFramebufferSize(window_, &w, &h);
  if (w == 0 || h == 0) return std::nullopt;

  frames_.waitSlot(device_);

  uint32_t imageIndex = 0;
  VkResult acq = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX,
                                       imageAvailable_[frames_.slot()],
                                       VK_NULL_HANDLE, &imageIndex);
  if (acq == VK_ERROR_OUT_OF_DATE_KHR) {
    recreateSwapchain();
    return std::nullopt;
  }
  if (acq != VK_SUCCESS && acq != VK_SUBOPTIMAL_KHR) VK_CHECK(acq);

  if (imagesInFlight_[imageIndex] != VK_NULL_HANDLE)
    VK_CHECK(vkWaitForFences(device_, 1, &imagesInFlight_[imageIndex], VK_TRUE,
                             UINT64_MAX));
  imagesInFlight_[imageIndex] = frames_.fence();

  return Frame{frames_.beginCmd(device_), imageIndex, frames_.slot(), extent_};
}

void VulkanContext::render(const Frame& frame,
                           const std::function<void(VkCommandBuffer)>& body) {
  gvk::barrierComputeToGraphics(frame.cmd);

  VkClearValue clear{};
  clear.color = {{0.02f, 0.02f, 0.03f, 1.0f}};
  gvk::beginColorRendering(frame.cmd, swapchainImages_[frame.imageIndex],
                           swapchainViews_[frame.imageIndex], extent_, clear);

  VkViewport vp{0.0f, 0.0f, (float)extent_.width, (float)extent_.height, 0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, extent_};
  vkCmdSetViewport(frame.cmd, 0, 1, &vp);
  vkCmdSetScissor(frame.cmd, 0, 1, &scissor);

  body(frame.cmd);
  gvk::endColorRendering(frame.cmd, swapchainImages_[frame.imageIndex],
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
}

void VulkanContext::endFrame(const Frame& frame) {
  VK_CHECK(vkEndCommandBuffer(frame.cmd));
  const VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
  submit.waitSemaphoreCount = 1;
  submit.pWaitSemaphores = &imageAvailable_[frame.slot];
  submit.pWaitDstStageMask = &wait;
  submit.commandBufferCount = 1;
  submit.pCommandBuffers = &frame.cmd;
  submit.signalSemaphoreCount = 1;
  submit.pSignalSemaphores = &renderFinished_[frame.imageIndex];
  VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submit, frames_.fence()));

  VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
  present.waitSemaphoreCount = 1;
  present.pWaitSemaphores = &renderFinished_[frame.imageIndex];
  present.swapchainCount = 1;
  present.pSwapchains = &swapchain_;
  present.pImageIndices = &frame.imageIndex;
  VkResult res = vkQueuePresentKHR(graphicsQueue_, &present);
  if (res == VK_ERROR_OUT_OF_DATE_KHR || res == VK_SUBOPTIMAL_KHR || resized_) {
    resized_ = false;
    recreateSwapchain();
  } else if (res != VK_SUCCESS) {
    VK_CHECK(res);
  }
  frames_.advance();
}

void VulkanContext::destroySwapchainResources() {
  for (auto v : swapchainViews_) vkDestroyImageView(device_, v, nullptr);
  for (auto s : renderFinished_) vkDestroySemaphore(device_, s, nullptr);
  swapchainViews_.clear();
  renderFinished_.clear();
  if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
  swapchain_ = VK_NULL_HANDLE;
}

void VulkanContext::recreateSwapchain() {
  int w = 0, h = 0;
  glfwGetFramebufferSize(window_, &w, &h);
  while (w == 0 || h == 0) {
    glfwWaitEvents();
    glfwGetFramebufferSize(window_, &w, &h);
  }
  vkDeviceWaitIdle(device_);
  destroySwapchainResources();
  createSwapchain();
}

void VulkanContext::shutdown() {
  if (!device_) return;
  vkDeviceWaitIdle(device_);
  destroySwapchainResources();
  for (auto s : imageAvailable_) vkDestroySemaphore(device_, s, nullptr);
  imageAvailable_.clear();
  frames_.destroy(device_);
  vkDestroyDevice(device_, nullptr);
  if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
  if (instance_) vkDestroyInstance(instance_, nullptr);
  device_ = VK_NULL_HANDLE;
  instance_ = VK_NULL_HANDLE;
}
