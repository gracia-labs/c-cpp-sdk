#pragma once

#include <glm/glm.hpp>

#include <cmath>
#include <limits>

// World-space AABB; an invalid/inverted box contributes nothing to a union.
struct BBox {
  glm::vec3 lo{std::numeric_limits<float>::max()};
  glm::vec3 hi{-std::numeric_limits<float>::max()};

  glm::vec3 center() const { return (lo + hi) * 0.5f; }
  glm::vec3 size() const { return hi - lo; }
  float radius() const { return glm::length(size()) * 0.5f; }

  bool valid() const {
    return lo.x <= hi.x && lo.y <= hi.y && lo.z <= hi.z && std::isfinite(lo.x) &&
           std::isfinite(lo.y) && std::isfinite(lo.z) && std::isfinite(hi.x) &&
           std::isfinite(hi.y) && std::isfinite(hi.z);
  }

  BBox unite(const BBox& o) const {
    if (!valid()) return o;
    if (!o.valid()) return *this;
    return {glm::min(lo, o.lo), glm::max(hi, o.hi)};
  }

  static BBox fromMinMax(const float m[6]) {
    return {glm::vec3(m[0], m[1], m[2]), glm::vec3(m[3], m[4], m[5])};
  }
};
