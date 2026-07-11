#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_

#include <map>
#include <optional>
#include <string>
#include <vector>

#include "base/values.h"
#include "custom_browser/browser/nexus/mojom/nexus_llm.mojom.h"

namespace nexus {

enum class NexusLlmBridgeDeltaBlockType {
  kText,
  kThinking,
};

struct NexusLlmBridgeOpenDeltaBlock {
  NexusLlmBridgeDeltaBlockType type = NexusLlmBridgeDeltaBlockType::kText;
  int index = 0;
};

struct NexusLlmBridgePendingToolCall {
  NexusLlmBridgePendingToolCall();
  NexusLlmBridgePendingToolCall(const NexusLlmBridgePendingToolCall&);
  NexusLlmBridgePendingToolCall(NexusLlmBridgePendingToolCall&&);
  NexusLlmBridgePendingToolCall& operator=(
      const NexusLlmBridgePendingToolCall&);
  NexusLlmBridgePendingToolCall& operator=(NexusLlmBridgePendingToolCall&&);
  ~NexusLlmBridgePendingToolCall();

  std::string tool_use_id;
  std::optional<std::string> name;
  std::string args_text;
};

struct NexusLlmBridgeStreamState {
  NexusLlmBridgeStreamState();
  NexusLlmBridgeStreamState(const NexusLlmBridgeStreamState&);
  NexusLlmBridgeStreamState(NexusLlmBridgeStreamState&&);
  NexusLlmBridgeStreamState& operator=(const NexusLlmBridgeStreamState&);
  NexusLlmBridgeStreamState& operator=(NexusLlmBridgeStreamState&&);
  ~NexusLlmBridgeStreamState();

  int next_content_block_index = 0;
  std::optional<NexusLlmBridgeOpenDeltaBlock> open_delta_block;
  std::map<std::string, NexusLlmBridgePendingToolCall> pending_tool_calls;
  std::vector<std::string> pending_tool_call_order;
  bool terminal_emitted = false;
};

mojom::NexusLlmRequestParamsPtr BuildMojoLlmRequest(
    const base::DictValue& payload,
    std::string* error_message);

base::DictValue CatalogToBridge(const mojom::NexusLlmCatalog& catalog);
base::DictValue CallResultToBridge(const mojom::NexusLlmCallResult& result);
base::DictValue StreamReadResultToBridge(
    NexusLlmBridgeStreamState* state,
    const mojom::NexusLlmStreamReadResult& result,
    bool* done_out,
    bool* should_cancel_out);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_NEXUS_LLM_BRIDGE_SUPPORT_H_
