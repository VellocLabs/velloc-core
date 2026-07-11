// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/tools/nexus_tool_signature.h"

#include <string>
#include <vector>

#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

base::DictValue MakeBrowserActionsArgs(base::ListValue actions) {
  base::DictValue args;
  args.Set("evaluation_previous_goal", "Previous goal was observed.");
  args.Set("memory", "Keep the current task context.");
  args.Set("next_goal", "Run the next browser action batch.");
  args.Set("actions", std::move(actions));
  return args;
}

base::DictValue MakeAction(std::string type) {
  base::DictValue action;
  action.Set("type", std::move(type));
  return action;
}

TEST(NexusToolSignatureTest, ComputesHashesPerBrowserActionItem) {
  base::DictValue click = MakeAction("click");
  click.Set("index", 7);

  base::DictValue input = MakeAction("input");
  input.Set("index", 8);
  input.Set("text", "  User@Example.COM  ");

  base::ListValue actions;
  actions.Append(std::move(click));
  actions.Append(std::move(input));

  std::vector<std::string> hashes =
      ComputeActionHashes("browser_actions",
                          MakeBrowserActionsArgs(std::move(actions)));

  ASSERT_EQ(hashes.size(), 2u);
  EXPECT_EQ(hashes[0].size(), 12u);
  EXPECT_EQ(hashes[1].size(), 12u);
  EXPECT_NE(hashes[0], hashes[1]);
}

TEST(NexusToolSignatureTest, OmitsLoopExemptActionItems) {
  base::ListValue actions;
  actions.Append(MakeAction("wait"));
  actions.Append(MakeAction("back"));
  actions.Append(MakeAction("done"));

  EXPECT_TRUE(ComputeActionHashes("browser_actions",
                                  MakeBrowserActionsArgs(std::move(actions)))
                  .empty());
}

TEST(NexusToolSignatureTest, SearchHashNormalizesTokenOrderAndCase) {
  base::DictValue first_search = MakeAction("search");
  first_search.Set("engine", "google");
  first_search.Set("query", "OpenAI pricing!");

  base::ListValue first_actions;
  first_actions.Append(std::move(first_search));

  base::DictValue second_search = MakeAction("search");
  second_search.Set("engine", "google");
  second_search.Set("query", "PRICING openai");

  base::ListValue second_actions;
  second_actions.Append(std::move(second_search));

  std::vector<std::string> first_hashes =
      ComputeActionHashes("browser_actions",
                          MakeBrowserActionsArgs(std::move(first_actions)));
  std::vector<std::string> second_hashes =
      ComputeActionHashes("browser_actions",
                          MakeBrowserActionsArgs(std::move(second_actions)));

  ASSERT_EQ(first_hashes.size(), 1u);
  ASSERT_EQ(second_hashes.size(), 1u);
  EXPECT_EQ(first_hashes[0], second_hashes[0]);
}

TEST(NexusToolSignatureTest, IgnoresNonBrowserActionsToolNames) {
  base::DictValue args;
  args.Set("index", 7);

  EXPECT_TRUE(ComputeActionHashes("click", args).empty());
  EXPECT_TRUE(ComputeActionHashes("browser_click", args).empty());
  EXPECT_FALSE(IsSequenceTerminator("click"));
  EXPECT_FALSE(IsSequenceTerminator("browser_click"));
  EXPECT_TRUE(IsSequenceTerminator("browser_actions"));
}

TEST(NexusToolSignatureTest, WrapResultEmitsOnlyActionHashesArray) {
  base::DictValue observation;
  observation.Set("ok", true);

  AgentToolMeta meta;
  meta.action_hashes = {"abc123def456", "fed654cba321"};
  meta.page_url = "https://example.test/";
  meta.page_interactive_count = 4;
  meta.page_dom_hash = "domhash";
  meta.post_url = "https://example.test/next";
  meta.post_focus_target_present = true;
  meta.post_focus_target_id = "7";
  meta.terminates_sequence = true;

  base::Value wrapped = WrapResultWithAgentMeta(
      base::Value(std::move(observation)), meta);
  ASSERT_TRUE(wrapped.is_dict());

  const base::DictValue& root = wrapped.GetDict();
  ASSERT_NE(root.FindDict("data"), nullptr);

  const base::DictValue* agent_meta = root.FindDict("agent_meta");
  ASSERT_NE(agent_meta, nullptr);
  EXPECT_EQ(agent_meta->Find("action_hash"), nullptr);

  const base::ListValue* action_hashes = agent_meta->FindList("action_hashes");
  ASSERT_NE(action_hashes, nullptr);
  ASSERT_EQ(action_hashes->size(), 2u);
  EXPECT_EQ((*action_hashes)[0].GetString(), "abc123def456");
  EXPECT_EQ((*action_hashes)[1].GetString(), "fed654cba321");
  EXPECT_TRUE(agent_meta->FindBool("terminates_sequence").value_or(false));
}

}  // namespace
}  // namespace nexus
