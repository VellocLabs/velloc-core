// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

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
#include "custom_browser/browser/nexus/session/nexus_session_tab_tag.h"
#include "custom_browser/ui/view/custom_browser_window_util.h"

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
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile_ ||
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
  Browser* browser = custom_browser::window_util::FindNexusWindow(profile_);
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

void NexusSessionTabTracker::OnTabStripModelChanged(
    TabStripModel* tab_strip_model,
    const TabStripModelChange& change,
    const TabStripSelectionChange& selection) {
  // Counts only depend on strip membership; pure selection flips are the
  // high-frequency case and never change them.
  if (change.type() == TabStripModelChange::kSelectionOnly) {
    return;
  }
  RecomputeAndNotify();
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
        Browser* browser =
            window ? window->GetBrowserForMigrationOnly() : nullptr;
        if (!browser || browser->profile() != profile_) {
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
