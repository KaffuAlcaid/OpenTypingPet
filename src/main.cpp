#include "appController.h"
#include "resourceIds.h"
#include <commctrl.h>
#include <memory>
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
  Gdiplus::GdiplusStartupInput graphicsInput;
  ULONG_PTR graphicsToken = 0;
  if (Gdiplus::GdiplusStartup(&graphicsToken, &graphicsInput, nullptr) !=
      Gdiplus::Ok)
    return 1;
  INITCOMMONCONTROLSEX controls{sizeof(controls),
                                ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
  InitCommonControlsEx(&controls);
  int count = 0;
  LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
  std::wstring initial;
  bool automatic = false;
  if (arguments) {
    for (int i = 1; i < count; ++i)
      if (std::wstring_view(arguments[i]) == L"--autostart" ||
          std::wstring_view(arguments[i]) == L"--background")
        automatic = true;
      else if (initial.empty())
        initial = arguments[i];
    LocalFree(arguments);
  }
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenTypingPet.SingleInstance");
  if (!single) {
    Gdiplus::GdiplusShutdown(graphicsToken);
    CoUninitialize();
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS ||
      FindWindowW(nullptr, L"OpenTypingPet")) {
    if (!automatic || !initial.empty()) {
      HWND window = nullptr;
      for (int attempt = 0; attempt < 50 && !window; ++attempt) {
        window = FindWindowW(L"OpenTypingPetSettings", nullptr);
        if (!window)
          Sleep(20);
      }
      if (window) {
        DWORD owner = 0;
        GetWindowThreadProcessId(window, &owner);
        AllowSetForegroundWindow(owner);
        COPYDATASTRUCT data{0x4F545050, DWORD((initial.size() + 1) * sizeof(wchar_t)), initial.data()};
        DWORD_PTR result = 0;
        SendMessageTimeoutW(window, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data),
                             SMTO_ABORTIFHUNG | SMTO_BLOCK, 2000, &result);
      }
    }
    if (single)
      CloseHandle(single);
    Gdiplus::GdiplusShutdown(graphicsToken);
    CoUninitialize();
    return 0;
  }
  int exitCode = 1;
  {
    AppController app(instance);
    if (app.initialize()) {
      if (!app.preferences().initialized || !initial.empty() ||
          app.preferences().showSettingsOnStart)
        app.openSettings();
      app.restore(initial);
      MSG message{};
      while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (!IsDialogMessageW(app.settingsWindow(), &message)) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
      }
      exitCode = int(message.wParam);
    }
  }
  if (single) {
    ReleaseMutex(single);
    CloseHandle(single);
  }
  Gdiplus::GdiplusShutdown(graphicsToken);
  CoUninitialize();
  return exitCode;
}
