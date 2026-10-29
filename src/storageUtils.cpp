#include "storageUtils.h"
#include <windows.h>
#include <bcrypt.h>
#include <objbase.h>
#include <fstream>

namespace Storage {
std::string utf8(const std::wstring &text) {
  if (text.empty()) return {};
  const int count = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
  if (!count) throw std::wstring(L"文本编码无效");
  std::string result(count, '\0');
  WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count, nullptr, nullptr);
  return result;
}
std::wstring wide(const std::string &text) {
  if (text.empty()) return {};
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), nullptr, 0);
  if (!count) throw std::wstring(L"文本不是有效的 UTF-8");
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), int(text.size()), result.data(), count);
  return result;
}
std::string hash(const std::vector<uint8_t> &bytes) {
  BCRYPT_ALG_HANDLE algorithm{};
  if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
    throw std::wstring(L"无法初始化资源校验");
  uint8_t digest[32]{};
  const auto result = BCryptHash(algorithm, nullptr, 0, const_cast<PUCHAR>(bytes.data()), ULONG(bytes.size()), digest, 32);
  BCryptCloseAlgorithmProvider(algorithm, 0);
  if (result < 0) throw std::wstring(L"无法计算资源校验值");
  constexpr char hex[] = "0123456789abcdef";
  std::string value;
  for (const auto byte : digest) { value += hex[byte >> 4]; value += hex[byte & 15]; }
  return value;
}
std::string newId() {
  GUID id{};
  if (FAILED(CoCreateGuid(&id))) throw std::wstring(L"无法创建标识");
  wchar_t text[40]{};
  StringFromGUID2(id, text, 40);
  return utf8(std::wstring(text + 1, 36));
}
std::vector<uint8_t> read(const std::filesystem::path &path, uint64_t limit) {
  std::ifstream stream(path, std::ios::binary | std::ios::ate);
  if (!stream) throw std::wstring(L"无法读取文件：") + path.filename().wstring();
  const auto length = stream.tellg();
  if (length < 0 || uint64_t(length) > limit) throw std::wstring(L"文件大小超过限制：") + path.filename().wstring();
  std::vector<uint8_t> bytes(size_t(length), uint8_t{});
  stream.seekg(0);
  if (length && !stream.read(reinterpret_cast<char *>(bytes.data()), length))
    throw std::wstring(L"文件读取失败：") + path.filename().wstring();
  return bytes;
}
void write(const std::filesystem::path &path, const std::vector<uint8_t> &bytes, bool backup) {
  std::filesystem::create_directories(path.parent_path());
  auto temporary = path;
  temporary += L".tmp-" + wide(newId());
  HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
  if (file == INVALID_HANDLE_VALUE) throw std::wstring(L"无法创建文件：") + path.filename().wstring();
  DWORD written = 0;
  bool ok = bytes.size() <= MAXDWORD && WriteFile(file, bytes.data(), DWORD(bytes.size()), &written, nullptr) &&
            written == bytes.size() && FlushFileBuffers(file);
  CloseHandle(file);
  if (ok) {
    auto previous = path;
    previous += L".bak";
    if (std::filesystem::exists(path))
      ok = ReplaceFileW(path.c_str(), temporary.c_str(), backup ? previous.c_str() : nullptr, 0, nullptr, nullptr);
    else
      ok = MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH);
  }
  if (!ok) {
    DeleteFileW(temporary.c_str());
    throw std::wstring(L"无法保存文件，请检查目录权限与磁盘空间：") + path.filename().wstring();
  }
}
}
