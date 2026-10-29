#pragma once
#include "characterDefinition.h"
#include <cstdint>
#include <functional>
#include <set>
#include <windows.h>
class InputSource {
public:
  InputSource();
  ~InputSource();
  std::function<void(const InputEvent &)> input;
  bool setEnabled(bool enabled, bool mouse);
  bool setDevices(bool keyboard, bool mouse);
  void clear();
  void keyEvent(int key, bool down, bool mouse);

private:
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  void readInput(HRAWINPUT handle);
  bool held(int generic, int left, int right) const;
  HWND m_window = nullptr;
  std::set<int> m_pressed, m_unusedModifiers, m_suppressed;
  bool m_enabled = false, m_registered = false, m_keyboard = false, m_mouse = false;
};
