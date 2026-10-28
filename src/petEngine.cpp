#include "petEngine.h"
#include <algorithm>
void PetEngine::setCharacter(std::shared_ptr<CharacterDefinition> character) {
  m_character = std::move(character);
  m_left = false;
  reset();
}
void PetEngine::play(size_t action) {
  if (!m_character || action >= m_character->clips.size())
    return;
  m_action = action;
  if (changed)
    changed();
}
void PetEngine::reset() {
  m_idleAt = 0;
  m_specialUntil = 0;
  m_specialStarted = 0;
  m_lastInput = 0;
  m_pending.reset();
  play(0);
}
void PetEngine::preview(size_t action) {
  m_pending.reset();
  m_specialUntil = 0;
  m_specialStarted = 0;
  m_lastInput = GetTickCount64();
  m_idleAt = action ? m_lastInput + m_idleDelay : 0;
  play(action);
}
void PetEngine::setTiming(unsigned idle, unsigned hold) {
  const auto now = GetTickCount64();
  m_idleDelay = idle;
  m_hold = hold;
  if (m_specialUntil)
    m_specialUntil = m_specialStarted + m_hold;
  if (m_idleAt)
    m_idleAt = std::max(m_lastInput + m_idleDelay, m_specialUntil);
  tick(now);
}
void PetEngine::input(int key, bool shift, bool ctrl, bool alt, bool repeated,
                      bool mouse) {
  if (!m_character)
    return;
  const auto now = GetTickCount64();
  for (const auto &binding : m_character->bindings) {
    if (binding.key != key || binding.shift != shift || binding.ctrl != ctrl ||
        binding.alt != alt)
      continue;
    if (repeated)
      return;
    m_pending.reset();
    m_lastInput = now;
    m_specialStarted = now;
    m_specialUntil = now + m_hold;
    m_idleAt = now + std::max(m_idleDelay, m_hold);
    play(binding.action);
    return;
  }
  if (mouse || key == 16 || key == 17 || key == 18 || key == 91 || key == 92)
    return;
  m_left = !m_left;
  m_lastInput = now;
  const size_t action = m_left ? 1 : 2;
  if (now < m_specialUntil)
    m_pending = action;
  else {
    m_pending.reset();
    m_specialUntil = 0;
    m_specialStarted = 0;
    play(action);
  }
  m_idleAt = std::max(now + m_idleDelay, m_specialUntil);
}
void PetEngine::tick(uint64_t now) {
  if (m_idleAt && now >= m_idleAt) {
    reset();
    return;
  }
  if (m_pending && now >= m_specialUntil) {
    const auto action = *m_pending;
    m_pending.reset();
    m_specialUntil = 0;
    m_specialStarted = 0;
    play(action);
  }
}
uint64_t PetEngine::deadline() const {
  if (m_pending && m_idleAt)
    return std::min(m_specialUntil, m_idleAt);
  if (m_pending)
    return m_specialUntil;
  return m_idleAt;
}
