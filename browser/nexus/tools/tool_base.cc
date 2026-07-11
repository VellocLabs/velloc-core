#include "custom_browser/browser/nexus/tools/tool_base.h"

namespace nexus {

ToolBase::ToolBase(ToolSession* session, Profile* profile,
                   const std::string& session_key)
    : session_(session), profile_(profile), session_key_(session_key) {}

ToolBase::~ToolBase() = default;

void ToolBase::OnStop(const std::string& /*reason*/) {
  // Default no-op. Subclasses that own per-session state override this
  // (e.g. BrowserTool::OnStop stops its actor::TaskId).
}

}  // namespace nexus
