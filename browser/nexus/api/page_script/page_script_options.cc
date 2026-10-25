// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/page_script/page_script_options.h"

#include "base/json/string_escape.h"
#include "base/strings/strcat.h"

namespace nexus {

std::optional<PageScriptWorld> ParsePageScriptWorld(
    const std::optional<std::string>& world) {
  if (!world.has_value() || *world == "isolated") {
    return PageScriptWorld::kIsolated;
  }
  if (*world == "main") {
    return PageScriptWorld::kMain;
  }
  return std::nullopt;
}

std::string_view PageScriptWorldName(PageScriptWorld world) {
  return world == PageScriptWorld::kMain ? "main" : "isolated";
}

std::optional<std::string> ValidatePageScriptTarget(
    const PageScriptTargetRequest& request) {
  const int set = (request.web_window_id.has_value() ? 1 : 0) +
                  (request.tab_id.has_value() ? 1 : 0);
  if (set == 0) {
    return "one of webWindowId or tabId is required";
  }
  if (set > 1) {
    return "webWindowId and tabId are exclusive";
  }
  return std::nullopt;
}

bool IsPageScriptPayloadWithinLimit(std::string_view payload) {
  return payload.size() <= kPageScriptMaxPayloadBytes;
}

std::string PageScriptBootstrapSource() {
  // Contract: idempotent per world (a second run is a no-op), and the
  // objects it defines cannot be replaced by later scripts of that world.
  // Security: defines nothing in a subframe, so only the top frame's
  // scripts hold a way to the host.
  return base::StrCat({
      "(() => {\n"
      "  const g = globalThis;\n"
      "  if (g !== g.top || g.vellocHost) return;\n"
      "  const call = g.",
      kPageScriptBindingName, " || g.", kPageScriptPageBindingName,
      ";\n"
      "  const listeners = new Set();\n"
      "  const host = Object.freeze({\n"
      "    send(data) {\n"
      "      if (typeof call !== 'function') {\n"
      "        throw new Error('vellocHost is not connected');\n"
      "      }\n"
      "      call(JSON.stringify(data === undefined ? null : data));\n"
      "    },\n"
      "    onMessage(listener) {\n"
      "      listeners.add(listener);\n"
      "      return () => listeners.delete(listener);\n"
      "    },\n"
      "  });\n"
      "  Object.defineProperty(g, 'vellocHost', {value: host});\n"
      "  Object.defineProperty(g, '",
      kPageScriptDispatchName,
      "', {\n"
      "    value(json) {\n"
      "      const data = JSON.parse(json);\n"
      "      for (const listener of [...listeners]) {\n"
      "        try { listener(data); } catch (e) { console.error(e); }\n"
      "      }\n"
      "      return listeners.size;\n"
      "    },\n"
      "  });\n"
      "})();\n"});
}

std::string PageScriptDispatchExpression(std::string_view json) {
  std::string literal;
  base::EscapeJSONString(json, /*put_in_quotes=*/true, &literal);
  return base::StrCat({"typeof ", kPageScriptDispatchName,
                       " === 'function' ? ", kPageScriptDispatchName, "(",
                       literal, ") : -1"});
}

}  // namespace nexus
