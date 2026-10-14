// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Pure command-line classification for the Nexus window routing policy
// (custom_browser_window_util.cc). Header-only and //base-only so it can be
// unit-tested without standing up a BrowserView (the window_hit_test.h
// pattern).
#ifndef CUSTOM_BROWSER_UI_VIEW_RELAUNCH_COMMAND_LINE_H_
#define CUSTOM_BROWSER_UI_VIEW_RELAUNCH_COMMAND_LINE_H_

#include <string_view>

#include "base/command_line.h"
#include "base/containers/span.h"
#include "build/build_config.h"

namespace custom_browser::window_util {

// True when |command_line| carries URL/file arguments, i.e. the launch exists
// to open something (an OS default-browser open, a link click while the
// process is running). The bare `about:blank` placeholder is exempt: the e2e
// harness appends it to every launch to keep the NTP from spawning, and it
// must not count as an external open.
inline bool HasExternalUrlArgs(const base::CommandLine& command_line) {
  constexpr base::CommandLine::StringViewType kAboutBlankPlaceholder =
#if BUILDFLAG(IS_WIN)
      L"about:blank";
#else
      "about:blank";
#endif
  for (const auto& arg : command_line.GetArgs()) {
    if (arg != kAboutBlankPlaceholder) {
      return true;
    }
  }
  return false;
}

// True when |command_line| is a plain relaunch: no URL/file arguments and none
// of |window_kind_switches| (switches asking for a specific kind of window or
// carrying their own action). A plain relaunch goes to the kNexus window.
inline bool IsPlainRelaunchCommandLine(
    const base::CommandLine& command_line,
    base::span<const std::string_view> window_kind_switches) {
  if (HasExternalUrlArgs(command_line)) {
    return false;
  }
  for (std::string_view name : window_kind_switches) {
    if (command_line.HasSwitch(name)) {
      return false;
    }
  }
  return true;
}

}  // namespace custom_browser::window_util

#endif  // CUSTOM_BROWSER_UI_VIEW_RELAUNCH_COMMAND_LINE_H_
