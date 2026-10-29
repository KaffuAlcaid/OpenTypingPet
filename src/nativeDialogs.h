#pragma once
#include "appSettings.h"
#include "characterDefinition.h"
#include <functional>
#include <optional>
#include <vector>

enum class FieldKind { Text, Number, Choice, Check, Key, Multiline, Sequence };
struct FormField {
  std::string id;
  std::wstring label, value;
  FieldKind kind = FieldKind::Text;
  std::vector<std::wstring> choices;
  bool readOnly = false;
  std::vector<std::wstring> choiceValues;
};
void styleCombo(HWND window, const ThemeColors &colors, HFONT font, unsigned dpi);
bool editForm(HWND owner, const std::wstring &title, std::vector<FormField> &fields,
              const ThemeColors &colors, const std::function<void(bool)> &captureState = {});
std::optional<InputEvent> captureInput(HWND owner, const ThemeColors &colors,
                                      const std::function<void(bool)> &captureState = {});
int chooseAction(HWND owner, const std::wstring &title, const std::wstring &message,
                 const std::vector<std::wstring> &choices, const ThemeColors &colors);
void showDetails(HWND owner, const std::wstring &title, const std::wstring &text, const ThemeColors &colors);
std::wstring chooseFile(HWND owner, bool save, const wchar_t *title,
                        const wchar_t *filterName = L"角色包", const wchar_t *pattern = L"*.zip");
