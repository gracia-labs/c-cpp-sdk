#include <gracia_demo/scene.hpp>

#include <glm/gtc/type_ptr.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <fstream>
#include <iterator>

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

std::string urlName(const std::string& url) {
  const auto slash = url.find_last_of('/');
  const std::string name = slash == std::string::npos ? url : url.substr(slash + 1);
  return name.empty() ? url : name;
}

// The path of a URL ends in .mint, before any query.
bool isMintUrl(const std::string& url) {
  std::string path = url.substr(0, url.find_first_of("?#"));
  for (char& c : path) c = (char)std::tolower((unsigned char)c);
  return path.size() >= 5 && path.compare(path.size() - 5, 5, ".mint") == 0;
}

}  // namespace

std::vector<uint8_t> readHeadFile(const std::filesystem::path& path) {
  std::ifstream in(path, std::ios::binary);
  return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

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

  // A .grv states its bounds in its head, which lands after the open.
  if (ext == ".grv") {
    std::filesystem::path headPath = path;
    headPath += ".head";
    const std::vector<uint8_t> head = readHeadFile(headPath);
    return fromVideo(gracia::VideoStream(context, nullptr, path, false, head),
                     path.filename().string(), {});
  }

  // Anything else: try it as MINT video (named .mint, or one despite no ext).
  if (ext != ".mint" && gracia::videoMintVersionFromFile(path) == 0) return nullptr;
  return fromVideo(gracia::VideoStream(context, nullptr, path, false),
                   path.filename().string(), authoredBBox(path));
}

std::unique_ptr<Scene> Scene::createStream(const gracia::Context* context,
                                           const std::string& url,
                                           const std::string& token,
                                           const std::vector<uint8_t>& head) {
  if (token.empty() && !isMintUrl(url))
    return fromVideo(gracia::VideoStream::fromGrvUrl(context, url.c_str(), head),
                     urlName(url), {});
  return fromVideo(
      gracia::VideoStream::fromUrl(context, url.c_str(), token.empty() ? nullptr : token.c_str()),
      urlName(url), {});
}

std::unique_ptr<Scene> Scene::fromVideo(gracia::VideoStream stream, std::string name,
                                        BBox authored) {
  if (!stream.get()) return nullptr;
  // A video has no percentile box: it frames on its whole box.
  const BBox bbox = authored.valid() ? authored : toBBox(stream.bbox());
  const uint32_t budget = clampU32(stream.maxSplatsCount());
  const double duration = stream.duration();
  return std::unique_ptr<Scene>(new Scene(std::move(stream), true, std::move(name), bbox,
                                          bbox, budget, duration));
}

Scene::RenderRef Scene::renderRef() {
  if (video_) return &vid();
  return &stat();
}

std::optional<GraciaPose> Scene::viewpoint(GraciaViewpoint display) const {
  return video_ ? vid().viewpoint(display) : std::nullopt;
}

std::optional<glm::vec3> Scene::pivot() const {
  if (!video_) return std::nullopt;
  const auto p = vid().pivot();
  if (!p) return std::nullopt;
  return glm::vec3((*p)[0], (*p)[1], (*p)[2]);
}

std::optional<BBox> Scene::portalBBox() const {
  if (!video_) return std::nullopt;
  const auto b = vid().portalBBox();
  if (!b) return std::nullopt;
  const BBox box = toBBox(*b);
  return box.valid() ? std::optional(box) : std::nullopt;
}

std::optional<glm::mat4> Scene::portalFollow() const {
  if (!video_) return std::nullopt;
  const auto m = vid().portalFollow();
  if (!m) return std::nullopt;
  return glm::make_mat4(m->data());
}

GraciaVideoStatus Scene::status() const {
  return video_ ? vid().status() : GRACIA_VIDEO_STATUS_OK;
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
  if (video_) vid().setTransform(glm::value_ptr(m));
  else stat().setTransform(glm::value_ptr(m));
}

void Scene::setTime(double t) {
  if (video_) vid().setTime(t);
}

void Scene::setSpeed(double s) {
  if (video_) vid().setSpeed(s);
}

void Scene::setVolume(float v) {
  if (video_) vid().setVolume(v);
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
    adaptiveBBox_ = bbox_;
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

std::optional<GraciaPose> SceneGroup::viewpoint(GraciaViewpoint display) const {
  for (const auto& s : scenes_)
    if (auto v = s->viewpoint(display)) return v;
  return std::nullopt;
}

std::optional<glm::vec3> SceneGroup::pivot() const {
  return scenes_.size() == 1 ? scenes_[0]->pivot() : std::nullopt;
}

std::optional<BBox> SceneGroup::portalBBox() const {
  return scenes_.size() == 1 ? scenes_[0]->portalBBox() : std::nullopt;
}

std::optional<glm::mat4> SceneGroup::portalFollow() const {
  return scenes_.size() == 1 ? scenes_[0]->portalFollow() : std::nullopt;
}

void SceneGroup::setTransform(const glm::mat4& m) {
  for (auto& s : scenes_) s->setTransform(m);
}

void SceneGroup::setTime(double t) {
  for (auto& s : scenes_) s->setTime(t);
}

void SceneGroup::setSpeed(double s) {
  for (auto& scene : scenes_) scene->setSpeed(s);
}

void SceneGroup::setVolume(float v) {
  for (auto& s : scenes_) s->setVolume(v);
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
