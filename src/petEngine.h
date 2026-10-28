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
  void preview(size_t action);
  void reset();
  void setTiming(unsigned idle, unsigned hold);
  void tick(uint64_t now);
  uint64_t deadline() const;

private:
  void play(size_t action);
  std::shared_ptr<CharacterDefinition> m_character;
  size_t m_action = 0;
  bool m_left = false;
  unsigned m_idleDelay = 800, m_hold = 0;
  uint64_t m_idleAt = 0, m_specialUntil = 0, m_specialStarted = 0,
           m_lastInput = 0;
  std::optional<size_t> m_pending;
};
