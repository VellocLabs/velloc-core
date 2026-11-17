#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <array>
#include <string>
#include <string_view>
#include <vector>

#include "base/hash/hash.h"
#include "base/memory/raw_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "chrome/browser/ui/tabs/tab_group_model.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/tab_groups/tab_group_color.h"
#include "components/tab_groups/tab_group_visual_data.h"
#include "components/tabs/public/tab_group.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/session/nexus_session_features.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/gfx/geometry/size.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

// Deterministic color picker for the session's tab group. Hashing the
// session_key onto one of eight non-grey palette entries gives every
// session a stable color across the browser-process lifetime without
// requiring any persisted state — group lifetime is in-process anyway.
tab_groups::TabGroupColorId DeterministicColorFor(
    std::string_view session_key) {
  static constexpr std::array<tab_groups::TabGroupColorId, 8> kPalette = {
      tab_groups::TabGroupColorId::kBlue,
      tab_groups::TabGroupColorId::kRed,
      tab_groups::TabGroupColorId::kYellow,
      tab_groups::TabGroupColorId::kGreen,
      tab_groups::TabGroupColorId::kPink,
      tab_groups::TabGroupColorId::kPurple,
      tab_groups::TabGroupColorId::kCyan,
      tab_groups::TabGroupColorId::kOrange,
  };
  const size_t i = base::FastHash(session_key) % kPalette.size();
  return kPalette[i];
}

std::u16string TabGroupTitleFor(std::string_view session_key) {
  // Twelve chars is enough to disambiguate typical short session ids
  // without bloating the tab-strip pill.
  constexpr size_t kMaxLen = 12;
  std::string truncated(session_key.substr(0, kMaxLen));
  if (session_key.size() > kMaxLen) {
    truncated.append("...");
  }
  return base::UTF8ToUTF16("Nexus: " + truncated);
}

// Resolve (Browser*, TabStripModel*, index, WebContents*) for a handle.
// `browser` is null when the tab is gone or its WebContents is
// orphaned.
struct LocatedTab {
  raw_ptr<BrowserWindowInterface> browser = nullptr;
  raw_ptr<TabStripModel> tab_strip = nullptr;
  int index = TabStripModel::kNoTab;
  raw_ptr<content::WebContents> contents = nullptr;
};

LocatedTab LocateTabForHandle(tabs::TabHandle handle) {
  LocatedTab out;
  tabs::TabInterface* tab = handle.Get();
  if (!tab) return out;
  content::WebContents* contents = tab->GetContents();
  if (!contents) return out;
  out.contents = contents;
  out.browser = tab->GetBrowserWindowInterface();
  if (!out.browser) return out;
  out.tab_strip = out.browser->tab_strip_model();
  if (!out.tab_strip) {
    out.browser = nullptr;
    return out;
  }
  out.index = out.tab_strip->GetIndexOfWebContents(contents);
  return out;
}

}  // namespace

void BrowserTool::HoldSessionTabsForTurn() {
  HoldTabsForTurn(CollectSessionTabs());
}

void BrowserTool::HoldTabsForTurn(base::span<const tabs::TabHandle> handles) {
  for (const tabs::TabHandle& handle : handles) {
    TakeCaptureHold(handle);
  }
}

void BrowserTool::TakeCaptureHold(tabs::TabHandle handle) {
  if (session_tab_capture_holds_.contains(handle)) {
    return;
  }
  tabs::TabInterface* tab = handle.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return;
  }
  // A visible capturer keeps the tab compositing while its window is occluded
  // behind the chat window, so observations and screenshots do not stall.
  // stay_hidden=false: the page counts as visible. stay_awake=false: no
  // display wake lock. is_activity=false: no synthetic user activity.
  session_tab_capture_holds_.emplace(
      handle, contents->IncrementCapturerCount(gfx::Size(),
                                               /*stay_hidden=*/false,
                                               /*stay_awake=*/false,
                                               /*is_activity=*/false));
}

void BrowserTool::BindTabToSession(tabs::TabHandle handle) {
  if (!::nexus::features::IsSessionScopedTabsEnabled() ||
      GetSessionKey().empty()) {
    return;
  }
  LocatedTab located = LocateTabForHandle(handle);
  if (!located.contents) {
    return;
  }
  ::nexus::NexusSessionTabTag::AttachTo(located.contents, GetSessionKey());

  // Add to (or create) the session's tab group. Best-effort: some
  // browser types (devtools, app windows) disable groups; that's fine,
  // the tag is still the source of truth. The visual group chrome is
  // kNexus-window-only: tabs hosted in a normal browsing window (the
  // separate-agent-window pref, or a user drag) stay ordinary-looking
  // tabs — the user asked for the session grouping to be invisible
  // there. The tag still associates them with the session.
  if (located.browser && located.tab_strip &&
      custom_browser::window_util::IsNexusBrowser(located.browser) &&
      located.tab_strip->SupportsTabGroups() &&
      located.index != TabStripModel::kNoTab) {
    if (tab_group_id_.has_value()) {
      std::optional<tab_groups::TabGroupId> current =
          located.tab_strip->GetTabGroupForTab(located.index);
      if (current != tab_group_id_) {
        located.tab_strip->AddToExistingGroup({located.index}, *tab_group_id_,
                                               /*add_to_end=*/true);
      }
    } else {
      tab_group_id_ = located.tab_strip->AddToNewGroup({located.index});
      if (TabGroup* group = located.tab_strip->group_model()->GetTabGroup(
              *tab_group_id_)) {
        tab_groups::TabGroupVisualData visual_data(
            TabGroupTitleFor(GetSessionKey()),
            DeterministicColorFor(GetSessionKey()),
            /*is_collapsed=*/false);
        group->SetVisualData(std::move(visual_data),
                              /*is_customized=*/true);
      }
      LOG(INFO) << "[BrowserTool] created tab group for session="
                << GetSessionKey()
                << " group=" << tab_group_id_->ToString();
    }
  }

  session_tabs_.insert(handle);

  // A tab is bound while a tool runs, so it joins the turn's holds.
  TakeCaptureHold(handle);

  // Nudge the profile-wide tracker: its TabStripModelObserver saw the
  // strip insert BEFORE the tag attach above, so the new tab was tallied
  // as untagged. This recount flips the session's counts and emits
  // `browser.sessionTabsChanged` to the guest (webview_area reveal).
  if (NexusSessionTabTracker* tracker =
          NexusSessionTabTrackerFactory::GetForProfile(GetProfile())) {
    tracker->NotifyTabBound(located.contents);
  }
}

void BrowserTool::RecordWorkTab(tabs::TabHandle handle) {
  // Verify the handle is still alive AND carries this session's tag
  // before storing — a working_tab_id resolved at the call site can
  // race a tab close on shutdown paths, and we don't want a stale
  // handle handed back by `LastWorkTab()` next call.
  tabs::TabInterface* tab = handle.Get();
  if (!tab) return;
  const std::string* tag =
      ::nexus::NexusSessionTabTag::SessionKeyFor(tab->GetContents());
  if (!tag || *tag != GetSessionKey()) {
    return;
  }
  last_work_tab_ = handle;
  // Mirror into the profile-wide tracker so `session.setActiveSession`
  // (handled on the page port, which cannot see this BrowserTool) can
  // honor "activate the last-worked tab" on session switch.
  if (NexusSessionTabTracker* tracker =
          NexusSessionTabTrackerFactory::GetForProfile(GetProfile())) {
    tracker->SetLastWorkTab(GetSessionKey(), handle);
  }
}

std::optional<tabs::TabHandle> BrowserTool::LastWorkTab() const {
  tabs::TabInterface* tab = last_work_tab_.Get();
  if (!tab) {
    return std::nullopt;
  }
  const std::string* tag =
      ::nexus::NexusSessionTabTag::SessionKeyFor(tab->GetContents());
  if (!tag || *tag != GetSessionKey()) {
    return std::nullopt;
  }
  return last_work_tab_;
}

std::vector<tabs::TabHandle> BrowserTool::CollectSessionTabs() const {
  std::vector<tabs::TabHandle> out;
  Profile* profile = GetProfile();
  if (!profile || GetSessionKey().empty()) {
    return out;
  }
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        BrowserWindowInterface* browser = window;
        if (!browser || browser->GetProfile() != profile) {
          return true;
        }
        TabStripModel* tab_strip = browser->tab_strip_model();
        if (!tab_strip) {
          return true;
        }
        for (int i = 0; i < tab_strip->count(); ++i) {
          content::WebContents* contents = tab_strip->GetWebContentsAt(i);
          const std::string* tag =
              ::nexus::NexusSessionTabTag::SessionKeyFor(contents);
          if (tag && *tag == GetSessionKey()) {
            tabs::TabInterface* tab = tab_strip->GetTabAtIndex(i);
            if (tab) {
              out.push_back(tab->GetHandle());
            }
          }
        }
        return true;
      });
  return out;
}

void BrowserTool::CloseAllSessionTabsOnDestroy() {
  if (!::nexus::features::IsSessionScopedTabsEnabled() ||
      GetSessionKey().empty()) {
    session_tab_capture_holds_.clear();
    session_tabs_.clear();
    tab_group_id_.reset();
    return;
  }
  // The tag — not session_tabs_ — is authoritative. A tab the user
  // dragged out of the group is still session-owned and gets closed.
  std::vector<tabs::TabHandle> tabs = CollectSessionTabs();
  LOG(INFO) << "[BrowserTool] closing tabs on destroy session="
            << GetSessionKey() << " count=" << tabs.size();
  for (const tabs::TabHandle& handle : tabs) {
    LocatedTab located = LocateTabForHandle(handle);
    if (!located.tab_strip || located.index == TabStripModel::kNoTab) {
      continue;
    }
    // CLOSE_USER_GESTURE keeps the close from being treated as a
    // crash-style teardown; CLOSE_CREATE_HISTORICAL_TAB lets the user
    // recover via Ctrl+Shift+T if they close the panel by mistake.
    located.tab_strip->CloseWebContentsAt(
        located.index,
        TabCloseTypes::CLOSE_USER_GESTURE |
            TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  }
  // Drop every capture hold (safe no-op for tabs already gone) — releases the
  // forced-render state before the tabs close.
  session_tab_capture_holds_.clear();
  session_tabs_.clear();
  tab_group_id_.reset();
}

}  // namespace nexus
