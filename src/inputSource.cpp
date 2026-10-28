#include "inputSource.h"
#include <vector>
#include <wtsapi32.h>
namespace {
int modifier(int key) {
  if (key == VK_SHIFT || key == VK_LSHIFT || key == VK_RSHIFT)
    return VK_SHIFT;
  if (key == VK_CONTROL || key == VK_LCONTROL || key == VK_RCONTROL)
    return VK_CONTROL;
  if (key == VK_MENU || key == VK_LMENU || key == VK_RMENU)
    return VK_MENU;
  return 0;
}
} // namespace
InputSource::InputSource() {
  WNDCLASSW cls{};
  cls.lpfnWndProc = windowProc;
  cls.hInstance = GetModuleHandleW(nullptr);
  cls.lpszClassName = L"OpenTypingPetInput";
  RegisterClassW(&cls);
  m_window = CreateWindowExW(0, cls.lpszClassName, L"", 0, 0, 0, 0, 0,
                             HWND_MESSAGE, nullptr, cls.hInstance, this);
  if (m_window)
    WTSRegisterSessionNotification(m_window, NOTIFY_FOR_THIS_SESSION);
}
InputSource::~InputSource() {
  setEnabled(false, false);
  if (m_window) {
    WTSUnRegisterSessionNotification(m_window);
    DestroyWindow(m_window);
  }
}
void InputSource::clear() {
  m_pressed.clear();
  m_unusedModifiers.clear();
}
bool InputSource::setEnabled(bool enabled, bool mouse) {
  if (!m_window)
    return false;
  m_enabled = false;
  clear();
  if (m_registered) {
    RAWINPUTDEVICE devices[] = {{1, 6, RIDEV_REMOVE, nullptr},
                                {1, 2, RIDEV_REMOVE, nullptr}};
    if (!RegisterRawInputDevices(devices, m_mouse ? 2 : 1,
                                 sizeof(RAWINPUTDEVICE)))
      return false;
  }
  m_registered = false;
  m_mouse = false;
  if (!enabled)
    return true;
  RAWINPUTDEVICE devices[] = {{1, 6, RIDEV_INPUTSINK, m_window},
                              {1, 2, RIDEV_INPUTSINK, m_window}};
  if (!RegisterRawInputDevices(devices, mouse ? 2 : 1, sizeof(RAWINPUTDEVICE)))
    return false;
  m_enabled = true;
  m_registered = true;
  m_mouse = mouse;
  return true;
}
bool InputSource::held(int generic, int left, int right) const {
  return m_pressed.contains(generic) || m_pressed.contains(left) ||
         m_pressed.contains(right);
}
void InputSource::keyEvent(int key, bool down, bool mouse) {
  if (key <= 0 || key >= 255)
    return;
  const int mod = modifier(key);
  const bool repeated = m_pressed.contains(key),
             solo = !down && m_unusedModifiers.contains(key);
  if (!down && !repeated)
    return;
  if (down) {
    m_pressed.insert(key);
    if (mod) {
      if (!repeated)
        m_unusedModifiers.insert(key);
    } else
      m_unusedModifiers.clear();
  } else {
    m_pressed.erase(key);
    m_unusedModifiers.erase(key);
  }
  if (input)
    input({mouse ? (down ? InputEventType::MouseDown : InputEventType::MouseUp)
                 : (down ? InputEventType::KeyDown : InputEventType::KeyUp),
           mod ? mod : key,
           mod != VK_SHIFT && held(VK_SHIFT, VK_LSHIFT, VK_RSHIFT),
           mod != VK_CONTROL && held(VK_CONTROL, VK_LCONTROL, VK_RCONTROL),
           mod != VK_MENU && held(VK_MENU, VK_LMENU, VK_RMENU), down && repeated, solo});
}
void InputSource::readInput(HRAWINPUT handle) {
  if (!m_enabled)
    return;
  UINT size = 0;
  if (GetRawInputData(handle, RID_INPUT, nullptr, &size,
                      sizeof(RAWINPUTHEADER)) != 0 ||
      size < sizeof(RAWINPUTHEADER) || size > 4096)
    return;
  std::vector<uint8_t> bytes(size);
  if (GetRawInputData(handle, RID_INPUT, bytes.data(), &size,
                      sizeof(RAWINPUTHEADER)) != size)
    return;
  const auto *raw = reinterpret_cast<const RAWINPUT *>(bytes.data());
  if (raw->header.dwType == RIM_TYPEKEYBOARD) {
    const auto &key = raw->data.keyboard;
    int vk = key.VKey;
    if (vk == VK_SHIFT)
      vk = int(MapVirtualKeyW(key.MakeCode, MAPVK_VSC_TO_VK_EX));
    else if (vk == VK_CONTROL)
      vk = (key.Flags & RI_KEY_E0) ? VK_RCONTROL : VK_LCONTROL;
    else if (vk == VK_MENU)
      vk = (key.Flags & RI_KEY_E0) ? VK_RMENU : VK_LMENU;
    keyEvent(vk, !(key.Flags & RI_KEY_BREAK), false);
  } else if (raw->header.dwType == RIM_TYPEMOUSE && m_mouse) {
    const int keys[] = {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1,
                        VK_XBUTTON2};
    for (int i = 0; i < 5; ++i) {
      if (raw->data.mouse.usButtonFlags & (1 << (i * 2)))
        keyEvent(keys[i], true, true);
      if (raw->data.mouse.usButtonFlags & (2 << (i * 2)))
        keyEvent(keys[i], false, true);
    }
  }
}
LRESULT CALLBACK InputSource::windowProc(HWND window, UINT message,
                                         WPARAM wParam, LPARAM lParam) {
  auto *self =
      reinterpret_cast<InputSource *>(GetWindowLongPtrW(window, GWLP_USERDATA));
  if (message == WM_NCCREATE) {
    self = static_cast<InputSource *>(
        reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams);
    SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
  }
  if (self && message == WM_INPUT)
    self->readInput(reinterpret_cast<HRAWINPUT>(lParam));
  if (self &&
      (message == WM_WTSSESSION_CHANGE || message == WM_POWERBROADCAST)) {
    self->clear();
    if (self->sessionChanged)
      self->sessionChanged();
  }
  return DefWindowProcW(window, message, wParam, lParam);
}
