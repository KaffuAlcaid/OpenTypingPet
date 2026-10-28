#include "appSettings.h"
#include <algorithm>
#include <cwchar>
#include <vector>
namespace {
constexpr auto keyPath = L"Software\\OpenTypingPet\\OpenTypingPet";
std::wstring readText(HKEY key, const wchar_t *name,
                      const std::wstring &fallback = L"") {
  DWORD size = 0;
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, nullptr,
                   &size) != ERROR_SUCCESS)
    return fallback;
  std::wstring text(size / sizeof(wchar_t), L'\0');
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_SZ, nullptr, text.data(),
                   &size) != ERROR_SUCCESS)
    return fallback;
  text.resize(wcsnlen(text.c_str(), text.size()));
  return text;
}
int readInt(HKEY key, const wchar_t *name, int fallback) {
  DWORD value = 0, size = sizeof(value);
  if (RegGetValueW(key, nullptr, name, RRF_RT_REG_DWORD, nullptr, &value,
                   &size) == ERROR_SUCCESS)
    return int(value);
  try {
    return std::stoi(readText(key, name));
  } catch (...) {
    return fallback;
  }
}
bool readBool(HKEY key, const wchar_t *name, bool fallback) {
  const auto value = readText(key, name, fallback ? L"true" : L"false");
  return value == L"true" || value == L"1";
}
void writeText(HKEY key, const wchar_t *name, const std::wstring &value) {
  RegSetValueExW(key, name, 0, REG_SZ,
                 reinterpret_cast<const BYTE *>(value.c_str()),
                 DWORD((value.size() + 1) * sizeof(wchar_t)));
}
void writeInt(HKEY key, const wchar_t *name, int value) {
  const DWORD data = DWORD(value);
  RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE *>(&data),
                 sizeof(data));
}
} // namespace
void Preferences::load() {
  HKEY key = nullptr;
  if (RegOpenKeyExW(HKEY_CURRENT_USER, keyPath, 0, KEY_QUERY_VALUE, &key) !=
      ERROR_SUCCESS)
    return;
  presetPath = readText(key, L"presetPath");
  width = std::clamp(readInt(key, L"width", 280), 100, 800);
  idleDelay = std::clamp(readInt(key, L"idleDelay", 800), 200, 5000);
  minimumHold = std::clamp(readInt(key, L"minimumHold", 0), 0, 500);
  topmost = readBool(key, L"topmost", true);
  locked = readBool(key, L"locked", false);
  smooth = readBool(key, L"smooth", false);
  try {
    opacity = std::clamp(
        int(std::stod(readText(key, L"opacity", L"1")) * 100 + 0.5), 20, 100);
  } catch (...) {
  }
  const auto mode = readText(key, L"guiTheme", L"system");
  theme = mode == L"light" ? 1 : mode == L"dark" ? 2 : 0;
  auto saved = readText(key, L"nativePosition");
  bool legacy = saved.empty();
  if (legacy)
    saved = readText(key, L"position");
  long x = 0, y = 0;
  if (swscanf_s(saved.c_str(), L"@Point(%ld %ld)", &x, &y) == 2) {
    position = {x, y};
    hasPosition = true;
    if (legacy) {
      const auto dpi = GetDpiForSystem();
      position = {MulDiv(x, dpi, 96), MulDiv(y, dpi, 96)};
    }
  }
  RegCloseKey(key);
}
void Preferences::save() const {
  HKEY key = nullptr;
  if (RegCreateKeyExW(HKEY_CURRENT_USER, keyPath, 0, nullptr, 0, KEY_SET_VALUE,
                      nullptr, &key, nullptr) != ERROR_SUCCESS)
    return;
  writeText(key, L"presetPath", presetPath);
  writeInt(key, L"width", width);
  writeInt(key, L"idleDelay", idleDelay);
  writeInt(key, L"minimumHold", minimumHold);
  writeText(key, L"opacity", std::to_wstring(opacity / 100.0));
  writeText(key, L"topmost", topmost ? L"true" : L"false");
  writeText(key, L"locked", locked ? L"true" : L"false");
  writeText(key, L"smooth", smooth ? L"true" : L"false");
  writeText(key, L"guiTheme",
            theme == 1   ? L"light"
            : theme == 2 ? L"dark"
                         : L"system");
  if (hasPosition)
    writeText(key, L"nativePosition",
              L"@Point(" + std::to_wstring(position.x) + L" " +
                  std::to_wstring(position.y) + L")");
  RegCloseKey(key);
}
ThemeColors themeColors(int mode) {
  DWORD light = 1, size = sizeof(light);
  RegGetValueW(
      HKEY_CURRENT_USER,
      L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
      L"AppsUseLightTheme", RRF_RT_REG_DWORD, nullptr, &light, &size);
  const bool dark = mode == 2 || (mode == 0 && !light);
  if (dark)
    return {true,
            RGB(29, 31, 32),
            RGB(37, 40, 41),
            RGB(33, 37, 36),
            RGB(236, 240, 237),
            RGB(177, 188, 181),
            RGB(69, 77, 72),
            RGB(117, 210, 178),
            RGB(48, 78, 65),
            RGB(47, 53, 50),
            RGB(255, 171, 156)};
  return {false,
          RGB(250, 251, 252),
          RGB(255, 255, 255),
          RGB(238, 242, 240),
          RGB(38, 51, 45),
          RGB(101, 115, 107),
          RGB(218, 226, 221),
          RGB(8, 127, 115),
          RGB(226, 240, 233),
          RGB(239, 243, 241),
          RGB(164, 66, 51)};
}
