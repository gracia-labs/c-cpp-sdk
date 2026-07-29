#pragma once

#include "xr_context.hpp"

#include <gracia/SDK.hpp>
#include <gracia_demo/player.hpp>

#include <array>
#include <filesystem>

// The headset half of the viewer: one stereo renderer and the scene placement.
// Everything platform agnostic — the SDK context, the scenes and the playback
// clock — lives in SplatsPlayer. The head pose is the camera, so there is none.
class XrViewer {
 public:
  ~XrViewer() { shutdown(); }

  bool load(const XrContext& xr, const std::vector<std::filesystem::path>& scenes,
            const std::string& streamUrl, const std::string& token);
  void shutdown();

  SplatsPlayer& player() { return player_; }

  // Per frame: advance the clock, then prepare (one SDK call for both eyes)
  // before the render passes, then record for each eye.
  void advance(float dt);
  void prepare(const XrFrameCtx& f);
  void record(uint32_t slot, uint32_t eye, VkCommandBuffer cmd);

  // Places the scene in front of the reference-space origin, and remembers the
  // numbers so advance() can place it again once late metadata gives the
  // bounding box its real size.
  void placeScene(float scaleMeters, float distanceMeters, float heightMeters);

  uint32_t visibleSplats() const {
    return renderer_ ? renderer_.readVisibleSplatsCount() : 0;
  }

 private:
  bool rebuildRenderer();

  SplatsPlayer player_;
  gracia::SdkSplatsRenderer renderer_;
  // Both eyes come from one render() call, so one entry holds both views.
  std::array<gracia::DrawCalls, XrContext::kFramesInFlight> frames_;
  bool checkedStereo_ = false;
  float scale_ = 0.75f, distance_ = 0.5f, height_ = 0.5f;
  GraciaColorFormat color_ = GRACIA_COLOR_FORMAT_RGBA8_UNORM;
  VkRenderPass pass_ = VK_NULL_HANDLE;
};
