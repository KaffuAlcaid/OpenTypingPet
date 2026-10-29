#pragma once
#include "characterDefinition.h"
#include <functional>
#include <optional>
class PetEngine {
public:
  std::function<void()> changed;
  void setCharacter(std::shared_ptr<CharacterDefinition> character);
  std::shared_ptr<CharacterDefinition> character() const { return m_character; }
  size_t action() const { return m_action; }
  void input(int key, bool shift, bool ctrl, bool alt, bool repeated,
             bool mouse);
  void handleEvent(const InputEvent &event);
  void animationFinished();
  void preview(size_t action);
  void reset();
  void setTiming(unsigned idle, unsigned hold, uint64_t now = GetTickCount64());
  void tick(uint64_t now);
  uint64_t deadline() const;
  void shiftTime(uint64_t duration);

private:
  void play(size_t action);
  void activate(size_t action, size_t rule);
  std::shared_ptr<CharacterDefinition> m_character;
  size_t m_action = 0;
  std::vector<size_t> m_sequence;
  int m_priority = 0;
  bool m_special = false, m_finished = false;
  unsigned m_idleDelay = 800, m_hold = 0;
  uint64_t m_idleAt = 0, m_specialUntil = 0, m_specialStarted = 0,
           m_lastInput = 0;
  struct Pending { size_t action, rule; };
  std::optional<Pending> m_pending;
};
