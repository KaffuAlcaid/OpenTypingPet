#include "animationDecoder.h"
#include <algorithm>
#include <cstring>
#include <limits>
#include <libplum.h>
#include <shlwapi.h>

namespace {
constexpr uint64_t MiB = 1024 * 1024;
void require(bool condition, const wchar_t *message) {
  if (!condition)
    throw std::wstring(message);
}
bool isApng(const std::vector<uint8_t> &bytes) {
  const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() < 8 || !std::equal(signature, signature + 8, bytes.begin()))
    return false;
  for (size_t offset = 8; offset + 12 <= bytes.size();) {
    const uint32_t size = uint32_t(bytes[offset]) << 24 |
                          uint32_t(bytes[offset + 1]) << 16 |
                          uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
    if (size > bytes.size() - offset - 12)
      break;
    if (std::memcmp(bytes.data() + offset + 4, "acTL", 4) == 0)
      return true;
    offset += size_t(size) + 12;
  }
  return false;
}
template <class T>
T metadata(const plum_image *image, int type, size_t index, T fallback) {
  const auto *node = plum_find_metadata(image, type);
  if (!node || index >= node->size / sizeof(T))
    return fallback;
  T value{};
  std::memcpy(&value, static_cast<const uint8_t *>(node->data) + index * sizeof(T),
              sizeof(T));
  return value;
}
AnimationClip decodeApng(const std::vector<uint8_t> &bytes,
                         const std::wstring &label, uint64_t &budget) {
  require(budget < 128 * MiB, L"预设解码后的图像总量超过 128 MB。");
  unsigned error = 0;
  std::unique_ptr<plum_image, decltype(&plum_destroy_image)> source(
      plum_load_image_limited(bytes.data(), bytes.size(),
                              PLUM_COLOR_32 | PLUM_ALPHA_INVERT,
                              size_t((128 * MiB - budget) / 4), &error),
      plum_destroy_image);
  require(source != nullptr, error == PLUM_ERR_IMAGE_TOO_LARGE
                                 ? L"APNG 解码后的图像总量超过限制。"
                                 : L"APNG 文件无效或已损坏。");
  require(source->type == PLUM_IMAGE_APNG, L"APNG 动画数据无效。");
  require(source->width && source->height && source->width <= 2048 &&
              source->height <= 2048 && source->frames <= 301,
          L"APNG 尺寸或帧数超过限制。");
  AnimationClip clip;
  clip.label = label;
  clip.format = L"APNG";
  clip.width = source->width;
  clip.height = source->height;
  const auto loops = metadata<uint32_t>(source.get(), PLUM_METADATA_LOOP_COUNT, 0, 1);
  clip.loops = loops ? int(std::min(loops, uint32_t(INT_MAX))) : -1;
  const size_t pixels = size_t(clip.width) * clip.height;
  std::vector<uint32_t> canvas(pixels), previous;
  uint8_t previousDisposal = PLUM_DISPOSAL_NONE;
  const auto *data = static_cast<const uint32_t *>(source->data);
  for (uint32_t i = 0; i < source->frames; ++i) {
    const auto duration = metadata<uint64_t>(source.get(), PLUM_METADATA_FRAME_DURATION, i, 0);
    // An APNG poster image is not an animation frame and must not seed the canvas.
    if (!duration) {
      previousDisposal = metadata<uint8_t>(source.get(), PLUM_METADATA_FRAME_DISPOSAL, i, 0);
      continue;
    }
    const auto area = metadata<plum_rectangle>(
        source.get(), PLUM_METADATA_FRAME_AREA, i, {0, 0, clip.width, clip.height});
    require(area.left <= clip.width && area.top <= clip.height &&
                area.width <= clip.width - area.left &&
                area.height <= clip.height - area.top,
            L"APNG 帧区域无效。");
    const auto disposal = metadata<uint8_t>(source.get(), PLUM_METADATA_FRAME_DISPOSAL, i, 0);
    if (disposal % PLUM_DISPOSAL_REPLACE == PLUM_DISPOSAL_PREVIOUS)
      previous = canvas;
    const bool replace = clip.frames.empty() || previousDisposal >= PLUM_DISPOSAL_REPLACE;
    for (uint32_t y = area.top; y < area.top + area.height; ++y)
      for (uint32_t x = area.left; x < area.left + area.width; ++x) {
        const size_t index = size_t(y) * clip.width + x;
        const uint32_t value = data[size_t(i) * pixels + index];
        const unsigned alpha = value >> 24;
        const unsigned red = ((value & 255) * alpha + 127) / 255;
        const unsigned green = (((value >> 8) & 255) * alpha + 127) / 255;
        const unsigned blue = (((value >> 16) & 255) * alpha + 127) / 255;
        uint32_t pixel = alpha << 24 | red << 16 | green << 8 | blue;
        if (!replace && alpha < 255) {
          const uint32_t below = canvas[index];
          const unsigned inverse = 255 - alpha;
          pixel = (alpha + ((below >> 24) * inverse + 127) / 255) << 24 |
                  (red + (((below >> 16) & 255) * inverse + 127) / 255) << 16 |
                  (green + (((below >> 8) & 255) * inverse + 127) / 255) << 8 |
                  (blue + ((below & 255) * inverse + 127) / 255);
        }
        canvas[index] = pixel;
      }
    auto frame = std::make_shared<Gdiplus::Bitmap>(clip.width, clip.height,
                                                  PixelFormat32bppPARGB);
    Gdiplus::BitmapData bitmap{};
    Gdiplus::Rect bounds(0, 0, clip.width, clip.height);
    require(frame->LockBits(&bounds, Gdiplus::ImageLockModeWrite,
                            PixelFormat32bppPARGB, &bitmap) == Gdiplus::Ok,
            L"无法创建 APNG 动画帧。");
    for (unsigned y = 0; y < clip.height; ++y)
      std::memcpy(static_cast<uint8_t *>(bitmap.Scan0) + ptrdiff_t(y) * bitmap.Stride,
                  canvas.data() + size_t(y) * clip.width, size_t(clip.width) * 4);
    frame->UnlockBits(&bitmap);
    clip.frames.push_back({std::move(frame), unsigned(std::clamp<uint64_t>(
                                               duration / 1000000 + (duration % 1000000 >= 500000),
                                               1, 3600000))});
    require(clip.frames.size() <= 300, L"动画超过 300 帧。");
    const auto operation = disposal % PLUM_DISPOSAL_REPLACE;
    if (operation == PLUM_DISPOSAL_BACKGROUND)
      for (uint32_t y = area.top; y < area.top + area.height; ++y)
        std::fill_n(canvas.begin() + size_t(y) * clip.width + area.left, area.width, 0);
    else if (operation == PLUM_DISPOSAL_PREVIOUS)
      canvas.swap(previous);
    previousDisposal = disposal;
  }
  require(!clip.frames.empty(), L"APNG 不包含可播放的动画帧。");
  budget += pixels * 4 * clip.frames.size();
  return clip;
}
} // namespace

AnimationClip decodeAnimation(const std::vector<uint8_t> &bytes,
                              const std::wstring &label, uint64_t &budget) {
  require(!bytes.empty() && bytes.size() <= 15 * MiB, L"图像为空或超过 15 MB。");
  if (isApng(bytes))
    return decodeApng(bytes, label, budget);
  IStream *rawStream = SHCreateMemStream(bytes.data(), UINT(bytes.size()));
  require(rawStream != nullptr, L"无法读取图像资源。");
  std::unique_ptr<IStream, void (*)(IStream *)> stream(rawStream, [](IStream *p) { p->Release(); });
  std::unique_ptr<Gdiplus::Image> source(Gdiplus::Image::FromStream(stream.get(), FALSE));
  require(source && source->GetLastStatus() == Gdiplus::Ok, L"无法解码图像。");
  GUID format{};
  source->GetRawFormat(&format);
  require(format == Gdiplus::ImageFormatPNG || format == Gdiplus::ImageFormatJPEG ||
              format == Gdiplus::ImageFormatGIF, L"仅支持 PNG、APNG、JPEG 和 GIF。");
  AnimationClip clip;
  clip.loops = format == Gdiplus::ImageFormatGIF ? 1 : -1;
  clip.format = format == Gdiplus::ImageFormatGIF ? L"GIF" :
                format == Gdiplus::ImageFormatPNG ? L"PNG" : L"JPEG";
  clip.label = label;
  clip.width = source->GetWidth();
  clip.height = source->GetHeight();
  require(clip.width && clip.height && clip.width <= 2048 && clip.height <= 2048,
          L"图像尺寸无效或超过 2048 像素。");
  const GUID dimension = Gdiplus::FrameDimensionTime;
  const unsigned count = format == Gdiplus::ImageFormatGIF ? std::max(1u, source->GetFrameCount(&dimension)) : 1;
  require(count <= 300, L"动画超过 300 帧。");
  budget += uint64_t(clip.width) * clip.height * 4 * count;
  require(budget <= 128 * MiB, L"预设解码后的图像总量超过 128 MB。");
  std::vector<unsigned> delays(count, 100);
  const unsigned delaySize = source->GetPropertyItemSize(PropertyTagFrameDelay);
  if (delaySize) {
    std::vector<uint8_t> buffer(delaySize);
    auto *property = reinterpret_cast<Gdiplus::PropertyItem *>(buffer.data());
    if (source->GetPropertyItem(PropertyTagFrameDelay, delaySize, property) == Gdiplus::Ok &&
        property->type == PropertyTagTypeLong)
      for (unsigned i = 0; i < count && (i + 1) * sizeof(ULONG) <= property->length; ++i) {
        const auto delay = static_cast<ULONG *>(property->value)[i];
        delays[i] = delay ? std::clamp(delay, 1ul, 360000ul) * 10 : 100;
      }
  }
  const unsigned loopSize = source->GetPropertyItemSize(PropertyTagLoopCount);
  if (loopSize) {
    std::vector<uint8_t> buffer(loopSize);
    auto *property = reinterpret_cast<Gdiplus::PropertyItem *>(buffer.data());
    if (source->GetPropertyItem(PropertyTagLoopCount, loopSize, property) == Gdiplus::Ok && property->length >= 2) {
      const auto loops = *static_cast<USHORT *>(property->value);
      clip.loops = loops ? int(loops) + 1 : -1;
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    if (count > 1)
      require(source->SelectActiveFrame(&dimension, i) == Gdiplus::Ok, L"动画帧无效。");
    auto frame = std::make_shared<Gdiplus::Bitmap>(clip.width, clip.height, PixelFormat32bppPARGB);
    Gdiplus::Graphics painter(frame.get());
    painter.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    require(painter.DrawImage(source.get(), Gdiplus::Rect(0, 0, clip.width, clip.height), 0, 0,
                              clip.width, clip.height, Gdiplus::UnitPixel) == Gdiplus::Ok,
            L"无法读取动画帧。");
    clip.frames.push_back({std::move(frame), delays[i]});
  }
  return clip;
}
