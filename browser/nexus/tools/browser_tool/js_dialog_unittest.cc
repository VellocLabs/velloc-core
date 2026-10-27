// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"

#include <string>
#include <vector>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

OpenJsDialog Parse(const std::string& type,
                   const std::string& message = "hello",
                   const std::string& default_prompt = "") {
  base::DictValue params;
  params.Set("type", type);
  params.Set("message", message);
  params.Set("defaultPrompt", default_prompt);
  params.Set("url", "https://example.test/");
  std::optional<OpenJsDialog> dialog = ParseJsDialogOpening(params);
  EXPECT_TRUE(dialog.has_value()) << type;
  return dialog.value_or(OpenJsDialog());
}

std::vector<int> ButtonIndexes(const base::DictValue& info) {
  std::vector<int> indexes;
  for (const base::Value& button : *info.FindList("buttons")) {
    indexes.push_back(*button.GetDict().FindInt("index"));
  }
  return indexes;
}

TEST(JsDialogTest, ParsesEveryDialogType) {
  for (const char* type : {"alert", "confirm", "prompt", "beforeunload"}) {
    EXPECT_EQ(Parse(type).type, type);
  }
  OpenJsDialog prompt = Parse("prompt", "Name?", "anon");
  EXPECT_EQ(prompt.message, "Name?");
  EXPECT_EQ(prompt.default_prompt, "anon");
  EXPECT_EQ(prompt.url, "https://example.test/");
}

TEST(JsDialogTest, RejectsUnknownOrMissingType) {
  base::DictValue params;
  EXPECT_FALSE(ParseJsDialogOpening(params).has_value());
  params.Set("type", "print");
  EXPECT_FALSE(ParseJsDialogOpening(params).has_value());
}

TEST(JsDialogTest, ReservedIndexesNeverCollideWithNodeIds) {
  EXPECT_TRUE(IsJsDialogIndex(kJsDialogAcceptIndex));
  EXPECT_TRUE(IsJsDialogIndex(kJsDialogDismissIndex));
  EXPECT_TRUE(IsJsDialogIndex(kJsDialogTextIndex));
  EXPECT_FALSE(IsJsDialogIndex(0));
  EXPECT_FALSE(IsJsDialogIndex(1));
}

TEST(JsDialogTest, AlertHasOnlyTheAcceptButton) {
  OpenJsDialog alert = Parse("alert");
  EXPECT_EQ(JsDialogAcceptForIndex(alert, kJsDialogAcceptIndex), true);
  EXPECT_EQ(JsDialogAcceptForIndex(alert, kJsDialogDismissIndex),
            std::nullopt);
  base::DictValue info = BuildJsDialogInfo(alert, 7);
  EXPECT_EQ(ButtonIndexes(info), std::vector<int>({-1}));
  EXPECT_EQ(info.FindInt("text_input_index"), std::nullopt);
}

TEST(JsDialogTest, ConfirmAndBeforeUnloadHaveBothButtons) {
  OpenJsDialog confirm = Parse("confirm");
  EXPECT_EQ(JsDialogAcceptForIndex(confirm, kJsDialogAcceptIndex), true);
  EXPECT_EQ(JsDialogAcceptForIndex(confirm, kJsDialogDismissIndex), false);
  EXPECT_EQ(JsDialogAcceptForIndex(confirm, kJsDialogTextIndex),
            std::nullopt);
  EXPECT_EQ(ButtonIndexes(BuildJsDialogInfo(confirm, 7)),
            std::vector<int>({-1, -2}));

  base::DictValue unload = BuildJsDialogInfo(Parse("beforeunload", ""), 7);
  const base::ListValue& buttons = *unload.FindList("buttons");
  EXPECT_EQ(*buttons[0].GetDict().FindString("label"), "Leave");
  EXPECT_EQ(*buttons[1].GetDict().FindString("label"), "Stay");
}

TEST(JsDialogTest, InfoTellsTheModelHowToAnswer) {
  base::DictValue info = BuildJsDialogInfo(Parse("confirm", "Delete?"), 7);
  EXPECT_EQ(info.FindInt("tab_id"), 7);
  EXPECT_EQ(*info.FindString("type"), "confirm");
  EXPECT_EQ(*info.FindString("message"), "Delete?");
  const std::string& instructions = *info.FindString("instructions");
  EXPECT_NE(instructions.find("BrowserClick"), std::string::npos);
  EXPECT_NE(instructions.find("index -1"), std::string::npos);
  EXPECT_NE(instructions.find("index -2"), std::string::npos);
  EXPECT_EQ(instructions.find("BrowserType"), std::string::npos);
}

TEST(JsDialogTest, PromptOffersTheTextIndexAndShowsStagedText) {
  OpenJsDialog prompt = Parse("prompt", "Name?", "anon");
  base::DictValue info = BuildJsDialogInfo(prompt, 7);
  EXPECT_EQ(info.FindInt("text_input_index"), kJsDialogTextIndex);
  EXPECT_EQ(*info.FindString("default_prompt"), "anon");
  EXPECT_EQ(info.FindString("staged_text"), nullptr);
  const std::string& instructions = *info.FindString("instructions");
  EXPECT_NE(instructions.find("BrowserType"), std::string::npos);
  EXPECT_NE(instructions.find("index -3"), std::string::npos);

  prompt.prompt_text = "Ada";
  EXPECT_EQ(*BuildJsDialogInfo(prompt, 7).FindString("staged_text"), "Ada");
}

TEST(JsDialogTest, AnsweredBlockNamesTheButtonAndPromptText) {
  OpenJsDialog prompt = Parse("prompt", "Name?", "anon");
  base::DictValue defaulted = BuildJsDialogAnswered(prompt, true);
  EXPECT_EQ(*defaulted.FindString("button"), "OK");
  EXPECT_EQ(*defaulted.FindString("text"), "anon");

  prompt.prompt_text = "Ada";
  EXPECT_EQ(*BuildJsDialogAnswered(prompt, true).FindString("text"), "Ada");

  base::DictValue cancelled = BuildJsDialogAnswered(prompt, false);
  EXPECT_EQ(*cancelled.FindString("button"), "Cancel");
  EXPECT_EQ(cancelled.FindString("text"), nullptr);

  EXPECT_EQ(*BuildJsDialogAnswered(Parse("beforeunload", ""), false)
                 .FindString("button"),
            "Stay");
}

}  // namespace
}  // namespace nexus
