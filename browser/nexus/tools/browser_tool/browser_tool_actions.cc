#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <memory>
#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/logging.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/strings/escape.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/actor/actor_keyed_service.h"
#include "chrome/browser/actor/actor_proto_conversion.h"
#include "chrome/browser/actor/actor_task.h"
#include "chrome/browser/actor/actor_task_metadata.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/actor.mojom.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_evaluate.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/frame_document.h"
#include "custom_browser/browser/nexus/tools/browser_tool/download_capture.h"
#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"
#include "custom_browser/browser/nexus/tools/browser_tool/screenshot_shaping.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/upload/nexus_file_chooser_interceptor.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"
#include "url/gurl.h"


namespace nexus {

// File-local shorthand: same `using` directive browser_tool.cc uses,
// so call sites stay unqualified across both .cc files.
using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// Traffic annotation for URL-mode BrowserDownload. Click-mode downloads
// are page-initiated and carry the page's own annotation; only the
// programmatic `DownloadManager::DownloadUrl` path needs one here.
constexpr net::NetworkTrafficAnnotationTag kDownloadTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_browser_download", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "When the user's AI agent runs the BrowserDownload tool with a "
          "direct URL, the browser fetches that file using the active tab's "
          "session (cookies/credentials) and saves it into the agent's local "
          "workspace so the agent can read it."
        trigger:
          "The agent explicitly invokes the BrowserDownload tool with a url."
        data:
          "The requested URL and the active tab's cookies/credentials for "
          "that site."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting:
          "Controlled by the Nexus agent feature; only fires on an explicit "
          "agent tool call."
        policy_exception_justification:
          "Downloads are initiated only by explicit agent tool calls, "
          "mirroring a user clicking a download link."
      })");

// =============================================================================
// BrowserScreenshot capture + shaping.
// =============================================================================

// The capture BrowserScreenshot asks upstream for: lossless PNG at native
// resolution. No `max_width` / `max_height` on purpose — element boxes are in
// captured device pixels, so the crop must run on the unscaled image; the
// long-edge cap is applied afterwards by `ShapeScreenshotResult`.
std::optional<page_content_annotations::ScreenshotOptions::
                  ScreenshotCollectionOptions>
ModelScreenshotCaptureOptions() {
  page_content_annotations::ScreenshotOptions::ScreenshotCollectionOptions
      options;
  options.screenshot_image_format =
      page_content_annotations::ScreenshotOptions::ScreenshotImageFormat::kPng;
  // Anything but kLow selects the full-compression PNG encoder upstream.
  options.screenshot_compression_quality = page_content_annotations::
      ScreenshotOptions::ScreenshotCompressionQuality::kHigh;
  return options;
}

// Wraps `inner` so a successful BrowserScreenshot result is shaped on the
// thread pool (decode / crop / resize / encode is CPU work that must stay off
// the UI thread) before it resolves. Errors pass straight through. The two
// callbacks share `inner` through a ref-counted holder; if the wrapper or the
// reply is dropped unfired, `inner`'s own destructor reports the drop.
ToolCallback WrapWithScreenshotShaping(ToolCallback inner,
                                       std::optional<int32_t> crop_index) {
  auto holder = base::MakeRefCounted<base::RefCountedData<ToolCallback>>(
      std::move(inner));
  return ToolCallback(
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> h,
             std::optional<int32_t> index, base::Value value) {
            if (!value.is_dict()) {
              std::move(h->data).Resolve(std::move(value));
              return;
            }
            base::ThreadPool::PostTaskAndReplyWithResult(
                FROM_HERE,
                {base::TaskPriority::USER_BLOCKING,
                 base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
                base::BindOnce(&ShapeScreenshotResult,
                               std::move(value).TakeDict(), index,
                               kScreenshotMaxEdge),
                base::BindOnce(
                    [](ToolCallback done, base::DictValue shaped) {
                      std::move(done).Resolve(std::move(shaped));
                    },
                    std::move(h->data)));
          },
          holder, crop_index),
      base::BindOnce(
          [](scoped_refptr<base::RefCountedData<ToolCallback>> h,
             std::string code, std::string /*message*/,
             base::DictValue params) {
            std::move(h->data).Reject(std::move(code), std::move(params));
          },
          holder));
}

// =============================================================================
// File-local helpers used by `read_page` / `find_text` / `read_element`
// completions. Anonymous-namespaced because they're not referenced from
// browser_tool.cc.
// =============================================================================

// Trims `s` to end on a UTF-8 boundary, dropping a partial trailing sequence:
// base::Value CHECKs that strings are valid UTF-8.
void EnsureUtf8TailComplete(std::string& s) {
  if (s.empty()) {
    return;
  }
  // Walk back from the last byte until we find a leading byte (one
  // whose top two bits are NOT `10`). Continuation bytes match
  // `(b & 0xC0) == 0x80`.
  size_t i = s.size();
  while (i > 0) {
    --i;
    const unsigned char b = static_cast<unsigned char>(s[i]);
    if ((b & 0xC0) == 0x80) {
      continue;  // continuation byte
    }
    // Determine the expected sequence length from the leading byte.
    int len = 1;
    if ((b & 0x80) == 0x00) {
      len = 1;  // ASCII
    } else if ((b & 0xE0) == 0xC0) {
      len = 2;
    } else if ((b & 0xF0) == 0xE0) {
      len = 3;
    } else if ((b & 0xF8) == 0xF0) {
      len = 4;
    }
    const size_t tail_len = s.size() - i;
    if (tail_len == static_cast<size_t>(len)) {
      // Complete sequence — keep as-is.
      return;
    }
    // Sequence at `i` is incomplete. Drop it (and everything after).
    s.resize(i);
    return;
  }
  // No leading byte found at all (corrupt input) — clear it.
  s.clear();
}

// Joins the leaf text of every TEXT / PARAGRAPH / HEADING / LIST_ITEM
// node beneath `node` into `out`, separating consecutive text fragments
// with a single newline so the LLM can reason about paragraph breaks.
// Stops appending once `out` reaches `max_chars` and returns `false`
// to signal the walker to abort early.
bool AppendVisibleText(const apc::ContentNode& node,
                       int max_chars,
                       std::string& out) {
  if (static_cast<int>(out.size()) >= max_chars) {
    return false;
  }
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    apc::ContentAttributeType t = attrs.attribute_type();
    const bool is_text_carrier =
        t == apc::CONTENT_ATTRIBUTE_TEXT ||
        t == apc::CONTENT_ATTRIBUTE_PARAGRAPH ||
        t == apc::CONTENT_ATTRIBUTE_HEADING ||
        t == apc::CONTENT_ATTRIBUTE_LIST_ITEM;
    if (is_text_carrier && attrs.has_text_data()) {
      const std::string& s = attrs.text_data().text_content();
      if (!s.empty()) {
        if (!out.empty()) {
          out.push_back('\n');
        }
        const int budget = max_chars - static_cast<int>(out.size());
        if (budget <= 0) {
          return false;
        }
        if (static_cast<int>(s.size()) <= budget) {
          out.append(s);
        } else {
          // Byte-level slice may fall in the middle of a multi-byte
          // UTF-8 sequence (Chinese / Japanese / Korean / emoji etc.).
          // Append the prefix then walk back to the last full sequence
          // boundary so `out` stays valid UTF-8 — base::Value::Set
          // CHECKs UTF-8 validity and crashes the process otherwise.
          out.append(s, 0, static_cast<size_t>(budget));
          EnsureUtf8TailComplete(out);
          return false;
        }
      }
    }
  }
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (!AppendVisibleText(child, max_chars, out)) {
      return false;
    }
  }
  return true;
}

// Lower-cases ASCII characters in-place. The LLM-supplied query is
// expected to be plain ASCII for now; full Unicode case folding would
// require ICU, which we can wire later when a real CJK case-fold use
// case shows up.
std::string ToLowerAscii(std::string_view in) {
  std::string out;
  out.reserve(in.size());
  for (char c : in) {
    out.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  return out;
}

// Recursive walker for `find_text`. For every node whose visible
// text contains `query_lower`, appends a `matches[]` entry with the text
// plus its nearest actionable ancestor's index / type / text (so
// the LLM can hand the ancestor's index straight to `click`).
//
// The ancestor index is the one the observation walk registered for that
// node (`NodeRegistry::IndexOf`); -1 when it has none.
struct FindTextWalker {
  // Owned (not a view) — the walker outlives the temporary returned by
  // ToLowerAscii(query) at construction time, so we copy the lowered
  // form into the struct.
  std::string query_lower;
  int max_results = 5;
  base::ListValue matches;
  raw_ptr<const NodeRegistry> registry = nullptr;
  int tab_id = 0;
  std::string main_document_id;
  // Document the node being visited belongs to.
  std::string document_id;
  // Stack of nearest actionable ancestor indexes.
  std::vector<int> ancestor_node_id_stack;
  std::vector<std::string> ancestor_type_stack;
  std::vector<std::string> ancestor_text_stack;
};

void FindTextWalk(const apc::ContentNode& node, FindTextWalker& w) {
  if (static_cast<int>(w.matches.size()) >= w.max_results) {
    return;
  }
  bool pushed_ancestor = false;
  if (node.has_content_attributes()) {
    const apc::ContentAttributes& attrs = node.content_attributes();
    if (IsActionable(attrs) && attrs.has_common_ancestor_dom_node_id()) {
      const std::optional<int> index = w.registry->IndexOf(
          w.tab_id, w.document_id, attrs.common_ancestor_dom_node_id(),
          w.document_id == w.main_document_id);
      w.ancestor_node_id_stack.push_back(index.value_or(-1));
      w.ancestor_type_stack.push_back(
          ContentAttributeTypeName(attrs.attribute_type()));
      w.ancestor_text_stack.push_back(ExtractNodeText(attrs));
      pushed_ancestor = true;
    }

    // Match check on the current node's leaf text. We use
    // CONTENT_ATTRIBUTE_TEXT / PARAGRAPH / HEADING / LIST_ITEM /
    // ANCHOR / FORM_CONTROL because those are the carriers the LLM
    // would search for.
    if (attrs.has_text_data()) {
      const std::string& raw = attrs.text_data().text_content();
      if (!raw.empty()) {
        const std::string lowered = ToLowerAscii(raw);
        if (lowered.find(w.query_lower) != std::string::npos) {
          base::DictValue match;
          match.Set("text", raw);
          if (!w.ancestor_node_id_stack.empty() &&
              w.ancestor_node_id_stack.back() >= 0) {
            match.Set("clickable_ancestor_index",
                      w.ancestor_node_id_stack.back());
            match.Set("ancestor_type", w.ancestor_type_stack.back());
            if (!w.ancestor_text_stack.back().empty()) {
              match.Set("ancestor_text", w.ancestor_text_stack.back());
            }
          }
          w.matches.Append(std::move(match));
        }
      }
    }
  }
  const std::string parent_document_id = w.document_id;
  w.document_id = ChildDocumentId(node, parent_document_id);
  for (const apc::ContentNode& child : node.children_nodes()) {
    if (static_cast<int>(w.matches.size()) >= w.max_results) {
      break;
    }
    FindTextWalk(child, w);
  }
  w.document_id = parent_document_id;
  if (pushed_ancestor) {
    w.ancestor_node_id_stack.pop_back();
    w.ancestor_type_stack.pop_back();
    w.ancestor_text_stack.pop_back();
  }
}

}  // namespace

// =============================================================================
// Private helper: RunCreateTabAction, for `RunNavigateOnUI` (new_tab=true) and
// `RunSearch`; always opens a new tab.
// =============================================================================

void BrowserTool::RunCreateTabAction(std::string url,
                                     std::string action_name,
                                     ToolCallback done) {
  // No `url.empty()` guard — empty required strings are rejected by
  // the codegen-emitted Extract() before this body runs.
  Profile* profile = GetProfile();

  // ---- New-tab path ----------------------------------------------------
  // CreateTabAction.window_id is required by `CreateCreateTabRequest`
  // (`!action.has_window_id()` returns nullptr → "failed to build
  // ToolRequest"), so resolve the target window's SessionID up front.
  // ResolveCreateTabWindowSessionId honors the
  // `nexus.browser_tools_separate_window` pref: when off it returns the
  // active (Nexus) window; when on it returns a dedicated agent window,
  // creating one on demand.
  std::optional<int32_t> active_window =
      ResolveCreateTabWindowSessionId(profile);
  if (!active_window.has_value()) {
    std::move(done).Reject("no_active_window", ActionParams(action_name));
    return;
  }

  // Materialize the actor task and snapshot its known-tabs set BEFORE
  // the create. After the CreateTab settles, the new tab will have been
  // added to this set via `TabManagementTool::UpdateTaskAfterInvoke` →
  // `ActorTask::AddTab`, so a set diff identifies the new TabHandle.
  std::optional<actor::TaskId> task_id = EnsureTaskCreated(done);
  if (!task_id.has_value()) {
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "unavailable"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "actor"));
    return;
  }
  actor::ActorTask::TabHandleSet tabs_before = task->GetTabs();

  // Build and dispatch the standalone CreateTab action.
  apc::Actions create_actions;
  create_actions.set_task_id(task_id->value());
  apc::CreateTabAction* create =
      create_actions.add_actions()->mutable_create_tab();
  create->set_window_id(*active_window);
  create->set_foreground(true);

  actor::BuildToolRequestResult tool_request_result =
      actor::BuildToolRequest(create_actions);
  if (!tool_request_result.has_value()) {
    std::move(done).Reject("invalid_action", ActionParams(action_name, "create_tab"));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  service->PerformActions(
      *task_id, std::move(tool_request_result.value()),
      actor::ActorTaskMetadata{},
      base::BindOnce(&BrowserTool::OnCreateTabActionDone,
                     weak_ptr_factory_.GetWeakPtr(), std::move(done),
                     std::move(url), std::move(action_name),
                     std::move(tabs_before), start_time));
}

void BrowserTool::OnCreateTabActionDone(
    ToolCallback done,
    std::string url,
    std::string action_name,
    actor::ActorTask::TabHandleSet tabs_before,
    base::TimeTicks /*start_time*/,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TabObservationStrategy /*observation_strategy*/) {
  // 149 folded the standalone result_code / index_of_failed_action out of
  // PerformActionsCallback into the per-action `action_results` vector;
  // reduce it via upstream's canonical helper (first non-OK wins). This
  // CreateTab request is single-action, so the result is just its code.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  if (!actor::IsOk(result_code)) {
    {
      base::DictValue reject_params = ActionParams(action_name);
      reject_params.Set("code", static_cast<int>(result_code));
      std::move(done).Reject("create_tab_failed", std::move(reject_params));
    }
    return;
  }
  Profile* profile = GetProfile();
  if (!profile || !task_id_.has_value()) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "navigate"));
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "navigate"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "navigate_actor"));
    return;
  }

  // Locate the freshly-created tab by diffing the task's tab set
  // against the pre-create snapshot. `TabManagementTool` calls
  // `task.AddTab(target_tab_)` in `UpdateTaskAfterInvoke`, so by the
  // time this callback fires the new TabHandle is in `task->GetTabs()`.
  std::optional<tabs::TabHandle> new_tab;
  for (const tabs::TabHandle& handle : task->GetTabs()) {
    if (tabs_before.find(handle) == tabs_before.end()) {
      new_tab = handle;
      break;
    }
  }
  if (!new_tab.has_value()) {
    std::move(done).Reject("tab_not_observed", ActionParams(action_name));
    return;
  }

  // Bind the new tab to this Nexus session before issuing the navigate.
  // BrowserTool::BindTabToSession attaches the WebContents tag (source
  // of truth), adds the tab to this session's tab group (creating the
  // group lazily on the first session tab), records membership in
  // `session_tabs_`, and marks it as last-active. No-op when the
  // feature flag is off or the session key is empty.
  BindTabToSession(*new_tab);

  // Stage 2 — navigate the new tab through `RunActorAction`, with a trailing
  // `WaitAction`: the snapshot otherwise fires at initial commit and can catch
  // an empty about:blank transition.
  apc::Actions nav_actions;
  apc::NavigateAction* nav = nav_actions.add_actions()->mutable_navigate();
  nav->mutable_url()->assign(url);
  nav->set_tab_id(new_tab->raw_value());

  apc::WaitAction* wait = nav_actions.add_actions()->mutable_wait();
  wait->set_wait_time_ms(2000);
  wait->set_observe_tab_id(new_tab->raw_value());

  // The freshly-created tab IS the working tab for this stage-2
  // navigate — emit its full elements in the observation.
  RunActorAction(std::move(nav_actions), action_name, std::move(done),
                  new_tab->raw_value());
}

// =============================================================================
// Observation runner. Bypasses the actor `PerformActions` path entirely:
// marks the target tab as "observe-once" on the actor task, then calls
// `BuildActionsResultWithObservations` directly with no actions — the
// framework still fetches APC + screenshot for the marked tabs because
// it walks `task->GetLastActedTabs()`, into which `ObserveTabOnce`
// inserted the tab.
// =============================================================================

void BrowserTool::RunTabObservation(int32_t tab_id_value,
                                     std::string_view action_name,
                                     ToolCallback done,
                                     ObservationCompletion completion) {
  // Observation-only actions still operate on a specific tab — record
  // it as the most-recent worked-on tab so a subsequent tab-less call
  // can default to it via `LastWorkTab()` when Chrome's foreground
  // tab isn't session-owned. Mirrors `RunActorAction`.
  RecordWorkTab(tabs::TabHandle(tab_id_value));
  WatchJsDialogs(tab_id_value);
  if (RefuseWhileJsDialogOpen(tab_id_value, action_name, done)) {
    return;
  }

  // Same per-action budget the actor-driven path uses — observation-
  // only actions (screenshot, read_page, find_text) also dispatch
  // through the actor framework and can hang on a stalled APC fetch.
  done = PausableBudgetWrap(std::move(done), action_timeout_,
                            std::string(action_name));
  done = TrackInFlight(std::move(done), action_name, tab_id_value);

  Profile* profile = GetProfile();
  if (!profile) {
    std::move(done).Reject("no_profile", ActionParams(action_name, "tab"));
    return;
  }
  std::optional<actor::TaskId> task_id_opt = EnsureTaskCreated(done);
  if (!task_id_opt.has_value()) {
    return;
  }
  auto* service = actor::ActorKeyedService::Get(profile);
  if (!service) {
    std::move(done).Reject("no_actor_service", ActionParams(action_name, "unavailable"));
    return;
  }
  actor::ActorTask* task = service->GetTask(*task_id_opt);
  if (!task) {
    std::move(done).Reject("task_missing", ActionParams(action_name, "actor"));
    return;
  }

  // Validate the target tab is live before handing it to the actor task.
  // ObserveTabOnce silently no-ops on a dead handle, but a silent no-op
  // would leave the upstream pipeline walking only `controlled_tabs_`
  // (potentially a stale carryover from a prior action whose
  // ActorTabData::last_observed_page_content_ is unsafe to overwrite),
  // which is the crash window described at the
  // OnActorPerformActionsDone guard.
  tabs::TabHandle target_handle(tab_id_value);
  if (!target_handle.Get()) {
    std::move(done).Reject("tab_gone", ActionParams(action_name));
    return;
  }
  task->ObserveTabOnce(target_handle);

  // BuildActionsResultWithObservations walks `task->GetLastActedTabs()`
  // (= controlled_tabs_ ∪ to_observe_tabs_), not `GetTabs()` — so the
  // empty-set guard mirrored from OnActorPerformActionsDone has to be
  // checked against that union here.
  if (task->GetLastActedTabs().empty()) {
    std::move(done).Reject("no_tabs", ActionParams(action_name));
    return;
  }

  base::TimeTicks start_time = base::TimeTicks::Now();
  // Chromium 149 signature: no prior action results here, so an empty vector.
  // Screenshot options only for BrowserScreenshot, the one action whose pixels
  // reach the model.
  actor::BuildActionsResultWithObservations(
      *profile, start_time,
      /*action_results=*/{},
      *task,
      /*skip_async_observation_information=*/false,
      /*screenshot_collection_options=*/
      action_name == "screenshot" ? ModelScreenshotCaptureOptions()
                                  : std::nullopt,
      base::BindOnce(std::move(completion), std::move(done),
                     std::string(action_name)));
}

// =============================================================================
// LLM-facing tool methods (24 of them). Each is the single entry point
// for one declarative *.tool.json. The codegen-emitted dispatcher
// trampoline in nexus_tools_registry.gen.cc routes here with typed
// input pre-extracted from the LLM's tool_use args.
// =============================================================================

void BrowserTool::RunScreenshotOnUI(::nexus::generated::BrowserScreenshotInput input,
                                    ToolCallback done) {
  // Phase 13: schema no longer exposes tab_id — always target the
  // currently-active tab. Cross-tab screenshots require BrowserActivateTab
  // first.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "screenshot", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // Shape the image (optional element crop, long-edge cap, PNG) on its way
  // out — see screenshot_shaping.h. Wrapping `done` covers every resolve
  // site of `OnActionsResultBuilt` (ok, actor error, settle-guard fallback)
  // in one place.
  done = WrapWithScreenshotShaping(std::move(done), input.index);
  // Reuse the standard `OnActionsResultBuilt`: it routes the `"screenshot"`
  // action name to `BuildObservationDict(include_screenshot_image=true)`, so
  // the working tab's dict carries the actual `screenshot: {mime,data_base64}`
  // image (every other action omits it). The `action_name == "screenshot"`
  // gate downstream is what distinguishes this path.
  RunTabObservation(
      *resolved_tab_id, "screenshot", std::move(done),
      base::BindOnce(&BrowserTool::OnActionsResultBuilt,
                     weak_ptr_factory_.GetWeakPtr()));
}

void BrowserTool::RunReadPageOnUI(::nexus::generated::BrowserReadPageInput input,
                                  ToolCallback done) {
  // Phase 13: schema no longer exposes tab_id / scope / max_chars.
  // tab_id → always-active; scope/max_chars → fixed server-side defaults
  // (full-page walk capped at 4000 chars; the body sets `truncated` when
  // the cap is hit). The legacy two-scope split is dead-code today (the
  // actor framework's APC fetch is always full-page either way), so
  // collapsing to a single mode is a no-op behavior change.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "read_page", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  constexpr int kReadPageMaxChars = 4000;
  RunTabObservation(
      *resolved_tab_id, "read_page", std::move(done),
      base::BindOnce(&BrowserTool::OnReadPageObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(),
                     /*scope=*/std::string("full_page"),
                     /*max_chars=*/kReadPageMaxChars,
                     *resolved_tab_id,
                     /*include_observation=*/
                     input.include_observation.value_or(false)));
}

void BrowserTool::RunNavigateOnUI(::nexus::generated::BrowserNavigateInput input,
                                  ToolCallback done) {
  // new_tab=true opens via `RunCreateTabAction` with action_name "navigate";
  // otherwise the active tab navigates in place.
  if (input.new_tab.value_or(false)) {
    RunCreateTabAction(std::move(input.url),
                       /*action_name=*/"navigate",
                       std::move(done));
    return;
  }

  // First-tab auto-promotion: with no session tab yet, route through the
  // new-tab path instead of rejecting with `no_session_tab`.
  if (CollectSessionTabs().empty()) {
    LOG(INFO) << "[BrowserTool] navigate: session owns no tabs; "
                 "auto-promoting to new_tab=true";
    RunCreateTabAction(std::move(input.url),
                       /*action_name=*/"navigate",
                       std::move(done));
    return;
  }

  // ---- In-place navigation on the active tab ---------------------------
  // The actor's CreateNavigateRequest rejects if `tab_id` is unset
  // (`tab_handle == TabHandle::Null()`), so we must resolve the active
  // tab handle and emit it explicitly.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "navigate", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::NavigateAction* navigate = actions.add_actions()->mutable_navigate();
  navigate->mutable_url()->assign(input.url);
  navigate->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "navigate", std::move(done),
                  *resolved_tab_id);
}

void BrowserTool::RunDownloadOnUI(::nexus::generated::BrowserDownloadInput input,
                                  ToolCallback done) {
  // Exactly one of index|url. (index = click to trigger a download whose
  // URL we can't see; url = direct download using the page's session.)
  const bool has_index = input.index.has_value();
  const bool has_url = input.url.has_value() && !input.url->empty();
  if (has_index == has_url) {
    std::move(done).Reject("invalid_args", ReasonParams("download_mode"));
    return;
  }

  // Downloads land in the session workspace's `downloads/` folder so the
  // file tools (which sandbox to the session cwd) can read them.
  ::nexus::ToolSession* session = GetSession();
  const base::FilePath cwd = session ? session->cwd() : base::FilePath();
  if (cwd.empty()) {
    std::move(done).Reject("no_workspace", ReasonParams("download"));
    return;
  }
  const base::FilePath dest_dir = cwd.Append(FILE_PATH_LITERAL("downloads"));

  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  if (!manager) {
    std::move(done).Reject("download_failed", ReasonParams("manager_unavailable"));
    return;
  }

  // Bound the wait on completion by the per-action budget; DownloadCapture
  // returns an in-progress result (rather than blocking) if a large file
  // is still streaming when this elapses.
  const base::TimeDelta budget = action_timeout_;

  // Optional caller filename — basename only, to block path traversal.
  std::optional<std::string> filename;
  if (input.filename && !input.filename->empty()) {
    filename = base::FilePath::FromUTF8Unsafe(*input.filename)
                   .BaseName()
                   .AsUTF8Unsafe();
  }

  if (has_url) {
    // Associate the download with the active tab so its cookies/auth apply
    // (the whole point of an in-browser download vs. a bare fetch).
    std::optional<int32_t> tab_id =
        ResolveTabIdOrReject(std::nullopt, GetProfile(), "download", done, this);
    if (!tab_id.has_value()) {
      return;
    }
    tabs::TabInterface* tab = tabs::TabHandle(*tab_id).Get();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    content::RenderFrameHost* rfh =
        contents ? contents->GetPrimaryMainFrame() : nullptr;
    if (!rfh) {
      std::move(done).Reject("no_active_tab", ReasonParams("download_page"));
      return;
    }
    GURL url(*input.url);
    if (!url.is_valid()) {
      {
      base::DictValue reject_params = ReasonParams("invalid_url");
      reject_params.Set("url", *input.url);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
      return;
    }
    // Derive a name from the URL when the caller didn't supply one.
    std::string name = filename.value_or(url.ExtractFileName());
    if (name.empty()) {
      name = "download";
    }
    // We force the download straight into <cwd>/downloads/ (so it streams
    // there with no post-completion move and no "Save As" prompt), but the
    // download writer won't create that directory and a freshly-created
    // session workspace doesn't have it yet — without this the transfer
    // interrupts with FILE_FAILED. Create the dir on a blocking pool, then
    // resume on the UI thread to start the download. Match nexus_fs_host's
    // ctor pattern (hop to thread_pool for the blocking CreateDirectory).
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath dir) { return base::CreateDirectory(dir); },
            dest_dir),
        base::BindOnce(&BrowserTool::StartUrlDownloadAfterDirReady,
                       weak_ptr_factory_.GetWeakPtr(), *tab_id, *input.url,
                       dest_dir, std::move(name), budget, std::move(done)));
    return;
  }

  // index mode: arm the capture, then click the element to trigger the
  // download. The click's own post-action observation is discarded (sink
  // callback) — DownloadCapture resolves the model's `done` when the
  // download the click produces completes (or the budget elapses, or no
  // download starts within the arm window).
  const NodeRegistry::Entry* entry =
      LookupNode(*input.index, std::nullopt, done, "download");
  if (!entry) {
    return;
  }

  scoped_refptr<DownloadCapture> capture = DownloadCapture::Create(
      manager, dest_dir, std::move(filename), budget, std::move(done));
  // 15s window for the click to actually start a download (covers the
  // actor round-trip + the page's navigation/JS that kicks it off).
  capture->ArmForClickCapture(base::Seconds(15));

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "download", ToolCallback(),
                 entry->tab_id);
}

void BrowserTool::StartUrlDownloadAfterDirReady(int32_t tab_id,
                                                std::string url,
                                                base::FilePath dest_dir,
                                                std::string name,
                                                base::TimeDelta budget,
                                                ToolCallback done,
                                                bool dir_ok) {
  if (!dir_ok) {
    std::move(done).Reject("download_failed", ReasonParams("downloads_dir"));
    return;
  }
  // Re-resolve everything on the UI thread — pointers can't safely cross the
  // mkdir thread hop, and the tab/manager may have changed while we waited.
  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  if (!manager) {
    std::move(done).Reject("download_failed", ReasonParams("manager_unavailable"));
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  content::RenderFrameHost* rfh =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!rfh) {
    std::move(done).Reject("no_active_tab", ReasonParams("download_page_gone"));
    return;
  }
  const base::FilePath dest =
      dest_dir.Append(base::FilePath::FromUTF8Unsafe(name));

  std::unique_ptr<download::DownloadUrlParameters> params =
      rfh->CreateDownloadUrlParameters(GURL(url), kDownloadTrafficAnnotation);
  params->set_file_path(dest);
  params->set_prompt(false);

  scoped_refptr<DownloadCapture> capture = DownloadCapture::Create(
      manager, dest_dir, std::move(name), budget, std::move(done));
  params->set_callback(base::BindOnce(
      [](scoped_refptr<DownloadCapture> cap, download::DownloadItem* item,
         download::DownloadInterruptReason reason) {
        cap->BindStartedItem(item,
                             reason == download::DOWNLOAD_INTERRUPT_REASON_NONE);
      },
      capture));
  manager->DownloadUrl(std::move(params),
                       /*blob_url_loader_factory=*/nullptr);
}

void BrowserTool::RunUploadOnUI(::nexus::generated::BrowserUploadInput input,
                                ToolCallback done) {
  // The file <input> (or the control that opens the picker). Its home tab
  // is the upload target.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "upload");
  if (!entry) {
    return;
  }

  // Resolve the local file path: absolute used as-is; relative resolves
  // against the session workspace (same sandbox the file tools use).
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    ::nexus::ToolSession* session = GetSession();
    const base::FilePath cwd = session ? session->cwd() : base::FilePath();
    if (cwd.empty()) {
      std::move(done).Reject("no_workspace", ReasonParams("file_path"));
      return;
    }
    path = cwd.Append(path);
  }

  tabs::TabInterface* tab = tabs::TabHandle(entry->tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    std::move(done).Reject("no_active_tab", ReasonParams("target_tab_gone"));
    return;
  }

  // Stage the file for the next chooser on this tab, then click the input.
  // The guarded hook in Browser::RunFileChooser answers the chooser the
  // click opens with `path` (no native dialog). The click's post-action
  // observation flows back to the model so it can confirm the attachment.
  // NOTE: existence isn't checked here (UI thread); a missing file simply
  // yields a failed attachment the model can see in the next observation.
  NexusPendingUpload::Arm(contents, {path});

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "upload", std::move(done), entry->tab_id);
}

namespace {

// Soft cap on the JSON-stringified value returned to the model. Mirrors
// browser-use's 20k cap (they truncate after serialization too). Anything
// past this is replaced with a "[truncated]" sentinel rather than silently
// cut mid-token so the model can detect when it lost data.
constexpr size_t kEvaluateResultMaxBytes = 20 * 1024;

// Stringifies a base::Value into JSON, then truncates to the soft cap.
// Returns `truncated` so the caller can flag it on the observation.
std::string SerializeAndTruncateForEvaluate(const base::Value& value,
                                            bool* truncated_out) {
  std::string serialized;
  if (!base::JSONWriter::Write(value, &serialized)) {
    *truncated_out = false;
    return std::string("\"[unserializable]\"");
  }
  if (serialized.size() <= kEvaluateResultMaxBytes) {
    *truncated_out = false;
    return serialized;
  }
  // Keep the head; the model can re-run a narrower query if it lost data.
  serialized.resize(kEvaluateResultMaxBytes);
  serialized.append("…[truncated]");
  *truncated_out = true;
  return serialized;
}

// Builds the success observation from a CDP Runtime.evaluate result:
// `exceptionDetails` → `error` + `exception` (still ok:true); `result.value` →
// JSON-stringified and capped; otherwise `result.description`.
base::DictValue BuildEvaluateObservation(base::DictValue cdp_result) {
  base::DictValue out;
  out.Set("ok", true);
  out.Set("action", "evaluate");

  if (base::DictValue* exc = cdp_result.FindDict("exceptionDetails")) {
    // The JS threw. We pass `ok:true` (the tool *call* succeeded — only the
    // user-supplied JS failed) but surface the exception verbatim so the model
    // can read the stack and adapt.
    // The page's own exception text when it gave one; the runtime renders
    // the fallback for a bare `exception`.
    if (const std::string* text = exc->FindString("text")) {
      out.Set("error", *text);
    }
    out.Set("exception", base::Value(std::move(*exc)));
    return out;
  }

  base::DictValue* obj = cdp_result.FindDict("result");
  if (!obj) {
    out.Set("result", std::string());
    out.Set("type", "undefined");
    return out;
  }
  const std::string* type = obj->FindString("type");
  if (type) {
    out.Set("type", *type);
  }
  if (const std::string* subtype = obj->FindString("subtype")) {
    out.Set("subtype", *subtype);
  }

  if (base::Value* value = obj->Find("value")) {
    bool truncated = false;
    std::string serialized =
        SerializeAndTruncateForEvaluate(*value, &truncated);
    out.Set("result", std::move(serialized));
    if (truncated) {
      out.Set("truncated", true);
    }
  } else if (const std::string* description = obj->FindString("description")) {
    // Non-serializable (function, DOM node, etc.). The description is CDP's
    // preview string ("function foo() { ... }", "HTMLDivElement", etc.) —
    // useful to the model as a "this was here but I can't see it" signal.
    out.Set("result", *description);
  } else {
    out.Set("result", std::string());
  }
  return out;
}

}  // namespace

void BrowserTool::RunEvaluateOnUI(::nexus::generated::BrowserEvaluateInput input,
                                  ToolCallback done) {
  // BrowserEvaluate is a first-class browser tool, always available like
  // the rest of the Browser* surface — no opt-in gate. (The former
  // `nexus.browser_tools_allow_evaluate` pref + Settings · Tools toggle +
  // `ShouldAllowAgentEvaluate` gate were removed.)
  if (input.code.empty()) {
    std::move(done).Reject("invalid_args", ReasonParams("code_required"));
    return;
  }

  // Resolve target tab. evaluate, like read_page/screenshot, always runs on
  // the active session-scoped tab — there's no per-call tab override.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "evaluate", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(*resolved_tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    {
      base::DictValue reject_params;
      reject_params.Set("tool", "evaluate");
      reject_params.Set("tab_id", *resolved_tab_id);
      std::move(done).Reject("tab_not_found", std::move(reject_params));
    }
    return;
  }

  // Hand off to the one-shot CDP session. The session attaches the
  // DevToolsAgentHost, sends Runtime.evaluate (awaitPromise=true,
  // returnByValue=true), and self-destructs on completion. Captured
  // `tab_id` and `done` are moved through to the reply closure.
  const int32_t tab_id = *resolved_tab_id;
  WatchJsDialogs(tab_id);
  if (RefuseWhileJsDialogOpen(tab_id, "evaluate", done)) {
    return;
  }
  done = TrackInFlight(std::move(done), "evaluate", tab_id);
  BrowserEvaluateSession::Start(
      contents, input.code,
      base::BindOnce(
          [](int32_t tab_id, ToolCallback inner_done, EvaluateOutcome outcome) {
            if (!outcome.error_code.empty()) {
              std::move(inner_done)
                  .Reject(std::move(outcome.error_code),
                          std::move(outcome.error_params));
              return;
            }
            base::DictValue observation =
                BuildEvaluateObservation(std::move(outcome.result));
            observation.Set("tab_id", tab_id);
            std::move(inner_done).Resolve(std::move(observation));
          },
          tab_id, std::move(done)));
}

void BrowserTool::RunBackOnUI(::nexus::generated::BrowserBackInput input,
                              ToolCallback done) {
  // Phase 13: schema dropped tab_id; always act on the active tab.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "back", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::HistoryBackAction* back = actions.add_actions()->mutable_back();
  back->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "back", std::move(done),
                  *resolved_tab_id);
}

// Phase 13: `RunForwardOnUI` retired alongside the `forward` tool. The
// forward-navigation use case was rare enough in agent workflows that
// keeping the surface area was net-negative; if a model goes back and
// then wants to "return", it can `BrowserNavigate(<original_url>)`.

void BrowserTool::RunCloseTabOnUI(::nexus::generated::BrowserCloseTabInput input,
                                  ToolCallback done) {
  // close_tab is one of two tools that KEEP `tab_id` (the other is
  // activate_tab). Both intentionally let the model target a non-active
  // tab — closing a background popup is the canonical use.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(input.tab_id, GetProfile(), "close_tab", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // The tab's document is going away with it: evict its registry entries
  // and its per-tab document record now (they are otherwise only pruned on
  // OnStop). Done before the action rather than in its callback so a
  // failed close costs at most one "stale index" re-observe for that tab.
  node_registry_.ClearTab(*resolved_tab_id);
  registry_document_by_tab_.erase(*resolved_tab_id);
  apc::Actions actions;
  apc::CloseTabAction* close_tab =
      actions.add_actions()->mutable_close_tab();
  close_tab->set_tab_id(*resolved_tab_id);
  // No working_tab_id — the targeted tab is being closed. Post-close
  // observation surfaces every surviving session-owned tab as basics
  // only. The next tab-less call falls back to LastWorkTab() if the
  // closed tab was the recorded one, or to first session-owned tab.
  RunActorAction(std::move(actions), "close_tab", std::move(done),
                  std::nullopt);
}

void BrowserTool::RunActivateTabOnUI(::nexus::generated::BrowserActivateTabInput input,
                                     ToolCallback done) {
  // ActivateTabInput.tab_id is required (bare int32_t, not optional);
  // the codegen-emitted Extract() rejects a missing key with
  // `invalid_args` before this body runs (see activate_tab.gen.h).
  //
  // Route through ResolveTabIdOrReject so the per-session check
  // (kSessionScopedTabs) gets applied uniformly — without it, the LLM
  // could activate a tab from another session simply by knowing its id.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(input.tab_id, GetProfile(), "activate_tab", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  // No stored "last active tab" — after this ActivateTabAction dispatches,
  // Chrome's `GetActiveTabHandleId(profile)` will return *resolved_tab_id
  // on the next call, which is what tab-less tool calls fall back to via
  // `GetSessionScopedActiveTabHandleId`.
  apc::Actions actions;
  apc::ActivateTabAction* activate_tab =
      actions.add_actions()->mutable_activate_tab();
  activate_tab->set_tab_id(*resolved_tab_id);
  RunActorAction(std::move(actions), "activate_tab", std::move(done),
                  *resolved_tab_id);
}

// Phase 13: `RunWaitOnUI` retired alongside the `wait` tool. The
// framework's actor pipeline already settles between actions; an
// explicit LLM-facing wait knob was used so rarely it was net-negative.
// If a specific page genuinely needs an extra settle, that's a bug in
// the actor pipeline to fix — not an LLM-facing parameter.

void BrowserTool::RunClickOnUI(::nexus::generated::BrowserClickInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id (always the index's home tab), button
  // (always LEFT), and click_count (always SINGLE). Right-click /
  // double/triple click were so rarely used by LLMs that the surface
  // was net-negative; dedicated tools can ship later if needed.
  if (JsDialogWatcher* watcher =
          IsJsDialogIndex(input.index) ? FindOpenJsDialog() : nullptr) {
    std::optional<bool> accept =
        JsDialogAcceptForIndex(*watcher->open_dialog(), input.index);
    if (accept.has_value()) {
      AnswerJsDialog(watcher, *accept, std::move(done));
    } else {
      RefuseWhileJsDialogOpen(watcher->tab_id(), "click", done);
    }
    return;
  }
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "click");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "click", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunInputOnUI(::nexus::generated::BrowserInputInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id. The index's home tab is the implicit
  // target — if it's not the active tab, BrowserActivateTab(N) first.
  if (input.index == kJsDialogTextIndex) {
    JsDialogWatcher* watcher = FindOpenJsDialog();
    if (watcher && watcher->open_dialog()->type == "prompt") {
      if (input.submit.value_or(false)) {
        watcher->open_dialog()->prompt_text = std::move(input.text);
        AnswerJsDialog(watcher, /*accept=*/true, std::move(done));
      } else {
        StageJsDialogText(std::move(input.text), std::move(done));
      }
      return;
    }
  }
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "input");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::TypeAction* type = actions.add_actions()->mutable_type();
  type->mutable_target()->set_content_node_id(entry->content_node_id);
  type->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  type->set_tab_id(entry->tab_id);
  type->set_text(std::move(input.text));
  // The actor's TypeAction currently only supports DELETE_EXISTING (the
  // text replaces existing content). The JS spec also documents this
  // as the contract for input.
  type->set_mode(apc::TypeAction::DELETE_EXISTING);
  type->set_follow_by_enter(input.submit.value_or(false));
  RunActorAction(std::move(actions), "input", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunSelectOnUI(::nexus::generated::BrowserSelectInput input,
                                ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "select");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::SelectAction* select = actions.add_actions()->mutable_select();
  select->mutable_target()->set_content_node_id(entry->content_node_id);
  select->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  select->set_tab_id(entry->tab_id);
  select->set_value(std::move(input.value));
  RunActorAction(std::move(actions), "select", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunHoverOnUI(::nexus::generated::BrowserHoverInput input,
                               ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "hover");
  if (!entry) {
    return;
  }
  apc::Actions actions;
  apc::MoveMouseAction* move = actions.add_actions()->mutable_move_mouse();
  move->mutable_target()->set_content_node_id(entry->content_node_id);
  move->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  move->set_tab_id(entry->tab_id);
  RunActorAction(std::move(actions), "hover", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunDragAndDropOnUI(::nexus::generated::BrowserDragAndDropInput input,
                                     ToolCallback done) {
  // Phase 13: dropped tab_id. Both indices must be on the same tab —
  // checked below — and that tab is always the registry's home tab
  // for from_index.
  const NodeRegistry::Entry* from_entry =
      LookupNode(input.from_index, std::nullopt, done, "drag_and_drop");
  if (!from_entry) {
    return;
  }
  const NodeRegistry::Entry* to_entry =
      LookupNode(input.to_index, std::nullopt, done, "drag_and_drop");
  if (!to_entry) {
    return;
  }
  if (from_entry->tab_id != to_entry->tab_id) {
    // The actor's DragAndReleaseAction takes a single tab_id and
    // operates within one renderer; allowing different home tabs for
    // the two endpoints would silently drop one of them. Reject up
    // front with a clear message instead.
    {
      base::DictValue reject_params = ReasonParams("different_tabs");
      reject_params.Set("from_index", input.from_index);
      reject_params.Set("from_tab", from_entry->tab_id);
      reject_params.Set("to_index", input.to_index);
      reject_params.Set("to_tab", to_entry->tab_id);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }
  apc::Actions actions;
  apc::DragAndReleaseAction* drag =
      actions.add_actions()->mutable_drag_and_release();
  drag->mutable_from_target()->set_content_node_id(from_entry->content_node_id);
  drag->mutable_from_target()
      ->mutable_document_identifier()
      ->set_serialized_token(from_entry->document_id);
  drag->mutable_to_target()->set_content_node_id(to_entry->content_node_id);
  drag->mutable_to_target()
      ->mutable_document_identifier()
      ->set_serialized_token(to_entry->document_id);
  drag->set_tab_id(from_entry->tab_id);
  RunActorAction(std::move(actions), "drag_and_drop", std::move(done),
                  from_entry->tab_id);
}

void BrowserTool::RunScrollToOnUI(::nexus::generated::BrowserScrollToInput input,
                                  ToolCallback done) {
  // Phase 13: dropped tab_id. Uses the registry's home tab for the index.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "scroll_to");
  if (!entry) {
    return;
  }

  apc::Actions actions;
  apc::ScrollToAction* scroll_to =
      actions.add_actions()->mutable_scroll_to();
  scroll_to->mutable_target()->set_content_node_id(entry->content_node_id);
  scroll_to->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  scroll_to->set_tab_id(entry->tab_id);
  RunActorAction(std::move(actions), "scroll_to", std::move(done),
                  entry->tab_id);
}

void BrowserTool::RunScrollOnUI(::nexus::generated::BrowserScrollInput input,
                                ToolCallback done) {
  // `amount` is in viewport units (default 1.0), converted to pixels with an
  // assumed viewport size.
  constexpr float kAssumedViewportHeightPx = 800.0f;
  constexpr float kAssumedViewportWidthPx = 1200.0f;
  const float amount = static_cast<float>(input.amount.value_or(1.0));

  int32_t resolved_tab_id = 0;
  const NodeRegistry::Entry* entry = nullptr;
  // Treat `index: 0` as "no element target" (viewport scroll). APC content-
  // node ids start above zero, so 0 is never a real element. Some LLMs
  // emit `0` instead of omitting the field for optional integers
  // (it's a JSON-encoder/sentinel default they fall back to), which used
  // to trap them in a retry loop: `LookupNode(0)` rejected with
  // `index_not_found`, the model re-sent the same shape, and the
  // conversation wedged because nothing else changed. Coerce here.
  const bool has_target_index =
      input.index.has_value() && *input.index != 0;
  if (has_target_index) {
    entry = LookupNode(*input.index, std::nullopt, done, "scroll");
    if (!entry) {
      return;
    }
    resolved_tab_id = entry->tab_id;
  } else {
    // Viewport scroll — the actor's `BuildToolRequest` requires a
    // tab_id on every ScrollAction even with no node target. Always
    // resolve to the active tab.
    std::optional<int32_t> resolved =
        ResolveTabIdOrReject(std::nullopt, GetProfile(), "scroll", done, this);
    if (!resolved.has_value()) {
      return;
    }
    resolved_tab_id = *resolved;
  }

  apc::Actions actions;
  apc::ScrollAction* scroll = actions.add_actions()->mutable_scroll();
  if (entry != nullptr) {
    scroll->mutable_target()->set_content_node_id(entry->content_node_id);
    scroll->mutable_target()
        ->mutable_document_identifier()
        ->set_serialized_token(entry->document_id);
  }
  scroll->set_tab_id(resolved_tab_id);

  // Map the LLM-facing direction string to the actor's enum. Reject
  // values that aren't in the .tool.json's enumerated set (the
  // codegen extractor doesn't enforce string enums today, so this
  // body is the live check).
  float pixel_distance = 0.0f;
  if (input.direction == "up") {
    scroll->set_direction(apc::ScrollAction::UP);
    pixel_distance = amount * kAssumedViewportHeightPx;
  } else if (input.direction == "down") {
    scroll->set_direction(apc::ScrollAction::DOWN);
    pixel_distance = amount * kAssumedViewportHeightPx;
  } else if (input.direction == "left") {
    scroll->set_direction(apc::ScrollAction::LEFT);
    pixel_distance = amount * kAssumedViewportWidthPx;
  } else if (input.direction == "right") {
    scroll->set_direction(apc::ScrollAction::RIGHT);
    pixel_distance = amount * kAssumedViewportWidthPx;
  } else {
    {
      base::DictValue reject_params = ReasonParams("scroll_direction");
      reject_params.Set("value", input.direction);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }

  scroll->set_distance(pixel_distance);
  RunActorAction(std::move(actions), "scroll", std::move(done),
                  resolved_tab_id);
}

void BrowserTool::RunSearchOnUI(::nexus::generated::BrowserSearchInput input,
                                ToolCallback done) {
  // No `input.query.empty()` guard — the codegen-emitted Extract()
  // already rejects an empty required string with `invalid_args`
  // before this body runs (see search.gen.h).

  // Engine → results-page URL template. Each template uses `q` as the
  // query placeholder so `base::ReplaceFirstSubstringAfterOffset` keeps
  // the wiring uniform. The set is intentionally small — the LLM can
  // always fall back to `navigate` for arbitrary URLs.
  std::string engine_id = input.engine.value_or("google");
  // Lowercase ASCII fold so "Baidu" / "BAIDU" / "baidu" all match.
  std::string engine_lower;
  engine_lower.reserve(engine_id.size());
  for (char c : engine_id) {
    engine_lower.push_back((c >= 'A' && c <= 'Z') ? (c | 0x20) : c);
  }
  std::string url;
  const std::string encoded =
      base::EscapeQueryParamValue(input.query, /*use_plus=*/true);
  if (engine_lower == "baidu") {
    url = base::StrCat({"https://www.baidu.com/s?wd=", encoded});
  } else if (engine_lower == "bing") {
    url = base::StrCat({"https://www.bing.com/search?q=", encoded});
  } else if (engine_lower == "duckduckgo" || engine_lower == "ddg") {
    url = base::StrCat({"https://duckduckgo.com/?q=", encoded});
  } else {
    // google (default) and any unrecognized engine fall through here so
    // a typo in the engine name still lands the user on a results page
    // instead of failing the call.
    url = base::StrCat({"https://www.google.com/search?q=", encoded});
  }
  // Search always opens results in a new tab; action_name "search" keeps the
  // result's `action` the user-facing tool name.
  RunCreateTabAction(std::move(url),
                     /*action_name=*/"search",
                     std::move(done));
}

// Phase 13: `RunCreateTabOnUI` retired alongside the `create_tab` tool.
// The capability lives on inside `RunNavigateOnUI` (new_tab=true path),
// which routes through `RunCreateTabAction` — the same helper this
// retired override used to call.

void BrowserTool::RunDoneOnUI(::nexus::generated::BrowserDoneInput input,
                              ToolCallback done) {
  // `done` resolves the per-call envelope with `is_done: true`; the
  // JS-side agent loop reads that flag to terminate.
  base::DictValue result;
  result.Set("ok", true);
  result.Set("is_done", true);
  result.Set("success", input.success);
  result.Set("text", std::move(input.text));
  // ToolCallback has overloads for DictValue / ListValue / Value, so
  // we pass the DictValue directly without wrapping in a base::Value.
  std::move(done).Resolve(std::move(result));
}

// Phase 13: `media_play` / `media_pause` / `media_seek` collapsed into
// a single `media_control` tool with an `action` discriminator. Three
// tools became one; one extra field traded for two fewer tools is a
// catalog-complexity win.
void BrowserTool::RunMediaControlOnUI(
    ::nexus::generated::BrowserMediaControlInput input, ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "media_control", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::MediaControlAction* media =
      actions.add_actions()->mutable_media_control();
  media->set_tab_id(*resolved_tab_id);

  // The codegen extractor doesn't enforce string enums today, so the
  // body is the live check. `action` is a required field so the
  // extractor already rejects a missing key.
  if (input.action == "play") {
    media->mutable_play();
  } else if (input.action == "pause") {
    media->mutable_pause();
  } else if (input.action == "seek") {
    if (!input.seek_time_ms.has_value()) {
      std::move(done).Reject("invalid_args", ReasonParams("seek_time_required"));
      return;
    }
    // Negative seek positions are clamped to 0 — the proto doc says
    // "should be non-negative" and the renderer rejects negatives, so
    // catching it here gives a cleaner result than a generic actor
    // error.
    const int32_t ms = *input.seek_time_ms;
    media->mutable_seek()->set_seek_time_milliseconds(
        static_cast<int64_t>(ms < 0 ? 0 : ms));
  } else {
    {
      base::DictValue reject_params = ReasonParams("media_action");
      reject_params.Set("value", input.action);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }
  RunActorAction(std::move(actions), "media_control", std::move(done),
                  *resolved_tab_id);
}

void BrowserTool::RunMediaStateOnUI(::nexus::generated::BrowserMediaStateInput input,
                                    ToolCallback done) {
  // Synchronous read: no actor task, no PerformActions round-trip,
  // no APC fetch. We just resolve the target WebContents and read
  // MediaSession's three sync accessors.
  //
  // Phase 13: dropped tab_id from the schema; always reads the active tab.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "media_state", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(*resolved_tab_id).Get();
  if (!tab || !tab->GetContents()) {
    {
      base::DictValue reject_params;
      reject_params.Set("tool", "media_state");
      reject_params.Set("tab_id", *resolved_tab_id);
      std::move(done).Reject("tab_not_found", std::move(reject_params));
    }
    return;
  }

  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "media_state");
  result.Set("tab_id", *resolved_tab_id);

  // `GetIfExists` returns null when no media has ever been associated
  // with the tab — common before the user presses play. We surface
  // this as `has_session:false` (NOT an error) so the LLM can branch
  // without hitting an exception path.
  content::MediaSession* session =
      content::MediaSession::GetIfExists(tab->GetContents());
  if (!session) {
    result.Set("has_session", false);
    std::move(done).Resolve(std::move(result));
    return;
  }
  result.Set("has_session", true);

  // Playback state (playing/paused). The mojom enum has only those
  // two values today; we stringify for LLM legibility.
  if (auto info = session->GetMediaSessionInfoSync()) {
    const char* state =
        info->playback_state ==
                media_session::mojom::MediaPlaybackState::kPlaying
            ? "playing"
            : "paused";
    result.Set("playback_state", state);
  }

  // Position / duration / rate. Pages that don't call
  // `navigator.mediaSession.setPositionState()` won't populate this —
  // we just omit the fields rather than emit fake zeros, so the LLM
  // can tell "I don't know the duration" from "duration is 0".
  if (auto position = session->GetMediaSessionPosition()) {
    result.Set(
        "position_ms",
        static_cast<int>(
            position->GetPosition().InMilliseconds()));
    result.Set(
        "duration_ms",
        static_cast<int>(position->duration().InMilliseconds()));
    result.Set("playback_rate", position->playback_rate());
    if (position->end_of_media()) {
      result.Set("end_of_media", true);
    }
  }

  // Page-supplied metadata. UTF-16 → UTF-8 because base::Value strings
  // are UTF-8. Empty fields are omitted so the LLM doesn't waste
  // attention on blanks; pages frequently set only a subset.
  const media_session::MediaMetadata& meta =
      session->GetMediaSessionMetadata();
  auto set_if_nonempty = [&](std::string_view key,
                             const std::u16string& value) {
    if (!value.empty()) {
      result.Set(key, base::UTF16ToUTF8(value));
    }
  };
  set_if_nonempty("title", meta.title);
  set_if_nonempty("artist", meta.artist);
  set_if_nonempty("album", meta.album);
  set_if_nonempty("source_title", meta.source_title);

  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunReadElementOnUI(::nexus::generated::BrowserReadElementInput input,
                                     ToolCallback done) {
  // Pure registry lookup. The node_registry already cached the
  // visible label / inner text from the most recent observation, so
  // there's no need to re-fetch the APC tree — the LLM uses this tool
  // for tiny snippets (button labels, list-item text) where the
  // registry's cached `text` is exactly what's wanted.
  //
  // Phase 13: dropped tab_id (use registry's home tab) and max_chars
  // (fixed server-side cap).
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "read_element");
  if (!entry) {
    return;  // LookupNode already rejected.
  }
  constexpr int kReadElementMaxChars = 2000;
  std::string text = entry->text;
  bool truncated = false;
  if (static_cast<int>(text.size()) > kReadElementMaxChars) {
    text.resize(static_cast<size_t>(kReadElementMaxChars));
    // Byte-level resize may bisect a multi-byte UTF-8 sequence on
    // pages with non-ASCII text — see EnsureUtf8TailComplete's docs.
    EnsureUtf8TailComplete(text);
    truncated = true;
  }
  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "read_element");
  result.Set("index", input.index);
  result.Set("type", entry->type);
  result.Set("element_text", std::move(text));
  result.Set("truncated", truncated);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::RunFindTextOnUI(::nexus::generated::BrowserFindTextInput input,
                                  ToolCallback done) {
  // No `input.query.empty()` guard — the codegen-emitted Extract()
  // already rejects an empty required string with `invalid_args`
  // before this body runs (see find_text.gen.h).
  //
  // Phase 13: dropped tab_id (always active tab) and max_results
  // (fixed server-side cap).
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "find_text", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  constexpr int kFindTextMaxResults = 5;
  RunTabObservation(
      *resolved_tab_id, "find_text", std::move(done),
      base::BindOnce(&BrowserTool::OnFindTextObservationBuilt,
                     weak_ptr_factory_.GetWeakPtr(),
                     std::move(input.query), kFindTextMaxResults,
                     /*include_observation=*/
                     input.include_observation.value_or(false)));
}

// =============================================================================
// Custom completions used by `read_page` + `find_text`. They walk the
// observation proto themselves to produce the LLM-facing payload
// instead of falling back to the generic OnActionsResultBuilt path.
// =============================================================================

void BrowserTool::OnReadPageObservationBuilt(
    std::string scope,
    int max_chars,
    int32_t requested_tab_id,
    bool include_observation,
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::optional<page_content_annotations::ScreenshotOptions::
                      ScreenshotCollectionOptions>
    /*screenshot_collection_options*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  // 149 folded the standalone result_code / index_of_failed_action into
  // the per-action `action_results` vector; reduce it via upstream's
  // canonical helper (first non-OK wins). Observation-only paths
  // (read_page / find_text) dispatch no actions, so this is kOk unless the
  // framework reports an observation-time failure.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  // Node registry accumulates across observations — see comment in
  // OnActionsResultBuilt. BuildObservationDict below registers the
  // new observation's nodes; prior nodes that didn't reappear remain
  // in the registry and surface a clean stale error if used.

  base::DictValue result;
  result.Set("action", action_name);
  result.Set("scope", scope);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("error_code", static_cast<int>(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }
  // BuildObservationDict always runs (registry + agent_meta), but elements[] are
  // stripped to basics by default — read_page answers with page_text — unless
  // `include_observation:true` (then always full). A stripped result discards
  // the staged baseline.
  base::DictValue observation = BuildObservationDict(
      *result_proto, /*include_screenshot_image=*/false,
      /*force_full=*/include_observation);
  if (!observation.empty()) {
    if (include_observation) {
      observation_baseline_.Commit();
    } else {
      observation_baseline_.Discard();
      browser_tool_internal::StripObservationToBasics(observation);
    }
    result.Set("observation", std::move(observation));
  } else {
    observation_baseline_.Discard();
  }

  // Find the requested tab within the observation. The actor framework
  // can return multiple `TabObservation`s in `result_proto->tabs()`
  // (e.g. when it batches observations across recently-acted tabs),
  // and historically we picked the first one — which silently returned
  // the wrong tab's content when navigation had moved focus elsewhere.
  // Match against `requested_tab_id` first; fall back to the first tab
  // with content if the requested tab somehow isn't in the observation.
  const apc::TabObservation* match = nullptr;
  const apc::TabObservation* first_with_content = nullptr;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    if (!tab_obs.annotated_page_content().has_root_node()) {
      continue;
    }
    if (!first_with_content) {
      first_with_content = &tab_obs;
    }
    if (tab_obs.id() == requested_tab_id) {
      match = &tab_obs;
      break;
    }
  }

  std::string page_text;
  std::optional<int> picked_tab_id;
  const apc::TabObservation* selected = match ? match : first_with_content;
  if (selected) {
    const apc::AnnotatedPageContent& apc_data =
        selected->annotated_page_content();
    AppendVisibleText(apc_data.root_node(), max_chars, page_text);
    picked_tab_id = selected->id();
    if (apc_data.has_main_frame_data()) {
      if (!apc_data.main_frame_data().url().empty()) {
        result.Set("url", apc_data.main_frame_data().url());
      }
      if (!apc_data.main_frame_data().title().empty()) {
        result.Set("title", apc_data.main_frame_data().title());
      }
    }
    if (!match && first_with_content) {
      // Defensive: requested tab wasn't in the observation set.
      // Surface a warning so the model knows the content came from a
      // different tab and can react (e.g. activate the right tab)
      // instead of silently consuming the wrong page.
      result.Set(
          "tab_mismatch_warning",
          base::StrCat(
              {"requested tab ", base::NumberToString(requested_tab_id),
               " was not in the observation; returning tab ",
               base::NumberToString(picked_tab_id.value_or(-1)),
               " content instead. Consider calling activate_tab first."}));
    }
  }
  if (picked_tab_id.has_value()) {
    result.Set("tab_id", *picked_tab_id);
  }
  result.Set("page_text", page_text);
  result.Set("truncated", static_cast<int>(page_text.size()) >= max_chars);
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

void BrowserTool::OnFindTextObservationBuilt(
    std::string query,
    int max_results,
    bool include_observation,
    ToolCallback done,
    std::string action_name,
    base::TimeTicks /*start_time*/,
    std::vector<actor::ActionResultWithLatencyInfo> action_results,
    actor::TaskId /*task_id*/,
    bool /*skip_async_observation_information*/,
    std::optional<page_content_annotations::ScreenshotOptions::
                      ScreenshotCollectionOptions>
    /*screenshot_collection_options*/,
    std::unique_ptr<apc::ActionsResult> result_proto,
    std::unique_ptr<actor::AggregatedJournal::PendingAsyncEntry> /*journal*/) {
  // 149 folded the standalone result_code / index_of_failed_action into
  // the per-action `action_results` vector; reduce it via upstream's
  // canonical helper (first non-OK wins). Observation-only paths
  // (read_page / find_text) dispatch no actions, so this is kOk unless the
  // framework reports an observation-time failure.
  const actor::mojom::ActionResultCode result_code =
      OverallActorResultCode(action_results);
  base::DictValue result;
  result.Set("action", action_name);
  result.Set("query", query);
  if (!actor::IsOk(result_code)) {
    result.Set("ok", false);
    result.Set("error_code", static_cast<int>(result_code));
    std::move(done).Resolve(std::move(result));
    return;
  }
  if (!result_proto) {
    result.Set("ok", false);
    result.Set("result", "no_observation");
    std::move(done).Resolve(std::move(result));
    return;
  }

  // Refresh the observation (also updates the node registry and
  // last_observation_* fields used by ToolSessionRegistry for agent_meta).
  // T0-b: default to basics-only — find_text's answer is its matches[]
  // (each already carries `clickable_ancestor_index`, enough to click
  // the result directly). `include_observation:true` returns the full
  // elements[] for a search-then-act-elsewhere flow — as an array, never
  // a T0-c stub (`force_full`). Baseline commit/discard mirrors read_page:
  // only shipped elements move the baseline.
  base::DictValue observation = BuildObservationDict(
      *result_proto, /*include_screenshot_image=*/false,
      /*force_full=*/include_observation);
  if (!observation.empty()) {
    if (include_observation) {
      observation_baseline_.Commit();
    } else {
      observation_baseline_.Discard();
      browser_tool_internal::StripObservationToBasics(observation);
    }
    result.Set("observation", std::move(observation));
  } else {
    observation_baseline_.Discard();
  }

  // Find walk. Limited to the first APC tree (the active observed
  // tab) since `find_text` is one-tab.
  FindTextWalker walker;
  walker.query_lower = ToLowerAscii(query);
  walker.max_results = max_results;
  for (const apc::TabObservation& tab_obs : result_proto->tabs()) {
    if (!tab_obs.has_annotated_page_content()) {
      continue;
    }
    const apc::AnnotatedPageContent& apc_data = tab_obs.annotated_page_content();
    if (apc_data.has_root_node()) {
      walker.registry = &node_registry_;
      walker.tab_id =
          apc_data.has_tab_id() ? apc_data.tab_id() : tab_obs.id();
      if (apc_data.has_main_frame_data() &&
          apc_data.main_frame_data().has_document_identifier()) {
        walker.main_document_id =
            apc_data.main_frame_data().document_identifier().serialized_token();
      }
      walker.document_id = walker.main_document_id;
      FindTextWalk(apc_data.root_node(), walker);
    }
    break;
  }
  result.Set("matches", std::move(walker.matches));
  result.Set("ok", true);
  std::move(done).Resolve(std::move(result));
}

// =============================================================================
// BrowserFillForm — batch form fill. Each `fields` entry is parsed into a
// `FillFormFieldOp` and becomes one action (type / select / check / uncheck /
// click; check and uncheck always click), dispatched in one PerformActions,
// plus a final click when `submit_index` is set. Returns the standard
// observation envelope.
// =============================================================================

namespace {

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
