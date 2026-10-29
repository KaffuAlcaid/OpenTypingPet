#include "characterManifest.h"
#include "animationDecoder.h"
#include "animationCache.h"
#include "storageUtils.h"
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
    if (!std::any_of(allowed.begin(), allowed.end(), [&](const char *field) { return key == field; }))
      throw std::wstring(L"未知字段：") + Storage::wide(key);
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
  if (name.starts_with("VK") && name.size() >= 3 && name.size() <= 5 &&
      std::all_of(name.begin() + 2, name.end(), [](char c) { return c >= '0' && c <= '9'; })) {
    const int code = std::stoi(name.substr(2));
    if (code > 0 && code < 255) return code;
  }
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
  std::string location;
  std::string file = manifestPath;
  const auto &bytes = resources.at(manifestPath);
  try {
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
    location = "formatVersion";
    require(number(document.at("formatVersion"), 1000) == 1, L"不支持此角色包格式版本。");
    auto character = std::make_shared<CharacterDefinition>();
    location = "id"; character->id = identifier(document.at("id"));
    location = "name"; character->name = label(document.at("name"));
    location = "defaultAction"; character->defaultAction = identifier(document.at("defaultAction"));
    location = "animations";
    const auto &animations = document.at("animations");
    location = "actions";
    const auto &actions = document.at("actions");
    location = "rules";
    const auto &rules = document.at("rules");
    require(animations.is_array() && !animations.empty() && animations.size() <= 64 &&
                actions.is_array() && !actions.empty() && actions.size() <= 64 &&
                rules.is_array() && rules.size() <= 128, L"角色包的动画、动作或规则数量无效。");
    const auto slash = manifestPath.find_last_of('/');
    const auto root = slash == std::string::npos ? "" : manifestPath.substr(0, slash + 1);
    std::vector<ImportDiagnostic> missing;
    for (size_t i = 0; i < animations.size(); ++i)
      if (animations[i].is_object() && animations[i].contains("source") && animations[i]["source"].is_string()) {
        const auto path = root + resourcePath(animations[i]["source"].get<std::string>());
        if (!resources.contains(path))
          missing.push_back({Storage::wide(manifestPath), L"animations[" + std::to_wstring(i) + L"].source",
                             L"找不到资源：" + Storage::wide(path)});
      }
    if (!missing.empty()) throw ImportFailure{std::move(missing)};
    uint64_t budget = 0;
    std::map<std::string, size_t> animationIds, decoded;
    size_t objectIndex = 0;
    for (const auto &animation : animations) {
      const auto animationLocation = "animations[" + std::to_string(objectIndex++) + "]";
      location = animationLocation;
      fields(animation, {"id", "source"});
      location += ".id";
      const auto id = identifier(animation.at("id"));
      require(!animationIds.contains(id), L"动画标识重复。");
      location = animationLocation + ".source";
      const auto path = root + resourcePath(animation.at("source").get<std::string>());
      require(resources.contains(path), L"角色包缺少动画引用的资源。");
      if (!decoded.contains(path)) {
        decoded[path] = character->clips.size();
        file = path;
        auto clip = cachedAnimation(resources.at(path), label(animation.at("id")), budget);
        file = manifestPath;
        clip.id = id;
        clip.source = path;
        character->clips.push_back(std::move(clip));
      }
      animationIds[id] = decoded.at(path);
    }
    std::set<std::string> actionIds;
    objectIndex = 0;
    for (const auto &item : actions) {
      location = "actions[" + std::to_string(objectIndex++) + "]";
      fields(item, {"id", "name", "animation", "loopCount", "minimumHoldMs", "interruptible", "nextAction"});
      ActionDefinition action;
      action.id = identifier(item.at("id"));
      action.label = label(item.value("name", Json(action.id)));
      require(actionIds.insert(action.id).second, L"动作标识重复。");
      const auto animation = identifier(item.at("animation"));
      if (!animationIds.contains(animation)) {
        location += ".animation";
        throw std::wstring(L"引用的动画不存在：") + Storage::wide(animation);
      }
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
    location = "defaultAction";
    if (!actionIds.contains(character->defaultAction))
      throw std::wstring(L"默认动作不存在：") + Storage::wide(character->defaultAction);
    for (size_t i = 0; i < character->actions.size(); ++i) {
      const auto &action = character->actions[i]; location = "actions[" + std::to_string(i) + "].nextAction";
      require(action.nextAction.empty() || actionIds.contains(action.nextAction), L"动作的结束去向不存在。");
    }
    std::set<std::string> ruleIds, triggers;
    objectIndex = 0;
    for (const auto &item : rules) {
      const auto ruleLocation = "rules[" + std::to_string(objectIndex++) + "]";
      location = ruleLocation;
      fields(item, {"id", "event", "key", "modifiers", "actions", "priority", "repeat"});
      InputRule rule;
      rule.id = identifier(item.at("id"));
      require(ruleIds.insert(rule.id).second, L"规则标识重复。");
      location = ruleLocation + ".event";
      const auto event = item.at("event").get<std::string>();
      if (event == "keyDown") rule.event = InputEventType::KeyDown;
      else if (event == "keyUp") rule.event = InputEventType::KeyUp;
      else if (event == "mouseDown") rule.event = InputEventType::MouseDown;
      else if (event == "mouseUp") rule.event = InputEventType::MouseUp;
      else if (event == "typing") rule.event = InputEventType::Typing;
      else throw std::wstring(L"不支持此事件类型。");
      location = ruleLocation + ".modifiers";
      const auto modifiers = item.value("modifiers", Json::object());
      fields(modifiers, {"shift", "ctrl", "alt"});
      rule.shift = modifiers.value("shift", false);
      rule.ctrl = modifiers.value("ctrl", false);
      rule.alt = modifiers.value("alt", false);
      if (rule.event != InputEventType::Typing) {
        location = ruleLocation + ".key";
        const auto key = item.at("key").get<std::string>();
        rule.key = keyCode(key);
        rule.modifierTap = rule.event == InputEventType::KeyUp &&
                           (rule.key == VK_SHIFT || rule.key == VK_CONTROL || rule.key == VK_MENU);
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
      location = ruleLocation + ".priority";
      rule.priority = int(number(item.value("priority", Json(0)), 1000));
      location = ruleLocation + ".repeat";
      const auto repeat = item.value("repeat", rule.event == InputEventType::Typing ? "restart" : "ignore");
      if (repeat == "restart") rule.repeat = RepeatBehavior::Restart;
      else if (repeat == "continue") rule.repeat = RepeatBehavior::Continue;
      else require(repeat == "ignore", L"不支持此重复触发行为。");
      location = ruleLocation + ".actions";
      require(item.at("actions").is_array() && !item.at("actions").empty() && item.at("actions").size() <= 16,
              L"规则必须引用 1 至 16 个动作。");
      for (const auto &target : item.at("actions")) {
        const auto action = identifier(target);
        location = ruleLocation + ".actions[" + std::to_string(rule.actions.size()) + "]";
        if (!actionIds.contains(action))
          throw std::wstring(L"引用的动作不存在：") + Storage::wide(action);
        rule.actions.push_back(action);
      }
      location = ruleLocation;
      const auto trigger = event + ":" + std::to_string(rule.key) + ":" +
                           std::to_string(rule.ctrl) + std::to_string(rule.shift) + std::to_string(rule.alt);
      require(triggers.insert(trigger).second, L"规则的触发条件重复。");
      character->rules.push_back(std::move(rule));
    }
    const auto &idle = character->animation(character->actionIndex(character->defaultAction));
    character->width = idle.width;
    character->height = idle.height;
    if (document.contains("canvas")) {
      location = "canvas";
      const auto &canvas = document.at("canvas");
      fields(canvas, {"width", "height"});
      character->width = number(canvas.at("width"), 2048);
      character->height = number(canvas.at("height"), 2048);
      require(character->width && character->height, L"角色画布尺寸不能为零。");
    }
    std::stable_sort(character->rules.begin(), character->rules.end(),
                      [](const auto &a, const auto &b) { return a.priority > b.priority; });
    return character;
  } catch (const Json::parse_error &error) {
    size_t line = 1, column = 1;
    for (size_t i = 0; i < std::min(error.byte ? error.byte - 1 : 0, bytes.size()); ++i)
      if (bytes[i] == '\n') { ++line; column = 1; }
      else if ((bytes[i] & 0xc0) != 0x80) ++column;
    throw ImportFailure{{{Storage::wide(manifestPath), {}, L"JSON 语法错误，请检查括号、引号与逗号", line, column}}};
  } catch (const Json::exception &) {
    throw ImportFailure{{{Storage::wide(file), Storage::wide(location), L"缺少必需字段或字段类型错误"}}};
  } catch (const std::wstring &message) {
    throw ImportFailure{{{Storage::wide(file), Storage::wide(location), message}}};
  }
}
int inputKeyCode(const std::string &name) { return keyCode(name); }
std::string inputKeyName(int key) {
  if ((key >= 'A' && key <= 'Z') || (key >= '0' && key <= '9')) return std::string(1, char(key));
  if (key >= VK_F1 && key <= VK_F24) return "F" + std::to_string(key - VK_F1 + 1);
  for (const auto *name : {"Backspace", "Tab", "Enter", "Shift", "Ctrl", "Alt", "CapsLock", "Esc", "Space", "PageUp",
                           "PageDown", "End", "Home", "Left", "Up", "Right", "Down", "Insert", "Delete", "MouseLeft",
                           "MouseRight", "MouseMiddle", "MouseX1", "MouseX2"})
    if (keyCode(name) == key) return name;
  return key > 0 && key < 255 ? "VK" + std::to_string(key) : std::string();
}
std::string animationResourceName(const AnimationClip &clip) {
  auto name = clip.id;
  std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return c >= 'A' && c <= 'Z' ? char(c + 32) : char(c); });
  if (name != clip.id) name += "~" + Storage::hash({clip.id.begin(), clip.id.end()});
  return "images/" + name + (clip.format == L"APNG" ? ".apng" : clip.format == L"GIF" ? ".gif" : clip.format == L"JPEG" ? ".jpg" : ".png");
}
nlohmann::json characterManifest(const CharacterDefinition &character) {
  Json result{{"formatVersion", 1}, {"id", character.id}, {"name", Storage::utf8(character.name)},
              {"canvas", {{"width", character.width}, {"height", character.height}}}, {"defaultAction", character.defaultAction},
              {"animations", Json::array()}, {"actions", Json::array()}, {"rules", Json::array()}};
  for (const auto &clip : character.clips)
    result["animations"].push_back({{"id", clip.id}, {"source", animationResourceName(clip)}});
  for (const auto &action : character.actions) {
    Json item{{"id", action.id}, {"name", Storage::utf8(action.label)}, {"animation", character.clips[action.clip].id},
              {"minimumHoldMs", action.minimumHold}, {"interruptible", action.interruptible}};
    if (action.loops) item["loopCount"] = *action.loops < 0 ? 0 : *action.loops;
    if (!action.nextAction.empty()) item["nextAction"] = action.nextAction;
    result["actions"].push_back(std::move(item));
  }
  for (const auto &rule : character.rules) {
    const char *events[] = {"keyDown", "keyUp", "mouseDown", "mouseUp", "typing"};
    Json item{{"id", rule.id}, {"event", events[int(rule.event)]}, {"actions", rule.actions}, {"priority", rule.priority},
              {"repeat", rule.repeat == RepeatBehavior::Restart ? "restart" : rule.repeat == RepeatBehavior::Continue ? "continue" : "ignore"}};
    if (rule.event != InputEventType::Typing) {
      const auto name = inputKeyName(rule.key);
      if (name.empty()) throw std::wstring(L"此按键暂不能导出为角色包");
      item["key"] = name;
      item["modifiers"] = {{"shift", rule.shift}, {"ctrl", rule.ctrl}, {"alt", rule.alt}};
    }
    result["rules"].push_back(std::move(item));
  }
  return result;
}
