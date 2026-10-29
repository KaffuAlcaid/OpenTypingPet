#pragma once
#include "characterDefinition.h"
#include "characterManifest.h"
namespace PresetLoader {
ImportResult load(const std::wstring &zipPath, const std::wstring &storageRoot);
std::wstring storageRoot();
CharacterResources readResources(const std::wstring &zipPath);
void writePackage(const std::wstring &zipPath, const CharacterResources &resources);
CharacterResources exportResources(const CharacterDefinition &character);
} // namespace PresetLoader
