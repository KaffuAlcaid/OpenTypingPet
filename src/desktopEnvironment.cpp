#include "desktopEnvironment.h"
#include <algorithm>
#include <dwmapi.h>
#include <shellapi.h>
#include <shellscalingapi.h>
#include <wtsapi32.h>
#include <filesystem>
#include <map>
#include <sstream>

namespace {
HWND eventWindow = nullptr;
constexpr GUID sessionDisplay{0x2b84c20e, 0xad23, 0x4ddf, {0x93, 0xdb, 0x05, 0xff, 0xbd, 0x7e, 0xfc, 0xa5}};
std::wstring lower(std::wstring text) {
  std::transform(text.begin(), text.end(), text.begin(), [](wchar_t c) { return wchar_t(towlower(c)); });
  return text;
}
}
DesktopEnvironment::~DesktopEnvironment() {
  if (m_foregroundHook) UnhookWinEvent(m_foregroundHook);
  if (m_locationHook) UnhookWinEvent(m_locationHook);
  if (m_power) UnregisterPowerSettingNotification(m_power);
  if (m_window) WTSUnRegisterSessionNotification(m_window);
  if (eventWindow == m_window) eventWindow = nullptr;
}
void DesktopEnvironment::attach(HWND window, bool notifications) {
  m_window = window;
  refreshMonitors();
  if (!notifications) return;
  WTSRegisterSessionNotification(window, NOTIFY_FOR_THIS_SESSION);
  m_power = RegisterPowerSettingNotification(window, &sessionDisplay, DEVICE_NOTIFY_WINDOW_HANDLE);
  eventWindow = window;
  m_foregroundHook = SetWinEventHook(EVENT_SYSTEM_FOREGROUND, EVENT_SYSTEM_FOREGROUND, nullptr, windowEvent, 0, 0,
                                     WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
  m_locationHook = SetWinEventHook(EVENT_OBJECT_LOCATIONCHANGE, EVENT_OBJECT_LOCATIONCHANGE, nullptr, windowEvent, 0, 0,
                                   WINEVENT_OUTOFCONTEXT | WINEVENT_SKIPOWNPROCESS);
}
void CALLBACK DesktopEnvironment::windowEvent(HWINEVENTHOOK, DWORD event, HWND window, LONG object, LONG, DWORD, DWORD) {
  if (eventWindow && (event == EVENT_SYSTEM_FOREGROUND || (object == OBJID_WINDOW && window == GetForegroundWindow())))
    PostMessageW(eventWindow, ChangedMessage, 0, 0);
}
bool DesktopEnvironment::handle(UINT message, WPARAM wParam, LPARAM lParam) {
  if (message == WM_WTSSESSION_CHANGE) {
    if (wParam == WTS_SESSION_LOCK || wParam == WTS_REMOTE_DISCONNECT || wParam == WTS_CONSOLE_DISCONNECT) locked = true;
    if (wParam == WTS_SESSION_UNLOCK || wParam == WTS_REMOTE_CONNECT || wParam == WTS_CONSOLE_CONNECT) locked = false;
    return true;
  }
  if (message == WM_POWERBROADCAST) {
    if (wParam == PBT_APMSUSPEND) suspended = true;
    if (wParam == PBT_APMRESUMEAUTOMATIC || wParam == PBT_APMRESUMESUSPEND) suspended = false;
    if (wParam == PBT_POWERSETTINGCHANGE && lParam) {
      const auto *setting = reinterpret_cast<const POWERBROADCAST_SETTING *>(lParam);
      if (setting->PowerSetting == sessionDisplay && setting->DataLength >= sizeof(DWORD)) {
        DWORD state = 0; memcpy(&state, setting->Data, sizeof(state)); displayOff = state == 0;
      }
    }
    return true;
  }
  return false;
}
void DesktopEnvironment::refreshMonitors() {
  std::map<std::wstring, std::pair<std::wstring, std::wstring>> names;
  for (int attempt = 0; attempt < 3; ++attempt) {
    UINT32 pathsCount = 0, modesCount = 0;
    if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &pathsCount, &modesCount) != ERROR_SUCCESS || pathsCount > 256 || modesCount > 1024) break;
    std::vector<DISPLAYCONFIG_PATH_INFO> paths(pathsCount);
    std::vector<DISPLAYCONFIG_MODE_INFO> modes(modesCount);
    const auto result = QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &pathsCount, paths.data(), &modesCount, modes.data(), nullptr);
    if (result == ERROR_INSUFFICIENT_BUFFER) continue;
    if (result != ERROR_SUCCESS) break;
    for (UINT32 i = 0; i < pathsCount; ++i) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME source{};
      source.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(source), paths[i].sourceInfo.adapterId, paths[i].sourceInfo.id};
      DISPLAYCONFIG_TARGET_DEVICE_NAME target{};
      target.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(target), paths[i].targetInfo.adapterId, paths[i].targetInfo.id};
      if (DisplayConfigGetDeviceInfo(&source.header) == ERROR_SUCCESS && DisplayConfigGetDeviceInfo(&target.header) == ERROR_SUCCESS)
        names[source.viewGdiDeviceName] = {target.monitorDevicePath, target.monitorFriendlyDeviceName};
    }
    break;
  }
  struct Context { decltype(names) *names; std::vector<DesktopMonitor> monitors; } context{&names, {}};
  EnumDisplayMonitors(nullptr, nullptr, [](HMONITOR monitor, HDC, LPRECT, LPARAM data) -> BOOL {
    auto &ctx = *reinterpret_cast<Context *>(data);
    MONITORINFOEXW info{}; info.cbSize = sizeof(info);
    if (!GetMonitorInfoW(monitor, reinterpret_cast<MONITORINFO *>(&info))) return TRUE;
    DesktopMonitor item; item.handle = monitor; item.bounds = info.rcMonitor; item.work = info.rcWork;
    item.primary = (info.dwFlags & MONITORINFOF_PRIMARY) != 0;
    const auto found = ctx.names->find(info.szDevice);
    item.id = found != ctx.names->end() && !found->second.first.empty() ? found->second.first : info.szDevice;
    item.name = found != ctx.names->end() && !found->second.second.empty() ? found->second.second : info.szDevice;
    UINT x = 96, y = 96;
    if (SUCCEEDED(GetDpiForMonitor(monitor, MDT_EFFECTIVE_DPI, &x, &y))) item.dpi = x;
    ctx.monitors.push_back(std::move(item));
    return TRUE;
  }, reinterpret_cast<LPARAM>(&context));
  if (!context.monitors.empty()) m_monitors = std::move(context.monitors);
  if (m_monitors.empty()) m_monitors.push_back({nullptr, L"primary", L"主显示器", {0,0,1920,1080}, {0,0,1920,1040}, 96, true});
}
const DesktopMonitor &DesktopEnvironment::monitor(const std::wstring &id, HWND fallback) const {
  for (const auto &item : m_monitors) if (item.id == id) return item;
  if (fallback) {
    const auto handle = MonitorFromWindow(fallback, MONITOR_DEFAULTTONEAREST);
    for (const auto &item : m_monitors) if (item.handle == handle) return item;
  }
  for (const auto &item : m_monitors) if (item.primary) return item;
  return m_monitors.front();
}
bool DesktopEnvironment::fullscreenOn(HMONITOR handle, const std::wstring &exceptions) const {
  const HWND foreground = GetForegroundWindow();
  if (!foreground || !IsWindowVisible(foreground) || IsIconic(foreground)) return false;
  if (foreground == GetShellWindow() || foreground == GetDesktopWindow()) return false;
  wchar_t windowClass[128]{};
  GetClassNameW(foreground, windowClass, 128);
  if (wcscmp(windowClass, L"Progman") == 0 || wcscmp(windowClass, L"WorkerW") == 0 ||
      wcscmp(windowClass, L"Shell_TrayWnd") == 0) return false;
  DWORD cloaked = 0;
  if (SUCCEEDED(DwmGetWindowAttribute(foreground, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked) return false;
  DWORD processId = 0;
  GetWindowThreadProcessId(foreground, &processId);
  if (processId == GetCurrentProcessId()) return false;
  if (!exceptions.empty()) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (process) {
      std::wstring path(32768, L'\0'); DWORD size = DWORD(path.size());
      if (QueryFullProcessImageNameW(process, 0, path.data(), &size)) {
        path.resize(size); const auto executable = lower(std::filesystem::path(path).filename().wstring());
        std::wistringstream list(lower(exceptions)); std::wstring name;
        while (std::getline(list, name, L';')) {
          const auto begin = name.find_first_not_of(L" \t\r\n"), end = name.find_last_not_of(L" \t\r\n");
          if (begin != std::wstring::npos && name.substr(begin, end - begin + 1) == executable) { CloseHandle(process); return false; }
        }
      }
      CloseHandle(process);
    }
  }
  RECT rect{};
  if (FAILED(DwmGetWindowAttribute(foreground, DWMWA_EXTENDED_FRAME_BOUNDS, &rect, sizeof(rect)))) GetWindowRect(foreground, &rect);
  const auto style = GetWindowLongPtrW(foreground, GWL_STYLE);
  for (const auto &item : m_monitors) {
    if (item.handle != handle) continue;
    const int tolerance = MulDiv(2, item.dpi, 96);
    const bool covers = rect.left <= item.bounds.left + tolerance && rect.top <= item.bounds.top + tolerance &&
                        rect.right >= item.bounds.right - tolerance && rect.bottom >= item.bounds.bottom - tolerance;
    if (!covers) return false;
    if (!(style & WS_CAPTION)) return true;
    QUERY_USER_NOTIFICATION_STATE state{};
    return SUCCEEDED(SHQueryUserNotificationState(&state)) && state == QUNS_RUNNING_D3D_FULL_SCREEN;
  }
  return false;
}
