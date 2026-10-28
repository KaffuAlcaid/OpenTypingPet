#include "petEngine.h"
#include <algorithm>

void PetEngine::setCharacter(std::shared_ptr<CharacterDefinition> character) {
  m_character = std::move(character);
  m_sequence.assign(m_character ? m_character->rules.size() : 0, 0);
  reset();
}
void PetEngine::play(size_t action) {
  if (!m_character || action >= m_character->actions.size())
    return;
  m_action = action;
  m_finished = false;
  if (changed)
    changed();
}
void PetEngine::reset() {
  m_idleAt = m_specialUntil = m_specialStarted = m_lastInput = 0;
  m_priority = 0;
  m_special = m_finished = false;
  m_pending.reset();
  if (m_character)
    play(m_character->actionIndex(m_character->defaultAction));
}
void PetEngine::preview(size_t action) {
  if (!m_character || action >= m_character->actions.size())
    return;
  m_pending.reset();
  m_specialUntil = m_specialStarted = 0;
  m_lastInput = GetTickCount64();
  m_idleAt = m_lastInput + m_idleDelay;
  m_priority = 0;
  m_special = false;
  play(action);
}
void PetEngine::setTiming(unsigned idle, unsigned hold) {
  m_idleDelay = idle;
  m_hold = hold;
  if (m_character && m_specialStarted) {
    const auto duration = std::max(m_character->actions[m_action].minimumHold, m_special ? m_hold : 0u);
    m_specialUntil = m_specialStarted + duration;
  }
  if (m_idleAt)
    m_idleAt = std::max(m_lastInput + m_idleDelay, m_specialUntil);
  tick(GetTickCount64());
}
void PetEngine::activate(size_t action, size_t ruleIndex) {
  const auto now = GetTickCount64();
  const auto &rule = m_character->rules[ruleIndex];
  m_priority = rule.priority;
  m_special = rule.event != InputEventType::Typing;
  m_lastInput = m_specialStarted = now;
  m_specialUntil = now + std::max(m_character->actions[action].minimumHold, m_special ? m_hold : 0u);
  m_idleAt = std::max(now + m_idleDelay, m_specialUntil);
  m_pending.reset();
  play(action);
}
void PetEngine::input(int key, bool shift, bool ctrl, bool alt, bool repeated, bool mouse) {
  handleEvent({mouse ? InputEventType::MouseDown : InputEventType::KeyDown,
               key, shift, ctrl, alt, repeated, false});
}
void PetEngine::handleEvent(const InputEvent &event) {
  if (!m_character)
    return;
  size_t selected = m_character->rules.size();
  for (size_t i = 0; i < m_character->rules.size(); ++i) {
    const auto &rule = m_character->rules[i];
    if (rule.event == event.type && rule.key == event.key &&
        rule.shift == event.shift && rule.ctrl == event.ctrl && rule.alt == event.alt &&
        (!rule.modifierTap || event.modifierTap)) {
      selected = i;
      break;
    }
  }
  if (selected == m_character->rules.size() && event.type == InputEventType::KeyDown &&
      event.key != VK_SHIFT && event.key != VK_CONTROL && event.key != VK_MENU &&
      event.key != VK_LWIN && event.key != VK_RWIN)
    for (size_t i = 0; i < m_character->rules.size(); ++i)
      if (m_character->rules[i].event == InputEventType::Typing) {
        selected = i;
        break;
      }
  if (selected == m_character->rules.size())
    return;
  const auto &rule = m_character->rules[selected];
  if (event.repeated && rule.repeat == RepeatBehavior::Ignore)
    return;
  const auto action = m_character->actionIndex(rule.actions[m_sequence[selected] % rule.actions.size()]);
  if (action == CharacterDefinition::noAction)
    return;
  const auto now = GetTickCount64();
  m_lastInput = now;
  m_idleAt = std::max(now + m_idleDelay, m_specialUntil);
  if (rule.repeat == RepeatBehavior::Continue && action == m_action)
    return;
  m_sequence[selected] = (m_sequence[selected] + 1) % rule.actions.size();
  if ((!m_character->actions[m_action].interruptible && !m_finished) ||
      (now < m_specialUntil && rule.priority < m_priority)) {
    m_pending = Pending{action, selected};
    return;
  }
  activate(action, selected);
}
void PetEngine::animationFinished() {
  m_finished = true;
  tick(GetTickCount64());
}
void PetEngine::tick(uint64_t now) {
  if (!m_character)
    return;
  const bool canLeave = m_character->actions[m_action].interruptible || m_finished;
  if (m_pending && now >= m_specialUntil && canLeave) {
    const auto pending = *m_pending;
    activate(pending.action, pending.rule);
    return;
  }
  if (m_finished && now >= m_specialUntil) {
    const auto &next = m_character->actions[m_action].nextAction;
    if (!next.empty()) {
      const auto action = m_character->actionIndex(next);
      if (action != CharacterDefinition::noAction) {
        m_specialStarted = now;
        m_specialUntil = now + m_character->actions[action].minimumHold;
        m_idleAt = action == m_character->actionIndex(m_character->defaultAction)
                       ? 0 : std::max(now + m_idleDelay, m_specialUntil);
        play(action);
        return;
      }
    }
  }
  if (m_idleAt && now >= m_idleAt && canLeave)
    reset();
}
uint64_t PetEngine::deadline() const {
  if (m_character && !m_character->actions[m_action].interruptible && !m_finished)
    return 0;
  uint64_t result = m_idleAt;
  if ((m_pending || (m_finished && m_character && !m_character->actions[m_action].nextAction.empty())) &&
      m_specialUntil)
    result = result ? std::min(result, m_specialUntil) : m_specialUntil;
  return result;
}
