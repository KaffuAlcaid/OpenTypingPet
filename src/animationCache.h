#pragma once
#include "characterDefinition.h"

AnimationClip cachedAnimation(const std::vector<uint8_t> &bytes, const std::wstring &label, uint64_t &budget);
uint64_t cachedAnimationBytes();
