// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_CLIENTS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_CLIENTS_H_

#include "base/observer_list_types.h"

namespace nexus::remote_access {

// What remote access looks like to the rest of the browser process: is any
// listener up, and how many remote devices are connected through it. Summed
// over every profile's `NexusRemoteAccessService`.
struct ConnectedClients {
  bool listening = false;
  int clients = 0;

  friend bool operator==(const ConnectedClients&,
                         const ConnectedClients&) = default;
};

// Told whenever the process-wide summary changes (and only then).
class ConnectedClientsObserver : public base::CheckedObserver {
 public:
  virtual void OnConnectedClientsChanged(const ConnectedClients& summary) = 0;
};

// The process-wide registry behind the tray tooltip and the quit confirmation:
// a //base-only leaf, so browser-process globals need not depend on the
// per-Profile service. UI thread only. A report replaces its `source`'s
// previous one; `Forget` drops it.
void ReportConnectedClients(const void* source, ConnectedClients clients);
void ForgetConnectedClients(const void* source);

// The summary over every source: `listening` if any is, `clients` summed.
ConnectedClients GetConnectedClients();

void AddConnectedClientsObserver(ConnectedClientsObserver* observer);
void RemoveConnectedClientsObserver(ConnectedClientsObserver* observer);

// Drops every source and observer. Tests only.
void ResetConnectedClientsForTesting();

}  // namespace nexus::remote_access

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_REMOTE_ACCESS_NEXUS_REMOTE_ACCESS_CLIENTS_H_
