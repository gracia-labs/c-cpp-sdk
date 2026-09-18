#pragma once

#include "vk_context.hpp"

#include <gracia/SDK.hpp>
#include <gracia_demo/player.hpp>
#include <gracia_demo/scene.hpp>

#include "camera.hpp"
#include "split_layout.hpp"

#include <array>
#include <filesystem>
#include <string>
#include <vector>

enum class SceneLayout { Mix, Split };

// Per-scene appearance, tuned independently in the UI (matches the Mac viewer).
struct SceneAppearance {
  float visibility = 1.0f;  // 0..1
  float hue = 0.0f;         // degrees, -180..180 (applied as turns = deg/360)
  float saturation = 1.0f;  // 0..2
  float value = 1.0f;       // 0..2
};

// A splats renderer paired with its depth resolver: one per scene in Split, one in Mix.
struct SceneRenderer {
  gracia::SdkSplatsRenderer splats;
  gracia::SdkDepthResolver resolver;
};

// One SceneRenderer's draws for a single in-flight frame.
struct FrameDraws {
  gracia::DrawCalls splats;               // color/depth/... from the splats renderer
  gracia::DepthResolveDrawCalls resolve;  // depth-as-color resolve, when enabled
  bool haveColor = false;
};

// The desktop half of the viewer: layout, renderers, camera and the appearance
// UI state. Everything platform agnostic — the SDK context, the scenes and the
// playback clock — lives in SplatsPlayer.
class SplatViewer {
 public:
  explicit SplatViewer(VulkanContext& ctx) : ctx_(ctx), input_(camera_) {}
  ~SplatViewer();

  SplatViewer(const SplatViewer&) = delete;
  SplatViewer& operator=(const SplatViewer&) = delete;

  bool loadScenes(const std::vector<std::filesystem::path>& paths, SceneLayout layout);
  bool loadStream(const std::string& url, const std::string& token);

  // Per frame, in order: advance (playback clock + settle + camera), then
  // prepare (SDK compute prep, blocking) before the render pass, then record.
  void advance(float dt);
  void prepare(const Frame& frame);
  void record(const Frame& frame, VkCommandBuffer cmd);

  // Playback and scenes.
  SplatsPlayer& player() { return player_; }
  const SplatsPlayer& player() const { return player_; }

  // Camera / input.
  SceneCamera& camera() { return camera_; }
  CameraInput& input() { return input_; }
  SplitLayout& split() { return split_; }
  SceneLayout layout() const { return layout_; }
  void setLayout(SceneLayout layout);
  void toggleLayout() {
    setLayout(layout_ == SceneLayout::Mix ? SceneLayout::Split : SceneLayout::Mix);
  }

  // Appearance.
  std::vector<SceneAppearance>& appearance() { return appearance_; }
  void applyAppearance();

  // Render the resolved splat depth as color instead of the shaded splats.
  bool depthAsColor() const { return depthAsColor_; }
  void toggleDepthAsColor() { depthAsColor_ = !depthAsColor_; }
  // Depth-as-color: linearized (near/far-normalized) vs raw projected device depth.
  bool depthLinear() const { return depthLinear_; }
  void toggleDepthLinear() { depthLinear_ = !depthLinear_; }
  // Prepare and sort without subgroup operations, for GPUs that lack them.
  bool legacyPipeline() const { return legacyPipeline_; }
  void toggleLegacyPipeline();

  uint32_t visibleSplats() const;

 private:
  bool afterLoad(SceneLayout layout);
  bool rebuildRenderers();
  void reframe();

  VulkanContext& ctx_;
  SplatsPlayer player_;
  std::vector<SceneRenderer> renderers_;
  bool depthAsColor_ = false;
  bool depthLinear_ = false;
  bool legacyPipeline_ = false;
  SceneLayout layout_ = SceneLayout::Mix;
  SplitLayout split_;

  SceneCamera camera_;
  CameraInput input_;

  std::vector<SceneAppearance> appearance_;
  bool autoFrames_ = true;

  // Draws per in-flight slot, one entry per renderer.
  std::array<std::vector<FrameDraws>, VulkanContext::kFramesInFlight> frames_;
};
