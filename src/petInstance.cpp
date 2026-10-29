#include "petInstance.h"
#include "animationRenderer.h"
#include "storageUtils.h"
#include <windowsx.h>

PetInstance::PetInstance(HINSTANCE instance, DesktopEnvironment &environment, bool interactive)
    : m_instance(instance), m_environment(environment), m_interactive(interactive) {
  record.instanceId = Storage::newId();
  engine.changed = [this] { onAction(); };
}
PetInstance::~PetInstance() {
  if (m_window) DestroyWindow(m_window);
  if (m_dc) { SelectObject(m_dc, m_oldBitmap); DeleteObject(m_bitmap); DeleteDC(m_dc); }
}
bool PetInstance::initialize() {
  WNDCLASSEXW type{sizeof(type)};
  type.style = CS_DBLCLKS; type.lpfnWndProc = windowProc; type.hInstance = m_instance;
  type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = L"OpenTypingPetDesktop";
  RegisterClassExW(&type);
  m_window = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, type.lpszClassName,
                              L"OpenTypingPet - 桌宠", WS_POPUP, 0, 0, 280, 280, nullptr, nullptr, m_instance, this);
  return m_window != nullptr;
}
void PetInstance::setCharacter(std::shared_ptr<CharacterDefinition> character, const std::set<std::string> &disabled) {
  source = std::move(character);
  std::shared_ptr<CharacterDefinition> effective;
  if (source) {
    effective = std::make_shared<CharacterDefinition>(*source);
    for (const auto &replacement : record.ruleOverrides) {
      if (replacement.actions.empty() || std::any_of(replacement.actions.begin(), replacement.actions.end(),
          [&](const auto &id) { return effective->actionIndex(id) == CharacterDefinition::noAction; })) continue;
      std::erase_if(effective->rules, [&](const auto &rule) {
        return rule.id != replacement.id && rule.event == replacement.event && rule.key == replacement.key &&
               rule.ctrl == replacement.ctrl && rule.shift == replacement.shift && rule.alt == replacement.alt;
      });
      const auto found = std::find_if(effective->rules.begin(), effective->rules.end(), [&](const auto &rule) { return rule.id == replacement.id; });
      if (found == effective->rules.end()) effective->rules.push_back(replacement); else *found = replacement;
    }
    std::erase_if(effective->rules, [&](const auto &rule) { return disabled.contains(rule.id) || record.disabledRules.contains(rule.id); });
    std::stable_sort(effective->rules.begin(), effective->rules.end(), [](const auto &a, const auto &b) { return a.priority > b.priority; });
    SetWindowTextW(m_window, (L"OpenTypingPet - " + effective->name).c_str());
  }
  engine.setCharacter(std::move(effective));
  applySettings();
  restorePlacement();
  if (!source) ShowWindow(m_window, SW_HIDE);
}
void PetInstance::applySettings() {
  engine.setTiming(record.settings.idleDelay, record.settings.minimumHold, m_suspended ? m_pausedAt : GetTickCount64());
  auto style = GetWindowLongPtrW(m_window, GWL_EXSTYLE);
  style = record.settings.locked ? style | WS_EX_TRANSPARENT : style & ~LONG_PTR(WS_EX_TRANSPARENT);
  SetWindowLongPtrW(m_window, GWL_EXSTYLE, style);
  SetWindowPos(m_window, record.settings.topmost ? HWND_TOPMOST : HWND_NOTOPMOST, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE | SWP_FRAMECHANGED);
  syncPause();
  render();
}
void PetInstance::setPolicy(unsigned reasons, bool hidden) {
  m_reasons = reasons; m_policyHidden = hidden; syncPause();
  if (m_interactive && engine.character()) ShowWindow(m_window, record.visible && !hidden ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void PetInstance::syncPause() {
  const bool pause = paused();
  const auto now = GetTickCount64();
  if (pause && !m_suspended) {
    m_pausedAt = now; m_remainingFrame = m_frameAt ? std::max<uint64_t>(1, m_frameAt > now ? m_frameAt - now : 1) : 0;
    m_frameAt = 0;
  } else if (!pause && m_suspended) {
    engine.shiftTime(now - m_pausedAt);
    if (m_remainingFrame) m_frameAt = now + m_remainingFrame;
  }
  m_suspended = pause;
}
void PetInstance::onAction() {
  if (m_suspended) m_pausedAt = GetTickCount64();
  m_frame = 0; m_loops = 0; m_frameAt = 0; m_remainingFrame = 0;
  if (const auto character = engine.character()) {
    const auto &clip = character->animation(engine.action());
    const auto &action = character->actions[engine.action()];
    if (clip.frames.size() > 1 || !action.nextAction.empty() || !action.interruptible) {
      m_remainingFrame = clip.frames[0].delay;
      if (!paused()) m_frameAt = GetTickCount64() + m_remainingFrame;
    }
  }
  render();
  if (changed) changed();
}
void PetInstance::tick(uint64_t now) {
  if (paused()) return;
  engine.tick(now);
  const auto character = engine.character();
  if (!character) return;
  const auto action = engine.action();
  const auto &clip = character->animation(action);
  bool dirty = false;
  int advanced = 0;
  while (m_frameAt && now >= m_frameAt && advanced++ < 300) {
    if (m_frame + 1 >= clip.frames.size()) {
      ++m_loops;
      const auto loops = character->loopCount(action);
      if (loops >= 0 && m_loops >= loops) { m_frameAt = 0; engine.animationFinished(); break; }
      m_frame = 0;
    } else ++m_frame;
    m_frameAt += clip.frames[m_frame].delay; dirty = true;
  }
  if (advanced >= 300 && m_frameAt) m_frameAt = now + clip.frames[m_frame].delay;
  if (dirty) { render(); if (changed) changed(); }
}
uint64_t PetInstance::deadline() const {
  if (paused()) return 0;
  const auto deadline = engine.deadline();
  return m_frameAt ? (deadline ? std::min(deadline, m_frameAt) : m_frameAt) : deadline;
}
SIZE PetInstance::targetSize(const DesktopMonitor &monitor) const {
  const auto character = engine.character();
  if (!character) return {280, 280};
  const double width = MulDiv(record.settings.width, monitor.dpi, 96);
  const double height = width * character->height / character->width;
  const double fit = std::min({1.0, double(monitor.work.right - monitor.work.left) / width,
                              double(monitor.work.bottom - monitor.work.top) / height});
  return {LONG(std::max(1.0, std::round(width * fit))), LONG(std::max(1.0, std::round(height * fit)))};
}
void PetInstance::render() {
  const auto character = engine.character();
  if (!m_window || !character) return;
  const auto &monitor = m_environment.monitor({}, m_window);
  auto size = targetSize(monitor);
  if (m_size.cx != size.cx || m_size.cy != size.cy) {
    if (m_dc) { SelectObject(m_dc, m_oldBitmap); DeleteObject(m_bitmap); DeleteDC(m_dc); m_dc = nullptr; }
    BITMAPINFO info{}; info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER); info.bmiHeader.biWidth = size.cx;
    info.bmiHeader.biHeight = -size.cy; info.bmiHeader.biPlanes = 1; info.bmiHeader.biBitCount = 32;
    m_dc = CreateCompatibleDC(nullptr);
    m_bitmap = CreateDIBSection(m_dc, &info, DIB_RGB_COLORS, &m_pixels, nullptr, 0);
    if (!m_bitmap) { DeleteDC(m_dc); m_dc = nullptr; m_size = {}; return; }
    m_oldBitmap = static_cast<HBITMAP>(SelectObject(m_dc, m_bitmap)); m_size = size;
  }
  memset(m_pixels, 0, size_t(size.cx) * size.cy * 4);
  {
    Gdiplus::Bitmap bitmap(size.cx, size.cy, size.cx * 4, PixelFormat32bppPARGB, static_cast<BYTE *>(m_pixels));
    Gdiplus::Graphics painter(&bitmap); painter.SetCompositingMode(Gdiplus::CompositingModeSourceCopy);
    paintAnimationFrame(painter, {0, 0, size.cx, size.cy}, character->animation(engine.action()), m_frame, record.settings.smooth);
  }
  RECT rect{}; GetWindowRect(m_window, &rect); POINT position{rect.left, rect.top}, origin{};
  BLENDFUNCTION blend{AC_SRC_OVER, 0, BYTE(record.settings.opacity * 255 / 100), AC_SRC_ALPHA};
  UpdateLayeredWindow(m_window, nullptr, &position, &size, m_dc, &origin, 0, &blend, ULW_ALPHA);
  if (m_interactive) ShowWindow(m_window, record.visible && !m_policyHidden ? SW_SHOWNOACTIVATE : SW_HIDE);
}
void PetInstance::restorePlacement() {
  if (!m_window || m_dragging || m_positioning) return;
  m_positioning = true;
  const auto &monitor = m_environment.monitor(record.placement.monitor, record.placement.valid ? nullptr : m_window);
  const auto size = targetSize(monitor);
  const int travelX = std::max(0L, monitor.work.right - monitor.work.left - size.cx);
  const int travelY = std::max(0L, monitor.work.bottom - monitor.work.top - size.cy);
  int x = monitor.work.left + int(std::round(travelX * record.placement.x));
  int y = monitor.work.top + int(std::round(travelY * record.placement.y));
  if (record.placement.edgeX >= 0) x = monitor.work.left + (record.placement.edgeX ? travelX - MulDiv(record.placement.gapX, monitor.dpi, 96) : MulDiv(record.placement.gapX, monitor.dpi, 96));
  if (record.placement.edgeY >= 0) y = monitor.work.top + (record.placement.edgeY ? travelY - MulDiv(record.placement.gapY, monitor.dpi, 96) : MulDiv(record.placement.gapY, monitor.dpi, 96));
  if (!record.placement.valid && record.settings.hasPosition) { x = record.settings.position.x; y = record.settings.position.y; }
  x = std::clamp(x, int(monitor.work.left), int(monitor.work.left) + travelX);
  y = std::clamp(y, int(monitor.work.top), int(monitor.work.top) + travelY);
  SetWindowPos(m_window, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
  render(); m_positioning = false;
}
void PetInstance::capturePlacement() {
  const auto &monitor = m_environment.monitor({}, m_window);
  RECT rect{}; GetWindowRect(m_window, &rect);
  const int travelX = std::max(1L, monitor.work.right - monitor.work.left - (rect.right - rect.left));
  const int travelY = std::max(1L, monitor.work.bottom - monitor.work.top - (rect.bottom - rect.top));
  record.placement.monitor = monitor.id;
  record.placement.x = std::clamp(double(rect.left - monitor.work.left) / travelX, 0.0, 1.0);
  record.placement.y = std::clamp(double(rect.top - monitor.work.top) / travelY, 0.0, 1.0);
  record.placement.valid = true;
  record.settings.position = {rect.left, rect.top}; record.settings.hasPosition = true;
}
void PetInstance::resetPosition() {
  const auto &monitor = m_environment.monitor(record.placement.monitor, m_window);
  record.placement = {monitor.id, 1, 1, 1, 1, 24, 24, true};
  restorePlacement(); if (positionChanged) positionChanged();
}
void PetInstance::moveToMonitor(const std::wstring &id) {
  record.placement.monitor = id; record.placement.valid = true;
  restorePlacement(); if (positionChanged) positionChanged();
}
LRESULT CALLBACK PetInstance::windowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
  auto *self = reinterpret_cast<PetInstance *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) { self = static_cast<PetInstance *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams); self->m_window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
  if (!self) return DefWindowProcW(window, message, wParam, lParam);
  switch (message) {
  case WM_MOUSEACTIVATE: return MA_NOACTIVATE;
  case WM_DRAWITEM: case WM_MEASUREITEM:
    return self->menuMessage ? self->menuMessage(message, wParam, lParam) : 0;
  case WM_LBUTTONDOWN:
    if (!self->record.settings.locked) {
      self->m_dragging = true; GetCursorPos(&self->m_cursorOrigin);
      RECT rect{}; GetWindowRect(window, &rect); self->m_dragOrigin = {rect.left, rect.top}; SetCapture(window);
      self->record.placement.edgeX = self->record.placement.edgeY = -1;
    }
    return 0;
  case WM_MOUSEMOVE:
    if (self->m_dragging) {
      POINT point{}; GetCursorPos(&point);
      SetWindowPos(window, nullptr, self->m_dragOrigin.x + point.x - self->m_cursorOrigin.x,
                     self->m_dragOrigin.y + point.y - self->m_cursorOrigin.y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
    return 0;
  case WM_LBUTTONUP:
    if (self->m_dragging) {
      self->m_dragging = false; ReleaseCapture(); self->capturePlacement();
      if (self->record.settings.snap && !(GetAsyncKeyState(VK_MENU) & 0x8000)) {
        const auto &monitor = self->m_environment.monitor({}, window);
        RECT rect{}; GetWindowRect(window, &rect); const int threshold = MulDiv(12, monitor.dpi, 96);
        if (abs(rect.left - monitor.work.left) <= threshold) { self->record.placement.edgeX = 0; self->record.placement.gapX = 0; }
        else if (abs(monitor.work.right - rect.right) <= threshold) { self->record.placement.edgeX = 1; self->record.placement.gapX = 0; }
        if (abs(rect.top - monitor.work.top) <= threshold) { self->record.placement.edgeY = 0; self->record.placement.gapY = 0; }
        else if (abs(monitor.work.bottom - rect.bottom) <= threshold) { self->record.placement.edgeY = 1; self->record.placement.gapY = 0; }
      }
      self->restorePlacement(); if (self->positionChanged) self->positionChanged();
    }
    return 0;
  case WM_CAPTURECHANGED: self->m_dragging = false; return 0;
  case WM_LBUTTONDBLCLK: if (self->openSettings) self->openSettings(); return 0;
  case WM_RBUTTONUP: { POINT point{}; GetCursorPos(&point); if (self->contextRequested) self->contextRequested(point); return 0; }
  case WM_DPICHANGED: self->render(); return 0;
  case WM_DISPLAYCHANGE: self->restorePlacement(); return 0;
  case WM_CLOSE: if (self->closeRequested) self->closeRequested(); return 0;
  }
  return DefWindowProcW(window, message, wParam, lParam);
}
