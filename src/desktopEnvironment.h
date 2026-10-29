#pragma once
#include "appSettings.h"
#include <string>
#include <vector>

struct DesktopMonitor {
  HMONITOR handle = nullptr;
  std::wstring id, name;
  RECT bounds{}, work{};
  UINT dpi = 96;
  bool primary = false;
};
class DesktopEnvironment {
public:
  static constexpr UINT ChangedMessage = WM_APP + 4;
  ~DesktopEnvironment();
  void attach(HWND window, bool notifications = true);
  bool handle(UINT message, WPARAM wParam, LPARAM lParam);
  void refreshMonitors();
  const std::vector<DesktopMonitor> &monitors() const { return m_monitors; }
  const DesktopMonitor &monitor(const std::wstring &id, HWND fallback = nullptr) const;
  bool fullscreenOn(HMONITOR monitor, const std::wstring &exceptions) const;
  bool locked = false, displayOff = false, suspended = false;
private:
  static void CALLBACK windowEvent(HWINEVENTHOOK, DWORD, HWND, LONG, LONG, DWORD, DWORD);
  HWND m_window = nullptr;
  HPOWERNOTIFY m_power = nullptr;
  HWINEVENTHOOK m_foregroundHook = nullptr, m_locationHook = nullptr;
  std::vector<DesktopMonitor> m_monitors;
};
