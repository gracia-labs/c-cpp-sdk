#pragma once

#include <volk.h>

#include <algorithm>
#include <cmath>
#include <optional>
#include <vector>

// Column ratios for split view plus divider hit-testing / drag maths, ported
// from the Apple SDK's SplitLayout. Split shows one scene per scissored column,
// an A/B wipe sharing one camera.
class SplitLayout {
 public:
  static constexpr float kDividerHitZone = 8.0f;  // px, half-width of grab band

  int paneCount() const { return (int)ratios_.size(); }
  bool active() const { return ratios_.size() > 1; }

  void reset(int count) {
    ratios_.assign(count > 0 ? count : 0, count > 0 ? 1.0f / count : 0.0f);
  }

  std::vector<float> dividerPositions(float width) const {
    std::vector<float> xs;
    if (!active()) return xs;
    float x = 0;
    for (size_t i = 0; i + 1 < ratios_.size(); ++i) {
      x += ratios_[i] * width;
      xs.push_back(x);
    }
    return xs;
  }

  std::optional<int> dividerIndex(float x, float width) const {
    const auto xs = dividerPositions(width);
    for (size_t i = 0; i < xs.size(); ++i)
      if (std::abs(x - xs[i]) < kDividerHitZone) return (int)i;
    return std::nullopt;
  }

  void drag(int i, float dx, float width) {
    if (width <= 0 || i < 0 || i + 1 >= (int)ratios_.size()) return;
    const float total = ratios_[i] + ratios_[i + 1];
    const float moved = (ratios_[i] * width + dx) / width;
    ratios_[i] = std::min(std::max(kMinRatio, moved), total - kMinRatio);
    ratios_[i + 1] = total - ratios_[i];
  }

  // One full-height column per pane; the last absorbs rounding.
  std::vector<VkRect2D> scissorRects(uint32_t width, uint32_t height) const {
    std::vector<VkRect2D> rects;
    if (ratios_.empty() || width == 0 || height == 0) return rects;
    int x = 0;
    for (size_t i = 0; i < ratios_.size(); ++i) {
      const int remaining = (int)width - x;
      const int w = (i == ratios_.size() - 1)
                        ? remaining
                        : std::min((int)std::lround(width * ratios_[i]), remaining);
      rects.push_back({{x, 0}, {(uint32_t)std::max(0, w), height}});
      x += std::max(0, w);
    }
    return rects;
  }

 private:
  static constexpr float kMinRatio = 0.05f;
  std::vector<float> ratios_;
};
