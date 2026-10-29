#include "inputSource.h"
#include <vector>
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
}
InputSource::~InputSource() {
  setEnabled(false, false);
  if (m_window) {
    DestroyWindow(m_window);
  }
}
void InputSource::clear() {
  m_pressed.clear();
  m_unusedModifiers.clear();
  m_suppressed.clear();
}
bool InputSource::setEnabled(bool enabled, bool mouse) {
  return setDevices(enabled, enabled && mouse);
}
bool InputSource::setDevices(bool keyboard, bool mouse) {
  if (!m_window)
    return false;
  if (m_keyboard == keyboard && m_mouse == mouse && m_enabled == (keyboard || mouse))
    return true;
  const bool enablingKeyboard = keyboard && !m_keyboard;
  const bool enablingMouse = mouse && !m_mouse;
  m_enabled = false;
  if (!keyboard) {
    std::erase_if(m_pressed, [](int key) { return key != 1 && key != 2 && key != 4 && key != 5 && key != 6; });
    m_unusedModifiers.clear();
  }
  if (!mouse) for (int key : {1, 2, 4, 5, 6}) m_pressed.erase(key);
  std::erase_if(m_suppressed, [&](int key) {
    const bool button = key == 1 || key == 2 || key == 4 || key == 5 || key == 6;
    return button ? !mouse : !keyboard;
  });
  if (m_registered) {
    std::vector<RAWINPUTDEVICE> devices;
    if (m_keyboard) devices.push_back({1, 6, RIDEV_REMOVE, nullptr});
    if (m_mouse) devices.push_back({1, 2, RIDEV_REMOVE, nullptr});
    if (!RegisterRawInputDevices(devices.data(), UINT(devices.size()), sizeof(RAWINPUTDEVICE)))
      return false;
  }
  m_registered = false;
  m_keyboard = false;
  m_mouse = false;
  if (!keyboard && !mouse)
    return true;
  std::vector<RAWINPUTDEVICE> devices;
  if (keyboard) devices.push_back({1, 6, RIDEV_INPUTSINK, m_window});
  if (mouse) devices.push_back({1, 2, RIDEV_INPUTSINK, m_window});
  if (!RegisterRawInputDevices(devices.data(), UINT(devices.size()), sizeof(RAWINPUTDEVICE))) {
    clear();
    return false;
  }
  m_enabled = true;
  m_registered = true;
  m_keyboard = keyboard;
  m_mouse = mouse;
  if (enablingKeyboard)
    for (int key : {VK_LSHIFT, VK_RSHIFT, VK_LCONTROL, VK_RCONTROL, VK_LMENU, VK_RMENU})
      if (GetAsyncKeyState(key) & 0x8000) m_pressed.insert(key);
  // A held input from a capture or pause must be released before it can trigger again.
  for (int key = 1; key < 255; ++key) {
    const bool button = key == 1 || key == 2 || key == 4 || key == 5 || key == 6;
    if ((button ? enablingMouse : enablingKeyboard) && !modifier(key) && (GetAsyncKeyState(key) & 0x8000))
      m_suppressed.insert(key);
  }
  return true;
}
bool InputSource::held(int generic, int left, int right) const {
  return m_pressed.contains(generic) || m_pressed.contains(left) ||
         m_pressed.contains(right);
}
void InputSource::keyEvent(int key, bool down, bool mouse) {
  if (key <= 0 || key >= 255)
    return;
  if (m_suppressed.contains(key)) {
    if (!down) m_suppressed.erase(key);
    return;
  }
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
  RAWINPUT data{};
  UINT size = sizeof(data);
  const auto read = GetRawInputData(handle, RID_INPUT, &data, &size, sizeof(RAWINPUTHEADER));
  if (read == UINT(-1) || read < sizeof(RAWINPUTHEADER))
    return;
  const auto *raw = &data;
  if (raw->header.dwType == RIM_TYPEKEYBOARD && m_keyboard) {
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
  return DefWindowProcW(window, message, wParam, lParam);
}
