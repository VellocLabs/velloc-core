// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_

#include <string>
#include <string_view>

namespace nexus {

// Returns a multi-line, model-facing instruction string describing how the
// user can install `tool_id` on `platform` ("windows" | "macos" | "linux").
// Returns the empty string when no hint is recorded for that combination —
// callers should still surface a generic "binary not found" message in that
// case so the model is not silently misled into thinking the tool exists.
//
// The returned text is intentionally terse and command-oriented (one
// shell-line per package manager) so the model can copy/paste it into the
// Bash tool without further reformatting. Hints are deliberately ordered
// from "most likely to be present" to "least likely" — the model picks the
// first one whose package manager exists on PATH.
std::string InstallHintFor(std::string_view tool_id,
                           std::string_view platform);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SHELL_INSTALL_HINTS_H_
