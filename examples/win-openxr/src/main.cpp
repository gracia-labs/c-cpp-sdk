#include "xr_context.hpp"
#include "xr_viewer.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
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

BOOL WINAPI onConsoleCtrl(DWORD type) {
  if (type != CTRL_C_EVENT && type != CTRL_BREAK_EVENT && type != CTRL_CLOSE_EVENT)
    return FALSE;
  gQuit.store(true, std::memory_order_relaxed);
  return TRUE;
}

struct Options {
  std::vector<std::filesystem::path> scenes;
  std::string stream, token;
  double seek = -1.0;
  float scale = 0.75f;     // target scene radius, metres
  float distance = 2.0f;   // in front of the origin, metres
  float height = 0.0f;     // above the origin, metres
};

Options parseArgs(int argc, char** argv) {
  Options o;
  for (int i = 1; i < argc; ++i) {
    std::string a = argv[i];
    if (a == "--stream" && i + 1 < argc) o.stream = argv[++i];
    else if (a == "--token" && i + 1 < argc) o.token = argv[++i];
    else if (a == "--seek" && i + 1 < argc) o.seek = atof(argv[++i]);
    else if (a == "--scale" && i + 1 < argc) o.scale = (float)atof(argv[++i]);
    else if (a == "--distance" && i + 1 < argc) o.distance = (float)atof(argv[++i]);
    else if (a == "--height" && i + 1 < argc) o.height = (float)atof(argv[++i]);
    else if (!a.empty() && a[0] != '-') o.scenes.emplace_back(a);
  }
  return o;
}

void usage() {
  std::printf(
      "usage: win_openxr_demo <scene.ply|.sog|.guf|.mint> [more scenes ...]\n"
      "       [--stream <url>] [--token <t>] [--seek <s>]\n"
      "       [--scale <m>] [--distance <m>] [--height <m>]\n");
}

}  // namespace

int main(int argc, char** argv) {
  // An XR runtime can leave buffered stdout unflushed at exit.
  setvbuf(stdout, nullptr, _IONBF, 0);
  SetConsoleCtrlHandler(onConsoleCtrl, TRUE);

  const Options opts = parseArgs(argc, argv);
  if (opts.scenes.empty() && opts.stream.empty()) {
    usage();
    return 1;
  }

  XrContext xr;
  if (!xr.init("Gracia OpenXR Viewer")) return 1;
  std::printf("OpenXR runtime: %s\n", xr.runtimeName().c_str());

  XrViewer viewer;
  if (!viewer.player().initSdk(
          xr.gpu(), std::filesystem::temp_directory_path() / "gracia_test_cache") ||
      !viewer.load(xr, opts.scenes, opts.stream, opts.token)) {
    std::fprintf(stderr, "Failed: %s\n", viewer.player().lastError().c_str());
    viewer.shutdown();
    xr.shutdown();
    return 1;
  }
  viewer.placeScene(opts.scale, opts.distance, opts.height);
  if (opts.seek >= 0) viewer.player().seek(opts.seek);

  // A stream has no budget until its metadata lands.
  if (const uint32_t budget = viewer.player().splatsBudget())
    std::printf("%u splats budgeted.\n", budget);
  std::printf("Put the headset on.\n");

  XrTime lastDisplayTime = 0;
  while (xr.pollEvents(gQuit.load(std::memory_order_relaxed))) {
    if (!xr.sessionRunning()) {
      // xrWaitFrame is illegal here, but pump() still has to run.
      viewer.advance(0.0f);
      std::this_thread::sleep_for(std::chrono::milliseconds(250));
      continue;
    }

    const XrFrameState fs = xr.waitFrame();
    float dt = lastDisplayTime
                   ? (float)(fs.predictedDisplayTime - lastDisplayTime) * 1e-9f
                   : 0.0f;
    lastDisplayTime = fs.predictedDisplayTime;
    dt = std::clamp(dt, 0.0f, 0.1f);  // survive a runtime hiccup

    viewer.advance(dt);

    xr.beginFrame();
    std::optional<XrFrameCtx> f = xr.acquire(fs);
    if (f) {
      viewer.prepare(*f);
      xr.render(*f, [&](VkCommandBuffer cmd, uint32_t eye) {
        viewer.record(f->slot, eye, cmd);
      });
    }
    xr.endFrame(fs, f);
  }

  xr.waitIdle();
  viewer.shutdown();
  xr.shutdown();
  return 0;
}
