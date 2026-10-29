#pragma once
#include "characterManifest.h"
#include "nativeDialogs.h"
#include <set>

struct CharacterEditResult {
  CharacterResources resources;
  std::set<std::string> disabledRules;
};
std::optional<CharacterEditResult> editCharacterWindow(
    HWND owner, const ThemeColors &colors, std::shared_ptr<CharacterDefinition> character,
    const std::set<std::string> &disabledRules, const std::function<void(bool)> &captureState);
std::optional<InputRule> editInputRule(HWND owner, const ThemeColors &colors,
                                     const CharacterDefinition &character, const InputRule *rule,
                                     const std::string &defaultAction, const std::function<void(bool)> &captureState);
