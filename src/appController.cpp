#include "appController.h"
#include "presetLoader.h"
#include "resourceIds.h"
#include "storageUtils.h"
#include "animationRenderer.h"
#include "characterEditor.h"
#include "nativeDialogs.h"
#include <algorithm>
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>
#include <limits>
#include <shlwapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>

namespace {
enum : unsigned {
  Open = 1000,
  Pause,
  Reset,
  Exit,
  Settings,
  Topmost,
  Locked,
  TabPet = 1010,
  TabActions,
  TabApp,
  TabLibrary,
  ThemeSystem = 1020,
  ThemeLight,
  ThemeDark,
  WidthSlider = 1030,
  WidthEdit,
  WidthMinus,
  WidthPlus,
  OpacitySlider,
  PixelMode,
  SmoothMode,
  IdleEdit = 1040,
  IdleMinus,
  IdlePlus,
  HoldEdit,
  HoldMinus,
  HoldPlus,
  Actions = 1050,
  StartSettings,
  LoginStartup,
  PreviewPlay,
  PreviewChecker = 1060,
  PreviewLight,
  PreviewDark,
  PetSelector = 1070,
  MonitorSelector,
  Snap,
  Visible,
  ClosePet,
  InheritDefaults,
  SaveDefaults,
  PauseAll,
  PauseLockOption,
  PauseDisplayOff,
  HideFullscreen,
  FullscreenExceptions,
  LibraryList = 1100,
  LibraryAdd,
  LibraryReplace,
  LibraryEdit,
  LibraryExport,
  LibraryRemove,
  LibraryFolder,
  LibraryRestore,
  CancelImport,
  RetryImport,
  ImportDetails,
  BindAction = 1120,
  ClearBindings,
  PreviewPrevious,
  PreviewNext,
  PreviewTimeline,
  PreviewSpeed,
  LibraryNew
};
constexpr UINT Imported = WM_APP + 1, TrayEvent = WM_APP + 2, RemovePetMessage = WM_APP + 5;
constexpr UINT_PTR AnimationTimer = 1, EnvironmentTimer = 2, EnvironmentDebounceTimer = 3;
constexpr unsigned SelectPetMenu = 2000;
constexpr int SidebarWidth = 176;
Gdiplus::Color color(COLORREF value, BYTE alpha = 255) {
  return Gdiplus::Color(alpha, GetRValue(value), GetGValue(value),
                        GetBValue(value));
}
void fill(HDC dc, RECT rect, COLORREF value) {
  HBRUSH brush = CreateSolidBrush(value);
  FillRect(dc, &rect, brush);
  DeleteObject(brush);
}
void text(HDC dc, const std::wstring &value, RECT rect, HFONT font,
          COLORREF foreground,
          UINT format = DT_LEFT | DT_VCENTER | DT_SINGLELINE |
                        DT_END_ELLIPSIS) {
  const auto old = SelectObject(dc, font);
  SetBkMode(dc, TRANSPARENT);
  SetTextColor(dc, foreground);
  DrawTextW(dc, value.c_str(), int(value.size()), &rect, format);
  SelectObject(dc, old);
}
void setControlText(HWND window, const std::wstring &value) {
  const auto length = GetWindowTextLengthW(window);
  std::wstring current(size_t(length) + 1, L'\0');
  GetWindowTextW(window, current.data(), length + 1);
  current.resize(size_t(length));
  if (current != value) SetWindowTextW(window, value.c_str());
}
std::wstring describeTrigger(const InputRule &rule) {
  if (rule.event == InputEventType::Typing) return L"普通打字";
  std::wstring key;
  switch (rule.key) {
  case VK_LBUTTON: key = L"鼠标左键"; break;
  case VK_RBUTTON: key = L"鼠标右键"; break;
  case VK_MBUTTON: key = L"鼠标中键"; break;
  case VK_XBUTTON1: key = L"鼠标侧键 1"; break;
  case VK_XBUTTON2: key = L"鼠标侧键 2"; break;
  default: key = Storage::wide(inputKeyName(rule.key)); break;
  }
  const auto combination = std::wstring(rule.ctrl ? L"Ctrl+" : L"") +
      (rule.shift ? L"Shift+" : L"") + (rule.alt ? L"Alt+" : L"") + key;
  if (rule.modifierTap) return L"单独按下并松开 " + combination;
  if (rule.event == InputEventType::KeyUp || rule.event == InputEventType::MouseUp)
    return L"松开 " + combination;
  return L"按 " + combination;
}
std::shared_ptr<Gdiplus::Bitmap> resourceImage(HINSTANCE instance, int id) {
  const auto resource =
      FindResourceW(instance, MAKEINTRESOURCEW(id), RT_RCDATA);
  if (!resource)
    return {};
  const auto data = LoadResource(instance, resource);
  IStream *stream =
      SHCreateMemStream(static_cast<const BYTE *>(LockResource(data)),
                        SizeofResource(instance, resource));
  if (!stream)
    return {};
  auto source =
      std::unique_ptr<Gdiplus::Bitmap>(Gdiplus::Bitmap::FromStream(stream));
  std::shared_ptr<Gdiplus::Bitmap> result;
  if (source && source->GetLastStatus() == Gdiplus::Ok) {
    result.reset(source->Clone(0, 0, source->GetWidth(), source->GetHeight(),
                               PixelFormat32bppPARGB));
  }
  source.reset();
  stream->Release();
  return result;
}
} // namespace

AppController::AppController(HINSTANCE instance, bool interactive)
    : m_instance(instance), m_interactive(interactive) {
  m_preferences.load();
  m_loginStartup = loginStartupEnabled();
  m_colors = themeColors(m_preferences.theme);
  m_library = std::make_unique<CharacterLibrary>(std::filesystem::path(PresetLoader::storageRoot()).parent_path());
  m_pets.push_back(std::make_unique<PetInstance>(instance, m_environment, interactive));
  m_pets.front()->record.settings = static_cast<const PetSettings &>(m_preferences);
  m_input.input = [this](const InputEvent &event) {
    if (m_capturingInput) return;
    for (const auto &pet : m_pets) if (pet->acceptsInput()) pet->engine.handleEvent(event);
    schedule();
  };
}
AppController::~AppController() {
  m_input.setEnabled(false, false);
  if (m_worker.joinable()) m_worker.join();
  if (m_interactive && m_settingsWindow) {
    NOTIFYICONDATAW data{sizeof(data)}; data.hWnd = m_settingsWindow; data.uID = 1; Shell_NotifyIconW(NIM_DELETE, &data);
  }
  m_pets.clear();
  if (m_settingsWindow) DestroyWindow(m_settingsWindow);
  for (auto font : {m_font, m_headingFont, m_titleFont}) if (font) DeleteObject(font);
  if (m_baseBrush) DeleteObject(m_baseBrush);
  if (m_windowBrush) DeleteObject(m_windowBrush);
  if (m_trayIcon) DestroyIcon(m_trayIcon);
}
int AppController::px(int logical) const { return MulDiv(logical, m_dpi, 96); }
bool AppController::initialize() {
  m_dpi = GetDpiForSystem();
  WNDCLASSEXW cls{sizeof(cls)};
  cls.style = CS_HREDRAW | CS_VREDRAW;
  cls.lpfnWndProc = settingsProc;
  cls.hInstance = m_instance;
  cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
  cls.hIcon = LoadIconW(m_instance, MAKEINTRESOURCEW(IDI_APP));
  cls.hIconSm = cls.hIcon;
  cls.lpszClassName = L"OpenTypingPetSettings";
  RegisterClassExW(&cls);
  RECT rect{0, 0, px(920), px(620)};
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, m_dpi);
  m_settingsWindow = CreateWindowExW(
      0, cls.lpszClassName, L"OpenTypingPet",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
      m_instance, this);
  if (!m_settingsWindow)
    return false;
  m_dpi = GetDpiForWindow(m_settingsWindow);
  m_environment.attach(m_settingsWindow, m_interactive);
  auto initial = m_pets.front()->record;
  m_pets.clear();
  createPet(initial);
  for (int id = PNG_PAW; id <= PNG_CLOSE; ++id)
    m_icons[id] = resourceImage(m_instance, id);
  createControls();
  applyTheme();
  syncControls();
  layout();
  currentPet().restorePlacement();
  m_taskbarMessage = RegisterWindowMessageW(L"TaskbarCreated");
  updateTray(true);
  DragAcceptFiles(m_settingsWindow, TRUE);
  return true;
}
HWND AppController::control(const wchar_t *type, const wchar_t *label,
                            unsigned id, DWORD style) {
  HWND window = CreateWindowExW(
      0, type, label, WS_CHILD | WS_VISIBLE | WS_TABSTOP | style, 0, 0, 0, 0,
      m_settingsWindow, reinterpret_cast<HMENU>(UINT_PTR(id)), m_instance,
      nullptr);
  m_controls[id] = window;
  return window;
}
void AppController::createControls() {
  for (auto entry : std::vector<std::pair<unsigned, const wchar_t *>>{
           {Open, L"导入角色包"}, {Pause, L"暂停响应"}, {Reset, L"重置位置"},
           {Topmost, L"始终置顶"}, {Locked, L"锁定并穿透鼠标"},
           {TabPet, L"桌宠"}, {TabActions, L"动作"}, {TabApp, L"应用"}, {TabLibrary, L"角色库"},
           {ThemeSystem, L"跟随系统"}, {ThemeLight, L"浅色"}, {ThemeDark, L"深色"},
           {PixelMode, L"像素"}, {SmoothMode, L"平滑"},
           {WidthMinus, L"−"}, {WidthPlus, L"+"}, {IdleMinus, L"−"}, {IdlePlus, L"+"},
           {HoldMinus, L"−"}, {HoldPlus, L"+"},
           {StartSettings, L"启动时打开设置"}, {LoginStartup, L"登录 Windows 后启动"},
           {PreviewPlay, L"暂停预览"}, {PreviewChecker, L"透明网格"},
           {PreviewLight, L"浅色背景"}, {PreviewDark, L"深色背景"},
           {Snap, L"贴边吸附"}, {Visible, L"显示桌宠"}, {ClosePet, L"关闭当前桌宠"},
           {InheritDefaults, L"使用角色默认设置"}, {SaveDefaults, L"保存为角色默认"}, {PauseAll, L"全部暂停"},
           {PauseLockOption, L"锁屏时暂停"}, {PauseDisplayOff, L"息屏时暂停"}, {HideFullscreen, L"全屏应用时隐藏并暂停"},
           {FullscreenExceptions, L"应用例外"}, {LibraryAdd, L"添加到桌面"}, {LibraryReplace, L"替换当前桌宠"},
           {LibraryEdit, L"编辑角色"}, {LibraryExport, L"导出 ZIP"}, {LibraryRemove, L"移除角色"},
           {LibraryFolder, L"打开目录"}, {LibraryRestore, L"恢复原始版本"}, {LibraryNew, L"新建角色"},
           {CancelImport, L"取消导入"}, {RetryImport, L"重新导入"}, {ImportDetails, L"查看详情"},
           {BindAction, L"设置触发"}, {ClearBindings, L"恢复角色键位"},
           {PreviewPrevious, L"上一帧"}, {PreviewNext, L"下一帧"}})
    control(WC_BUTTONW, entry.second, entry.first, BS_OWNERDRAW | BS_NOTIFY);
  for (auto id : {WidthEdit, IdleEdit, HoldEdit}) {
    auto window = control(WC_EDITW, L"", id, ES_CENTER | ES_NUMBER | WS_BORDER);
    SendMessageW(window, EM_SETLIMITTEXT, 5, 0);
    SetWindowSubclass(window, editProc, id, reinterpret_cast<DWORD_PTR>(this));
    SetWindowTheme(window, L"", L"");
  }
  for (auto id : {WidthSlider, OpacitySlider, PreviewTimeline}) {
    auto window = control(TRACKBAR_CLASSW, L"", id, TBS_NOTICKS | TBS_HORZ);
    SetWindowTheme(window, L"", L"");
    SendMessageW(window, TBM_SETRANGEMIN, FALSE, id == WidthSlider ? 100 : id == OpacitySlider ? 20 : 0);
    SendMessageW(window, TBM_SETRANGEMAX, FALSE, id == WidthSlider ? 800 : id == OpacitySlider ? 100 : 1);
    SendMessageW(window, TBM_SETPAGESIZE, 0, 10);
    SetWindowSubclass(window, sliderProc, id, reinterpret_cast<DWORD_PTR>(this));
  }
  control(WC_LISTBOXW, L"", Actions,
          LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL);
  SetWindowSubclass(m_controls[Actions], listProc, Actions, reinterpret_cast<DWORD_PTR>(this));
  control(WC_LISTBOXW, L"", LibraryList, LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | LBS_NOINTEGRALHEIGHT | WS_VSCROLL);
  SetWindowSubclass(m_controls[LibraryList], listProc, LibraryList, reinterpret_cast<DWORD_PTR>(this));
  for (unsigned id : {PetSelector, MonitorSelector, PreviewSpeed}) {
    control(WC_COMBOBOXW, L"", id, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL);
    SetWindowTheme(m_controls[id], L"", L"");
    SendMessageW(m_controls[id], CB_SETDROPPEDWIDTH, px(id == PetSelector ? 240 : 200), 0);
  }
  for (const auto *speed : {L"0.25×", L"0.5×", L"1×", L"2×"})
    SendMessageW(m_controls[PreviewSpeed], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(speed));
  SendMessageW(m_controls[PreviewSpeed], CB_SETCURSEL, 2, 0);
  m_tooltips = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
                               WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
                               CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                               m_settingsWindow, nullptr, m_instance, nullptr);
  updateTooltips();
}
void AppController::updateTooltips() {
  if (!m_tooltips)
    return;
  const std::map<unsigned, std::wstring> tips = {
      {Pause, currentPet().record.paused ? L"继续响应输入" : L"暂停响应输入"},
      {PreviewPlay, m_previewPlaying ? L"暂停预览" : L"播放预览"},
      {PreviewPrevious, L"上一帧"}, {PreviewNext, L"下一帧"},
      {PreviewChecker, L"透明网格"}, {PreviewLight, L"浅色背景"}, {PreviewDark, L"深色背景"},
      {WidthMinus, L"减小宽度"}, {WidthPlus, L"增大宽度"},
      {IdleMinus, L"缩短待机延迟"}, {IdlePlus, L"延长待机延迟"},
      {HoldMinus, L"缩短动作保持"}, {HoldPlus, L"延长动作保持"}};
  for (const auto &[id, tip] : tips) {
    const bool exists = m_tooltipText.contains(id);
    m_tooltipText[id] = tip;
    TOOLINFOW tool{sizeof(tool)};
    tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
    tool.hwnd = m_settingsWindow;
    tool.uId = reinterpret_cast<UINT_PTR>(m_controls[id]);
    tool.lpszText = m_tooltipText[id].data();
    SendMessageW(m_tooltips, exists ? TTM_UPDATETIPTEXTW : TTM_ADDTOOLW, 0,
                   reinterpret_cast<LPARAM>(&tool));
    if (id == Pause || id == PreviewPlay || (id >= PreviewChecker && id <= PreviewDark))
      setControlText(m_controls[id], tip);
  }
}
void AppController::layout() {
  if (m_controls.empty())
    return;
  RECT rect{};
  GetClientRect(m_settingsWindow, &rect);
  const int w = MulDiv(rect.right, 96, m_dpi), h = MulDiv(rect.bottom, 96, m_dpi);
  const int left = SidebarWidth + 24, right = w - 288, span = right - left, preview = w - 264;
  auto place = [&](unsigned id, int x, int y, int width, int height) {
    const RECT bounds{px(x), px(y), px(x + width), px(y + height)};
    const auto previous = m_controlBounds.find(id);
    if (previous != m_controlBounds.end() && EqualRect(&previous->second, &bounds)) return;
    m_controlBounds[id] = bounds;
    MoveWindow(m_controls[id], bounds.left, bounds.top, bounds.right - bounds.left, bounds.bottom - bounds.top, FALSE);
    InvalidateRect(m_controls[id], nullptr, FALSE);
  };
  place(Open, w - 202, 22, 136, 34);
  place(Pause, w - 58, 22, 34, 34);
  const unsigned tabs[] = {TabLibrary, TabPet, TabActions, TabApp};
  for (int i = 0; i < 4; ++i) place(tabs[i], 12, 92 + i * 48, SidebarWidth - 24, 40);
  place(PetSelector, 12, 326, SidebarWidth - 24, 200);
  place(PauseAll, 12, h - 130, SidebarWidth - 24, 34);
  place(ClosePet, 12, h - 86, SidebarWidth - 24, 34);
  const int half = (span - 12) / 2;
  place(Topmost, left, 132, half, 34);
  place(Visible, left + half + 12, 132, half, 34);
  place(Locked, left, 176, span, 34);
  place(Snap, left, 220, span, 34);
  place(WidthSlider, left + 68, 276, span - 194, 34);
  place(WidthMinus, right - 118, 276, 28, 34);
  place(WidthEdit, right - 90, 276, 62, 34);
  place(WidthPlus, right - 28, 276, 28, 34);
  place(OpacitySlider, left + 68, 322, span - 128, 34);
  place(PixelMode, left + 68, 368, 80, 34);
  place(SmoothMode, left + 152, 368, 80, 34);
  place(MonitorSelector, left + 68, 414, span - 68, 180);
  place(InheritDefaults, left, 458, span, 32);
  place(Reset, left, 504, half, 34);
  place(SaveDefaults, left + half + 12, 504, half, 34);
  place(Actions, left, 94, span, h - 340);
  if (SendMessageW(m_controls[Actions], LB_GETITEMHEIGHT, 0, 0) != px(64))
    SendMessageW(m_controls[Actions], LB_SETITEMHEIGHT, 0, px(64));
  place(BindAction, left, h - 234, half, 34);
  place(ClearBindings, left + half + 12, h - 234, half, 34);
  place(IdleMinus, right - 154, h - 150, 30, 34);
  place(IdleEdit, right - 124, h - 150, 64, 34);
  place(IdlePlus, right - 60, h - 150, 30, 34);
  place(HoldMinus, right - 154, h - 102, 30, 34);
  place(HoldEdit, right - 124, h - 102, 64, 34);
  place(HoldPlus, right - 60, h - 102, 30, 34);
  for (int i = 0; i < 3; ++i) place(ThemeSystem + i, w - 294 + i * 90, 104, 86, 34);
  place(StartSettings, left, 198, w - left - 24, 34);
  place(LoginStartup, left, 242, w - left - 24, 34);
  place(PauseLockOption, left, 330, w - left - 24, 34);
  place(PauseDisplayOff, left, 374, w - left - 24, 34);
  place(HideFullscreen, left, 418, w - left - 24, 34);
  place(FullscreenExceptions, left, 466, 132, 34);
  for (int i = 0; i < 3; ++i)
    place(PreviewChecker + i, preview + 52 + i * 36, 464, 28, 28);
  place(PreviewTimeline, preview, 498, 224, 24);
  place(PreviewPrevious, preview, 528, 32, 32);
  place(PreviewPlay, preview + 38, 528, 32, 32);
  place(PreviewNext, preview + 76, 528, 32, 32);
  place(PreviewSpeed, preview + 124, 528, 100, 180);
  place(LibraryList, left, 94, span, h - 218);
  if (SendMessageW(m_controls[LibraryList], LB_GETITEMHEIGHT, 0, 0) != px(72))
    SendMessageW(m_controls[LibraryList], LB_SETITEMHEIGHT, 0, px(72));
  place(LibraryNew, left, h - 108, half, 34);
  place(LibraryRemove, left + half + 12, h - 108, half, 34);
  const unsigned libraryButtons[] = {LibraryAdd, LibraryReplace, LibraryEdit, LibraryExport, LibraryFolder, LibraryRestore};
  for (int i = 0; i < 6; ++i) place(libraryButtons[i], preview + (i % 2) * 116, 370 + (i / 2) * 46, 108, 34);
  place(CancelImport, w - 118, h - 30, 94, 26);
  place(RetryImport, w - 230, h - 30, 98, 26);
  place(ImportDetails, w - 124, h - 30, 100, 26);
  updateControlVisibility();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
  schedule();
}
void AppController::updateControlVisibility() {
  for (auto [id, window] : m_controls) {
    bool visible = true;
    if (id == Topmost || id == Locked || id == Reset || id == Snap || id == Visible ||
        id == MonitorSelector || id == InheritDefaults || id == SaveDefaults ||
        (id >= WidthSlider && id <= SmoothMode))
      visible = m_tab == 0;
    if (id == Actions || id == PreviewPlay || id == BindAction || id == ClearBindings ||
        (id >= PreviewPrevious && id <= PreviewSpeed) || (id >= IdleEdit && id <= HoldPlus))
      visible = m_tab == 1;
    if ((id >= ThemeSystem && id <= ThemeDark) || id == StartSettings || id == LoginStartup ||
        (id >= PauseLockOption && id <= FullscreenExceptions))
      visible = m_tab == 2;
    if (id >= PreviewChecker && id <= PreviewDark)
      visible = m_tab == 0 || m_tab == 1;
    if ((id >= LibraryList && id <= LibraryRestore) || id == LibraryNew) visible = m_tab == 3;
    if (id == CancelImport) visible = m_busy;
    if (id == RetryImport) visible = !m_busy && m_failedImport.has_value();
    if (id == ImportDetails) visible = !m_busy && !m_error.empty();
    if (visible != bool(GetWindowLongPtrW(window, GWL_STYLE) & WS_VISIBLE))
      ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
  }
}
void AppController::syncControls(unsigned changed) {
  if (m_controls.empty())
    return;
  m_syncing = true;
  setControlText(m_controls[WidthEdit], std::to_wstring(petSettings().width));
  setControlText(m_controls[IdleEdit], std::to_wstring(petSettings().idleDelay));
  setControlText(m_controls[HoldEdit], std::to_wstring(petSettings().minimumHold));
  if (SendMessageW(m_controls[WidthSlider], TBM_GETPOS, 0, 0) != petSettings().width)
    SendMessageW(m_controls[WidthSlider], TBM_SETPOS, TRUE, petSettings().width);
  if (SendMessageW(m_controls[OpacitySlider], TBM_GETPOS, 0, 0) != petSettings().opacity)
    SendMessageW(m_controls[OpacitySlider], TBM_SETPOS, TRUE, petSettings().opacity);
  EnableWindow(m_controls[Open], !m_busy);
  EnableWindow(m_controls[Pause], engine().character() != nullptr);
  EnableWindow(m_controls[Reset], engine().character() != nullptr);
  EnableWindow(m_controls[PreviewPlay], engine().character() != nullptr);
  const bool hasPet = engine().character() != nullptr;
  for (unsigned id : {ClosePet, SaveDefaults, InheritDefaults, BindAction, ClearBindings, MonitorSelector})
    EnableWindow(m_controls[id], hasPet);
  const bool hasEntry = m_selectedLibrary >= 0 && size_t(m_selectedLibrary) < m_library->entries.size();
  for (unsigned id : {LibraryAdd, LibraryReplace, LibraryEdit, LibraryExport, LibraryRemove, LibraryFolder, LibraryRestore})
    EnableWindow(m_controls[id], hasEntry && !m_busy);
  EnableWindow(m_controls[LibraryAdd], hasEntry && !m_busy && (m_pets.size() < 4 || !hasPet));
  EnableWindow(m_controls[LibraryRestore], hasEntry && !m_busy &&
      m_library->entries[m_selectedLibrary].currentHash != m_library->entries[m_selectedLibrary].sourceHash);
  EnableWindow(m_controls[LibraryNew], !m_busy);
  setControlText(m_controls[PauseAll], m_library->allPaused ? L"全部继续" : L"全部暂停");
  bool animated = false;
  if (const auto character = engine().character(); character && m_previewAction < character->actions.size())
    animated = character->animation(m_previewAction).frames.size() > 1;
  for (unsigned id : {PreviewPlay, PreviewPrevious, PreviewNext, PreviewTimeline, PreviewSpeed})
    EnableWindow(m_controls[id], animated);
  updateTooltips();
  for (auto [id, window] : m_controls)
    if (!changed || id == changed || id == InheritDefaults ||
        ((changed == PixelMode || changed == SmoothMode) && (id == PixelMode || id == SmoothMode)))
      InvalidateRect(window, nullptr, FALSE);
  updateControlVisibility();
  m_syncing = false;
  RECT content{}; GetClientRect(m_settingsWindow, &content); content.left = px(SidebarWidth);
  if (changed) content.top = content.bottom - px(32);
  InvalidateRect(m_settingsWindow, &content, FALSE);
}
void AppController::applyTheme() {
  m_colors = themeColors(m_preferences.theme);
  if (m_baseBrush)
    DeleteObject(m_baseBrush);
  if (m_windowBrush)
    DeleteObject(m_windowBrush);
  m_baseBrush = CreateSolidBrush(m_colors.base);
  m_windowBrush = CreateSolidBrush(m_colors.window);
  for (auto font : {m_font, m_headingFont, m_titleFont})
    if (font)
      DeleteObject(font);
  auto makeFont = [&](int size, int weight) {
    return CreateFontW(-px(size), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
  };
  m_font = makeFont(14, FW_NORMAL);
  m_headingFont = makeFont(16, FW_SEMIBOLD);
  m_titleFont = makeFont(22, FW_SEMIBOLD);
  for (auto [id, window] : m_controls)
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
  for (unsigned id : {PetSelector, MonitorSelector, PreviewSpeed})
    styleCombo(m_controls[id], m_colors, m_font, m_dpi);
  BOOL dark = m_colors.dark;
  DwmSetWindowAttribute(m_settingsWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  if (m_tooltips) {
    SetWindowTheme(m_tooltips, L"", L"");
    SendMessageW(m_tooltips, TTM_SETTIPBKCOLOR, m_colors.base, 0);
    SendMessageW(m_tooltips, TTM_SETTIPTEXTCOLOR, m_colors.text, 0);
  }
  RedrawWindow(m_settingsWindow, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_UPDATENOW);
}
void AppController::changeTheme(int mode) {
  m_preferences.theme = std::clamp(mode, 0, 2);
  applyTheme();
  syncControls();
  if (m_interactive)
    m_preferences.save();
}
void AppController::icon(Gdiplus::Graphics &graphics, int id,
                         Gdiplus::Rect rect, COLORREF foreground) {
  auto image = m_icons[id];
  if (!image)
    return;
  Gdiplus::ColorMatrix matrix = {
      {{0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0},
       {0, 0, 0, 0, 0},
       {0, 0, 0, 1, 0},
       {GetRValue(foreground) / 255.f, GetGValue(foreground) / 255.f,
        GetBValue(foreground) / 255.f, 0, 1}}};
  Gdiplus::ImageAttributes attributes;
  attributes.SetColorMatrix(&matrix);
  graphics.SetInterpolationMode(Gdiplus::InterpolationModeHighQualityBicubic);
  graphics.DrawImage(image.get(), rect, 0, 0, image->GetWidth(),
                     image->GetHeight(), Gdiplus::UnitPixel, &attributes);
}
void AppController::drawFrame(Gdiplus::Graphics &graphics, Gdiplus::Rect rect,
                              const AnimationClip &clip, size_t frame,
                              bool smooth) {
  if (clip.frames.empty())
    return;
  frame = std::min(frame, clip.frames.size() - 1);
  const double scale = std::min(double(rect.Width) / clip.width,
                                double(rect.Height) / clip.height);
  const int w = int(std::round(clip.width * scale)),
            h = int(std::round(clip.height * scale));
  const Gdiplus::Rect target(rect.X + (rect.Width - w) / 2,
                             rect.Y + (rect.Height - h) / 2, w, h);
  graphics.SetInterpolationMode(
      smooth ? Gdiplus::InterpolationModeHighQualityBicubic
             : Gdiplus::InterpolationModeNearestNeighbor);
  graphics.SetPixelOffsetMode(Gdiplus::PixelOffsetModeHalf);
  Gdiplus::ImageAttributes attributes;
  attributes.SetWrapMode(Gdiplus::WrapModeTileFlipXY);
  graphics.DrawImage(clip.frames[frame].image.get(), target, 0, 0, clip.width,
                     clip.height, Gdiplus::UnitPixel, &attributes);
}
void AppController::paintSettings(HDC dc) {
  RECT client{};
  GetClientRect(m_settingsWindow, &client);
  const int w = client.right, h = client.bottom;
  const int left = px(SidebarWidth + 24), right = w - px(288);
  fill(dc, client, m_colors.window);
  fill(dc, {0, 0, px(SidebarWidth), h}, m_colors.panel);
  fill(dc, {px(SidebarWidth - 1), 0, px(SidebarWidth), h}, m_colors.border);
  fill(dc, {px(SidebarWidth), h - px(32), w, h}, m_colors.base);
  fill(dc, {px(SidebarWidth), h - px(33), w, h - px(32)}, m_colors.border);
  Gdiplus::Graphics graphics(dc);
  icon(graphics, PNG_PAW, {px(16), px(24), px(24), px(24)}, m_colors.accent);
  text(dc, L"OpenTypingPet", {px(14), px(52), px(SidebarWidth - 10), px(76)}, m_font, m_colors.text);
  const wchar_t *titles[] = {L"桌宠", L"动作", L"应用", L"角色库"};
  text(dc, titles[m_tab], {left, px(20), w - px(220), px(60)}, m_titleFont, m_colors.text);
  const auto character = engine().character();
  if (m_tab == 0) {
    text(dc, character ? character->name : L"尚未选择角色",
         {left, px(82), right, px(114)}, m_headingFont, m_colors.text);
    fill(dc, {left, px(264), right, px(265)}, m_colors.border);
    text(dc, L"宽度", {left, px(276), left + px(64), px(310)}, m_font, m_colors.text);
    text(dc, L"不透明度", {left, px(322), left + px(68), px(356)}, m_font, m_colors.text);
    text(dc, std::to_wstring(petSettings().opacity) + L"%",
         {right - px(56), px(322), right, px(356)}, m_font, m_colors.text,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    text(dc, L"缩放", {left, px(368), left + px(64), px(402)}, m_font, m_colors.text);
    text(dc, L"显示器", {left, px(414), left + px(64), px(448)}, m_font, m_colors.text);
  } else if (m_tab == 1) {
    fill(dc, {left, h - px(192), right, h - px(191)}, m_colors.border);
    text(dc, L"动作时序", {left, h - px(186), right, h - px(158)}, m_font, m_colors.muted);
    text(dc, L"待机延迟", {left, h - px(150), right - px(162), h - px(116)}, m_font, m_colors.text);
    text(dc, L"特殊动作保持", {left, h - px(102), right - px(162), h - px(68)}, m_font, m_colors.text);
    for (int y : {150, 102})
      text(dc, L"ms", {right - px(26), h - px(y), right, h - px(y - 34)}, m_font, m_colors.muted);
    if (!character)
      text(dc, L"尚无动作", {left, px(110), right, px(152)}, m_font, m_colors.muted);
  } else if (m_tab == 2) {
    text(dc, L"界面主题", {left, px(104), w - px(310), px(138)}, m_font, m_colors.text);
    fill(dc, {left, px(158), w - px(24), px(159)}, m_colors.border);
    text(dc, L"启动", {left, px(166), w - px(24), px(194)}, m_font, m_colors.muted);
    fill(dc, {left, px(290), w - px(24), px(291)}, m_colors.border);
    text(dc, L"自动暂停", {left, px(298), w - px(24), px(326)}, m_font, m_colors.muted);
    const int detailsColumn = left + (w - px(24) - left) / 2;
    text(dc, L"OpenTypingPet 0.1.0", {left, px(512), w - px(24), px(536)}, m_font, m_colors.text);
    text(dc, L"作者：KaffuAlcaid", {left, px(536), detailsColumn - px(12), px(560)}, m_font, m_colors.muted);
    text(dc, L"许可证：GPL-3.0-only", {detailsColumn, px(536), w - px(24), px(560)}, m_font, m_colors.muted);
  } else if (m_selectedLibrary >= 0 && size_t(m_selectedLibrary) < m_library->entries.size()) {
    auto &entry = m_library->entries[m_selectedLibrary];
    const int preview = w - px(264);
    if (const auto thumbnail = m_library->thumbnail(entry)) graphics.DrawImage(thumbnail.get(), preview + px(48), px(104), px(128), px(128));
    text(dc, entry.name, {preview, px(244), w - px(40), px(280)}, m_headingFont, m_colors.text);
    text(dc, L"动作数量：" + std::to_wstring(entry.actionCount),
         {preview, px(284), w - px(40), px(308)}, m_font, m_colors.muted);
    text(dc, L"角色包大小：" + std::to_wstring((entry.bytes + 1023) / 1024) + L" KiB",
         {preview, px(308), w - px(40), px(332)}, m_font, m_colors.muted);
    text(dc, entry.currentHash == entry.sourceHash ? L"原始角色包" : L"已编辑",
         {preview, px(338), w - px(40), px(362)}, m_font, m_colors.muted);
  } else if (m_tab == 3) {
    text(dc, L"角色库为空", {left, px(106), right, px(142)}, m_headingFont, m_colors.muted);
  }
  text(dc, L"当前桌宠", {px(14), px(294), px(SidebarWidth - 12), px(322)}, m_font, m_colors.muted);
  if (m_tab == 0 || m_tab == 1)
    paintPreview(dc);
  const std::wstring status = m_busy ? L"正在导入" : !m_error.empty() ? m_error : currentPet().paused() ? L"已暂停" : character ? L"运行中" : L"就绪";
  text(dc, status, {left, h - px(30), w - px(m_failedImport ? 240 : 138), h}, m_font,
       currentPet().record.paused || !character ? m_colors.muted : m_colors.accent);
}
void AppController::paintPreview(HDC dc) {
  RECT client{};
  GetClientRect(m_settingsWindow, &client);
  const int left = client.right - px(264), right = client.right - px(40);
  text(dc, L"预览", {left, px(94), right, px(122)}, m_font, m_colors.muted);
  const RECT area{left, px(136), right, px(360)};
  const COLORREF background = m_previewBackground == 1 ? RGB(247, 248, 250) : RGB(35, 37, 42);
  fill(dc, area, m_previewBackground ? background : m_colors.base);
  if (!m_previewBackground) {
    const int tile = px(14);
    for (int y = area.top; y < area.bottom; y += tile)
      for (int x = area.left; x < area.right; x += tile)
        if (((x - area.left) / tile + (y - area.top) / tile) % 2)
          fill(dc, {x, y, std::min(x + tile, int(area.right)), std::min(y + tile, int(area.bottom))},
               m_colors.dark ? RGB(43, 45, 50) : RGB(235, 238, 242));
  }
  const auto character = engine().character();
  if (!character) {
    Gdiplus::Graphics graphics(dc);
    icon(graphics, PNG_PAW, {left + px(84), px(244), px(56), px(56)}, m_colors.muted);
    return;
  }
  const size_t action = m_tab == 1 ? m_previewAction : engine().action();
  if (action >= character->actions.size())
    return;
  const auto &clip = character->animation(action);
  Gdiplus::Graphics graphics(dc);
  drawFrame(graphics, {area.left + px(8), area.top + px(8),
                       area.right - area.left - px(16), area.bottom - area.top - px(16)},
            clip, m_tab == 1 ? m_previewFrame : currentPet().frame(), petSettings().smooth);
  text(dc, character->actions[action].label, {left, px(368), right, px(398)},
       m_headingFont, m_colors.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
  text(dc, L"格式：" + clip.format, {left, px(398), right, px(418)}, m_font, m_colors.muted);
  text(dc, L"尺寸：" + std::to_wstring(clip.width) + L" × " + std::to_wstring(clip.height) + L" 像素",
       {left, px(418), right, px(438)}, m_font, m_colors.muted);
  text(dc, L"帧数：" + std::to_wstring(clip.frames.size()),
       {left, px(438), right, px(458)}, m_font, m_colors.muted);
}
void AppController::invalidatePreview() {
  RECT rect{};
  GetClientRect(m_settingsWindow, &rect);
  rect.left = rect.right - px(272);
  rect.top = px(88);
  rect.bottom -= px(38);
  InvalidateRect(m_settingsWindow, &rect, FALSE);
}
void AppController::selectPreview(size_t action) {
  const auto character = engine().character();
  if (!character || action >= character->actions.size())
    return;
  m_previewAction = action;
  m_previewPlaying = true;
  m_previewFrame = m_previewLoops = 0;
  const auto &clip = character->animation(action);
  SendMessageW(m_controls[PreviewTimeline], TBM_SETRANGE, TRUE, MAKELPARAM(0, int(clip.frames.size() - 1)));
  SendMessageW(m_controls[PreviewTimeline], TBM_SETPOS, TRUE, 0);
  m_previewAt = m_previewPlaying && clip.frames.size() > 1 ? GetTickCount64() + clip.frames[0].delay : 0;
  for (unsigned id : {PreviewPlay, PreviewPrevious, PreviewNext, PreviewTimeline, PreviewSpeed})
    EnableWindow(m_controls[id], clip.frames.size() > 1);
  updateTooltips();
  InvalidateRect(m_controls[PreviewPlay], nullptr, FALSE);
  SendMessageW(m_controls[Actions], LB_SETCURSEL, action, 0);
  invalidatePreview();
  schedule();
}
void AppController::paintControl(const DRAWITEMSTRUCT &draw) {
  if (draw.CtlType == ODT_MENU) { paintMenu(draw); return; }
  RECT visible = draw.rcItem;
  if (draw.CtlType == ODT_LISTBOX) {
    RECT client{}; GetClientRect(draw.hwndItem, &client);
    if (!IntersectRect(&visible, &visible, &client)) return;
  }
  const int width = draw.rcItem.right - draw.rcItem.left, height = draw.rcItem.bottom - draw.rcItem.top;
  if (width <= 0 || height <= 0) return;
  HDC memory = CreateCompatibleDC(draw.hDC);
  HBITMAP bitmap = CreateCompatibleBitmap(draw.hDC, width, height);
  if (!memory || !bitmap) {
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    return;
  }
  const auto previous = SelectObject(memory, bitmap);
  auto buffered = draw;
  buffered.hDC = memory;
  buffered.rcItem = {0, 0, width, height};
  if (draw.CtlID == Actions) paintAction(buffered);
  else if (draw.CtlID == LibraryList) paintLibraryEntry(buffered);
  else if (draw.CtlType == ODT_COMBOBOX) paintSelector(buffered);
  else paintButton(buffered);
  BitBlt(draw.hDC, visible.left, visible.top, visible.right - visible.left, visible.bottom - visible.top,
         memory, visible.left - draw.rcItem.left, visible.top - draw.rcItem.top, SRCCOPY);
  SelectObject(memory, previous);
  DeleteObject(bitmap);
  DeleteDC(memory);
}
void AppController::paintButton(const DRAWITEMSTRUCT &draw) {
  const unsigned id = draw.CtlID;
  RECT rect = draw.rcItem;
  const bool disabled = draw.itemState & ODS_DISABLED;
  const bool tab = id >= TabPet && id <= TabLibrary;
  const bool activeTab = tab && int(id - TabPet) == m_tab;
  const bool selected = (id >= ThemeSystem && id <= ThemeDark && int(id - ThemeSystem) == m_preferences.theme) ||
                        (id == PixelMode && !petSettings().smooth) || (id == SmoothMode && petSettings().smooth);
  const bool check = id == Topmost || id == Locked || id == StartSettings || id == LoginStartup ||
      id == Snap || id == Visible || id == InheritDefaults || id == PauseLockOption || id == PauseDisplayOff || id == HideFullscreen;
  COLORREF background = (draw.itemState & ODS_SELECTED) || selected ? m_colors.selection : m_colors.button;
  if (tab) background = activeTab ? m_colors.selection : m_colors.panel;
  if (check) background = m_colors.window;
  fill(draw.hDC, rect, background);
  const auto foreground = disabled ? m_colors.muted : selected || activeTab ? m_colors.accent : m_colors.text;
  if (id >= PreviewChecker && id <= PreviewDark) {
    RECT swatch = rect;
    InflateRect(&swatch, -px(5), -px(5));
    fill(draw.hDC, swatch, id == PreviewDark ? RGB(35, 37, 42) : RGB(247, 248, 250));
    if (id == PreviewChecker) {
      const int middleX = (swatch.left + swatch.right) / 2, middleY = (swatch.top + swatch.bottom) / 2;
      fill(draw.hDC, {swatch.left, swatch.top, middleX, middleY}, RGB(164, 169, 179));
      fill(draw.hDC, {middleX, middleY, swatch.right, swatch.bottom}, RGB(164, 169, 179));
    }
    HBRUSH border = CreateSolidBrush(int(id - PreviewChecker) == m_previewBackground ? m_colors.accent : m_colors.border);
    FrameRect(draw.hDC, &rect, border);
    DeleteObject(border);
  } else {
    if (check) {
      const bool checked = id == Topmost ? petSettings().topmost : id == Locked ? petSettings().locked :
                           id == StartSettings ? m_preferences.showSettingsOnStart : id == LoginStartup ? m_loginStartup :
                           id == Snap ? petSettings().snap : id == Visible ? currentPet().record.visible :
                           id == InheritDefaults ? currentPet().record.inheritDefaults : id == PauseLockOption ? m_preferences.pauseOnLock :
                           id == PauseDisplayOff ? m_preferences.pauseOnDisplayOff : m_preferences.fullscreenPolicy != 0;
      RECT box{rect.right - px(22), px(8), rect.right - px(2), px(28)};
      fill(draw.hDC, box, checked ? m_colors.accent : m_colors.base);
      if (checked)
        text(draw.hDC, L"✓", box, m_headingFont, m_colors.dark ? RGB(20, 40, 30) : RGB(255, 255, 255),
             DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      else {
        HBRUSH border = CreateSolidBrush(m_colors.border);
        FrameRect(draw.hDC, &box, border);
        DeleteObject(border);
      }
      rect.right -= px(32);
    }
    int image = id == Open ? PNG_FOLDER : id == Pause ? (currentPet().record.paused ? PNG_PLAY : PNG_PAUSE) :
                id == PreviewPlay ? (m_previewPlaying ? PNG_PAUSE : PNG_PLAY) :
                id == Reset ? PNG_RESET : id == TabPet ? PNG_PAW : id == TabActions ? PNG_PLAY :
                id == TabApp ? PNG_SETTINGS : id == TabLibrary ? PNG_FOLDER : 0;
    const bool iconOnly = id == Pause || id == PreviewPlay;
    if (image) {
      Gdiplus::Graphics graphics(draw.hDC);
      const int x = iconOnly ? (rect.right - px(18)) / 2 : px(12);
      icon(graphics, image, {x, (rect.bottom - px(18)) / 2, px(18), px(18)}, foreground);
      if (!iconOnly) rect.left += px(38);
    }
    if (!iconOnly) {
      wchar_t caption[128]{};
      GetWindowTextW(draw.hwndItem, caption, 128);
      if (id == PreviewPrevious) wcscpy_s(caption, L"‹");
      if (id == PreviewNext) wcscpy_s(caption, L"›");
      text(draw.hDC, caption, rect, m_font, foreground,
           (tab || check ? DT_LEFT : DT_CENTER) | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
  }
  if ((draw.itemState & ODS_FOCUS) && !(draw.itemState & ODS_NOFOCUSRECT)) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -px(3), -px(3));
    DrawFocusRect(draw.hDC, &focus);
  }
}
void AppController::paintAction(const DRAWITEMSTRUCT &draw) {
  const auto character = engine().character();
  if (draw.itemID == UINT(-1) || !character || draw.itemID >= character->actions.size())
    return;
  const auto &action = character->actions[draw.itemID];
  const auto &clip = character->animation(draw.itemID);
  fill(draw.hDC, draw.rcItem, draw.itemID == m_previewAction ? m_colors.selection : m_colors.window);
  {
    Gdiplus::Graphics graphics(draw.hDC);
    drawFrame(graphics, {draw.rcItem.left + px(6), draw.rcItem.top + px(8), px(48), px(48)}, clip, 0, petSettings().smooth);
  }
  RECT label{draw.rcItem.left + px(66), draw.rcItem.top + px(7), draw.rcItem.right - px(12), draw.rcItem.top + px(32)};
  text(draw.hDC, action.label, label, m_font, m_colors.text);
  std::wstring trigger;
  for (const auto &rule : character->rules)
    if (std::find(rule.actions.begin(), rule.actions.end(), action.id) != rule.actions.end()) {
      if (!trigger.empty()) trigger += L" 或";
      trigger += describeTrigger(rule);
    }
  if (trigger.empty())
    trigger = action.id == character->defaultAction ? L"默认动作" : L"动作结束后触发";
  else
    trigger += trigger.back() < 128 ? L" 时触发" : L"时触发";
  label.top = draw.rcItem.top + px(32);
  label.bottom = draw.rcItem.bottom - px(5);
  text(draw.hDC, trigger, label, m_font, m_colors.muted);
  fill(draw.hDC, {draw.rcItem.left, draw.rcItem.bottom - 1, draw.rcItem.right, draw.rcItem.bottom}, m_colors.border);
  if (draw.itemState & ODS_FOCUS) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -px(2), -px(2));
    DrawFocusRect(draw.hDC, &focus);
  }
}

PetInstance &AppController::createPet(const PetRecord &record) {
  auto pet = std::make_unique<PetInstance>(m_instance, m_environment, m_interactive);
  pet->record = record;
  if (pet->record.instanceId.empty()) pet->record.instanceId = Storage::newId();
  if (!pet->initialize()) throw std::wstring(L"无法创建桌宠窗口");
  const auto id = pet->record.instanceId;
  auto *pointer = pet.get();
  pet->changed = [this, pointer] {
    if (!m_controls.empty() && m_activePet < m_pets.size() && m_pets[m_activePet].get() == pointer &&
        m_tab == 0 && IsWindowVisible(m_settingsWindow)) invalidatePreview();
    schedule();
  };
  pet->positionChanged = [this] { saveState(); applyEnvironment(); };
  pet->openSettings = [this, id] {
    for (size_t i = 0; i < m_pets.size(); ++i) if (m_pets[i]->record.instanceId == id) { selectPet(i); break; }
    openSettings();
  };
  pet->contextRequested = [this, id](POINT point) {
    for (size_t i = 0; i < m_pets.size(); ++i) if (m_pets[i]->record.instanceId == id) { selectPet(i); break; }
    contextMenu(point);
  };
  pet->closeRequested = [this, id] {
    auto value = std::make_unique<std::string>(id);
    if (PostMessageW(m_settingsWindow, RemovePetMessage, 0, reinterpret_cast<LPARAM>(value.get()))) value.release();
  };
  pet->menuMessage = [this](UINT message, WPARAM wParam, LPARAM lParam) { return this->message(message, wParam, lParam); };
  m_pets.push_back(std::move(pet));
  return *pointer;
}
void AppController::restore(const std::wstring &path) {
  const bool loaded = m_library->load(m_error);
  if (loaded && !m_library->instances.empty()) {
    m_pets.clear(); m_activePet = 0;
    for (const auto &record : m_library->instances) {
      createPet(record);
      const auto *entry = m_library->find(record.libraryId);
      if (entry) m_importQueue.push_back({m_library->packagePath(entry->currentHash), entry->libraryId, record.instanceId, ImportMode::Restore, {}, m_importGeneration});
    }
    startImport();
  }
  refreshLibrary(); refreshInstances();
  if (!path.empty()) importPreset(path);
  else if (!loaded && !m_preferences.presetPath.empty()) importPreset(m_preferences.presetPath);
  applyEnvironment();
}
void AppController::queueImport(ImportJob job) {
  job.generation = m_importGeneration;
  m_importQueue.push_back(std::move(job));
  startImport();
}
void AppController::importPreset(const std::wstring &path) {
  queueImport({path});
}
void AppController::startImport() {
  if (m_busy || m_quitting || m_importQueue.empty()) return;
  if (m_worker.joinable()) m_worker.join();
  const auto job = m_importQueue.front(); m_importQueue.pop_front();
  m_busy = true; m_error.clear(); m_diagnostics.clear();
  syncControls();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
  const HWND target = m_settingsWindow;
  const auto storage = (m_library->root() / L"characters").wstring();
  m_worker = std::thread([target, storage, job] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = std::make_unique<ImportCompletion>(); result->job = job;
    try { result->result = PresetLoader::load(job.path, storage); }
    catch (...) { result->result.error = L"无法导入角色包"; }
    if (PostMessageW(target, Imported, 0, reinterpret_cast<LPARAM>(result.get()))) result.release();
    CoUninitialize();
  });
}
void AppController::finishImport(ImportCompletion *raw) {
  std::unique_ptr<ImportCompletion> completion(raw);
  if (m_worker.joinable()) m_worker.join();
  const auto &job = completion->job;
  auto &result = completion->result;
  if (job.generation != m_importGeneration) { m_busy = false; startImport(); syncControls(); return; }
  if (!result.character) {
    m_error = result.error; m_diagnostics = std::move(result.diagnostics); m_failedImport = job;
    openSettings();
  } else {
    try {
      CharacterEntry *entry = nullptr;
      if (!job.libraryId.empty()) entry = m_library->find(job.libraryId);
      if (job.mode == ImportMode::Restore || job.mode == ImportMode::Add || job.mode == ImportMode::Replace) {
        if (!entry) { m_busy = false; startImport(); syncControls(); return; }
      } else {
        std::string replace = job.libraryId;
        if (replace.empty())
          for (const auto &existing : m_library->entries)
            if (existing.packageId == result.character->id && existing.currentHash != result.character->contentHash) {
              const auto choice = chooseAction(m_settingsWindow, L"导入角色", L"角色库中已有相同标识的角色",
                                                {L"更新已有角色", L"另存为新角色", L"取消"}, m_colors);
              if (choice < 0 || choice == 2) { m_busy = false; startImport(); syncControls(); return; }
              if (choice == 0) replace = existing.libraryId;
              break;
            }
        entry = &m_library->add(result.character, job.path, petSettings(), replace, job.mode == ImportMode::Edited);
        if (job.disabledRules) entry->disabledRules = *job.disabledRules;
        if (job.temporary && entry->sourceHash == entry->currentHash) entry->sourcePath.clear();
        if (!replace.empty())
          for (const auto &pet : m_pets)
            if (pet->record.libraryId == entry->libraryId) {
              if (pet->record.inheritDefaults) pet->record.settings = entry->defaults;
              pet->setCharacter(result.character, entry->disabledRules);
            }
      }
      for (size_t i = 0; i < m_library->entries.size(); ++i)
        if (m_library->entries[i].libraryId == entry->libraryId) m_selectedLibrary = int(i);
      PetInstance *target = nullptr;
      if (job.mode == ImportMode::Restore || job.mode == ImportMode::Replace) {
        for (const auto &pet : m_pets) if (pet->record.instanceId == job.instanceId) { target = pet.get(); break; }
      } else if (job.mode == ImportMode::Add) {
        if (m_pets.size() >= 4 && !currentPet().record.libraryId.empty()) throw std::wstring(L"最多同时显示 4 个桌宠");
        if (m_pets.size() == 1 && currentPet().record.libraryId.empty()) target = &currentPet();
        else {
          PetRecord record; record.settings = entry->defaults;
          target = &createPet(record); m_activePet = m_pets.size() - 1;
        }
      } else if (currentPet().record.libraryId.empty()) target = &currentPet();
      if (target) {
        if (job.mode != ImportMode::Restore) {
          target->record.libraryId = entry->libraryId;
          target->record.settings = entry->defaults;
          target->record.inheritDefaults = true;
          target->record.ruleOverrides.clear(); target->record.disabledRules.clear();
        }
        target->setCharacter(result.character, entry->disabledRules);
        if (!target->record.placement.valid) {
          target->resetPosition();
          if (m_pets.size() > 1) {
            RECT bounds{}; GetWindowRect(target->window(), &bounds);
            SetWindowPos(target->window(), nullptr, bounds.left - int(m_pets.size() - 1) * 32,
                         bounds.top - int(m_pets.size() - 1) * 24, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
            target->capturePlacement();
          }
        }
      }
      m_preferences.initialized = true;
      m_preferences.presetPath = result.character->archivePath;
      if (m_interactive) m_preferences.save();
      if (job.mode == ImportMode::Import) { m_tab = 3; layout(); }
      refreshLibrary(); refreshInstances(); selectPet(m_activePet);
      m_failedImport.reset();
      saveState();
      if (job.temporary) { std::error_code ignored; std::filesystem::remove(job.path, ignored); }
    } catch (const std::wstring &error) { m_error = error; }
      catch (...) { m_error = L"无法保存角色库"; }
  }
  m_busy = false;
  applyEnvironment(); syncControls(); updateTray();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
  startImport();
}
void AppController::selectPet(size_t index) {
  if (index >= m_pets.size()) return;
  m_activePet = index;
  if (m_controls.empty()) return;
  m_syncing = true;
  SendMessageW(m_controls[PetSelector], CB_SETCURSEL, index, 0);
  SendMessageW(m_controls[Actions], LB_RESETCONTENT, 0, 0);
  if (const auto character = engine().character()) {
    for (const auto &action : character->actions)
      SendMessageW(m_controls[Actions], LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(action.label.c_str()));
    selectPreview(character->actionIndex(character->defaultAction));
  } else { m_previewAction = m_previewFrame = 0; m_previewAt = 0; }
  m_syncing = false;
  refreshInstances();
  syncControls(); updateTray(); InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::closePet(std::string id) {
  std::erase_if(m_pets, [&](const auto &pet) { return pet->record.instanceId == id; });
  if (m_pets.empty()) createPet(PetRecord{});
  m_activePet = std::min(m_activePet, m_pets.size() - 1);
  refreshInstances(); refreshLibrary(); selectPet(m_activePet); saveState(); applyEnvironment();
}
void AppController::saveState() {
  if (!m_interactive || !m_library) return;
  m_library->instances.clear();
  for (const auto &pet : m_pets)
    if (m_library->find(pet->record.libraryId)) m_library->instances.push_back(pet->record);
  try { m_library->save(); }
  catch (const std::wstring &error) { m_error = error; InvalidateRect(m_settingsWindow, nullptr, FALSE); }
  catch (...) { m_error = L"无法保存用户配置"; InvalidateRect(m_settingsWindow, nullptr, FALSE); }
}
void AppController::refreshLibrary() {
  if (!m_controls.contains(LibraryList)) return;
  SendMessageW(m_controls[LibraryList], LB_RESETCONTENT, 0, 0);
  for (const auto &entry : m_library->entries)
    SendMessageW(m_controls[LibraryList], LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(entry.name.c_str()));
  if (!m_library->entries.empty()) {
    m_selectedLibrary = std::clamp(m_selectedLibrary, 0, int(m_library->entries.size()) - 1);
    SendMessageW(m_controls[LibraryList], LB_SETCURSEL, m_selectedLibrary, 0);
  } else m_selectedLibrary = -1;
}
void AppController::refreshInstances() {
  if (!m_controls.contains(PetSelector)) return;
  m_syncing = true;
  SendMessageW(m_controls[PetSelector], CB_RESETCONTENT, 0, 0);
  for (size_t i = 0; i < m_pets.size(); ++i) {
    const auto *entry = m_library->find(m_pets[i]->record.libraryId);
    const auto label = std::to_wstring(i + 1) + L" 号：" + (entry ? entry->name : L"尚未选择角色");
    SendMessageW(m_controls[PetSelector], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
  }
  SendMessageW(m_controls[PetSelector], CB_SETCURSEL, m_activePet, 0);
  SendMessageW(m_controls[MonitorSelector], CB_RESETCONTENT, 0, 0);
  int selected = 0;
  const auto &monitors = m_environment.monitors();
  for (size_t i = 0; i < monitors.size(); ++i) {
    const auto label = L"显示器 " + std::to_wstring(i + 1) + L"（" + monitors[i].name + L"）";
    SendMessageW(m_controls[MonitorSelector], CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    if (monitors[i].id == currentPet().record.placement.monitor) selected = int(i);
  }
  SendMessageW(m_controls[MonitorSelector], CB_SETCURSEL, selected, 0);
  m_syncing = false;
}

void AppController::paintSelector(const DRAWITEMSTRUCT &draw) {
  fill(draw.hDC, draw.rcItem, draw.itemState & ODS_SELECTED ? m_colors.selection : m_colors.base);
  if (draw.itemID == UINT(-1)) return;
  const auto length = SendMessageW(draw.hwndItem, CB_GETLBTEXTLEN, draw.itemID, 0);
  if (length < 0) return;
  std::wstring label(size_t(length) + 1, L'\0');
  SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(label.data()));
  label.resize(size_t(length));
  RECT rect = draw.rcItem; rect.left += px(8); rect.right -= px(4);
  text(draw.hDC, label, rect, m_font, draw.itemState & ODS_DISABLED ? m_colors.muted : m_colors.text);
}
void AppController::paintLibraryEntry(const DRAWITEMSTRUCT &draw) {
  if (draw.itemID >= m_library->entries.size()) return;
  auto &entry = m_library->entries[draw.itemID];
  fill(draw.hDC, draw.rcItem, draw.itemID == unsigned(m_selectedLibrary) ? m_colors.selection : m_colors.window);
  if (const auto thumbnail = m_library->thumbnail(entry)) {
    Gdiplus::Graphics graphics(draw.hDC);
    graphics.DrawImage(thumbnail.get(), draw.rcItem.left + px(6), draw.rcItem.top + px(8), px(56), px(56));
  }
  RECT label{draw.rcItem.left + px(72), draw.rcItem.top + px(8), draw.rcItem.right - px(8), draw.rcItem.top + px(36)};
  text(draw.hDC, entry.name, label, m_font, m_colors.text);
  const auto count = std::count_if(m_pets.begin(), m_pets.end(), [&](const auto &pet) { return pet->record.libraryId == entry.libraryId; });
  label.top += px(28); label.bottom += px(28);
  text(draw.hDC, std::to_wstring(entry.actionCount) + L" 个动作" + (count ? L"，已添加 " + std::to_wstring(count) + L" 个桌宠" : L""),
       label, m_font, m_colors.muted);
}
void AppController::showDiagnostics() {
  std::wstring details;
  for (const auto &issue : m_diagnostics) {
    if (!details.empty()) details += L"\r\n\r\n";
    details += L"文件：" + issue.file;
    if (issue.line) details += L"\r\n位置：第 " + std::to_wstring(issue.line) + L" 行，第 " + std::to_wstring(issue.column) + L" 列";
    if (!issue.field.empty()) details += L"\r\n字段：" + issue.field;
    details += L"\r\n" + issue.message;
  }
  if (details.empty()) details = m_error;
  if (m_failedImport) details = L"角色包：" + m_failedImport->path + L"\r\n\r\n" + details;
  showDetails(m_settingsWindow, L"问题详情", details, m_colors);
}
void AppController::editCharacter() {
  if (m_busy) return;
  const auto *entry = m_selectedLibrary >= 0 && size_t(m_selectedLibrary) < m_library->entries.size() ? &m_library->entries[m_selectedLibrary] : nullptr;
  const std::string id = entry ? entry->libraryId : std::string();
  std::shared_ptr<CharacterDefinition> character;
  if (entry) {
    auto result = m_library->open(*entry);
    if (!result.character) { m_error = result.error; m_diagnostics = std::move(result.diagnostics); showDiagnostics(); return; }
    character = std::move(result.character);
  }
  auto edited = editCharacterWindow(m_settingsWindow, m_colors, std::move(character), entry ? entry->disabledRules : std::set<std::string>(),
      [this](bool capturing) { m_capturingInput = capturing; configureInput(); });
  if (!edited) return;
  const auto path = (m_library->root() / L"drafts" / (Storage::wide(Storage::newId()) + L".zip")).wstring();
  PresetLoader::writePackage(path, edited->resources);
  ImportJob job{path, id, {}, ImportMode::Edited}; job.disabledRules = std::move(edited->disabledRules); job.temporary = true;
  queueImport(std::move(job));
}
void AppController::editBinding() {
  const auto character = engine().character();
  if (!character || m_previewAction >= character->actions.size()) return;
  const auto action = character->actions[m_previewAction].id;
  std::vector<InputRule> bindings;
  std::vector<std::wstring> choices;
  for (const auto &rule : character->rules)
    if (std::find(rule.actions.begin(), rule.actions.end(), action) != rule.actions.end()) {
      bindings.push_back(rule); choices.push_back(rule.label);
    }
  // A compact choice list keeps heavily bound roles usable.
  std::vector<FormField> selection{{"binding", L"输入规则", L"新增触发", FieldKind::Choice}};
  selection[0].choices = {L"新增触发"};
  selection[0].choices.insert(selection[0].choices.end(), choices.begin(), choices.end());
  if (!editForm(m_settingsWindow, L"设置触发", selection, m_colors)) return;
  const auto chosen = std::find(choices.begin(), choices.end(), selection[0].value);
  const InputRule *existing = chosen == choices.end() ? nullptr : &bindings[size_t(chosen - choices.begin())];
  if (existing) {
    const auto operation = chooseAction(m_settingsWindow, L"输入规则", existing->label, {L"编辑", L"停用", L"取消"}, m_colors);
    if (operation < 0 || operation == 2) return;
    if (operation == 1) {
      currentPet().record.disabledRules.insert(existing->id);
      const auto *entry = m_library->find(currentPet().record.libraryId);
      currentPet().setCharacter(currentPet().source, entry ? entry->disabledRules : std::set<std::string>());
      selectPet(m_activePet); applyEnvironment(); saveState(); return;
    }
  }
  const auto edited = editInputRule(m_settingsWindow, m_colors, *character, existing, action,
      [this](bool capturing) { m_capturingInput = capturing; configureInput(); });
  if (!edited) return;
  auto &record = currentPet().record;
  std::erase_if(record.ruleOverrides, [&](const auto &rule) { return rule.id == edited->id; });
  record.ruleOverrides.push_back(*edited); record.disabledRules.erase(edited->id);
  const auto *entry = m_library->find(record.libraryId);
  currentPet().setCharacter(currentPet().source, entry ? entry->disabledRules : std::set<std::string>());
  selectPet(m_activePet); applyEnvironment(); saveState();
}
void AppController::libraryCommand(unsigned id) {
  if (m_busy) return;
  try {
    if (id == LibraryNew) { const int selection = m_selectedLibrary; m_selectedLibrary = -1; editCharacter(); m_selectedLibrary = selection; return; }
    if (m_selectedLibrary < 0 || size_t(m_selectedLibrary) >= m_library->entries.size()) return;
    const auto entry = m_library->entries[m_selectedLibrary];
    if (id == LibraryAdd || id == LibraryReplace) {
      queueImport({m_library->packagePath(entry.currentHash), entry.libraryId, currentPet().record.instanceId,
                   id == LibraryAdd ? ImportMode::Add : ImportMode::Replace});
    } else if (id == LibraryEdit) editCharacter();
    else if (id == LibraryFolder) {
      const auto path = std::filesystem::path(m_library->packagePath(entry.currentHash)).parent_path().wstring();
      ShellExecuteW(m_settingsWindow, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    } else if (id == LibraryExport) {
      const auto path = chooseFile(m_settingsWindow, true, L"导出角色包");
      if (path.empty()) return;
      auto result = m_library->open(entry);
      if (!result.character) { m_error = result.error; m_diagnostics = result.diagnostics; showDiagnostics(); return; }
      std::erase_if(result.character->rules, [&](const auto &rule) { return entry.disabledRules.contains(rule.id); });
      PresetLoader::writePackage(path, PresetLoader::exportResources(*result.character));
    } else if (id == LibraryRestore) {
      if (entry.currentHash == entry.sourceHash) return;
      if (chooseAction(m_settingsWindow, L"恢复原始版本", entry.name, {L"恢复原始版本", L"取消"}, m_colors) != 0) return;
      ImportJob job{m_library->packagePath(entry.sourceHash), entry.libraryId, {}, ImportMode::Edited};
      job.disabledRules = std::set<std::string>(); queueImport(std::move(job));
    } else if (id == LibraryRemove) {
      if (chooseAction(m_settingsWindow, L"移除角色", entry.name + L"\r\n此角色的桌宠也会关闭", {L"移除", L"取消"}, m_colors) != 0) return;
      std::erase_if(m_pets, [&](const auto &pet) { return pet->record.libraryId == entry.libraryId; });
      m_library->remove(entry.libraryId);
      if (m_pets.empty()) createPet(PetRecord{});
      m_activePet = std::min(m_activePet, m_pets.size() - 1);
      refreshInstances(); refreshLibrary(); selectPet(m_activePet); applyEnvironment(); saveState();
    }
  } catch (const std::wstring &error) { m_error = error; showDiagnostics(); }
    catch (...) { m_error = L"无法完成角色操作"; showDiagnostics(); }
  syncControls();
}
void AppController::choosePreset() {
  if (m_busy)
    return;
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return;
  const COMDLG_FILTERSPEC filter{L"角色包", L"*.zip"};
  dialog->SetFileTypes(1, &filter);
  dialog->SetTitle(L"导入角色包");
  FILEOPENDIALOGOPTIONS options{};
  dialog->GetOptions(&options);
  dialog->SetOptions(options | FOS_FILEMUSTEXIST | FOS_FORCEFILESYSTEM);
  if (SUCCEEDED(dialog->Show(m_settingsWindow))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr;
      if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
        importPreset(path);
        CoTaskMemFree(path);
      }
      item->Release();
    }
  }
  dialog->Release();
}
void AppController::openSettings() {
  if (!m_interactive)
    return;
  ShowWindow(m_settingsWindow, SW_RESTORE);
  SetForegroundWindow(m_settingsWindow);
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::pause(bool value) {
  currentPet().record.paused = value;
  applyEnvironment(); syncControls(); updateTray(); saveState();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::applyEnvironment() {
  for (const auto &pet : m_pets) {
    unsigned reasons = m_library->allPaused ? PauseGlobal : 0;
    if (m_preferences.pauseOnLock && m_environment.locked) reasons |= PauseLock;
    if (m_preferences.pauseOnDisplayOff && m_environment.displayOff) reasons |= PauseDisplay;
    if (m_environment.suspended) reasons |= PauseSleep;
    const bool fullscreen = m_preferences.fullscreenPolicy &&
        m_environment.fullscreenOn(MonitorFromWindow(pet->window(), MONITOR_DEFAULTTONEAREST), m_preferences.fullscreenExceptions);
    if (fullscreen) reasons |= PauseFullscreen;
    pet->setPolicy(reasons, fullscreen);
  }
  if (m_interactive && m_preferences.fullscreenPolicy) SetTimer(m_settingsWindow, EnvironmentTimer, 1000, nullptr);
  else KillTimer(m_settingsWindow, EnvironmentTimer);
  configureInput(); schedule();
}
void AppController::configureInput() {
  bool keyboard = false, mouse = false;
  for (const auto &pet : m_pets) if (pet->acceptsInput())
    for (const auto &rule : pet->engine.character()->rules) {
      if (rule.event == InputEventType::MouseDown || rule.event == InputEventType::MouseUp) {
        mouse = true;
        keyboard |= rule.ctrl || rule.shift || rule.alt;
      }
      else keyboard = true;
    }
  if (!m_input.setDevices(m_interactive && !m_capturingInput && keyboard, m_interactive && !m_capturingInput && mouse))
    m_error = L"无法更新输入监听";
}
void AppController::onAction() { currentPet().render(); invalidatePreview(); schedule(); }
void AppController::onTimer() {
  const auto now = GetTickCount64();
  for (const auto &pet : m_pets) pet->tick(now);
  const auto character = engine().character();
  if (character && m_tab == 1 && m_previewPlaying && m_previewAt &&
      IsWindowVisible(m_settingsWindow) && !IsIconic(m_settingsWindow)) {
    const auto &clip = character->animation(m_previewAction);
    bool updated = false;
    int advanced = 0;
    while (m_previewAt && now >= m_previewAt && advanced++ < 300) {
      if (m_previewFrame + 1 >= clip.frames.size()) {
        ++m_previewLoops;
        const int loops = character->loopCount(m_previewAction);
        if (loops >= 0 && m_previewLoops >= unsigned(loops)) {
          m_previewAt = 0; m_previewPlaying = false; updateTooltips();
          InvalidateRect(m_controls[PreviewPlay], nullptr, FALSE); break;
        }
        m_previewFrame = 0;
      } else ++m_previewFrame;
      m_previewAt += uint64_t(std::max(1.0, clip.frames[m_previewFrame].delay / m_previewSpeed));
      updated = true;
    }
    if (advanced >= 300 && m_previewAt) m_previewAt = now + clip.frames[m_previewFrame].delay;
    if (updated) {
      SendMessageW(m_controls[PreviewTimeline], TBM_SETPOS, TRUE, m_previewFrame);
      invalidatePreview();
    }
  }
  schedule();
}
void AppController::schedule() {
  if (!m_settingsWindow) return;
  KillTimer(m_settingsWindow, AnimationTimer);
  uint64_t deadline = 0;
  for (const auto &pet : m_pets)
    if (const auto next = pet->deadline()) deadline = deadline ? std::min(deadline, next) : next;
  if (m_previewAt && m_previewPlaying && m_tab == 1 && IsWindowVisible(m_settingsWindow) && !IsIconic(m_settingsWindow))
    deadline = deadline ? std::min(deadline, m_previewAt) : m_previewAt;
  if (deadline) {
    const auto now = GetTickCount64();
    SetTimer(m_settingsWindow, AnimationTimer, UINT(std::clamp<uint64_t>(deadline > now ? deadline - now : 1, 10, 60000)), nullptr);
  }
}
void AppController::renderPet() { currentPet().render(); }
void AppController::setWindowOptions() { currentPet().applySettings(); }
void AppController::resetPosition() { currentPet().resetPosition(); }
void AppController::ensureOnScreen() { for (const auto &pet : m_pets) pet->restorePlacement(); }
void AppController::savePosition() { currentPet().capturePlacement(); saveState(); }

void AppController::updateTray(bool add) {
  if (!m_interactive || !m_settingsWindow)
    return;
  if (!m_trayIcon) {
    Gdiplus::Bitmap image(32, 32, PixelFormat32bppARGB);
    {
      Gdiplus::Graphics graphics(&image);
      graphics.Clear(Gdiplus::Color(0, 0, 0, 0));
      icon(graphics, PNG_PAW, {0, 0, 32, 32}, RGB(20, 156, 133));
    }
    image.GetHICON(&m_trayIcon);
  }
  NOTIFYICONDATAW data{sizeof(data)};
  data.hWnd = m_settingsWindow;
  data.uID = 1;
  data.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
  data.uCallbackMessage = TrayEvent;
  data.hIcon = m_trayIcon;
  const auto character = engine().character();
  const auto label = L"OpenTypingPet" +
                     (character ? L" - " + character->name : std::wstring()) +
                     (currentPet().record.paused ? L"（已暂停）" : L"");
  wcsncpy_s(data.szTip, label.c_str(), _TRUNCATE);
  Shell_NotifyIconW(add ? NIM_ADD : NIM_MODIFY, &data);
  if (add) {
    data.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &data);
  }
}
void AppController::paintMenu(const DRAWITEMSTRUCT &draw) {
  const auto *item = reinterpret_cast<const MenuItem *>(draw.itemData);
  if (!item)
    return;
  if (item->label.empty()) {
    fill(draw.hDC, draw.rcItem, m_colors.base);
    const int middle = (draw.rcItem.top + draw.rcItem.bottom) / 2;
    fill(draw.hDC,
         {draw.rcItem.left + px(8), middle,
          draw.rcItem.right - px(8), middle + 1},
         m_colors.border);
    return;
  }
  const bool selected = draw.itemState & ODS_SELECTED,
             disabled = draw.itemState & ODS_DISABLED;
  const auto foreground = disabled ? m_colors.muted : m_colors.text;
  fill(draw.hDC, draw.rcItem, selected ? m_colors.selection : m_colors.base);
  RECT caption = draw.rcItem;
  caption.left += px(38);
  caption.right -= px(16);
  text(draw.hDC, item->label, caption, m_font, foreground);
  if (item->icon) {
    Gdiplus::Graphics graphics(draw.hDC);
    icon(graphics, item->icon,
         {draw.rcItem.left + px(10), draw.rcItem.top + px(7), px(18), px(18)},
         foreground);
  } else if (item->checked) {
    RECT check = draw.rcItem;
    check.left += px(8);
    check.right = check.left + px(22);
    text(draw.hDC, L"✓", check, m_headingFont, m_colors.accent,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
}
HMENU AppController::createMenu() {
  m_menuItems.clear();
  if (m_pets.size() > 1) {
    for (size_t i = 0; i < m_pets.size(); ++i) {
      const auto *entry = m_library->find(m_pets[i]->record.libraryId);
      m_menuItems.push_back({SelectPetMenu + unsigned(i), std::to_wstring(i + 1) + L" 号：" + (entry ? entry->name : L"桌宠"), 0, i == m_activePet});
    }
    m_menuItems.push_back({0, L""});
  }
  const std::vector<MenuItem> commands = {
      {Settings, L"设置", PNG_SETTINGS},
      {Open, L"导入角色包", PNG_FOLDER},
      {0, L""},
      {Pause, currentPet().record.paused ? L"继续" : L"暂停", currentPet().record.paused ? PNG_PLAY : PNG_PAUSE},
      {Visible, L"显示当前桌宠", 0, currentPet().record.visible},
      {Locked, L"锁定位置 / 鼠标穿透", 0, petSettings().locked},
      {Topmost, L"始终置顶", 0, petSettings().topmost},
      {Reset, L"重置位置", PNG_RESET},
      {ClosePet, L"关闭当前桌宠"},
      {0, L""},
      {PauseAll, m_library->allPaused ? L"全部继续" : L"全部暂停"},
      {Exit, L"退出", PNG_CLOSE}};
  m_menuItems.insert(m_menuItems.end(), commands.begin(), commands.end());
  HMENU menu = CreatePopupMenu();
  MENUINFO info{sizeof(info)};
  info.fMask = MIM_BACKGROUND;
  info.hbrBack = m_baseBrush;
  SetMenuInfo(menu, &info);
  for (size_t n = 0; n < m_menuItems.size(); ++n) {
    auto &item = m_menuItems[n];
    MENUITEMINFOW entry{sizeof(entry)};
    entry.fMask = MIIM_ID | MIIM_FTYPE | MIIM_STRING | MIIM_DATA | MIIM_STATE;
    entry.fType = MFT_OWNERDRAW;
    entry.wID = item.id;
    entry.dwTypeData = item.label.data();
    entry.dwItemData = reinterpret_cast<ULONG_PTR>(&item);
    entry.fState = (item.label.empty() ? MFS_DISABLED : 0) |
                   (item.checked ? MFS_CHECKED : 0) |
                   ((item.id == Open && m_busy) ? MFS_DISABLED : 0);
    InsertMenuItemW(menu, UINT(-1), TRUE, &entry);
  }
  return menu;
}
void AppController::contextMenu(POINT point) {
  HMENU menu = createMenu();
  HWND previous = GetForegroundWindow();
  HWND owner = IsWindowVisible(m_settingsWindow) ? m_settingsWindow : petWindow();
  SetForegroundWindow(owner);
  const auto selected =
      TrackPopupMenuEx(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y,
                       owner, nullptr);
  DestroyMenu(menu);
  PostMessageW(m_settingsWindow, WM_NULL, 0, 0);
  if (selected)
    command(selected, BN_CLICKED);
  if (selected != Settings && selected != Open && IsWindow(previous)) {
    DWORD ownerProcessId = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &ownerProcessId);
    if (ownerProcessId == GetCurrentProcessId())
      SetForegroundWindow(previous);
  }
}
void AppController::command(unsigned id, unsigned notification) {
  if (m_syncing)
    return;
  if (id >= SelectPetMenu && id < SelectPetMenu + 4 && notification == BN_CLICKED) {
    selectPet(id - SelectPetMenu); return;
  }
  const bool edit = id == WidthEdit || id == IdleEdit || id == HoldEdit;
  // Owner-drawn buttons also send focus notifications through WM_COMMAND.
  if (!edit && id != Actions && id != LibraryList && id != PetSelector && id != MonitorSelector &&
      id != PreviewSpeed && notification != BN_CLICKED)
    return;
  if (id == PetSelector || id == MonitorSelector || id == PreviewSpeed) {
    if (notification != CBN_SELCHANGE) return;
    const auto index = SendMessageW(m_controls[id], CB_GETCURSEL, 0, 0);
    if (index < 0) return;
    if (id == PetSelector) selectPet(size_t(index));
    else if (id == MonitorSelector && size_t(index) < m_environment.monitors().size()) {
      currentPet().moveToMonitor(m_environment.monitors()[index].id); applyEnvironment(); saveState();
    } else if (id == PreviewSpeed && index < 4) {
      const double speeds[] = {0.25, 0.5, 1, 2}; m_previewSpeed = speeds[index];
      if (m_previewPlaying && engine().character()) {
        m_previewAt = GetTickCount64() + uint64_t(std::max(1.0, engine().character()->animation(m_previewAction).frames[m_previewFrame].delay / m_previewSpeed));
        schedule();
      }
    }
    return;
  }
  if (id == LibraryList) {
    if (notification == LBN_SELCHANGE) { m_selectedLibrary = int(SendMessageW(m_controls[id], LB_GETCURSEL, 0, 0)); syncControls(); }
    return;
  }
  if ((id >= LibraryAdd && id <= LibraryRestore) || id == LibraryNew) { libraryCommand(id); return; }
  if (id == ClosePet) { closePet(currentPet().record.instanceId); return; }
  if (id == PauseAll) { m_library->allPaused = !m_library->allPaused; applyEnvironment(); saveState(); syncControls(); return; }
  if (id == CancelImport) { ++m_importGeneration; m_importQueue.clear(); m_error = L"导入已取消"; syncControls(); return; }
  if (id == RetryImport) { if (m_failedImport) queueImport(*m_failedImport); return; }
  if (id == ImportDetails) { showDiagnostics(); return; }
  if (id == BindAction) { editBinding(); return; }
  if (id == ClearBindings) {
    auto &record = currentPet().record; record.ruleOverrides.clear(); record.disabledRules.clear();
    const auto *entry = m_library->find(record.libraryId);
    currentPet().setCharacter(currentPet().source, entry ? entry->disabledRules : std::set<std::string>());
    selectPet(m_activePet); applyEnvironment(); saveState(); return;
  }
  if (id == PauseLockOption || id == PauseDisplayOff || id == HideFullscreen || id == FullscreenExceptions) {
    if (id == PauseLockOption) m_preferences.pauseOnLock = !m_preferences.pauseOnLock;
    else if (id == PauseDisplayOff) m_preferences.pauseOnDisplayOff = !m_preferences.pauseOnDisplayOff;
    else if (id == HideFullscreen) m_preferences.fullscreenPolicy = !m_preferences.fullscreenPolicy;
    else {
      std::vector<FormField> fields{{"exceptions", L"应用文件名（以分号分隔）", m_preferences.fullscreenExceptions, FieldKind::Multiline}};
      if (!editForm(m_settingsWindow, L"全屏应用例外", fields, m_colors)) return;
      m_preferences.fullscreenExceptions = fields[0].value;
    }
    if (m_interactive) m_preferences.save(); applyEnvironment(); syncControls(id); return;
  }
  if (id == SaveDefaults || id == InheritDefaults) {
    auto *entry = m_library->find(currentPet().record.libraryId); if (!entry) return;
    if (id == SaveDefaults) {
      entry->defaults = petSettings(); currentPet().record.inheritDefaults = true;
      for (const auto &pet : m_pets) if (pet->record.libraryId == entry->libraryId && pet->record.inheritDefaults) {
        pet->record.settings = entry->defaults; pet->applySettings(); pet->restorePlacement();
      }
    } else {
      currentPet().record.inheritDefaults = !currentPet().record.inheritDefaults;
      if (currentPet().record.inheritDefaults) { petSettings() = entry->defaults; currentPet().applySettings(); currentPet().restorePlacement(); }
    }
    saveState(); syncControls(); return;
  }
  if (id == Open) {
    choosePreset();
    return;
  }
  if (id == Settings) {
    openSettings();
    return;
  }
  if (id == Pause) {
    pause(!currentPet().record.paused);
    return;
  }
  if (id == Reset) {
    resetPosition();
    return;
  }
  if (id == Exit) {
    quit();
    return;
  }
  if (id >= TabPet && id <= TabLibrary) {
    const auto previous = TabPet + unsigned(m_tab);
    if (previous == id) return;
    m_tab = int(id - TabPet);
    if (m_tab == 1)
      selectPreview(m_previewAction);
    layout();
    InvalidateRect(m_controls[previous], nullptr, FALSE);
    InvalidateRect(m_controls[id], nullptr, FALSE);
    return;
  }
  if (id >= ThemeSystem && id <= ThemeDark) {
    changeTheme(int(id - ThemeSystem));
    return;
  }
  if (id == Actions) {
    if (notification == LBN_SELCHANGE) {
      const auto index = SendMessageW(m_controls[Actions], LB_GETCURSEL, 0, 0);
      if (index != LB_ERR)
        selectPreview(size_t(index));
    }
    return;
  }
  if (id >= PreviewChecker && id <= PreviewDark) {
    m_previewBackground = int(id - PreviewChecker);
    invalidatePreview();
    for (unsigned swatch = PreviewChecker; swatch <= PreviewDark; ++swatch)
      InvalidateRect(m_controls[swatch], nullptr, FALSE);
    return;
  }
  if (id == PreviewPlay) {
    m_previewPlaying = !m_previewPlaying;
    if (m_previewPlaying && engine().character()) {
      const auto &clip = engine().character()->animation(m_previewAction);
      if (m_previewFrame + 1 == clip.frames.size()) { m_previewFrame = 0; m_previewLoops = 0; }
      m_previewAt = GetTickCount64() + uint64_t(std::max(1.0, clip.frames[m_previewFrame].delay / m_previewSpeed));
    }
    else
      m_previewAt = 0;
    updateTooltips();
    InvalidateRect(m_controls[PreviewPlay], nullptr, FALSE);
    schedule();
    return;
  }
  if (id == PreviewPrevious || id == PreviewNext) {
    if (!engine().character()) return;
    const auto count = engine().character()->animation(m_previewAction).frames.size();
    m_previewPlaying = false; m_previewAt = 0;
    m_previewFrame = id == PreviewNext ? (m_previewFrame + 1) % count : (m_previewFrame + count - 1) % count;
    SendMessageW(m_controls[PreviewTimeline], TBM_SETPOS, TRUE, m_previewFrame);
    updateTooltips(); invalidatePreview(); schedule(); return;
  }
  if (id == StartSettings || id == LoginStartup) {
    if (id == StartSettings) {
      m_preferences.showSettingsOnStart = !m_preferences.showSettingsOnStart;
      if (m_interactive)
        m_preferences.save();
    } else if (!m_interactive || setLoginStartup(!m_loginStartup)) {
      m_loginStartup = !m_loginStartup;
      m_error.clear();
    } else
      m_error = L"无法更新登录启动设置。";
    InvalidateRect(m_controls[id], nullptr, FALSE);
    RECT footer{}; GetClientRect(m_settingsWindow, &footer); footer.left = px(SidebarWidth); footer.top = footer.bottom - px(32);
    InvalidateRect(m_settingsWindow, &footer, FALSE);
    return;
  }
  if (id == Snap) petSettings().snap = !petSettings().snap;
  else if (id == Visible) { currentPet().record.visible = !currentPet().record.visible; applyEnvironment(); saveState(); syncControls(id); return; }
  else if (id == Topmost) {
    petSettings().topmost = !petSettings().topmost;
    setWindowOptions();
  } else if (id == Locked) {
    petSettings().locked = !petSettings().locked;
    setWindowOptions();
  } else if (id == PixelMode || id == SmoothMode)
    petSettings().smooth = id == SmoothMode;
  else if (id == WidthMinus)
    petSettings().width = std::max(100, petSettings().width - 10);
  else if (id == WidthPlus)
    petSettings().width = std::min(800, petSettings().width + 10);
  else if (id == IdleMinus)
    petSettings().idleDelay = std::max(200, petSettings().idleDelay - 100);
  else if (id == IdlePlus)
    petSettings().idleDelay = std::min(5000, petSettings().idleDelay + 100);
  else if (id == HoldMinus)
    petSettings().minimumHold = std::max(0, petSettings().minimumHold - 50);
  else if (id == HoldPlus)
    petSettings().minimumHold = std::min(500, petSettings().minimumHold + 50);
  else if (id == WidthEdit || id == IdleEdit || id == HoldEdit) {
    if (notification != EN_KILLFOCUS)
      return;
    wchar_t buffer[32]{};
    GetWindowTextW(m_controls[id], buffer, 32);
    wchar_t *end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    if (end != buffer && !*end) {
      if (id == WidthEdit)
        petSettings().width = std::clamp(int(value), 100, 800);
      else if (id == IdleEdit)
        petSettings().idleDelay = std::clamp(int(value), 200, 5000);
      else
        petSettings().minimumHold = std::clamp(int(value), 0, 500);
    }
  } else
    return;
  currentPet().applySettings();
  currentPet().record.inheritDefaults = false;
  syncControls(id);
  renderPet();
  ensureOnScreen();
  updateTray();
  saveState();
  if (id == PixelMode || id == SmoothMode || id == WidthEdit || id == WidthMinus || id == WidthPlus)
    invalidatePreview();
  schedule();
}
void AppController::quit() {
  if (m_quitting)
    return;
  m_quitting = true;
  m_input.setEnabled(false, false);
  savePosition();
  if (m_worker.joinable())
    m_worker.join();
  MSG queued{};
  while (PeekMessageW(&queued, m_settingsWindow, Imported, Imported, PM_REMOVE))
    delete reinterpret_cast<ImportCompletion *>(queued.lParam);
  if (m_interactive) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = m_settingsWindow;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
  }
  ShowWindow(petWindow(), SW_HIDE);
  ShowWindow(m_settingsWindow, SW_HIDE);
  PostQuitMessage(0);
}
LRESULT CALLBACK AppController::settingsProc(HWND window, UINT message,
                                             WPARAM wParam, LPARAM lParam) {
  auto *self = reinterpret_cast<AppController *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<AppController *>(
        reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
    self->m_settingsWindow = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  return self ? self->message(message, wParam, lParam)
              : DefWindowProcW(window, message, wParam, lParam);
}
LRESULT CALLBACK AppController::editProc(HWND window, UINT message,
                                         WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR data) {
  auto *self = reinterpret_cast<AppController *>(data);
  if (message == WM_GETDLGCODE && lParam &&
      reinterpret_cast<MSG *>(lParam)->wParam == VK_RETURN)
    return DLGC_WANTALLKEYS;
  if (message == WM_KEYDOWN && wParam == VK_RETURN) {
    self->command(unsigned(id), EN_KILLFOCUS);
    SetFocus(self->m_settingsWindow);
    return 0;
  }
  return DefSubclassProc(window, message, wParam, lParam);
}
void AppController::paintSlider(HWND window, HDC dc) {
  RECT client{}, track{}, thumb{};
  GetClientRect(window, &client);
  fill(dc, client, m_colors.window);
  SendMessageW(window, TBM_GETCHANNELRECT, 0, reinterpret_cast<LPARAM>(&track));
  SendMessageW(window, TBM_GETTHUMBRECT, 0, reinterpret_cast<LPARAM>(&thumb));
  const int centerX = (thumb.left + thumb.right) / 2;
  const int centerY = (thumb.top + thumb.bottom) / 2;
  track.top = centerY - px(2);
  track.bottom = centerY + px(2);
  fill(dc, track, m_colors.border);
  RECT progress = track;
  progress.right = centerX;
  fill(dc, progress, m_colors.accent);
  const int radius = px(7);
  HBRUSH brush = CreateSolidBrush(IsWindowEnabled(window) ? m_colors.accent : m_colors.muted);
  const auto oldBrush = SelectObject(dc, brush);
  const auto oldPen = SelectObject(dc, GetStockObject(NULL_PEN));
  Ellipse(dc, centerX - radius, centerY - radius, centerX + radius, centerY + radius);
  SelectObject(dc, oldPen);
  SelectObject(dc, oldBrush);
  DeleteObject(brush);
  if (GetFocus() == window) {
    InflateRect(&client, -px(2), -px(2));
    DrawFocusRect(dc, &client);
  }
}
LRESULT CALLBACK AppController::sliderProc(HWND window, UINT message, WPARAM wParam,
                                          LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
  auto *self = reinterpret_cast<AppController *>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, sliderProc, id);
    return DefSubclassProc(window, message, wParam, lParam);
  }
  if (message == WM_ERASEBKGND)
    return 1;
  if (message == WM_PRINTCLIENT || (message == WM_PRINT && (lParam & PRF_CLIENT))) {
    self->paintSlider(window, reinterpret_cast<HDC>(wParam));
    return 0;
  }
  if (message == WM_PAINT) {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(window, &paint);
    RECT client{}; GetClientRect(window, &client);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, client.right), std::max(1L, client.bottom));
    if (memory && bitmap) {
      const auto previous = SelectObject(memory, bitmap);
      self->paintSlider(window, memory);
      BitBlt(dc, 0, 0, client.right, client.bottom, memory, 0, 0, SRCCOPY);
      SelectObject(memory, previous);
    }
    if (bitmap) DeleteObject(bitmap);
    if (memory) DeleteDC(memory);
    EndPaint(window, &paint);
    return 0;
  }
  const bool setting = message == TBM_SETPOS || message == TBM_SETRANGE ||
                       message == TBM_SETRANGEMIN || message == TBM_SETRANGEMAX;
  const bool interaction = message == WM_KEYDOWN || message == WM_KEYUP || message == WM_MOUSEWHEEL ||
                           message == WM_LBUTTONDOWN || message == WM_LBUTTONUP || message == WM_MOUSEMOVE || message == WM_TIMER;
  const auto position = interaction ? SendMessageW(window, TBM_GETPOS, 0, 0) : 0;
  const auto result = DefSubclassProc(window, message, wParam, lParam);
  // Native thumb-only invalidation does not cover the custom filled track.
  if ((setting && wParam) || (interaction && SendMessageW(window, TBM_GETPOS, 0, 0) != position) ||
      message == WM_SETFOCUS || message == WM_KILLFOCUS || message == WM_ENABLE || message == WM_SIZE)
    InvalidateRect(window, nullptr, FALSE);
  return result;
}
void AppController::paintScrollBar(HWND window, HDC supplied) {
  SCROLLBARINFO info{sizeof(info)};
  if (!GetScrollBarInfo(window, OBJID_VSCROLL, &info) ||
      !(GetWindowLongPtrW(window, GWL_STYLE) & WS_VSCROLL))
    return;
  RECT bounds{};
  GetWindowRect(window, &bounds);
  RECT track = info.rcScrollBar;
  OffsetRect(&track, -bounds.left, -bounds.top);
  if (track.right <= track.left || track.bottom <= track.top)
    return;
  HDC dc = supplied ? supplied : GetWindowDC(window);
  fill(dc, track, m_colors.window);
  RECT thumb{track.left + std::max(2L, (track.right - track.left) / 3),
             track.top + info.xyThumbTop,
             track.right - std::max(2L, (track.right - track.left) / 3),
             track.top + info.xyThumbBottom};
  if (thumb.bottom > thumb.top)
    fill(dc, thumb, m_colors.muted);
  if (info.dxyLineButton > 0) {
    RECT up = track, down = track;
    up.bottom = up.top + info.dxyLineButton;
    down.top = down.bottom - info.dxyLineButton;
    text(dc, L"˄", up, m_font, m_colors.muted,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    text(dc, L"˅", down, m_font, m_colors.muted,
         DT_CENTER | DT_VCENTER | DT_SINGLELINE);
  }
  if (!supplied)
    ReleaseDC(window, dc);
}
LRESULT CALLBACK AppController::listProc(HWND window, UINT message,
                                         WPARAM wParam, LPARAM lParam,
                                         UINT_PTR id, DWORD_PTR data) {
  auto *self = reinterpret_cast<AppController *>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, listProc, id);
    return DefSubclassProc(window, message, wParam, lParam);
  }
  const auto result = DefSubclassProc(window, message, wParam, lParam);
  if (message == WM_PRINT && (lParam & PRF_NONCLIENT))
    self->paintScrollBar(window, reinterpret_cast<HDC>(wParam));
  else if (message == WM_NCPAINT || message == WM_PAINT ||
           message == WM_VSCROLL || message == WM_MOUSEWHEEL ||
           message == WM_NCMOUSEMOVE || message == WM_NCLBUTTONUP ||
           message == WM_NCLBUTTONDOWN || message == WM_SIZE ||
           message == WM_TIMER)
    self->paintScrollBar(window);
  return result;
}
LRESULT AppController::message(UINT message, WPARAM wParam, LPARAM lParam) {
  if (m_environment.handle(message, wParam, lParam)) {
    applyEnvironment(); syncControls();
    return message == WM_POWERBROADCAST ? TRUE : 0;
  }
  if (m_taskbarMessage && message == m_taskbarMessage) {
    updateTray(true);
    return 0;
  }
  switch (message) {
  case WM_COPYDATA: {
    const auto *data = reinterpret_cast<COPYDATASTRUCT *>(lParam);
    if (!data || data->dwData != 0x4F545050 || !data->lpData ||
        data->cbData < sizeof(wchar_t) || data->cbData > 65536 || data->cbData % sizeof(wchar_t))
      return FALSE;
    const auto *path = static_cast<const wchar_t *>(data->lpData);
    if (path[data->cbData / sizeof(wchar_t) - 1] != L'\0')
      return FALSE;
    openSettings();
    if (*path)
      importPreset(path);
    return TRUE;
  }
  case WM_CLOSE:
    m_preferences.initialized = true;
    if (m_interactive)
      m_preferences.save();
    ShowWindow(m_settingsWindow, SW_HIDE);
    schedule();
    return 0;
  case WM_SIZE:
    layout();
    return 0;
  case WM_ERASEBKGND:
    return 1;
  case WM_PRINTCLIENT:
    paintSettings(reinterpret_cast<HDC>(wParam));
    return 0;
  case WM_PAINT: {
    PAINTSTRUCT paint{};
    HDC dc = BeginPaint(m_settingsWindow, &paint);
    RECT rect{};
    GetClientRect(m_settingsWindow, &rect);
    HDC memory = CreateCompatibleDC(dc);
    HBITMAP bitmap = CreateCompatibleBitmap(dc, std::max(1L, rect.right),
                                            std::max(1L, rect.bottom));
    auto old = SelectObject(memory, bitmap);
    paintSettings(memory);
    BitBlt(dc, 0, 0, rect.right, rect.bottom, memory, 0, 0, SRCCOPY);
    SelectObject(memory, old);
    DeleteObject(bitmap);
    DeleteDC(memory);
    EndPaint(m_settingsWindow, &paint);
    return 0;
  }
  case WM_GETMINMAXINFO: {
    auto *limits = reinterpret_cast<MINMAXINFO *>(lParam);
    RECT rect{0, 0, px(820), px(600)};
    AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, m_dpi);
    limits->ptMinTrackSize = {rect.right - rect.left, rect.bottom - rect.top};
    return 0;
  }
  case WM_DPICHANGED: {
    m_dpi = HIWORD(wParam);
    auto *rect = reinterpret_cast<RECT *>(lParam);
    SetWindowPos(m_settingsWindow, nullptr, rect->left, rect->top,
                 rect->right - rect->left, rect->bottom - rect->top,
                 SWP_NOZORDER | SWP_NOACTIVATE);
    applyTheme();
    layout();
    return 0;
  }
  case WM_SETTINGCHANGE:
    m_environment.refreshMonitors(); ensureOnScreen(); refreshInstances(); applyEnvironment();
    applyTheme();
    return 0;
  case WM_THEMECHANGED:
    applyTheme();
    return 0;
  case WM_DISPLAYCHANGE:
    m_environment.refreshMonitors();
    ensureOnScreen();
    refreshInstances(); applyEnvironment();
    return 0;
  case DesktopEnvironment::ChangedMessage:
    SetTimer(m_settingsWindow, EnvironmentDebounceTimer, 180, nullptr);
    return 0;
  case RemovePetMessage: {
    std::unique_ptr<std::string> id(reinterpret_cast<std::string *>(lParam));
    if (id) closePet(*id);
    return 0;
  }
  case WM_COMMAND:
    command(LOWORD(wParam), HIWORD(wParam));
    return 0;
  case WM_TIMER:
    if (wParam == AnimationTimer)
      onTimer();
    else if (wParam == EnvironmentTimer || wParam == EnvironmentDebounceTimer) {
      KillTimer(m_settingsWindow, EnvironmentDebounceTimer); applyEnvironment();
    }
    return 0;
  case Imported:
    finishImport(reinterpret_cast<ImportCompletion *>(lParam));
    return 0;
  case TrayEvent: {
    const auto event = LOWORD(lParam);
    if (event == NIN_SELECT || event == NIN_KEYSELECT)
      openSettings();
    else if (event == WM_CONTEXTMENU) {
      POINT point{};
      GetCursorPos(&point);
      contextMenu(point);
    }
    return 0;
  }
  case WM_DROPFILES: {
    auto drop = reinterpret_cast<HDROP>(wParam);
    if (DragQueryFileW(drop, UINT(-1), nullptr, 0) == 1) {
      const auto size = DragQueryFileW(drop, 0, nullptr, 0);
      std::wstring path(size + 1, L'\0');
      DragQueryFileW(drop, 0, path.data(), size + 1);
      path.resize(size);
      importPreset(path);
    }
    DragFinish(drop);
    return 0;
  }
  case WM_HSCROLL: {
    HWND slider = reinterpret_cast<HWND>(lParam);
    const auto id = GetDlgCtrlID(slider);
    const int value = int(SendMessageW(slider, TBM_GETPOS, 0, 0));
    if (id == PreviewTimeline) {
      m_previewPlaying = false; m_previewAt = 0; m_previewFrame = size_t(value);
      updateTooltips(); invalidatePreview(); schedule(); return 0;
    }
    if (id == WidthSlider)
      petSettings().width = value;
    else if (id == OpacitySlider)
      petSettings().opacity = value;
    else
      break;
    currentPet().record.inheritDefaults = false;
    syncControls(id);
    renderPet();
    if (LOWORD(wParam) == TB_ENDTRACK) {
      ensureOnScreen();
      saveState();
    }
    if (id == WidthSlider) invalidatePreview();
    else {
      RECT client{}; GetClientRect(m_settingsWindow, &client);
      RECT opacity{client.right - px(344), px(322), client.right - px(288), px(356)};
      InvalidateRect(m_settingsWindow, &opacity, FALSE);
    }
    return 0;
  }
  case WM_CTLCOLORSTATIC:
  case WM_CTLCOLORBTN:
  case WM_CTLCOLORLISTBOX:
  case WM_CTLCOLOREDIT: {
    HDC dc = reinterpret_cast<HDC>(wParam);
    SetTextColor(dc, m_colors.text);
    SetBkColor(dc,
               message == WM_CTLCOLOREDIT ? m_colors.base : m_colors.window);
    return reinterpret_cast<LRESULT>(
        message == WM_CTLCOLOREDIT ? m_baseBrush : m_windowBrush);
  }
  case WM_MEASUREITEM: {
    auto *measure = reinterpret_cast<MEASUREITEMSTRUCT *>(lParam);
    measure->itemHeight = px(64);
    if (measure->CtlType == ODT_COMBOBOX) measure->itemHeight = px(26);
    else if (measure->CtlID == LibraryList) measure->itemHeight = px(72);
    if (measure->CtlType == ODT_MENU) {
      const auto *item = reinterpret_cast<const MenuItem *>(measure->itemData);
      measure->itemHeight = px(item && item->label.empty() ? 8 : 32);
      measure->itemWidth = px(250);
    }
    return TRUE;
  }
  case WM_DRAWITEM: {
    const auto &draw = *reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
    paintControl(draw);
    return TRUE;
  }
  }
  return DefWindowProcW(m_settingsWindow, message, wParam, lParam);
}
