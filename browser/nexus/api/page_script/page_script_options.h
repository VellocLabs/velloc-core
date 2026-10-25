// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_PAGE_SCRIPT_OPTIONS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_PAGE_SCRIPT_OPTIONS_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace nexus {

// Pure input checks and constants for the `pageScript` capability (F-29); no
// browser state.

// The largest message either direction carries, in bytes of JSON text.
inline constexpr size_t kPageScriptMaxPayloadBytes = 1024 * 1024;

// The private world the channel's scripts run in, and the two host-call
// names the bootstrap looks for: one bound into that world, one bound into
// the page's own world only when the opener asked for it.
inline constexpr char kPageScriptWorldName[] = "velloc-page-script";
inline constexpr char kPageScriptBindingName[] = "__vellocHostSend";
inline constexpr char kPageScriptPageBindingName[] = "__vellocHostSendPage";
// The function the bootstrap leaves for the host's postMessage to call.
inline constexpr char kPageScriptDispatchName[] = "__vellocDispatch";

enum class PageScriptWorld { kIsolated, kMain };

// 'isolated' (or absent) | 'main' → the world; anything else is nullopt.
std::optional<PageScriptWorld> ParsePageScriptWorld(
    const std::optional<std::string>& world);

// The wire name of a world.
std::string_view PageScriptWorldName(PageScriptWorld world);

// What `attach` was asked to reach: each field is optional on the wire.
struct PageScriptTargetRequest {
  std::optional<int32_t> web_window_id;
  std::optional<int32_t> tab_id;
};

// Returns the error reason (for `invalid_args`) unless exactly one target
// field is set.
std::optional<std::string> ValidatePageScriptTarget(
    const PageScriptTargetRequest& request);

bool IsPageScriptPayloadWithinLimit(std::string_view payload);

// The source the channel runs at the start of every document: it defines
// `vellocHost` (send / onMessage) over whichever host-call name the world
// carries, and the dispatcher the host's postMessage calls.
std::string PageScriptBootstrapSource();

// The expression that hands `json` (JSON text) to the dispatcher. `json` is
// embedded as a string literal, so its content cannot be read as code.
std::string PageScriptDispatchExpression(std::string_view json);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_PAGE_SCRIPT_PAGE_SCRIPT_OPTIONS_H_
