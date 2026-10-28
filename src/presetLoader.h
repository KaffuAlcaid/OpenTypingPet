#pragma once
#include "characterDefinition.h"
namespace PresetLoader {
ImportResult load(const std::wstring &zipPath, const std::wstring &storageRoot);
std::wstring storageRoot();
} // namespace PresetLoader
