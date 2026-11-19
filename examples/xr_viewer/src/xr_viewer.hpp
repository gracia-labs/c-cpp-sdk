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
            const std::string& streamUrl, const std::string& token,
            const std::vector<uint8_t>& key);
  void shutdown();

  SplatsPlayer& player() { return player_; }

  // Per frame: advance the clock, then prepare (one SDK call for both eyes)
  // before the render passes, then record for each eye.
  void advance(float dt);
  void prepare(const XrFrameCtx& f);
  void record(uint32_t slot, uint32_t eye, VkCommandBuffer cmd);

  // The placement of content with no VR viewpoint: in front of the origin of the
  // reference space, scaled to a radius. prepare() places the scene once its box
  // lands, and again after late metadata.
  void placeScene(float scaleMeters, float distanceMeters, float heightMeters);

  uint32_t visibleSplats() const {
    return renderer_ ? renderer_.readVisibleSplatsCount() : 0;
  }

 private:
  bool rebuildRenderer();
  void place(const std::optional<float>& floorY);

  SplatsPlayer player_;
  gracia::SdkSplatsRenderer renderer_;
  // Both eyes come from one render() call, so one entry holds both views.
  std::array<gracia::DrawCalls, XrContext::kFramesInFlight> frames_;
  bool checkedStereo_ = false;
  bool placed_ = false;
  bool hasFloor_ = false;
  bool reportedProblem_ = false;
  float scale_ = 0.75f, distance_ = 0.5f, height_ = 0.5f;
  GraciaColorFormat color_ = GRACIA_COLOR_FORMAT_RGBA8_UNORM;
};
