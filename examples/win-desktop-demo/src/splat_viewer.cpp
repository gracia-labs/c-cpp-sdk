#include "splat_viewer.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cstdint>

SplatViewer::~SplatViewer() {
  ctx_.waitIdle();
  for (auto& f : frames_) f.clear();
  renderers_.clear();
  player_.reset();
}

bool SplatViewer::loadScenes(const std::vector<std::filesystem::path>& paths,
                             SceneLayout layout) {
  ctx_.waitIdle();
  if (!player_.loadScenes(paths)) return false;
  return afterLoad(layout);
}

bool SplatViewer::loadStream(const std::string& url, const std::string& token) {
  ctx_.waitIdle();
  if (!player_.loadStream(url, token)) return false;
  return afterLoad(SceneLayout::Mix);
}

bool SplatViewer::afterLoad(SceneLayout layout) {
  const size_t count = player_.sceneCount();
  layout_ = count > 1 ? layout : SceneLayout::Mix;
  if (!rebuildRenderers()) {
    player_.reset();
    return false;
  }
  appearance_.assign(count, SceneAppearance{});
  autoFrames_ = true;
  reframe();
  split_.reset(layout_ == SceneLayout::Split ? (int)count : 0);
  applyAppearance();
  return true;
}

bool SplatViewer::rebuildRenderers() {
  for (auto& f : frames_) f.clear();
  renderers_.clear();
  SceneGroup* group = player_.group();
  if (!group) return true;

  std::vector<uint32_t> budgets;
  if (layout_ == SceneLayout::Mix) {
    budgets.push_back(group->splatsBudget());
  } else {
    for (const auto& s : group->scenes()) budgets.push_back(s->splatsBudget());
  }
  for (uint32_t b : budgets) {
    auto r = gracia::SdkSplatsRenderer::create(player_.sdk(), std::max(b, 1u), false);
    if (!r) {
      renderers_.clear();
      return false;
    }
    r.setFlag("splats_count_readback", true);
    renderers_.push_back({std::move(r), gracia::SdkDepthResolver::create(player_.sdk())});
  }
  return true;
}

void SplatViewer::setLayout(SceneLayout layout) {
  if (player_.sceneCount() <= 1 || layout == layout_) return;
  ctx_.waitIdle();
  layout_ = layout;
  rebuildRenderers();
  split_.reset(layout_ == SceneLayout::Split ? (int)player_.sceneCount() : 0);
}

void SplatViewer::reframe() {
  SceneGroup* group = player_.group();
  if (!group) return;
  const BBox full = group->boundingBox();
  if (!full.valid()) {
    camera_.setBBox(full, full, autoFrames_);
    return;
  }
  const BBox subject = group->adaptiveBBox();
  camera_.setBBox(full, subject.valid() ? subject : full, autoFrames_);
}

void SplatViewer::applyAppearance() {
  SceneGroup* group = player_.group();
  if (!group) return;
  const auto& scenes = group->scenes();
  for (size_t i = 0; i < scenes.size() && i < appearance_.size(); ++i) {
    const SceneAppearance& a = appearance_[i];
    scenes[i]->setVisibility(a.visibility);
    scenes[i]->setColorGrade(a.hue / 360.0f, a.saturation, a.value, 1.0f);
  }
}

void SplatViewer::advance(float dt) {
  player_.advance(dt);

  // Late video metadata landed: rebuild renderers on the real budgets and reframe.
  if (player_.consumeSettled()) {
    ctx_.waitIdle();
    rebuildRenderers();
    reframe();
    applyAppearance();
  }

  camera_.update(dt);
}

void SplatViewer::prepare(const Frame& frame) {
  const uint32_t slot = frame.slot;
  const size_t n = renderers_.size();
  auto& draws = frames_[slot];
  draws.clear();
  draws.resize(n);  // FrameDraws is move-only; default-construct in place
  SceneGroup* group = player_.group();
  if (!player_.hasScenes() || !camera_.isFramed() || n == 0) return;

  const auto& scenes = group->scenes();
  const float aspect = (float)frame.extent.width / (float)frame.extent.height;
  const RenderView view = camera_.renderView(aspect, (float)frame.extent.height);
  const uint32_t w = frame.extent.width, h = frame.extent.height;

  const GraciaRendererSplatsPass pass =
      makeSplatsPass(GRACIA_COLOR_FORMAT_RGBA8_UNORM, GRACIA_STEREO_MODE_MULTIPASS);
  glm::mat4 proj = view.projection;
  glm::mat4 cam = view.cameraToWorld;
  glm::mat4 loco(1.0f);
  float* projections[2] = {glm::value_ptr(proj), glm::value_ptr(proj)};
  float* viewTransforms[2] = {glm::value_ptr(cam), glm::value_ptr(cam)};

  // The SDK's compute prep goes into this frame's buffer, before the render
  // pass; vk_context barriers it before the draws replay.
  GraciaCommandBuffer gcb{};
  gcb.handle = frame.cmd;
  auto renderInto = [&](SceneRenderer& r,
                        const std::vector<Scene::RenderRef>& objs) {
    return r.splats.render(objs, projections, viewTransforms, glm::value_ptr(loco), w, h,
                           false, pass, nullptr, nullptr, gcb);
  };

  if (layout_ == SceneLayout::Mix) {
    std::vector<Scene::RenderRef> objs;
    objs.reserve(scenes.size());
    for (const auto& s : scenes) objs.push_back(s->renderRef());
    draws[0].splats = renderInto(renderers_[0], objs);
  } else {
    for (size_t i = 0; i < n && i < scenes.size(); ++i) {
      std::vector<Scene::RenderRef> objs{scenes[i]->renderRef()};
      draws[i].splats = renderInto(renderers_[i], objs);
    }
  }

  bool anyBuffering = false;
  for (size_t i = 0; i < n; ++i) {
    auto& fd = draws[i];
    auto& dc = fd.splats;
    // Draw while buffering too: the SDK holds the last decoded frame, so the
    // picture freezes instead of blanking. `buffering` is HUD state, not a gate.
    fd.haveColor = !dc.color.views.empty() && dc.color.views[0].has_value();
    anyBuffering = anyBuffering || dc.buffering;

    // Resolve the splat depth data into a color draw call executed in record().
    if (depthAsColor_ && fd.haveColor && !dc.depth.views.empty() &&
        dc.depth.views[0].has_value()) {
      gracia::DrawCall* depthView = &dc.depth.views[0].value();
      // Linearizing spreads over near/far, so fit the projection to the subject.
      glm::mat4 resolveProj = depthLinear_ ? camera_.subjectProjection() : proj;
      fd.resolve = renderers_[i].resolver.render(
          {depthView}, dc.depth.stereo.value_or(GRACIA_STEREO_MODE_MULTIPASS),
          w, h, {glm::value_ptr(resolveProj)},
          GRACIA_COLOR_FORMAT_RGBA8_UNORM, GRACIA_DEPTH_FORMAT_UNDEFINED, depthLinear_, gcb);
    }
  }
  player_.setBuffering(anyBuffering);
}

void SplatViewer::record(const Frame& frame, VkCommandBuffer cmd) {
  auto& draws = frames_[frame.slot];
  if (draws.empty()) return;

  auto exec = [&](size_t i) {
    GraciaRenderPass rp{};
    rp.commandBuffer = cmd;
    rp.renderPass = ctx_.renderPass();
    auto& rc = draws[i].resolve.color;
    if (depthAsColor_ && !rc.views.empty() && rc.views[0].has_value())
      rc.views[0]->execute(rp);
    else
      draws[i].splats.color.views[0]->execute(rp);
  };

  if (layout_ == SceneLayout::Mix) {
    if (draws[0].haveColor) exec(0);
    return;
  }
  const auto rects = split_.scissorRects(frame.extent.width, frame.extent.height);
  for (size_t i = 0; i < draws.size(); ++i) {
    if (i >= rects.size() || rects[i].extent.width == 0 || !draws[i].haveColor) continue;
    vkCmdSetScissor(cmd, 0, 1, &rects[i]);
    exec(i);
  }
}

uint32_t SplatViewer::visibleSplats() const {
  uint32_t total = 0;
  for (const auto& r : renderers_) total += r.splats.readVisibleSplatsCount();
  return total;
}
