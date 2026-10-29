#include "nativeDialogs.h"
#include "characterManifest.h"
#include "storageUtils.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <shobjidl.h>
#include <uxtheme.h>
#include <windowsx.h>
#include <map>
#include <set>
#include <sstream>

namespace {
void fill(HDC dc, RECT area, COLORREF color) { HBRUSH brush = CreateSolidBrush(color); FillRect(dc, &area, brush); DeleteObject(brush); }
void text(HDC dc, RECT area, const std::wstring &value, HFONT font, COLORREF color, UINT flags = DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
  const auto previous = SelectObject(dc, font); SetBkMode(dc, TRANSPARENT); SetTextColor(dc, color);
  DrawTextW(dc, value.c_str(), int(value.size()), &area, flags); SelectObject(dc, previous);
}
struct ComboStyle { ThemeColors colors; HFONT font; unsigned dpi; };
LRESULT CALLBACK comboProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam, UINT_PTR id, DWORD_PTR data) {
  auto *style = reinterpret_cast<ComboStyle *>(data);
  if (message == WM_NCDESTROY) {
    RemoveWindowSubclass(window, comboProc, id); delete style;
    return DefSubclassProc(window, message, wParam, lParam);
  }
  const auto result = DefSubclassProc(window, message, wParam, lParam);
  if (message == WM_PAINT || message == WM_PRINTCLIENT || (message == WM_PRINT && (lParam & PRF_CLIENT))) {
    HDC dc = message == WM_PAINT ? GetDC(window) : reinterpret_cast<HDC>(wParam);
    RECT area{}; GetClientRect(window, &area); fill(dc, area, style->colors.base);
    const bool enabled = IsWindowEnabled(window) != FALSE;
    HBRUSH border = CreateSolidBrush(GetFocus() == window ? style->colors.accent : style->colors.border);
    FrameRect(dc, &area, border); DeleteObject(border);
    RECT label = area; label.left += MulDiv(8, style->dpi, 96); label.right -= MulDiv(28, style->dpi, 96);
    const auto selection = SendMessageW(window, CB_GETCURSEL, 0, 0);
    const auto length = selection >= 0 ? SendMessageW(window, CB_GETLBTEXTLEN, selection, 0) : -1;
    if (length >= 0) {
      std::wstring caption(size_t(length) + 1, L'\0'); SendMessageW(window, CB_GETLBTEXT, selection, reinterpret_cast<LPARAM>(caption.data())); caption.resize(size_t(length));
      text(dc, label, caption, style->font, enabled ? style->colors.text : style->colors.muted);
    }
    RECT arrow{label.right, area.top, area.right, area.bottom};
    text(dc, arrow, L"⌄", style->font, style->colors.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    if (message == WM_PAINT) ReleaseDC(window, dc);
  }
  return result;
}
struct Dialog {
  HWND window = nullptr, owner = nullptr;
  ThemeColors colors;
  HFONT font = nullptr;
  HBRUSH brush = nullptr;
  unsigned dpi = 96;
  bool done = false, accepted = false, armed = false;
  int choice = -1;
  std::vector<FormField> fields;
  std::vector<int> rows;
  std::vector<HWND> inputs;
  std::vector<std::wstring> buttons;
  std::wstring message;
  std::optional<InputEvent> captured;
  std::set<int> modifiers;
  std::function<void(bool)> captureState;
  enum Mode { Form, Choose, Capture, Details, Sequence } mode = Form;
  std::vector<std::wstring> sequence;
  void loadSequence(const std::wstring &value) {
    sequence.clear(); std::wistringstream stream(value); std::wstring id;
    while (std::getline(stream, id, L',')) {
      const auto begin = id.find_first_not_of(L" \t\r\n"), end = id.find_last_not_of(L" \t\r\n");
      if (begin != std::wstring::npos) sequence.push_back(id.substr(begin, end - begin + 1));
    }
  }
  std::wstring sequenceText(bool names) const {
    std::wstring result;
    for (const auto &id : sequence) {
      if (!result.empty()) result += names ? L" → " : L", ";
      const auto found = std::find(fields[0].choiceValues.begin(), fields[0].choiceValues.end(), id);
      result += names && found != fields[0].choiceValues.end() ? fields[0].choices[size_t(found - fields[0].choiceValues.begin())] : id;
    }
    return result;
  }
  void refreshSequence(int selected) {
    const auto list = GetDlgItem(window, 5000); SendMessageW(list, LB_RESETCONTENT, 0, 0);
    for (size_t i = 0; i < sequence.size(); ++i) {
      const auto found = std::find(fields[0].choiceValues.begin(), fields[0].choiceValues.end(), sequence[i]);
      const auto label = std::to_wstring(i + 1) + L" · " + (found == fields[0].choiceValues.end() ? sequence[i] : fields[0].choices[size_t(found - fields[0].choiceValues.begin())]);
      SendMessageW(list, LB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
    }
    SendMessageW(list, LB_SETCURSEL, std::clamp(selected, 0, std::max(0, int(sequence.size()) - 1)), 0);
    EnableWindow(GetDlgItem(window, IDOK), !sequence.empty());
    EnableWindow(GetDlgItem(window, 5002), sequence.size() < 16 && !fields[0].choices.empty());
    for (int id : {5003, 5004, 5005}) EnableWindow(GetDlgItem(window, id), !sequence.empty());
  }
  int px(int value) const { return MulDiv(value, dpi, 96); }
  HWND control(const wchar_t *type, const std::wstring &value, int id, DWORD style, int x, int y, int width, int height) {
    HWND handle = CreateWindowExW(0, type, value.c_str(), WS_CHILD | WS_VISIBLE | WS_TABSTOP | style,
                                   px(x), px(y), px(width), px(height), window, reinterpret_cast<HMENU>(INT_PTR(id)), GetModuleHandleW(nullptr), nullptr);
    SendMessageW(handle, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE); SetWindowTheme(handle, L"", L"");
    if (wcscmp(type, WC_COMBOBOXW) == 0) styleCombo(handle, colors, font, dpi);
    return handle;
  }
  void sync() {
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].kind == FieldKind::Check) { InvalidateRect(inputs[i], nullptr, TRUE); continue; }
      if (fields[i].kind == FieldKind::Sequence) {
        Dialog display; display.fields = {fields[i]}; display.loadSequence(fields[i].value);
        SetWindowTextW(inputs[i], display.sequenceText(true).c_str()); continue;
      }
      if (fields[i].kind == FieldKind::Choice) {
        const auto &values = fields[i].choiceValues.empty() ? fields[i].choices : fields[i].choiceValues;
        auto found = std::find(values.begin(), values.end(), fields[i].value);
        SendMessageW(inputs[i], CB_SETCURSEL, found == values.end() ? 0 : found - values.begin(), 0);
      } else SetWindowTextW(inputs[i], fields[i].value.c_str());
    }
  }
  void collect() {
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].kind == FieldKind::Check || fields[i].kind == FieldKind::Sequence) continue;
      if (fields[i].kind == FieldKind::Choice) {
        const auto selected = SendMessageW(inputs[i], CB_GETCURSEL, 0, 0);
        const auto &values = fields[i].choiceValues.empty() ? fields[i].choices : fields[i].choiceValues;
        if (selected >= 0 && size_t(selected) < values.size()) fields[i].value = values[size_t(selected)];
        continue;
      }
      const int length = GetWindowTextLengthW(inputs[i]);
      std::wstring value(size_t(length) + 1, L'\0'); GetWindowTextW(inputs[i], value.data(), length + 1); value.resize(length);
      fields[i].value = std::move(value);
    }
  }
  void acceptInput(int key, bool mouse, bool down) {
    if (!armed || done) return;
    if (key == VK_ESCAPE) { done = true; return; }
    if (key == VK_SHIFT || key == VK_CONTROL || key == VK_MENU) {
      if (down) { modifiers.insert(key); return; }
      if (!modifiers.contains(key)) return;
    } else if (!down) return;
    const bool shift = key != VK_SHIFT && (GetKeyState(VK_SHIFT) & 0x8000);
    const bool ctrl = key != VK_CONTROL && (GetKeyState(VK_CONTROL) & 0x8000);
    const bool alt = key != VK_MENU && (GetKeyState(VK_MENU) & 0x8000);
    captured = InputEvent{mouse ? InputEventType::MouseDown : down ? InputEventType::KeyDown : InputEventType::KeyUp,
                          key, shift, ctrl, alt, false, !down};
    accepted = true; done = true;
  }
  static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    auto *self = reinterpret_cast<Dialog *>(GetWindowLongPtrW(window, GWLP_USERDATA));
    if (message == WM_NCCREATE) { self = static_cast<Dialog *>(reinterpret_cast<CREATESTRUCTW *>(lParam)->lpCreateParams); self->window = window; SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self)); }
    if (!self) return DefWindowProcW(window, message, wParam, lParam);
    switch (message) {
    case WM_CLOSE: self->done = true; return 0;
    case WM_DPICHANGED: {
      const unsigned previousDpi = self->dpi; self->dpi = HIWORD(wParam);
      HFONT previousFont = self->font;
      self->font = CreateFontW(-self->px(14),0,0,0,FW_NORMAL,FALSE,FALSE,FALSE,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,CLIP_DEFAULT_PRECIS,CLEARTYPE_QUALITY,DEFAULT_PITCH,L"Microsoft YaHei UI");
      for (HWND child = GetWindow(window,GW_CHILD); child; child = GetWindow(child,GW_HWNDNEXT)) {
        RECT bounds{};GetWindowRect(child,&bounds);MapWindowPoints(nullptr,window,reinterpret_cast<POINT *>(&bounds),2);
        MoveWindow(child,MulDiv(bounds.left,self->dpi,previousDpi),MulDiv(bounds.top,self->dpi,previousDpi),MulDiv(bounds.right-bounds.left,self->dpi,previousDpi),MulDiv(bounds.bottom-bounds.top,self->dpi,previousDpi),TRUE);
        SendMessageW(child,WM_SETFONT,reinterpret_cast<WPARAM>(self->font),TRUE);
        wchar_t type[64]{};GetClassNameW(child,type,64);
        if(wcscmp(type,WC_COMBOBOXW)==0){styleCombo(child,self->colors,self->font,self->dpi);SendMessageW(child,CB_SETITEMHEIGHT,WPARAM(-1),self->px(30));SendMessageW(child,CB_SETITEMHEIGHT,0,self->px(30));}
        if(wcscmp(type,WC_LISTBOXW)==0)SendMessageW(child,LB_SETITEMHEIGHT,0,self->px(30));
      }
      if(previousFont)DeleteObject(previousFont);
      const auto *bounds=reinterpret_cast<const RECT *>(lParam);
      SetWindowPos(window,nullptr,bounds->left,bounds->top,bounds->right-bounds->left,bounds->bottom-bounds->top,SWP_NOZORDER|SWP_NOACTIVATE);
      return 0;
    }
    case WM_ACTIVATE: if (self->mode == Capture && LOWORD(wParam) == WA_INACTIVE) self->done = true; return 0;
    case WM_ERASEBKGND: return 1;
    case WM_PRINTCLIENT: case WM_PAINT: {
      PAINTSTRUCT paint{}; HDC dc = message == WM_PAINT ? BeginPaint(window, &paint) : reinterpret_cast<HDC>(wParam); RECT area{}; GetClientRect(window, &area); fill(dc, area, self->colors.window);
      if (self->mode == Form)
        for (size_t i = 0; i < self->fields.size(); ++i)
          if (self->fields[i].kind != FieldKind::Check)
            text(dc, {self->px(20), self->px(self->rows[i]), self->px(154), self->px(self->rows[i] + 34)}, self->fields[i].label, self->font, self->colors.text, DT_LEFT | DT_WORDBREAK);
      if (self->mode == Choose)
        text(dc, {self->px(20), self->px(20), area.right - self->px(20), self->px(110)}, self->message, self->font, self->colors.text, DT_LEFT | DT_WORDBREAK);
      if (self->mode == Capture) {
        text(dc, {self->px(20), self->px(26), area.right - self->px(20), self->px(60)}, self->armed ? L"等待输入" : L"准备录入", self->font, self->colors.text, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
        RECT capture{self->px(20), self->px(78), area.right - self->px(20), self->px(164)}; fill(dc, capture, self->colors.panel);
        text(dc, capture, L"鼠标录入区域", self->font, self->colors.muted, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      }
      if (message == WM_PAINT) EndPaint(window, &paint); return 0;
    }
    case WM_TIMER:
      if (self->mode == Capture && !self->armed) {
        self->armed = true;
        for (int key : {VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_SHIFT, VK_CONTROL, VK_MENU, VK_RETURN, VK_SPACE})
          if (GetAsyncKeyState(key) & 0x8000) self->armed = false;
        if (self->armed) { KillTimer(window, 1); InvalidateRect(window, nullptr, FALSE); }
      }
      return 0;
    case WM_KEYDOWN: case WM_SYSKEYDOWN: case WM_KEYUP: case WM_SYSKEYUP:
      if (self->mode == Capture) { self->acceptInput(int(wParam), false, message == WM_KEYDOWN || message == WM_SYSKEYDOWN); return 0; }
      break;
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: case WM_MBUTTONDOWN: case WM_XBUTTONDOWN:
      if (self->mode == Capture && GET_Y_LPARAM(lParam) >= self->px(78) && GET_Y_LPARAM(lParam) <= self->px(164)) {
        const int key = message == WM_LBUTTONDOWN ? VK_LBUTTON : message == WM_RBUTTONDOWN ? VK_RBUTTON : message == WM_MBUTTONDOWN ? VK_MBUTTON : GET_XBUTTON_WPARAM(wParam) == XBUTTON1 ? VK_XBUTTON1 : VK_XBUTTON2;
        self->acceptInput(key, true, true); return TRUE;
      }
      break;
    case WM_COMMAND: {
      const int id = LOWORD(wParam), notification = HIWORD(wParam);
      if (notification != BN_CLICKED) return 0;
      if (id == IDCANCEL) { self->done = true; return 0; }
      if (id == IDOK) {
        if (self->mode == Sequence) self->fields[0].value = self->sequenceText(false); else self->collect();
        self->accepted = true; self->done = true; return 0;
      }
      if (self->mode == Sequence && id >= 5002 && id <= 5005) {
        const auto index = int(SendMessageW(GetDlgItem(window, 5000), LB_GETCURSEL, 0, 0));
        int selected = index;
        if (id == 5002 && self->sequence.size() < 16) {
          const auto choice = SendMessageW(GetDlgItem(window, 5001), CB_GETCURSEL, 0, 0);
          if (choice >= 0 && size_t(choice) < self->fields[0].choiceValues.size()) {
            self->sequence.push_back(self->fields[0].choiceValues[choice]); selected = int(self->sequence.size()) - 1;
          }
        } else if (index >= 0 && size_t(index) < self->sequence.size()) {
          if (id == 5003) self->sequence.erase(self->sequence.begin() + index);
          if (id == 5004 && index > 0) { std::swap(self->sequence[index], self->sequence[index - 1]); --selected; }
          if (id == 5005 && size_t(index + 1) < self->sequence.size()) { std::swap(self->sequence[index], self->sequence[index + 1]); ++selected; }
        }
        self->refreshSequence(selected); return 0;
      }
      if (id >= 100 && id < 100 + int(self->fields.size()) && self->fields[id - 100].kind == FieldKind::Check) {
        auto &value = self->fields[id - 100].value; value = value == L"1" ? L"0" : L"1"; InvalidateRect(self->inputs[id - 100], nullptr, FALSE); return 0;
      }
      if (id >= 1000 && id < 1000 + int(self->fields.size())) {
        self->collect();
        if (self->fields[id - 1000].kind == FieldKind::Sequence) {
          Dialog sequence; sequence.owner = window; sequence.colors = self->colors; sequence.mode = Sequence;
          sequence.fields = {self->fields[id - 1000]}; sequence.loadSequence(sequence.fields[0].value);
          sequence.run(L"动作序列");
          if (sequence.accepted) { self->fields[id - 1000].value = sequence.fields[0].value; self->sync(); }
          return 0;
        }
        if (auto event = captureInput(window, self->colors, self->captureState)) {
          const bool up = event->type == InputEventType::KeyUp;
          for (auto &field : self->fields) {
            if (field.id == "key") field.value = Storage::wide(inputKeyName(event->key));
            if (field.id == "event") field.value = event->type == InputEventType::MouseDown ? L"mouseDown" : up ? L"keyUp" : L"keyDown";
            if (field.id == "ctrl") field.value = event->ctrl ? L"1" : L"0";
            if (field.id == "shift") field.value = event->shift ? L"1" : L"0";
            if (field.id == "alt") field.value = event->alt ? L"1" : L"0";
          }
          self->sync();
        }
        return 0;
      }
      if (id >= 2000 && id < 2000 + int(self->buttons.size())) { self->choice = id - 2000; self->done = true; return 0; }
      if (id == 3000 && self->mode == Details) {
        if (OpenClipboard(window)) {
          HGLOBAL data = GlobalAlloc(GMEM_MOVEABLE, (self->message.size() + 1) * sizeof(wchar_t));
          if (data) { auto *buffer = GlobalLock(data); memcpy(buffer, self->message.c_str(), (self->message.size() + 1) * sizeof(wchar_t)); GlobalUnlock(data); EmptyClipboard(); if (!SetClipboardData(CF_UNICODETEXT, data)) GlobalFree(data); }
          CloseClipboard();
        }
      }
      return 0;
    }
    case WM_DRAWITEM: {
      const auto &draw = *reinterpret_cast<DRAWITEMSTRUCT *>(lParam);
      const int id = int(draw.CtlID); RECT rect = draw.rcItem;
      fill(draw.hDC, rect, draw.itemState & ODS_SELECTED ? self->colors.selection : self->colors.button);
      std::wstring caption;
      if (draw.CtlType == ODT_COMBOBOX) {
        if (self->mode == Sequence && draw.itemID < self->fields[0].choices.size()) caption = self->fields[0].choices[draw.itemID];
        if (draw.itemID != UINT(-1) && id >= 100 && id < 100 + int(self->fields.size()) && draw.itemID < self->fields[id - 100].choices.size())
          caption = self->fields[id - 100].choices[draw.itemID];
        rect.left += self->px(8);
      } else if (draw.CtlType == ODT_LISTBOX) {
        if (draw.itemID == UINT(-1)) return TRUE;
        const auto size = SendMessageW(draw.hwndItem, LB_GETTEXTLEN, draw.itemID, 0);
        if (size >= 0) { caption.resize(size_t(size) + 1); SendMessageW(draw.hwndItem, LB_GETTEXT, draw.itemID, reinterpret_cast<LPARAM>(caption.data())); caption.resize(size_t(size)); }
        rect.left += self->px(8);
      } else {
        wchar_t value[160]{}; GetWindowTextW(draw.hwndItem, value, 160); caption = value;
        if (id >= 100 && id < 100 + int(self->fields.size()) && self->fields[id - 100].kind == FieldKind::Check) {
          fill(draw.hDC, rect, self->colors.window);
          RECT box{rect.right - self->px(24), self->px(6), rect.right - self->px(4), self->px(26)};
          fill(draw.hDC, box, self->fields[id - 100].value == L"1" ? self->colors.accent : self->colors.button);
          if (self->fields[id - 100].value == L"1") text(draw.hDC, box, L"✓", self->font, self->colors.dark ? RGB(15,30,25) : RGB(255,255,255), DT_CENTER | DT_VCENTER | DT_SINGLELINE);
          rect.right -= self->px(34);
        }
      }
      text(draw.hDC, rect, caption, self->font, draw.itemState & ODS_DISABLED ? self->colors.muted : self->colors.text,
           draw.CtlType == ODT_COMBOBOX || (id >= 100 && id < 100 + int(self->fields.size())) ? DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS : DT_CENTER | DT_VCENTER | DT_SINGLELINE);
      if (draw.itemState & ODS_FOCUS) { InflateRect(&rect, -2, -2); DrawFocusRect(draw.hDC, &rect); }
      return TRUE;
    }
    case WM_MEASUREITEM: reinterpret_cast<MEASUREITEMSTRUCT *>(lParam)->itemHeight = self->px(30); return TRUE;
    case WM_CTLCOLOREDIT: case WM_CTLCOLORSTATIC: case WM_CTLCOLORLISTBOX:
      SetTextColor(reinterpret_cast<HDC>(wParam), self->colors.text); SetBkColor(reinterpret_cast<HDC>(wParam), self->colors.base); return reinterpret_cast<LRESULT>(self->brush);
    }
    return DefWindowProcW(window, message, wParam, lParam);
  }
  void run(const std::wstring &title) {
    dpi = GetDpiForWindow(owner); if (!dpi) dpi = 96;
    font = CreateFontW(-px(14), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Microsoft YaHei UI");
    brush = CreateSolidBrush(colors.base);
    WNDCLASSW type{}; type.lpfnWndProc = proc; type.hInstance = GetModuleHandleW(nullptr); type.hCursor = LoadCursorW(nullptr, IDC_ARROW); type.lpszClassName = L"OpenTypingPetForm"; RegisterClassW(&type);
    int height = 240, width = 540, y = 20;
    if (mode == Form) { for (const auto &field : fields) { rows.push_back(y); y += field.kind == FieldKind::Multiline ? 110 : 44; } height = y + 66; }
    if (mode == Choose) { width = 560; height = 128 + int(buttons.size()) * 42; }
    if (mode == Details) { width = 720; height = 420; }
    if (mode == Sequence) { width = 560; height = 420; }
    RECT bounds{0, 0, px(width), px(height)}; AdjustWindowRectExForDpi(&bounds, WS_CAPTION | WS_SYSMENU, FALSE, 0, dpi);
    RECT parent{}; GetWindowRect(owner, &parent);
    window = CreateWindowExW(WS_EX_DLGMODALFRAME, type.lpszClassName, title.c_str(), WS_CAPTION | WS_SYSMENU | WS_CLIPCHILDREN,
                             parent.left + (parent.right - parent.left - bounds.right + bounds.left) / 2,
                             parent.top + (parent.bottom - parent.top - bounds.bottom + bounds.top) / 2,
                             bounds.right - bounds.left, bounds.bottom - bounds.top, owner, nullptr, type.hInstance, this);
    if (!window) { DeleteObject(font); DeleteObject(brush); return; }
    BOOL dark = colors.dark; DwmSetWindowAttribute(window, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (mode == Form) {
      for (size_t i = 0; i < fields.size(); ++i) {
        const auto &field = fields[i]; HWND input = nullptr;
        if (field.kind == FieldKind::Check) input = control(WC_BUTTONW, field.label, int(100 + i), BS_OWNERDRAW, 20, rows[i], width - 40, 32);
        else if (field.kind == FieldKind::Choice) {
          input = control(WC_COMBOBOXW, L"", int(100 + i), CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 164, rows[i], width - 184, 240);
          for (const auto &option : field.choices) SendMessageW(input, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(option.c_str()));
        } else {
          DWORD style = WS_BORDER | ES_AUTOHSCROLL;
          if (field.readOnly || field.kind == FieldKind::Sequence) style |= ES_READONLY;
          if (field.kind == FieldKind::Number) style |= ES_NUMBER;
          if (field.kind == FieldKind::Multiline) style = WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL | (field.readOnly ? ES_READONLY : 0);
          const bool button = field.kind == FieldKind::Key || field.kind == FieldKind::Sequence;
          input = control(WC_EDITW, field.value, int(100 + i), style, 164, rows[i], width - 184 - (button ? 76 : 0), field.kind == FieldKind::Multiline ? 98 : 32);
          SendMessageW(input, EM_SETLIMITTEXT, field.kind == FieldKind::Multiline ? 4096 : 320, 0);
          if (button) control(WC_BUTTONW, field.kind == FieldKind::Key ? L"录入" : L"选择", int(1000 + i), BS_OWNERDRAW, width - 90, rows[i], 70, 32);
        }
        if (field.readOnly && field.kind == FieldKind::Choice) EnableWindow(input, FALSE);
        inputs.push_back(input);
      }
      sync();
      control(WC_BUTTONW, L"确定", IDOK, BS_OWNERDRAW, width - 200, height - 46, 84, 32);
    }
    if (mode == Sequence) {
      control(WC_LISTBOXW, L"", 5000, LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOTIFY | WS_VSCROLL | LBS_NOINTEGRALHEIGHT, 20, 20, 430, 270);
      control(WC_BUTTONW, L"↑", 5004, BS_OWNERDRAW, 466, 20, 72, 34);
      control(WC_BUTTONW, L"↓", 5005, BS_OWNERDRAW, 466, 66, 72, 34);
      control(WC_BUTTONW, L"移除", 5003, BS_OWNERDRAW, 466, 112, 72, 34);
      const auto selector = control(WC_COMBOBOXW, L"", 5001, CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS | WS_VSCROLL, 20, 306, 430, 200);
      for (const auto &label : fields[0].choices) SendMessageW(selector, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str()));
      SendMessageW(selector, CB_SETCURSEL, 0, 0);
      control(WC_BUTTONW, L"添加", 5002, BS_OWNERDRAW, 466, 306, 72, 34);
      control(WC_BUTTONW, L"确定", IDOK, BS_OWNERDRAW, width - 200, height - 46, 84, 32);
      refreshSequence(0);
    }
    if (mode == Choose)
      for (size_t i = 0; i < buttons.size(); ++i) control(WC_BUTTONW, buttons[i], int(2000 + i), BS_OWNERDRAW, 20, 118 + int(i) * 42, width - 40, 34);
    else {
      control(WC_BUTTONW, mode == Details ? L"关闭" : L"取消", IDCANCEL, BS_OWNERDRAW, width - 104, height - 46, 84, 32);
      if (mode == Details) {
        control(WC_EDITW, message, 4000, WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL, 20, 20, width - 40, height - 84);
        control(WC_BUTTONW, L"复制详情", 3000, BS_OWNERDRAW, 20, height - 46, 108, 32);
      }
    }
    EnableWindow(owner, FALSE); ShowWindow(window, SW_SHOW); SetForegroundWindow(window);
    SetFocus(mode == Form && !inputs.empty() ? inputs.front() : window);
    if (mode == Capture) SetTimer(window, 1, 30, nullptr);
    MSG messageValue{};
    while (!done) {
      const auto result = GetMessageW(&messageValue, nullptr, 0, 0);
      if (result <= 0) { if (!result) PostQuitMessage(int(messageValue.wParam)); break; }
      if (mode == Capture || !IsDialogMessageW(window, &messageValue)) { TranslateMessage(&messageValue); DispatchMessageW(&messageValue); }
    }
    EnableWindow(owner, TRUE); DestroyWindow(window); SetForegroundWindow(owner);
    DeleteObject(font); DeleteObject(brush);
  }
};
}
void styleCombo(HWND window, const ThemeColors &colors, HFONT font, unsigned dpi) {
  DWORD_PTR previous = 0;
  if (GetWindowSubclass(window, comboProc, 1, &previous)) *reinterpret_cast<ComboStyle *>(previous) = {colors, font, dpi};
  else { auto *style = new ComboStyle{colors, font, dpi}; if (!SetWindowSubclass(window, comboProc, 1, reinterpret_cast<DWORD_PTR>(style))) delete style; }
  InvalidateRect(window, nullptr, FALSE);
}
bool editForm(HWND owner, const std::wstring &title, std::vector<FormField> &fields,
              const ThemeColors &colors, const std::function<void(bool)> &captureState) {
  Dialog dialog; dialog.owner = owner; dialog.colors = colors; dialog.fields = fields; dialog.captureState = captureState;
  dialog.run(title); if (dialog.accepted) fields = std::move(dialog.fields); return dialog.accepted;
}
std::optional<InputEvent> captureInput(HWND owner, const ThemeColors &colors, const std::function<void(bool)> &captureState) {
  Dialog dialog; dialog.owner = owner; dialog.colors = colors; dialog.mode = Dialog::Capture;
  if (captureState) captureState(true);
  dialog.run(L"录入输入");
  if (captureState) captureState(false);
  return dialog.captured;
}
int chooseAction(HWND owner, const std::wstring &title, const std::wstring &message,
                 const std::vector<std::wstring> &choices, const ThemeColors &colors) {
  Dialog dialog; dialog.owner = owner; dialog.colors = colors; dialog.message = message; dialog.buttons = choices; dialog.mode = Dialog::Choose;
  dialog.run(title); return dialog.choice;
}
void showDetails(HWND owner, const std::wstring &title, const std::wstring &text, const ThemeColors &colors) {
  Dialog dialog; dialog.owner = owner; dialog.colors = colors; dialog.message = text; dialog.mode = Dialog::Details; dialog.run(title);
}
std::wstring chooseFile(HWND owner, bool save, const wchar_t *title, const wchar_t *filterName, const wchar_t *pattern) {
  IFileDialog *dialog = nullptr;
  if (FAILED(CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) return {};
  COMDLG_FILTERSPEC filter{filterName, pattern}; dialog->SetFileTypes(1, &filter); dialog->SetTitle(title);
  DWORD options = 0; dialog->GetOptions(&options); dialog->SetOptions(options | FOS_FORCEFILESYSTEM | (save ? FOS_OVERWRITEPROMPT : FOS_FILEMUSTEXIST));
  if (save) dialog->SetDefaultExtension(L"zip");
  std::wstring result;
  if (SUCCEEDED(dialog->Show(owner))) {
    IShellItem *item = nullptr;
    if (SUCCEEDED(dialog->GetResult(&item))) {
      PWSTR path = nullptr; if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) { result = path; CoTaskMemFree(path); } item->Release();
    }
  }
  dialog->Release(); return result;
}
