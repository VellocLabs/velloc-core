#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

#include <memory>
#include <utility>

#include "base/check.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "custom_browser/browser/nexus/api/browser/browser_api.gen.h"
#include "custom_browser/browser/nexus/api/exec/exec_api.gen.h"
#include "custom_browser/browser/nexus/api/files/files_api.gen.h"
#include "custom_browser/browser/nexus/nexus_bridge_capability_handler.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_signature.h"

namespace nexus {

namespace {

// The runtime renders an error's text from its code, so a code the family's
// `.api.json` files do not declare reaches the model as a bare code.
// `tool_dropped` comes from ToolCallback itself and is valid on every family.
bool IsDeclaredToolErrorCode(std::string_view capability,
                             std::string_view code) {
  if (code == "tool_dropped") {
    return true;
  }
  if (capability == "files") {
    return generated::IsFilesErrorCode(code);
  }
  if (capability == "exec") {
    return generated::IsExecErrorCode(code);
  }
  if (capability == "browser") {
    return generated::IsBrowserErrorCode(code);
  }
  return false;
}

base::DictValue BuildRoute(std::string_view capability,
                           std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", std::string(capability));
  route.Set("action", std::string(action));
  return route;
}

// `done` can be answered by exactly one of the ToolCallback's two halves;
// both closures share it through this holder and the one that fires takes it.
struct DoneHolder {
  explicit DoneHolder(BridgeResponseCallback done) : done(std::move(done)) {}
  BridgeResponseCallback done;
};

}  // namespace

ToolSessionRegistry::ToolSessionRegistry(Profile* profile)
    : profile_(profile) {}

ToolSessionRegistry::~ToolSessionRegistry() {
  // The connection is going away (webview reload, panel close): give every
  // live session a final chance to stop its actor tasks before the map is
  // cleared and each ToolSession (and its tagged tabs) is destroyed.
  for (auto& [key, session] : sessions_) {
    if (session) {
      session->OnStop(OnStopReason::kDisconnected);
    }
  }
}

ToolSession& ToolSessionRegistry::GetOrCreateSession(
    const std::string& session_key) {
  auto it = sessions_.find(session_key);
  if (it != sessions_.end()) {
    return *it->second;
  }
  auto session = std::make_unique<ToolSession>(profile_, session_key);
  ToolSession* raw = session.get();
  sessions_.emplace(session_key, std::move(session));
  return *raw;
}

bool ToolSessionRegistry::HasSession(const std::string& session_key) const {
  return sessions_.find(session_key) != sessions_.end();
}

void ToolSessionRegistry::OnSessionStopped(std::string session_key,
                                           OnStopReason reason) {
  // `session_key` is taken by value: erasing the entry below destroys the
  // ToolSession that owns the string a by-reference caller might have
  // passed in.
  auto it = sessions_.find(session_key);
  if (it == sessions_.end()) {
    return;
  }
  if (it->second) {
    it->second->OnStop(reason);
  }
  if (reason == OnStopReason::kSessionDeleted) {
    sessions_.erase(it);
  }
}

void ToolSessionRegistry::Run(NexusBridgeCapabilityHandler& handler,
                              std::string_view capability,
                              std::string_view action,
                              std::string tool_name,
                              BridgeResponseCallback done,
                              ToolInvoke invoke) {
  const NexusBridgeCapabilityHandler::RequestScope* request =
      handler.current_request();
  base::DictValue args;
  if (request && request->payload) {
    args = request->payload->Clone();
  }
  const std::string session_key = handler.RequestSessionKey();
  ToolSession& session = GetOrCreateSession(session_key);

  const std::string request_id = done.id();
  session.set_progress_sink(base::BindRepeating(
      [](base::WeakPtr<NexusBridgeCapabilityHandler> handler,
         std::string request_id, std::string capability, std::string action,
         base::Value progress) {
        if (!handler) {
          return;
        }
        if (progress.is_dict()) {
          progress.GetDict().Set("request_id", request_id);
        } else {
          base::DictValue wrapped;
          wrapped.Set("request_id", request_id);
          wrapped.Set("value", std::move(progress));
          progress = base::Value(std::move(wrapped));
        }
        handler->SendEvent(BuildRoute(capability, action), std::move(progress));
      },
      handler.AsWeakPtr(), request_id, std::string(capability),
      std::string(action)));

  auto holder = std::make_shared<DoneHolder>(std::move(done));

  auto on_success = base::BindOnce(
      [](base::WeakPtr<ToolSessionRegistry> self, std::string tool_name,
         base::DictValue args, std::string session_key,
         std::shared_ptr<DoneHolder> holder, base::Value result) {
        AgentToolMeta meta;
        meta.action_hashes = ComputeActionHashes(tool_name, args);
        meta.terminates_sequence = IsSequenceTerminator(tool_name);
        if (self) {
          auto it = self->sessions_.find(session_key);
          if (it != self->sessions_.end() && it->second) {
            const BrowserTool& bt = it->second->browser_tool();
            if (!bt.last_observation_url().empty()) {
              meta.page_url = bt.last_observation_url();
              meta.page_interactive_count =
                  bt.last_observation_interactive_count();
              meta.page_dom_hash = ComputeDomHash(
                  bt.last_observation_normalized_projection());
              meta.post_url = bt.last_observation_url();
            }
            if (bt.has_last_observation_focus_target()) {
              meta.post_focus_target_present = true;
              meta.post_focus_target_id = bt.last_observation_focus_target_id();
            }
            it->second->ClearProgressSink();
          }
        }
        std::move(holder->done)
            .Success(WrapResultWithAgentMeta(std::move(result), meta));
      },
      weak_ptr_factory_.GetWeakPtr(), std::move(tool_name), std::move(args),
      session_key, holder);

  auto on_error = base::BindOnce(
      [](base::WeakPtr<ToolSessionRegistry> self, std::string capability,
         std::string session_key, std::shared_ptr<DoneHolder> holder,
         std::string code, std::string message, base::DictValue params) {
        DCHECK(IsDeclaredToolErrorCode(capability, code))
            << capability << " rejected with undeclared error code '" << code
            << "'; add it to the action's .api.json errors";
        if (self) {
          auto it = self->sessions_.find(session_key);
          if (it != self->sessions_.end() && it->second) {
            it->second->ClearProgressSink();
          }
        }
        std::move(holder->done)
            .Error(std::move(code), std::move(message), std::move(params));
      },
      weak_ptr_factory_.GetWeakPtr(), std::string(capability), session_key,
      holder);

  std::move(invoke).Run(
      session, ToolCallback(std::move(on_success), std::move(on_error)));
}

}  // namespace nexus
