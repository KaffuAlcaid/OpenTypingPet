#include "animationCache.h"
#include "animationDecoder.h"
#include "storageUtils.h"
#include <map>
#include <mutex>

namespace {
std::mutex cacheMutex;
std::map<std::string, std::weak_ptr<const AnimationClip>> cache;
uint64_t imageBytes(const AnimationClip &clip) { return uint64_t(clip.width) * clip.height * 4 * clip.frames.size(); }
uint64_t prune() {
  uint64_t total = 0;
  for (auto i = cache.begin(); i != cache.end();) {
    if (const auto clip = i->second.lock()) { total += imageBytes(*clip); ++i; }
    else i = cache.erase(i);
  }
  return total;
}
}
AnimationClip cachedAnimation(const std::vector<uint8_t> &bytes, const std::wstring &label, uint64_t &budget) {
  const auto key = Storage::hash(bytes);
  std::lock_guard lock(cacheMutex);
  auto stored = cache[key].lock();
  if (!stored) {
    uint64_t decoded = 0;
    auto result = std::make_shared<AnimationClip>(decodeAnimation(bytes, label, decoded));
    if (prune() + decoded > 256 * 1024 * 1024)
      throw std::wstring(L"动画缓存超过 256 MiB，请先关闭部分桌宠或减小素材");
    stored = std::move(result);
    cache[key] = stored;
  }
  budget += imageBytes(*stored);
  if (budget > 128 * 1024 * 1024) throw std::wstring(L"角色解码后的图像总量超过 128 MiB");
  auto result = *stored;
  result.label = label;
  result.storage = std::move(stored);
  return result;
}
uint64_t cachedAnimationBytes() { std::lock_guard lock(cacheMutex); return prune(); }
