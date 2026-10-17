// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_clients.h"

#include "base/containers/flat_map.h"
#include "base/no_destructor.h"
#include "base/observer_list.h"

namespace nexus::remote_access {

namespace {

struct Registry {
  base::flat_map<const void*, ConnectedClients> sources;
  base::ObserverList<ConnectedClientsObserver> observers;
};

Registry& GetRegistry() {
  static base::NoDestructor<Registry> registry;
  return *registry;
}

ConnectedClients Summarize(const Registry& registry) {
  ConnectedClients summary;
  for (const auto& [source, clients] : registry.sources) {
    summary.listening = summary.listening || clients.listening;
    summary.clients += clients.clients;
  }
  return summary;
}

// Applies `mutate` and tells the observers when the SUMMARY moved — a report
// that changes nothing visible (a second profile reporting 0) is silent.
template <typename Mutate>
void Update(Mutate mutate) {
  Registry& registry = GetRegistry();
  const ConnectedClients before = Summarize(registry);
  mutate(registry);
  const ConnectedClients after = Summarize(registry);
  if (after == before) {
    return;
  }
  for (ConnectedClientsObserver& observer : registry.observers) {
    observer.OnConnectedClientsChanged(after);
  }
}

}  // namespace

void ReportConnectedClients(const void* source, ConnectedClients clients) {
  Update([&](Registry& registry) { registry.sources[source] = clients; });
}

void ForgetConnectedClients(const void* source) {
  Update([&](Registry& registry) { registry.sources.erase(source); });
}

ConnectedClients GetConnectedClients() {
  return Summarize(GetRegistry());
}

bool HasConnectedDevices(const ConnectedClients& summary) {
  return summary.listening && summary.clients > 0;
}

void AddConnectedClientsObserver(ConnectedClientsObserver* observer) {
  GetRegistry().observers.AddObserver(observer);
}

void RemoveConnectedClientsObserver(ConnectedClientsObserver* observer) {
  GetRegistry().observers.RemoveObserver(observer);
}

void ResetConnectedClientsForTesting() {
  Registry& registry = GetRegistry();
  registry.sources.clear();
  registry.observers.Clear();
}

}  // namespace nexus::remote_access
