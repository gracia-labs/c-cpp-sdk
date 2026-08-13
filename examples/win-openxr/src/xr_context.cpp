#include "xr_context.hpp"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

bool XrContext::init(const char* appName, bool float16) {
  // volk first: OpenXR needs a real vkGetInstanceProcAddr to build the instance.
  if (!gvk::initVulkanLoader()) return false;
  if (!createXrInstance(appName)) return false;
  if (!loadXrExtensionFns()) return false;
  if (!getSystem()) return false;
  if (!createVulkan()) return false;
  pipelineCache_ = gvk::createPipelineCache(device_);
  if (!createSession()) return false;
  fp16_ = float16;
  if (!createSwapchains(float16)) return false;
  // Not PRESENT_SRC_KHR: these images are the runtime's, not a window's.
  // The view mask makes this a multiview pass, which is what puts both eyes in
  // one pass. render() is given the stereo mode that matches.
  if (fp16_) {
    // Two subpasses: the splats, then the sRGB decode. createResolve fills the
    // intermediate image the framebuffers need, so it runs before them.
    renderPass_ = createResolveRenderPass();
    if (!renderPass_ || !createResolve()) return false;
  } else {
    renderPass_ = gvk::createColorRenderPass(
        device_, viewFormat_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, kViewMask);
  }
  createFramebuffers();
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

bool XrContext::createSwapchains(bool wantFloat16) {
  uint32_t formatCount = 0;
  XR_TRY(xrEnumerateSwapchainFormats(session_, 0, &formatCount, nullptr),
         "xrEnumerateSwapchainFormats");
  std::vector<int64_t> formats(formatCount);
  XR_TRY(xrEnumerateSwapchainFormats(session_, formatCount, &formatCount,
                                     formats.data()),
         "xrEnumerateSwapchainFormats");

  const auto offered = [&](VkFormat f) {
    return std::find(formats.begin(), formats.end(), (int64_t)f) != formats.end();
  };

  // --fp16: a 16-bit float target, so the blend of many overlapping splats does
  // not quantize to 256 levels at every step the way RGBA8 does.
  //
  // A float format has no sRGB twin to alias, and a runtime reads a float
  // swapchain as linear, so the encoded values the SDK writes have to be decoded
  // before the runtime sees them. That is what the resolve subpass is for
  // (createResolveRenderPass). It has to come after the blend, not inside the
  // splat shader: decoding is not linear, so decoding each splat first would
  // change the space the splats composite in, and they were trained in the
  // encoded one.
  if (wantFloat16) {
    if (!offered(VK_FORMAT_R16G16B16A16_SFLOAT)) {
      std::fprintf(stderr,
                   "--fp16: the runtime offers no R16G16B16A16_SFLOAT format.\n"
                   "It offered %zu format(s):", formats.size());
      for (int64_t f : formats) std::fprintf(stderr, " %lld", (long long)f);
      std::fprintf(stderr, "\n");
      return false;
    }
    swapchainFormat_ = viewFormat_ = VK_FORMAT_R16G16B16A16_SFLOAT;
    graciaColorFormat_ = GRACIA_COLOR_FORMAT_RGBA16_FLOAT;
    std::printf("Color: RGBA16F (--fp16). The splats blend in 16-bit float and a\n"
                "       resolve subpass decodes the result to linear.\n");
  } else {
    // Splats are trained in sRGB, so the SDK writes values that are already
    // encoded, and each end of the chain wants to encode them again: an sRGB
    // attachment converts on write, and the compositor converts a UNORM
    // swapchain because it reads that as linear. Two encodes make the picture
    // overbright.
    //
    // So the swapchain is declared sRGB, which tells the compositor the contents
    // are encoded and to pass them through, while the views and the render pass
    // use the UNORM twin so the write itself converts nothing. The formats are
    // in the same compatibility class, which is what makes the aliased view
    // legal.
    static const struct {
      VkFormat srgb, unorm;
    } kPairs[] = {{VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM},
                  {VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_B8G8R8A8_UNORM}};
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
  }

  // One swapchain with two array layers, not one per eye. Multiview fills both
  // layers in a single pass, so there is one image to acquire and release, and
  // the two projection views differ only by `imageArrayIndex`.
  XrSwapchainCreateInfo sci{XR_TYPE_SWAPCHAIN_CREATE_INFO};
  sci.usageFlags =
      XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
  sci.format = (int64_t)swapchainFormat_;
  sci.sampleCount = 1;  // runtimes need not hand back multisampled images
  sci.width = eyeExtent_.width;
  sci.height = eyeExtent_.height;
  sci.faceCount = 1;
  sci.arraySize = kEyes;
  sci.mipCount = 1;
  XR_TRY(xrCreateSwapchain(session_, &sci, &swapchain_), "xrCreateSwapchain");

  uint32_t imageCount = 0;
  XR_TRY(xrEnumerateSwapchainImages(swapchain_, 0, &imageCount, nullptr),
         "xrEnumerateSwapchainImages");
  images_.assign(imageCount, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
  XR_TRY(xrEnumerateSwapchainImages(swapchain_, imageCount, &imageCount,
                                    (XrSwapchainImageBaseHeader*)images_.data()),
         "xrEnumerateSwapchainImages");
  return true;
}

VkRenderPass XrContext::createResolveRenderPass() {
  // Subpass 0 draws the splats, subpass 1 decodes them. That order is fixed:
  // replay the draw calls in the first subpass, because a later one rejects them.
  //
  // 0: the float intermediate the splats blend into. Nothing needs it after the
  //    pass, so it is never stored.
  // 1: the swapchain. Subpass 1 covers every pixel, so it is never loaded.
  VkAttachmentDescription atts[2]{};
  atts[0].format = viewFormat_;  // the encoded blend, in the same float format
  atts[0].samples = VK_SAMPLE_COUNT_1_BIT;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  atts[1] = atts[0];
  atts[1].format = viewFormat_;
  atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[1].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

  VkAttachmentReference splatsColor{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolveInput{0, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
  VkAttachmentReference resolveColor{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};

  VkSubpassDescription subpasses[2]{};
  subpasses[0].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpasses[0].colorAttachmentCount = 1;
  subpasses[0].pColorAttachments = &splatsColor;
  subpasses[1].pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  subpasses[1].inputAttachmentCount = 1;
  subpasses[1].pInputAttachments = &resolveInput;
  subpasses[1].colorAttachmentCount = 1;
  subpasses[1].pColorAttachments = &resolveColor;

  VkSubpassDependency deps[2]{};
  // One intermediate serves every frame, so the clear has to wait for the
  // previous frame to finish reading it.
  deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
  deps[0].dstSubpass = 0;
  deps[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT |
                         VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].srcAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  // The blend must land before the decode reads it. BY_REGION says a pixel only
  // depends on its own pixel, which is what makes this a subpass and not a pass.
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = 1;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
  deps[1].dstAccessMask = VK_ACCESS_INPUT_ATTACHMENT_READ_BIT;
  deps[1].dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT;

  // Both subpasses see both eyes. dependencyCount stays 0, which gives every
  // dependency a view offset of 0: view N depends on view N.
  const uint32_t viewMasks[2] = {kViewMask, kViewMask};
  const uint32_t correlation = kViewMask;
  VkRenderPassMultiviewCreateInfo mv{
      VK_STRUCTURE_TYPE_RENDER_PASS_MULTIVIEW_CREATE_INFO};
  mv.subpassCount = 2;
  mv.pViewMasks = viewMasks;
  mv.correlationMaskCount = 1;
  mv.pCorrelationMasks = &correlation;

  VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
  rp.pNext = &mv;
  rp.attachmentCount = 2;
  rp.pAttachments = atts;
  rp.subpassCount = 2;
  rp.pSubpasses = subpasses;
  rp.dependencyCount = 2;
  rp.pDependencies = deps;

  VkRenderPass pass = VK_NULL_HANDLE;
  VK_CHECK(vkCreateRenderPass(device_, &rp, nullptr, &pass));
  return pass;
}

namespace {

// The demo allocates exactly one image, so this is the whole memory story.
uint32_t findMemoryType(VkPhysicalDevice gpu, uint32_t bits,
                        VkMemoryPropertyFlags want) {
  VkPhysicalDeviceMemoryProperties props{};
  vkGetPhysicalDeviceMemoryProperties(gpu, &props);
  for (uint32_t i = 0; i < props.memoryTypeCount; ++i)
    if ((bits & (1u << i)) &&
        (props.memoryTypes[i].propertyFlags & want) == want)
      return i;
  return ~0u;
}

VkShaderModule loadShader(VkDevice device, const uint32_t* code, size_t bytes) {
  VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
  ci.codeSize = bytes;
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  VK_CHECK(vkCreateShaderModule(device, &ci, nullptr, &m));
  return m;
}

}  // namespace

bool XrContext::createResolve() {
  // The intermediate the splats blend into. TRANSIENT plus INPUT_ATTACHMENT is
  // the hint that it never leaves the pass; a desktop driver still backs it with
  // real memory, but the intent is what a tiler needs to keep it on chip.
  VkImageCreateInfo ii{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
  ii.imageType = VK_IMAGE_TYPE_2D;
  ii.format = viewFormat_;
  ii.extent = {eyeExtent_.width, eyeExtent_.height, 1};
  ii.mipLevels = 1;
  ii.arrayLayers = kEyes;
  ii.samples = VK_SAMPLE_COUNT_1_BIT;
  ii.tiling = VK_IMAGE_TILING_OPTIMAL;
  ii.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT |
             VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT;
  ii.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ii.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VK_CHECK(vkCreateImage(device_, &ii, nullptr, &resolveImage_));

  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(device_, resolveImage_, &req);
  // LAZILY_ALLOCATED first: a tiler gives it no memory at all. A desktop GPU has
  // no such type, so fall back to ordinary device-local memory.
  uint32_t type = findMemoryType(physicalDevice_, req.memoryTypeBits,
                                 VK_MEMORY_PROPERTY_LAZILY_ALLOCATED_BIT);
  if (type == ~0u)
    type = findMemoryType(physicalDevice_, req.memoryTypeBits,
                          VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (type == ~0u) {
    std::fprintf(stderr, "No memory type for the --fp16 resolve target\n");
    return false;
  }
  VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
  ai.allocationSize = req.size;
  ai.memoryTypeIndex = type;
  VK_CHECK(vkAllocateMemory(device_, &ai, nullptr, &resolveMemory_));
  VK_CHECK(vkBindImageMemory(device_, resolveImage_, resolveMemory_, 0));

  VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
  vi.image = resolveImage_;
  vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
  vi.format = viewFormat_;
  vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kEyes};
  VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &resolveView_));

  // One input-attachment binding, one set. No sampler: a subpass input is read
  // at the current fragment, so there is nothing to filter.
  VkDescriptorSetLayoutBinding bind{};
  bind.binding = 0;
  bind.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
  bind.descriptorCount = 1;
  bind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
  VkDescriptorSetLayoutCreateInfo sl{
      VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
  sl.bindingCount = 1;
  sl.pBindings = &bind;
  VK_CHECK(vkCreateDescriptorSetLayout(device_, &sl, nullptr, &resolveSetLayout_));

  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT, 1};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.maxSets = 1;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &size;
  VK_CHECK(vkCreateDescriptorPool(device_, &pi, nullptr, &resolvePool_));

  VkDescriptorSetAllocateInfo da{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
  da.descriptorPool = resolvePool_;
  da.descriptorSetCount = 1;
  da.pSetLayouts = &resolveSetLayout_;
  VK_CHECK(vkAllocateDescriptorSets(device_, &da, &resolveSet_));

  VkDescriptorImageInfo di{};
  di.imageView = resolveView_;
  di.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
  VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
  write.dstSet = resolveSet_;
  write.dstBinding = 0;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT;
  write.pImageInfo = &di;
  vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);

  VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
  pl.setLayoutCount = 1;
  pl.pSetLayouts = &resolveSetLayout_;
  VK_CHECK(vkCreatePipelineLayout(device_, &pl, nullptr, &resolveLayout_));

  static const uint32_t kVert[] =
#include "srgb_resolve.vert.inc"
      ;
  static const uint32_t kFrag[] =
#include "srgb_resolve.frag.inc"
      ;
  VkShaderModule vert = loadShader(device_, kVert, sizeof(kVert));
  VkShaderModule frag = loadShader(device_, kFrag, sizeof(kFrag));

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vert;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = frag;

  // The fullscreen triangle comes from gl_VertexIndex, so there is no vertex
  // input, no buffer and no index buffer.
  VkPipelineVertexInputStateCreateInfo vin{
      VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
  VkPipelineInputAssemblyStateCreateInfo asm_{
      VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
  asm_.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
  VkPipelineViewportStateCreateInfo vp{
      VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
  vp.viewportCount = 1;
  vp.scissorCount = 1;
  VkPipelineRasterizationStateCreateInfo rs{
      VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;
  VkPipelineMultisampleStateCreateInfo ms{
      VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineDepthStencilStateCreateInfo ds{
      VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
  VkPipelineColorBlendAttachmentState blend{};
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{
      VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;
  const VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT,
                                 VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dss{
      VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
  dss.dynamicStateCount = 2;
  dss.pDynamicStates = dyn;

  VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vin;
  gp.pInputAssemblyState = &asm_;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pDepthStencilState = &ds;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &dss;
  gp.layout = resolveLayout_;
  gp.renderPass = renderPass_;
  gp.subpass = 1;  // the decode, after the splats
  VK_CHECK(vkCreateGraphicsPipelines(device_, pipelineCache_, 1, &gp, nullptr,
                                     &resolvePipeline_));

  vkDestroyShaderModule(device_, vert, nullptr);
  vkDestroyShaderModule(device_, frag, nullptr);
  return true;
}

void XrContext::createFramebuffers() {
  views_.resize(images_.size());
  framebuffers_.resize(images_.size());
  for (size_t i = 0; i < images_.size(); ++i) {
    // A 2D_ARRAY view over both layers: multiview picks the layer by view index,
    // so one view covers both eyes.
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = images_[i].image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
    vi.format = viewFormat_;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, kEyes};
    VK_CHECK(vkCreateImageView(device_, &vi, nullptr, &views_[i]));

    // With --fp16 the pass has two attachments: the shared intermediate the
    // splats blend into, then this swapchain image the decode writes.
    const VkImageView attachments[2] = {fp16_ ? resolveView_ : views_[i],
                                        views_[i]};
    VkFramebufferCreateInfo fi{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fi.renderPass = renderPass_;
    fi.attachmentCount = fp16_ ? 2u : 1u;
    fi.pAttachments = attachments;
    fi.width = eyeExtent_.width;
    fi.height = eyeExtent_.height;
    // One, not two. A multiview pass takes its layer count from the view mask,
    // and a framebuffer for such a pass must declare a single layer.
    fi.layers = 1;
    VK_CHECK(vkCreateFramebuffer(device_, &fi, nullptr, &framebuffers_[i]));
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
                       const std::function<void(VkCommandBuffer)>& body) {
  gvk::barrierComputeToGraphics(f.cmd);

  uint32_t index = 0;
  XrSwapchainImageAcquireInfo ai{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
  if (XR_FAILED(xrAcquireSwapchainImage(swapchain_, &ai, &index))) return;
  XrSwapchainImageWaitInfo wi{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
  wi.timeout = XR_INFINITE_DURATION;
  xrWaitSwapchainImage(swapchain_, &wi);
  f.acquired = true;

  // Both projection views name the same swapchain and differ only by the layer
  // that multiview wrote.
  for (uint32_t eye = 0; eye < kEyes; ++eye) {
    projViews_[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
    projViews_[eye].pose = f.views[eye].pose;
    projViews_[eye].fov = f.views[eye].fov;
    projViews_[eye].subImage.swapchain = swapchain_;
    projViews_[eye].subImage.imageArrayIndex = eye;
    projViews_[eye].subImage.imageRect.offset = {0, 0};
    projViews_[eye].subImage.imageRect.extent = {(int32_t)eyeExtent_.width,
                                                 (int32_t)eyeExtent_.height};
  }

  VkClearValue clears[2]{};
  clears[0].color = {{0.02f, 0.02f, 0.03f, 1.0f}};
  VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
  bi.renderPass = renderPass_;
  bi.framebuffer = framebuffers_[index];
  bi.renderArea = {{0, 0}, eyeExtent_};
  bi.clearValueCount = fp16_ ? 2u : 1u;
  bi.pClearValues = clears;
  // One pass for both eyes: the view mask on the render pass runs every draw
  // twice, once per layer. The viewport covers one eye, because each layer is
  // one eye at full size.
  vkCmdBeginRenderPass(f.cmd, &bi, VK_SUBPASS_CONTENTS_INLINE);

  VkViewport vp{0.0f, 0.0f, (float)eyeExtent_.width, (float)eyeExtent_.height,
                0.0f, 1.0f};
  VkRect2D scissor{{0, 0}, eyeExtent_};
  vkCmdSetViewport(f.cmd, 0, 1, &vp);
  vkCmdSetScissor(f.cmd, 0, 1, &scissor);

  body(f.cmd);

  if (fp16_) {
    // Subpass 1: three vertices, no buffers. It reads the blend the splats just
    // made and writes it decoded into the swapchain.
    vkCmdNextSubpass(f.cmd, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, resolvePipeline_);
    vkCmdBindDescriptorSets(f.cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            resolveLayout_, 0, 1, &resolveSet_, 0, nullptr);
    vkCmdDraw(f.cmd, 3, 1, 0, 0);
  }

  vkCmdEndRenderPass(f.cmd);
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
    if (f->acquired) {
      XrSwapchainImageReleaseInfo ri{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
      xrReleaseSwapchainImage(swapchain_, &ri);
    }

    // A frame that acquired nothing would describe an image we never filled.
    if (f->acquired) {
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

  for (auto fb : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
  for (auto v : views_) vkDestroyImageView(device_, v, nullptr);
  framebuffers_.clear();
  views_.clear();
  if (swapchain_) xrDestroySwapchain(swapchain_);
  swapchain_ = XR_NULL_HANDLE;
  if (resolvePipeline_) vkDestroyPipeline(device_, resolvePipeline_, nullptr);
  if (resolveLayout_) vkDestroyPipelineLayout(device_, resolveLayout_, nullptr);
  if (resolvePool_) vkDestroyDescriptorPool(device_, resolvePool_, nullptr);
  if (resolveSetLayout_)
    vkDestroyDescriptorSetLayout(device_, resolveSetLayout_, nullptr);
  if (resolveView_) vkDestroyImageView(device_, resolveView_, nullptr);
  if (resolveImage_) vkDestroyImage(device_, resolveImage_, nullptr);
  if (resolveMemory_) vkFreeMemory(device_, resolveMemory_, nullptr);
  resolvePipeline_ = VK_NULL_HANDLE;
  resolveLayout_ = VK_NULL_HANDLE;
  resolvePool_ = VK_NULL_HANDLE;
  resolveSetLayout_ = VK_NULL_HANDLE;
  resolveView_ = VK_NULL_HANDLE;
  resolveImage_ = VK_NULL_HANDLE;
  resolveMemory_ = VK_NULL_HANDLE;
  if (renderPass_) vkDestroyRenderPass(device_, renderPass_, nullptr);
  frames_.destroy(device_);
  if (pipelineCache_) vkDestroyPipelineCache(device_, pipelineCache_, nullptr);
  renderPass_ = VK_NULL_HANDLE;
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
