#include "vk_context.hpp"
#include "splat_viewer.hpp"

#include <imgui.h>
#include <backends/imgui_impl_glfw.h>
#include <backends/imgui_impl_vulkan.h>

#include <GLFW/glfw3.h>

#include <portable-file-dialogs.h>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <windows.h>

// Favor the high performance NVIDIA or AMD GPUs. Must live in the executable:
// the linker can drop an unreferenced object out of a static library.
extern "C" {
// http://developer.download.nvidia.com/devzone/devcenter/gamegraphics/files/OptimusRenderingPolicies.pdf
__declspec(dllexport) DWORD NvOptimusEnablement = 0x00000001;
// https://gpuopen.com/learn/amdpowerxpressrequesthighperformance/
__declspec(dllexport) DWORD AmdPowerXpressRequestHighPerformance = 0x00000001;
}
#endif  // defined(_WIN32)

namespace {

// Ctrl-C arrives on a thread Windows injects, so the flag has to be atomic.
// Setting it beats the default handler, which stops every other thread and then
// waits on a fence that nothing is left alive to signal.
std::atomic<bool> gQuit{false};

#if defined(_WIN32)
BOOL WINAPI onConsoleCtrl(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
    return FALSE;
  gQuit.store(true, std::memory_order_relaxed);
  return TRUE;
}
#endif

struct AppState {
  SplatViewer* viewer = nullptr;
  VulkanContext* ctx = nullptr;
  bool draggingDivider = false;
  int dividerIndex = -1;
  float dividerLastX = 0;
  std::vector<std::filesystem::path> pendingLoad;
};

AppState& app(GLFWwindow* w) {
  return *static_cast<AppState*>(glfwGetWindowUserPointer(w));
}

// Cursor position in framebuffer pixels, y down (matches the SDK's expectation).
glm::vec2 cursorFB(GLFWwindow* w) {
  double cx = 0, cy = 0;
  glfwGetCursorPos(w, &cx, &cy);
  int ww = 1, wh = 1, fw = 1, fh = 1;
  glfwGetWindowSize(w, &ww, &wh);
  glfwGetFramebufferSize(w, &fw, &fh);
  const float sx = ww > 0 ? (float)fw / ww : 1.0f;
  const float sy = wh > 0 ? (float)fh / wh : 1.0f;
  return {(float)cx * sx, (float)cy * sy};
}

float fbWidth(GLFWwindow* w) {
  int fw = 1, fh = 1;
  glfwGetFramebufferSize(w, &fw, &fh);
  return (float)fw;
}

void onCursorPos(GLFWwindow* w, double, double) {
  AppState& a = app(w);
  const glm::vec2 p = cursorFB(w);
  if (a.draggingDivider) {
    a.viewer->split().drag(a.dividerIndex, p.x - a.dividerLastX, fbWidth(w));
    a.dividerLastX = p.x;
    return;
  }
  a.viewer->input().pointerDragged(p);
}

void onMouseButton(GLFWwindow* w, int button, int action, int mods) {
  AppState& a = app(w);
  const bool pressed = action == GLFW_PRESS;
  if (!pressed) {
    a.draggingDivider = false;
    a.viewer->input().pointerUp();
    return;
  }
  if (ImGui::GetIO().WantCaptureMouse) return;

  const glm::vec2 p = cursorFB(w);
  if (button == GLFW_MOUSE_BUTTON_LEFT && a.viewer->layout() == SceneLayout::Split) {
    if (auto i = a.viewer->split().dividerIndex(p.x, fbWidth(w))) {
      a.draggingDivider = true;
      a.dividerIndex = *i;
      a.dividerLastX = p.x;
      return;
    }
  }
  const bool secondary =
      button == GLFW_MOUSE_BUTTON_RIGHT || button == GLFW_MOUSE_BUTTON_MIDDLE;
  a.viewer->input().pointerDown(p, secondary, (mods & GLFW_MOD_ALT) != 0);
}

void onScroll(GLFWwindow* w, double, double yoff) {
  if (!ImGui::GetIO().WantCaptureMouse)
    app(w).viewer->input().scroll((float)yoff, false);
}

void onKey(GLFWwindow* w, int key, int, int action, int mods) {
  AppState& a = app(w);
  a.viewer->input().setShift((mods & GLFW_MOD_SHIFT) != 0);
  if (ImGui::GetIO().WantCaptureKeyboard) return;

  if (action == GLFW_PRESS || action == GLFW_REPEAT) {
    if (a.viewer->input().keyDown(key)) return;  // movement key
  } else if (action == GLFW_RELEASE) {
    a.viewer->input().keyUp(key);
    return;
  }
  if (action != GLFW_PRESS) return;
  switch (key) {
    case GLFW_KEY_SPACE: a.viewer->player().togglePlay(); break;
    case GLFW_KEY_0: a.viewer->camera().reset(); break;
    case GLFW_KEY_M: a.viewer->toggleLayout(); break;
    case GLFW_KEY_D: a.viewer->toggleDepthAsColor(); break;
    case GLFW_KEY_L: a.viewer->toggleDepthLinear(); break;
    default: break;
  }
}

// GLFW and the dialog hand out UTF-8; a plain char path is the ANSI code page on Windows.
std::filesystem::path utf8Path(const std::string& s) {
  return std::u8string(s.begin(), s.end());
}

void onDrop(GLFWwindow* w, int count, const char** paths) {
  AppState& a = app(w);
  a.pendingLoad.clear();
  for (int i = 0; i < count; ++i) a.pendingLoad.push_back(utf8Path(paths[i]));
}

// Native multi-select open dialog: Win32 on Windows, zenity or kdialog on Linux.
std::vector<std::filesystem::path> openFileDialog() {
  std::vector<std::filesystem::path> out;
  for (const std::string& file :
       pfd::open_file("Open one or more .ply, .sog, .guf or .mint scenes", "",
                      {"Gracia scenes", "*.ply *.sog *.guf *.mint", "All files", "*"},
                      pfd::opt::multiselect)
           .result())
    out.push_back(utf8Path(file));
  return out;
}

VkDescriptorPool createImGuiPool(VkDevice device) {
  VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 16};
  VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
  pi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  pi.maxSets = 16;
  pi.poolSizeCount = 1;
  pi.pPoolSizes = &size;
  VkDescriptorPool pool = VK_NULL_HANDLE;
  VK_CHECK(vkCreateDescriptorPool(device, &pi, nullptr, &pool));
  return pool;
}

// --- UI ---------------------------------------------------------------------

void drawMenuBar(SplatViewer& viewer, bool& openStream, bool& openFlag) {
  if (!ImGui::BeginMainMenuBar()) return;
  if (ImGui::BeginMenu("File")) {
    if (ImGui::MenuItem("Open…", "Ctrl+O")) {
      auto paths = openFileDialog();
      if (!paths.empty())
        viewer.loadScenes(paths, paths.size() > 1 ? SceneLayout::Split
                                                  : SceneLayout::Mix);
    }
    if (ImGui::MenuItem("Open Stream…", "Ctrl+L")) openStream = true;
    if (ImGui::MenuItem("Set Flag…", "Ctrl+K")) openFlag = true;
    ImGui::Separator();
    if (ImGui::MenuItem("Clear Network Cache")) {
      const size_t n = viewer.player().clearCache();
      std::fprintf(stderr, "Cleared %zu cache entries\n", n);
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("%s", viewer.player().cacheDir().string().c_str());
    ImGui::EndMenu();
  }
  if (ImGui::BeginMenu("View")) {
    const bool split = viewer.layout() == SceneLayout::Split;
    if (ImGui::MenuItem("Split panes", "M", split, viewer.player().sceneCount() > 1))
      viewer.toggleLayout();
    if (ImGui::MenuItem("Depth as color", "D", viewer.depthAsColor()))
      viewer.toggleDepthAsColor();
    if (ImGui::MenuItem("Linearize depth", "L", viewer.depthLinear(), viewer.depthAsColor()))
      viewer.toggleDepthLinear();
    ImGui::EndMenu();
  }
  ImGui::EndMainMenuBar();
}

void drawHUD(SplatViewer& viewer, float fps) {
  ImGui::SetNextWindowPos(ImVec2(10, 28), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.35f);
  const ImGuiWindowFlags flags =
      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
      ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
      ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  const SplatsPlayer& p = viewer.player();
  ImGui::Begin("HUD", nullptr, flags);
  if (!p.hasScenes()) {
    ImGui::TextUnformatted(
        "Drop .ply / .sog / .guf / .mint files here, or File > Open.");
  } else {
    std::string names;
    for (size_t i = 0; i < p.sceneCount(); ++i) {
      if (i) names += ", ";
      names += p.sceneName(i);
    }
    const char* layout =
        p.sceneCount() > 1
            ? (viewer.layout() == SceneLayout::Split ? " [split]" : " [mix]")
            : "";
    ImGui::Text("%s%s", names.c_str(), layout);
    ImGui::Text("%u / %u splats", viewer.visibleSplats(), p.splatsBudget());
    ImGui::Text("%.0f fps  ·  GPU %.0f MB", fps, (double)p.gpuBytes() / 1e6);
  }
  if (!p.lastError().empty())
    ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", p.lastError().c_str());
  ImGui::End();
}

void drawAppearance(SplatViewer& viewer) {
  if (!viewer.player().hasScenes()) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + vp->WorkSize.x - 300, vp->WorkPos.y + 28),
                          ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowSize(ImVec2(288, 0), ImGuiCond_FirstUseEver);
  ImGui::Begin("Appearance");
  auto& app = viewer.appearance();
  bool changed = false;
  for (size_t i = 0; i < app.size(); ++i) {
    ImGui::PushID((int)i);
    if (ImGui::CollapsingHeader(viewer.player().sceneName(i).c_str(),
                                ImGuiTreeNodeFlags_DefaultOpen)) {
      changed |= ImGui::SliderFloat("Visibility", &app[i].visibility, 0.0f, 1.0f);
      changed |= ImGui::SliderFloat("Hue", &app[i].hue, -180.0f, 180.0f, "%.0f°");
      changed |= ImGui::SliderFloat("Saturation", &app[i].saturation, 0.0f, 2.0f);
      changed |= ImGui::SliderFloat("Value", &app[i].value, 0.0f, 2.0f);
      if (ImGui::SmallButton("Reset")) {
        app[i] = SceneAppearance{};
        changed = true;
      }
    }
    ImGui::PopID();
  }
  ImGui::End();
  if (changed) viewer.applyAppearance();
}

// Takes the place of the play/pause button while the video cannot show the
// current time, so the transport keeps its layout.
void drawSpinner() {
  const ImVec2 size(ImGui::CalcTextSize("Pause").x + ImGui::GetStyle().FramePadding.x * 2,
                    ImGui::GetFrameHeight());
  const ImVec2 at = ImGui::GetCursorScreenPos();
  const ImVec2 centre(at.x + size.x * 0.5f, at.y + size.y * 0.5f);
  const float t = (float)ImGui::GetTime() * 6.0f;
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PathArcTo(centre, size.y * 0.3f, t, t + 4.7f, 24);
  dl->PathStroke(ImGui::GetColorU32(ImGuiCol_Text), 0, 2.0f);
  ImGui::Dummy(size);
}

void drawTransport(SplatViewer& viewer) {
  SplatsPlayer& p = viewer.player();
  if (!p.hasVideo()) return;
  const ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x + 10, vp->WorkPos.y + vp->WorkSize.y - 58),
                          ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x - 20, 0), ImGuiCond_Always);
  ImGui::SetNextWindowBgAlpha(0.6f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration |
                                 ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
  ImGui::Begin("Transport", nullptr, flags);
  if (p.isBuffering())
    drawSpinner();
  else if (ImGui::Button(p.isPlaying() ? "Pause" : "Play "))
    p.togglePlay();
  ImGui::SameLine();

  float t = (float)p.playhead();
  const float dur = (float)p.duration();
  ImGui::SetNextItemWidth(vp->WorkSize.x * 0.55f);
  // NoRoundToFormat: otherwise "%.1fs" quantizes the playhead to 100ms steps.
  const bool moved = ImGui::SliderFloat("##time", &t, 0.0f, dur > 0 ? dur : 1.0f,
                                        "%.1fs", ImGuiSliderFlags_NoRoundToFormat);
  // The whole drag, not just the frames the value changes on.
  p.setScrubbing(ImGui::IsItemActive());
  if (moved) p.seek(t);
  ImGui::SameLine();
  ImGui::Text("%.1f / %.1f s", p.playhead(), p.duration());
  ImGui::SameLine();
  float speed = (float)p.playbackSpeed();
  ImGui::SetNextItemWidth(120);
  if (ImGui::SliderFloat("speed", &speed, 0.1f, 4.0f, "%.1f×"))
    p.setPlaybackSpeed(speed);
  ImGui::End();
}

void drawDialogs(SplatViewer& viewer, bool& openStream, bool& openFlag) {
  if (openStream) {
    ImGui::OpenPopup("Open Stream");
    openStream = false;
  }
  if (openFlag) {
    ImGui::OpenPopup("Set Flag");
    openFlag = false;
  }
  if (ImGui::BeginPopupModal("Open Stream", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    static char url[512] = "https://";
    static char token[256] = "";
    ImGui::InputText("URL", url, sizeof(url));
    ImGui::InputText("Token", token, sizeof(token));
    if (ImGui::Button("Open") && url[0]) {
      viewer.loadStream(url, token);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  if (ImGui::BeginPopupModal("Set Flag", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    static char key[128] = "";
    static bool on = true;
    ImGui::InputText("Key", key, sizeof(key));
    ImGui::Checkbox("On", &on);
    if (ImGui::Button("Apply") && key[0]) {
      viewer.player().setFlag(key, on);
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
}

struct Options {
  std::vector<std::filesystem::path> scenes;
  SceneLayout layout = SceneLayout::Mix;
  std::string stream, token;
  float zoom = 1.0f;
  double seek = -1.0;
};

Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--split") o.layout = SceneLayout::Split;
    else if (a == "--stream" && i + 1 < argc) o.stream = argv[++i];
    else if (a == "--token" && i + 1 < argc) o.token = argv[++i];
    else if (a == "--zoom" && i + 1 < argc) o.zoom = (float)atof(argv[++i]);
    else if (a == "--seek" && i + 1 < argc) o.seek = atof(argv[++i]);
    else if (!a.empty() && a[0] != '-') o.scenes.emplace_back(a);
  }
  return o;
}

}  // namespace

int main(int argc, char** argv) {
#if defined(_WIN32)
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#endif
  const Options opts = parseArgs(argc, argv);

  if (!glfwInit()) {
    std::fprintf(stderr, "glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window =
      glfwCreateWindow(1280, 800, "Gracia Splat Viewer (Vulkan)", nullptr, nullptr);
  if (!window) {
    glfwTerminate();
    return 1;
  }

  VulkanContext ctx;
  if (!ctx.init(window, "Gracia Splat Viewer")) {
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  auto viewer = std::make_unique<SplatViewer>(ctx);
  if (!viewer->player().initSdk(
          ctx.gpu(), std::filesystem::temp_directory_path() / TARGET_NAME "_vulkan_cache")) {
    std::fprintf(stderr, "SDK init failed: %s\n", viewer->player().lastError().c_str());
    viewer.reset();
    ctx.shutdown();
    glfwDestroyWindow(window);
    glfwTerminate();
    return 1;
  }

  if (!opts.scenes.empty()) {
    if (!viewer->loadScenes(opts.scenes, opts.layout))
      std::fprintf(stderr, "Load failed: %s\n", viewer->player().lastError().c_str());
    if (opts.zoom != 1.0f) viewer->camera().zoom(opts.zoom);
    if (opts.seek >= 0) {
      viewer->player().setPlaying(false);
      viewer->player().seek(opts.seek);
    }
  } else if (!opts.stream.empty()) {
    if (!viewer->loadStream(opts.stream, opts.token))
      std::fprintf(stderr, "Stream failed: %s\n", viewer->player().lastError().c_str());
  }

  AppState state{viewer.get(), &ctx};
  glfwSetWindowUserPointer(window, &state);
  glfwSetCursorPosCallback(window, onCursorPos);
  glfwSetMouseButtonCallback(window, onMouseButton);
  glfwSetScrollCallback(window, onScroll);
  glfwSetKeyCallback(window, onKey);
  glfwSetDropCallback(window, onDrop);
  glfwSetFramebufferSizeCallback(window, [](GLFWwindow* w, int, int) {
    app(w).ctx->requestResize();
  });

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui::StyleColorsDark();
  ImGui_ImplGlfw_InitForVulkan(window, true);
  const gvk::Gpu gpu = ctx.gpu();
  VkDescriptorPool imguiPool = createImGuiPool(gpu.device);
  ImGui_ImplVulkan_LoadFunctions(
      [](const char* name, void* user) {
        return vkGetInstanceProcAddr(reinterpret_cast<VkInstance>(user), name);
      },
      reinterpret_cast<void*>(gpu.instance));
  ImGui_ImplVulkan_InitInfo init{};
  init.Instance = gpu.instance;
  init.PhysicalDevice = gpu.physicalDevice;
  init.Device = gpu.device;
  init.QueueFamily = gpu.queues.graphics;
  init.Queue = ctx.graphicsQueue();
  init.DescriptorPool = imguiPool;
  const VkFormat imguiColorFormat = ctx.colorFormat();
  init.UseDynamicRendering = true;
  init.PipelineRenderingCreateInfo = {VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR};
  init.PipelineRenderingCreateInfo.colorAttachmentCount = 1;
  init.PipelineRenderingCreateInfo.pColorAttachmentFormats = &imguiColorFormat;
  init.MinImageCount = ctx.minImageCount();
  init.ImageCount = ctx.imageCount();
  init.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  ImGui_ImplVulkan_Init(&init);

  bool openStream = false, openFlag = false;
  double lastTime = glfwGetTime();
  float fps = 0;

  while (!glfwWindowShouldClose(window) && !gQuit.load(std::memory_order_relaxed)) {
    glfwPollEvents();

    if (!state.pendingLoad.empty()) {
      viewer->loadScenes(state.pendingLoad,
                         state.pendingLoad.size() > 1 ? SceneLayout::Split
                                                      : SceneLayout::Mix);
      state.pendingLoad.clear();
    }

    const double now = glfwGetTime();
    float dt = (float)(now - lastTime);
    lastTime = now;
    if (dt > 0) fps += (1.0f / dt - fps) * 0.1f;

    viewer->advance(dt);

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    drawMenuBar(*viewer, openStream, openFlag);
    drawHUD(*viewer, fps);
    drawAppearance(*viewer);
    drawTransport(*viewer);
    drawDialogs(*viewer, openStream, openFlag);
    ImGui::Render();
    ImDrawData* drawData = ImGui::GetDrawData();

    auto frame = ctx.beginFrame();
    if (!frame) continue;
    viewer->prepare(*frame);
    ctx.render(*frame, [&](VkCommandBuffer cmd) {
      viewer->record(*frame, cmd);
      ImGui_ImplVulkan_RenderDrawData(drawData, cmd);
    });
    ctx.endFrame(*frame);
  }

  ctx.waitIdle();
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  vkDestroyDescriptorPool(gpu.device, imguiPool, nullptr);
  viewer.reset();
  ctx.shutdown();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
