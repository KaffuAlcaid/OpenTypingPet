#pragma once
#include <string>
#include <windows.h>
struct PetSettings {
  int width = 280, opacity = 100, idleDelay = 800, minimumHold = 0;
  bool topmost = true, locked = false, smooth = false, hasPosition = false;
  bool snap = true;
  POINT position{};
};
struct Preferences : PetSettings {
  std::wstring presetPath;
  int theme = 0;
  bool initialized = false, showSettingsOnStart = false;
  bool pauseOnLock = true, pauseOnDisplayOff = true;
  int fullscreenPolicy = 0;
  std::wstring fullscreenExceptions;
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
