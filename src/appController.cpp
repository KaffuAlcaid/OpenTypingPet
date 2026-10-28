#include "appController.h"
#include "presetLoader.h"
#include "resourceIds.h"
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
  TabDisplay = 1010,
  TabPlayback,
  TabActions,
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
  Actions = 1050
};
constexpr UINT Imported = WM_APP + 1, TrayEvent = WM_APP + 2;
constexpr UINT_PTR AnimationTimer = 1;
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
  m_colors = themeColors(m_preferences.theme);
  m_engine.setTiming(m_preferences.idleDelay, m_preferences.minimumHold);
  m_engine.changed = [this] { onAction(); };
  m_input.input = [this](int key, bool shift, bool ctrl, bool alt, bool repeat,
                         bool mouse) {
    m_engine.input(key, shift, ctrl, alt, repeat, mouse);
    schedule();
  };
  m_input.sessionChanged = [this] { m_engine.reset(); };
}
AppController::~AppController() {
  m_input.setEnabled(false, false);
  if (m_worker.joinable())
    m_worker.join();
  if (m_interactive && m_settingsWindow) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = m_settingsWindow;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
  }
  if (m_petWindow)
    DestroyWindow(m_petWindow);
  if (m_settingsWindow)
    DestroyWindow(m_settingsWindow);
  if (m_petDC) {
    SelectObject(m_petDC, m_oldPetBitmap);
    DeleteObject(m_petBitmap);
    DeleteDC(m_petDC);
  }
  for (auto font : {m_font, m_headingFont, m_titleFont})
    if (font)
      DeleteObject(font);
  if (m_baseBrush)
    DeleteObject(m_baseBrush);
  if (m_windowBrush)
    DeleteObject(m_windowBrush);
  if (m_trayIcon)
    DestroyIcon(m_trayIcon);
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
  RECT rect{0, 0, px(900), px(670)};
  AdjustWindowRectExForDpi(&rect, WS_OVERLAPPEDWINDOW, FALSE, 0, m_dpi);
  m_settingsWindow = CreateWindowExW(
      0, cls.lpszClassName, L"OpenTypingPet",
      WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT,
      rect.right - rect.left, rect.bottom - rect.top, nullptr, nullptr,
      m_instance, this);
  if (!m_settingsWindow)
    return false;
  m_dpi = GetDpiForWindow(m_settingsWindow);
  cls.style = CS_DBLCLKS;
  cls.lpfnWndProc = petProc;
  cls.lpszClassName = L"OpenTypingPetDesktop";
  RegisterClassExW(&cls);
  m_petWindow = CreateWindowExW(
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, cls.lpszClassName,
      L"OpenTypingPet - 桌宠", WS_POPUP, 0, 0, px(m_preferences.width),
      px(m_preferences.width), nullptr, nullptr, m_instance, this);
  if (!m_petWindow)
    return false;
  for (int id = PNG_PAW; id <= PNG_CLOSE; ++id)
    m_icons[id] = resourceImage(m_instance, id);
  createControls();
  applyTheme();
  syncControls();
  layout();
  if (m_preferences.hasPosition)
    SetWindowPos(m_petWindow, nullptr, m_preferences.position.x,
                 m_preferences.position.y, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  else
    resetPosition();
  setWindowOptions();
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
           {Open, L"导入预设"},
           {Pause, L""},
           {Reset, L"重置位置"},
           {Topmost, L"始终置顶"},
           {Locked, L"锁定位置 / 鼠标穿透"},
           {TabDisplay, L"显示"},
           {TabPlayback, L"播放"},
           {TabActions, L"动作"},
           {ThemeSystem, L"跟随系统"},
           {ThemeLight, L"亮色"},
           {ThemeDark, L"深色"},
           {PixelMode, L"像素"},
           {SmoothMode, L"平滑"},
           {WidthMinus, L"−"},
           {WidthPlus, L"+"},
           {IdleMinus, L"−"},
           {IdlePlus, L"+"},
           {HoldMinus, L"−"},
           {HoldPlus, L"+"}})
    control(WC_BUTTONW, entry.second, entry.first, BS_OWNERDRAW | BS_NOTIFY);
  for (auto id : {WidthEdit, IdleEdit, HoldEdit}) {
    auto window = control(WC_EDITW, L"", id,
                          ES_CENTER | ES_NUMBER | WS_BORDER | WS_TABSTOP);
    SendMessageW(window, EM_SETLIMITTEXT, 5, 0);
    SetWindowSubclass(window, editProc, id, reinterpret_cast<DWORD_PTR>(this));
    SetWindowTheme(window, L"", L"");
  }
  for (auto id : {WidthSlider, OpacitySlider}) {
    auto window = control(TRACKBAR_CLASSW, L"", id, TBS_NOTICKS | TBS_HORZ);
    SendMessageW(window, TBM_SETRANGEMIN, FALSE, id == WidthSlider ? 100 : 20);
    SendMessageW(window, TBM_SETRANGEMAX, FALSE, id == WidthSlider ? 800 : 100);
    SendMessageW(window, TBM_SETPAGESIZE, 0, 10);
  }
  control(WC_LISTBOXW, L"", Actions,
          LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY |
              LBS_NOINTEGRALHEIGHT | WS_VSCROLL);
  SetWindowSubclass(m_controls[Actions], listProc, Actions,
                    reinterpret_cast<DWORD_PTR>(this));
}
void AppController::layout() {
  if (m_controls.empty())
    return;
  RECT rect{};
  GetClientRect(m_settingsWindow, &rect);
  const int w = MulDiv(rect.right, 96, m_dpi),
            h = MulDiv(rect.bottom, 96, m_dpi);
  const int left = 288, right = w - 24, span = right - left;
  auto place = [&](unsigned id, int x, int y, int width, int height) {
    MoveWindow(m_controls[id], px(x), px(y), px(width), px(height), TRUE);
  };
  place(Open, w - 174, 16, 116, 36);
  place(Pause, w - 50, 16, 34, 36);
  place(Reset, 22, h - 80, 220, 38);
  for (int i = 0; i < 3; ++i)
    place(TabDisplay + i, left + i * 96, 88, 96, 40);
  const int themeX = std::max(left + 90, right - 252);
  for (int i = 0; i < 3; ++i)
    place(ThemeSystem + i, themeX + i * 84, 148, 84, 34);
  place(Topmost, left, 220, span, 32);
  place(Locked, left, 262, span, 32);
  place(WidthSlider, left + 84, 340, std::max(32, span - 242), 36);
  place(WidthMinus, right - 140, 340, 34, 36);
  place(WidthEdit, right - 106, 340, 72, 36);
  place(WidthPlus, right - 34, 340, 34, 36);
  place(OpacitySlider, left + 84, 388, std::max(32, span - 160), 34);
  place(PixelMode, left + 84, 436, 88, 34);
  place(SmoothMode, left + 180, 436, 88, 34);
  place(IdleMinus, right - 190, 190, 34, 36);
  place(IdleEdit, right - 156, 190, 86, 36);
  place(IdlePlus, right - 70, 190, 34, 36);
  place(HoldMinus, right - 190, 250, 34, 36);
  place(HoldEdit, right - 156, 250, 86, 36);
  place(HoldPlus, right - 70, 250, 34, 36);
  place(Actions, left, 148, span, std::max(50, h - 204));
  SendMessageW(m_controls[Actions], LB_SETITEMHEIGHT, 0, px(76));
  for (auto [id, window] : m_controls) {
    bool visible = true;
    if ((id >= ThemeSystem && id <= SmoothMode) || id == Topmost ||
        id == Locked)
      visible = m_tab == 0;
    if (id >= IdleEdit && id <= HoldPlus)
      visible = m_tab == 1;
    if (id == Actions)
      visible = m_tab == 2;
    ShowWindow(window, visible ? SW_SHOWNA : SW_HIDE);
  }
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::syncControls() {
  if (m_controls.empty())
    return;
  m_syncing = true;
  SetWindowTextW(m_controls[WidthEdit],
                 std::to_wstring(m_preferences.width).c_str());
  SetWindowTextW(m_controls[IdleEdit],
                 std::to_wstring(m_preferences.idleDelay).c_str());
  SetWindowTextW(m_controls[HoldEdit],
                 std::to_wstring(m_preferences.minimumHold).c_str());
  SendMessageW(m_controls[WidthSlider], TBM_SETPOS, TRUE, m_preferences.width);
  SendMessageW(m_controls[OpacitySlider], TBM_SETPOS, TRUE,
               m_preferences.opacity);
  EnableWindow(m_controls[Open], !m_busy);
  EnableWindow(m_controls[Pause], m_engine.character() != nullptr);
  EnableWindow(m_controls[Reset], m_engine.character() != nullptr);
  for (auto [id, window] : m_controls)
    InvalidateRect(window, nullptr, TRUE);
  m_syncing = false;
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
  m_titleFont = makeFont(21, FW_SEMIBOLD);
  for (auto [id, window] : m_controls)
    SendMessageW(window, WM_SETFONT, reinterpret_cast<WPARAM>(m_font), TRUE);
  BOOL dark = m_colors.dark;
  DwmSetWindowAttribute(m_settingsWindow, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark,
                        sizeof(dark));
  RedrawWindow(m_settingsWindow, nullptr, nullptr,
               RDW_INVALIDATE | RDW_ALLCHILDREN);
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
  fill(dc, client, m_colors.window);
  fill(dc, {0, 0, w, px(68)}, m_colors.base);
  fill(dc, {0, px(68), px(264), h - px(36)}, m_colors.panel);
  fill(dc, {0, h - px(36), w, h}, m_colors.base);
  fill(dc, {0, px(67), w, px(68)}, m_colors.border);
  fill(dc, {px(263), px(68), px(264), h - px(36)}, m_colors.border);
  fill(dc, {0, h - px(37), w, h - px(36)}, m_colors.border);
  Gdiplus::Graphics graphics(dc);
  icon(graphics, PNG_PAW, {px(22), px(20), px(28), px(28)}, m_colors.accent);
  text(dc, L"OpenTypingPet", {px(62), 0, px(300), px(68)}, m_titleFont,
       m_colors.text);
  const auto character = m_engine.character();
  if (character && !character->clips.empty())
    drawFrame(graphics, {px(22), px(88), px(220), px(224)},
              character->clips[m_engine.action()], m_frame,
              m_preferences.smooth);
  else
    icon(graphics, PNG_PAW, {px(100), px(160), px(64), px(64)}, m_colors.muted);
  text(dc, character ? character->name : L"尚未选择角色",
       {px(22), px(326), px(242), px(386)}, m_titleFont, m_colors.text,
       DT_LEFT | DT_WORDBREAK);
  if (character)
    text(dc, std::to_wstring(character->clips.size()) + L" 个动作",
         {px(22), px(382), px(242), px(408)}, m_font, m_colors.muted);
  fill(dc, {px(22), px(420), px(242), px(421)}, m_colors.border);
  std::wstring state = m_busy      ? L"正在导入…"
                       : m_paused  ? L"已暂停"
                       : character ? character->clips[m_engine.action()].label
                                   : L"未运行";
  text(dc, state, {px(22), px(430), px(242), px(462)}, m_font,
       m_paused ? m_colors.muted : m_colors.accent);
  const int left = px(288), right = w - px(24);
  if (m_tab == 0) {
    text(dc, L"界面主题", {left, px(148), left + px(90), px(182)}, m_font,
         m_colors.text);
    text(dc, L"窗口", {left, px(190), right, px(216)}, m_headingFont,
         m_colors.text);
    fill(dc, {left, px(304), right, px(305)}, m_colors.border);
    text(dc, L"外观", {left, px(308), right, px(336)}, m_headingFont,
         m_colors.text);
    text(dc, L"宽度", {left, px(340), left + px(80), px(376)}, m_font,
         m_colors.text);
    text(dc, L"不透明度", {left, px(388), left + px(84), px(422)}, m_font,
         m_colors.text);
    text(dc, std::to_wstring(m_preferences.opacity) + L"%",
         {right - px(72), px(388), right, px(422)}, m_font, m_colors.text,
         DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
    text(dc, L"缩放方式", {left, px(436), left + px(84), px(470)}, m_font,
         m_colors.text);
  } else if (m_tab == 1) {
    text(dc, L"动作时序", {left, px(148), right, px(178)}, m_headingFont,
         m_colors.text);
    text(dc, L"待机延迟", {left, px(190), right - px(200), px(226)}, m_font,
         m_colors.text);
    text(dc, L"特殊动作最短显示", {left, px(250), right - px(200), px(286)},
         m_font, m_colors.text);
    text(dc, L"毫秒", {right - px(30), px(190), right, px(226)}, m_font,
         m_colors.muted);
    text(dc, L"毫秒", {right - px(30), px(250), right, px(286)}, m_font,
         m_colors.muted);
  }
  if (!m_error.empty())
    text(dc, m_error, {px(22), h - px(36), w - px(76), h}, m_font,
         m_colors.error);
  else
    text(dc,
         m_busy      ? L"正在导入…"
         : m_paused  ? L"已暂停"
         : character ? L"运行中"
                     : L"就绪",
         {px(22), h - px(36), w - px(76), h}, m_font, m_colors.muted);
  text(dc, L"0.1.0", {w - px(70), h - px(36), w - px(22), h}, m_font,
       m_colors.muted, DT_RIGHT | DT_VCENTER | DT_SINGLELINE);
}
void AppController::paintButton(const DRAWITEMSTRUCT &draw) {
  const unsigned id = draw.CtlID;
  RECT rect = draw.rcItem;
  const bool disabled = draw.itemState & ODS_DISABLED;
  const bool selected = (id >= ThemeSystem && id <= ThemeDark &&
                         int(id - ThemeSystem) == m_preferences.theme) ||
                        (id == PixelMode && !m_preferences.smooth) ||
                        (id == SmoothMode && m_preferences.smooth);
  const bool tab = id >= TabDisplay && id <= TabActions;
  COLORREF background = (draw.itemState & ODS_SELECTED) || selected
                            ? m_colors.selection
                            : m_colors.button;
  if (tab || id == Pause)
    background = m_colors.window;
  if (id == Reset)
    background = m_colors.panel;
  fill(draw.hDC, rect, background);
  const auto foreground = disabled   ? m_colors.muted
                          : selected ? m_colors.accent
                                     : m_colors.text;
  if (tab) {
    const bool active = int(id - TabDisplay) == m_tab;
    fill(draw.hDC,
         {0, rect.bottom - px(active ? 2 : 1), rect.right, rect.bottom},
         active ? m_colors.accent : m_colors.border);
  }
  if (id == Topmost || id == Locked) {
    fill(draw.hDC, rect, m_colors.window);
    const bool checked =
        id == Topmost ? m_preferences.topmost : m_preferences.locked;
    RECT box{rect.right - px(24), px(6), rect.right - px(4), px(26)};
    fill(draw.hDC, box, checked ? m_colors.accent : m_colors.base);
    if (checked)
      text(draw.hDC, L"✓", box, m_headingFont,
           m_colors.dark ? RGB(20, 40, 30) : RGB(255, 255, 255),
           DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    else {
      HBRUSH border = CreateSolidBrush(m_colors.border);
      FrameRect(draw.hDC, &box, border);
      DeleteObject(border);
    }
    rect.right -= px(34);
  }
  wchar_t caption[128]{};
  GetWindowTextW(draw.hwndItem, caption, 128);
  int image = id == Open    ? PNG_FOLDER
              : id == Pause ? (m_paused ? PNG_PLAY : PNG_PAUSE)
              : id == Reset ? PNG_RESET
                            : 0;
  if (image) {
    Gdiplus::Graphics graphics(draw.hDC);
    const int x = id == Pause ? (rect.right - px(18)) / 2 : px(10);
    icon(graphics, image, {x, (rect.bottom - px(18)) / 2, px(18), px(18)},
         foreground);
    if (id != Pause)
      rect.left += px(30);
  }
  const auto textColor =
      tab ? (int(id - TabDisplay) == m_tab ? m_colors.accent : m_colors.muted)
          : foreground;
  text(draw.hDC, caption, rect, m_font, textColor,
       (id == Topmost || id == Locked ? DT_LEFT : DT_CENTER) | DT_VCENTER |
           DT_SINGLELINE);
  if (draw.itemState & ODS_FOCUS) {
    RECT focus = draw.rcItem;
    InflateRect(&focus, -px(3), -px(3));
    DrawFocusRect(draw.hDC, &focus);
  }
}
void AppController::paintAction(const DRAWITEMSTRUCT &draw) {
  const auto character = m_engine.character();
  if (draw.itemID == UINT(-1) || !character ||
      draw.itemID >= character->clips.size())
    return;
  const auto &clip = character->clips[draw.itemID];
  const bool active = draw.itemID == m_engine.action();
  fill(draw.hDC, draw.rcItem, active ? m_colors.selection : m_colors.window);
  Gdiplus::Graphics graphics(draw.hDC);
  drawFrame(graphics,
            {draw.rcItem.left + px(6), draw.rcItem.top + px(6), px(64), px(64)},
            clip, 0, false);
  RECT label = draw.rcItem;
  label.left += px(86);
  label.right -= px(88);
  text(draw.hDC, clip.label, label, m_font, m_colors.text);
  if (clip.frames.size() > 1) {
    RECT count = draw.rcItem;
    count.left = count.right - px(92);
    count.right -= px(32);
    text(draw.hDC, std::to_wstring(clip.frames.size()) + L" 帧", count, m_font,
         m_colors.muted);
  }
  icon(graphics, PNG_PLAY,
       {draw.rcItem.right - px(26), draw.rcItem.top + px(28), px(18), px(18)},
       m_colors.text);
  fill(draw.hDC,
       {draw.rcItem.left, draw.rcItem.bottom - 1, draw.rcItem.right,
        draw.rcItem.bottom},
       m_colors.border);
}

void AppController::restore(const std::wstring &path) {
  const auto &preset = path.empty() ? m_preferences.presetPath : path;
  if (!preset.empty())
    importPreset(preset);
}
void AppController::importPreset(const std::wstring &path) {
  if (m_busy || m_quitting)
    return;
  if (m_worker.joinable())
    m_worker.join();
  m_busy = true;
  m_error.clear();
  syncControls();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
  const HWND target = m_settingsWindow;
  m_worker = std::thread([target, path] {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    auto result = std::make_unique<ImportResult>();
    try {
      *result = PresetLoader::load(path, PresetLoader::storageRoot());
    } catch (...) {
      result->error = L"无法导入角色预设。";
    }
    if (PostMessageW(target, Imported, 0,
                     reinterpret_cast<LPARAM>(result.get())))
      result.release();
    CoUninitialize();
  });
}
void AppController::finishImport(ImportResult *raw) {
  std::unique_ptr<ImportResult> result(raw);
  if (m_worker.joinable())
    m_worker.join();
  m_busy = false;
  if (!result->character) {
    m_error = result->error;
  } else {
    m_preferences.presetPath = result->character->archivePath;
    m_engine.setCharacter(std::move(result->character));
    const auto character = m_engine.character();
    SetWindowTextW(m_petWindow,
                   (L"OpenTypingPet - " + character->name).c_str());
    SendMessageW(m_controls[Actions], LB_RESETCONTENT, 0, 0);
    for (const auto &clip : character->clips)
      SendMessageW(m_controls[Actions], LB_ADDSTRING, 0,
                   reinterpret_cast<LPARAM>(clip.label.c_str()));
    if (m_interactive)
      m_preferences.save();
    configureInput();
    ensureOnScreen();
  }
  syncControls();
  updateTray();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::choosePreset() {
  if (m_busy)
    return;
  IFileOpenDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr,
                              CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
    return;
  const COMDLG_FILTERSPEC filter{L"TypingPet 预设", L"*.zip"};
  dialog->SetFileTypes(1, &filter);
  dialog->SetTitle(L"导入 TypingPet 预设");
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
  m_paused = value;
  configureInput();
  m_engine.reset();
  syncControls();
  updateTray();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
}
void AppController::configureInput() {
  bool mouse = false;
  const auto character = m_engine.character();
  if (character)
    for (const auto &binding : character->bindings)
      mouse |= binding.key == 1 || binding.key == 2 || binding.key == 4 ||
               binding.key == 5 || binding.key == 6;
  if (!m_input.setEnabled(m_interactive && character && !m_paused, mouse)) {
    m_paused = true;
    m_error = L"无法更新输入监听，已停止处理输入。请重新启动程序。";
  }
}
void AppController::onAction() {
  m_frame = 0;
  m_loops = 0;
  const auto character = m_engine.character();
  if (character) {
    const auto &clip = character->clips[m_engine.action()];
    m_frameAt = !m_paused && clip.frames.size() > 1
                    ? GetTickCount64() + clip.frames[0].delay
                    : 0;
  }
  renderPet();
  schedule();
  if (IsWindowVisible(m_settingsWindow)) {
    RECT area{px(0), px(68), px(263), px(470)};
    InvalidateRect(m_settingsWindow, &area, FALSE);
    InvalidateRect(m_controls[Actions], nullptr, FALSE);
  }
}
void AppController::onTimer() {
  const auto now = GetTickCount64();
  m_engine.tick(now);
  const auto character = m_engine.character();
  bool changed = false;
  if (character && !m_paused) {
    const auto &clip = character->clips[m_engine.action()];
    int advanced = 0;
    while (m_frameAt && now >= m_frameAt && advanced++ < 300) {
      if (m_frame + 1 >= clip.frames.size()) {
        ++m_loops;
        if (clip.loops >= 0 && m_loops >= clip.loops) {
          m_frameAt = 0;
          break;
        }
        m_frame = 0;
      } else
        ++m_frame;
      m_frameAt += clip.frames[m_frame].delay;
      changed = true;
    }
    if (advanced >= 300 && m_frameAt)
      m_frameAt = now + clip.frames[m_frame].delay;
  }
  if (changed) {
    renderPet();
    if (IsWindowVisible(m_settingsWindow)) {
      RECT area{px(22), px(88), px(242), px(312)};
      InvalidateRect(m_settingsWindow, &area, FALSE);
    }
  }
  schedule();
}
void AppController::schedule() {
  if (!m_settingsWindow)
    return;
  KillTimer(m_settingsWindow, AnimationTimer);
  uint64_t deadline = m_engine.deadline();
  if (m_frameAt && !m_paused)
    deadline = deadline ? std::min(deadline, m_frameAt) : m_frameAt;
  if (deadline) {
    const auto now = GetTickCount64();
    SetTimer(m_settingsWindow, AnimationTimer,
             UINT(std::clamp<uint64_t>(deadline > now ? deadline - now : 1, 10,
                                       60000)),
             nullptr);
  }
}
void AppController::renderPet() {
  const auto character = m_engine.character();
  if (!m_petWindow || !character)
    return;
  const auto &clip = character->clips[m_engine.action()];
  const UINT dpi = GetDpiForWindow(m_petWindow);
  int width = MulDiv(m_preferences.width, dpi, 96),
      height =
          int(std::round(double(width) * character->height / character->width));
  if (width <= 0 || height <= 0 || width > 8192 || height > 8192)
    return;
  if (m_petSize.cx != width || m_petSize.cy != height) {
    if (m_petDC) {
      SelectObject(m_petDC, m_oldPetBitmap);
      DeleteObject(m_petBitmap);
      DeleteDC(m_petDC);
      m_petDC = nullptr;
    }
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = width;
    info.bmiHeader.biHeight = -height;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    info.bmiHeader.biCompression = BI_RGB;
    m_petDC = CreateCompatibleDC(nullptr);
    m_petBitmap = CreateDIBSection(m_petDC, &info, DIB_RGB_COLORS, &m_petPixels,
                                   nullptr, 0);
    if (!m_petBitmap) {
      DeleteDC(m_petDC);
      m_petDC = nullptr;
      m_petSize = {};
      return;
    }
    m_oldPetBitmap = static_cast<HBITMAP>(SelectObject(m_petDC, m_petBitmap));
    m_petSize = {width, height};
  }
  memset(m_petPixels, 0, size_t(width) * height * 4);
  {
    Gdiplus::Bitmap target(width, height, width * 4, PixelFormat32bppPARGB,
                           static_cast<BYTE *>(m_petPixels));
    Gdiplus::Graphics graphics(&target);
    graphics.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    drawFrame(graphics, {0, 0, width, height}, clip, m_frame,
              m_preferences.smooth);
  }
  RECT rect{};
  GetWindowRect(m_petWindow, &rect);
  POINT position{rect.left, rect.top}, origin{};
  SIZE size{width, height};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(m_preferences.opacity * 255 / 100),
                      AC_SRC_ALPHA};
  UpdateLayeredWindow(m_petWindow, nullptr, &position, &size, m_petDC, &origin,
                      0, &blend, ULW_ALPHA);
  if (m_interactive)
    ShowWindow(m_petWindow, SW_SHOWNOACTIVATE);
}
void AppController::setWindowOptions() {
  if (!m_petWindow)
    return;
  auto style = GetWindowLongPtrW(m_petWindow, GWL_EXSTYLE);
  if (m_preferences.locked)
    style |= WS_EX_TRANSPARENT;
  else
    style &= ~LONG_PTR(WS_EX_TRANSPARENT);
  SetWindowLongPtrW(m_petWindow, GWL_EXSTYLE, style);
  SetWindowPos(m_petWindow,
               m_preferences.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0,
               0, SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
}
void AppController::resetPosition() {
  if (!m_petWindow)
    return;
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(MonitorFromWindow(m_petWindow, MONITOR_DEFAULTTOPRIMARY),
                  &info);
  RECT rect{};
  GetWindowRect(m_petWindow, &rect);
  SetWindowPos(m_petWindow, nullptr,
               info.rcWork.right - (rect.right - rect.left) - px(24),
               info.rcWork.bottom - (rect.bottom - rect.top) - px(24), 0, 0,
               SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  savePosition();
}
void AppController::ensureOnScreen() {
  if (!m_petWindow || m_dragging)
    return;
  RECT rect{};
  GetWindowRect(m_petWindow, &rect);
  MONITORINFO info{sizeof(info)};
  GetMonitorInfoW(MonitorFromRect(&rect, MONITOR_DEFAULTTONEAREST), &info);
  RECT overlap{};
  if (!IntersectRect(&overlap, &rect, &info.rcWork) ||
      overlap.right - overlap.left < 32 || overlap.bottom - overlap.top < 32)
    resetPosition();
}
void AppController::savePosition() {
  if (!m_petWindow)
    return;
  RECT rect{};
  GetWindowRect(m_petWindow, &rect);
  m_preferences.position = {rect.left, rect.top};
  m_preferences.hasPosition = true;
  if (m_interactive && m_engine.character())
    m_preferences.save();
}
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
  const auto character = m_engine.character();
  const auto label = L"OpenTypingPet" +
                     (character ? L" - " + character->name : std::wstring()) +
                     (m_paused ? L"（已暂停）" : L"");
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
  m_menuItems = {
      {Settings, L"设置", PNG_SETTINGS},
      {Open, L"导入预设", PNG_FOLDER},
      {0, L""},
      {Pause, m_paused ? L"继续" : L"暂停", m_paused ? PNG_PLAY : PNG_PAUSE},
      {Locked, L"锁定位置 / 鼠标穿透", 0, m_preferences.locked},
      {Topmost, L"始终置顶", 0, m_preferences.topmost},
      {Reset, L"重置位置", PNG_RESET},
      {0, L""},
      {Exit, L"退出", PNG_CLOSE}};
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
  HWND owner = IsWindowVisible(m_settingsWindow) ? m_settingsWindow : m_petWindow;
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
  if (id == Open) {
    choosePreset();
    return;
  }
  if (id == Settings) {
    openSettings();
    return;
  }
  if (id == Pause) {
    pause(!m_paused);
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
  if (id >= TabDisplay && id <= TabActions) {
    m_tab = int(id - TabDisplay);
    layout();
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
        m_engine.preview(size_t(index));
    }
    return;
  }
  if (id == Topmost) {
    m_preferences.topmost = !m_preferences.topmost;
    setWindowOptions();
  } else if (id == Locked) {
    m_preferences.locked = !m_preferences.locked;
    setWindowOptions();
  } else if (id == PixelMode || id == SmoothMode)
    m_preferences.smooth = id == SmoothMode;
  else if (id == WidthMinus)
    m_preferences.width = std::max(100, m_preferences.width - 10);
  else if (id == WidthPlus)
    m_preferences.width = std::min(800, m_preferences.width + 10);
  else if (id == IdleMinus)
    m_preferences.idleDelay = std::max(200, m_preferences.idleDelay - 100);
  else if (id == IdlePlus)
    m_preferences.idleDelay = std::min(5000, m_preferences.idleDelay + 100);
  else if (id == HoldMinus)
    m_preferences.minimumHold = std::max(0, m_preferences.minimumHold - 50);
  else if (id == HoldPlus)
    m_preferences.minimumHold = std::min(500, m_preferences.minimumHold + 50);
  else if (id == WidthEdit || id == IdleEdit || id == HoldEdit) {
    if (notification != EN_KILLFOCUS)
      return;
    wchar_t buffer[32]{};
    GetWindowTextW(m_controls[id], buffer, 32);
    wchar_t *end = nullptr;
    const long value = wcstol(buffer, &end, 10);
    if (end != buffer && !*end) {
      if (id == WidthEdit)
        m_preferences.width = std::clamp(int(value), 100, 800);
      else if (id == IdleEdit)
        m_preferences.idleDelay = std::clamp(int(value), 200, 5000);
      else
        m_preferences.minimumHold = std::clamp(int(value), 0, 500);
    }
  } else
    return;
  m_engine.setTiming(m_preferences.idleDelay, m_preferences.minimumHold);
  syncControls();
  renderPet();
  ensureOnScreen();
  updateTray();
  if (m_interactive)
    m_preferences.save();
  InvalidateRect(m_settingsWindow, nullptr, FALSE);
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
    delete reinterpret_cast<ImportResult *>(queued.lParam);
  if (m_interactive) {
    NOTIFYICONDATAW data{sizeof(data)};
    data.hWnd = m_settingsWindow;
    data.uID = 1;
    Shell_NotifyIconW(NIM_DELETE, &data);
  }
  ShowWindow(m_petWindow, SW_HIDE);
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
  if (m_taskbarMessage && message == m_taskbarMessage) {
    updateTray(true);
    return 0;
  }
  switch (message) {
  case WM_CLOSE:
    ShowWindow(m_settingsWindow, SW_HIDE);
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
    RECT rect{0, 0, px(760), px(580)};
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
  case WM_THEMECHANGED:
    applyTheme();
    return 0;
  case WM_DISPLAYCHANGE:
    ensureOnScreen();
    return 0;
  case WM_COMMAND:
    command(LOWORD(wParam), HIWORD(wParam));
    return 0;
  case WM_TIMER:
    if (wParam == AnimationTimer)
      onTimer();
    return 0;
  case Imported:
    finishImport(reinterpret_cast<ImportResult *>(lParam));
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
    if (id == WidthSlider)
      m_preferences.width = value;
    else if (id == OpacitySlider)
      m_preferences.opacity = value;
    else
      break;
    syncControls();
    renderPet();
    if (LOWORD(wParam) == TB_ENDTRACK) {
      ensureOnScreen();
      if (m_interactive)
        m_preferences.save();
    }
    InvalidateRect(m_settingsWindow, nullptr, FALSE);
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
    measure->itemHeight = px(76);
    if (measure->CtlType == ODT_MENU) {
      const auto *item = reinterpret_cast<const MenuItem *>(measure->itemData);
      measure->itemHeight = px(item && item->label.empty() ? 8 : 32);
      measure->itemWidth = px(250);
    }
    return TRUE;
  }
  case WM_DRAWITEM: {
    const auto &draw = *reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
    if (draw.CtlType == ODT_MENU)
      paintMenu(draw);
    else if (draw.CtlID == Actions)
      paintAction(draw);
    else
      paintButton(draw);
    return TRUE;
  }
  case WM_NOTIFY: {
    auto *header = reinterpret_cast<NMHDR *>(lParam);
    if (header->code == NM_CUSTOMDRAW &&
        (header->idFrom == WidthSlider || header->idFrom == OpacitySlider)) {
      auto *draw = reinterpret_cast<NMCUSTOMDRAW *>(lParam);
      if (draw->dwDrawStage == CDDS_PREPAINT)
        return CDRF_NOTIFYITEMDRAW;
      if (draw->dwDrawStage == CDDS_ITEMPREPAINT) {
        if (draw->dwItemSpec == TBCD_CHANNEL) {
          RECT track = draw->rc;
          const int middle = (track.top + track.bottom) / 2;
          track.top = middle - px(2);
          track.bottom = middle + px(2);
          fill(draw->hdc, track, m_colors.border);
          return CDRF_SKIPDEFAULT;
        }
        if (draw->dwItemSpec == TBCD_THUMB) {
          HBRUSH brush = CreateSolidBrush(m_colors.accent);
          auto oldBrush = SelectObject(draw->hdc, brush);
          auto oldPen = SelectObject(draw->hdc, GetStockObject(NULL_PEN));
          const int centerX = (draw->rc.left + draw->rc.right) / 2;
          const int centerY = (draw->rc.top + draw->rc.bottom) / 2;
          const int radius = px(8);
          Ellipse(draw->hdc, centerX - radius, centerY - radius,
                  centerX + radius, centerY + radius);
          SelectObject(draw->hdc, oldPen);
          SelectObject(draw->hdc, oldBrush);
          DeleteObject(brush);
          return CDRF_SKIPDEFAULT;
        }
      }
    }
    break;
  }
  }
  return DefWindowProcW(m_settingsWindow, message, wParam, lParam);
}
LRESULT CALLBACK AppController::petProc(HWND window, UINT message,
                                        WPARAM wParam, LPARAM lParam) {
  auto *self = reinterpret_cast<AppController *>(
      GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<AppController *>(
        reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
    self->m_petWindow = window;
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (!self)
    return DefWindowProcW(window, message, wParam, lParam);
  switch (message) {
  case WM_MOUSEACTIVATE:
    return MA_NOACTIVATE;
  case WM_MEASUREITEM:
  case WM_DRAWITEM:
    return self->message(message, wParam, lParam);
  case WM_LBUTTONDOWN:
    if (!self->m_preferences.locked) {
      self->m_dragging = true;
      GetCursorPos(&self->m_cursorOrigin);
      RECT rect{};
      GetWindowRect(window, &rect);
      self->m_dragOrigin = {rect.left, rect.top};
      SetCapture(window);
    }
    return 0;
  case WM_MOUSEMOVE:
    if (self->m_dragging) {
      POINT point{};
      GetCursorPos(&point);
      SetWindowPos(window, nullptr,
                   self->m_dragOrigin.x + point.x - self->m_cursorOrigin.x,
                   self->m_dragOrigin.y + point.y - self->m_cursorOrigin.y, 0,
                   0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
  case WM_LBUTTONUP:
    if (self->m_dragging) {
      self->m_dragging = false;
      ReleaseCapture();
      self->ensureOnScreen();
      self->savePosition();
    }
    return 0;
  case WM_CAPTURECHANGED:
    self->m_dragging = false;
    return 0;
  case WM_LBUTTONDBLCLK:
    self->openSettings();
    return 0;
  case WM_RBUTTONUP: {
    POINT point{};
    GetCursorPos(&point);
    self->contextMenu(point);
    return 0;
  }
  case WM_DPICHANGED: {
    auto *rect = reinterpret_cast<RECT *>(lParam);
    SetWindowPos(window, nullptr, rect->left, rect->top, 0, 0,
                 SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    self->renderPet();
    self->ensureOnScreen();
    return 0;
  }
  case WM_DISPLAYCHANGE:
    self->ensureOnScreen();
    return 0;
  case WM_CLOSE:
    self->quit();
    return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}
