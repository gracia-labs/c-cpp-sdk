#pragma once

// Include order matters: openxr_platform.h declares none of its own
// prerequisites and needs the Vulkan types in scope.
#include <windows.h>
#include <unknwn.h>

#include <gracia_demo/frame_ring.hpp>
#include <gracia_demo/vk_common.hpp>

#include <gracia/SDK.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include <array>
#include <chrono>
#include <functional>
#include <optional>
#include <string>
#include <vector>

// Never aborts: no runtime and no headset are normal outcomes.
#define XR_TRY(expr, what)                                                    \
  do {                                                                        \
    XrResult xr_try_result_ = (expr);                                         \
    if (XR_FAILED(xr_try_result_)) {                                          \
      std::fprintf(stderr, "OpenXR error %d in %s (%s:%d)\n",                 \
                   (int)xr_try_result_, (what), __FILE__, __LINE__);          \
      return false;                                                           \
    }                                                                         \
  } while (0)

// One in-flight XR frame. `views` hold the per-eye pose and fov from xrLocateViews.
struct XrFrameCtx {
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  uint32_t slot = 0;
  std::array<XrView, 2> views{};  // 0 = left, 1 = right (spec order)
  bool acquired = false;
  VkExtent2D eyeExtent{};  // per eye, not the combined width
};

// OpenXR session plus the Vulkan device it chose, and a color-only render pass
// the splats replay into. One swapchain of two array layers, filled by one
// multiview pass, into a 16-bit float target.
class XrContext {
 public:
  // Two, not three: xrWaitFrame paces us, so a third only adds latency.
  static constexpr uint32_t kFramesInFlight = 2;
  static constexpr uint32_t kEyes = 2;
  // One bit for each eye. The render pass, the stereo mode given to render() and
  // the layer count of the swapchain all have to agree with this.
  static constexpr uint32_t kViewMask = 0b11;

  // `float16` asks for an RGBA16F swapchain instead of the 8-bit sRGB one.
  bool init(const char* appName, bool float16 = false);
  void shutdown();

  gvk::Gpu gpu() const {
    return {vkInstance_, physicalDevice_, device_, pipelineCache_, queues_};
  }
  VkRenderPass renderPass() const { return renderPass_; }
  // Always matches the render pass attachment and the XR swapchain format.
  GraciaColorFormat colorFormat() const { return graciaColorFormat_; }
  const std::string& runtimeName() const { return runtimeName_; }
  void waitIdle() const {
    if (device_) vkDeviceWaitIdle(device_);
  }

  // Drains the event queue. False means the loop must exit.
  // `quitRequested` asks the runtime to end the session rather than dropping it.
  bool pollEvents(bool quitRequested);
  bool sessionRunning() const { return sessionRunning_; }

  XrFrameState waitFrame();
  void beginFrame();  // always paired with endFrame, even with nothing to draw

  // Nullopt when there is nothing to draw; endFrame still runs.
  std::optional<XrFrameCtx> acquire(const XrFrameState& fs);

  // Runs body once, in a cleared multiview pass that covers both eyes.
  void render(XrFrameCtx& f, const std::function<void(VkCommandBuffer)>& body);

  void endFrame(const XrFrameState& fs, std::optional<XrFrameCtx>& f);

 private:
  bool createXrInstance(const char* appName);
  bool loadXrExtensionFns();
  bool getSystem();
  bool createVulkan();
  bool createSession();
  bool createSwapchains(bool wantFloat16);
  void createFramebuffers();
  // --fp16 only. Two subpasses: the splats into a float intermediate, then the
  // sRGB decode from it into the swapchain.
  VkRenderPass createResolveRenderPass();
  // The intermediate image, its descriptor, and the subpass-1 pipeline.
  bool createResolve();

  XrInstance instance_ = XR_NULL_HANDLE;
  XrSystemId systemId_ = XR_NULL_SYSTEM_ID;
  XrSession session_ = XR_NULL_HANDLE;
  XrSpace appSpace_ = XR_NULL_HANDLE;
  XrSessionState state_ = XR_SESSION_STATE_UNKNOWN;
  bool sessionRunning_ = false;
  bool exitLoop_ = false;
  bool exitRequested_ = false;
  std::chrono::steady_clock::time_point exitRequestedAt_{};
  std::string runtimeName_;
  XrEnvironmentBlendMode blendMode_ = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;

  // The loader exports none of these; they come from xrGetInstanceProcAddr.
  PFN_xrGetVulkanGraphicsRequirements2KHR pfnGetVkReq_ = nullptr;
  PFN_xrCreateVulkanInstanceKHR pfnCreateVkInstance_ = nullptr;
  PFN_xrGetVulkanGraphicsDevice2KHR pfnGetVkDevice_ = nullptr;
  PFN_xrCreateVulkanDeviceKHR pfnCreateVkDevice_ = nullptr;

  // One swapchain, two array layers: layer 0 is the left eye, layer 1 the right.
  XrSwapchain swapchain_ = XR_NULL_HANDLE;
  std::vector<XrSwapchainImageVulkan2KHR> images_;  // the runtime owns the VkImages
  std::vector<VkImageView> views_;                  // 2D_ARRAY, both layers
  std::vector<VkFramebuffer> framebuffers_;
  std::array<XrViewConfigurationView, kEyes> cfgViews_{};
  std::array<XrCompositionLayerProjectionView, kEyes> projViews_{};
  VkExtent2D eyeExtent_{};

  VkInstance vkInstance_ = VK_NULL_HANDLE;
  VkPhysicalDevice physicalDevice_ = VK_NULL_HANDLE;
  VkDevice device_ = VK_NULL_HANDLE;
  VkPipelineCache pipelineCache_ = VK_NULL_HANDLE;
  gvk::QueueFamilies queues_;
  VkQueue graphicsQueue_ = VK_NULL_HANDLE;  // shared: SDK submits + XR runtime

  // What OpenXR is told the swapchain holds, and what we render through. They
  // differ so neither end of the chain gamma-encodes: see createSwapchains.
  VkFormat swapchainFormat_ = VK_FORMAT_UNDEFINED;
  VkFormat viewFormat_ = VK_FORMAT_UNDEFINED;
  GraciaColorFormat graciaColorFormat_ = GRACIA_COLOR_FORMAT_RGBA8_UNORM;
  VkRenderPass renderPass_ = VK_NULL_HANDLE;

  // --fp16: the splats land in this image in subpass 0, and subpass 1 decodes it
  // from sRGB into the linear float swapchain. One image for every frame: the
  // render pass has an external dependency that waits for the previous frame to
  // finish reading it.
  bool fp16_ = false;
  VkImage resolveImage_ = VK_NULL_HANDLE;
  VkDeviceMemory resolveMemory_ = VK_NULL_HANDLE;
  VkImageView resolveView_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout resolveSetLayout_ = VK_NULL_HANDLE;
  VkDescriptorPool resolvePool_ = VK_NULL_HANDLE;
  VkDescriptorSet resolveSet_ = VK_NULL_HANDLE;
  VkPipelineLayout resolveLayout_ = VK_NULL_HANDLE;
  VkPipeline resolvePipeline_ = VK_NULL_HANDLE;

  gvk::FrameRing frames_;
};
