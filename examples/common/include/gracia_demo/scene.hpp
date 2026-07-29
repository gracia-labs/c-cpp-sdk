#pragma once

#include <gracia_demo/math3d.hpp>

#include <gracia/SDK.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <variant>
#include <vector>

// A single loaded scene: static splats (.ply/.sog/.guf) or MINT volumetric video
// (file or HTTP stream). Wraps the SDK's own RAII types (gracia::StaticSplats /
// gracia::VideoStream) and dispatches to whichever it holds. Video metadata
// settles late, so the cached numbers are refreshed by pump().
class Scene {
 public:
  // The reference type SdkSplatsRenderer::render() consumes.
  using RenderRef = std::variant<gracia::StaticSplats*, gracia::VideoStream*>;

  Scene(const Scene&) = delete;
  Scene& operator=(const Scene&) = delete;

  static std::unique_ptr<Scene> create(const gracia::Context* context,
                                       const std::filesystem::path& path);
  static std::unique_ptr<Scene> createStream(const gracia::Context* context,
                                             const std::string& url,
                                             const std::string& token);

  bool isVideo() const { return video_; }
  const std::string& name() const { return name_; }
  RenderRef renderRef();

  const BBox& boundingBox() const { return bbox_; }
  const BBox& adaptiveBBox() const { return adaptiveBBox_; }
  uint32_t splatsBudget() const { return splatsBudget_; }
  double duration() const { return duration_; }
  bool isSettled() const {
    return splatsBudget_ > 0 && bbox_.valid() && (!video_ || duration_ > 0);
  }

  void setColorGrade(float hue, float saturation, float value, float weight);
  void setVisibility(float v);
  void setFlag(const std::string& key, bool value);
  void setTransform(const glm::mat4& m);

  void setTime(double t);   // video only
  bool pump();              // drives decode; true once frames are ready
  void setPlaybackRange(double start, double end);

 private:
  using Object = std::variant<gracia::StaticSplats, gracia::VideoStream>;
  Scene(Object obj, bool video, std::string name, BBox bbox, BBox adaptive,
        uint32_t budget, double duration)
      : obj_(std::move(obj)), video_(video), name_(std::move(name)), bbox_(bbox),
        adaptiveBBox_(adaptive), splatsBudget_(budget), duration_(duration) {}
  void settle();
  gracia::StaticSplats& stat() { return std::get<gracia::StaticSplats>(obj_); }
  gracia::VideoStream& vid() { return std::get<gracia::VideoStream>(obj_); }

  Object obj_;
  bool video_ = false;
  std::string name_;
  BBox bbox_;
  BBox adaptiveBBox_;
  uint32_t splatsBudget_ = 0;
  double duration_ = 0;
};

// Several scenes as one: union bbox, summed budget, one clock. Read live from the
// scenes each call so a video's late numbers are picked up.
class SceneGroup {
 public:
  explicit SceneGroup(std::vector<std::unique_ptr<Scene>> scenes)
      : scenes_(std::move(scenes)) {}

  const std::vector<std::unique_ptr<Scene>>& scenes() const { return scenes_; }
  size_t size() const { return scenes_.size(); }
  bool empty() const { return scenes_.empty(); }

  BBox boundingBox() const;
  BBox adaptiveBBox() const;
  uint32_t splatsBudget() const;
  double duration() const;
  bool isSettled() const;
  bool hasVideo() const;

  void setTransform(const glm::mat4& m);
  void setTime(double t);
  bool pump();
  void setPlaybackRange(double start, double end);
  void setFlag(const std::string& key, bool value);

 private:
  std::vector<std::unique_ptr<Scene>> scenes_;
};
