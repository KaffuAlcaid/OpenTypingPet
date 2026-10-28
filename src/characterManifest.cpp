#include "characterManifest.h"
#include "animationDecoder.h"
#include <nlohmann/json.hpp>
#include <set>

namespace {
using Json = nlohmann::json;
void require(bool value, const wchar_t *message) {
  if (!value)
    throw std::wstring(message);
}
void fields(const Json &object, std::initializer_list<const char *> allowed) {
  require(object.is_object(), L"角色配置项必须为对象。");
  for (const auto &[key, value] : object.items()) {
    (void)value;
    require(std::any_of(allowed.begin(), allowed.end(), [&](const char *field) { return key == field; }),
            L"角色配置包含不支持的字段。");
  }
}
unsigned number(const Json &value, unsigned maximum) {
  require(value.is_number_integer() && value >= 0 && value <= maximum, L"角色配置的数值超出允许范围。");
  return value.get<unsigned>();
}
std::string identifier(const Json &value) {
  require(value.is_string(), L"角色、动作和规则标识必须为字符串。");
  const auto result = value.get<std::string>();
  require(!result.empty() && result.size() <= 80 &&
              std::all_of(result.begin(), result.end(), [](unsigned char c) {
                return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
              }), L"标识只能使用英文字母、数字、点、连字符和下划线，且不能超过 80 字节。");
  return result;
}
std::wstring label(const Json &value) {
  require(value.is_string(), L"名称必须为字符串。");
  const auto bytes = value.get<std::string>();
  require(!bytes.empty() && bytes.size() <= 320 && bytes.find('\0') == std::string::npos,
          L"名称为空或过长。");
  const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), nullptr, 0);
  require(count > 0 && count <= 80, L"名称不是有效的 UTF-8 或超过 80 个字符。");
  std::wstring result(count, L'\0');
  MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), int(bytes.size()), result.data(), count);
  require(std::none_of(result.begin(), result.end(), [](wchar_t c) { return c < 32; }), L"名称不能包含控制字符。");
  return result;
}
std::string resourcePath(std::string path) {
  std::replace(path.begin(), path.end(), '\\', '/');
  require(!path.empty() && path.size() <= 240 && path[0] != '/' &&
              path.find(':') == std::string::npos && path.find('\0') == std::string::npos,
          L"动画资源路径无效。");
  size_t offset = 0;
  while (offset < path.size()) {
    const auto end = path.find('/', offset);
    const auto piece = path.substr(offset, end == std::string::npos ? end : end - offset);
    require(!piece.empty() && piece != "." && piece != "..", L"动画资源必须位于角色包内。");
    if (end == std::string::npos)
      break;
    offset = end + 1;
  }
  std::transform(path.begin(), path.end(), path.begin(), [](unsigned char c) {
    return c >= 'A' && c <= 'Z' ? char(c + 'a' - 'A') : char(c);
  });
  return path;
}
int keyCode(const std::string &name) {
  if (name.size() == 1 && ((name[0] >= 'A' && name[0] <= 'Z') || (name[0] >= '0' && name[0] <= '9')))
    return name[0];
  const std::map<std::string, int> keys = {
      {"Backspace", VK_BACK}, {"Tab", VK_TAB}, {"Enter", VK_RETURN}, {"Shift", VK_SHIFT},
      {"Ctrl", VK_CONTROL}, {"Alt", VK_MENU}, {"CapsLock", VK_CAPITAL}, {"Esc", VK_ESCAPE},
      {"Space", VK_SPACE}, {"PageUp", VK_PRIOR}, {"PageDown", VK_NEXT}, {"End", VK_END},
      {"Home", VK_HOME}, {"Left", VK_LEFT}, {"Up", VK_UP}, {"Right", VK_RIGHT}, {"Down", VK_DOWN},
      {"Insert", VK_INSERT}, {"Delete", VK_DELETE}, {"MouseLeft", VK_LBUTTON}, {"MouseRight", VK_RBUTTON},
      {"MouseMiddle", VK_MBUTTON}, {"MouseX1", VK_XBUTTON1}, {"MouseX2", VK_XBUTTON2}};
  if (const auto found = keys.find(name); found != keys.end())
    return found->second;
  for (int i = 1; i <= 24; ++i)
    if (name == "F" + std::to_string(i))
      return VK_F1 + i - 1;
  throw std::wstring(L"规则包含不支持的按键名称。");
}
} // namespace

std::shared_ptr<CharacterDefinition> loadCharacterManifest(
    const CharacterResources &resources, const std::string &manifestPath) {
  try {
    const auto &bytes = resources.at(manifestPath);
    require(bytes.size() <= 65536, L"character.json 不能超过 64 KB。");
    std::vector<std::set<std::string>> objects;
    const auto document = Json::parse(bytes, [&](int depth, Json::parse_event_t event, Json &value) {
      require(depth <= 32, L"角色配置嵌套过深。");
      if (event == Json::parse_event_t::object_start)
        objects.emplace_back();
      else if (event == Json::parse_event_t::object_end)
        objects.pop_back();
      else if (event == Json::parse_event_t::key)
        require(objects.back().insert(value.get<std::string>()).second, L"角色配置包含重复字段。");
      return true;
    });
    fields(document, {"formatVersion", "id", "name", "canvas", "defaultAction", "animations", "actions", "rules"});
    require(number(document.at("formatVersion"), 1000) == 1, L"不支持此角色包格式版本。");
    auto character = std::make_shared<CharacterDefinition>();
    character->id = identifier(document.at("id"));
    character->name = label(document.at("name"));
    character->defaultAction = identifier(document.at("defaultAction"));
    const auto &animations = document.at("animations");
    const auto &actions = document.at("actions");
    const auto &rules = document.at("rules");
    require(animations.is_array() && !animations.empty() && animations.size() <= 64 &&
                actions.is_array() && !actions.empty() && actions.size() <= 64 &&
                rules.is_array() && rules.size() <= 128, L"角色包的动画、动作或规则数量无效。");
    const auto slash = manifestPath.find_last_of('/');
    const auto root = slash == std::string::npos ? "" : manifestPath.substr(0, slash + 1);
    uint64_t budget = 0;
    std::map<std::string, size_t> animationIds, decoded;
    for (const auto &animation : animations) {
      fields(animation, {"id", "source"});
      const auto id = identifier(animation.at("id"));
      require(!animationIds.contains(id), L"动画标识重复。");
      const auto path = root + resourcePath(animation.at("source").get<std::string>());
      require(resources.contains(path), L"角色包缺少动画引用的资源。");
      if (!decoded.contains(path)) {
        decoded[path] = character->clips.size();
        character->clips.push_back(decodeAnimation(resources.at(path), label(animation.at("id")), budget));
      }
      animationIds[id] = decoded.at(path);
    }
    std::set<std::string> actionIds;
    for (const auto &item : actions) {
      fields(item, {"id", "name", "animation", "loopCount", "minimumHoldMs", "interruptible", "nextAction"});
      ActionDefinition action;
      action.id = identifier(item.at("id"));
      action.label = label(item.value("name", Json(action.id)));
      require(actionIds.insert(action.id).second, L"动作标识重复。");
      const auto animation = identifier(item.at("animation"));
      require(animationIds.contains(animation), L"动作引用的动画不存在。");
      action.clip = animationIds.at(animation);
      action.minimumHold = number(item.value("minimumHoldMs", Json(0)), 5000);
      action.interruptible = item.value("interruptible", true);
      if (item.contains("loopCount")) {
        const auto loops = number(item.at("loopCount"), 100000);
        action.loops = loops ? int(loops) : -1;
      }
      if (item.contains("nextAction")) {
        action.nextAction = identifier(item.at("nextAction"));
        require(!action.loops || *action.loops > 0, L"无限循环动作不能设置结束动作。");
        if (!action.loops)
          action.loops = 1;
      }
      if (!action.interruptible) {
        require(!action.loops || *action.loops > 0, L"不可打断的动作不能无限循环。");
        if (!action.loops)
          action.loops = 1;
      }
      character->actions.push_back(std::move(action));
    }
    require(actionIds.contains(character->defaultAction), L"默认动作不存在。");
    for (const auto &action : character->actions)
      require(action.nextAction.empty() || actionIds.contains(action.nextAction), L"动作的结束去向不存在。");
    std::set<std::string> ruleIds, triggers;
    for (const auto &item : rules) {
      fields(item, {"id", "event", "key", "modifiers", "actions", "priority", "repeat"});
      InputRule rule;
      rule.id = identifier(item.at("id"));
      require(ruleIds.insert(rule.id).second, L"规则标识重复。");
      const auto event = item.at("event").get<std::string>();
      if (event == "keyDown") rule.event = InputEventType::KeyDown;
      else if (event == "keyUp") rule.event = InputEventType::KeyUp;
      else if (event == "mouseDown") rule.event = InputEventType::MouseDown;
      else if (event == "mouseUp") rule.event = InputEventType::MouseUp;
      else if (event == "typing") rule.event = InputEventType::Typing;
      else throw std::wstring(L"不支持此事件类型。");
      const auto modifiers = item.value("modifiers", Json::object());
      fields(modifiers, {"shift", "ctrl", "alt"});
      rule.shift = modifiers.value("shift", false);
      rule.ctrl = modifiers.value("ctrl", false);
      rule.alt = modifiers.value("alt", false);
      if (rule.event != InputEventType::Typing) {
        const auto key = item.at("key").get<std::string>();
        rule.key = keyCode(key);
        const bool mouseKey = rule.key == 1 || rule.key == 2 || rule.key == 4 || rule.key == 5 || rule.key == 6;
        require(mouseKey == (rule.event == InputEventType::MouseDown || rule.event == InputEventType::MouseUp),
                L"事件类型与按键不匹配。");
        rule.label = (event.ends_with("Up") ? L"松开 " : L"") +
                     std::wstring(rule.ctrl ? L"Ctrl+" : L"") + (rule.shift ? L"Shift+" : L"") +
                     (rule.alt ? L"Alt+" : L"") + label(Json(key));
      } else {
        require(!item.contains("key") && modifiers.empty(), L"普通打字规则不能指定单独按键或修饰键。");
        rule.label = L"普通打字";
      }
      rule.priority = int(number(item.value("priority", Json(0)), 1000));
      const auto repeat = item.value("repeat", rule.event == InputEventType::Typing ? "restart" : "ignore");
      if (repeat == "restart") rule.repeat = RepeatBehavior::Restart;
      else if (repeat == "continue") rule.repeat = RepeatBehavior::Continue;
      else require(repeat == "ignore", L"不支持此重复触发行为。");
      require(item.at("actions").is_array() && !item.at("actions").empty() && item.at("actions").size() <= 16,
              L"规则必须引用 1 至 16 个动作。");
      for (const auto &target : item.at("actions")) {
        const auto action = identifier(target);
        require(actionIds.contains(action), L"规则引用的动作不存在。");
        rule.actions.push_back(action);
      }
      const auto trigger = event + ":" + std::to_string(rule.key) + ":" +
                           std::to_string(rule.ctrl) + std::to_string(rule.shift) + std::to_string(rule.alt);
      require(triggers.insert(trigger).second, L"规则的触发条件重复。");
      character->rules.push_back(std::move(rule));
    }
    const auto &idle = character->animation(character->actionIndex(character->defaultAction));
    character->width = idle.width;
    character->height = idle.height;
    if (document.contains("canvas")) {
      const auto &canvas = document.at("canvas");
      fields(canvas, {"width", "height"});
      character->width = number(canvas.at("width"), 2048);
      character->height = number(canvas.at("height"), 2048);
      require(character->width && character->height, L"角色画布尺寸不能为零。");
    }
    std::stable_sort(character->rules.begin(), character->rules.end(),
                      [](const auto &a, const auto &b) { return a.priority > b.priority; });
    return character;
  } catch (const Json::exception &) {
    throw std::wstring(L"character.json 语法无效、缺少必需字段或字段类型不正确。");
  }
}
