#pragma once
#include "characterDefinition.h"
#include <map>

using CharacterResources = std::map<std::string, std::vector<uint8_t>>;
std::shared_ptr<CharacterDefinition> loadCharacterManifest(
    const CharacterResources &resources, const std::string &manifestPath);
