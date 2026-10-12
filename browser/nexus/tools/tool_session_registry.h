#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_REGISTRY_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_REGISTRY_H_

#include <map>
#include <memory>
#include <string>
#include <string_view>

#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"

class Profile;

namespace nexus {

class NexusBridgeCapabilityHandler;

// The per-connection table of host tool sessions, keyed by the runtime's
// session id and erased only when the runtime deletes the session. `Run` is
// the one path from a host tool call to a bridge response: resolve the
// session, install the progress sink, wrap in agent-meta, answer.
class ToolSessionRegistry {
 public:
  // Invoked once the session is resolved; runs the tool and answers through
  // the ToolCallback.
  using ToolInvoke = base::OnceCallback<void(ToolSession&, ToolCallback)>;

  explicit ToolSessionRegistry(Profile* profile);
  ~ToolSessionRegistry();

  ToolSessionRegistry(const ToolSessionRegistry&) = delete;
  ToolSessionRegistry& operator=(const ToolSessionRegistry&) = delete;

  ToolSession& GetOrCreateSession(const std::string& session_key);
  bool HasSession(const std::string& session_key) const;
  size_t session_count() const { return sessions_.size(); }

  // Stops the session's tools. The session is erased only for
  // kSessionDeleted; every other reason keeps it warm for the next turn.
  void OnSessionStopped(std::string session_key, OnStopReason reason);

  // Runs one host tool call for the frame `handler` is currently serving
  // (NexusBridgeCapabilityHandler::current_request()). `capability` /
  // `action` name the route progress events go out on; `tool_name` is the
  // model-facing tool name the agent-meta envelope is computed for.
  void Run(NexusBridgeCapabilityHandler& handler,
           std::string_view capability,
           std::string_view action,
           std::string tool_name,
           BridgeResponseCallback done,
           ToolInvoke invoke);

 private:
  raw_ptr<Profile> profile_;
  std::map<std::string, std::unique_ptr<ToolSession>> sessions_;
  base::WeakPtrFactory<ToolSessionRegistry> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_TOOL_SESSION_REGISTRY_H_
