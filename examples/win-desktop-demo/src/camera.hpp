#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gracia_demo/math3d.hpp>

#include <optional>

struct RenderView {
  glm::mat4 projection{1.0f};
  glm::mat4 cameraToWorld{1.0f};
};

enum class MoveKey { W, A, S, D, R, F, Q, E, Shift };

struct GestureDelta {
  float rotX = 0, rotY = 0, panX = 0, panY = 0, zoom = 0;
  bool settled() const;
  GestureDelta& operator+=(const GestureDelta& r);
  GestureDelta& operator-=(const GestureDelta& r);
  GestureDelta operator*(float s) const;
};

// Accumulates raw input, drained on the render clock with critical damping (a
// short inertial tail after release).
class GestureInput {
 public:
  float height = 1.0f;  // view height in pixels; normalizes drag feel
  void addRotation(float dx, float dy);
  void addPan(float dx, float dy);
  void addZoom(float z);
  void addScroll(float deltaY, bool precise);
  void reset() { pending_ = {}; }
  std::optional<GestureDelta> drain(float dt);

 private:
  GestureDelta accumulated_{};
  GestureDelta pending_{};
};

// Trackball orbit + fly camera with inertia, ported from the Apple SDK's
// GraciaCamera. No world-up: rotation is about the camera's own axes, so it
// rolls. Produces a camera-to-world transform and a reverse-Z Vulkan projection.
class SceneCamera {
 public:
  SceneCamera() { apply(); }

  void setReverseZ(bool r) { reverseZ_ = r; }

  // `adaptive` is what the camera frames/zooms at (strays don't shrink scale);
  // `full` bounds the far plane. An invalid box un-frames; reposition=false keeps
  // the current pose (spawn).
  void setBBox(const BBox& full, const BBox& adaptive, bool reposition = true);
  void reset();
  bool isFramed() const { return framed_; }

  bool update(float dt);          // advance inertia + keys; true if moved
  void zoom(float factor);        // multiplicative; factor > 1 moves closer
  void setKeys(unsigned mask) { keys_ = mask; }
  RenderView renderView(float aspect, float viewHeightPixels);
  // Projection with near/far tight to the subject, for linearized depth-as-color.
  glm::mat4 subjectProjection() const;
  GestureInput& input() { return input_; }

 private:
  glm::vec3 right() const { return orient_ * glm::vec3(1, 0, 0); }
  glm::vec3 up() const { return orient_ * glm::vec3(0, 1, 0); }
  glm::vec3 back() const { return orient_ * glm::vec3(0, 0, 1); }
  bool flying() const;
  float reach() const;
  float flySpeed() const { return sceneRadius_ / 4.5f * flyTrim_; }

  void frameScene(const BBox& full, const BBox& subject, bool reposition);
  void pose(const glm::vec3& target, const glm::vec3& eye);
  void turn(float dx, float dy);
  void dolly(float amount);
  void pan(float px, float py);
  bool move(float dt);
  void apply();
  void updateProjection();
  glm::mat4 makeProjection(float near, float far) const;

  glm::vec3 eye_{0};
  glm::quat orient_{1, 0, 0, 0};
  unsigned keys_ = 0;

  glm::vec3 sceneCentre_{0};
  float sceneRadius_ = 12.0f;
  float fullRadius_ = 12.0f;
  float flyTrim_ = 1.0f;
  bool framed_ = false;
  std::optional<std::pair<glm::vec3, glm::vec3>> framing_;  // (target, eye)

  float fov_ = 60.0f, aspect_ = 1.0f, near_ = 0.05f, far_ = 10000.0f;
  bool reverseZ_ = true;
  glm::mat4 projection_{1.0f};

  GestureInput input_;
};

// Turns pointer / scroll / key events into camera motion. Pointer in pixels, y down.
class CameraInput {
 public:
  explicit CameraInput(SceneCamera& cam) : cam_(cam) {}

  void pointerDown(glm::vec2 p, bool secondary, bool option);
  void pointerDragged(glm::vec2 p);
  void pointerUp();
  void scroll(float deltaY, bool precise);

  bool keyDown(int glfwKey);  // true if consumed as a movement key
  bool keyUp(int glfwKey);
  void setShift(bool down);
  void releaseAllKeys();

 private:
  SceneCamera& cam_;
  std::optional<glm::vec2> lastPoint_;
  bool dragIsPan_ = false;
  unsigned keys_ = 0;
};
