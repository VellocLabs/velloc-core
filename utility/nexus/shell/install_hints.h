// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_

#include <string>
#include <string_view>

namespace nexus {

// A terse, model-facing install instruction for `tool_id` on `platform`, one
// line per package manager, most likely first; empty when none is recorded
// (callers still report "binary not found").
std::string InstallHintFor(std::string_view tool_id,
                           std::string_view platform);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_
