#pragma once
#include "characterDefinition.h"
#include <algorithm>
#include <cmath>

inline void paintAnimationFrame(Gdiplus::Graphics &graphics, Gdiplus::Rect rect,
                                 const AnimationClip &clip, size_t frame, bool smooth) {
  if (clip.frames.empty() || rect.Width <= 0 || rect.Height <= 0) return;
  frame = std::min(frame, clip.frames.size() - 1);
  const double scale = std::min(double(rect.Width) / clip.width, double(rect.Height) / clip.height);
  const int width = int(std::round(clip.width * scale)), height = int(std::round(clip.height * scale));
  const Gdiplus::Rect target(rect.X + (rect.Width - width) / 2, rect.Y + (rect.Height - height) / 2, width, height);
  graphics.SetInterpolationMode(smooth ? Gdiplus::InterpolationModeHighQualityBicubic : Gdiplus::InterpolationModeNearestNeighbor);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::ImageAttributes attributes; attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
  graphics.DrawImage(clip.frames[frame].image.get(), target, 0, 0, clip.width, clip.height, Gdiplus::UnitPixel, &attributes);
}
