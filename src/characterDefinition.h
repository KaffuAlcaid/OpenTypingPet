#pragma once
#include <windows.h>

#include <objidl.h>

#include <gdiplus.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>
struct AnimationFrame {
  std::shared_ptr<Gdiplus::Bitmap> image;
  unsigned delay = 200;
};
struct AnimationClip {
  std::wstring label;
  unsigned width = 0, height = 0;
  int loops = -1;
  std::vector<AnimationFrame> frames;
};
struct InputBinding {
  int key = 0;
  bool shift = false, ctrl = false, alt = false;
  size_t action = 0;
  std::wstring label;
};
struct CharacterDefinition {
  std::wstring name, archivePath;
  unsigned width = 500, height = 500;
  std::vector<AnimationClip> clips;
  std::vector<InputBinding> bindings;
};
struct ImportResult {
  std::shared_ptr<CharacterDefinition> character;
  std::wstring error;
};
