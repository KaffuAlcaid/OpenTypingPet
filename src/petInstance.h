#pragma once
#include "characterLibrary.h"
#include "desktopEnvironment.h"
#include "petEngine.h"

enum PauseReason : unsigned { PauseGlobal = 1, PauseLock = 2, PauseDisplay = 4, PauseFullscreen = 8, PauseSleep = 16 };
class PetInstance {
public:
  PetInstance(HINSTANCE instance, DesktopEnvironment &environment, bool interactive);
  ~PetInstance();
  bool initialize();
  void setCharacter(std::shared_ptr<CharacterDefinition> character, const std::set<std::string> &disabled = {});
  void applySettings();
  void setPolicy(unsigned reasons, bool hidden);
  bool paused() const { return record.paused || m_reasons || !record.visible || m_policyHidden; }
  bool acceptsInput() const { return engine.character() && !paused(); }
  void tick(uint64_t now);
  uint64_t deadline() const;
  void render();
  void restorePlacement();
  void capturePlacement();
  void resetPosition();
  void moveToMonitor(const std::wstring &id);
  HWND window() const { return m_window; }
  size_t frame() const { return m_frame; }
  PetRecord record;
  PetEngine engine;
  std::shared_ptr<CharacterDefinition> source;
  std::function<void()> changed, positionChanged, openSettings, closeRequested;
  std::function<void(POINT)> contextRequested;
  std::function<LRESULT(UINT, WPARAM, LPARAM)> menuMessage;
private:
  static LRESULT CALLBACK windowProc(HWND, UINT, WPARAM, LPARAM);
  void onAction();
  void syncPause();
  SIZE targetSize(const DesktopMonitor &monitor) const;
  HINSTANCE m_instance;
  DesktopEnvironment &m_environment;
  bool m_interactive, m_suspended = false, m_policyHidden = false, m_dragging = false, m_positioning = false;
  unsigned m_reasons = 0;
  HWND m_window = nullptr;
  HDC m_dc = nullptr;
  HBITMAP m_bitmap = nullptr, m_oldBitmap = nullptr;
  void *m_pixels = nullptr;
  SIZE m_size{};
  size_t m_frame = 0;
  int m_loops = 0;
  uint64_t m_frameAt = 0, m_pausedAt = 0, m_remainingFrame = 0;
  POINT m_dragOrigin{}, m_cursorOrigin{};
};
