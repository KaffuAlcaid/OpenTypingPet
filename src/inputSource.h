#pragma once
#include <cstdint>
#include <functional>
#include <set>
#include <windows.h>
class InputSource {
public:
  InputSource();
  ~InputSource();
  std::function<void(int, bool, bool, bool, bool, bool)> input;
  std::function<void()> sessionChanged;
  bool setEnabled(bool enabled, bool mouse);
  void clear();
  void keyEvent(int key, bool down, bool mouse);

private:
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  void readInput(HRAWINPUT handle);
  bool held(int generic, int left, int right) const;
  HWND m_window = nullptr;
  std::set<int> m_pressed, m_unusedModifiers;
  bool m_enabled = false, m_registered = false, m_mouse = false;
};
