// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"

#include <utility>

#include "base/strings/strcat.h"

namespace nexus {

namespace {

bool IsKnownType(const std::string& type) {
  return type == "alert" || type == "confirm" || type == "prompt" ||
         type == "beforeunload";
}

bool HasDismissButton(const std::string& type) {
  return type != "alert";
}

std::string AcceptLabel(const std::string& type) {
  return type == "beforeunload" ? "Leave" : "OK";
}

std::string DismissLabel(const std::string& type) {
  return type == "beforeunload" ? "Stay" : "Cancel";
}

base::DictValue Button(int index, std::string label) {
  base::DictValue button;
  button.Set("index", index);
  button.Set("label", std::move(label));
  return button;
}

// Model-facing guidance carried in the result; the indexes are the
// constants above.
std::string Instructions(const OpenJsDialog& dialog) {
  std::string text = base::StrCat(
      {"The page opened a native ", dialog.type,
       " dialog. The tab is frozen until the dialog is answered, so other "
       "browser actions on it are refused. Answer it with BrowserClick: "
       "index -1 presses \"",
       AcceptLabel(dialog.type), "\""});
  if (HasDismissButton(dialog.type)) {
    base::StrAppend(&text, {", index -2 presses \"",
                            DismissLabel(dialog.type), "\""});
  }
  base::StrAppend(&text, {"."});
  if (dialog.type == "prompt") {
    base::StrAppend(&text,
                    {" To answer with your own text, first call BrowserType "
                     "with index -3 and the text, then BrowserClick index "
                     "-1; without it, \"OK\" submits the default value."});
  }
  return text;
}

}  // namespace

std::optional<OpenJsDialog> ParseJsDialogOpening(
    const base::DictValue& params) {
  const std::string* type = params.FindString("type");
  if (!type || !IsKnownType(*type)) {
    return std::nullopt;
  }
  OpenJsDialog dialog;
  dialog.type = *type;
  if (const std::string* message = params.FindString("message")) {
    dialog.message = *message;
  }
  if (const std::string* default_prompt = params.FindString("defaultPrompt")) {
    dialog.default_prompt = *default_prompt;
  }
  if (const std::string* url = params.FindString("url")) {
    dialog.url = *url;
  }
  return dialog;
}

bool IsJsDialogIndex(int index) {
  return index == kJsDialogAcceptIndex || index == kJsDialogDismissIndex ||
         index == kJsDialogTextIndex;
}

std::optional<bool> JsDialogAcceptForIndex(const OpenJsDialog& dialog,
                                           int index) {
  if (index == kJsDialogAcceptIndex) {
    return true;
  }
  if (index == kJsDialogDismissIndex && HasDismissButton(dialog.type)) {
    return false;
  }
  return std::nullopt;
}

base::DictValue BuildJsDialogInfo(const OpenJsDialog& dialog, int32_t tab_id) {
  base::DictValue info;
  info.Set("tab_id", tab_id);
  info.Set("type", dialog.type);
  info.Set("message", dialog.message);
  info.Set("url", dialog.url);
  if (dialog.type == "prompt") {
    info.Set("default_prompt", dialog.default_prompt);
    info.Set("text_input_index", kJsDialogTextIndex);
    if (dialog.prompt_text.has_value()) {
      info.Set("staged_text", *dialog.prompt_text);
    }
  }
  base::ListValue buttons;
  buttons.Append(Button(kJsDialogAcceptIndex, AcceptLabel(dialog.type)));
  if (HasDismissButton(dialog.type)) {
    buttons.Append(Button(kJsDialogDismissIndex, DismissLabel(dialog.type)));
  }
  info.Set("buttons", std::move(buttons));
  info.Set("instructions", Instructions(dialog));
  return info;
}

base::DictValue BuildJsDialogAnswered(const OpenJsDialog& dialog,
                                      bool accept) {
  base::DictValue answered;
  answered.Set("type", dialog.type);
  answered.Set("button",
               accept ? AcceptLabel(dialog.type) : DismissLabel(dialog.type));
  if (dialog.type == "prompt" && accept) {
    answered.Set("text", dialog.prompt_text.value_or(dialog.default_prompt));
  }
  return answered;
}

}  // namespace nexus
