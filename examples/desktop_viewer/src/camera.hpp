#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <gracia/SDK.hpp>
#include <gracia_demo/math3d.hpp>

#include <cstdint>
#include <optional>

struct RenderView {
  glm::mat4 projection{1.0f};
  glm::mat4 cameraToWorld{1.0f};
};

// Trackball orbit + fly camera backed by `gracia::Camera` from SDK.hpp.
class SceneCamera {
 public:
  SceneCamera() : cam_({60.0f, 1.0f, 1.0f, /*reverseZ=*/true, /*flipY=*/true}) {}

  void setReverseZ(bool r) {
    auto cfg = cam_.config();
    cfg.reverseZ = r;
    cam_.setConfig(cfg);
  }

  bool freeLook() const { return cam_.config().freeLook; }
  void setFreeLook(bool on) {
    auto cfg = cam_.config();
    cfg.freeLook = on;
    cam_.setConfig(cfg);
  }

  void setBBox(const BBox& full, const BBox& adaptive,
               std::optional<glm::vec3> pivot, bool reposition = true);
  void place(const glm::vec3& eye, const glm::quat& orientation);
  void follow(const glm::mat4& move);
  void reset() { cam_.reset(); }
  bool isFramed() { return cam_.view().framed; }

  bool update(float dt) { return cam_.update(dt); }
  void zoom(float factor) { cam_.zoom(factor); }
  void setKeys(uint32_t mask) { cam_.setKeys(mask); }
  void addRotation(float dx, float dy) { cam_.input({.rotate = {dx, dy}}); }
  void addPan(float dx, float dy) { cam_.input({.pan = {dx, dy}}); }
  void addScroll(float deltaY, bool precise) {
    cam_.input({.scroll = deltaY, .scrollPrecise = precise});
  }
  RenderView renderView(float aspect, float viewHeightPixels);
  glm::mat4 subjectProjection();

 private:
  gracia::Camera cam_;
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
  uint32_t keys_ = 0;
};
