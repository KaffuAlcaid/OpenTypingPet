#pragma once
#include "appSettings.h"
#include "inputSource.h"
#include "petEngine.h"
#include "petInstance.h"
#include "characterLibrary.h"
#include <map>
#include <shellapi.h>
#include <thread>
#include <deque>

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
  HWND petWindow() const { return currentPet().window(); }
  PetEngine &engine() { return currentPet().engine; }
  const PetEngine &engine() const { return currentPet().engine; }
  PetInstance &currentPet() { return *m_pets[m_activePet]; }
  const PetInstance &currentPet() const { return *m_pets[m_activePet]; }
  PetSettings &petSettings() { return currentPet().record.settings; }
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
  void updateControlVisibility();
  void syncControls(unsigned changed = 0);
  void command(unsigned id, unsigned notification);
  void choosePreset();
  enum class ImportMode { Import, Add, Replace, Restore, Update, Edited };
  struct ImportJob {
    std::wstring path;
    std::string libraryId, instanceId;
    ImportMode mode = ImportMode::Import;
    std::optional<PetRecord> restore;
    uint64_t generation = 0;
    std::optional<std::set<std::string>> disabledRules;
    bool temporary = false;
  };
  struct ImportCompletion { ImportResult result; ImportJob job; };
  void finishImport(ImportCompletion *result);
  void queueImport(ImportJob job);
  void startImport();
  PetInstance &createPet(const PetRecord &record);
  void selectPet(size_t index);
  void closePet(std::string id);
  void refreshLibrary();
  void refreshInstances();
  void saveState();
  void applyEnvironment();
  void showDiagnostics();
  void editCharacter();
  void editBinding();
  void libraryCommand(unsigned id);
  void paintLibraryEntry(const DRAWITEMSTRUCT &draw);
  void paintSelector(const DRAWITEMSTRUCT &draw);
  void onAction();
  void onTimer();
  void schedule();
  void configureInput();
  void setWindowOptions();
  void ensureOnScreen();
  void savePosition();
  void updateTray(bool add = false);
  void paintButton(const DRAWITEMSTRUCT &draw);
  void paintControl(const DRAWITEMSTRUCT &draw);
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
  bool m_interactive = true, m_busy = false, m_capturingInput = false,
       m_quitting = false, m_syncing = false, m_environmentPending = false;
  HWND m_settingsWindow = nullptr;
  HWND m_tooltips = nullptr;
  Preferences m_preferences;
  ThemeColors m_colors{};
  DesktopEnvironment m_environment;
  std::unique_ptr<CharacterLibrary> m_library;
  std::vector<std::unique_ptr<PetInstance>> m_pets;
  size_t m_activePet = 0;
  int m_selectedLibrary = -1;
  uint64_t m_importGeneration = 1;
  std::deque<ImportJob> m_importQueue;
  std::optional<ImportJob> m_failedImport;
  std::vector<ImportDiagnostic> m_diagnostics;
  InputSource m_input;
  std::thread m_worker;
  std::map<unsigned, HWND> m_controls;
  std::map<unsigned, RECT> m_controlBounds;
  std::map<unsigned, std::wstring> m_tooltipText;
  std::map<int, std::shared_ptr<Gdiplus::Bitmap>> m_icons;
  std::vector<MenuItem> m_menuItems;
  HFONT m_font = nullptr, m_headingFont = nullptr, m_titleFont = nullptr;
  HBRUSH m_baseBrush = nullptr, m_windowBrush = nullptr;
  HICON m_trayIcon = nullptr;
  UINT m_taskbarMessage = 0;
  int m_tab = 0;
  unsigned m_dpi = 96;
  size_t m_previewAction = 0, m_previewFrame = 0;
  unsigned m_previewLoops = 0;
  uint64_t m_previewAt = 0;
  bool m_previewPlaying = true, m_loginStartup = false;
  double m_previewSpeed = 1.0;
  int m_previewBackground = 0;
  std::wstring m_error;
};
