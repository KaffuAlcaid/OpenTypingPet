#include "presetLoader.h"
#include "animationDecoder.h"
#include "characterManifest.h"
#include <algorithm>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <map>
#include <miniz.h>
#include <set>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
namespace fs = std::filesystem;
namespace {
constexpr uint64_t MiB = 1024 * 1024;
void require(bool condition, const wchar_t *message) {
  if (!condition)
    throw std::wstring(message);
}
std::wstring utf8(const std::vector<uint8_t> &data) {
  const auto *bytes = reinterpret_cast<const char *>(data.data());
  int size = int(data.size());
  if (size >= 3 && data[0] == 0xef && data[1] == 0xbb && data[2] == 0xbf) {
    bytes += 3;
    size -= 3;
  }
  if (!size)
    return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes,
                                        size, nullptr, 0);
  require(count > 0, L"预设文本不是有效的 UTF-8。");
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes, size, result.data(),
                      count);
  return result;
}
std::wstring trim(std::wstring value) {
  const auto start = value.find_first_not_of(L" \r\n\t");
  return start == std::wstring::npos
             ? L""
             : value.substr(start,
                            value.find_last_not_of(L" \r\n\t") - start + 1);
}
std::vector<uint8_t> read(const fs::path &path) {
  std::ifstream file(path, std::ios::binary | std::ios::ate);
  require(bool(file), L"无法打开预设 ZIP。");
  const auto size = file.tellg();
  require(size > 0 && size <= 64 * MiB, L"ZIP 大小必须在 64 MB 以内。");
  std::vector<uint8_t> bytes(static_cast<size_t>(size));
  file.seekg(0);
  file.read(reinterpret_cast<char *>(bytes.data()), size);
  require(bool(file), L"读取 ZIP 失败。");
  return bytes;
}
std::wstring hash(const std::vector<uint8_t> &bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  require(BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM,
                                      nullptr, 0) >= 0,
          L"无法初始化资源校验。");
  uint8_t digest[32]{};
  const auto status =
      BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                 ULONG(bytes.size()), digest, 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  require(status >= 0, L"无法计算资源校验值。");
  std::wstring result;
  const wchar_t *hex = L"0123456789abcdef";
  for (auto byte : digest) {
    result += hex[byte >> 4];
    result += hex[byte & 15];
  }
  return result;
}
std::wstring keyName(int key) {
  if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9'))
    return std::wstring(1, wchar_t(key));
  if (key >= VK_F1 && key <= VK_F24)
    return L"F" + std::to_wstring(key - VK_F1 + 1);
  switch (key) {
  case 1:
    return L"鼠标左键";
  case 2:
    return L"鼠标右键";
  case 4:
    return L"鼠标中键";
  case 5:
    return L"鼠标侧键 1";
  case 6:
    return L"鼠标侧键 2";
  case 8:
    return L"Backspace";
  case 9:
    return L"Tab";
  case 13:
    return L"Enter";
  case 16:
    return L"Shift";
  case 17:
    return L"Ctrl";
  case 18:
    return L"Alt";
  case 20:
    return L"CapsLock";
  case 27:
    return L"Esc";
  case 32:
    return L"Space";
  case 33:
    return L"Page Up";
  case 34:
    return L"Page Down";
  case 35:
    return L"End";
  case 36:
    return L"Home";
  case 37:
    return L"Left";
  case 38:
    return L"Up";
  case 39:
    return L"Right";
  case 40:
    return L"Down";
  case 45:
    return L"Insert";
  case 46:
    return L"Delete";
  default:
    return L"VK " + std::to_wstring(key);
  }
}
} // namespace
std::wstring PresetLoader::storageRoot() {
  PWSTR path = nullptr;
  require(
      SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path)),
      L"无法定位应用数据目录。");
  fs::path result(path);
  CoTaskMemFree(path);
  return (result / L"OpenTypingPet" / L"OpenTypingPet" / L"characters")
      .wstring();
}
ImportResult PresetLoader::load(const std::wstring &zipPath,
                                const std::wstring &storageRoot) {
  mz_zip_archive archive{};
  try {
    auto zip = read(zipPath);
    require(mz_zip_reader_init_mem(&archive, zip.data(), zip.size(), 0),
            L"ZIP 无效、已损坏或已加密。");
    require(mz_zip_reader_get_num_files(&archive) <= 256, L"角色包包含过多文件。");
    CharacterResources resources;
    uint64_t unpacked = 0;
    std::string manifestPath;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
      mz_zip_archive_file_stat stat{};
      require(mz_zip_reader_file_stat(&archive, i, &stat), L"ZIP 目录无效。");
      std::string path(stat.m_filename);
      std::replace(path.begin(), path.end(), '\\', '/');
      require(!path.empty() && path[0] != '/' && path.find(':') == std::string::npos,
              L"ZIP 包含无效路径。");
      std::istringstream pieces(path);
      std::string piece;
      while (std::getline(pieces, piece, '/'))
        require(piece != ".." && piece != "." && !piece.empty(), L"ZIP 包含无效路径。");
      if (stat.m_is_directory)
        continue;
      require(stat.m_uncomp_size <= 15 * MiB, L"ZIP 解压后的文件过大。");
      unpacked += stat.m_uncomp_size;
      require(unpacked <= 96 * MiB, L"ZIP 解压后的文件过大。");
      std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
        return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : char(c);
      });
      const auto slash = path.find_last_of('/');
      const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
      const bool manifest = name == "character.json";
      const bool image = path.ends_with(".png") || path.ends_with(".apng") ||
                         path.ends_with(".gif") || path.ends_with(".jpg") || path.ends_with(".jpeg");
      if (!manifest && !image && name != "name.txt" && name != "patterns.txt")
        continue;
      require(!resources.contains(path), L"ZIP 包含重复资源。");
      require(!stat.m_is_encrypted, L"不支持加密角色包。");
      require(image || stat.m_uncomp_size <= (manifest ? 65536 : 16384),
              L"角色配置文件过大。");
      if (manifest) {
        require(manifestPath.empty(), L"角色包只能包含一份 character.json。");
        manifestPath = path;
      }
      std::vector<uint8_t> bytes(size_t(stat.m_uncomp_size));
      require(mz_zip_reader_extract_to_mem(&archive, i, bytes.data(), bytes.size(), 0),
              L"无法解压角色资源。");
      resources.emplace(std::move(path), std::move(bytes));
    }
    mz_zip_reader_end(&archive);
    archive = {};
    const auto digest = hash(zip);
    std::string digestText(digest.size(), '\0');
    std::transform(digest.begin(), digest.end(), digestText.begin(), [](wchar_t c) { return char(c); });
    std::shared_ptr<CharacterDefinition> character;
    if (!manifestPath.empty()) {
      character = loadCharacterManifest(resources, manifestPath);
    } else {
      CharacterResources files;
      for (auto &[path, bytes] : resources) {
        const auto slash = path.find_last_of('/');
        const auto name = path.substr(slash == std::string::npos ? 0 : slash + 1);
        require(!files.contains(name), L"TypingPet 预设包含重复资源。");
        files.emplace(name, std::move(bytes));
      }
      for (const auto *required : {"name.txt", "basic.png", "left.png", "right.png"})
        require(files.contains(required), L"预设缺少角色名称或基础图片。");
      character = std::make_shared<CharacterDefinition>();
      character->id = "typingpet." + digestText;
      character->name = trim(utf8(files.at("name.txt"))).substr(0, 80);
      require(!character->name.empty(), L"角色名称为空。");
      character->defaultAction = "idle";
      uint64_t budget = 0;
      const auto addAction = [&](const std::string &id, const std::wstring &name, const std::string &file) {
        require(files.contains(file), L"预设缺少动作图片。");
        ActionDefinition action;
        action.id = id;
        action.label = name;
        action.clip = character->clips.size();
        character->clips.push_back(decodeAnimation(files.at(file), name, budget));
        character->actions.push_back(std::move(action));
      };
      addAction("idle", L"待机", "basic.png");
      addAction("typeLeft", L"左手", "left.png");
      addAction("typeRight", L"右手", "right.png");
      character->width = character->clips[0].width;
      character->height = character->clips[0].height;
      std::wistringstream lines(files.contains("patterns.txt") ? utf8(files.at("patterns.txt")) : L"");
      std::set<std::wstring> triggers;
      std::wstring line;
      size_t index = 0;
      while (std::getline(lines, line)) {
        const size_t imageIndex = index++;
        line = trim(line);
        if (line.empty())
          continue;
        require(imageIndex < 20, L"预设超过 20 个特殊动作。");
        std::vector<std::wstring> fields;
        size_t offset = 0;
        while (true) {
          const auto end = line.find(L',', offset);
          fields.push_back(line.substr(offset, end == std::wstring::npos ? end : end - offset));
          if (end == std::wstring::npos)
            break;
          offset = end + 1;
        }
        require(fields.size() == 4, L"按键绑定格式无效。");
        require(!fields[0].empty() && fields[0].size() <= 3 &&
                    std::all_of(fields[0].begin(), fields[0].end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }),
                L"键码无效。");
        const int key = std::stoi(fields[0]);
        require(key > 0 && key <= 254, L"键码无效。");
        for (size_t n = 1; n < 4; ++n)
          require(fields[n] == L"0" || fields[n] == L"1", L"修饰键只能为 0 或 1。");
        const auto canonical = std::to_wstring(key) + L"," + fields[1] + L"," + fields[2] + L"," + fields[3];
        require(triggers.insert(canonical).second, L"预设包含重复按键绑定。");
        InputRule rule;
        rule.id = "special" + std::to_string(imageIndex);
        rule.key = key;
        rule.shift = fields[1] == L"1";
        rule.ctrl = fields[2] == L"1";
        rule.alt = fields[3] == L"1";
        rule.priority = 100;
        if (key == 1 || key == 2 || key == 4 || key == 5 || key == 6)
          rule.event = InputEventType::MouseDown;
        if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) {
          rule.event = InputEventType::KeyUp;
          rule.modifierTap = true;
        }
        if (rule.ctrl) rule.label += L"Ctrl+";
        if (rule.shift) rule.label += L"Shift+";
        if (rule.alt) rule.label += L"Alt+";
        rule.label += keyName(key);
        rule.actions.push_back(rule.id);
        addAction(rule.id, rule.label, "pattern" + std::to_string(imageIndex) + ".png");
        character->rules.push_back(std::move(rule));
      }
      InputRule typing;
      typing.id = "typing";
      typing.event = InputEventType::Typing;
      typing.label = L"普通打字";
      typing.actions = {"typeLeft", "typeRight"};
      typing.repeat = RepeatBehavior::Restart;
      character->rules.push_back(std::move(typing));
    }
    character->contentHash = digestText;
    const fs::path directory = fs::path(storageRoot) / digest;
    fs::create_directories(directory);
    const auto archivePath = directory / L"preset.zip";
    if (!fs::exists(archivePath) || read(archivePath) != zip) {
      const auto temporary = directory / (L"preset-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char *>(zip.data()), std::streamsize(zip.size()));
      stream.close();
      require(bool(stream) && MoveFileExW(temporary.c_str(), archivePath.c_str(),
                                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
              L"无法保存角色预设。");
    }
    character->archivePath = archivePath.wstring();
    return {std::move(character), {}};
  } catch (const std::wstring &error) {
    if (archive.m_pState)
      mz_zip_reader_end(&archive);
    return {nullptr, error};
  } catch (...) {
    if (archive.m_pState)
      mz_zip_reader_end(&archive);
    return {nullptr, L"无法读取预设，请检查文件内容、目录权限和磁盘空间。"};
  }
}
