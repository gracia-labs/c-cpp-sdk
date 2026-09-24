#include <gracia_demo/player.hpp>

#include <algorithm>
#include <system_error>

GraciaRendererSplatsPass makeSplatsPass(GraciaColorFormat color,
                                        GraciaStereoMode stereo) {
  GraciaRendererSplatsPass p{};
  p.colorFormat = color;
  p.depth.format = GRACIA_DEPTH_FORMAT_UNDEFINED;  // color only
  p.sampleCount = 1;
  p.stereo = stereo;
  return p;
}

bool SplatsPlayer::initSdk(const gvk::Gpu& gpu,
                           const std::filesystem::path& cacheDir) {
  std::error_code ec;
  std::filesystem::create_directories(cacheDir, ec);
  cacheDir_ = cacheDir;

  GraciaContextDescriptor desc{};
  desc.getInstanceProcAddr = vkGetInstanceProcAddr;
  desc.instance = gpu.instance;
  desc.physicalDevice = gpu.physicalDevice;
  desc.device = gpu.device;
  desc.graphicsQueueFamilyIndex = gpu.queues.graphics;
  desc.computeQueueFamilyIndex = gpu.queues.compute;
  desc.transferQueueFamilyIndex = gpu.queues.transfer;

  sdk_ = std::make_unique<gracia::Context>(desc, cacheDir);
  if (!*sdk_) {
    lastError_ = "SDK context creation failed";
    sdk_.reset();
    return false;
  }
  return true;
}

bool SplatsPlayer::loadScenes(const std::vector<std::filesystem::path>& paths) {
  lastError_.clear();
  if (!sdk_) {
    lastError_ = "SDK not initialized";
    return false;
  }
  if (paths.empty()) return false;

  // Build every scene before tearing down, so a bad file leaves the current one.
  std::vector<std::unique_ptr<Scene>> scenes;
  for (const auto& p : paths) {
    auto s = Scene::create(sdk_.get(), p);
    if (!s) {
      lastError_ = "failed to load " + p.filename().string();
      return false;
    }
    scenes.push_back(std::move(s));
  }
  install(std::move(scenes));
  return true;
}

bool SplatsPlayer::loadStream(const std::string& url, const std::string& token) {
  lastError_.clear();
  if (!sdk_) {
    lastError_ = "SDK not initialized";
    return false;
  }
  auto s = Scene::createStream(sdk_.get(), url, token);
  if (!s) {
    lastError_ = "failed to open stream";
    return false;
  }
  std::vector<std::unique_ptr<Scene>> scenes;
  scenes.push_back(std::move(s));
  install(std::move(scenes));
  return true;
}

void SplatsPlayer::install(std::vector<std::unique_ptr<Scene>> scenes) {
  group_ = std::make_unique<SceneGroup>(std::move(scenes));
  playhead_ = 0;
  isPlaying_ = true;
  renderBuffering_ = false;
  isScrubbing_ = false;
  isReady_ = !group_->hasVideo();
  appliedSettle_ = group_->isSettled();
  settledPending_ = false;
  if (group_->duration() > 0) group_->setPlaybackRange(0, group_->duration());
}

size_t SplatsPlayer::clearCache() {
  std::error_code ec;
  size_t removed = 0;
  for (const auto& entry : std::filesystem::directory_iterator(cacheDir_, ec)) {
    std::error_code one;
    const auto n = std::filesystem::remove_all(entry.path(), one);
    if (!one) removed += (size_t)n;
  }
  return removed;
}

void SplatsPlayer::reset() {
  group_.reset();
  sdk_.reset();
}

void SplatsPlayer::setPlaybackSpeed(double s) {
  playbackSpeed_ = std::clamp(s, 0.0, 8.0);
}

void SplatsPlayer::seek(double t) {
  if (!group_ || group_->duration() <= 0) return;
  const double end = std::max(0.0, group_->duration() - 0.001);
  playhead_ = std::min(std::max(0.0, t), end);
  // Apply now: a viewer's UI runs after advance(), so a deferred seek renders a
  // frame stale.
  group_->setTime(playhead_);
}

void SplatsPlayer::advance(float dt) {
  if (group_ && group_->hasVideo()) {
    // Client owns the clock: dt only, parked while scrubbing or buffering.
    if (isPlaying_ && !isScrubbing_ && !isBuffering() && group_->duration() > 0) {
      playhead_ += dt * playbackSpeed_;
      if (playhead_ >= group_->duration()) playhead_ = 0;
    }
    group_->setTime(playhead_);
    // isReady_ comes from the decoder, not the draw calls: a viewer that skips a
    // frame would never report it and the stream deadlocks. AGENTS.md rule 4.
    isReady_ = group_->pump();
  }

  if (group_ && !appliedSettle_ && group_->isSettled()) {
    appliedSettle_ = true;
    settledPending_ = true;
    group_->setPlaybackRange(0, group_->duration());
  }
}

bool SplatsPlayer::consumeSettled() {
  const bool s = settledPending_;
  settledPending_ = false;
  return s;
}

std::string SplatsPlayer::sceneName(size_t i) const {
  return (group_ && i < group_->size()) ? group_->scenes()[i]->name() : std::string();
}

void SplatsPlayer::setFlag(const std::string& key, bool value) {
  if (group_) group_->setFlag(key, value);
}
