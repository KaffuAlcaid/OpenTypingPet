#pragma once
#include <string>
#include <windows.h>
struct Preferences {
  std::wstring presetPath;
  int width = 280, opacity = 100, idleDelay = 800, minimumHold = 0, theme = 0;
  bool topmost = true, locked = false, smooth = false, hasPosition = false;
  bool initialized = false, showSettingsOnStart = false;
  POINT position{};
  void load();
  void save() const;
};
struct ThemeColors {
  bool dark = false;
  COLORREF window, base, panel, text, muted, border, accent, selection, button,
      error;
};
ThemeColors themeColors(int mode);
bool loginStartupEnabled();
bool setLoginStartup(bool enabled);
