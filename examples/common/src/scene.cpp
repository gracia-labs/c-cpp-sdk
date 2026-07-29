#include <gracia_demo/scene.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>

namespace {

std::string lowerExt(const std::filesystem::path& path) {
  std::string ext = path.extension().string();
  for (char& c : ext) c = (char)std::tolower((unsigned char)c);
  return ext;
}

BBox toBBox(const GraciaBBox& b) { return BBox::fromMinMax(b.minmax); }

uint32_t clampU32(uint64_t v) { return v > UINT32_MAX ? UINT32_MAX : (uint32_t)v; }

// Authored bounds from a MINT's layout — readable before the first frame decodes.
BBox authoredBBox(const std::filesystem::path& path) {
  gracia::VideoDesc desc(nullptr, path);
  if (!desc.get()) return {};
  BBox b = toBBox(desc.bbox());
  return b.valid() ? b : BBox{};
}

}  // namespace

std::unique_ptr<Scene> Scene::create(const gracia::Context* context,
                                     const std::filesystem::path& path) {
  const std::string ext = lowerExt(path);
  GraciaContentType type = GRACIA_CONTENT_UNKNOWN;
  if (ext == ".ply") type = GRACIA_CONTENT_PLY;
  else if (ext == ".sog") type = GRACIA_CONTENT_SOG;
  else if (ext == ".guf") type = GRACIA_CONTENT_GUF;

  if (type != GRACIA_CONTENT_UNKNOWN) {
    gracia::StaticSplats splats(context, nullptr, path, type);
    if (!splats.get()) return nullptr;
    const uint32_t count = splats.count();
    if (count == 0) return nullptr;
    BBox bbox = toBBox(splats.bbox());
    BBox adaptive = toBBox(splats.adaptiveBBox());
    return std::unique_ptr<Scene>(new Scene(std::move(splats), false,
                                            path.filename().string(), bbox, adaptive,
                                            count, 0.0));
  }

  // Anything else: try it as MINT video (named .mint, or one despite no ext).
  if (ext != ".mint" && gracia::videoMintVersionFromFile(path) == 0) return nullptr;
  gracia::VideoStream stream(context, static_cast<const char*>(nullptr), path, false);
  if (!stream.get()) return nullptr;
  BBox authored = authoredBBox(path);
  BBox bbox = authored.valid() ? authored : toBBox(stream.bbox());
  const uint32_t budget = clampU32(stream.maxSplatsCount());
  const double duration = stream.duration();
  return std::unique_ptr<Scene>(new Scene(std::move(stream), true,
                                          path.filename().string(), bbox, bbox, budget,
                                          duration));
}

std::unique_ptr<Scene> Scene::createStream(const gracia::Context* context,
                                           const std::string& url,
                                           const std::string& token) {
  gracia::VideoStream stream(context, url.c_str(), token.empty() ? nullptr : token.c_str());
  if (!stream.get()) return nullptr;
  std::string name = url;
  if (auto slash = url.find_last_of('/'); slash != std::string::npos)
    name = url.substr(slash + 1);
  BBox bbox = toBBox(stream.bbox());
  BBox adaptive = toBBox(stream.adaptiveBBox());
  const uint32_t budget = clampU32(stream.maxSplatsCount());
  const double duration = stream.duration();
  return std::unique_ptr<Scene>(new Scene(std::move(stream), true,
                                          name.empty() ? url : name, bbox, adaptive,
                                          budget, duration));
}

Scene::RenderRef Scene::renderRef() {
  if (video_) return &vid();
  return &stat();
}

void Scene::setColorGrade(float hue, float saturation, float value, float weight) {
  if (video_) vid().setColorGrade(hue, saturation, value, weight);
  else stat().setColorGrade(hue, saturation, value, weight);
}

void Scene::setVisibility(float v) {
  if (video_) vid().setVisibility(v);
  else stat().setVisibility(v);
}

void Scene::setFlag(const std::string& key, bool value) {
  if (video_) vid().setFlag(key.c_str(), value);
  else stat().setFlag(key.c_str(), value);
}

void Scene::setTransform(const glm::mat4& m) {
  float cols[16];
  std::copy_n(glm::value_ptr(m), 16, cols);
  if (video_) vid().setTransform(cols);
  else stat().setTransform(cols);
}

void Scene::setTime(double t) {
  if (video_) vid().setTime(t);
}

bool Scene::pump() {
  if (!video_) return false;
  if (!isSettled()) settle();
  return vid().ready();
}

void Scene::settle() {
  if (duration_ == 0) duration_ = vid().duration();
  if (splatsBudget_ == 0) splatsBudget_ = clampU32(vid().maxSplatsCount());
  if (!bbox_.valid()) {
    bbox_ = toBBox(vid().bbox());
    adaptiveBBox_ = toBBox(vid().adaptiveBBox());
  }
}

void Scene::setPlaybackRange(double start, double end) {
  if (!video_) return;
  vid().setPlaybackStart(start);
  vid().setPlaybackEnd(end);
}

// --- SceneGroup -------------------------------------------------------------

BBox SceneGroup::boundingBox() const {
  BBox b;
  for (const auto& s : scenes_) b = b.unite(s->boundingBox());
  return b;
}

BBox SceneGroup::adaptiveBBox() const {
  BBox b;
  for (const auto& s : scenes_) b = b.unite(s->adaptiveBBox());
  return b;
}

uint32_t SceneGroup::splatsBudget() const {
  uint64_t total = 0;
  for (const auto& s : scenes_) total += s->splatsBudget();
  return clampU32(total);
}

double SceneGroup::duration() const {
  double d = 0;
  for (const auto& s : scenes_) {
    const double sd = s->duration();
    if (sd > 0 && (d == 0 || sd < d)) d = sd;
  }
  return d;
}

bool SceneGroup::isSettled() const {
  for (const auto& s : scenes_)
    if (!s->isSettled()) return false;
  return true;
}

bool SceneGroup::hasVideo() const {
  for (const auto& s : scenes_)
    if (s->isVideo()) return true;
  return false;
}

void SceneGroup::setTransform(const glm::mat4& m) {
  for (auto& s : scenes_) s->setTransform(m);
}

void SceneGroup::setTime(double t) {
  for (auto& s : scenes_) s->setTime(t);
}

bool SceneGroup::pump() {
  bool ready = true;
  for (auto& s : scenes_)
    if (s->isVideo()) ready = s->pump() && ready;  // pump all; don't short-circuit
  return ready;
}

void SceneGroup::setPlaybackRange(double start, double end) {
  for (auto& s : scenes_) s->setPlaybackRange(start, end);
}

void SceneGroup::setFlag(const std::string& key, bool value) {
  for (auto& s : scenes_) s->setFlag(key, value);
}
