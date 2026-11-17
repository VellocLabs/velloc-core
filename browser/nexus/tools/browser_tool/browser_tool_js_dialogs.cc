#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <string>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/memory/ref_counted.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

// =============================================================================
// Native JS dialogs (F-21).
// =============================================================================

namespace {

// Rewrites a result as the BrowserClick that answered a dialog.
ToolCallback AsDialogAnswerResult(ToolCallback done,
                                  base::DictValue answered) {
  auto shared =
      base::MakeRefCounted<base::RefCountedData<ToolCallback>>(std::move(done));
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> target,
             base::DictValue answered, base::Value value) {
            if (value.is_dict()) {
              value.GetDict().Set("action", "click");
              value.GetDict().Set("dialog_answered", std::move(answered));
            }
            std::move(target->data).Resolve(std::move(value));
          },
          shared, std::move(answered)),
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> target,
             std::string code, std::string /*message*/,
             base::DictValue params) {
            std::move(target->data).Reject(std::move(code), std::move(params));
          },
          shared));
}

}  // namespace

void BrowserTool::WatchJsDialogs(int32_t tab_id) {
  base::EraseIf(dialog_watchers_, [](const auto& entry) {
    return !entry.second->web_contents();
  });
  if (dialog_watchers_.contains(tab_id)) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return;
  }
  auto watcher = std::make_unique<JsDialogWatcher>(contents, tab_id, this);
  if (!watcher->Start()) {
    return;
  }
  dialog_watchers_.emplace(tab_id, std::move(watcher));
}

JsDialogWatcher* BrowserTool::FindOpenJsDialog() {
  auto work = dialog_watchers_.find(last_work_tab_.raw_value());
  if (work != dialog_watchers_.end() && work->second->open_dialog()) {
    return work->second.get();
  }
  for (auto& [tab_id, watcher] : dialog_watchers_) {
    if (watcher->open_dialog()) {
      return watcher.get();
    }
  }
  return nullptr;
}

bool BrowserTool::RefuseWhileJsDialogOpen(int32_t tab_id,
                                          std::string_view action_name,
                                          ToolCallback& done) {
  auto it = dialog_watchers_.find(tab_id);
  if (it == dialog_watchers_.end() || !it->second->open_dialog()) {
    return false;
  }
  base::DictValue result;
  result.Set("action", action_name);
  result.Set("ok", false);
  result.Set("dialog", BuildJsDialogInfo(*it->second->open_dialog(), tab_id));
  std::move(done).Resolve(std::move(result));
  return true;
}

ToolCallback BrowserTool::TrackInFlight(ToolCallback done,
                                        std::string_view action_name,
                                        int32_t tab_id) {
  auto action = base::MakeRefCounted<browser_tool_internal::InFlightAction>(
      std::move(done), std::string(action_name), tab_id);
  in_flight_ = action;
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<browser_tool_internal::InFlightAction> action,
             base::Value value) { action->Resolve(std::move(value)); },
          action),
      base::BindOnce(
          [](scoped_refptr<browser_tool_internal::InFlightAction> action,
             std::string code, std::string /*message*/,
             base::DictValue params) {
            action->Reject(std::move(code), std::move(params));
          },
          action));
}

void BrowserTool::AnswerJsDialog(JsDialogWatcher* watcher,
                                 bool accept,
                                 ToolCallback done) {
  const int32_t tab_id = watcher->tab_id();
  RecordWorkTab(tabs::TabHandle(tab_id));
  done = PausableBudgetWrap(std::move(done), action_timeout_, "click");
  base::DictValue answered =
      BuildJsDialogAnswered(*watcher->open_dialog(), accept);
  watcher->Answer(accept,
                  base::BindOnce(&BrowserTool::OnJsDialogAnswered,
                                 weak_ptr_factory_.GetWeakPtr(), tab_id,
                                 std::move(answered), std::move(done)));
}

void BrowserTool::OnJsDialogAnswered(int32_t tab_id,
                                     base::DictValue answered,
                                     ToolCallback done,
                                     bool ok) {
  if (!ok) {
    answered.Set("failed", true);
  }
  done = AsDialogAnswerResult(std::move(done), std::move(answered));
  // The action the dialog interrupted finishes now that the tab runs again;
  // its observation becomes this click's result.
  if (in_flight_ && in_flight_->tab_id() == tab_id &&
      in_flight_->awaiting_successor()) {
    in_flight_->SetSuccessor(std::move(done));
    return;
  }
  RunTabObservation(tab_id, "click", std::move(done),
                    base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                                   weak_ptr_factory_.GetWeakPtr()));
}

void BrowserTool::StageJsDialogText(std::string text, ToolCallback done) {
  JsDialogWatcher* watcher = FindOpenJsDialog();
  CHECK(watcher);
  OpenJsDialog* dialog = watcher->open_dialog();
  dialog->prompt_text = std::move(text);
  base::DictValue result;
  result.Set("action", "input");
  result.Set("ok", true);
  result.Set("dialog", BuildJsDialogInfo(*dialog, watcher->tab_id()));
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::OnJsDialogOpened(int32_t tab_id) {
  auto it = dialog_watchers_.find(tab_id);
  if (it == dialog_watchers_.end() || !it->second->open_dialog()) {
    return;
  }
  const OpenJsDialog& dialog = *it->second->open_dialog();
  LOG(INFO) << "[BrowserTool] JS dialog opened type=" << dialog.type
            << " tab_id=" << tab_id << " session=" << GetSessionKey();
  if (!in_flight_ || in_flight_->tab_id() != tab_id) {
    return;
  }
  base::DictValue result;
  result.Set("action", in_flight_->action_name());
  result.Set("ok", true);
  result.Set("dialog", BuildJsDialogInfo(dialog, tab_id));
  in_flight_->ResolveEarly(base::Value(std::move(result)));
}

void BrowserTool::OnJsDialogClosed(int32_t tab_id) {
  VLOG(1) << "[BrowserTool] JS dialog closed tab_id=" << tab_id
          << " session=" << GetSessionKey();
}

}  // namespace nexus
