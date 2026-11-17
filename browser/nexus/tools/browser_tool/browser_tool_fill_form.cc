#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <string>
#include <utility>
#include <vector>

#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

// =============================================================================
// BrowserFillForm — batch form fill. Each `fields` entry is parsed into a
// `FillFormFieldOp` and becomes one action (type / select / check / uncheck /
// click; check and uncheck always click), dispatched in one PerformActions,
// plus a final click when `submit_index` is set. Returns the standard
// observation envelope.
// =============================================================================

namespace {

namespace apc = optimization_guide::proto;

constexpr int kFillFormMaxFields = 50;

struct FillFormFieldOp {
  int32_t index = 0;
  std::string action;   // 'type' | 'select' | 'check' | 'uncheck' | 'click'
  std::string value;    // populated only for 'type' and 'select'
};

// Parses one base::DictValue (a fields[] entry) into FillFormFieldOp.
// Returns true on success. On failure, sets *error to a model-readable
// message naming the bad field index. Leaves *out_op untouched on error.
bool ParseFillFormEntry(const base::DictValue& entry,
                        size_t array_pos,
                        FillFormFieldOp* out_op,
                        base::DictValue* error) {
  error->Set("position", static_cast<int>(array_pos));
  const std::optional<int> index_opt = entry.FindInt("index");
  if (!index_opt.has_value()) {
    error->Set("reason", "field_index_missing");
    return false;
  }
  const std::string* action = entry.FindString("action");
  if (!action || action->empty()) {
    error->Set("reason", "field_action_missing");
    return false;
  }
  // Whitelist match — keep this in sync with the schema's enum hint.
  if (*action != "type" && *action != "select" && *action != "check" &&
      *action != "uncheck" && *action != "click") {
    error->Set("reason", "field_action_invalid");
    error->Set("value", *action);
    return false;
  }
  const bool needs_value = (*action == "type" || *action == "select");
  const std::string* value = entry.FindString("value");
  if (needs_value && (!value || value->empty())) {
    error->Set("reason", "field_value_required");
    error->Set("value", *action);
    return false;
  }
  out_op->index = *index_opt;
  out_op->action = *action;
  if (value) {
    out_op->value = *value;
  }
  return true;
}

}  // namespace

void BrowserTool::RunFillFormOnUI(::nexus::generated::BrowserFillFormInput input,
                                  ToolCallback done) {
  // ---- Parse + validate fields[] --------------------------------------
  if (input.fields.empty()) {
    std::move(done).Reject("invalid_args", ReasonParams("fields_empty"));
    return;
  }
  if (input.fields.size() > static_cast<size_t>(kFillFormMaxFields)) {
    {
      base::DictValue reject_params = ReasonParams("too_many_fields");
      reject_params.Set("count", static_cast<int>(input.fields.size()));
      reject_params.Set("max", static_cast<int>(kFillFormMaxFields));
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }
  std::vector<FillFormFieldOp> ops;
  ops.reserve(input.fields.size());
  for (size_t i = 0; i < input.fields.size(); ++i) {
    FillFormFieldOp op;
    base::DictValue err;
    if (!ParseFillFormEntry(input.fields[i], i, &op, &err)) {
      std::move(done).Reject("invalid_args", std::move(err));
      return;
    }
    ops.push_back(std::move(op));
  }

  // ---- Resolve every index against the node registry up front -------
  // We do this BEFORE building any proto so a stale index fails fast
  // with a clean error rather than mid-batch via the actor framework.
  std::vector<const NodeRegistry::Entry*> entries;
  entries.reserve(ops.size());
  std::optional<int32_t> common_tab_id;
  for (size_t i = 0; i < ops.size(); ++i) {
    const NodeRegistry::Entry* entry =
        LookupNode(ops[i].index, std::nullopt, done, "fill_form");
    if (!entry) {
      // LookupNode already rejected `done`.
      return;
    }
    // All fields must be on the same tab — the batch goes through one
    // PerformActions call against one tab_id. Cross-tab forms are a
    // pathological case the LLM should split into two BrowserFillForm
    // calls with a BrowserActivateTab between.
    if (common_tab_id.has_value() && *common_tab_id != entry->tab_id) {
      {
      base::DictValue reject_params = ReasonParams("field_tab_mismatch");
      reject_params.Set("position", static_cast<int>(i));
      reject_params.Set("index", ops[i].index);
      reject_params.Set("tab", entry->tab_id);
      reject_params.Set("common_tab", *common_tab_id);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
      return;
    }
    common_tab_id = entry->tab_id;
    entries.push_back(entry);
  }

  // ---- Submit-button resolution (optional) -------------------------
  const NodeRegistry::Entry* submit_entry = nullptr;
  if (input.submit_index.has_value()) {
    submit_entry = LookupNode(*input.submit_index, std::nullopt, done,
                              "fill_form");
    if (!submit_entry) {
      return;
    }
    if (common_tab_id.has_value() &&
        submit_entry->tab_id != *common_tab_id) {
      {
      base::DictValue reject_params = ReasonParams("submit_tab_mismatch");
      reject_params.Set("submit_index", *input.submit_index);
      reject_params.Set("tab", submit_entry->tab_id);
      reject_params.Set("common_tab", *common_tab_id);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
      return;
    }
  }

  // ---- Build the apc::Actions proto --------------------------------
  apc::Actions actions;
  const int32_t target_tab_id = *common_tab_id;
  for (size_t i = 0; i < ops.size(); ++i) {
    const FillFormFieldOp& op = ops[i];
    const NodeRegistry::Entry* entry = entries[i];
    if (op.action == "type") {
      apc::TypeAction* type = actions.add_actions()->mutable_type();
      type->mutable_target()->set_content_node_id(entry->content_node_id);
      type->mutable_target()
          ->mutable_document_identifier()
          ->set_serialized_token(entry->document_id);
      type->set_tab_id(entry->tab_id);
      type->set_text(op.value);
      type->set_mode(apc::TypeAction::DELETE_EXISTING);
      type->set_follow_by_enter(false);
    } else if (op.action == "select") {
      apc::SelectAction* select = actions.add_actions()->mutable_select();
      select->mutable_target()->set_content_node_id(entry->content_node_id);
      select->mutable_target()
          ->mutable_document_identifier()
          ->set_serialized_token(entry->document_id);
      select->set_tab_id(entry->tab_id);
      select->set_value(op.value);
    } else {
      // 'check', 'uncheck', and 'click' all dispatch as left-single
      // click. v1 doesn't read the checkbox's pre-state — so 'check'
      // on an already-checked box will toggle it back off. Future
      // enhancement: pre-read the registry's cached checkbox state
      // and skip the click when the state already matches.
      apc::ClickAction* click = actions.add_actions()->mutable_click();
      click->mutable_target()->set_content_node_id(entry->content_node_id);
      click->mutable_target()
          ->mutable_document_identifier()
          ->set_serialized_token(entry->document_id);
      click->set_tab_id(entry->tab_id);
      click->set_click_type(apc::ClickAction::LEFT);
      click->set_click_count(apc::ClickAction::SINGLE);
    }
  }

  // Optional final submit click.
  if (submit_entry) {
    apc::ClickAction* click = actions.add_actions()->mutable_click();
    click->mutable_target()->set_content_node_id(submit_entry->content_node_id);
    click->mutable_target()
        ->mutable_document_identifier()
        ->set_serialized_token(submit_entry->document_id);
    click->set_tab_id(submit_entry->tab_id);
    click->set_click_type(apc::ClickAction::LEFT);
    click->set_click_count(apc::ClickAction::SINGLE);
  }

  // stop_on_error is informational in v1 — the actor framework already
  // stops the batch at the first failing action. False would require a
  // per-field retry loop bypassing the batch model; out of scope today.
  // The flag is accepted from the schema for forward compatibility.
  (void)input.stop_on_error;

  RunActorAction(std::move(actions), "fill_form", std::move(done),
                  target_tab_id);
}

}  // namespace nexus
