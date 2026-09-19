// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_service.h"

#include <array>
#include <string_view>
#include <utility>

#include "base/functional/bind.h"
#include "base/json/json_writer.h"
#include "base/location.h"
#include "base/logging.h"
#include "base/rand_util.h"
#include "base/strings/string_number_conversions.h"
#include "base/supports_user_data.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "chrome/browser/content_settings/host_content_settings_map_factory.h"
#include "chrome/browser/profiles/profile.h"
#include "components/content_settings/core/browser/host_content_settings_map.h"
#include "components/content_settings/core/common/content_settings.h"
#include "components/content_settings/core/common/content_settings_pattern.h"
#include "components/content_settings/core/common/content_settings_types.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/nexus_guest_config.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_clients.h"
#include "custom_browser/browser/nexus/nexus_interface_version.h"
#include "custom_browser/browser/nexus/remote_access/remote_access_router.h"
#include "net/base/ip_address.h"
#include "net/base/network_interfaces.h"
#include "net/http/http_request_headers.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/mojom/url_response_head.mojom.h"

namespace nexus::remote_access {

namespace {

// Port bounds enforced by the pref table (plan §8.3). Port 0 is a valid
// special case: an explicit request for an OS-assigned ephemeral port (the
// bound port is echoed back in the status snapshot). It is NOT a conflicted
// fixed port — a fixed port that is already in use still errors port_in_use
// and is never silently switched (§8.3).
constexpr int kMinPort = 1024;
constexpr int kMaxPort = 65535;
// Reverse-proxy body cap. Plan §8.3 asked for 32 MiB, but DownloadToString
// DCHECKs above SimpleURLLoader's own bound (5 MiB) — so that bound IS the cap.
// A larger upstream body fails the load and is answered 502; the bundle's
// chunks are far below it.
constexpr size_t kMaxProxyBodyBytes =
    network::SimpleURLLoader::kMaxBoundedStringDownloadSize;

const char kRemoteAccessServiceKey[] = "nexus::RemoteAccessService";

// Traffic annotation for the proxy's OUTBOUND fetch to the guest origin (the
// only upstream it will ever contact). The listener's own inbound side has
// its own annotation in nexus_remote_access_listener.cc.
constexpr net::NetworkTrafficAnnotationTag kProxyTraffic =
    net::DefineNetworkTrafficAnnotation("nexus_remote_access_proxy", R"(
      semantics {
        sender: "Nexus Remote Access Reverse Proxy"
        description:
          "Fetches the Nexus guest origin's immutable build entry (and its "
          "versioned assets + three fixed root files) so the desktop's "
          "embedded relay can serve them to a remote browser over plain "
          "HTTP on the LAN/VPN. The ONLY upstream is the resolved guest "
          "origin; no query, cookie, or arbitrary path is forwarded."
        trigger:
          "A remote browser, joined via the user-enabled remote-access "
          "listener, requests the bundle over the listener's /v<N>/… routes."
        data:
          "A path under the guest origin. No user data, credentials, or "
          "conversation content is included — this is a GET for static "
          "build artifacts."
        destination: OTHER
        internal {
          contacts {
            email: "niclas@niclas.cc"
          }
        }
        user_data {
          type: OTHER
        }
        last_reviewed: "2026-09-18"
      }
      policy {
        cookies_allowed: NO
        setting:
          "Remote access is OFF by default and toggled in Settings."
        policy_exception_justification:
          "Not yet covered by enterprise policy."
      }
    )");

class RemoteAccessServiceUserData : public base::SupportsUserData::Data {
 public:
  explicit RemoteAccessServiceUserData(Profile* profile) : service(profile) {}
  NexusRemoteAccessService service;
};

// True when `address` is an IPv4 literal the listener may be asked to bind.
// Loopback is allowed (local debugging) and — since 2026-09-20 — so is the
// wildcard 0.0.0.0 (the JS layer's default bind; C++ is a pure executor with
// no default concept and merely accepts the literal it is handed). :: is not
// accepted. Pure on purpose: whether the machine still HAS a specific address
// is answered by the bind itself (StartResult::kAddressUnavailable) — asking
// net::GetNetworkList here would be a blocking call on the UI thread.
bool IsBindableLiteral(const std::string& address) {
  net::IPAddress ip;
  return ip.AssignFromIPLiteral(address) && ip.IsIPv4();
}

}  // namespace

RemoteAccessStatus::RemoteAccessStatus() = default;
RemoteAccessStatus::~RemoteAccessStatus() = default;
RemoteAccessStatus::RemoteAccessStatus(const RemoteAccessStatus&) = default;
RemoteAccessStatus& RemoteAccessStatus::operator=(const RemoteAccessStatus&) =
    default;
RemoteAccessStatus::RemoteAccessStatus(RemoteAccessStatus&&) noexcept =
    default;
RemoteAccessStatus& RemoteAccessStatus::operator=(
    RemoteAccessStatus&&) noexcept = default;

NexusRemoteAccessService::ProxyFetch::ProxyFetch() = default;
NexusRemoteAccessService::ProxyFetch::~ProxyFetch() = default;
NexusRemoteAccessService::ProxyFetch::ProxyFetch(ProxyFetch&&) noexcept =
    default;
NexusRemoteAccessService::ProxyFetch&
NexusRemoteAccessService::ProxyFetch::operator=(ProxyFetch&&) noexcept =
    default;

NexusRemoteAccessService::NexusRemoteAccessService(Profile* profile)
    : profile_(profile) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // Bound to a WeakPtr of this UI-thread object: safe because the listener
  // always runs its ProxyCallback on the sequence it was created on.
  listener_ = std::make_unique<NexusRemoteAccessListener>(
      base::BindRepeating(&NexusRemoteAccessService::HandleProxyRequest,
                          weak_factory_.GetWeakPtr()),
      // A device connecting or leaving is a status change like any other:
      // the page's count and the tray tooltip both come from it.
      base::BindRepeating(&NexusRemoteAccessService::OnListenerClientsChanged,
                          weak_factory_.GetWeakPtr()));
  if (profile_) {
    url_loader_factory_ = profile_->GetDefaultStoragePartition()
                              ->GetURLLoaderFactoryForBrowserProcess();
  }
  // Prime the address cache off-thread so the first getStatus has it.
  RequestAddressesRefresh();
  // Bring the listener up if the prefs say it should be (e.g. the user left
  // remote access enabled across a restart).
  SyncListenerToPrefs();
}

NexusRemoteAccessService::~NexusRemoteAccessService() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // This profile's devices leave the process-wide count with it.
  ForgetConnectedClients(this);
  // ~NexusRemoteAccessListener tears the relay down without blocking.
}

// static
NexusRemoteAccessService& GetRemoteAccessService(Profile* profile) {
  CHECK(profile);
  auto* data = static_cast<RemoteAccessServiceUserData*>(
      profile->GetUserData(kRemoteAccessServiceKey));
  if (!data) {
    auto owned = std::make_unique<RemoteAccessServiceUserData>(profile);
    data = owned.get();
    profile->SetUserData(kRemoteAccessServiceKey, std::move(owned));
  }
  return data->service;
}

RemoteAccessStatus NexusRemoteAccessService::BuildStatus() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  RemoteAccessStatus status;
  PrefService* prefs = profile_->GetPrefs();
  status.enabled = prefs->GetBoolean(prefs::kNexusRemoteAccessEnabled);
  status.bind_address = prefs->GetString(prefs::kNexusRemoteAccessBindAddress);
  // When listening, report the ACTUAL bound port (which differs from the
  // configured pref whenever the pref was 0 = OS-assigned ephemeral). When
  // not listening there is no bound port, so report the configured pref.
  status.port = state_ == ListenerState::kListening
                    ? static_cast<int>(bound_port_)
                    : prefs->GetInteger(prefs::kNexusRemoteAccessPort);
  status.state = state_;
  status.error = error_;
  status.clients = ConnectedClientCount();
  if (state_ == ListenerState::kListening) {
    // Under a wildcard bind the runtime must dial a concrete same-machine
    // address — 0.0.0.0 is not dialable. 127.0.0.1 is always local and always
    // passes the /host/* loopback gate, so the control URL uses it.
    const std::string dial_address =
        bind_ip_wildcard_ ? "127.0.0.1" : status.bind_address;
    status.control_url = "ws://" + dial_address + ":" +
                         base::NumberToString(bound_port_) + "/host/control";
    status.control_token = control_token_;
  }
  status.addresses = cached_addresses_;
  return status;
}

RemoteAccessStatus NexusRemoteAccessService::GetStatus() {
  // The address list is a cache refreshed off-thread: net::GetNetworkList is
  // a blocking ScopedBlockingCall, so it must never run on the UI thread
  // (BrowserMainLoop calls DisallowUnresponsiveTasks() at startup; a sync
  // call here DCHECKs — thread_restrictions.cc). The first snapshot after
  // construction can be empty/stale until the refresh lands; a refresh that
  // changes the list pushes a status change. Only THIS entry point asks for a
  // refresh — the event path (BuildStatus) must not, or every refresh would
  // schedule the next one forever.
  RequestAddressesRefresh();
  return BuildStatus();
}

RemoteAccessStatus NexusRemoteAccessService::GetStatusForEvent() {
  RemoteAccessStatus status = BuildStatus();
  // The control token never leaves the full-trust getStatus surface (§8.8).
  status.control_token.reset();
  return status;
}

uint64_t NexusRemoteAccessService::AddStatusSink(StatusSink sink) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  const uint64_t id = next_status_sink_id_++;
  status_sinks_.emplace(id, std::move(sink));
  return id;
}

void NexusRemoteAccessService::RemoveStatusSink(uint64_t id) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  status_sinks_.erase(id);
}

int NexusRemoteAccessService::ConnectedClientCount() const {
  // Only a listening relay has clients. The count is pushed from the IO
  // thread, so a report posted just before a Stop can still land after it;
  // gating on the state keeps that from showing devices on a stopped relay.
  return state_ == ListenerState::kListening
             ? static_cast<int>(listener_->client_count())
             : 0;
}

void NexusRemoteAccessService::OnListenerClientsChanged(size_t count) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  NotifyStatusChanged();
}

void NexusRemoteAccessService::NotifyStatusChanged() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The process-wide summary hears every change, subscribed page or not: the
  // tray tooltip and the quit confirmation read it with no page open.
  ReportConnectedClients(
      this, {.listening = state_ == ListenerState::kListening,
             .clients = ConnectedClientCount()});
  if (status_sinks_.empty()) {
    return;
  }
  const RemoteAccessStatus snapshot = GetStatusForEvent();
  for (const auto& [id, sink] : status_sinks_) {
    sink.Run(snapshot);
  }
}

RemoteAccessStatus NexusRemoteAccessService::Configure(
    std::optional<bool> enabled,
    std::optional<std::string> bind_address,
    std::optional<int> port) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrefService* prefs = profile_->GetPrefs();
  if (enabled.has_value()) {
    prefs->SetBoolean(prefs::kNexusRemoteAccessEnabled, *enabled);
  }
  if (bind_address.has_value()) {
    prefs->SetString(prefs::kNexusRemoteAccessBindAddress, *bind_address);
  }
  if (port.has_value()) {
    prefs->SetInteger(prefs::kNexusRemoteAccessPort, *port);
  }
  SyncListenerToPrefs();
  return GetStatus();
}

const base::ListValue& NexusRemoteAccessService::GetPairings() const {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  return profile_->GetPrefs()->GetList(prefs::kNexusRemoteAccessPairings);
}

bool NexusRemoteAccessService::SetPairings(base::ListValue pairings) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  // The schema is the runtime's, but the pref file is this process's: bound
  // what a caller can park in it. Both limits sit far above the real table
  // (a pairing row is ~300 bytes and the runtime caps the table itself).
  if (pairings.size() > kMaxStoredPairings) {
    return false;
  }
  for (const base::Value& entry : pairings) {
    std::optional<std::string> json = base::WriteJson(entry);
    if (!entry.is_dict() || !json || json->size() > kMaxStoredPairingBytes) {
      return false;
    }
  }
  profile_->GetPrefs()->SetList(prefs::kNexusRemoteAccessPairings,
                                std::move(pairings));
  return true;
}

// ---- listener lifecycle ----------------------------------------------------

void NexusRemoteAccessService::SyncListenerToPrefs() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  PrefService* prefs = profile_->GetPrefs();
  const bool enabled = prefs->GetBoolean(prefs::kNexusRemoteAccessEnabled);
  const std::string bind_address =
      prefs->GetString(prefs::kNexusRemoteAccessBindAddress);
  const int port = prefs->GetInteger(prefs::kNexusRemoteAccessPort);

  if (!enabled) {
    StopListener(ListenerState::kOff, std::nullopt);
    return;
  }
  // An unusable config is a reported error, never a silent fallback.
  if (!IsBindableLiteral(bind_address)) {
    StopListener(ListenerState::kError, "no_address");
    return;
  }
  // Port 0 is the explicit OS-assigned-ephemeral request — accept it
  // through; any other out-of-range value is a config error.
  if (port != 0 && (port < kMinPort || port > kMaxPort)) {
    StopListener(ListenerState::kError, "bad_port");
    return;
  }

  // Already starting / listening on exactly this config: nothing to do. (A
  // restart would drop every client and mint a new control token for no
  // reason.) A run that ended in kError is retried — the port may be free now.
  const bool running = state_ == ListenerState::kStarting ||
                       state_ == ListenerState::kListening;
  if (running && bind_address == active_bind_address_ &&
      port == active_port_) {
    return;
  }
  StartListener(bind_address, port);
}

void NexusRemoteAccessService::StartListener(const std::string& bind_address,
                                             int port) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Mint a fresh memory-only control token for this run (32 bytes hex).
  std::array<uint8_t, 32> token_bytes;
  base::RandBytes(token_bytes);
  control_token_ = base::HexEncodeLower(token_bytes);

  ListenerConfig config;
  config.bind_address = bind_address;
  config.port = static_cast<uint16_t>(port);
  config.interface_version = kNexusInterfaceVersion;
  config.control_token = control_token_;
  // A wildcard (0.0.0.0) bind answers clients on every NIC: hand the Core the
  // machine's current IPv4 literals so its Host gate can recognise a request
  // that arrived on any of them (pushed, never pulled — the Core never calls
  // net::GetNetworkList, a blocking call, on its IO thread). The cache may be
  // empty this early; the listener still binds, the Host gate just matches
  // loopback-only until PushLocalAddresses hands it the full set — from
  // OnListenerStarted if the refresh landed during kStarting, else from
  // OnAddressesRefreshed. Loopback is always present (the runtime dials
  // 127.0.0.1).
  net::IPAddress bind_ip;
  if (bind_ip.AssignFromIPLiteral(bind_address) && bind_ip.IsZero()) {
    config.bind_is_wildcard = true;
    for (const BindAddressInfo& a : cached_addresses_) {
      config.local_addresses.push_back(a.address);
    }
    if (config.local_addresses.empty()) {
      config.local_addresses.push_back("127.0.0.1");
    }
  }
  // Debug builds relax the proxy to all GETs so an unbundled vite dev server
  // works as the upstream (plan §8.3's last routing row). Release builds only
  // ever serve the immutable entry / versioned assets / fixed root files.
#if !defined(NDEBUG)
  config.relaxed_proxy = true;
#endif

  active_bind_address_ = bind_address;
  active_port_ = port;
  bound_port_ = 0;
  bind_ip_wildcard_ = config.bind_is_wildcard;
  state_ = ListenerState::kStarting;
  error_.reset();
  // The listener stops a previous run before binding this one, and drops this
  // callback if a later Start/Stop supersedes it.
  listener_->Start(std::move(config),
                  base::BindOnce(&NexusRemoteAccessService::OnListenerStarted,
                                 weak_factory_.GetWeakPtr()));
  NotifyStatusChanged();
}

void NexusRemoteAccessService::OnListenerStarted(StartResult result,
                                                 uint16_t bound_port) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK_EQ(state_, ListenerState::kStarting);
  if (result != StartResult::kOk) {
    const char* error = "start_failed";
    if (result == StartResult::kPortInUse) {
      error = "port_in_use";
    } else if (result == StartResult::kAddressUnavailable) {
      error = "no_address";
    }
    StopListener(ListenerState::kError, error);
    return;
  }
  bound_port_ = bound_port;
  state_ = ListenerState::kListening;
  // A refresh that landed while this run was still kStarting cached the NIC
  // set but could not push it (the Core was not up), and every later refresh
  // of an unchanged set early-returns — so without this push a boot-time
  // auto-start could keep the loopback-only gate it was configured with and
  // 403 every LAN client until the NIC set next changed.
  PushLocalAddresses();
  // The runtime can only dial back through LNA once the grant is in place.
  ApplyLoopbackGrant(true);
  NotifyStatusChanged();
}

void NexusRemoteAccessService::StopListener(ListenerState state,
                                            std::optional<std::string> error) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  DCHECK(state == ListenerState::kOff || state == ListenerState::kError);
  const bool changed = state_ != state || error_ != error;
  // Only a deliberate OFF is final to the connected clients. An error state
  // (no_address, bad_port, a bind that failed) is a condition that can clear
  // — and the browser quitting does not come through here at all — so those
  // say going-away and let the page keep trying.
  listener_->Stop(state == ListenerState::kOff
                      ? NexusRemoteAccessListener::StopReason::kUserDisabled
                      : NexusRemoteAccessListener::StopReason::kGoingAway);
  bound_port_ = 0;
  control_token_.clear();
  active_bind_address_.clear();
  active_port_ = 0;
  bind_ip_wildcard_ = false;
  state_ = state;
  error_ = std::move(error);
  ApplyLoopbackGrant(false);
  if (changed) {
    NotifyStatusChanged();
  }
}

// ---- reverse proxy (UI thread) ---------------------------------------------

void NexusRemoteAccessService::HandleProxyRequest(
    std::string url_path,
    std::string if_none_match,
    std::string if_modified_since,
    NexusRemoteAccessListener::ProxyDoneCallback done) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  // Coalesce concurrent requests for the same path onto one upstream load.
  auto it = in_flight_proxy_.find(url_path);
  if (it != in_flight_proxy_.end()) {
    it->second->waiters.push_back(std::move(done));
    return;
  }

  // The ONLY upstream is the guest origin (§8.8). `url_path` is already
  // normalized + query-stripped by the router.
  const GURL upstream = GetResolvedNexusGuestURL(profile_).Resolve(url_path);

  auto resource_request = std::make_unique<network::ResourceRequest>();
  resource_request->url = upstream;
  resource_request->method = "GET";
  resource_request->credentials_mode =
      network::mojom::CredentialsMode::kOmit;  // no cookies (§8.8)
  // Forward only the revalidation headers.
  if (!if_none_match.empty()) {
    resource_request->headers.SetHeader(net::HttpRequestHeaders::kIfNoneMatch,
                                        if_none_match);
  }
  if (!if_modified_since.empty()) {
    resource_request->headers.SetHeader(
        net::HttpRequestHeaders::kIfModifiedSince, if_modified_since);
  }

  auto fetch = std::make_unique<ProxyFetch>();
  fetch->waiters.push_back(std::move(done));
  fetch->loader =
      network::SimpleURLLoader::Create(std::move(resource_request),
                                       kProxyTraffic);
  fetch->loader->SetAllowHttpErrorResults(true);  // surface 4xx/5xx as-is
  fetch->loader->DownloadToString(
      url_loader_factory_.get(),
      base::BindOnce(&NexusRemoteAccessService::OnProxyComplete,
                     weak_factory_.GetWeakPtr(), url_path, if_none_match,
                     if_modified_since),
      kMaxProxyBodyBytes);
  in_flight_proxy_.emplace(url_path, std::move(fetch));
}

void NexusRemoteAccessService::OnProxyComplete(
    const std::string& url_path,
    const std::string& if_none_match,
    const std::string& if_modified_since,
    std::optional<std::string> body) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  auto it = in_flight_proxy_.find(url_path);
  if (it == in_flight_proxy_.end()) {
    return;
  }
  std::unique_ptr<ProxyFetch> fetch = std::move(it->second);
  in_flight_proxy_.erase(it);

  int status = 502;  // upstream failure default → 502, no-store
  std::map<std::string, std::string> headers;
  std::string response_body;

  const network::mojom::URLResponseHead* head =
      fetch->loader->ResponseInfo();
  if (body.has_value() && head && head->headers) {
    status = head->headers->response_code();
    response_body = std::move(*body);
    // Pass through only the forwardable response headers (Content-Type,
    // Cache-Control, ETag, Last-Modified). Content-Encoding is dropped by
    // the listener (SimpleURLLoader already decompressed); Set-Cookie / HSTS
    // / Alt-Svc / hop-by-hop never cross.
    size_t iter = 0;
    std::string name;
    std::string value;
    while (head->headers->EnumerateHeaderLines(&iter, &name, &value)) {
      const std::string lower = base::ToLowerASCII(name);
      if (IsForwardableResponseHeader(lower)) {
        headers[lower] = value;
      }
    }
  } else {
    headers["cache-control"] = "no-store";
  }

  for (auto& waiter : fetch->waiters) {
    std::move(waiter).Run(status, headers, response_body);
  }
}

// ---- LNA grant ---------------------------------------------------------------

void NexusRemoteAccessService::ApplyLoopbackGrant(bool grant) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  HostContentSettingsMap* hcsm =
      HostContentSettingsMapFactory::GetForProfile(profile_);
  if (!hcsm) {
    return;
  }
  // Scope the grant to the ONE guest origin (spike S1: the grant must go
  // through HostContentSettingsMap; seeding the raw pref does NOT work, and
  // globally disabling LocalNetworkAccessChecks* is forbidden).
  const GURL guest = GetResolvedNexusGuestURL(profile_);
  const GURL origin = guest.DeprecatedGetOriginAsURL();
  if (!origin.is_valid()) {
    return;
  }
  const ContentSettingsPattern pattern =
      ContentSettingsPattern::FromURLToSchemefulSitePattern(origin);
  hcsm->SetContentSettingCustomScope(
      pattern, ContentSettingsPattern::Wildcard(),
      ContentSettingsType::LOOPBACK_NETWORK,
      grant ? CONTENT_SETTING_ALLOW : CONTENT_SETTING_DEFAULT);
}

// ---- network list ------------------------------------------------------------

// static
std::vector<BindAddressInfo> NexusRemoteAccessService::ComputeAddresses() {
  // Blocking (ScopedBlockingCall in GetNetworkList): runs on a MayBlock
  // worker via RequestAddressesRefresh, never on the UI thread.
  std::vector<BindAddressInfo> addresses;
  net::NetworkInterfaceList list;
  if (net::GetNetworkList(&list,
                          net::INCLUDE_HOST_SCOPE_VIRTUAL_INTERFACES)) {
    for (const auto& iface : list) {
      if (!iface.address.IsIPv4() || iface.address.IsZero()) {
        continue;
      }
      BindAddressInfo info;
      info.address = iface.address.ToString();
      info.interface_name = iface.friendly_name;
      addresses.push_back(std::move(info));
    }
  }
  // Always offer loopback for local debugging, first.
  BindAddressInfo loopback;
  loopback.address = "127.0.0.1";
  loopback.interface_name = "Loopback";
  addresses.insert(addresses.begin(), std::move(loopback));
  return addresses;
}

void NexusRemoteAccessService::RequestAddressesRefresh() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (addresses_refresh_in_flight_ || addresses_refresh_disabled_for_testing_) {
    return;
  }
  addresses_refresh_in_flight_ = true;
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN},
      base::BindOnce(&NexusRemoteAccessService::ComputeAddresses),
      base::BindOnce(&NexusRemoteAccessService::OnAddressesWorkerReply,
                     weak_factory_.GetWeakPtr()));
}

void NexusRemoteAccessService::OnAddressesWorkerReply(
    std::vector<BindAddressInfo> addresses) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  addresses_refresh_in_flight_ = false;
  if (addresses_refresh_disabled_for_testing_) {
    return;  // a test owns the address set; drop a reply already in flight
  }
  OnAddressesRefreshed(std::move(addresses));
}

void NexusRemoteAccessService::OnAddressesRefreshed(
    std::vector<BindAddressInfo> addresses) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (addresses == cached_addresses_) {
    return;
  }
  cached_addresses_ = std::move(addresses);
  // Under a wildcard bind the listener's Host gate matches against the NIC
  // set it was handed at Start — which may have been loopback-only if the
  // refresh had not landed yet. Push the now-current set so a remote on a
  // real NIC is recognised without waiting for the next configure.
  if (state_ == ListenerState::kListening) {
    PushLocalAddresses();
  }
  // Push so a subscriber sees the new list without polling getStatus.
  NotifyStatusChanged();
}

void NexusRemoteAccessService::PushLocalAddresses() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!bind_ip_wildcard_ || cached_addresses_.empty()) {
    return;
  }
  std::vector<std::string> locals;
  locals.reserve(cached_addresses_.size());
  for (const BindAddressInfo& a : cached_addresses_) {
    locals.push_back(a.address);
  }
  local_address_pushes_for_testing_.push_back(locals);
  listener_->SetLocalAddresses(std::move(locals));
}

}  // namespace nexus::remote_access
