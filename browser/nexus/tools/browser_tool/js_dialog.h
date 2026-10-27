// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_H_

// Native JavaScript dialogs (alert / confirm / prompt / beforeunload) as the
// browser tool presents them to the model: the open-dialog record parsed from
// CDP `Page.javascriptDialogOpening`, the `dialog` result block, and the
// reserved BrowserClick / BrowserType indexes that answer it (F-21).
// //base only.

#include <cstdint>
#include <optional>
#include <string>

#include "base/values.h"

namespace nexus {

// Reserved observation indexes. Negative, so they never collide with an APC
// content node id.
inline constexpr int kJsDialogAcceptIndex = -1;
inline constexpr int kJsDialogDismissIndex = -2;
inline constexpr int kJsDialogTextIndex = -3;

struct OpenJsDialog {
  // CDP dialog type: "alert", "confirm", "prompt" or "beforeunload".
  std::string type;
  std::string message;
  std::string default_prompt;
  std::string url;
  // Text staged by BrowserType on `kJsDialogTextIndex`; sent with an accept.
  std::optional<std::string> prompt_text;
};

// Parses `Page.javascriptDialogOpening` params; nullopt on an unknown type.
std::optional<OpenJsDialog> ParseJsDialogOpening(const base::DictValue& params);

bool IsJsDialogIndex(int index);

// The accept flag `index` stands for on `dialog`, or nullopt when the index
// is not a button of that dialog (an alert has only the accept button).
std::optional<bool> JsDialogAcceptForIndex(const OpenJsDialog& dialog,
                                           int index);

// The `dialog` block of a browser result: what is open, its button indexes,
// and how to answer it.
base::DictValue BuildJsDialogInfo(const OpenJsDialog& dialog, int32_t tab_id);

// The `dialog_answered` block of the result that answered a dialog.
base::DictValue BuildJsDialogAnswered(const OpenJsDialog& dialog, bool accept);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_H_
