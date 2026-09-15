// Copyright 2026 The Custom Browser Authors. All rights reserved.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USER_CONTENT_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USER_CONTENT_H_

#include <optional>
#include <string>

#include "base/values.h"

namespace nexus {

// The URL an OpenAI-style image part carries for a bridge `image` part: its
// `url` as is, an `image` that is already a URL or `data:` URI as is, or the
// raw base64 in `image` / `data` wrapped as `data:<mimeType>;base64,...`
// (image/png when the part names no type). OpenAI-compatible backends (e.g.
// Kimi) reject a bare base64 payload. nullopt when the part has no payload.
std::optional<std::string> ImageUrlForPart(const base::DictValue& part);

// The `content` of a Responses-API `{type:"message", role:"user"}` item for
// the bridge parts of one user message.
//
// A text-only message stays one string, joined by '\n' — the shape every
// earlier request had. Anything else becomes a part list:
//   text     → {type:"input_text", text}
//   image    → {type:"input_image", image_url}
//   document → {type:"input_file", filename, file_data:"data:<mime>;base64,..."}
//              when `accepts_input_file`, else an `input_text` carrying
//              DocumentUnavailableMarker.
// Flattening to a string dropped the image and document parts, so a pasted
// screenshot or an @-mentioned PDF never reached the model. Unknown part
// types are skipped, as the chat-completions path does.
//
// `accepts_input_file` is false for the ChatGPT (Codex) backend: its input
// vocabulary has input_text / input_image only, and a part it rejects would
// stay in the transcript and fail every later turn.
base::Value BuildResponsesUserContent(const base::ListValue& parts,
                                      bool accepts_input_file);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_LLM_PROVIDERS_OPENAI_OPENAI_USER_CONTENT_H_
