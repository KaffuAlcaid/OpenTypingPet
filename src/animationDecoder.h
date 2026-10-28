#pragma once
#include "characterDefinition.h"

AnimationClip decodeAnimation(const std::vector<uint8_t> &bytes,
                              const std::wstring &label, uint64_t &budget);
