#include "characterLibrary.h"
#include "presetLoader.h"
#include "storageUtils.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <fstream>

namespace {
using Json = nlohmann::json;
bool validHash(const std::string &value) {
  return value.size() == 64 && std::all_of(value.begin(), value.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
Json settingsJson(const PetSettings &value) {
  return {{"width", value.width}, {"opacity", value.opacity}, {"idleDelay", value.idleDelay},
          {"minimumHold", value.minimumHold}, {"topmost", value.topmost}, {"locked", value.locked},
          {"smooth", value.smooth}, {"snap", value.snap}};
}
PetSettings readSettings(const Json &value) {
  PetSettings result;
  result.width = std::clamp(value.value("width", 280), 100, 800);
  result.opacity = std::clamp(value.value("opacity", 100), 20, 100);
  result.idleDelay = std::clamp(value.value("idleDelay", 800), 200, 5000);
  result.minimumHold = std::clamp(value.value("minimumHold", 0), 0, 500);
  result.topmost = value.value("topmost", true);
  result.locked = value.value("locked", false);
  result.smooth = value.value("smooth", false);
  result.snap = value.value("snap", true);
  return result;
}
Json ruleJson(const InputRule &rule) {
  return {{"id", rule.id}, {"event", int(rule.event)}, {"key", rule.key}, {"shift", rule.shift},
          {"ctrl", rule.ctrl}, {"alt", rule.alt}, {"modifierTap", rule.modifierTap}, {"actions", rule.actions},
          {"priority", rule.priority}, {"repeat", int(rule.repeat)}, {"label", Storage::utf8(rule.label)}};
}
InputRule readRule(const Json &item) {
  InputRule rule;
  rule.id = item.at("id").get<std::string>();
  rule.event = InputEventType(std::clamp(item.value("event", 0), 0, 4));
  rule.key = std::clamp(item.value("key", 0), 0, 254);
  rule.shift = item.value("shift", false); rule.ctrl = item.value("ctrl", false); rule.alt = item.value("alt", false);
  rule.modifierTap = item.value("modifierTap", false);
  rule.actions = item.at("actions").get<std::vector<std::string>>();
  rule.priority = std::clamp(item.value("priority", 0), 0, 1000);
  rule.repeat = RepeatBehavior(std::clamp(item.value("repeat", 2), 0, 2));
  rule.label = Storage::wide(item.value("label", std::string()));
  if (rule.actions.empty() || rule.actions.size() > 16) throw std::wstring(L"键位覆盖无效");
  return rule;
}
}
CharacterLibrary::CharacterLibrary(std::filesystem::path root) : m_root(std::move(root)) {}
std::wstring CharacterLibrary::packagePath(const std::string &hash) const {
  if (!validHash(hash)) throw std::wstring(L"角色文件标识无效");
  return (m_root / L"characters" / Storage::wide(hash) / L"preset.zip").wstring();
}
bool CharacterLibrary::load(std::wstring &message) {
  const auto path = m_root / L"userState.json";
  m_canSave = true;
  if (!std::filesystem::exists(path) && !std::filesystem::exists(path.wstring() + L".bak")) return false;
  for (int attempt = 0; attempt < 2; ++attempt) {
    try {
      const auto bytes = Storage::read(attempt ? std::filesystem::path(path.wstring() + L".bak") : path, 8 * 1024 * 1024);
      const auto data = Json::parse(bytes);
      if (data.at("schemaVersion") != 1) throw std::wstring(L"用户配置版本不受支持");
      std::vector<CharacterEntry> library;
      std::vector<PetRecord> pets;
      std::set<std::string> ids;
      if (!data.at("library").is_array() || data["library"].size() > 1000) throw std::wstring(L"角色库条目数量无效");
      for (const auto &item : data.at("library")) {
        CharacterEntry entry;
        entry.libraryId = item.at("libraryId").get<std::string>();
        entry.packageId = item.at("packageId").get<std::string>();
        entry.sourceHash = item.at("sourceHash").get<std::string>();
        entry.currentHash = item.at("currentHash").get<std::string>();
        if (entry.libraryId.empty() || !ids.insert(entry.libraryId).second || !validHash(entry.sourceHash) || !validHash(entry.currentHash))
          throw std::wstring(L"角色库标识无效");
        entry.name = Storage::wide(item.at("name").get<std::string>());
        entry.sourcePath = Storage::wide(item.value("sourcePath", std::string()));
        entry.bytes = item.value("bytes", uint64_t(0));
        entry.actionCount = item.value("actionCount", 0u);
        entry.defaults = readSettings(item.value("defaults", Json::object()));
        entry.disabledRules = item.value("disabledRules", std::set<std::string>());
        library.push_back(std::move(entry));
      }
      if (!data.at("instances").is_array() || data["instances"].size() > 4) throw std::wstring(L"桌宠实例数量无效");
      std::set<std::string> instanceIds;
      for (const auto &item : data.at("instances")) {
        PetRecord pet;
        pet.instanceId = item.at("instanceId").get<std::string>();
        pet.libraryId = item.at("libraryId").get<std::string>();
        if (!ids.contains(pet.libraryId) || pet.instanceId.empty() || !instanceIds.insert(pet.instanceId).second)
          throw std::wstring(L"桌宠实例引用无效");
        pet.settings = readSettings(item.value("settings", Json::object()));
        pet.paused = item.value("paused", false); pet.visible = item.value("visible", true);
        pet.inheritDefaults = item.value("inheritDefaults", true);
        pet.disabledRules = item.value("disabledRules", std::set<std::string>());
        if (item.contains("ruleOverrides")) {
          if (item["ruleOverrides"].size() > 128) throw std::wstring(L"键位覆盖数量无效");
          for (const auto &rule : item["ruleOverrides"]) pet.ruleOverrides.push_back(readRule(rule));
        }
        if (item.contains("placement")) {
          const auto &position = item["placement"];
          pet.placement.monitor = Storage::wide(position.value("monitor", std::string()));
          pet.placement.x = std::clamp(position.value("x", 1.0), 0.0, 1.0);
          pet.placement.y = std::clamp(position.value("y", 1.0), 0.0, 1.0);
          pet.placement.edgeX = std::clamp(position.value("edgeX", -1), -1, 1);
          pet.placement.edgeY = std::clamp(position.value("edgeY", -1), -1, 1);
          pet.placement.gapX = std::clamp(position.value("gapX", 24), 0, 8192);
          pet.placement.gapY = std::clamp(position.value("gapY", 24), 0, 8192);
          pet.placement.valid = true;
        }
        pets.push_back(std::move(pet));
      }
      entries = std::move(library); instances = std::move(pets); allPaused = data.value("allPaused", false);
      if (attempt) {
        const auto preserved = m_root / (L"userState.invalid-" + Storage::wide(Storage::newId()) + L".json");
        if (std::filesystem::exists(path)) std::filesystem::copy_file(path, preserved);
        Storage::write(path, bytes);
        message = L"已从备份恢复用户配置";
      }
      return true;
    } catch (...) {
      if (attempt) {
        try {
          for (const auto &original : {path, std::filesystem::path(path.wstring() + L".bak")})
            if (std::filesystem::exists(original))
              std::filesystem::copy_file(original, m_root / (L"userState.invalid-" + Storage::wide(Storage::newId()) + L".json"));
          message = L"用户配置无法读取，原文件已备份为 userState.invalid 文件";
        } catch (...) { m_canSave = false; message = L"无法读取或备份用户配置，当前设置暂时无法保存"; }
        return true;
      }
    }
  }
  return false;
}
void CharacterLibrary::save() const {
  if (!m_canSave) throw std::wstring(L"用户配置尚未备份，无法覆盖原文件");
  Json state{{"schemaVersion", 1}, {"allPaused", allPaused}, {"library", Json::array()}, {"instances", Json::array()}};
  for (const auto &entry : entries)
    state["library"].push_back({{"libraryId", entry.libraryId}, {"packageId", entry.packageId},
      {"sourceHash", entry.sourceHash}, {"currentHash", entry.currentHash}, {"name", Storage::utf8(entry.name)},
      {"sourcePath", Storage::utf8(entry.sourcePath)}, {"bytes", entry.bytes}, {"actionCount", entry.actionCount},
      {"defaults", settingsJson(entry.defaults)}, {"disabledRules", entry.disabledRules}});
  for (const auto &pet : instances) {
    Json item{{"instanceId", pet.instanceId}, {"libraryId", pet.libraryId}, {"settings", settingsJson(pet.settings)},
              {"paused", pet.paused}, {"visible", pet.visible}, {"inheritDefaults", pet.inheritDefaults},
              {"disabledRules", pet.disabledRules}, {"ruleOverrides", Json::array()}};
    for (const auto &rule : pet.ruleOverrides) item["ruleOverrides"].push_back(ruleJson(rule));
    if (pet.placement.valid)
      item["placement"] = {{"monitor", Storage::utf8(pet.placement.monitor)}, {"x", pet.placement.x}, {"y", pet.placement.y},
                            {"edgeX", pet.placement.edgeX}, {"edgeY", pet.placement.edgeY},
                            {"gapX", pet.placement.gapX}, {"gapY", pet.placement.gapY}};
    state["instances"].push_back(std::move(item));
  }
  const auto text = state.dump(2);
  Storage::write(m_root / L"userState.json", {text.begin(), text.end()}, true);
}
CharacterEntry *CharacterLibrary::find(const std::string &id) {
  const auto found = std::find_if(entries.begin(), entries.end(), [&](const auto &entry) { return entry.libraryId == id; });
  return found == entries.end() ? nullptr : &*found;
}
const CharacterEntry *CharacterLibrary::find(const std::string &id) const {
  return const_cast<CharacterLibrary *>(this)->find(id);
}
CharacterEntry &CharacterLibrary::add(const std::shared_ptr<CharacterDefinition> &character,
                                     const std::wstring &source, const PetSettings &defaults,
                                     const std::string &replaceId, bool edited) {
  if (!character) throw std::wstring(L"角色数据为空");
  if (replaceId.empty())
    for (auto &entry : entries) if (entry.currentHash == character->contentHash) return entry;
  CharacterEntry *existing = replaceId.empty() ? nullptr : find(replaceId);
  CharacterEntry candidate = existing ? *existing : CharacterEntry{};
  auto *entry = &candidate;
  if (!existing) {
    if (entries.size() >= 1000) throw std::wstring(L"角色库已达到 1000 个条目");
    entry->libraryId = Storage::newId(); entry->defaults = defaults;
  }
  entry->packageId = character->id; entry->name = character->name; entry->currentHash = character->contentHash;
  if (!edited || entry->sourceHash.empty()) { entry->sourceHash = character->contentHash; entry->sourcePath = source; }
  entry->bytes = std::filesystem::file_size(character->archivePath);
  entry->actionCount = unsigned(character->actions.size());
  writeThumbnail(*entry, *character);
  if (existing) { *existing = std::move(candidate); return *existing; }
  entries.push_back(std::move(candidate)); return entries.back();
}
std::shared_ptr<Gdiplus::Bitmap> CharacterLibrary::thumbnail(CharacterEntry &entry) {
  if (!entry.thumbnail) {
    size_t loaded = 0;
    for (const auto &item : entries) loaded += item.thumbnail != nullptr;
    if (loaded >= 64) for (auto &item : entries) item.thumbnail.reset();
    const auto path = m_root / L"thumbnails" / (Storage::wide(entry.currentHash) + L".png");
    auto bitmap = std::make_shared<Gdiplus::Bitmap>(path.c_str());
    if (bitmap->GetLastStatus() == Gdiplus::Ok) entry.thumbnail = std::move(bitmap);
  }
  return entry.thumbnail;
}
void CharacterLibrary::remove(const std::string &id) {
  std::erase_if(instances, [&](const auto &pet) { return pet.libraryId == id; });
  std::erase_if(entries, [&](const auto &entry) { return entry.libraryId == id; });
}
ImportResult CharacterLibrary::open(const CharacterEntry &entry) const {
  return PresetLoader::load(packagePath(entry.currentHash), (m_root / L"characters").wstring());
}
void CharacterLibrary::writeThumbnail(CharacterEntry &entry, const CharacterDefinition &character) {
  auto thumbnail = std::make_shared<Gdiplus::Bitmap>(128, 128, PixelFormat32bppPARGB);
  const auto &clip = character.animation(character.actionIndex(character.defaultAction));
  {
    Gdiplus::Graphics painter(thumbnail.get());
    painter.Clear(Gdiplus::Color(0, 0, 0, 0));
    painter.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
    const double scale = std::min(128.0 / clip.width, 128.0 / clip.height);
    const int width = int(clip.width * scale), height = int(clip.height * scale);
    painter.DrawImage(clip.frames.front().image.get(), (128 - width) / 2, (128 - height) / 2, width, height);
  }
  UINT count = 0, size = 0;
  Gdiplus::GetImageEncodersSize(&count, &size);
  std::vector<uint8_t> buffer(size);
  auto *encoders = reinterpret_cast<Gdiplus::ImageCodecInfo *>(buffer.data());
  Gdiplus::GetImageEncoders(count, size, encoders);
  const auto directory = m_root / L"thumbnails";
  std::filesystem::create_directories(directory);
  const auto path = directory / (Storage::wide(entry.currentHash) + L".png");
  if (!std::filesystem::exists(path))
    for (UINT i = 0; i < count; ++i)
      if (wcscmp(encoders[i].MimeType, L"image/png") == 0) { thumbnail->Save(path.c_str(), &encoders[i].Clsid); break; }
  entry.thumbnail = std::move(thumbnail);
}
