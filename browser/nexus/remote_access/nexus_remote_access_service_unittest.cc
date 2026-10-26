// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_service.h"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>

#include "base/functional/callback_helpers.h"
#include "base/run_loop.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/test/bind.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "chrome/browser/prefs/browser_prefs.h"
#include "chrome/test/base/testing_profile.h"
#include "components/prefs/pref_service.h"
#include "components/sync_preferences/testing_pref_service_syncable.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_names.h"
#include "custom_browser/browser/nexus/api/llm/llm_pref_registration.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_clients.h"
#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_listener.h"
#include "custom_browser/browser/nexus/nexus_interface_version.h"
#include "net/base/address_list.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/base/network_handle.h"
#include "net/log/net_log_source.h"
#include "net/socket/tcp_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::remote_access {
namespace {

std::unique_ptr<TestingProfile> CreateProfile() {
  auto prefs =
      std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
  // TestingProfile::Init reads the standard profile pref set, so the supplied
  // store must carry it plus the nexus prefs (remote_access registers its
  // enabled/bind_address/port defaults there).
  RegisterUserProfilePrefs(prefs->registry());
  RegisterNexusProfilePrefs(prefs->registry());

  TestingProfile::Builder builder;
  builder.SetPrefService(std::move(prefs));
  // A regular TestingProfile would eagerly construct GlicKeyedService, which
  // crashes without a real ProfileManager. See nexus_test_support.h.
  test::AddGlicDisablingFactory(builder);
  return builder.Build();
}

class NexusRemoteAccessServiceTest : public testing::Test {
 protected:
  void RunUntilIdle() { task_environment_.RunUntilIdle(); }

  // The bind happens on the listener's own IO thread, which RunUntilIdle does
  // not see: wait for the status change that reports a settled state.
  RemoteAccessStatus WaitUntilSettled(NexusRemoteAccessService& service) {
    if (service.GetStatus().state == ListenerState::kStarting) {
      base::RunLoop loop;
      const uint64_t sink = service.AddStatusSink(base::BindLambdaForTesting(
          [&](const RemoteAccessStatus& status) {
            if (status.state != ListenerState::kStarting) {
              loop.Quit();
            }
          }));
      loop.Run();
      service.RemoveStatusSink(sink);
    }
    return service.GetStatus();
  }

 private:
  content::BrowserTaskEnvironment task_environment_{
      base::test::TaskEnvironment::TimeSource::MOCK_TIME};
};

// GetStatus must never populate addresses synchronously (it runs where
// blocking is disallowed): before the async refresh it returns the empty cache.
TEST_F(NexusRemoteAccessServiceTest, GetStatusReturnsCacheWithoutBlocking) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  // Constructing the service (the adapter-ctor path) must not have crashed or
  // blocked; the very first snapshot reads the cache, which is empty until the
  // posted refresh lands. (We do NOT RunUntilIdle before this call.)
  const RemoteAccessStatus first = service.GetStatus();
  EXPECT_FALSE(first.enabled);
  EXPECT_EQ(first.state, ListenerState::kOff);
  // The synchronous-path bug would have filled this inline; the cache is empty
  // until the worker reply runs.
  EXPECT_TRUE(first.addresses.empty());
}

// After the async refresh completes, the cache carries the bindable addresses
// with the documented invariants: loopback first, IPv4 literals only, 0.0.0.0
// excluded.
TEST_F(NexusRemoteAccessServiceTest, AddressesPopulatedAfterRefresh) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  // Let the MayBlock worker compute + the reply update the cache.
  RunUntilIdle();

  const RemoteAccessStatus status = service.GetStatus();
  ASSERT_FALSE(status.addresses.empty());
  // Loopback is always offered first (local debugging).
  EXPECT_EQ("127.0.0.1", status.addresses.front().address);
  for (const auto& entry : status.addresses) {
    net::IPAddress ip;
    ASSERT_TRUE(ip.AssignFromIPLiteral(entry.address));
    EXPECT_TRUE(ip.IsIPv4());
    EXPECT_FALSE(ip.IsZero());
  }
}

// Constructing + destroying the service with remote access OFF (the default)
// must be a clean, non-blocking round trip — this is exactly the cold-boot
// path that crashed.
TEST_F(NexusRemoteAccessServiceTest, ConstructAndTeardownWhenOff) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  EXPECT_EQ(service.GetStatus().state, ListenerState::kOff);
  RunUntilIdle();
  // Tear down the profile (and its user-data service) without hanging.
  profile.reset();
}

// WP-5 contract: configure with port 0 is an explicit request for an
// OS-assigned ephemeral port — NOT a bad_port. The bind is asynchronous (the
// UI thread never waits on the IO thread), so Configure answers 'starting' and
// the outcome arrives as a status change: 'listening' with the ACTUAL bound
// port echoed back (non-zero), while the configured pref stays 0. This is the
// e2e specs' enableListener path, which polls
// remote.setAccess{enabled:true, bindAddress:'127.0.0.1', port:0} until it
// reads 'listening'.
TEST_F(NexusRemoteAccessServiceTest, ConfigurePortZeroBindsEphemeralAndEchoesIt) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  const RemoteAccessStatus starting =
      service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1",
                        /*port=*/0);
  // The whole point: port 0 must NOT be rejected as bad_port.
  EXPECT_EQ(starting.state, ListenerState::kStarting);
  EXPECT_FALSE(starting.error.has_value());
  // No token and no URL before there is something to dial.
  EXPECT_FALSE(starting.control_url.has_value());
  EXPECT_FALSE(starting.control_token.has_value());

  const RemoteAccessStatus status = WaitUntilSettled(service);
  EXPECT_EQ(status.state, ListenerState::kListening);
  EXPECT_FALSE(status.error.has_value());
  // The ACTUAL bound port is echoed back, and it is a real (non-zero) port.
  EXPECT_GT(status.port, 0);
  // The control URL is built from the same bound port.
  ASSERT_TRUE(status.control_url.has_value());
  EXPECT_NE(status.control_url->find(base::NumberToString(status.port)),
            std::string::npos);
  EXPECT_TRUE(status.control_token.has_value());

  // The configured pref remains 0 (ephemeral is a per-start choice, not a
  // persisted port); only the snapshot surfaces the bound port.
  EXPECT_EQ(profile->GetPrefs()->GetInteger(prefs::kNexusRemoteAccessPort), 0);

  service.Configure(/*enabled=*/false, std::nullopt, std::nullopt);
  EXPECT_EQ(service.GetStatus().state, ListenerState::kOff);
}

// Re-sending the configure the listener is already running on must change
// NOTHING: same port, same control token, no restart. The e2e specs poll
// setAccess until 'listening', so every poll after the first is exactly this
// call. (The earlier code started a second listener beside the live one on
// every configure — on a fixed port that reported port_in_use against itself,
// and on port 0 it hopped ports and rotated the token under the runtime.)
TEST_F(NexusRemoteAccessServiceTest, ReconfiguringTheSameConfigIsANoOp) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1", /*port=*/0);
  const RemoteAccessStatus first = WaitUntilSettled(service);
  ASSERT_EQ(first.state, ListenerState::kListening);

  const RemoteAccessStatus again =
      service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1",
                        /*port=*/0);
  EXPECT_EQ(again.state, ListenerState::kListening);
  EXPECT_EQ(again.port, first.port);
  EXPECT_EQ(again.control_token, first.control_token);
}

// A fixed (non-zero) port someone ELSE holds must STILL error port_in_use and
// never silently switch — the rule the ephemeral case must not weaken.
TEST_F(NexusRemoteAccessServiceTest, FixedPortInUseStillErrorsPortInUse) {
  // Someone else: an independent listener squatting on an ephemeral port.
  NexusRemoteAccessListener squatter(base::DoNothing());
  base::test::TestFuture<StartResult, uint16_t> squatting;
  ListenerConfig squat_config;
  squat_config.bind_address = "127.0.0.1";
  squatter.Start(squat_config, squatting.GetCallback());
  ASSERT_EQ(squatting.Get<0>(), StartResult::kOk);
  const int occupied = squatting.Get<1>();

  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1",
                    /*port=*/occupied);
  const RemoteAccessStatus conflict = WaitUntilSettled(service);
  EXPECT_EQ(conflict.state, ListenerState::kError);
  ASSERT_TRUE(conflict.error.has_value());
  EXPECT_EQ(*conflict.error, "port_in_use");
  // It did not hop: the reported port is still the configured one.
  EXPECT_EQ(conflict.port, occupied);
}

// An address this machine does not have is reported as no_address — learned
// from the bind, never by enumerating interfaces on the UI thread (the old
// IsBindableAddress called net::GetNetworkList right here, a blocking call
// that FATALs inside the startup DisallowUnresponsiveTasks scope for any
// non-loopback address).
TEST_F(NexusRemoteAccessServiceTest, AddressTheMachineLacksIsNoAddress) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  base::DisallowUnresponsiveTasks();
  service.Configure(/*enabled=*/true, /*bind_address=*/"192.0.2.1",
                    /*port=*/0);
  base::internal::ResetThreadRestrictionsForTesting();

  const RemoteAccessStatus status = WaitUntilSettled(service);
  EXPECT_EQ(status.state, ListenerState::kError);
  ASSERT_TRUE(status.error.has_value());
  EXPECT_EQ(*status.error, "no_address");

  // IPv6 "::" is still refused outright, without a bind (the listener is
  // IPv4-only; the wildcard that IS accepted is 0.0.0.0 — see the wildcard
  // test below).
  const RemoteAccessStatus v6 = service.Configure(
      /*enabled=*/true, /*bind_address=*/"::", /*port=*/0);
  EXPECT_EQ(v6.state, ListenerState::kError);
  EXPECT_EQ(v6.error.value_or(""), "no_address");
}

// A real proxy-routed HTTP request, end to end through the service. The
// upstream answer depends on whether a dev server is running, so either a 502
// or a forwarded response passes; the guard is that nothing crashes.
TEST_F(NexusRemoteAccessServiceTest, ProxyRequestIsAnsweredNotCrashed) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1", /*port=*/0);
  const RemoteAccessStatus status = WaitUntilSettled(service);
  ASSERT_EQ(status.state, ListenerState::kListening);
  const uint16_t port = static_cast<uint16_t>(status.port);

  auto sync = [](auto op) {
    base::RunLoop loop;
    int result = net::ERR_IO_PENDING;
    const int rv = op(base::BindLambdaForTesting([&](int r) {
      result = r;
      loop.Quit();
    }));
    if (rv != net::ERR_IO_PENDING) {
      return rv;
    }
    loop.Run();
    return result;
  };

  net::TCPClientSocket socket(
      net::AddressList(net::IPEndPoint(net::IPAddress::IPv4Localhost(), port)),
      nullptr, nullptr, nullptr, net::NetLogSource(),
      net::handles::kInvalidNetworkHandle);
  ASSERT_EQ(net::OK, sync([&](net::CompletionOnceCallback cb) {
              return socket.Connect(std::move(cb));
            }));
  // A path proxyable in Release too (the versioned-asset shape).
  const std::string request =
      "GET /v" + base::NumberToString(kNexusInterfaceVersion) +
      "/assets/app.js HTTP/1.1\r\nHost: 127.0.0.1:" +
      base::NumberToString(port) + "\r\n\r\n";
  auto write_buffer = base::MakeRefCounted<net::StringIOBuffer>(request);
  ASSERT_EQ(static_cast<int>(request.size()),
            sync([&](net::CompletionOnceCallback cb) {
              return socket.Write(write_buffer.get(), request.size(),
                                  std::move(cb), TRAFFIC_ANNOTATION_FOR_TESTS);
            }));

  std::string response;
  auto read_buffer = base::MakeRefCounted<net::IOBufferWithSize>(4096);
  while (response.find("\r\n\r\n") == std::string::npos) {
    const int read = sync([&](net::CompletionOnceCallback cb) {
      return socket.Read(read_buffer.get(), read_buffer->size(), std::move(cb));
    });
    if (read <= 0) {
      break;
    }
    response.append(read_buffer->data(), static_cast<size_t>(read));
  }
  EXPECT_NE(response.find("HTTP/1.1 "), std::string::npos) << response;
  // Two valid outcomes: the proxy's own "502" + "no-store", or a dev server's
  // response with a Content-Type. Match case-insensitively (forwarded names are
  // lowercased, SetBody's "Content-Type:" is not).
  const std::string lower = base::ToLowerASCII(response);
  const bool proxy_502 = lower.find("no-store") != std::string::npos;
  const bool proxied_upstream =
      lower.find("content-type:") != std::string::npos;
  EXPECT_TRUE(proxy_502 || proxied_upstream) << response;
}

// A subscriber must not keep the address refresh spinning. The earlier event
// path built its payload with GetStatus(), which itself schedules a refresh,
// whose completion pushed another event: an endless worker-task + event loop
// for as long as anyone was subscribed.
TEST_F(NexusRemoteAccessServiceTest, AddressRefreshDoesNotFeedItself) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  int events = 0;
  const uint64_t sink = service.AddStatusSink(base::BindLambdaForTesting(
      [&](const RemoteAccessStatus&) { ++events; }));
  // Would never return while the loop existed.
  RunUntilIdle();
  // At most the one push for the first populated list.
  EXPECT_LE(events, 1);

  // An unchanged refresh pushes nothing.
  events = 0;
  service.GetStatus();
  RunUntilIdle();
  EXPECT_EQ(events, 0);
  service.RemoveStatusSink(sink);
}

// ---- pairing persistence ---------------------------------------------------

// A pairing written through SetPairings round-trips verbatim through the
// profile pref store (kNexusRemoteAccessPairings) — this is the on-disk half
// of "a minted link keeps working across a desktop restart". The runtime owns
// the schema; C++ stores the list opaquely, so the assertion is field-for-field
// equality, including the plaintext pairSecret.
TEST_F(NexusRemoteAccessServiceTest, PairingsRoundTripThroughThePrefStore) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  base::DictValue scope;
  scope.Set("mode", "control");
  scope.Set("sessions", "all");
  base::DictValue pairing;
  pairing.Set("pairId", "abc123XYZ-_");
  pairing.Set("secret", "c2VjcmV0LWJhc2U2NHVybC0zMmJ5dGVzLXBhZGRlZA");
  pairing.Set("scope", scope.Clone());
  pairing.Set("label", "My laptop");
  pairing.Set("createdAt", 1758268800000.0);
  pairing.Set("expiresAt", 1758355200000.0);
  pairing.Set("revoked", false);
  pairing.Set("lastSeenAt", 1758270000000.0);

  base::ListValue list;
  list.Append(pairing.Clone());
  service.SetPairings(std::move(list));

  const base::ListValue& stored = service.GetPairings();
  ASSERT_EQ(stored.size(), 1u);
  EXPECT_EQ(stored[0], base::Value(pairing.Clone()));

  // And it landed in the pref itself (not a service-side shadow).
  const base::ListValue& pref =
      profile->GetPrefs()->GetList(prefs::kNexusRemoteAccessPairings);
  ASSERT_EQ(pref.size(), 1u);
  EXPECT_EQ(pref[0], base::Value(std::move(pairing)));
}

// The list is opaque but bounded: an oversized write is refused whole and the
// stored list is left exactly as it was.
TEST_F(NexusRemoteAccessServiceTest, OversizedPairingsAreRefusedUnwritten) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  base::DictValue kept;
  kept.Set("pairId", "kept");
  base::ListValue good;
  good.Append(kept.Clone());
  ASSERT_TRUE(service.SetPairings(std::move(good)));

  base::ListValue too_many;
  for (size_t i = 0; i <= NexusRemoteAccessService::kMaxStoredPairings; ++i) {
    too_many.Append(kept.Clone());
  }
  EXPECT_FALSE(service.SetPairings(std::move(too_many)));

  base::DictValue fat;
  fat.Set("label", std::string(
                       NexusRemoteAccessService::kMaxStoredPairingBytes, 'x'));
  base::ListValue too_fat;
  too_fat.Append(std::move(fat));
  EXPECT_FALSE(service.SetPairings(std::move(too_fat)));

  base::ListValue not_dicts;
  not_dicts.Append("a string");
  EXPECT_FALSE(service.SetPairings(std::move(not_dicts)));

  const base::ListValue& stored = service.GetPairings();
  ASSERT_EQ(stored.size(), 1u);
  EXPECT_EQ(stored[0], base::Value(std::move(kept)));
}

// The default pref is an empty list — a fresh profile has no pairings.
TEST_F(NexusRemoteAccessServiceTest, PairingsDefaultEmpty) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  EXPECT_TRUE(service.GetPairings().empty());
}

// ---- auto-start across a restart -------------------------------------------

// Build a profile whose pref store is pre-seeded with remote-access prefs —
// exactly what a browser restart hands the service constructor.
std::unique_ptr<TestingProfile> CreateProfileWithRemoteAccessPrefs(
    bool enabled,
    const std::string& bind_address,
    int port) {
  auto prefs =
      std::make_unique<sync_preferences::TestingPrefServiceSyncable>();
  RegisterUserProfilePrefs(prefs->registry());
  RegisterNexusProfilePrefs(prefs->registry());
  TestingProfile::Builder builder;
  builder.SetPrefService(std::move(prefs));
  test::AddGlicDisablingFactory(builder);
  std::unique_ptr<TestingProfile> profile = builder.Build();
  profile->GetPrefs()->SetBoolean(prefs::kNexusRemoteAccessEnabled, enabled);
  profile->GetPrefs()->SetString(prefs::kNexusRemoteAccessBindAddress,
                                 bind_address);
  profile->GetPrefs()->SetInteger(prefs::kNexusRemoteAccessPort, port);
  return profile;
}

// The whole point of persisting the enabled flag + port: a service constructed
// on already-configured prefs (the browser-restart path, where BrowserMainLoop
// builds the profile's service with no Configure call) must bring the listener
// up on its own. Constructing it is the trigger — SyncListenerToPrefs runs in
// the ctor — so the test only has to build the service and wait for the bind.
TEST_F(NexusRemoteAccessServiceTest, ListenerAutoStartsOnBootWhenEnabled) {
  const int fixed_port = 47617;
  std::unique_ptr<TestingProfile> profile =
      CreateProfileWithRemoteAccessPrefs(true, "127.0.0.1", fixed_port);

  // No Configure — construction alone must start the listener.
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  const RemoteAccessStatus booted = WaitUntilSettled(service);
  EXPECT_EQ(booted.state, ListenerState::kListening);
  EXPECT_EQ(booted.port, fixed_port);
  EXPECT_TRUE(booted.enabled);
  EXPECT_TRUE(booted.control_token.has_value());
}

// The paired negative: a restart with enabled=false must NOT start the
// listener (default-OFF is the security model).
TEST_F(NexusRemoteAccessServiceTest, ListenerStaysOffOnBootWhenDisabled) {
  std::unique_ptr<TestingProfile> profile =
      CreateProfileWithRemoteAccessPrefs(false, "127.0.0.1", 47619);
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  RunUntilIdle();
  EXPECT_EQ(service.GetStatus().state, ListenerState::kOff);
}

// ---- wildcard (0.0.0.0) bind -------------------------------------------------

// The wildcard 0.0.0.0 is a bindable literal (the JS layer's
// default; C++ is a pure executor and merely accepts it). The service must NOT
// reject it as no_address, must bind, and — because 0.0.0.0 is not dialable —
// must hand the runtime a control URL on 127.0.0.1 (which always passes the
// /host/* loopback gate). The status bind_address still reports the configured
// wildcard.
TEST_F(NexusRemoteAccessServiceTest, WildcardBindIsAcceptedAndDialsLoopback) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());

  const RemoteAccessStatus starting =
      service.Configure(/*enabled=*/true, /*bind_address=*/"0.0.0.0",
                        /*port=*/0);
  // Not rejected as no_address / bad_port — the bind proceeds.
  EXPECT_EQ(starting.state, ListenerState::kStarting);
  EXPECT_FALSE(starting.error.has_value());

  const RemoteAccessStatus status = WaitUntilSettled(service);
  ASSERT_EQ(status.state, ListenerState::kListening);
  EXPECT_FALSE(status.error.has_value());
  // The configured literal is reported as-is.
  EXPECT_EQ(status.bind_address, "0.0.0.0");
  // But the runtime dials loopback, never the wildcard.
  ASSERT_TRUE(status.control_url.has_value());
  EXPECT_EQ(status.control_url->find("ws://127.0.0.1:"), 0u) << *status.control_url;
  EXPECT_EQ(status.control_url->find("0.0.0.0"), std::string::npos)
      << *status.control_url;

  service.Configure(/*enabled=*/false, std::nullopt, std::nullopt);
  EXPECT_EQ(service.GetStatus().state, ListenerState::kOff);
}

// The restart path with sharing on: the ctor posts the address refresh and
// then auto-starts the listener, so the refresh can land while the run is
// still kStarting. It caches the NIC set but cannot push it (no Core yet), and
// every later refresh of the same set early-returns as unchanged — so the push
// has to happen when the listener comes up, or a LAN client's
// `Host: <nic-ip>:<port>` is 403 until the NIC set next changes.
TEST_F(NexusRemoteAccessServiceTest,
       WildcardPushesAddressesThatLandedWhileStarting) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  // Let the ctor's own refresh land first, so the only refresh inside the
  // kStarting window is the one placed there below.
  RunUntilIdle();

  const RemoteAccessStatus starting =
      service.Configure(/*enabled=*/true, /*bind_address=*/"0.0.0.0",
                        /*port=*/0);
  ASSERT_EQ(starting.state, ListenerState::kStarting);

  // 192.0.2.0/24 is TEST-NET-1: never a real NIC, so only this refresh can
  // have put it in a pushed set.
  BindAddressInfo loopback;
  loopback.address = "127.0.0.1";
  loopback.interface_name = "Loopback";
  BindAddressInfo lan;
  lan.address = "192.0.2.7";
  lan.interface_name = "TestNet";
  service.LandAddressesRefreshForTesting({loopback, lan});
  ASSERT_EQ(service.GetStatus().state, ListenerState::kStarting);

  ASSERT_EQ(WaitUntilSettled(service).state, ListenerState::kListening);
  bool pushed = false;
  for (const std::vector<std::string>& set :
       service.local_address_pushes_for_testing()) {
    pushed = pushed || std::ranges::contains(set, "192.0.2.7");
  }
  EXPECT_TRUE(pushed)
      << "the NIC set cached during kStarting never reached the Host gate";

  service.Configure(/*enabled=*/false, std::nullopt, std::nullopt);
}

// A single-literal bind has no NIC set to match against: nothing is pushed.
TEST_F(NexusRemoteAccessServiceTest, LiteralBindPushesNoAddresses) {
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  RunUntilIdle();
  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1",
                    /*port=*/0);
  ASSERT_EQ(WaitUntilSettled(service).state, ListenerState::kListening);
  EXPECT_TRUE(service.local_address_pushes_for_testing().empty());
  service.Configure(/*enabled=*/false, std::nullopt, std::nullopt);
}

// ---- connected devices -------------------------------------------------------

// A device connecting is a status change: a status event carries the new
// count, and the process-wide summary the tray and the quit confirmation read
// follows it. Real sockets end to end: the runtime's control
// socket, then a browser client's /rt.
TEST_F(NexusRemoteAccessServiceTest, AConnectedDeviceReachesStatusAndTheTray) {
  ResetConnectedClientsForTesting();
  std::unique_ptr<TestingProfile> profile = CreateProfile();
  NexusRemoteAccessService& service = GetRemoteAccessService(profile.get());
  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1", /*port=*/0);
  const RemoteAccessStatus status = WaitUntilSettled(service);
  ASSERT_EQ(status.state, ListenerState::kListening);
  ASSERT_TRUE(status.control_token.has_value());
  const uint16_t port = static_cast<uint16_t>(status.port);
  EXPECT_EQ(GetConnectedClients(),
            (ConnectedClients{.listening = true, .clients = 0}));

  auto sync = [](auto op) {
    base::RunLoop loop;
    int result = net::ERR_IO_PENDING;
    const int rv = op(base::BindLambdaForTesting([&](int r) {
      result = r;
      loop.Quit();
    }));
    if (rv != net::ERR_IO_PENDING) {
      return rv;
    }
    loop.Run();
    return result;
  };
  // An upgraded WebSocket on `target`, read up to the end of the 101.
  auto open_websocket = [&](const std::string& target,
                            const std::string& origin) {
    auto socket = std::make_unique<net::TCPClientSocket>(
        net::AddressList(
            net::IPEndPoint(net::IPAddress::IPv4Localhost(), port)),
        nullptr, nullptr, nullptr, net::NetLogSource(),
        net::handles::kInvalidNetworkHandle);
    EXPECT_EQ(net::OK, sync([&](net::CompletionOnceCallback cb) {
                return socket->Connect(std::move(cb));
              }));
    const std::string request =
        "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
        base::NumberToString(port) +
        (origin.empty() ? std::string() : "\r\nOrigin: " + origin) +
        "\r\nConnection: Upgrade\r\nUpgrade: websocket"
        "\r\nSec-WebSocket-Version: 13"
        "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    auto write_buffer = base::MakeRefCounted<net::StringIOBuffer>(request);
    EXPECT_EQ(static_cast<int>(request.size()),
              sync([&](net::CompletionOnceCallback cb) {
                return socket->Write(write_buffer.get(), request.size(),
                                     std::move(cb),
                                     TRAFFIC_ANNOTATION_FOR_TESTS);
              }));
    std::string response;
    auto read_buffer = base::MakeRefCounted<net::IOBufferWithSize>(4096);
    while (response.find("\r\n\r\n") == std::string::npos) {
      const int read = sync([&](net::CompletionOnceCallback cb) {
        return socket->Read(read_buffer.get(), read_buffer->size(),
                            std::move(cb));
      });
      if (read <= 0) {
        break;
      }
      response.append(read_buffer->data(), static_cast<size_t>(read));
    }
    EXPECT_NE(response.find(" 101 "), std::string::npos) << response;
    return socket;
  };
  // Waits for a status event carrying `clients`. The event can already have
  // run while `open_websocket` spun its own loops, so look first.
  auto wait_for_clients = [&](int clients) {
    if (service.GetStatus().clients == clients) {
      return;
    }
    base::RunLoop loop;
    const uint64_t sink = service.AddStatusSink(base::BindLambdaForTesting(
        [&](const RemoteAccessStatus& event) {
          if (event.clients == clients) {
            loop.Quit();
          }
        }));
    loop.Run();
    service.RemoveStatusSink(sink);
  };

  auto control =
      open_websocket("/host/control?token=" + *status.control_token, "");
  auto device =
      open_websocket("/rt", "http://127.0.0.1:" + base::NumberToString(port));
  wait_for_clients(1);
  EXPECT_EQ(service.GetStatus().clients, 1);
  EXPECT_EQ(GetConnectedClients(),
            (ConnectedClients{.listening = true, .clients = 1}));

  device.reset();
  wait_for_clients(0);
  EXPECT_EQ(GetConnectedClients().clients, 0);

  // Off: nothing listening, nobody connected.
  service.Configure(/*enabled=*/false, std::nullopt, std::nullopt);
  EXPECT_EQ(GetConnectedClients(), ConnectedClients());

  // The profile going away takes its entry with it.
  service.Configure(/*enabled=*/true, /*bind_address=*/"127.0.0.1", /*port=*/0);
  WaitUntilSettled(service);
  EXPECT_TRUE(GetConnectedClients().listening);
  profile.reset();
  EXPECT_EQ(GetConnectedClients(), ConnectedClients());
}

}  // namespace
}  // namespace nexus::remote_access
