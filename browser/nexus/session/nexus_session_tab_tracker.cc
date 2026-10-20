// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/session/nexus_session_tab_tracker.h"

#include <algorithm>
#include <utility>

#include "base/logging.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/browser.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface.h"
#include "chrome/browser/ui/browser_window/public/browser_window_interface_iterator.h"
#include "chrome/browser/ui/tabs/tab_strip_model.h"
#include "components/keyed_service/content/browser_context_dependency_manager.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/web_contents.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/ui/tabs/tab_enums.h"
#include "content/public/browser/navigation_controller.h"
#include "content/public/common/referrer.h"
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"
#include "ui/base/page_transition_types.h"
#include "url/gurl.h"

namespace nexus {

NexusSessionTabTracker::NexusSessionTabTracker(Profile* profile)
    : profile_(profile), strip_tracker_(this, this) {
  // Init() replays an OnTabStripModelChanged(kInserted) per existing tracked
  // browser, so the cache reflects pre-existing tagged tabs (e.g. a tracker
  // created lazily after the first tool call already ran).
  strip_tracker_.Init();
}

NexusSessionTabTracker::~NexusSessionTabTracker() = default;

void NexusSessionTabTracker::AddObserver(Observer* observer) {
  observers_.AddObserver(observer);
}

void NexusSessionTabTracker::RemoveObserver(Observer* observer) {
  observers_.RemoveObserver(observer);
}

void NexusSessionTabTracker::NotifyTabBound(content::WebContents* contents) {
  if (!contents) {
    return;
  }
  RecomputeAndNotify();
}

void NexusSessionTabTracker::SetLastWorkTab(const std::string& session_key,
                                            tabs::TabHandle handle) {
  if (session_key.empty()) {
    return;
  }
  last_work_tabs_[session_key] = handle;
}

std::optional<tabs::TabHandle> NexusSessionTabTracker::GetLastWorkTab(
    const std::string& session_key) const {
  auto it = last_work_tabs_.find(session_key);
  if (it == last_work_tabs_.end()) {
    return std::nullopt;
  }
  tabs::TabInterface* tab = it->second.Get();
  if (!tab) {
    return std::nullopt;
  }
  const std::string* tag =
      NexusSessionTabTag::SessionKeyFor(tab->GetContents());
  if (!tag || *tag != session_key) {
    return std::nullopt;
  }
  return it->second;
}

SessionTabCounts NexusSessionTabTracker::GetCounts(
    const std::string& session_key) const {
  auto it = counts_.find(session_key);
  return it == counts_.end() ? SessionTabCounts{} : it->second;
}

std::vector<tabs::TabHandle>
NexusSessionTabTracker::CollectSessionTabsInNexusWindow(
    const std::string& session_key) const {
  std::vector<tabs::TabHandle> out;
  if (session_key.empty()) {
    return out;
  }
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        BrowserWindowInterface* browser =
            window ? window : nullptr;
        if (!browser || browser->GetProfile() != profile_ ||
            !custom_browser::window_util::IsNexusBrowser(browser)) {
          return true;
        }
        TabStripModel* tab_strip = browser->tab_strip_model();
        if (!tab_strip) {
          return true;
        }
        for (int i = 0; i < tab_strip->count(); ++i) {
          const std::string* tag = NexusSessionTabTag::SessionKeyFor(
              tab_strip->GetWebContentsAt(i));
          if (tag && *tag == session_key) {
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

bool NexusSessionTabTracker::ActivateSessionTab(
    const std::string& session_key) {
  std::vector<tabs::TabHandle> nexus_tabs =
      CollectSessionTabsInNexusWindow(session_key);
  if (nexus_tabs.empty()) {
    return false;
  }
  tabs::TabHandle target = nexus_tabs.front();
  if (std::optional<tabs::TabHandle> last_work = GetLastWorkTab(session_key);
      last_work.has_value() &&
      std::ranges::contains(nexus_tabs, *last_work)) {
    target = *last_work;
  }
  tabs::TabInterface* tab = target.Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    return false;
  }
  BrowserWindowInterface* browser = custom_browser::window_util::FindNexusWindow(profile_);
  TabStripModel* tab_strip = browser ? browser->tab_strip_model() : nullptr;
  if (!tab_strip) {
    return false;
  }
  const int index = tab_strip->GetIndexOfWebContents(contents);
  if (index == TabStripModel::kNoTab) {
    return false;
  }
  if (index != tab_strip->active_index()) {
    tab_strip->ActivateTabAt(index);
  }
  return true;
}

std::vector<SessionTabEntry> NexusSessionTabTracker::GetSessionTabs(
    const std::string& session_key) const {
  std::vector<SessionTabEntry> out;
  if (session_key.empty()) {
    return out;
  }
  BrowserWindowInterface* browser = custom_browser::window_util::FindNexusWindow(profile_);
  TabStripModel* tab_strip = browser ? browser->tab_strip_model() : nullptr;
  if (!tab_strip) {
    return out;
  }
  for (int i = 0; i < tab_strip->count(); ++i) {
    content::WebContents* contents = tab_strip->GetWebContentsAt(i);
    const std::string* tag = NexusSessionTabTag::SessionKeyFor(contents);
    if (!tag || *tag != session_key) {
      continue;
    }
    tabs::TabInterface* tab = tab_strip->GetTabAtIndex(i);
    if (!tab) {
      continue;
    }
    SessionTabEntry entry;
    entry.id = tab->GetHandle().raw_value();
    entry.title = base::UTF16ToUTF8(contents->GetTitle());
    entry.url = contents->GetVisibleURL().spec();
    entry.active = i == tab_strip->active_index();
    out.push_back(std::move(entry));
  }
  return out;
}

int NexusSessionTabTracker::FindSessionTab(const std::string& session_key,
                                           int32_t tab_id,
                                           TabStripModel** out_strip) const {
  if (session_key.empty()) {
    return TabStripModel::kNoTab;
  }
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  const std::string* tag = NexusSessionTabTag::SessionKeyFor(contents);
  if (!tag || *tag != session_key) {
    return TabStripModel::kNoTab;
  }
  BrowserWindowInterface* browser = custom_browser::window_util::FindNexusWindow(profile_);
  TabStripModel* tab_strip = browser ? browser->tab_strip_model() : nullptr;
  if (!tab_strip) {
    return TabStripModel::kNoTab;
  }
  const int index = tab_strip->GetIndexOfWebContents(contents);
  if (index != TabStripModel::kNoTab && out_strip) {
    *out_strip = tab_strip;
  }
  return index;
}

bool NexusSessionTabTracker::ActivateTab(const std::string& session_key,
                                         int32_t tab_id) {
  TabStripModel* tab_strip = nullptr;
  const int index = FindSessionTab(session_key, tab_id, &tab_strip);
  if (index == TabStripModel::kNoTab) {
    return false;
  }
  if (index != tab_strip->active_index()) {
    tab_strip->ActivateTabAt(index);
  }
  return true;
}

bool NexusSessionTabTracker::CloseTab(const std::string& session_key,
                                      int32_t tab_id) {
  TabStripModel* tab_strip = nullptr;
  const int index = FindSessionTab(session_key, tab_id, &tab_strip);
  if (index == TabStripModel::kNoTab) {
    return false;
  }
  tab_strip->CloseWebContentsAt(
      index, TabCloseTypes::CLOSE_USER_GESTURE |
                 TabCloseTypes::CLOSE_CREATE_HISTORICAL_TAB);
  return true;
}

bool NexusSessionTabTracker::NavigateTab(const std::string& session_key,
                                         int32_t tab_id,
                                         const GURL& url) {
  TabStripModel* tab_strip = nullptr;
  const int index = FindSessionTab(session_key, tab_id, &tab_strip);
  if (index == TabStripModel::kNoTab || !url.is_valid()) {
    return false;
  }
  content::WebContents* contents = tab_strip->GetWebContentsAt(index);
  if (!contents) {
    return false;
  }
  contents->GetController().LoadURL(url, content::Referrer(),
                                    ui::PAGE_TRANSITION_TYPED, std::string());
  return true;
}

void NexusSessionTabTracker::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  // Counts only depend on strip membership; pure selection flips are the
  // high-frequency case and never change them. The tab list does carry the
  // active tab, so a flip between tagged tabs re-publishes their sessions.
  if (change.type() == TabStripModelChange::kSelectionOnly) {
    if (selection.active_tab_changed()) {
      NotifySessionTabsFor(selection.old_contents);
      NotifySessionTabsFor(selection.new_contents);
    }
    return;
  }
  RecomputeAndNotify();
}

void NexusSessionTabTracker::OnTabChangedAt(tabs::TabInterface* tab,
                                            TabChangeType change_type) {
  // A title or URL change arrives as kAll; a loading flip carries the new
  // URL too, so both re-publish the tab's session.
  if (change_type == TabChangeType::kAttentionOnly ||
      change_type == TabChangeType::kBlockedOnly) {
    return;
  }
  NotifySessionTabsFor(tab ? tab->GetContents() : nullptr);
}

void NexusSessionTabTracker::NotifySessionTabsFor(
    content::WebContents* contents) {
  const std::string* tag = NexusSessionTabTag::SessionKeyFor(contents);
  if (tag && !tag->empty()) {
    NotifySessionTabs(*tag);
  }
}

void NexusSessionTabTracker::NotifySessionTabs(const std::string& session_key) {
  const std::vector<SessionTabEntry> tabs = GetSessionTabs(session_key);
  for (Observer& observer : observers_) {
    observer.OnSessionTabsChanged(session_key, tabs);
  }
}

bool NexusSessionTabTracker::ShouldTrackBrowser(
    BrowserWindowInterface* browser) {
  return browser && browser->GetProfile() == profile_;
}

std::vector<SessionTabSnapshotEntry>
NexusSessionTabTracker::SnapshotTaggedTabs() const {
  std::vector<SessionTabSnapshotEntry> snapshot;
  ForEachCurrentBrowserWindowInterfaceOrderedByActivation(
      [&](BrowserWindowInterface* window) {
        BrowserWindowInterface* browser =
            window ? window : nullptr;
        if (!browser || browser->GetProfile() != profile_) {
          return true;
        }
        TabStripModel* tab_strip = browser->tab_strip_model();
        if (!tab_strip) {
          return true;
        }
        const bool in_nexus_window =
            custom_browser::window_util::IsNexusBrowser(browser);
        for (int i = 0; i < tab_strip->count(); ++i) {
          const std::string* tag = NexusSessionTabTag::SessionKeyFor(
              tab_strip->GetWebContentsAt(i));
          if (tag && !tag->empty()) {
            snapshot.push_back({*tag, in_nexus_window});
          }
        }
        return true;
      });
  return snapshot;
}

void NexusSessionTabTracker::RecomputeAndNotify() {
  SessionTabCountsMap new_counts = BuildCountsFromSnapshot(SnapshotTaggedTabs());
  std::vector<SessionTabCountsChange> changes =
      DiffSessionTabCounts(counts_, new_counts);
  counts_ = std::move(new_counts);
  for (const SessionTabCountsChange& change : changes) {
    for (Observer& observer : observers_) {
      observer.OnSessionTabCountsChanged(change.session_key, change.counts);
    }
  }
  // Membership, order and the active tab can all move on one strip event
  // without any count changing (a reorder, a close that activates a
  // neighbour): every session with a tab, and every one that just lost
  // its last, gets its list again.
  std::vector<std::string> keys;
  for (const auto& [session_key, counts] : counts_) {
    keys.push_back(session_key);
  }
  for (const SessionTabCountsChange& change : changes) {
    if (!std::ranges::contains(keys, change.session_key)) {
      keys.push_back(change.session_key);
    }
  }
  for (const std::string& session_key : keys) {
    NotifySessionTabs(session_key);
  }
}

// ---------------------------------------------------------------------------
// Factory

// static
NexusSessionTabTracker* NexusSessionTabTrackerFactory::GetForProfile(
    Profile* profile) {
  if (!profile) {
    return nullptr;
  }
  return static_cast<NexusSessionTabTracker*>(
      GetInstance()->GetServiceForBrowserContext(profile, /*create=*/true));
}

// static
NexusSessionTabTrackerFactory* NexusSessionTabTrackerFactory::GetInstance() {
  static base::NoDestructor<NexusSessionTabTrackerFactory> instance;
  return instance.get();
}

NexusSessionTabTrackerFactory::NexusSessionTabTrackerFactory()
    : BrowserContextKeyedServiceFactory(
          "NexusSessionTabTracker",
          BrowserContextDependencyManager::GetInstance()) {}

NexusSessionTabTrackerFactory::~NexusSessionTabTrackerFactory() = default;

std::unique_ptr<KeyedService>
NexusSessionTabTrackerFactory::BuildServiceInstanceForBrowserContext(
    content::BrowserContext* context) const {
  return std::make_unique<NexusSessionTabTracker>(
      Profile::FromBrowserContext(context));
}

}  // namespace nexus
