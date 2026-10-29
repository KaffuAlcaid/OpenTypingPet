#pragma once
#include "characterDefinition.h"
#include <map>
#include <nlohmann/json_fwd.hpp>

using CharacterResources = std::map<std::string, std::vector<uint8_t>>;
std::shared_ptr<CharacterDefinition> loadCharacterManifest(
    const CharacterResources &resources, const std::string &manifestPath);
nlohmann::json characterManifest(const CharacterDefinition &character);
std::string animationResourceName(const AnimationClip &clip);
int inputKeyCode(const std::string &name);
std::string inputKeyName(int key);
