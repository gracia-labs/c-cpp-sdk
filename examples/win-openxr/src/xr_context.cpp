#include "xr_context.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

bool XrContext::init(const char* appName) {
  // volk first: OpenXR needs a real vkGetInstanceProcAddr to build the instance.
  if (volkInitialize() != VK_SUCCESS) {
    std::fprintf(stderr, "Vulkan loader not found (install a Vulkan runtime)\n");
    return false;
  }
  if (!createXrInstance(appName)) return false;
  if (!loadXrExtensionFns()) return false;
  if (!getSystem()) return false;
  if (!createVulkan()) return false;
  pipelineCache_ = gvk::createPipelineCache(device_);
  if (!createSession()) return false;
  if (!createSwapchains()) return false;
  createEyeViews();
  frames_.init(device_, queues_.graphics, kFramesInFlight);
  return true;
}

bool XrContext::createXrInstance(const char* appName) {
  uint32_t n = 0;
  xrEnumerateInstanceExtensionProperties(nullptr, 0, &n, nullptr);
  std::vector<XrExtensionProperties> exts(n, {XR_TYPE_EXTENSION_PROPERTIES});
  XrResult enumRes =
      xrEnumerateInstanceExtensionProperties(nullptr, n, &n, exts.data());
  if (XR_FAILED(enumRes)) {
    std::fprintf(stderr,
                 "No OpenXR runtime found (error %d). Install and start one "
                 "(Meta Quest Link, SteamVR, ...).\n",
                 (int)enumRes);
    return false;
  }
  bool haveVulkan2 = false;
  for (const auto& e : exts)
    if (std::strcmp(e.extensionName, XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME) == 0)
      haveVulkan2 = true;
  if (!haveVulkan2) {
    std::fprintf(stderr, "Runtime does not support %s\n",
                 XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME);
    return false;
  }

  const char* wanted[] = {XR_KHR_VULKAN_ENABLE2_EXTENSION_NAME};
  XrInstanceCreateInfo ci{XR_TYPE_INSTANCE_CREATE_INFO};
  std::snprintf(ci.applicationInfo.applicationName,
                sizeof(ci.applicationInfo.applicationName), "%s", appName);
  ci.applicationInfo.apiVersion = XR_API_VERSION_1_0;
  ci.enabledExtensionCount = 1;
  ci.enabledExtensionNames = wanted;
  XR_TRY(xrCreateInstance(&ci, &instance_), "xrCreateInstance");

  XrInstanceProperties ip{XR_TYPE_INSTANCE_PROPERTIES};
  if (XR_SUCCEEDED(xrGetInstanceProperties(instance_, &ip))) runtimeName_ = ip.runtimeName;
  return true;
}

bool XrContext::loadXrExtensionFns() {
  auto get = [&](const char* name, PFN_xrVoidFunction* out) {
    return XR_SUCCEEDED(xrGetInstanceProcAddr(instance_, name, out));
  };
  if (!get("xrGetVulkanGraphicsRequirements2KHR",
           (PFN_xrVoidFunction*)&pfnGetVkReq_) ||
      !get("xrCreateVulkanInstanceKHR", (PFN_xrVoidFunction*)&pfnCreateVkInstance_) ||
      !get("xrGetVulkanGraphicsDevice2KHR", (PFN_xrVoidFunction*)&pfnGetVkDevice_) ||
      !get("xrCreateVulkanDeviceKHR", (PFN_xrVoidFunction*)&pfnCreateVkDevice_)) {
    std::fprintf(stderr, "Runtime is missing an XR_KHR_vulkan_enable2 entry point\n");
    return false;
  }
  return true;
}

bool XrContext::getSystem() {
  XrSystemGetInfo sgi{XR_TYPE_SYSTEM_GET_INFO};
  sgi.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;

  // Runtime up, headset not. The spec allows a later call to succeed.
  XrResult r = XR_ERROR_FORM_FACTOR_UNAVAILABLE;
  for (int attempt = 0; attempt < 20; ++attempt) {
    r = xrGetSystem(instance_, &sgi, &systemId_);
    if (r != XR_ERROR_FORM_FACTOR_UNAVAILABLE) break;
    if (attempt == 0) std::printf("Waiting for a headset...\n");
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  if (r == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
    std::fprintf(stderr, "No headset available. Connect one and try again.\n");
    return false;
  }
  XR_TRY(r, "xrGetSystem");

  XrSystemProperties sp{XR_TYPE_SYSTEM_PROPERTIES};
  if (XR_SUCCEEDED(xrGetSystemProperties(instance_, systemId_, &sp)))
    std::printf("Headset: %s\n", sp.systemName);
  return true;
}

bool XrContext::createVulkan() {
  // Mandatory before xrCreateSession. The reported maximum is what the runtime
  // was tested against, not a cap.
  XrGraphicsRequirementsVulkan2KHR req{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
  XR_TRY(pfnGetVkReq_(instance_, systemId_, &req), "xrGetVulkanGraphicsRequirements2KHR");
  if (XR_VERSION_MAJOR(req.maxApiVersionSupported) == 1 &&
      XR_VERSION_MINOR(req.maxApiVersionSupported) < 3)
    std::printf("Note: runtime reports Vulkan up to %u.%u; asking for 1.3 anyway\n",
                (unsigned)XR_VERSION_MAJOR(req.maxApiVersionSupported),
                (unsigned)XR_VERSION_MINOR(req.maxApiVersionSupported));

  VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
  app.pApplicationName = "gracia-openxr-demo";
  app.pEngineName = "gracia-demo";
  app.apiVersion = VK_API_VERSION_1_3;

  VkInstanceCreateInfo vkci{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
  vkci.pApplicationInfo = &app;  // no surface extensions: there is no window

  XrVulkanInstanceCreateInfoKHR xici{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
  xici.systemId = systemId_;
  xici.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xici.vulkanCreateInfo = &vkci;
  VkResult vkres = VK_ERROR_UNKNOWN;
  XR_TRY(pfnCreateVkInstance_(instance_, &xici, &vkInstance_, &vkres),
         "xrCreateVulkanInstanceKHR");
  if (vkres != VK_SUCCESS) {
    std::fprintf(stderr, "xrCreateVulkanInstanceKHR: Vulkan error %d\n", (int)vkres);
    return false;
  }
  volkLoadInstanceOnly(vkInstance_);

  // The runtime picks the GPU; choosing it ourselves fails on hybrid machines.
  XrVulkanGraphicsDeviceGetInfoKHR gdi{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
  gdi.systemId = systemId_;
  gdi.vulkanInstance = vkInstance_;
  XR_TRY(pfnGetVkDevice_(instance_, &gdi, &physicalDevice_),
         "xrGetVulkanGraphicsDevice2KHR");

  VkPhysicalDeviceProperties props{};
  vkGetPhysicalDeviceProperties(physicalDevice_, &props);
  std::printf("GPU: %s\n", props.deviceName);

  queues_ = gvk::pickQueueFamilies(physicalDevice_);
  gvk::printQueueFamilies(queues_);

  const gvk::DeviceRequest request(physicalDevice_, queues_, /*wantSwapchain=*/false);
  XrVulkanDeviceCreateInfoKHR xdci{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
  xdci.systemId = systemId_;
  xdci.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
  xdci.vulkanPhysicalDevice = physicalDevice_;
  xdci.vulkanCreateInfo = &request.info();
  vkres = VK_ERROR_UNKNOWN;
  XR_TRY(pfnCreateVkDevice_(instance_, &xdci, &device_, &vkres),
         "xrCreateVulkanDeviceKHR");
  if (vkres != VK_SUCCESS) {
    std::fprintf(stderr, "xrCreateVulkanDeviceKHR: Vulkan error %d\n", (int)vkres);
    return false;
  }
  volkLoadDevice(device_);

  // Index 0 of the graphics family: the SDK and the XR binding want the same one.
  vkGetDeviceQueue(device_, queues_.graphics, 0, &graphicsQueue_);
  return true;
}

bool XrContext::createSession() {
  XrGraphicsBindingVulkan2KHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN2_KHR};
  binding.instance = vkInstance_;
  binding.physicalDevice = physicalDevice_;
  binding.device = device_;
  binding.queueFamilyIndex = queues_.graphics;
  binding.queueIndex = 0;

  XrSessionCreateInfo sci{XR_TYPE_SESSION_CREATE_INFO};
  sci.next = &binding;
  sci.systemId = systemId_;
  XR_TRY(xrCreateSession(instance_, &sci, &session_), "xrCreateSession");

  uint32_t viewCount = 0;
  XR_TRY(xrEnumerateViewConfigurationViews(
             instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
             &viewCount, nullptr),
         "xrEnumerateViewConfigurationViews");
  if (viewCount != kEyes) {
    std::fprintf(stderr, "Expected %u stereo views, runtime reports %u\n", kEyes,
                 viewCount);
    return false;
  }
  for (auto& v : cfgViews_) v = {XR_TYPE_VIEW_CONFIGURATION_VIEW};
  XR_TRY(xrEnumerateViewConfigurationViews(
             instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
             viewCount, &viewCount, cfgViews_.data()),
         "xrEnumerateViewConfigurationViews");
  eyeExtent_ = {cfgViews_[0].recommendedImageRectWidth,
                cfgViews_[0].recommendedImageRectHeight};
  std::printf("Per-eye render target: %ux%u\n", eyeExtent_.width, eyeExtent_.height);

  uint32_t blendCount = 0;
  if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(
          instance_, systemId_, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0,
          &blendCount, nullptr)) &&
      blendCount > 0) {
    std::vector<XrEnvironmentBlendMode> modes(blendCount);
    xrEnumerateEnvironmentBlendModes(instance_, systemId_,
                                     XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO,
                                     blendCount, &blendCount, modes.data());
    blendMode_ = modes[0];
  }

  // LOCAL: every runtime supports it, and the origin is the initial head pose.
  XrReferenceSpaceCreateInfo rsci{XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
  rsci.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
  rsci.poseInReferenceSpace.orientation.w = 1.0f;
  XR_TRY(xrCreateReferenceSpace(session_, &rsci, &appSpace_), "xrCreateReferenceSpace");
  return true;
}

bool XrContext::createSwapchains() {
  uint32_t formatCount = 0;
  XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
         "xrEnumerateSwapchainFormats");
  std::vector<int64_t> formats(formatCount);
  XR_TRY(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount,
                                     formats.data()),
         "xrEnumerateSwapchainFormats");

  // Splats are trained in sRGB, so the SDK writes values that are already
  // encoded, and each end of the chain wants to encode them again: an sRGB
  // attachment converts on write, and the compositor converts a UNORM swapchain
  // because it reads that as linear. Two encodes make the picture overbright.
  //
  // So the swapchain is declared sRGB, which tells the compositor the contents
  // are encoded and to pass them through, while the views and the render pass
  // use the UNORM twin so the write itself converts nothing. The formats are in
  // the same compatibility class, which is what makes the aliased view legal.
  static const struct {
    VkFormat srgb, unorm;
  } kPairs[] = {{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
                {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM}};
  const auto offered = [&](VkFormat f) {
    return std::find(formats.begin(), formats.end(), (int64_t)f) != formats.end();
  };
  for (const auto& p : kPairs)
    if (offered(p.srgb)) {
      swapchainFormat_ = p.srgb;
      viewFormat_ = p.unorm;
      break;
    }
  // No sRGB offered: fall back to a plain UNORM swapchain and accept whatever
  // the compositor does with it.
  if (swapchainFormat_ == VK_FORMAT_UNDEFINED)
    for (const auto& p : kPairs)
      if (offered(p.unorm)) {
        swapchainFormat_ = viewFormat_ = p.unorm;
        std::printf("Note: runtime offers no sRGB format; colors may be bright\n");
        break;
      }
  if (swapchainFormat_ == VK_FORMAT_UNDEFINED) {
    std::fprintf(stderr, "Runtime offers no 8-bit RGBA swapchain format\n");
    return false;
  }
  // The render pass and the SDK both follow the view format, never the
  // swapchain's, so nothing in our half of the chain converts.
  graciaColorFormat_ = GRACIA_COLOR_FORMAT_RGBA8_UNORM;

  for (uint32_t eye = 0; eye < kEyes; ++eye) {
    XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
    sci.usageFlags = XR_SWAPCHAIN_USAGE_SAMPLED_BIT |
                     XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    sci.format = (int64_t)swapchainFormat_;
    sci.sampleCount = 1;  // runtimes need not hand back multisampled images
    sci.width = eyeExtent_.width;
    sci.height = eyeExtent_.height;
    sci.faceCount = 1;
    sci.arraySize = 1;  // one swapchain per eye keeps acquire/release per eye
    sci.mipCount = 1;
    XR_TRY(xrCreateSwapchain(session_, &sci, &eyes_[eye].swapchain),
           "xrCreateSwapchain");

    uint32_t imageCount = 0;
    XR_TRY(xrEnumerateSwapchainImages(eyes_[eye].swapchain, 0, &imageCount, nullptr),
           "xrEnumerateSwapchainImages");
    eyes_[eye].images.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
    XR_TRY(xrEnumerateSwapchainImages(
               eyes_[eye].swapchain, imageCount, &imageCount,
               (XrSwapchainImageBaseHeader*)eyes_[eye].images.data()),
           "xrEnumerateSwapchainImages");
  }
  return true;
}

void XrContext::createEyeViews() {
  for (auto& eye : eyes_) {
    eye.views.resize(eye.images.size());
    for (size_t i = 0; i < eye.images.size(); ++i)
      eye.views[i] = gvk::createColorView(device_, eye.images[i].image, viewFormat_);
  }
}

bool XrContext::pollEvents(bool quitRequested) {
  XrEventDataBuffer ev{XR_TYPE_EVENT_DATA_BUFFER};
  while (true) {
    ev = {XR_TYPE_EVENT_DATA_BUFFER};  // the header must be reset for every call
    if (xrPollEvent(instance_, &ev) != XR_SUCCESS) break;

    if (ev.type == XR_TYPE_EVENT_DATA_EVENTS_LOST) {
      const auto& lost = *reinterpret_cast<XrEventDataEventsLost*>(&ev);
      std::fprintf(stderr, "OpenXR dropped %u events\n", lost.lostEventCount);
    } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
      exitLoop_ = true;
    } else if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
      const auto& e = *reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
      state_ = e.state;
      switch (state_) {
        case XR_SESSION_STATE_READY: {
          XrSessionBeginInfo bi{XR_TYPE_SESSION_BEGIN_INFO};
          bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
          if (XR_SUCCEEDED(xrBeginSession(session_, &bi))) sessionRunning_ = true;
          break;
        }
        case XR_SESSION_STATE_STOPPING:
          sessionRunning_ = false;
          xrEndSession(session_);
          break;
        case XR_SESSION_STATE_EXITING:
        case XR_SESSION_STATE_LOSS_PENDING:
          exitLoop_ = true;
          break;
        default:
          break;
      }
    }
  }

  // Ctrl-C: ask the runtime to end the session so it transitions out through
  // STOPPING and EXITING instead of vanishing mid-frame. It needs frames to do
  // that, so keep looping; leave anyway if it does not answer.
  if (quitRequested && !exitLoop_) {
    if (!sessionRunning_) return false;
    const auto now = std::chrono::steady_clock::now();
    if (!exitRequested_) {
      exitRequested_ = true;
      exitRequestedAt_ = now;
      xrRequestExitSession(session_);
    } else if (now - exitRequestedAt_ > std::chrono::seconds(2)) {
      return false;
    }
  }
  return !exitLoop_;
}

XrFrameState XrContext::waitFrame() {
  XrFrameState fs{XR_TYPE_FRAME_STATE};
  XrFrameWaitInfo fwi{XR_TYPE_FRAME_WAIT_INFO};
  xrWaitFrame(session_, &fwi, &fs);
  return fs;
}

void XrContext::beginFrame() {
  XrFrameBeginInfo fbi{XR_TYPE_FRAME_BEGIN_INFO};
  xrBeginFrame(session_, &fbi);
}

std::optional<XrFrameCtx> XrContext::acquire(const XrFrameState& fs) {
  if (!fs.shouldRender) return std::nullopt;

  XrViewState vs{XR_TYPE_VIEW_STATE};
  XrViewLocateInfo vli{XR_TYPE_VIEW_LOCATE_INFO};
  vli.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
  vli.displayTime = fs.predictedDisplayTime;
  vli.space = appSpace_;

  std::array<XrView, kEyes> views{};
  for (auto& v : views) v = {XR_TYPE_VIEW};
  uint32_t got = 0;
  if (XR_FAILED(xrLocateViews(session_, &vli, &vs, kEyes, &got, views.data())))
    return std::nullopt;
  if (got != kEyes || !(vs.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) ||
      !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT))
    return std::nullopt;

  XrFrameCtx f{};
  f.slot = frames_.slot();
  f.views = views;
  f.eyeExtent = eyeExtent_;

  // The draw calls recorded into this slot are freed by the caller right after
  // this wait, so the GPU must be done with them first.
  frames_.waitSlot(device_);
  f.cmd = frames_.beginCmd(device_);
  return f;
}

void XrContext::render(XrFrameCtx& f,
                       const std::function<void(VkCommandBuffer, uint32_t)>& body) {
  gvk::barrierComputeToGraphics(f.cmd);

  for (uint32_t eye = 0; eye < kEyes; ++eye) {
    uint32_t index = 0;
    XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(eyes_[eye].swapchain, &ai, &index))) return;
    XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(eyes_[eye].swapchain, &wi);
    f.acquiredCount = eye + 1;

    // The composition layer must describe the image we are about to fill.
    projViews_[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    projViews_[eye].pose = f.views[eye].pose;
    projViews_[eye].fov = f.views[eye].fov;
    projViews_[eye].subImage.swapchain = eyes_[eye].swapchain;
    projViews_[eye].subImage.imageArrayIndex = 0;
    projViews_[eye].subImage.imageRect.offset = {0, 0};
    projViews_[eye].subImage.imageRect.extent = {(int32_t)eyeExtent_.width,
                                                 (int32_t)eyeExtent_.height};

    VkClearValue clear{};
    clear.color = {{0.02f, 0.02f, 0.03f, 1.0f}};
    // Not PRESENT_SRC_KHR: these images are the runtime's, not a window's.
    gvk::beginColorRendering(f.cmd, eyes_[eye].images[index].image,
                             eyes_[eye].views[index], eyeExtent_, clear);

    VkViewport vp{0.0f, 0.0f, (float)eyeExtent_.width, (float)eyeExtent_.height,
                  0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, eyeExtent_};
    vkCmdSetViewport(f.cmd, 0, 1, &vp);
    vkCmdSetScissor(f.cmd, 0, 1, &scissor);

    body(f.cmd, eye);
    gvk::endColorRendering(f.cmd, eyes_[eye].images[index].image,
                           VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
  }
}

void XrContext::endFrame(const XrFrameState& fs, std::optional<XrFrameCtx>& f) {
  std::vector<XrCompositionLayerBaseHeader*> layers;
  XrCompositionLayerProjection layer{XR_TYPE_COMPOSITION_LAYER_PROJECTION};

  if (f) {
    VK_CHECK(vkEndCommandBuffer(f->cmd));
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &f->cmd;
    VK_CHECK(vkQueueSubmit(graphicsQueue_, 1, &submit, frames_.fence()));

    // After the submit: the runtime may read an image still executing, but not
    // one that was never sent.
    for (uint32_t eye = 0; eye < f->acquiredCount; ++eye) {
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(eyes_[eye].swapchain, &ri);
    }

    // A partly acquired frame would describe an image we never filled.
    if (f->acquiredCount == kEyes) {
      layer.space = appSpace_;
      layer.viewCount = kEyes;
      layer.views = projViews_.data();
      layers.push_back(reinterpret_cast<XrCompositionLayerBaseHeader*>(&layer));
    }
    frames_.advance();
  }

  XrFrameEndInfo fei{XR_TYPE_FRAME_END_INFO};
  fei.displayTime = fs.predictedDisplayTime;
  fei.environmentBlendMode = blendMode_;
  fei.layerCount = (uint32_t)layers.size();
  fei.layers = layers.data();
  xrEndFrame(session_, &fei);
}

void XrContext::shutdown() {
  if (device_) vkDeviceWaitIdle(device_);

  for (auto& eye : eyes_) {

    for (auto v : eye.views) vkDestroyImageView(device_, v, nullptr);
    eye.views.clear();
    if (eye.swapchain) xrDestroySwapchain(eye.swapchain);
    eye.swapchain = XR_NULL_HANDLE;
  }
  frames_.destroy(device_);
  if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
  pipelineCache_ = VK_NULL_HANDLE;

  if (appSpace_) xrDestroySpace(appSpace_);
  if (session_) xrDestroySession(session_);
  appSpace_ = XR_NULL_HANDLE;
  session_ = XR_NULL_HANDLE;

  if (device_) vkDestroyDevice(device_, nullptr);
  if (vkInstance_) vkDestroyInstance(vkInstance_, nullptr);
  device_ = VK_NULL_HANDLE;
  vkInstance_ = VK_NULL_HANDLE;

  if (instance_) xrDestroyInstance(instance_);
  instance_ = XR_NULL_HANDLE;
}
