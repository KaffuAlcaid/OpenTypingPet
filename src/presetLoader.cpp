#include "presetLoader.h"
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
bool apng(const std::vector<uint8_t> &bytes) {
  const uint8_t signature[] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (bytes.size() < 8 || !std::equal(signature, signature + 8, bytes.begin()))
    return false;
  for (size_t offset = 8; offset + 12 <= bytes.size();) {
    const uint32_t size = uint32_t(bytes[offset]) << 24 |
                          uint32_t(bytes[offset + 1]) << 16 |
                          uint32_t(bytes[offset + 2]) << 8 | bytes[offset + 3];
    if (size > bytes.size() - offset - 12)
      break;
    if (std::equal(bytes.begin() + offset + 4, bytes.begin() + offset + 8,
                   "acTL"))
      return true;
    offset += size_t(size) + 12;
  }
  return false;
}
AnimationClip decode(const std::vector<uint8_t> &bytes,
                     const std::wstring &label, uint64_t &budget) {
  require(!bytes.empty() && bytes.size() <= 15 * MiB,
          L"图像为空或超过 15 MB。");
  require(!apng(bytes), L"当前版本尚不支持 APNG。");
  IStream *rawStream = SHCreateMemStream(bytes.data(), UINT(bytes.size()));
  require(rawStream != nullptr, L"无法读取图像资源。");
  std::unique_ptr<IStream, void (*)(IStream *)> stream(
      rawStream, [](IStream *p) { p->Release(); });
  std::unique_ptr<Gdiplus::Image> source(
      Gdiplus::Image::FromStream(stream.get(), FALSE));
  require(source && source->GetLastStatus() == Gdiplus::Ok, L"无法解码图像。");
  GUID format{};
  source->GetRawFormat(&format);
  require(format == Gdiplus::ImageFormatPNG ||
              format == Gdiplus::ImageFormatJPEG ||
              format == Gdiplus::ImageFormatGIF,
          L"仅支持 PNG、JPEG 和 GIF。");
  AnimationClip clip;
  if (format == Gdiplus::ImageFormatGIF)
    clip.loops = 1;
  clip.label = label;
  clip.width = source->GetWidth();
  clip.height = source->GetHeight();
  require(clip.width && clip.height && clip.width <= 2048 &&
              clip.height <= 2048,
          L"图像尺寸无效或超过 2048 像素。");
  const GUID dimension = Gdiplus::FrameDimensionTime;
  const unsigned count = format == Gdiplus::ImageFormatGIF
                             ? std::max(1u, source->GetFrameCount(&dimension))
                             : 1;
  require(count <= 300, L"动画超过 300 帧。");
  budget += uint64_t(clip.width) * clip.height * 4 * count;
  require(budget <= 128 * MiB, L"预设解码后的图像总量超过 128 MB。");
  std::vector<unsigned> delays(count, 100);
  const unsigned delaySize = source->GetPropertyItemSize(PropertyTagFrameDelay);
  if (delaySize) {
    std::vector<uint8_t> buffer(delaySize);
    auto *property = reinterpret_cast<Gdiplus::PropertyItem *>(buffer.data());
    if (source->GetPropertyItem(PropertyTagFrameDelay, delaySize, property) ==
            Gdiplus::Ok &&
        property->type == PropertyTagTypeLong)
      for (unsigned i = 0;
           i < count && (i + 1) * sizeof(ULONG) <= property->length; ++i) {
        const auto delay = static_cast<ULONG *>(property->value)[i];
        delays[i] = delay ? std::clamp(delay, 1ul, 360000ul) * 10 : 100;
      }
  }
  const unsigned loopSize = source->GetPropertyItemSize(PropertyTagLoopCount);
  if (loopSize) {
    std::vector<uint8_t> buffer(loopSize);
    auto *property = reinterpret_cast<Gdiplus::PropertyItem *>(buffer.data());
    if (source->GetPropertyItem(PropertyTagLoopCount, loopSize, property) ==
            Gdiplus::Ok &&
        property->length >= 2) {
      const auto loops = *static_cast<USHORT *>(property->value);
      clip.loops = loops ? int(loops) + 1 : -1;
    }
  }
  for (unsigned i = 0; i < count; ++i) {
    if (count > 1)
      require(source->SelectActiveFrame(&dimension, i) == Gdiplus::Ok,
              L"动画帧无效。");
    auto frame = std::make_shared<Gdiplus::Bitmap>(clip.width, clip.height,
                                                   PixelFormat32bppPARGB);
    Gdiplus::Graphics painter(frame.get());
    painter.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    require(painter.DrawImage(
                source.get(), Gdiplus::Rect(0, 0, clip.width, clip.height), 0,
                0, clip.width, clip.height, Gdiplus::UnitPixel) == Gdiplus::Ok,
            L"无法读取动画帧。");
    clip.frames.push_back({std::move(frame), delays[i]});
  }
  return clip;
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
    require(mz_zip_reader_get_num_files(&archive) <= 96, L"预设包含过多文件。");
    std::map<std::string, std::vector<uint8_t>> files;
    uint64_t unpacked = 0;
    for (mz_uint i = 0; i < mz_zip_reader_get_num_files(&archive); ++i) {
      mz_zip_archive_file_stat stat{};
      require(mz_zip_reader_file_stat(&archive, i, &stat), L"ZIP 目录无效。");
      std::string path(stat.m_filename);
      std::replace(path.begin(), path.end(), '\\', '/');
      require(!path.empty() && path[0] != '/' &&
                  path.find(':') == std::string::npos,
              L"ZIP 包含无效路径。");
      std::istringstream pieces(path);
      std::string piece;
      while (std::getline(pieces, piece, '/'))
        require(piece != "..", L"ZIP 包含无效路径。");
      if (stat.m_is_directory)
        continue;
      require(stat.m_uncomp_size <= 15 * MiB, L"ZIP 解压后的文件过大。");
      unpacked += stat.m_uncomp_size;
      require(unpacked <= 96 * MiB, L"ZIP 解压后的文件过大。");
      auto name = path.substr(path.find_last_of('/') == std::string::npos
                                  ? 0
                                  : path.find_last_of('/') + 1);
      std::transform(name.begin(), name.end(), name.begin(),
                     [](unsigned char c) { return char(std::tolower(c)); });
      bool known = name == "name.txt" || name == "patterns.txt" ||
                   name == "basic.png" || name == "left.png" ||
                   name == "right.png";
      if (name.starts_with("pattern") && name.ends_with(".png") &&
          name.size() >= 12 && name.size() <= 13) {
        const auto index = name.substr(7, name.size() - 11);
        known = std::all_of(index.begin(), index.end(),
                            [](char c) { return c >= '0' && c <= '9'; });
      }
      if (!known)
        continue;
      require(!files.contains(name), L"预设包含重复资源。");
      require(!stat.m_is_encrypted, L"不支持加密预设。");
      require(!name.ends_with(".txt") || stat.m_uncomp_size <= 16384,
              L"预设配置文件过大。");
      std::vector<uint8_t> bytes(size_t(stat.m_uncomp_size));
      require(mz_zip_reader_extract_to_mem(&archive, i, bytes.data(),
                                           bytes.size(), 0),
              L"无法解压预设资源。");
      files.emplace(std::move(name), std::move(bytes));
    }
    mz_zip_reader_end(&archive);
    archive = {};
    for (const auto *required :
         {"name.txt", "basic.png", "left.png", "right.png"})
      require(files.contains(required), L"预设缺少角色名称或基础图片。");
    auto character = std::make_shared<CharacterDefinition>();
    character->name = trim(utf8(files.at("name.txt"))).substr(0, 80);
    require(!character->name.empty(), L"角色名称为空。");
    uint64_t budget = 0;
    character->clips.push_back(decode(files.at("basic.png"), L"待机", budget));
    character->clips.push_back(decode(files.at("left.png"), L"左手", budget));
    character->clips.push_back(decode(files.at("right.png"), L"右手", budget));
    character->width = character->clips[0].width;
    character->height = character->clips[0].height;
    std::wistringstream lines(
        files.contains("patterns.txt") ? utf8(files.at("patterns.txt")) : L"");
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
      for (;;) {
        const auto end = line.find(L',', offset);
        fields.push_back(line.substr(
            offset, end == std::wstring::npos ? end : end - offset));
        if (end == std::wstring::npos)
          break;
        offset = end + 1;
      }
      require(fields.size() == 4, L"按键绑定格式无效。");
      require(!fields[0].empty() &&
                  std::all_of(fields[0].begin(), fields[0].end(),
                              [](wchar_t c) { return c >= L'0' && c <= L'9'; }),
              L"键码无效。");
      const int key = std::stoi(fields[0]);
      require(key > 0 && key <= 254, L"键码无效。");
      for (size_t n = 1; n < 4; ++n)
        require(fields[n] == L"0" || fields[n] == L"1",
                L"修饰键只能为 0 或 1。");
      const auto canonical = std::to_wstring(key) + L"," + fields[1] + L"," +
                             fields[2] + L"," + fields[3];
      require(triggers.insert(canonical).second, L"预设包含重复按键绑定。");
      InputBinding binding{key,
                           fields[1] == L"1",
                           fields[2] == L"1",
                           fields[3] == L"1",
                           character->clips.size(),
                           {}};
      if (binding.ctrl)
        binding.label += L"Ctrl+";
      if (binding.shift)
        binding.label += L"Shift+";
      if (binding.alt)
        binding.label += L"Alt+";
      binding.label += keyName(key);
      const auto name = "pattern" + std::to_string(imageIndex) + ".png";
      require(files.contains(name), L"预设缺少特殊动作图片。");
      character->clips.push_back(decode(files.at(name), binding.label, budget));
      character->bindings.push_back(std::move(binding));
    }
    const fs::path directory = fs::path(storageRoot) / hash(zip);
    fs::create_directories(directory);
    const auto archivePath = directory / L"preset.zip";
    if (!fs::exists(archivePath) || read(archivePath) != zip) {
      const auto temporary =
          directory /
          (L"preset-" + std::to_wstring(GetCurrentProcessId()) + L".tmp");
      std::ofstream stream(temporary, std::ios::binary | std::ios::trunc);
      stream.write(reinterpret_cast<const char *>(zip.data()),
                   std::streamsize(zip.size()));
      stream.close();
      require(
          bool(stream) &&
              MoveFileExW(temporary.c_str(), archivePath.c_str(),
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
