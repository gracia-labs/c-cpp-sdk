#include "camera.hpp"

#include <glm/gtc/constants.hpp>

#include <GLFW/glfw3.h>

#include <cmath>

namespace {

namespace K {
constexpr float kRotate = 2.5f, kPan = 1.6f, kWheelZoom = 0.0015f;
constexpr float kFlyTrimMin = 1.0f / 64, kFlyTrimMax = 64.0f;
constexpr float kBoost = 4.0f, kRoll = 1.4f;
constexpr float kReachFloor = 0.02f, kDollyFloor = 0.15f, kReachCeiling = 1000.0f;
constexpr float kNearFraction = 0.002f, kMoveDtCap = 1.0f / 20;
constexpr float kTau = 0.022f, kIdle = 1e-4f, kDefaultRadius = 12.0f;
}  // namespace K

constexpr unsigned bit(MoveKey k) { return 1u << static_cast<int>(k); }
constexpr unsigned kMoveMask = bit(MoveKey::W) | bit(MoveKey::A) | bit(MoveKey::S) |
                               bit(MoveKey::D) | bit(MoveKey::R) | bit(MoveKey::F) |
                               bit(MoveKey::Q) | bit(MoveKey::E);

glm::vec3 nrm(glm::vec3 v, glm::vec3 fallback) {
  float l = glm::length(v);
  return (std::isfinite(l) && l > 1e-6f) ? v / l : fallback;
}

float perspectiveScale(float distance, float fovYDeg, float pixels) {
  return 2.0f * distance * std::tan(fovYDeg * glm::pi<float>() / 360.0f) /
         glm::max(1.0f, pixels);
}

std::pair<float, float> yawPitch(glm::vec3 fwd) {
  return {std::atan2(fwd.x, -fwd.z), std::asin(glm::clamp(fwd.y, -1.0f, 1.0f))};
}

std::optional<MoveKey> moveKeyFor(int glfwKey) {
  switch (glfwKey) {
    case GLFW_KEY_W: return MoveKey::W;
    case GLFW_KEY_A: return MoveKey::A;
    case GLFW_KEY_S: return MoveKey::S;
    case GLFW_KEY_D: return MoveKey::D;
    case GLFW_KEY_R: return MoveKey::R;
    case GLFW_KEY_F: return MoveKey::F;
    case GLFW_KEY_Q: return MoveKey::Q;
    case GLFW_KEY_E: return MoveKey::E;
    default: return std::nullopt;
  }
}

}  // namespace

// --- GestureDelta / GestureInput --------------------------------------------

bool GestureDelta::settled() const {
  return std::abs(rotX) + std::abs(rotY) + std::abs(panX) + std::abs(panY) +
             std::abs(zoom) <
         K::kIdle;
}
GestureDelta& GestureDelta::operator+=(const GestureDelta& r) {
  rotX += r.rotX; rotY += r.rotY; panX += r.panX; panY += r.panY; zoom += r.zoom;
  return *this;
}
GestureDelta& GestureDelta::operator-=(const GestureDelta& r) {
  rotX -= r.rotX; rotY -= r.rotY; panX -= r.panX; panY -= r.panY; zoom -= r.zoom;
  return *this;
}
GestureDelta GestureDelta::operator*(float s) const {
  return {rotX * s, rotY * s, panX * s, panY * s, zoom * s};
}

void GestureInput::addRotation(float dx, float dy) {
  const float h = glm::max(height, 1.0f);
  accumulated_.rotX += (dx / h) * K::kRotate;
  accumulated_.rotY += (dy / h) * K::kRotate;
}
void GestureInput::addPan(float dx, float dy) {
  accumulated_.panX += dx;
  accumulated_.panY += dy;
}
void GestureInput::addZoom(float z) { accumulated_.zoom += z; }
void GestureInput::addScroll(float deltaY, bool precise) {
  addZoom(deltaY * (precise ? 1.0f : 100.0f) * K::kWheelZoom);
}

std::optional<GestureDelta> GestureInput::drain(float dt) {
  pending_ += accumulated_;
  accumulated_ = {};
  if (pending_.settled()) return std::nullopt;
  const GestureDelta out = pending_ * (1.0f - std::exp(-dt / K::kTau));
  pending_ -= out;
  return out;
}

// --- SceneCamera ------------------------------------------------------------

bool SceneCamera::flying() const { return (keys_ & kMoveMask) != 0; }

float SceneCamera::reach() const {
  return glm::clamp(glm::distance(eye_, sceneCentre_), sceneRadius_ * K::kReachFloor,
                    sceneRadius_ * K::kReachCeiling);
}

void SceneCamera::setBBox(const BBox& full, const BBox& adaptive, bool reposition) {
  if (!full.valid()) {
    framed_ = false;
    return;
  }
  frameScene(full, adaptive.valid() ? adaptive : full, reposition);
}

void SceneCamera::frameScene(const BBox& full, const BBox& subject, bool reposition) {
  sceneCentre_ = subject.center();
  const float r = subject.radius();
  sceneRadius_ = (std::isfinite(r) && r > 0) ? r : K::kDefaultRadius;
  fullRadius_ = glm::max(full.radius(), 1e-4f);
  flyTrim_ = 1.0f;
  framed_ = true;
  if (!reposition) return;

  const float halfFovV = fov_ * glm::pi<float>() / 360.0f;
  const float halfFov =
      glm::min(halfFovV, std::atan(std::tan(halfFovV) * glm::max(aspect_, 1e-3f)));
  const float dist = subject.radius() / std::sin(halfFov) * 1.05f;
  framing_ = {subject.center(), subject.center() + glm::vec3(0, 0, 1) * glm::max(dist, 1e-3f)};
  reset();
}

void SceneCamera::reset() {
  if (!framing_) return;
  pose(framing_->first, framing_->second);
  framed_ = true;
}

void SceneCamera::pose(const glm::vec3& target, const glm::vec3& eye) {
  eye_ = eye;
  auto [yaw, pitch] = yawPitch(nrm(target - eye, glm::vec3(0, 0, -1)));
  orient_ = glm::angleAxis(-yaw, glm::vec3(0, 1, 0)) *
            glm::angleAxis(pitch, glm::vec3(1, 0, 0));
  input_.reset();
  apply();
}

// Trackball about the scene centre using the camera's own axes (preserves
// |eye - centre|, avoids depending on the asset's up).
void SceneCamera::turn(float dx, float dy) {
  if (dx == 0 && dy == 0) return;
  const glm::quat rotation =
      glm::normalize(glm::angleAxis(-dy, right()) * glm::angleAxis(-dx, up()));
  orient_ = glm::normalize(rotation * orient_);
  eye_ = sceneCentre_ + rotation * (eye_ - sceneCentre_);
}

void SceneCamera::dolly(float amount) {
  eye_ -= back() * (amount * glm::max(reach(), sceneRadius_ * K::kDollyFloor));
}

void SceneCamera::pan(float px, float py) {
  const float s = perspectiveScale(reach(), fov_, input_.height) * K::kPan;
  eye_ += right() * (-px * s) + up() * (py * s);
}

bool SceneCamera::move(float dt) {
  if (keys_ == 0) return false;
  const float step = glm::min(dt, K::kMoveDtCap) *
                     ((keys_ & bit(MoveKey::Shift)) ? K::kBoost : 1.0f);
  bool moved = false;

  float roll = 0;
  if (keys_ & bit(MoveKey::Q)) roll -= K::kRoll * step;
  if (keys_ & bit(MoveKey::E)) roll += K::kRoll * step;
  if (roll != 0) {
    orient_ = glm::normalize(orient_ * glm::angleAxis(roll, glm::vec3(0, 0, 1)));
    moved = true;
  }

  glm::vec3 v(0);
  if (keys_ & bit(MoveKey::W)) v -= back();
  if (keys_ & bit(MoveKey::S)) v += back();
  if (keys_ & bit(MoveKey::D)) v += right();
  if (keys_ & bit(MoveKey::A)) v -= right();
  if (keys_ & bit(MoveKey::R)) v += up();
  if (keys_ & bit(MoveKey::F)) v -= up();
  if (v != glm::vec3(0)) {
    eye_ += glm::normalize(v) * (step * flySpeed());
    moved = true;
  }
  return moved;
}

bool SceneCamera::update(float dt) {
  bool moved = move(dt);
  if (auto o = input_.drain(dt)) {
    turn(o->rotX, o->rotY);
    if (o->zoom != 0) {
      // While flying the wheel trims fly speed instead of dollying (as in Unity).
      if (flying())
        flyTrim_ = glm::clamp(flyTrim_ * std::exp(o->zoom), K::kFlyTrimMin, K::kFlyTrimMax);
      else
        dolly(o->zoom);
    }
    if (o->panX != 0 || o->panY != 0) pan(o->panX, o->panY);
    moved = true;
  }
  if (moved) apply();
  return moved;
}

void SceneCamera::zoom(float factor) {
  if (factor > 0) input_.addZoom(std::log(factor));
}

// Depth range tracks the eye, so a camera flown far out doesn't clip the scene.
void SceneCamera::apply() {
  near_ = glm::max(0.01f, reach() * K::kNearFraction);
  far_ = glm::max(reach() + fullRadius_ * 4.0f, near_ * 1000.0f);
}

glm::mat4 SceneCamera::makeProjection(float near, float far) const {
  const float t = std::tan(fov_ * glm::pi<float>() / 360.0f);
  const float n = reverseZ_ ? far : near;
  const float f = reverseZ_ ? near : far;
  glm::mat4 p(0.0f);
  p[0][0] = 1.0f / (glm::max(aspect_, 1e-6f) * t);
  p[1][1] = -1.0f / t;  // Vulkan clip-space Y flip
  p[2][2] = f / (n - f);
  p[2][3] = -1.0f;
  p[3][2] = (f * n) / (n - f);
  return p;
}

void SceneCamera::updateProjection() { projection_ = makeProjection(near_, far_); }

// Reverse-Z projection with near/far tight to the subject, so linear depth fills 0..1.
glm::mat4 SceneCamera::subjectProjection() const {
  const float d = glm::dot(back(), eye_ - sceneCentre_);  // eye-to-centre along view axis
  const float near = glm::max(0.01f, d - sceneRadius_);
  const float far = glm::max(near * 1.0001f, d + sceneRadius_);
  return makeProjection(near, far);
}

RenderView SceneCamera::renderView(float aspect, float viewHeightPixels) {
  if (aspect > 0 && std::isfinite(aspect)) aspect_ = aspect;
  input_.height = glm::max(viewHeightPixels, 1.0f);
  updateProjection();
  glm::mat4 c2w(1.0f);
  c2w[0] = glm::vec4(right(), 0);
  c2w[1] = glm::vec4(up(), 0);
  c2w[2] = glm::vec4(back(), 0);
  c2w[3] = glm::vec4(eye_, 1);
  return {projection_, c2w};
}

// --- CameraInput ------------------------------------------------------------

void CameraInput::pointerDown(glm::vec2 p, bool secondary, bool option) {
  lastPoint_ = p;
  dragIsPan_ = secondary || option;
}

void CameraInput::pointerDragged(glm::vec2 p) {
  if (!lastPoint_) return;
  const glm::vec2 d = p - *lastPoint_;
  lastPoint_ = p;
  dragIsPan_ ? cam_.input().addPan(d.x, d.y) : cam_.input().addRotation(d.x, d.y);
}

void CameraInput::pointerUp() {
  lastPoint_ = std::nullopt;
  dragIsPan_ = false;
}

void CameraInput::scroll(float deltaY, bool precise) {
  cam_.input().addScroll(deltaY, precise);
}

bool CameraInput::keyDown(int glfwKey) {
  auto k = moveKeyFor(glfwKey);
  if (!k) return false;
  keys_ |= bit(*k);
  cam_.setKeys(keys_);
  return true;
}

bool CameraInput::keyUp(int glfwKey) {
  auto k = moveKeyFor(glfwKey);
  if (!k) return false;
  keys_ &= ~bit(*k);
  cam_.setKeys(keys_);
  return true;
}

void CameraInput::setShift(bool down) {
  if (down) keys_ |= bit(MoveKey::Shift);
  else keys_ &= ~bit(MoveKey::Shift);
  cam_.setKeys(keys_);
}

void CameraInput::releaseAllKeys() {
  keys_ = 0;
  cam_.setKeys(0);
}
