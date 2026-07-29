#pragma once

#include <gracia/SDK.hpp>

#include <gracia_demo/scene.hpp>
#include <gracia_demo/vk_common.hpp>

#include <filesystem>
#include <memory>
#include <string>
#include <vector>

// Colour only: splats are sorted back-to-front and alpha-blended.
GraciaRendererSplatsPass makeSplatsPass(GraciaColorFormat color,
                                        GraciaStereoMode stereo);

// The SDK context, the scenes and the playback clock: everything a viewer does
// that does not depend on what it draws into. Clock rules are in AGENTS.md.
class SplatsPlayer {
 public:
  bool initSdk(const gvk::Gpu& gpu, const std::filesystem::path& cacheDir);
  gracia::Context* sdk() const { return sdk_.get(); }

  const std::filesystem::path& cacheDir() const { return cacheDir_; }
  // Deletes the downloaded stream data. Entries the SDK still holds open are
  // skipped, so clearing while a stream plays leaves that stream's data.
  size_t clearCache();

  bool loadScenes(const std::vector<std::filesystem::path>& paths);
  bool loadStream(const std::string& url, const std::string& token);
  void reset();

  // Once per frame, always: pump() is the only network check.
  void advance(float dt);
  // What the renderer reported for the frame it just built.
  void setBuffering(bool anyBuffering) { renderBuffering_ = anyBuffering; }
  // True once after late metadata settled: rebuild renderers, frame again.
  bool consumeSettled();

  // Transport.
  void seek(double t);
  bool isPlaying() const { return isPlaying_; }
  void setPlaying(bool p) { isPlaying_ = p; }
  void togglePlay() { isPlaying_ = !isPlaying_; }
  double playbackSpeed() const { return playbackSpeed_; }
  void setPlaybackSpeed(double s);
  double playhead() const { return playhead_; }
  double duration() const { return group_ ? group_->duration() : 0.0; }
  // The video cannot show the current time: nothing decoded yet, or the renderer
  // is waiting on data. Parks the clock, and drives the transport spinner.
  bool isBuffering() const { return !isReady_ || renderBuffering_; }
  // Held while a scrub bar is dragged: the bar owns the clock then.
  void setScrubbing(bool s) { isScrubbing_ = s; }

  SceneGroup* group() const { return group_.get(); }
  bool hasScenes() const { return group_ && !group_->empty(); }
  bool hasVideo() const { return group_ && group_->hasVideo(); }
  size_t sceneCount() const { return group_ ? group_->size() : 0; }
  std::string sceneName(size_t i) const;
  uint32_t splatsBudget() const { return group_ ? group_->splatsBudget() : 0; }
  uint64_t gpuBytes() const { return sdk_ ? sdk_->gpuAllocatedBytes() : 0; }

  void setFlag(const std::string& key, bool value);
  const std::string& lastError() const { return lastError_; }

 private:
  void install(std::vector<std::unique_ptr<Scene>> scenes);

  std::unique_ptr<gracia::Context> sdk_;
  std::unique_ptr<SceneGroup> group_;
  std::filesystem::path cacheDir_;

  double playhead_ = 0;
  bool isPlaying_ = true;
  double playbackSpeed_ = 1.0;
  bool renderBuffering_ = false;
  bool isScrubbing_ = false;
  bool isReady_ = true;
  bool appliedSettle_ = true;
  bool settledPending_ = false;

  std::string lastError_;
};
