#pragma once
#include "appSettings.h"
#include "characterDefinition.h"
#include <filesystem>
#include <map>
#include <set>

struct PetPlacement {
  std::wstring monitor;
  double x = 1, y = 1;
  int edgeX = -1, edgeY = -1, gapX = 24, gapY = 24;
  bool valid = false;
};
struct CharacterEntry {
  std::string libraryId, packageId, sourceHash, currentHash;
  std::wstring name, sourcePath;
  uint64_t bytes = 0;
  unsigned actionCount = 0;
  PetSettings defaults;
  std::set<std::string> disabledRules;
  std::shared_ptr<Gdiplus::Bitmap> thumbnail;
};
struct PetRecord {
  std::string instanceId, libraryId;
  PetSettings settings;
  PetPlacement placement;
  bool paused = false, visible = true, inheritDefaults = true;
  std::vector<InputRule> ruleOverrides;
  std::set<std::string> disabledRules;
};
class CharacterLibrary {
public:
  explicit CharacterLibrary(std::filesystem::path root);
  bool load(std::wstring &message);
  void save() const;
  const std::filesystem::path &root() const { return m_root; }
  std::vector<CharacterEntry> entries;
  std::vector<PetRecord> instances;
  bool allPaused = false;
  CharacterEntry *find(const std::string &id);
  const CharacterEntry *find(const std::string &id) const;
  CharacterEntry &add(const std::shared_ptr<CharacterDefinition> &character,
                      const std::wstring &source, const PetSettings &defaults,
                      const std::string &replaceId = {}, bool edited = false);
  void remove(const std::string &id);
  ImportResult open(const CharacterEntry &entry) const;
  std::wstring packagePath(const std::string &hash) const;
  std::shared_ptr<Gdiplus::Bitmap> thumbnail(CharacterEntry &entry);
private:
  std::filesystem::path m_root;
  bool m_canSave = true;
  void writeThumbnail(CharacterEntry &entry, const CharacterDefinition &character);
};
