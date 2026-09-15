// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_RESULT_MESSAGES_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_RESULT_MESSAGES_H_

#include <string>

#include "base/values.h"

namespace nexus {

// Serializes the `content` parts of one bridge `role:"tool"` message into
// OpenAI-compatible wire entries. The guest merges a round's parallel tool
// results into ONE tool message with N `tool-result` parts, so both helpers
// emit one wire entry PER PART — a single shared entry keeps only the last
// result, and the server then sees unanswered tool_calls.
//
// Tool outputs are text-only on the wire. Images in a result (e.g.
// BrowserScreenshot) are delivered as ONE follow-up `user` message appended
// AFTER every tool entry of the batch, so the sequence stays
// `assistant(tool_calls) → tool… → user`: a user message between two tool
// messages breaks "tool messages must directly follow the tool_calls".
// That message groups the images under `Tool call <id>:` labels and keeps the
// short text parts around each image (`PDF file read: ...`, `<page ...>`):
// with bare images, parallel reads of several PDFs were attributed to the
// wrong files. The tool messages' own text is unchanged.

// /chat/completions (OpenAI-compatible providers and Ollama): appends
// `{role:"tool", tool_call_id, content}` per part, then the image message
// with `image_url` parts.
void AppendChatCompletionToolMessages(const base::ListValue& content_parts,
                                      base::ListValue& messages);

// /responses: appends `{type:"function_call_output", call_id, output}` per
// part, then a `{type:"message", role:"user"}` item with `input_image` parts.
void AppendResponsesToolOutputItems(const base::ListValue& content_parts,
                                    base::ListValue& input);

// The text that stands in for a `document` part (a PDF) wherever it occurs —
// tool result or user message (an @-mentioned PDF inlined by the guest).
// These transports have no document part; dropping it silently left the
// model answering from a file it never saw, so the marker says so and points
// at the page-image Read that does reach it. `mime_type` may be null.
std::string DocumentUnavailableMarker(const std::string* mime_type);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_TOOL_RESULT_MESSAGES_H_
