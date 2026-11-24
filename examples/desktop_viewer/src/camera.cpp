#include "camera.hpp"

#include <glm/gtc/type_ptr.hpp>

#include <cmath>

namespace {

GraciaBBox toGraciaBBox(const BBox& b) {
  if (!b.valid()) return {};
  return {{b.lo.x, b.lo.y, b.lo.z, b.hi.x, b.hi.y, b.hi.z}};
}

// GLFW names the letter keys by their ASCII codes.
uint32_t keyMaskFor(int glfwKey) {
  switch (glfwKey) {
    case 'W': return GRACIA_CAMERA_KEY_W;
    case 'A': return GRACIA_CAMERA_KEY_A;
    case 'S': return GRACIA_CAMERA_KEY_S;
    case 'D': return GRACIA_CAMERA_KEY_D;
    case 'R': return GRACIA_CAMERA_KEY_R;
    case 'F': return GRACIA_CAMERA_KEY_F;
    case 'Q': return GRACIA_CAMERA_KEY_Q;
    case 'E': return GRACIA_CAMERA_KEY_E;
    default: return 0;
  }
}

}  // namespace

void SceneCamera::setBBox(const BBox& full, const BBox& adaptive,
                          std::optional<glm::vec3> pivot, bool reposition) {
  std::optional<std::array<float, 3>> p;
  if (pivot) p = {pivot->x, pivot->y, pivot->z};
  cam_.frame(toGraciaBBox(full), adaptive.valid() ? std::optional(toGraciaBBox(adaptive)) : std::nullopt,
             p, reposition);
}

void SceneCamera::place(const glm::vec3& eye, const glm::quat& orientation) {
  const glm::quat q = glm::normalize(orientation);
  cam_.place({{eye.x, eye.y, eye.z}, {q.x, q.y, q.z, q.w}});
  cam_.reset();
}

void SceneCamera::follow(const glm::mat4& move) { cam_.follow(glm::value_ptr(move)); }

RenderView SceneCamera::renderView(float aspect, float viewHeightPixels) {
  const GraciaCameraView v = cam_.view(aspect, viewHeightPixels);
  return {glm::make_mat4(v.projection), glm::make_mat4(v.cameraToWorld)};
}

glm::mat4 SceneCamera::subjectProjection() {
  return glm::make_mat4(cam_.view().subjectProjection);
}

void CameraInput::pointerDown(glm::vec2 p, bool secondary, bool option) {
  lastPoint_ = p;
  dragIsPan_ = secondary || option;
}

void CameraInput::pointerDragged(glm::vec2 p) {
  if (!lastPoint_) return;
  const glm::vec2 d = p - *lastPoint_;
  lastPoint_ = p;
  dragIsPan_ ? cam_.addPan(d.x, d.y) : cam_.addRotation(d.x, d.y);
}

void CameraInput::pointerUp() {
  lastPoint_ = std::nullopt;
  dragIsPan_ = false;
}

void CameraInput::scroll(float deltaY, bool precise) { cam_.addScroll(deltaY, precise); }

bool CameraInput::keyDown(int glfwKey) {
  const uint32_t bit = keyMaskFor(glfwKey);
  if (!bit) return false;
  keys_ |= bit;
  cam_.setKeys(keys_);
  return true;
}

bool CameraInput::keyUp(int glfwKey) {
  const uint32_t bit = keyMaskFor(glfwKey);
  if (!bit) return false;
  keys_ &= ~bit;
  cam_.setKeys(keys_);
  return true;
}

void CameraInput::setShift(bool down) {
  if (down) keys_ |= GRACIA_CAMERA_KEY_SHIFT;
  else keys_ &= ~GRACIA_CAMERA_KEY_SHIFT;
  cam_.setKeys(keys_);
}

void CameraInput::releaseAllKeys() {
  keys_ = 0;
  cam_.setKeys(0);
}
