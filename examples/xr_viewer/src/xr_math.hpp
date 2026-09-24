#pragma once

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <openxr/openxr.h>

#include <cmath>

#include <gracia_demo/math3d.hpp>

// Reverse-Z projection from an asymmetric XrFovf, in the layout
// SceneCamera::makeProjection produces. The fov values are angles, not tangents.
inline glm::mat4 xrProjection(const XrFovf& fov, float nearZ, float farZ,
                              bool reverseZ = true) {
  const float tanL = std::tan(fov.angleLeft);
  const float tanR = std::tan(fov.angleRight);
  const float tanU = std::tan(fov.angleUp);
  const float tanD = std::tan(fov.angleDown);

  const float w = tanR - tanL;
  const float h = tanD - tanU;  // negative: the Vulkan clip-space Y flip

  const float n = reverseZ ? farZ : nearZ;
  const float f = reverseZ ? nearZ : farZ;

  glm::mat4 p(0.0f);
  p[0][0] = 2.0f / w;
  p[1][1] = 2.0f / h;
  p[2][0] = (tanR + tanL) / w;
  p[2][1] = (tanU + tanD) / h;
  p[2][2] = f / (n - f);
  p[2][3] = -1.0f;
  p[3][2] = (f * n) / (n - f);
  return p;
}

// XrPosef to camera-to-world. Same convention as the demo camera, so no basis
// change. Do not invert: the SDK does that itself.
inline glm::mat4 xrPose(const XrPosef& pose) {
  // XrQuaternionf is {x,y,z,w}; glm::quat takes (w,x,y,z). Feeding it in XR order
  // compiles cleanly and is wrong.
  const glm::quat q(pose.orientation.w, pose.orientation.x, pose.orientation.y,
                    pose.orientation.z);
  glm::mat4 m = glm::mat4_cast(glm::normalize(q));
  m[3] = glm::vec4(pose.position.x, pose.position.y, pose.position.z, 1.0f);
  return m;
}

// The user stands at the origin, so move the scene in front of them. Feed this
// to setTransform; locomotionTransform moves the user's rig instead.
inline glm::mat4 placeSceneForXr(const BBox& bbox, float targetRadiusMeters,
                                 float distanceMeters, float heightMeters) {
  if (!bbox.valid()) return glm::mat4(1.0f);

  float r = bbox.radius();
  if (!std::isfinite(r) || r <= 0.0f) r = 12.0f;
  const float s = targetRadiusMeters / r;

  glm::mat4 m(1.0f);
  m = glm::translate(m, glm::vec3(0.0f, heightMeters, -distanceMeters));
  m = glm::scale(m, glm::vec3(s));
  m = glm::translate(m, -bbox.center());
  return m;
}
