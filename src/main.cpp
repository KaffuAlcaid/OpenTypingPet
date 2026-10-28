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
  HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\OpenTypingPet.SingleInstance");
  if (!single) {
    Gdiplus::GdiplusShutdown(graphicsToken);
    CoUninitialize();
    return 1;
  }
  if (GetLastError() == ERROR_ALREADY_EXISTS ||
      FindWindowW(nullptr, L"OpenTypingPet")) {
    MessageBoxW(nullptr,
                L"OpenTypingPet 已在运行。请从系统托盘打开设置。",
                L"OpenTypingPet", MB_OK | MB_ICONINFORMATION);
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
      int count = 0;
      LPWSTR *arguments = CommandLineToArgvW(GetCommandLineW(), &count);
      const std::wstring initial = count > 1 ? arguments[1] : L"";
      if (arguments)
        LocalFree(arguments);
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
