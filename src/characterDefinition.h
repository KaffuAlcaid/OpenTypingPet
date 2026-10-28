#pragma once
#include <windows.h>

#include <objidl.h>

#include <gdiplus.h>

#include <cstdint>
#include <algorithm>
#include <limits>
#include <memory>
#include <optional>
#include <string>
#include <vector>
struct AnimationFrame {
  std::shared_ptr<Gdiplus::Bitmap> image;
  unsigned delay = 200;
};
struct AnimationClip {
  std::wstring label, format;
  unsigned width = 0, height = 0;
  int loops = -1;
  std::vector<AnimationFrame> frames;
};
enum class InputEventType { KeyDown, KeyUp, MouseDown, MouseUp, Typing };
struct InputEvent {
  InputEventType type = InputEventType::KeyDown;
  int key = 0;
  bool shift = false, ctrl = false, alt = false, repeated = false;
  bool modifierTap = false;
};
struct ActionDefinition {
  std::string id;
  std::wstring label;
  size_t clip = 0;
  std::optional<int> loops;
  unsigned minimumHold = 0;
  bool interruptible = true;
  std::string nextAction;
};
enum class RepeatBehavior { Restart, Continue, Ignore };
struct InputRule {
  std::string id;
  InputEventType event = InputEventType::KeyDown;
  int key = 0;
  bool shift = false, ctrl = false, alt = false;
  bool modifierTap = false;
  std::vector<std::string> actions;
  int priority = 0;
  RepeatBehavior repeat = RepeatBehavior::Ignore;
  std::wstring label;
};
struct CharacterDefinition {
  std::wstring name, archivePath;
  std::string id, contentHash, defaultAction;
  unsigned formatVersion = 1;
  unsigned width = 500, height = 500;
  std::vector<AnimationClip> clips;
  std::vector<ActionDefinition> actions;
  std::vector<InputRule> rules;
  static constexpr size_t noAction = std::numeric_limits<size_t>::max();
  size_t actionIndex(const std::string &actionId) const {
    const auto found = std::find_if(actions.begin(), actions.end(),
                                    [&](const auto &action) { return action.id == actionId; });
    return found == actions.end() ? noAction : size_t(found - actions.begin());
  }
  const AnimationClip &animation(size_t action) const { return clips[actions[action].clip]; }
  int loopCount(size_t action) const { return actions[action].loops.value_or(animation(action).loops); }
};
struct ImportResult {
  std::shared_ptr<CharacterDefinition> character;
  std::wstring error;
};
