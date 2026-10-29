#include "characterEditor.h"
#include "animationRenderer.h"
#include "animationCache.h"
#include "presetLoader.h"
#include "storageUtils.h"
#include "resourceIds.h"
#include <nlohmann/json.hpp>
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <algorithm>
#include <future>
#include <sstream>
#include <shlwapi.h>

namespace {
using Json = nlohmann::json;
std::wstring errorText(const ImportFailure &failure) {
  std::wstring text;
  for (const auto &issue : failure.diagnostics) {
    if (!text.empty()) text += L"\r\n";
    text += issue.file + (issue.field.empty() ? L"" : L" / " + issue.field) + L"：" + issue.message;
  }
  return text;
}
unsigned number(const std::wstring &text, unsigned maximum) {
  if (text.empty() || text.size() > 8 || !std::all_of(text.begin(), text.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
    throw std::wstring(L"请输入有效的非负整数");
  const auto value = std::stoul(text);
  if (value > maximum) throw std::wstring(L"数值超过允许范围：") + std::to_wstring(maximum);
  return unsigned(value);
}
std::string value(const std::vector<FormField> &fields, const std::string &id) {
  for (const auto &field : fields) if (field.id == id) return Storage::utf8(field.value);
  return {};
}
FormField choice(std::string id, std::wstring label, std::wstring selected,
                 std::vector<std::wstring> labels, std::vector<std::wstring> values) {
  FormField field{std::move(id), std::move(label), std::move(selected), FieldKind::Choice};
  field.choices = std::move(labels); field.choiceValues = std::move(values); return field;
}
std::vector<std::string> actionSequence(const std::string &text) {
  std::vector<std::string> result; std::istringstream stream(text); std::string action;
  while (std::getline(stream, action, ',')) {
    const auto begin = action.find_first_not_of(" \t\r\n"), end = action.find_last_not_of(" \t\r\n");
    if (begin != std::string::npos) result.push_back(action.substr(begin, end - begin + 1));
  }
  if (result.empty() || result.size() > 16) throw std::wstring(L"动作序列需要 1 至 16 个动作标识，以英文逗号分隔");
  return result;
}
Json ruleDocument(const InputRule &rule) {
  CharacterDefinition temporary;
  temporary.rules.push_back(rule);
  return characterManifest(temporary)["rules"][0];
}
struct EditorSnapshot {
  Json document;
  std::map<std::string, std::shared_ptr<const std::vector<uint8_t>>> resources;
  std::set<std::string> disabled;
};
enum : unsigned { PageRole=4000, PageImages, PageActions, PageRules, Items=4010, AddItem, EditItem, DeleteItem,
                  ReplaceImage, ToggleRule, UndoEdit, RedoEdit, SaveEdit, ExportEdit, CloseEdit, PlayPreview,
                  PreviousFrame, NextFrame, FrameSlider, SpeedSelector };
class EditorWindow {
public:
  HWND owner = nullptr, window = nullptr;
  ThemeColors colors;
  std::function<void(bool)> captureState;
  EditorSnapshot state;
  std::optional<CharacterEditResult> output;
  bool done = false, dirty = false, playing = true, busy = false;
  unsigned dpi = 96;
  int page = 0, selected = 0;
  size_t frame = 0, clipIndex = 0;
  uint64_t nextFrame = 0;
  double speed = 1;
  std::shared_ptr<CharacterDefinition> preview;
  std::vector<EditorSnapshot> undo, redo;
  std::map<unsigned, HWND> controls;
  HFONT font = nullptr, heading = nullptr;
  HBRUSH brush = nullptr;
  HWND tooltips = nullptr;
  std::map<unsigned,std::wstring> tips;
  std::map<int, std::shared_ptr<Gdiplus::Bitmap>> icons;
  std::wstring message;
  int px(int value) const { return MulDiv(value,dpi,96); }
  CharacterResources materialize(bool enabledOnly = false) const {
    CharacterResources resources;
    for (const auto &[name, bytes] : state.resources) resources[name] = *bytes;
    Json document = state.document;
    if (enabledOnly) std::erase_if(document["rules"].get_ref<Json::array_t &>(), [&](const auto &rule) { return state.disabled.contains(rule.at("id").template get<std::string>()); });
    const auto text = document.dump(2); resources["character.json"] = {text.begin(),text.end()};
    return resources;
  }
  uint64_t historyBytes() const {
    std::set<const std::vector<uint8_t> *> unique; uint64_t total = 0;
    const auto count = [&](const EditorSnapshot &snapshot) {
      for (const auto &[name, bytes] : snapshot.resources) if (unique.insert(bytes.get()).second) total += bytes->size();
    };
    count(state); for (const auto &snapshot : undo) count(snapshot); for (const auto &snapshot : redo) count(snapshot);
    return total;
  }
  void pruneResources() {
    std::set<std::string> referenced;
    for (const auto &item : state.document["animations"]) referenced.insert(item.at("source").get<std::string>());
    std::erase_if(state.resources, [&](const auto &item) { return !referenced.contains(item.first); });
    while (!undo.empty() && (undo.size() > 50 || historyBytes() > 128 * 1024 * 1024)) undo.erase(undo.begin());
  }
  void remember() { undo.push_back(state); if (undo.size() > 50) undo.erase(undo.begin()); redo.clear(); dirty=true; }
  void schedule() {
    KillTimer(window, 1);
    if (!busy && playing && nextFrame) {
      const auto now = GetTickCount64();
      SetTimer(window, 1, UINT(std::clamp<uint64_t>(nextFrame > now ? nextFrame - now : 1, 10, 60000)), nullptr);
    }
  }
  bool validate() {
    busy=true; message=L"正在检查角色包"; InvalidateRect(window,nullptr,FALSE);
    const auto resources=materialize();
    auto future=std::async(std::launch::async,[resources] {
      CoInitializeEx(nullptr,COINIT_MULTITHREADED);
      ImportResult result;
      try { result.character=loadCharacterManifest(resources,"character.json"); }
      catch(const ImportFailure &failure){result.error=errorText(failure);}
      catch(const std::wstring &error){result.error=error;}
      catch(...){result.error=L"角色配置无效";}
      CoUninitialize();return result;
    });
    for(auto [id,control]:controls) EnableWindow(control,FALSE);
    while(future.wait_for(std::chrono::milliseconds(10))!=std::future_status::ready){
      MSG msg{};while(PeekMessageW(&msg,nullptr,0,0,PM_REMOVE)){if(msg.message==WM_QUIT){PostQuitMessage(int(msg.wParam));done=true;break;}TranslateMessage(&msg);DispatchMessageW(&msg);}
    }
    const auto result=future.get();busy=false;
    for(auto [id,control]:controls) EnableWindow(control,TRUE);
    if(!result.character){preview.reset();nextFrame=0;message=result.error;sync();return false;}
    preview=result.character;message.clear();selectPreview();sync();return true;
  }
  void refreshDraftPreview() {
    if (!state.document["actions"].empty()) { validate(); return; }
    preview.reset(); nextFrame = 0; message.clear();
    if (state.document["animations"].empty()) return;
    auto draft = std::make_shared<CharacterDefinition>();
    uint64_t budget = 0;
    for (const auto &item : state.document["animations"]) {
      auto clip = cachedAnimation(*state.resources.at(item["source"].get<std::string>()), L"素材", budget);
      clip.id = item["id"].get<std::string>(); draft->clips.push_back(std::move(clip));
    }
    ActionDefinition action; action.id = "preview"; action.label = L"素材";
    draft->actions.push_back(action); draft->defaultAction = "preview"; preview = std::move(draft);
    selectPreview(); sync();
  }
  void populate() {
    SendMessageW(controls[Items],LB_RESETCONTENT,0,0);
    const char *keys[]={"","animations","actions","rules"};
    if(page){for(const auto &item:state.document[keys[page]]){
      const auto id=item.at("id").get<std::string>();
      std::wstring name=Storage::wide(item.value("name",id));
      if(page==3 && state.disabled.contains(id))name+=L"（已停用）";
      SendMessageW(controls[Items],LB_ADDSTRING,0,reinterpret_cast<LPARAM>(name.c_str()));
    }}
    const int count=int(SendMessageW(controls[Items],LB_GETCOUNT,0,0));selected=std::clamp(selected,0,std::max(0,count-1));
    if(count)SendMessageW(controls[Items],LB_SETCURSEL,selected,0);
    selectPreview();sync();layout();
  }
  void selectPreview(){
    frame=0;nextFrame=0;clipIndex=0;playing=true;
    if(!preview||preview->clips.empty())return;
    try{
      size_t action=preview->actionIndex(preview->defaultAction);
      if(page==1&&selected<int(state.document["animations"].size())){
        const auto id=state.document["animations"][selected]["id"].get<std::string>();
        for(size_t i=0;i<preview->clips.size();++i)if(preview->clips[i].id==id)clipIndex=i;
      }else{
        if(page==2&&selected<int(state.document["actions"].size()))action=preview->actionIndex(state.document["actions"][selected]["id"].get<std::string>());
        if(page==3&&selected<int(state.document["rules"].size()))action=preview->actionIndex(state.document["rules"][selected]["actions"][0].get<std::string>());
        if(action!=CharacterDefinition::noAction)clipIndex=preview->actions[action].clip;
      }
      const auto &clip=preview->clips[clipIndex];
      if(clip.frames.size()>1)nextFrame=GetTickCount64()+uint64_t(clip.frames[0].delay/speed);
      SendMessageW(controls[FrameSlider],TBM_SETRANGE,TRUE,MAKELPARAM(0,int(clip.frames.size()-1)));
      SendMessageW(controls[FrameSlider],TBM_SETPOS,TRUE,0);
    }catch(...){clipIndex=0;}
    InvalidateRect(window,nullptr,FALSE);
  }
  void sync(){
    const char *keys[]={"","animations","actions","rules"};
    const bool has=page && !state.document[keys[page]].empty();
    EnableWindow(controls[EditItem],page==0||has);EnableWindow(controls[DeleteItem],has);
    EnableWindow(controls[ReplaceImage],page==1&&has);EnableWindow(controls[ToggleRule],page==3&&has);
    EnableWindow(controls[UndoEdit],!undo.empty());EnableWindow(controls[RedoEdit],!redo.empty());
    const bool animated=preview&&!preview->clips.empty()&&preview->clips[clipIndex].frames.size()>1;
    for(unsigned id:{PlayPreview,PreviousFrame,NextFrame,FrameSlider,SpeedSelector})EnableWindow(controls[id],animated);
    for(auto [id,control]:controls)InvalidateRect(control,nullptr,FALSE);
    InvalidateRect(window,nullptr,FALSE);
    schedule();
  }
  void layout(){
    RECT rect{};GetClientRect(window,&rect);const int w=MulDiv(rect.right,96,dpi),h=MulDiv(rect.bottom,96,dpi);
    auto place=[&](unsigned id,int x,int y,int width,int height){MoveWindow(controls[id],px(x),px(y),px(width),px(height),TRUE);};
    for(int i=0;i<4;++i)place(PageRole+i,20+i*94,18,90,34);
    place(UndoEdit,w-98,18,34,34);place(RedoEdit,w-58,18,34,34);
    place(Items,20,76,260,h-162);
    place(AddItem,20,h-72,76,32);place(EditItem,102,h-72,76,32);place(DeleteItem,184,h-72,96,32);
    place(ReplaceImage,302,76,112,32);place(ToggleRule,302,76,112,32);
    place(PreviousFrame,322,h-144,34,32);place(PlayPreview,362,h-144,34,32);place(NextFrame,402,h-144,34,32);
    place(FrameSlider,448,h-144,std::max(40,w-600),32);place(SpeedSelector,w-144,h-144,104,160);
    place(ExportEdit,w-330,h-72,96,32);place(SaveEdit,w-226,h-72,128,32);place(CloseEdit,w-90,h-72,70,32);
    ShowWindow(controls[Items],page?SW_SHOWNA:SW_HIDE);ShowWindow(controls[AddItem],page?SW_SHOWNA:SW_HIDE);
    ShowWindow(controls[DeleteItem],page?SW_SHOWNA:SW_HIDE);
    ShowWindow(controls[ReplaceImage],page==1?SW_SHOWNA:SW_HIDE);ShowWindow(controls[ToggleRule],page==3?SW_SHOWNA:SW_HIDE);
    SendMessageW(controls[Items],LB_SETITEMHEIGHT,0,px(44));InvalidateRect(window,nullptr,FALSE);
  }
  void editRole(){
    std::vector<FormField> fields={{"id",L"角色标识",Storage::wide(state.document.value("id",std::string())),FieldKind::Text},
      {"name",L"角色名称",Storage::wide(state.document.value("name",std::string()))},
      {"width",L"画布宽度",std::to_wstring(state.document["canvas"].value("width",500)),FieldKind::Number},
      {"height",L"画布高度",std::to_wstring(state.document["canvas"].value("height",500)),FieldKind::Number}};
    std::vector<std::wstring> labels,values;for(const auto &action:state.document["actions"]){values.push_back(Storage::wide(action["id"].get<std::string>()));labels.push_back(Storage::wide(action.value("name",action["id"].get<std::string>())));}
    if(!values.empty())fields.push_back(choice("default",L"默认动作",Storage::wide(state.document.value("defaultAction",std::string())),labels,values));
    if(!editForm(window,L"角色信息",fields,colors))return;
    const auto width=number(fields[2].value,2048),height=number(fields[3].value,2048);if(!width||!height)throw std::wstring(L"画布尺寸需要大于零");
    remember();state.document["id"]=value(fields,"id");state.document["name"]=value(fields,"name");state.document["canvas"]={{"width",width},{"height",height}};
    if(!values.empty())state.document["defaultAction"]=value(fields,"default");
  }
  std::string nextId(const char *array,const char *prefix){for(int i=1;;++i){auto id=std::string(prefix)+std::to_string(i);bool exists=false;for(const auto &item:state.document[array])exists|=item["id"]==id;if(!exists)return id;}}
  void image(bool replace){
    const auto path=chooseFile(window,false,L"选择动画素材",L"图片与动画",L"*.png;*.apng;*.gif;*.jpg;*.jpeg");if(path.empty())return;
    auto bytes=std::make_shared<const std::vector<uint8_t>>(Storage::read(path,15*1024*1024));uint64_t budget=0;
    auto clip=cachedAnimation(*bytes,L"预览",budget);clip.id=replace?state.document["animations"][selected]["id"].get<std::string>():nextId("animations","animation");
    const auto source=animationResourceName(clip);
    uint64_t total = bytes->size();
    for (const auto &[name, resource] : state.resources) if (name != source) total += resource->size();
    if (total > 96 * 1024 * 1024) throw std::wstring(L"角色素材总量超过 96 MiB");
    remember();state.resources[source]=std::move(bytes);
    if(replace)state.document["animations"][selected]["source"]=source;
    else{state.document["animations"].push_back({{"id",clip.id},{"source",source}});selected=int(state.document["animations"].size())-1;}
  }
  void action(bool add){
    if(state.document["animations"].empty())throw std::wstring(L"请先添加动画素材");
    const auto previous=add?Json{{"id",nextId("actions","action")},{"name","新动作"},{"animation",state.document["animations"][0]["id"]}}:state.document["actions"][selected];
    std::vector<std::wstring> animationIds;for(const auto &animation:state.document["animations"])animationIds.push_back(Storage::wide(animation["id"].get<std::string>()));
    std::vector<FormField> fields={{"id",L"动作标识",Storage::wide(previous["id"].get<std::string>()),FieldKind::Text,{},!add},
      {"name",L"显示名称",Storage::wide(previous.value("name",previous["id"].get<std::string>()))},
      choice("animation",L"动画素材",Storage::wide(previous["animation"].get<std::string>()),animationIds,animationIds),
      {"loops",L"播放次数（0 为循环）",previous.contains("loopCount")?std::to_wstring(previous["loopCount"].get<unsigned>()):L"",FieldKind::Text},
      {"hold",L"保持时间（ms）",std::to_wstring(previous.value("minimumHoldMs",0u)),FieldKind::Number},
      {"interrupt",L"允许其他动作打断",previous.value("interruptible",true)?L"1":L"0",FieldKind::Check}};
    std::vector<std::wstring> names{L"保持当前动作"},ids{L""};for(const auto &a:state.document["actions"]){names.push_back(Storage::wide(a.value("name",a["id"].get<std::string>())));ids.push_back(Storage::wide(a["id"].get<std::string>()));}
    fields.push_back(choice("next",L"播放结束后",Storage::wide(previous.value("nextAction",std::string())),names,ids));
    if(!editForm(window,add?L"新增动作":L"编辑动作",fields,colors))return;
    Json item{{"id",value(fields,"id")},{"name",value(fields,"name")},{"animation",value(fields,"animation")},
      {"minimumHoldMs",number(fields[4].value,5000)},{"interruptible",fields[5].value==L"1"}};
    if(!fields[3].value.empty())item["loopCount"]=number(fields[3].value,100000);
    if(!value(fields,"next").empty())item["nextAction"]=value(fields,"next");
    remember();if(add){state.document["actions"].push_back(item);selected=int(state.document["actions"].size())-1;if(state.document["defaultAction"]=="")state.document["defaultAction"]=item["id"];}
    else state.document["actions"][selected]=item;
  }
  void rule(bool add){
    if(!validate())throw message;
    InputRule previous;const InputRule *pointer=nullptr;
    if(!add){const auto id=state.document["rules"][selected]["id"].get<std::string>();for(const auto &r:preview->rules)if(r.id==id){previous=r;pointer=&previous;break;}}
    const auto result=editInputRule(window,colors,*preview,pointer,preview->defaultAction,captureState);if(!result)return;
    remember();if(add){state.document["rules"].push_back(ruleDocument(*result));selected=int(state.document["rules"].size())-1;}
    else state.document["rules"][selected]=ruleDocument(*result);
  }
  void erase(){
    const char *array=page==1?"animations":page==2?"actions":"rules";
    if(state.document[array].empty())return;const auto id=state.document[array][selected]["id"].get<std::string>();
    if(page==1)for(const auto &a:state.document["actions"])if(a["animation"]==id)throw std::wstring(L"该素材仍被动作引用");
    if(page==2){if(state.document["defaultAction"]==id)throw std::wstring(L"请先选择其他默认动作");for(const auto &r:state.document["rules"])for(const auto &a:r["actions"])if(a==id)throw std::wstring(L"该动作仍被输入规则引用");for(const auto &a:state.document["actions"])if(a.value("nextAction",std::string())==id)throw std::wstring(L"该动作仍被结束跳转引用");}
    remember();state.document[array].erase(state.document[array].begin()+selected);state.disabled.erase(id);
  }
  void command(unsigned id,unsigned notification){
    if(busy)return;
    if(id==Items){if(notification==LBN_SELCHANGE){selected=std::max(0,int(SendMessageW(controls[Items],LB_GETCURSEL,0,0)));selectPreview();sync();}return;}
    if(id==SpeedSelector){if(notification==CBN_SELCHANGE){const double speeds[]={0.25,0.5,1,2};int index=int(SendMessageW(controls[id],CB_GETCURSEL,0,0));if(index>=0&&index<4)speed=speeds[index];}return;}
    if(notification!=BN_CLICKED)return;
    try{
      if(id>=PageRole&&id<=PageRules){page=int(id-PageRole);selected=0;populate();return;}
      if(id==CloseEdit){if(!dirty||chooseAction(window,L"关闭编辑器",L"角色修改尚未应用",{L"放弃修改",L"继续编辑"},colors)==0)done=true;return;}
      if(id==UndoEdit||id==RedoEdit){auto &from=id==UndoEdit?undo:redo;auto &to=id==UndoEdit?redo:undo;if(!from.empty()){to.push_back(state);state=std::move(from.back());from.pop_back();dirty=true;refreshDraftPreview();populate();}return;}
      if(id==SaveEdit||id==ExportEdit){if(!validate()){showDetails(window,L"角色配置问题",message,colors);return;}if(id==SaveEdit){output=CharacterEditResult{materialize(),state.disabled};done=true;}
        else{const auto path=chooseFile(window,true,L"导出角色包");if(!path.empty()){PresetLoader::writePackage(path,materialize(true));message=L"角色包已导出";}}InvalidateRect(window,nullptr,FALSE);return;}
      if(id==PlayPreview){playing=!playing;nextFrame=playing&&preview?GetTickCount64()+preview->clips[clipIndex].frames[frame].delay:0;sync();return;}
      if(id==PreviousFrame||id==NextFrame){if(preview){playing=false;nextFrame=0;const auto count=preview->clips[clipIndex].frames.size();frame=id==NextFrame?(frame+1)%count:(frame+count-1)%count;SendMessageW(controls[FrameSlider],TBM_SETPOS,TRUE,frame);sync();}return;}
      if(id==ToggleRule){const auto key=state.document["rules"][selected]["id"].get<std::string>();remember();if(!state.disabled.erase(key))state.disabled.insert(key);populate();return;}
      if(id==ReplaceImage)image(true);
      else if(id==DeleteItem)erase();
      else if(id==AddItem||id==EditItem){if(page==0)editRole();else if(page==1)image(id==EditItem);else if(page==2)action(id==AddItem);else rule(id==AddItem);}
      pruneResources();
      refreshDraftPreview();
      populate();
    }catch(const ImportFailure &failure){message=errorText(failure);showDetails(window,L"角色配置问题",message,colors);}
    catch(const std::wstring &error){message=error;showDetails(window,L"编辑角色",message,colors);}
    catch(...){message=L"输入内容无效，请检查字段";showDetails(window,L"编辑角色",message,colors);}
  }
  static LRESULT CALLBACK proc(HWND window,UINT message,WPARAM wParam,LPARAM lParam){
    auto *self=reinterpret_cast<EditorWindow*>(GetWindowLongPtrW(window,GWLP_USERDATA));
    if(message==WM_NCCREATE){self=static_cast<EditorWindow*>(reinterpret_cast<CREATESTRUCTW*>(lParam)->lpCreateParams);self->window=window;SetWindowLongPtrW(window,GWLP_USERDATA,reinterpret_cast<LONG_PTR>(self));}
    if(!self)return DefWindowProcW(window,message,wParam,lParam);
    switch(message){
    case WM_CLOSE:self->command(CloseEdit,BN_CLICKED);return 0;
    case WM_DPICHANGED: {
      self->dpi = HIWORD(wParam);
      HFONT previous = self->font;
      self->font = CreateFontW(-self->px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
      for (const auto &[id, child] : self->controls) SendMessageW(child, WM_SETFONT, reinterpret_cast<WPARAM>(self->font), TRUE);
      if (previous) DeleteObject(previous);
      styleCombo(self->controls[SpeedSelector], self->colors, self->font, self->dpi);
      const auto *bounds = reinterpret_cast<const RECT *>(lParam);
      SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
      self->layout(); return 0;
    }
    case WM_SIZE:self->layout();return 0;
    case WM_GETMINMAXINFO:{auto *limits=reinterpret_cast<MINMAXINFO*>(lParam);limits->ptMinTrackSize={self->px(860),self->px(640)};return 0;}
    case WM_ERASEBKGND:return 1;
    case WM_COMMAND:self->command(LOWORD(wParam),HIWORD(wParam));return 0;
    case WM_HSCROLL:if(reinterpret_cast<HWND>(lParam)==self->controls[FrameSlider]){self->playing=false;self->nextFrame=0;self->frame=size_t(SendMessageW(self->controls[FrameSlider],TBM_GETPOS,0,0));self->sync();}return 0;
    case WM_TIMER:if(!self->busy&&self->playing&&self->nextFrame&&self->preview){const auto now=GetTickCount64();const auto &clip=self->preview->clips[self->clipIndex];int steps=0;while(now>=self->nextFrame&&steps++<300){self->frame=(self->frame+1)%clip.frames.size();self->nextFrame+=uint64_t(std::max(1.0,clip.frames[self->frame].delay/self->speed));}if(steps){SendMessageW(self->controls[FrameSlider],TBM_SETPOS,TRUE,self->frame);InvalidateRect(window,nullptr,FALSE);}}self->schedule();return 0;
    case WM_PRINTCLIENT: case WM_PAINT:{PAINTSTRUCT paint{};HDC dc=message==WM_PAINT?BeginPaint(window,&paint):reinterpret_cast<HDC>(wParam);RECT area{};GetClientRect(window,&area);HBRUSH background=CreateSolidBrush(self->colors.window);FillRect(dc,&area,background);DeleteObject(background);
      SetBkMode(dc,TRANSPARENT);SetTextColor(dc,self->colors.text);SelectObject(dc,self->font);
      auto text=[&](std::wstring value,RECT rect,UINT flags=DT_LEFT|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS){DrawTextW(dc,value.c_str(),int(value.size()),&rect,flags);};
      if(self->page==0){text(L"角色名称",{self->px(20),self->px(90),self->px(280),self->px(120)});text(Storage::wide(self->state.document.value("name",std::string())),{self->px(20),self->px(126),self->px(280),self->px(162)});text(L"角色标识",{self->px(20),self->px(186),self->px(280),self->px(218)});text(Storage::wide(self->state.document.value("id",std::string())),{self->px(20),self->px(222),self->px(280),self->px(264)});text(L"画布："+std::to_wstring(self->state.document["canvas"].value("width",500))+L" × "+std::to_wstring(self->state.document["canvas"].value("height",500)),{self->px(20),self->px(292),self->px(280),self->px(332)});}
      if(self->preview&&!self->preview->clips.empty()){Gdiplus::Graphics graphics(dc);const auto &clip=self->preview->clips[self->clipIndex];paintAnimationFrame(graphics,{self->px(310),self->px(126),area.right-self->px(350),area.bottom-self->px(320)},clip,self->frame,true);text(L"第 "+std::to_wstring(self->frame+1)+L" / "+std::to_wstring(clip.frames.size())+L" 帧 · "+std::to_wstring(clip.frames[self->frame].delay)+L" ms",{self->px(310),area.bottom-self->px(184),area.right-self->px(30),area.bottom-self->px(154)},DT_CENTER|DT_VCENTER|DT_SINGLELINE);}
      SetTextColor(dc,self->colors.muted);text(self->message.empty()?(self->dirty?L"有未应用的修改":L"就绪"):self->message,{self->px(20),area.bottom-self->px(32),area.right-self->px(20),area.bottom});if(message==WM_PAINT)EndPaint(window,&paint);return 0;}
    case WM_DRAWITEM:{const auto &draw=*reinterpret_cast<DRAWITEMSTRUCT*>(lParam);RECT rect=draw.rcItem;const bool selected=(draw.itemState&ODS_SELECTED)||(draw.CtlID>=PageRole&&draw.CtlID<=PageRules&&draw.CtlID-PageRole==unsigned(self->page));HBRUSH fill=CreateSolidBrush(selected?self->colors.selection:self->colors.button);FillRect(draw.hDC,&rect,fill);DeleteObject(fill);SelectObject(draw.hDC,self->font);SetBkMode(draw.hDC,TRANSPARENT);SetTextColor(draw.hDC,draw.itemState&ODS_DISABLED?self->colors.muted:self->colors.text);wchar_t label[256]{};
      if(draw.CtlType==ODT_LISTBOX){if(draw.itemID==UINT(-1))return TRUE;SendMessageW(draw.hwndItem,LB_GETTEXT,draw.itemID,reinterpret_cast<LPARAM>(label));rect.left+=self->px(10);}
      else if (draw.CtlType == ODT_COMBOBOX) { if (draw.itemID != UINT(-1)) SendMessageW(draw.hwndItem, CB_GETLBTEXT, draw.itemID, reinterpret_cast<LPARAM>(label)); }
      else GetWindowTextW(draw.hwndItem,label,256);
      if (draw.CtlID == PlayPreview) {
        const auto bitmap = self->icons[self->playing ? PNG_PAUSE : PNG_PLAY];
        if (bitmap) {
          Gdiplus::Graphics graphics(draw.hDC); Gdiplus::ImageAttributes attributes;
          const auto foreground = self->colors.text;
          Gdiplus::ColorMatrix matrix = {{{0,0,0,0,0},{0,0,0,0,0},{0,0,0,0,0},{0,0,0,1,0},
              {GetRValue(foreground)/255.0f,GetGValue(foreground)/255.0f,GetBValue(foreground)/255.0f,0,1}}};
          attributes.SetColorMatrix(&matrix);
          graphics.DrawImage(bitmap.get(),Gdiplus::Rect((rect.right-self->px(20))/2,(rect.bottom-self->px(20))/2,self->px(20),self->px(20)),0,0,bitmap->GetWidth(),bitmap->GetHeight(),Gdiplus::UnitPixel,&attributes);
        }
        if(draw.itemState&ODS_FOCUS){InflateRect(&rect,-2,-2);DrawFocusRect(draw.hDC,&rect);}
        return TRUE;
      }
      const wchar_t *caption=draw.CtlID==UndoEdit?L"↶":draw.CtlID==RedoEdit?L"↷":draw.CtlID==PreviousFrame?L"‹":draw.CtlID==NextFrame?L"›":label;
      DrawTextW(draw.hDC,caption,-1,&rect,(draw.CtlType==ODT_LISTBOX?DT_LEFT:DT_CENTER)|DT_VCENTER|DT_SINGLELINE|DT_END_ELLIPSIS);if(draw.itemState&ODS_FOCUS){InflateRect(&rect,-2,-2);DrawFocusRect(draw.hDC,&rect);}return TRUE;}
    case WM_MEASUREITEM:{auto *measure=reinterpret_cast<MEASUREITEMSTRUCT*>(lParam);measure->itemHeight=self->px(measure->CtlType==ODT_COMBOBOX?26:44);return TRUE;}
    case WM_CTLCOLOREDIT:case WM_CTLCOLORSTATIC:case WM_CTLCOLORLISTBOX:SetTextColor(reinterpret_cast<HDC>(wParam),self->colors.text);SetBkColor(reinterpret_cast<HDC>(wParam),self->colors.base);return reinterpret_cast<LRESULT>(self->brush);
    }return DefWindowProcW(window,message,wParam,lParam);
  }
  void run(){
    dpi=GetDpiForWindow(owner);if(!dpi)dpi=96;font=CreateFontW(-px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");brush=CreateSolidBrush(colors.base);
    WNDCLASSW type{};type.lpfnWndProc=proc;type.hInstance=GetModuleHandleW(nullptr);type.hCursor=LoadCursorW(nullptr,IDC_ARROW);type.lpszClassName=L"OpenTypingPetEditor";RegisterClassW(&type);
    RECT parent{};GetWindowRect(owner,&parent);window=CreateWindowExW(0,type.lpszClassName,L"角色编辑器",WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN,parent.left+px(16),parent.top+px(16),px(960),px(680),owner,nullptr,type.hInstance,this);
    BOOL dark=colors.dark;DwmSetWindowAttribute(window,DWMWA_USE_IMMERSIVE_DARK_MODE,&dark,sizeof(dark));
    auto control=[&](const wchar_t *type,const wchar_t *label,unsigned id,DWORD style){HWND child=CreateWindowExW(0,type,label,WS_CHILD|WS_VISIBLE|WS_TABSTOP|style,0,0,1,1,window,reinterpret_cast<HMENU>(UINT_PTR(id)),GetModuleHandleW(nullptr),nullptr);SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(font),TRUE);SetWindowTheme(child,L"",L"");controls[id]=child;};
    for(auto [id,label]:std::vector<std::pair<unsigned,const wchar_t*>>{{PageRole,L"角色"},{PageImages,L"素材"},{PageActions,L"动作"},{PageRules,L"规则"},{AddItem,L"新增"},{EditItem,L"编辑"},{DeleteItem,L"删除"},{ReplaceImage,L"更换素材"},{ToggleRule,L"启用 / 停用"},{UndoEdit,L"撤销"},{RedoEdit,L"重做"},{SaveEdit,L"保存并应用"},{ExportEdit,L"导出 ZIP"},{CloseEdit,L"关闭"},{PlayPreview,L"播放 / 暂停"},{PreviousFrame,L"上一帧"},{NextFrame,L"下一帧"}})control(WC_BUTTONW,label,id,BS_OWNERDRAW);
    control(WC_LISTBOXW,L"",Items,LBS_OWNERDRAWFIXED|LBS_HASSTRINGS|LBS_NOTIFY|WS_VSCROLL|LBS_NOINTEGRALHEIGHT);
    control(TRACKBAR_CLASSW,L"",FrameSlider,TBS_NOTICKS|TBS_HORZ);control(WC_COMBOBOXW,L"",SpeedSelector,CBS_DROPDOWNLIST|CBS_OWNERDRAWFIXED|CBS_HASSTRINGS|WS_VSCROLL);
    styleCombo(controls[SpeedSelector], colors, font, dpi);
    for(const wchar_t *label:{L"0.25×",L"0.5×",L"1×",L"2×"})SendMessageW(controls[SpeedSelector],CB_ADDSTRING,0,reinterpret_cast<LPARAM>(label));SendMessageW(controls[SpeedSelector],CB_SETCURSEL,2,0);
    for (int id : {PNG_PLAY, PNG_PAUSE}) {
      const auto resource = FindResourceW(type.hInstance, MAKEINTRESOURCEW(id), RT_RCDATA);
      if (!resource) continue;
      IStream *stream = SHCreateMemStream(static_cast<const BYTE *>(LockResource(LoadResource(type.hInstance, resource))), SizeofResource(type.hInstance, resource));
      if (!stream) continue;
      { Gdiplus::Bitmap original(stream); if (original.GetLastStatus() == Gdiplus::Ok) icons[id].reset(original.Clone(0,0,original.GetWidth(),original.GetHeight(),PixelFormat32bppPARGB)); }
      stream->Release();
    }
    tooltips=CreateWindowExW(WS_EX_TOPMOST,TOOLTIPS_CLASSW,nullptr,WS_POPUP|TTS_ALWAYSTIP,0,0,0,0,window,nullptr,type.hInstance,nullptr);
    for(auto [id,label]:std::vector<std::pair<unsigned,const wchar_t*>>{{UndoEdit,L"撤销"},{RedoEdit,L"重做"},{PreviousFrame,L"上一帧"},{NextFrame,L"下一帧"},{PlayPreview,L"播放 / 暂停"}}){tips[id]=label;TOOLINFOW tool{sizeof(tool)};tool.uFlags=TTF_IDISHWND|TTF_SUBCLASS;tool.hwnd=window;tool.uId=reinterpret_cast<UINT_PTR>(controls[id]);tool.lpszText=tips[id].data();SendMessageW(tooltips,TTM_ADDTOOLW,0,reinterpret_cast<LPARAM>(&tool));}
    populate();if(!state.document["actions"].empty())validate();EnableWindow(owner,FALSE);ShowWindow(window,SW_SHOW);SetForegroundWindow(window);schedule();
    MSG msg{};while(!done){const auto result=GetMessageW(&msg,nullptr,0,0);if(result<=0){if(!result)PostQuitMessage(int(msg.wParam));break;}
      if(msg.message==WM_KEYDOWN&&(GetKeyState(VK_CONTROL)&0x8000)&&GetAncestor(msg.hwnd,GA_ROOT)==window){
        if(msg.wParam=='Z'){command(GetKeyState(VK_SHIFT)&0x8000?RedoEdit:UndoEdit,BN_CLICKED);continue;}
        if(msg.wParam=='Y'){command(RedoEdit,BN_CLICKED);continue;}
      }
      if(!IsDialogMessageW(window,&msg)){TranslateMessage(&msg);DispatchMessageW(&msg);}}
    EnableWindow(owner,TRUE);DestroyWindow(window);SetForegroundWindow(owner);DeleteObject(font);DeleteObject(brush);
  }
};
}
std::optional<InputRule> editInputRule(HWND owner,const ThemeColors &colors,const CharacterDefinition &character,
                                     const InputRule *existing,const std::string &defaultAction,const std::function<void(bool)> &captureState){
  InputRule rule=existing?*existing:InputRule{};if(!existing){rule.id="rule"+Storage::newId();rule.actions={defaultAction};rule.key=VK_RETURN;rule.priority=100;}
  const std::vector<std::wstring> events={L"键盘按下",L"键盘松开",L"鼠标按下",L"鼠标松开",L"普通打字"};
  const std::vector<std::wstring> values={L"keyDown",L"keyUp",L"mouseDown",L"mouseUp",L"typing"};
  std::wstring sequence;for(const auto &id:rule.actions){if(!sequence.empty())sequence+=L", ";sequence+=Storage::wide(id);}
  std::vector<FormField> fields={choice("event",L"触发事件",values[int(rule.event)],events,values),
    {"key",L"按键 / 鼠标按钮",Storage::wide(inputKeyName(rule.key)),FieldKind::Key},
    {"ctrl",L"同时按住 Ctrl",rule.ctrl?L"1":L"0",FieldKind::Check},{"shift",L"同时按住 Shift",rule.shift?L"1":L"0",FieldKind::Check},{"alt",L"同时按住 Alt",rule.alt?L"1":L"0",FieldKind::Check},
    {"actions",L"动作序列",sequence,FieldKind::Sequence},{"priority",L"优先级",std::to_wstring(rule.priority),FieldKind::Number},
    choice("repeat",L"重复触发",rule.repeat==RepeatBehavior::Restart?L"restart":rule.repeat==RepeatBehavior::Continue?L"continue":L"ignore",{L"重新播放",L"保持播放进度",L"长按仅触发一次"},{L"restart",L"continue",L"ignore"})};
  for (auto &field : fields) if (field.id == "actions")
    for (const auto &action : character.actions) { field.choices.push_back(action.label); field.choiceValues.push_back(Storage::wide(action.id)); }
  while(editForm(owner,L"输入规则",fields,colors,captureState)){
    try{
      const auto event=value(fields,"event");rule.event=event=="keyUp"?InputEventType::KeyUp:event=="mouseDown"?InputEventType::MouseDown:event=="mouseUp"?InputEventType::MouseUp:event=="typing"?InputEventType::Typing:InputEventType::KeyDown;
      rule.key=rule.event==InputEventType::Typing?0:inputKeyCode(value(fields,"key"));rule.ctrl=value(fields,"ctrl")=="1";rule.shift=value(fields,"shift")=="1";rule.alt=value(fields,"alt")=="1";
      if(rule.event==InputEventType::Typing)rule.ctrl=rule.shift=rule.alt=false;
      rule.actions=actionSequence(value(fields,"actions"));for(const auto &id:rule.actions)if(character.actionIndex(id)==CharacterDefinition::noAction)throw std::wstring(L"动作不存在：")+Storage::wide(id);
      rule.priority=int(number(Storage::wide(value(fields,"priority")),1000));const auto repeat=value(fields,"repeat");rule.repeat=repeat=="restart"?RepeatBehavior::Restart:repeat=="continue"?RepeatBehavior::Continue:RepeatBehavior::Ignore;
      CharacterDefinition validation=character;std::erase_if(validation.rules,[&](const auto &item){return item.id==rule.id;});validation.rules.push_back(rule);
      const auto document=characterManifest(validation);
      const bool mouseKey=rule.key==1||rule.key==2||rule.key==4||rule.key==5||rule.key==6;
      if(rule.event!=InputEventType::Typing&&mouseKey!=(rule.event==InputEventType::MouseDown||rule.event==InputEventType::MouseUp))throw std::wstring(L"触发事件与按键类型不匹配");
      for(const auto &other:validation.rules)if(other.id!=rule.id&&other.event==rule.event&&other.key==rule.key&&other.ctrl==rule.ctrl&&other.shift==rule.shift&&other.alt==rule.alt)throw std::wstring(L"此触发条件已有规则");
      rule.modifierTap=rule.event==InputEventType::KeyUp&&(rule.key==VK_SHIFT||rule.key==VK_CONTROL||rule.key==VK_MENU);
      rule.label=rule.event==InputEventType::Typing?L"普通打字":std::wstring(rule.ctrl?L"Ctrl+":L"")+(rule.shift?L"Shift+":L"")+(rule.alt?L"Alt+":L"")+Storage::wide(inputKeyName(rule.key));
      if(rule.event==InputEventType::KeyUp||rule.event==InputEventType::MouseUp)rule.label=L"松开 "+rule.label;
      return rule;
    }catch(const std::wstring &error){showDetails(owner,L"输入规则",error,colors);}catch(...){showDetails(owner,L"输入规则",L"输入内容无效",colors);}
  }return {};
}
std::optional<CharacterEditResult> editCharacterWindow(HWND owner,const ThemeColors &colors,
    std::shared_ptr<CharacterDefinition> character,const std::set<std::string> &disabled,const std::function<void(bool)> &captureState){
  EditorWindow editor;editor.owner=owner;editor.colors=colors;editor.captureState=captureState;editor.state.disabled=disabled;
  if(character){const auto resources=PresetLoader::exportResources(*character);for(const auto &[name,bytes]:resources)if(name!="character.json")editor.state.resources[name]=std::make_shared<const std::vector<uint8_t>>(bytes);editor.state.document=characterManifest(*character);editor.preview=std::move(character);}
  else editor.state.document={{"formatVersion",1},{"id","local."+Storage::newId()},{"name","新角色"},{"canvas",{{"width",500},{"height",500}}},{"defaultAction",""},{"animations",Json::array()},{"actions",Json::array()},{"rules",Json::array()}};
  editor.run();return editor.output;
}
