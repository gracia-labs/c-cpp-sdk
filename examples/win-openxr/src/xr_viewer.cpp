#include "xr_viewer.hpp"

#include <glm/gtc/type_ptr.hpp>

#include "xr_math.hpp"

#include <algorithm>

namespace {

// The scene is scaled to a small radius a couple of metres away. A depth
// attachment would need compareOp GREATER and a 0.0 clear (reverse-Z).
constexpr float kNearZ = 1e-5f;
constexpr float kFarZ = 1e2f;

}  // namespace

bool XrViewer::load(const XrContext& xr,
                    const std::vector<std::filesystem::path>& scenes,
                    const std::string& streamUrl, const std::string& token) {
  color_ = xr.colorFormat();
  const bool ok = scenes.empty() ? player_.loadStream(streamUrl, token)
                                 : player_.loadScenes(scenes);
  return ok && rebuildRenderer();
}

void XrViewer::shutdown() {
  for (auto& f : frames_) f = {};
  renderer_ = {};
  player_.reset();
}

bool XrViewer::rebuildRenderer() {
  for (auto& f : frames_) f = {};
  renderer_ = {};

  // One renderer: a headset has no split layout. vr=true here is half of
  // stereo; render() carries the other half.
  renderer_ = gracia::SdkSplatsRenderer::create(
      player_.sdk(), std::max(player_.splatsBudget(), 1u), true);
  if (!renderer_) return false;
  renderer_.setFlag("splats_count_readback", true);
  checkedStereo_ = false;
  return true;
}

void XrViewer::advance(float dt) {
  player_.advance(dt);
  // Late metadata: rebuild on the real budget, place the scene again.
  if (player_.consumeSettled()) {
    rebuildRenderer();
    placeScene(scale_, distance_, height_);
  }
}

void XrViewer::placeScene(float scaleMeters, float distanceMeters,
                          float heightMeters) {
  scale_ = scaleMeters;
  distance_ = distanceMeters;
  height_ = heightMeters;
  SceneGroup* group = player_.group();
  if (!group) return;
  const BBox box = group->boundingBox();
  if (!box.valid()) return;  // a stream has no box until its metadata lands
  group->setTransform(placeSceneForXr(box, scaleMeters, distanceMeters, heightMeters));
}

void XrViewer::prepare(const XrFrameCtx& f) {
  auto& draws = frames_[f.slot];
  draws = {};
  SceneGroup* group = player_.group();
  if (!player_.hasScenes() || !renderer_) return;

  glm::mat4 proj[2], cam[2];
  for (int i = 0; i < 2; ++i) {
    proj[i] = xrProjection(f.views[i].fov, kNearZ, kFarZ);
    cam[i] = xrPose(f.views[i].pose);  // camera-to-world; the SDK inverts it
  }
  glm::mat4 loco(1.0f);
  float* projections[2] = {glm::value_ptr(proj[0]), glm::value_ptr(proj[1])};
  float* viewTransforms[2] = {glm::value_ptr(cam[0]), glm::value_ptr(cam[1])};

  std::vector<Scene::RenderRef> objs;
  objs.reserve(group->size());
  for (const auto& s : group->scenes()) objs.push_back(s->renderRef());

  // One call for both eyes: per eye would burn two of the SDK's three ring slots.
  GraciaCommandBuffer gcb{};
  gcb.handle = f.cmd;
  // No motion or mesh pass: the headset viewer draws color and nothing else.
  draws = renderer_.render(
      objs, projections, viewTransforms, glm::value_ptr(loco), f.eyeExtent.width,
      f.eyeExtent.height, true,
      makeSplatsPass(color_, GRACIA_STEREO_MODE_MULTIPASS),
      /*motionPass=*/nullptr, /*meshPass=*/nullptr, gcb);

  player_.setBuffering(draws.buffering);

  // One view back means stereo is off: a stable mono picture, and no error.
  if (!checkedStereo_ && !draws.color.views.empty()) {
    checkedStereo_ = true;
    if (draws.color.views.size() < XrContext::kEyes)
      std::fprintf(stderr,
                   "WARNING: the SDK returned %zu view(s), not 2. Stereo is off; "
                   "check the vr flag on both create() and render().\n",
                   draws.color.views.size());
  }
}

void XrViewer::record(uint32_t slot, uint32_t eye, VkCommandBuffer cmd) {
  auto& draws = frames_[slot];
  // Draw while buffering too: the SDK holds the last decoded frame.
  if (eye >= draws.color.views.size() || !draws.color.views[eye].has_value()) return;

  // renderPass stays null: the SDK then records with dynamic rendering.
  GraciaRenderPass rp{};
  rp.commandBuffer = cmd;
  draws.color.views[eye]->execute(rp);
}
