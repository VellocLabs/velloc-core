// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/nexus_remote_access_listener.h"

#include <map>
#include <memory>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/functional/callback_helpers.h"
#include "base/location.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/run_loop.h"
#include "base/sequence_checker.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/test/bind.h"
#include "base/test/task_environment.h"
#include "base/test/test_future.h"
#include "base/threading/thread_restrictions.h"
#include "base/time/time.h"
#include "net/base/address_list.h"
#include "net/base/io_buffer.h"
#include "net/base/ip_address.h"
#include "net/base/ip_endpoint.h"
#include "net/base/net_errors.h"
#include "net/log/net_log_source.h"
#include "net/server/web_socket_encoder.h"
#include "net/socket/tcp_client_socket.h"
#include "net/traffic_annotation/network_traffic_annotation_test_helper.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::remote_access {
namespace {

// Stands in for NexusRemoteAccessService, with the SAME shape that crashed:
// an owner-sequence object handing the listener a callback bound to its own
// WeakPtr. Dereferencing that WeakPtr — or entering Handle() — anywhere but
// the owner's sequence is a FATAL in a DCHECK build.
class FakeOwner {
 public:
  NexusRemoteAccessListener::ProxyCallback Callback() {
    return base::BindRepeating(&FakeOwner::Handle, weak_factory_.GetWeakPtr());
  }

  // Bytes of filler the reply carries ahead of its marker.
  void set_padding_bytes(size_t bytes) { padding_bytes_ = bytes; }
  int calls() const { return calls_; }
  const std::string& last_path() const { return last_path_; }

 private:
  void Handle(std::string url_path,
              std::string /*if_none_match*/,
              std::string /*if_modified_since*/,
              NexusRemoteAccessListener::ProxyDoneCallback done) {
    DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
    ++calls_;
    last_path_ = std::move(url_path);
    std::move(done).Run(200, {{"content-type", "text/plain"}},
                        std::string(padding_bytes_, '.') + "hello-from-owner");
  }

  int calls_ = 0;
  size_t padding_bytes_ = 0;
  std::string last_path_;
  SEQUENCE_CHECKER(sequence_checker_);
  base::WeakPtrFactory<FakeOwner> weak_factory_{this};
};

ListenerConfig LoopbackConfig(uint16_t port = 0) {
  ListenerConfig config;
  config.bind_address = "127.0.0.1";
  config.port = port;
  config.interface_version = 1;
  config.control_token = std::string(64, 'a');
  // Every GET proxies, so the request below reaches the ProxyCallback whatever
  // the versioned-path table says.
  config.relaxed_proxy = true;
  return config;
}

class NexusRemoteAccessListenerTest : public testing::Test {
 protected:
  using Started = std::tuple<StartResult, uint16_t>;

  Started StartAndWait(NexusRemoteAccessListener& listener,
                       ListenerConfig config) {
    base::test::TestFuture<StartResult, uint16_t> started;
    listener.Start(std::move(config), started.GetCallback());
    return started.Take();
  }

  // Runs one net operation to completion on the test's (IO) main thread.
  template <typename Op>
  int Sync(Op op) {
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
  }

  std::unique_ptr<net::TCPClientSocket> Connect(uint16_t port) {
    auto socket = std::make_unique<net::TCPClientSocket>(
        net::AddressList(net::IPEndPoint(net::IPAddress::IPv4Localhost(), port)),
        nullptr, nullptr, nullptr, net::NetLogSource());
    EXPECT_EQ(net::OK, Sync([&](net::CompletionOnceCallback cb) {
                return socket->Connect(std::move(cb));
              }));
    return socket;
  }

  void WriteAll(net::TCPClientSocket& socket, const std::string& bytes) {
    auto buffer = base::MakeRefCounted<net::StringIOBuffer>(bytes);
    EXPECT_EQ(static_cast<int>(bytes.size()),
              Sync([&](net::CompletionOnceCallback cb) {
                return socket.Write(buffer.get(), bytes.size(), std::move(cb),
                                    TRAFFIC_ANNOTATION_FOR_TESTS);
              }));
  }

  // Everything read until `until` shows up (or the peer closes).
  std::string ReadUntil(net::TCPClientSocket& socket, const std::string& until) {
    std::string response;
    auto read_buffer = base::MakeRefCounted<net::IOBufferWithSize>(4096);
    while (response.find(until) == std::string::npos) {
      const int read = Sync([&](net::CompletionOnceCallback cb) {
        return socket.Read(read_buffer.get(), read_buffer->size(),
                           std::move(cb));
      });
      if (read <= 0) {
        break;
      }
      response.append(read_buffer->data(), static_cast<size_t>(read));
    }
    return response;
  }

  // Sends one real HTTP GET over a real TCP connection and returns everything
  // read until `until` shows up (or the peer closes).
  std::string HttpGet(uint16_t port,
                      const std::string& path,
                      const std::string& until) {
    std::unique_ptr<net::TCPClientSocket> socket = Connect(port);
    WriteAll(*socket, "GET " + path + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                          base::NumberToString(port) + "\r\n\r\n");
    return ReadUntil(*socket, until);
  }

  // A connected, upgraded WebSocket on `target` — the runtime's own half of
  // the /host/* protocol, over a real socket. The caller KEEPS IT ALIVE: the
  // listener's control state lives and dies with this connection.
  // `origin` non-empty adds an `Origin:` header — a browser client's /rt
  // upgrade carries one, and the listener refuses /rt without it.
  std::unique_ptr<net::TCPClientSocket> OpenWebSocket(
      uint16_t port,
      const std::string& target,
      const std::string& origin = std::string()) {
    std::unique_ptr<net::TCPClientSocket> socket = Connect(port);
    WriteAll(*socket,
             "GET " + target + " HTTP/1.1\r\nHost: 127.0.0.1:" +
                 base::NumberToString(port) +
                 (origin.empty() ? std::string() : "\r\nOrigin: " + origin) +
                 "\r\nConnection: Upgrade\r\nUpgrade: websocket"
                 "\r\nSec-WebSocket-Version: 13"
                 "\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n");
    const std::string response = ReadUntil(*socket, "\r\n\r\n");
    EXPECT_NE(response.find(" 101 "), std::string::npos) << response;
    return socket;
  }

  // One text frame from the CLIENT side, so it is masked (RFC 6455 section
  // 5.1: a client always masks, a server never does).
  void SendText(net::TCPClientSocket& socket, const std::string& text) {
    std::string frame;
    net::WebSocketEncoder::CreateClient(std::string())
        ->EncodeTextFrame(text, 0x11223344, &frame);
    WriteAll(socket, frame);
  }

  // Everything the peer sends until it closes the socket — how a close FRAME
  // is collected (ReadUntil needs a sentinel the binary frame does not have).
  std::string ReadUntilClosed(net::TCPClientSocket& socket) {
    std::string bytes;
    auto read_buffer = base::MakeRefCounted<net::IOBufferWithSize>(4096);
    while (true) {
      const int read = Sync([&](net::CompletionOnceCallback cb) {
        return socket.Read(read_buffer.get(), read_buffer->size(),
                           std::move(cb));
      });
      if (read <= 0) {
        break;
      }
      bytes.append(read_buffer->data(), static_cast<size_t>(read));
    }
    return bytes;
  }

  // `GET /` until it stops being the 503 — the control frames above are
  // processed on the listener's own IO thread, which this test cannot join, so
  // poll for the outcome instead of racing it. Returns the last response
  // either way, so a failure prints what actually came back.
  std::string GetRootUntilRedirect(uint16_t port) {
    std::string response;
    for (int attempt = 0; attempt < 100; ++attempt) {
      response = HttpGet(port, "/", "\r\n\r\n");
      if (response.find(" 302 ") != std::string::npos) {
        break;
      }
      base::RunLoop loop;
      base::SequencedTaskRunner::GetCurrentDefault()->PostDelayedTask(
          FROM_HERE, loop.QuitClosure(), base::Milliseconds(10));
      loop.Run();
    }
    return response;
  }

  // A full TaskEnvironment: the listener joins its IO thread on the thread
  // pool. The main thread is IO-capable for the test's own client socket.
  base::test::TaskEnvironment task_environment_{
      base::test::TaskEnvironment::MainThreadType::IO};
};

TEST_F(NexusRemoteAccessListenerTest, StartBindsAndReportsThePort) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  // Port 0 → the OS assigns a free port; Start reports the BOUND one.
  const auto [result, port] = StartAndWait(listener, LoopbackConfig());
  EXPECT_EQ(result, StartResult::kOk);
  EXPECT_NE(port, 0);
  EXPECT_EQ(listener.client_count(), 0u);
}

TEST_F(NexusRemoteAccessListenerTest, StartOnAnOccupiedPortIsPortInUse) {
  FakeOwner owner;
  NexusRemoteAccessListener first(owner.Callback());
  const auto [first_result, first_port] = StartAndWait(first, LoopbackConfig());
  ASSERT_EQ(first_result, StartResult::kOk);

  // A second listener on the SAME port must fail with port_in_use — never
  // silently switch ports (the minted link would die).
  NexusRemoteAccessListener second(owner.Callback());
  const auto [result, port] = StartAndWait(second, LoopbackConfig(first_port));
  EXPECT_EQ(result, StartResult::kPortInUse);
  EXPECT_EQ(port, 0u);
}

TEST_F(NexusRemoteAccessListenerTest, StartWithANonLiteralAddressFails) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.bind_address = "not-an-ip";
  const auto [result, port] = StartAndWait(listener, std::move(config));
  EXPECT_EQ(result, StartResult::kFailed);
  EXPECT_EQ(port, 0u);
}

// "This machine does not have that address" is learned from the bind itself,
// so nobody has to enumerate interfaces (a blocking call) to pre-validate.
TEST_F(NexusRemoteAccessListenerTest, StartOnAnAddressTheMachineLacks) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.bind_address = "192.0.2.1";  // TEST-NET-1 (RFC 5737): never local
  const auto [result, port] = StartAndWait(listener, std::move(config));
  EXPECT_EQ(result, StartResult::kAddressUnavailable);
  EXPECT_EQ(port, 0u);
}

TEST_F(NexusRemoteAccessListenerTest, StopIsIdempotentAndSafeWhenNotStarted) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  listener.Stop(NexusRemoteAccessListener::StopReason::kGoingAway);
  // before any Start: a no-op, not a crash

  ASSERT_EQ(std::get<0>(StartAndWait(listener, LoopbackConfig())),
            StartResult::kOk);
  listener.Stop(NexusRemoteAccessListener::StopReason::kUserDisabled);
  listener.Stop(NexusRemoteAccessListener::StopReason::kGoingAway);
}

// A restart stops the previous run FIRST, on the same IO sequence, so moving
// back onto the port the previous run held cannot collide with itself. (The
// synchronous design reported port_in_use here: it bound the new listener
// while the old one still held the port.)
TEST_F(NexusRemoteAccessListenerTest, RestartOntoItsOwnPortSucceeds) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  const auto [first_result, first_port] =
      StartAndWait(listener, LoopbackConfig());
  ASSERT_EQ(first_result, StartResult::kOk);

  const auto [result, port] =
      StartAndWait(listener, LoopbackConfig(first_port));
  EXPECT_EQ(result, StartResult::kOk);
  EXPECT_EQ(port, first_port);
}

TEST_F(NexusRemoteAccessListenerTest, ASupersededStartNeverReports) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  bool stale_ran = false;
  listener.Start(LoopbackConfig(),
                 base::BindLambdaForTesting(
                     [&](StartResult, uint16_t) { stale_ran = true; }));
  const auto [result, port] = StartAndWait(listener, LoopbackConfig());
  EXPECT_EQ(result, StartResult::kOk);
  task_environment_.RunUntilIdle();
  EXPECT_FALSE(stale_ran);
}

// THE regression test for the WP-5 crash #4 (FATAL sequence_checker.cc:
// WeakPtr<NexusRemoteAccessService>::get() under
// NexusRemoteAccessListener::OnHttpRequest on the NexusRemoteAccessIO thread).
// Start/Stop never hit it — it takes a REAL proxy-routed HTTP request. The
// owner's callback is bound to an owner-sequence WeakPtr, exactly as the
// service binds it; if the IO thread ever runs it directly again, FakeOwner's
// WeakPtr / sequence checker FATALs this test. The reply must then make it
// back to the IO thread and out on the wire.
TEST_F(NexusRemoteAccessListenerTest, ProxyRequestIsServedOnTheOwnerSequence) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  const auto [result, port] = StartAndWait(listener, LoopbackConfig());
  ASSERT_EQ(result, StartResult::kOk);

  const std::string response =
      HttpGet(port, "/v1/assets/app.js?cachebust=1", "hello-from-owner");

  EXPECT_EQ(owner.calls(), 1);
  // (relaxed mode forwards the query — a dev server needs it; Release never
  // does, pinned by the router's own tests.)
  EXPECT_EQ(owner.last_path(), "/v1/assets/app.js?cachebust=1");
  EXPECT_NE(response.find("HTTP/1.1 200"), std::string::npos) << response;
  EXPECT_NE(response.find("hello-from-owner"), std::string::npos) << response;
}

// net::HttpServer silently drops a response above the connection's send
// buffer limit — 1 MiB by default, smaller than plenty of bundle chunks (the
// remote page then hangs on a module that never arrives).
TEST_F(NexusRemoteAccessListenerTest, ProxiesABodyLargerThanTheDefaultBuffer) {
  FakeOwner owner;
  owner.set_padding_bytes(2 * 1024 * 1024);
  NexusRemoteAccessListener listener(owner.Callback());
  const auto [result, port] = StartAndWait(listener, LoopbackConfig());
  ASSERT_EQ(result, StartResult::kOk);

  const std::string response =
      HttpGet(port, "/v1/assets/big.js", "hello-from-owner");
  EXPECT_GT(response.size(), 2u * 1024 * 1024);
  EXPECT_NE(response.find("hello-from-owner"), std::string::npos);
}

// THE regression test for "no runtime entry path reported". The runtime names
// its immutable build entry in the hello it sends on the control socket, and
// that hello is the ONLY place the listener can learn it — nothing else pushes
// one. The listener used to ignore every control-socket message, so `GET /`
// answered 503 for the whole life of the process and the minted link never
// loaded anything.
TEST_F(NexusRemoteAccessListenerTest, EntryRedirectUsesTheControlHello) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.relaxed_proxy = false;  // Release: `GET /` redirects, or 503s
  const std::string token = config.control_token;
  const auto [result, port] = StartAndWait(listener, std::move(config));
  ASSERT_EQ(result, StartResult::kOk);

  // No runtime yet: the state the user saw.
  EXPECT_NE(HttpGet(port, "/", "\r\n\r\n").find(" 503 "), std::string::npos);

  std::unique_ptr<net::TCPClientSocket> control =
      OpenWebSocket(port, "/host/control?token=" + token);
  SendText(*control, R"({"hello":1,"entryPath":"/v1/b/20260917T103000000Z/"})");

  const std::string response = GetRootUntilRedirect(port);
  EXPECT_NE(response.find(" 302 "), std::string::npos) << response;
  EXPECT_NE(response.find("Location:/v1/b/20260917T103000000Z/"),
            std::string::npos)
      << response;
  EXPECT_EQ(owner.calls(), 0);  // a redirect fetches nothing upstream
}

// The redirect is what a remembered device opens (the bare address, the
// installed app's start_url), so it has to come out of the HTTP cache when this
// desktop is off: uncached, the browser showed its own "site can't be reached"
// although the entry it points at and every asset were cached. The 503 is the
// opposite — "no runtime yet" must never be remembered.
TEST_F(NexusRemoteAccessListenerTest, EntryRedirectIsCacheableAndThe503IsNot) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.relaxed_proxy = false;
  const std::string token = config.control_token;
  const auto [result, port] = StartAndWait(listener, std::move(config));
  ASSERT_EQ(result, StartResult::kOk);

  const std::string unavailable = HttpGet(port, "/", "\r\n\r\n");
  EXPECT_NE(unavailable.find(" 503 "), std::string::npos) << unavailable;
  EXPECT_EQ(unavailable.find("max-age"), std::string::npos) << unavailable;

  std::unique_ptr<net::TCPClientSocket> control =
      OpenWebSocket(port, "/host/control?token=" + token);
  SendText(*control, R"({"hello":1,"entryPath":"/v1/b/20260917T103000000Z/"})");

  const std::string response = GetRootUntilRedirect(port);
  EXPECT_NE(response.find(" 302 "), std::string::npos) << response;
  EXPECT_NE(response.find("Cache-Control:private, max-age=31536000"),
            std::string::npos)
      << response;
}

// The owner hears every change in the number of connected /rt clients. A
// handle that stores the count and tells nobody leaves the settings page and
// every other surface at 0 with devices connected.
TEST_F(NexusRemoteAccessListenerTest, TellsTheOwnerWhenAClientJoinsAndLeaves) {
  FakeOwner owner;
  std::vector<size_t> counts;
  base::RunLoop joined;
  base::RunLoop left;
  NexusRemoteAccessListener listener(
      owner.Callback(), base::BindLambdaForTesting([&](size_t count) {
        counts.push_back(count);
        (count == 1 ? joined : left).Quit();
      }));
  ListenerConfig config = LoopbackConfig();
  const std::string token = config.control_token;
  const auto [result, port] = StartAndWait(listener, std::move(config));
  ASSERT_EQ(result, StartResult::kOk);

  // A runtime behind the relay, or /rt is refused before it is counted.
  std::unique_ptr<net::TCPClientSocket> control =
      OpenWebSocket(port, "/host/control?token=" + token);
  std::unique_ptr<net::TCPClientSocket> client = OpenWebSocket(
      port, "/rt", "http://127.0.0.1:" + base::NumberToString(port));
  joined.Run();
  EXPECT_EQ(listener.client_count(), 1u);

  client.reset();  // the device goes away
  left.Run();
  EXPECT_EQ(listener.client_count(), 0u);
  EXPECT_EQ(counts, (std::vector<size_t>{1, 0}));
}

// Only a deliberate OFF sends 4003 (`listener_disabled`), which the client
// treats as final; a quit, rebind or bind error sends 1001 so the page keeps
// retrying.
TEST_F(NexusRemoteAccessListenerTest, ClientCloseCodeSaysWhyTheListenerWent) {
  for (const auto& [reason, expected_code] :
       std::vector<std::pair<NexusRemoteAccessListener::StopReason, uint16_t>>{
           {NexusRemoteAccessListener::StopReason::kUserDisabled, 4003},
           {NexusRemoteAccessListener::StopReason::kGoingAway, 1001},
       }) {
    FakeOwner owner;
    NexusRemoteAccessListener listener(owner.Callback());
    ListenerConfig config = LoopbackConfig();
    const std::string token = config.control_token;
    const auto [result, port] = StartAndWait(listener, std::move(config));
    ASSERT_EQ(result, StartResult::kOk);

    // A runtime behind the relay, or /rt is refused 4001 before it is spliced.
    std::unique_ptr<net::TCPClientSocket> control =
        OpenWebSocket(port, "/host/control?token=" + token);
    std::unique_ptr<net::TCPClientSocket> client = OpenWebSocket(
        port, "/rt", "http://127.0.0.1:" + base::NumberToString(port));

    listener.Stop(reason);

    // The server's close frame: unmasked, opcode 0x8, then a 2-byte
    // big-endian status code (RFC 6455 section 5.5.1).
    const std::string bytes = ReadUntilClosed(*client);
    ASSERT_GE(bytes.size(), 4u) << "no close frame arrived";
    EXPECT_EQ(static_cast<uint8_t>(bytes[0]) & 0x0f, 0x8) << "not a close frame";
    const uint16_t code =
        static_cast<uint16_t>((static_cast<uint8_t>(bytes[2]) << 8) |
                              static_cast<uint8_t>(bytes[3]));
    EXPECT_EQ(code, expected_code);
  }
}

// A control message the listener cannot use is ignored, not adopted and not
// fatal: the socket stays up and the NEXT hello still lands. All four are sent
// before the good one on the SAME socket, so TCP order makes the 302 below
// proof that every one of them was read and dropped (a dev runtime's null
// entry path included — it has no versioned entry to name).
TEST_F(NexusRemoteAccessListenerTest, UnusableControlMessagesAreIgnored) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.relaxed_proxy = false;
  const std::string token = config.control_token;
  const auto [result, port] = StartAndWait(listener, std::move(config));
  ASSERT_EQ(result, StartResult::kOk);

  std::unique_ptr<net::TCPClientSocket> control =
      OpenWebSocket(port, "/host/control?token=" + token);
  SendText(*control, "not json at all");
  SendText(*control, R"({"hello":1,"entryPath":null})");
  SendText(*control, R"({"hello":1,"entryPath":"https://evil.example/"})");
  SendText(*control, R"({"closed":"0123456789abcdef0123456789abcdef"})");
  SendText(*control, R"({"hello":1,"entryPath":"/v1/b/20260917T103000000Z/"})");

  const std::string response = GetRootUntilRedirect(port);
  EXPECT_NE(response.find(" 302 "), std::string::npos) << response;
  EXPECT_NE(response.find("Location:/v1/b/20260917T103000000Z/"),
            std::string::npos)
      << response;
}

// A restart (the user changes the port, or toggles remote access off and on)
// drops the control socket, so the fresh Core is seeded with what the previous
// one learned: the re-minted link works immediately instead of 503ing until the
// runtime's dialer reconnects.
TEST_F(NexusRemoteAccessListenerTest, ARestartKeepsTheReportedEntryPath) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  ListenerConfig config = LoopbackConfig();
  config.relaxed_proxy = false;
  const std::string token = config.control_token;
  const auto [result, port] = StartAndWait(listener, std::move(config));
  ASSERT_EQ(result, StartResult::kOk);

  std::unique_ptr<net::TCPClientSocket> control =
      OpenWebSocket(port, "/host/control?token=" + token);
  SendText(*control, R"({"hello":1,"entryPath":"/v1/b/20260917T103000000Z/"})");
  ASSERT_NE(GetRootUntilRedirect(port).find(" 302 "), std::string::npos);
  // Drain the Core's report of what it learned onto this (the owner's) thread.
  task_environment_.RunUntilIdle();

  ListenerConfig restart = LoopbackConfig();
  restart.relaxed_proxy = false;
  const auto [restart_result, restart_port] =
      StartAndWait(listener, std::move(restart));
  ASSERT_EQ(restart_result, StartResult::kOk);

  // A brand-new port with no control socket behind it, answering at once.
  const std::string response = HttpGet(restart_port, "/", "\r\n\r\n");
  EXPECT_NE(response.find(" 302 "), std::string::npos) << response;
  EXPECT_NE(response.find("Location:/v1/b/20260917T103000000Z/"),
            std::string::npos)
      << response;
}

// With no entry path reported: Release answers 503, but the Debug relaxed
// proxy serves the upstream's own "/" — a vite dev server has no immutable
// build entry, so the minted link (http://<bind>:<port>/#rt=...) would
// otherwise be a permanent 503 in dev (what the e2e specs open).
TEST_F(NexusRemoteAccessListenerTest, RootWithNoEntryPath) {
  FakeOwner owner;
  NexusRemoteAccessListener listener(owner.Callback());
  const auto [result, port] = StartAndWait(listener, LoopbackConfig());
  ASSERT_EQ(result, StartResult::kOk);
  const std::string relaxed = HttpGet(port, "/", "hello-from-owner");
  EXPECT_NE(relaxed.find("hello-from-owner"), std::string::npos) << relaxed;
  EXPECT_EQ(owner.last_path(), "/");

  ListenerConfig strict = LoopbackConfig();
  strict.relaxed_proxy = false;
  const auto [strict_result, strict_port] =
      StartAndWait(listener, std::move(strict));
  ASSERT_EQ(strict_result, StartResult::kOk);
  const std::string response = HttpGet(strict_port, "/", "\r\n\r\n");
  EXPECT_NE(response.find("503"), std::string::npos) << response;
  EXPECT_EQ(owner.calls(), 1);
}

// Pins "nothing here ever blocks". The service is built — and torn down — on
// the UI thread inside BrowserMainLoop's DisallowUnresponsiveTasks scope,
// where a WaitableEvent wait or a thread join is a FATAL
// (thread_restrictions.cc). The earlier design needed a
// ScopedAllowBaseSyncPrimitivesOutsideBlockingScope to survive this; this one
// must survive it with no allowance at all.
TEST_F(NexusRemoteAccessListenerTest, NeverBlocksInsideDisallowUnresponsiveTasks) {
  FakeOwner owner;
  base::DisallowUnresponsiveTasks();
  {
    auto listener = std::make_unique<NexusRemoteAccessListener>(owner.Callback());
    const auto [result, port] = StartAndWait(*listener, LoopbackConfig());
    EXPECT_EQ(result, StartResult::kOk);
    listener->Stop(NexusRemoteAccessListener::StopReason::kGoingAway);
    // Destroy a STARTED listener too: the join must happen off this thread.
    EXPECT_EQ(std::get<0>(StartAndWait(*listener, LoopbackConfig())),
              StartResult::kOk);
    listener.reset();
  }
  // DisallowUnresponsiveTasks is one-way; restore for the fixture's teardown.
  base::internal::ResetThreadRestrictionsForTesting();
}

}  // namespace
}  // namespace nexus::remote_access
