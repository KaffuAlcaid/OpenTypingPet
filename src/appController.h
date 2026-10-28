#pragma once
#include "appSettings.h"
#include "inputSource.h"
#include "petEngine.h"
#include <map>
#include <shellapi.h>
#include <thread>

class AppController {
public:
  explicit AppController(HINSTANCE instance, bool interactive = true);
  ~AppController();
  bool initialize();
  void restore(const std::wstring &path);
  void importPreset(const std::wstring &path);
  void openSettings();
  void pause(bool value);
  void applyTheme();
  void changeTheme(int mode);
  void paintSettings(HDC dc);
  void renderPet();
  void contextMenu(POINT point);
  void resetPosition();
  void quit();
  HWND settingsWindow() const { return m_settingsWindow; }
  HWND petWindow() const { return m_petWindow; }
  PetEngine &engine() { return m_engine; }
  Preferences &preferences() { return m_preferences; }
  const ThemeColors &colors() const { return m_colors; }
  bool busy() const { return m_busy; }
  const std::wstring &error() const { return m_error; }

private:
  struct MenuItem {
    unsigned id;
    std::wstring label;
    int icon = 0;
    bool checked = false;
  };
  static LRESULT CALLBACK settingsProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK petProc(HWND, UINT, WPARAM, LPARAM);
  static LRESULT CALLBACK editProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                   DWORD_PTR);
  static LRESULT CALLBACK listProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR,
                                   DWORD_PTR);
  static LRESULT CALLBACK sliderProc(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
  void paintSlider(HWND window, HDC dc);
  void paintScrollBar(HWND window, HDC dc = nullptr);
  LRESULT message(UINT, WPARAM, LPARAM);
  void createControls();
  HWND control(const wchar_t *type, const wchar_t *text, unsigned id,
               DWORD style);
  void layout();
  void syncControls();
  void command(unsigned id, unsigned notification);
  void choosePreset();
  void finishImport(ImportResult *result);
  void onAction();
  void onTimer();
  void schedule();
  void configureInput();
  void setWindowOptions();
  void ensureOnScreen();
  void savePosition();
  void updateTray(bool add = false);
  void paintButton(const DRAWITEMSTRUCT &draw);
  void paintAction(const DRAWITEMSTRUCT &draw);
  void paintPreview(HDC dc);
  void selectPreview(size_t action);
  void invalidatePreview();
  void updateTooltips();
  void paintMenu(const DRAWITEMSTRUCT &draw);
  HMENU createMenu();
  void icon(Gdiplus::Graphics &graphics, int id, Gdiplus::Rect rect,
            COLORREF color);
  void drawFrame(Gdiplus::Graphics &graphics, Gdiplus::Rect rect,
                 const AnimationClip &clip, size_t frame, bool smooth);
  int px(int logical) const;
  HINSTANCE m_instance;
  bool m_interactive = true, m_busy = false, m_paused = false,
       m_dragging = false, m_quitting = false, m_syncing = false;
  HWND m_settingsWindow = nullptr, m_petWindow = nullptr;
  HWND m_tooltips = nullptr;
  Preferences m_preferences;
  ThemeColors m_colors{};
  PetEngine m_engine;
  InputSource m_input;
  std::thread m_worker;
  std::map<unsigned, HWND> m_controls;
  std::map<unsigned, std::wstring> m_tooltipText;
  std::map<int, std::shared_ptr<Gdiplus::Bitmap>> m_icons;
  std::vector<MenuItem> m_menuItems;
  HFONT m_font = nullptr, m_headingFont = nullptr, m_titleFont = nullptr;
  HBRUSH m_baseBrush = nullptr, m_windowBrush = nullptr;
  HICON m_trayIcon = nullptr;
  UINT m_taskbarMessage = 0;
  int m_tab = 0;
  unsigned m_dpi = 96;
  size_t m_frame = 0;
  int m_loops = 0;
  uint64_t m_frameAt = 0;
  size_t m_previewAction = 0, m_previewFrame = 0;
  unsigned m_previewLoops = 0;
  uint64_t m_previewAt = 0;
  bool m_previewPlaying = true, m_loginStartup = false;
  int m_previewBackground = 0;
  POINT m_dragOrigin{}, m_cursorOrigin{};
  std::wstring m_error;
  HDC m_petDC = nullptr;
  HBITMAP m_petBitmap = nullptr, m_oldPetBitmap = nullptr;
  void *m_petPixels = nullptr;
  SIZE m_petSize{};
};
