// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/remote_access/remote_access_router.h"

#include <optional>
#include <string>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus::remote_access {
namespace {

constexpr int kVersion = 1;

RouteContext MakeContext() {
  RouteContext ctx;
  ctx.expected_host = "192.168.1.10:47615";
  ctx.control_token = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  ctx.interface_version = kVersion;
  ctx.has_control_socket = true;
  ctx.relaxed_proxy = false;
  return ctx;
}

RouteRequest MakeGet(std::string path) {
  RouteRequest req;
  req.method = "GET";
  req.raw_path = std::move(path);
  req.host_header = "192.168.1.10:47615";
  req.peer_is_loopback = false;
  return req;
}

TEST(RemoteAccessRouterTest, ProxiesTheImmutableEntry) {
  RouteContext ctx = MakeContext();
  const char* paths[] = {
      "/v1/b/20260917T103000000Z/",
      "/v1/b/20260917T103000000Z/index.html",
      "/v1/b/20260917T103000000Z/assets-manifest.json",
  };
  for (const char* p : paths) {
    RouteDecision d = Route(MakeGet(p), ctx);
    ASSERT_EQ(d.kind, RouteKind::kProxy) << p;
    EXPECT_EQ(d.upstream_path, p);
  }
}

TEST(RemoteAccessRouterTest, ProxiesVersionedAssets) {
  RouteContext ctx = MakeContext();
  RouteDecision d = Route(MakeGet("/v1/assets/index-abc123.js"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kProxy);
  EXPECT_EQ(d.upstream_path, "/v1/assets/index-abc123.js");
  // Nested asset paths (e.g. locales) proxy too.
  RouteDecision d2 = Route(MakeGet("/v1/assets/locales/en-xyz.json"), ctx);
  EXPECT_EQ(d2.kind, RouteKind::kProxy);
}

// Each fixed file at the path the deploy serves it from: the favicon (a
// release index.html's `<link rel="icon" href="/v<N>/product_logo_128.png">`),
// velloc.icon, and the web app manifest with its two install icons exist only
// under the version root; managed-binaries.json is the page's one
// root-relative fetch.
TEST(RemoteAccessRouterTest, ProxiesTheFixedFilesWhereTheDeployServesThem) {
  RouteContext ctx = MakeContext();
  for (const char* p :
       {"/v1/product_logo_128.png", "/v1/velloc.icon",
        "/v1/manifest.webmanifest", "/v1/app_icon_192.png",
        "/v1/app_icon_512.png", "/managed-binaries.json"}) {
    RouteDecision d = Route(MakeGet(p), ctx);
    ASSERT_EQ(d.kind, RouteKind::kProxy) << p;
    EXPECT_EQ(d.upstream_path, p);
  }

  // The versioned rows follow the binary's interface version.
  ctx.interface_version = 2;
  for (const char* p :
       {"/v2/product_logo_128.png", "/v2/velloc.icon",
        "/v2/manifest.webmanifest", "/v2/app_icon_512.png",
        "/managed-binaries.json"}) {
    EXPECT_EQ(Route(MakeGet(p), ctx).kind, RouteKind::kProxy) << p;
  }
  EXPECT_EQ(Route(MakeGet("/v1/product_logo_128.png"), ctx).kind,
            RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, RefusesTheFixedFilesAnywhereElse) {
  RouteContext ctx = MakeContext();
  const char* refused[] = {
      // The root forms 404 upstream — only managed-binaries.json is staged
      // at the site root.
      "/product_logo_128.png",
      "/velloc.icon",
      "/manifest.webmanifest",
      "/app_icon_192.png",
      // Another interface version's copies are not this binary's contract.
      "/v2/product_logo_128.png",
      "/v2/velloc.icon",
      "/v2/manifest.webmanifest",
      // managed-binaries.json is fetched root-relative only.
      "/v1/managed-binaries.json",
      // Exact matches, not prefixes.
      "/v1/product_logo_128.png/x",
      "/v1/product_logo_128.pngx",
      "/v1/velloc.icon.map",
      "/v1/manifest.webmanifest.bak",
      "/v1/app_icon_256.png",
      "/x/v1/product_logo_128.png",
      "/managed-binaries.json/x",
      // The entry directory is not the version root.
      "/v1/b/20260917T103000000Z/product_logo_128.png",
  };
  for (const char* p : refused) {
    RouteDecision d = Route(MakeGet(p), ctx);
    ASSERT_EQ(d.kind, RouteKind::kReject) << p;
    EXPECT_EQ(d.status, RejectStatus::kNotFound) << p;
  }
}

TEST(RemoteAccessRouterTest, RejectsOtherPathsAndWrongVersion) {
  RouteContext ctx = MakeContext();
  // Wrong interface version is not this binary's contract.
  EXPECT_EQ(Route(MakeGet("/v2/assets/x.js"), ctx).kind, RouteKind::kReject);
  // Arbitrary paths are not proxied.
  EXPECT_EQ(Route(MakeGet("/etc/passwd"), ctx).kind, RouteKind::kReject);
  EXPECT_EQ(Route(MakeGet("/v1/build-info.json"), ctx).kind,
            RouteKind::kReject);
  // An entry with a non-key alphabet is not an entry.
  EXPECT_EQ(Route(MakeGet("/v1/b/has-hyphen-/"), ctx).kind, RouteKind::kReject);
  EXPECT_EQ(Route(MakeGet("/v1/b/"), ctx).kind, RouteKind::kReject);
  // The asset prefix must be followed by something.
  EXPECT_EQ(Route(MakeGet("/v1/assets/"), ctx).kind, RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, RejectsTraversalAndBypasses) {
  RouteContext ctx = MakeContext();
  const char* bad[] = {
      "/v1/../secret",
      "/v1/assets/../build-info.json",
      "/v1/assets/../../secret",
      "/v1/%2e%2e/secret",        // percent-encoded ".."
      "/v1/assets/%2E%2E/x",
      "/v1/assets/..%2fsecret",   // encoded slash
      "/v1/assets/%2e%2e%2fsecret",
      "/v1/\\secret",              // backslash separator (Windows)
      "/v1/assets/..\\secret",
      "/v1/assets/%zz/x",          // malformed escape
  };
  for (const char* p : bad) {
    RouteDecision d = Route(MakeGet(p), ctx);
    ASSERT_EQ(d.kind, RouteKind::kReject) << p;
    EXPECT_EQ(d.status, RejectStatus::kNotFound) << p;
  }
}

TEST(RemoteAccessRouterTest, AllowsDotsThatAreNotTraversal) {
  RouteContext ctx = MakeContext();
  // A segment that merely CONTAINS dots is a file name, not traversal.
  RouteDecision d = Route(MakeGet("/v1/assets/..foo.js"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kProxy);
  RouteDecision d2 = Route(MakeGet("/v1/assets/x..js"), ctx);
  EXPECT_EQ(d2.kind, RouteKind::kProxy);
}

TEST(RemoteAccessRouterTest, CollapsesDuplicateSlashes) {
  RouteContext ctx = MakeContext();
  RouteDecision d = Route(MakeGet("/v1//assets//x.js"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kProxy);
  EXPECT_EQ(d.upstream_path, "/v1/assets/x.js");
  // "//" cannot smuggle a traversal past the segment check.
  EXPECT_EQ(Route(MakeGet("/v1//..//assets/x"), ctx).kind, RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, HostMismatchIs403) {
  RouteContext ctx = MakeContext();
  RouteRequest req = MakeGet("/v1/assets/x.js");
  req.host_header = "example.com:47615";  // DNS rebind attempt
  RouteDecision d = Route(req, ctx);
  EXPECT_EQ(d.kind, RouteKind::kReject);
  EXPECT_EQ(d.status, RejectStatus::kForbidden);

  RouteRequest req2 = MakeGet("/v1/assets/x.js");
  req2.host_header = "192.168.1.10";  // missing port
  EXPECT_EQ(Route(req2, ctx).status, RejectStatus::kForbidden);
}

// Under a 0.0.0.0 wildcard bind the listener answers clients that arrive on
// ANY of the machine's NICs. The Host gate must accept a request whose Host is
// one of the pushed local_addresses + the bound port, and still 403 a foreign
// IP / hostname / wrong port (the DNS-rebinding protection is not weakened).
TEST(RemoteAccessRouterTest, WildcardBindAcceptsAnyLocalNicHost) {
  RouteContext ctx;
  ctx.expected_host = "0.0.0.0:47615";
  ctx.control_token =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  ctx.interface_version = kVersion;
  ctx.has_control_socket = true;
  ctx.relaxed_proxy = false;
  ctx.bind_is_wildcard = true;
  ctx.local_addresses = {"127.0.0.1", "192.168.1.10", "10.0.0.4"};

  // Each local NIC literal + the bound port proxies.
  for (const char* host :
       {"192.168.1.10:47615", "10.0.0.4:47615", "127.0.0.1:47615"}) {
    RouteRequest req = MakeGet("/v1/assets/x.js");
    req.host_header = host;
    EXPECT_EQ(Route(req, ctx).kind, RouteKind::kProxy) << host;
  }

  // A NIC IP on the WRONG port is not this listener.
  RouteRequest wrong_port = MakeGet("/v1/assets/x.js");
  wrong_port.host_header = "192.168.1.10:9999";
  EXPECT_EQ(Route(wrong_port, ctx).status, RejectStatus::kForbidden);

  // A foreign IP the machine does not have → 403.
  RouteRequest foreign = MakeGet("/v1/assets/x.js");
  foreign.host_header = "203.0.113.5:47615";
  EXPECT_EQ(Route(foreign, ctx).status, RejectStatus::kForbidden);

  // A hostname (DNS rebind) → 403.
  RouteRequest named = MakeGet("/v1/assets/x.js");
  named.host_header = "example.com:47615";
  EXPECT_EQ(Route(named, ctx).status, RejectStatus::kForbidden);

  // Missing port → 403.
  RouteRequest no_port = MakeGet("/v1/assets/x.js");
  no_port.host_header = "192.168.1.10";
  EXPECT_EQ(Route(no_port, ctx).status, RejectStatus::kForbidden);
}

// The /rt Origin gate follows the same wildcard rule: a remote's Origin names
// the NIC it dialed, so any local NIC origin passes and a cross-origin page is
// still refused.
TEST(RemoteAccessRouterTest, WildcardClientSocketAcceptsLocalNicOrigin) {
  RouteContext ctx;
  ctx.expected_host = "0.0.0.0:47615";
  ctx.control_token =
      "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  ctx.interface_version = kVersion;
  ctx.has_control_socket = true;
  ctx.bind_is_wildcard = true;
  ctx.local_addresses = {"127.0.0.1", "192.168.1.10"};

  RouteRequest req = MakeGet("/rt");
  req.is_websocket_upgrade = true;
  req.host_header = "192.168.1.10:47615";
  req.origin_header = "http://192.168.1.10:47615";
  EXPECT_EQ(Route(req, ctx).kind, RouteKind::kClientSocket);

  // Cross-origin → 403 even under the wildcard.
  RouteRequest foreign = req;
  foreign.origin_header = "http://192.168.1.99:9000";
  EXPECT_EQ(Route(foreign, ctx).status, RejectStatus::kForbidden);

  // A scheme-less Origin naming a local NIC is NOT this listener's origin.
  RouteRequest no_scheme = req;
  no_scheme.origin_header = "192.168.1.10:47615";
  EXPECT_EQ(Route(no_scheme, ctx).status, RejectStatus::kForbidden);

  // A wrong scheme is not the http origin either (the listener is plain http).
  RouteRequest https = req;
  https.origin_header = "https://192.168.1.10:47615";
  EXPECT_EQ(Route(https, ctx).status, RejectStatus::kForbidden);

  // An empty Origin is refused.
  RouteRequest empty = req;
  empty.origin_header = "";
  EXPECT_EQ(Route(empty, ctx).status, RejectStatus::kForbidden);
}

// Single-literal bind: the Origin must be exactly "http://" + expected_host.
// A scheme-less Origin equal to the host passes HostMatches but is not a
// browser WebSocket origin for this listener — refuse it.
TEST(RemoteAccessRouterTest, ClientSocketRequiresHttpSchemeOnOrigin) {
  RouteContext ctx = MakeContext();
  RouteRequest req = MakeGet("/rt");
  req.is_websocket_upgrade = true;

  // Exact match passes.
  req.origin_header = "http://192.168.1.10:47615";
  EXPECT_EQ(Route(req, ctx).kind, RouteKind::kClientSocket);

  // Scheme-less → 403 even though the host part matches.
  RouteRequest no_scheme = req;
  no_scheme.origin_header = "192.168.1.10:47615";
  RouteDecision d = Route(no_scheme, ctx);
  EXPECT_EQ(d.kind, RouteKind::kReject);
  EXPECT_EQ(d.status, RejectStatus::kForbidden);

  // https:// → 403 (this listener is plain http).
  RouteRequest https = req;
  https.origin_header = "https://192.168.1.10:47615";
  EXPECT_EQ(Route(https, ctx).status, RejectStatus::kForbidden);

  // Trailing path junk after a valid prefix is not the origin.
  RouteRequest junk = req;
  junk.origin_header = "http://192.168.1.10:47615/extra";
  EXPECT_EQ(Route(junk, ctx).status, RejectStatus::kForbidden);
}

TEST(RemoteAccessRouterTest, RootRedirectsToEntry) {
  RouteContext ctx = MakeContext();
  RouteDecision d = Route(MakeGet("/"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kEntryRedirect);
  // A query on "/" is ignored by the proxy/redirect decision.
  EXPECT_EQ(Route(MakeGet("/?x=1"), ctx).kind, RouteKind::kEntryRedirect);
}

TEST(RemoteAccessRouterTest, NonGetIsRejected) {
  RouteContext ctx = MakeContext();
  RouteRequest req = MakeGet("/v1/assets/x.js");
  req.method = "POST";
  EXPECT_EQ(Route(req, ctx).kind, RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, ClientSocketRequiresUpgradeAndOrigin) {
  RouteContext ctx = MakeContext();
  RouteRequest req = MakeGet("/rt");
  req.is_websocket_upgrade = true;
  req.origin_header = "http://192.168.1.10:47615";  // the listener's own origin
  EXPECT_EQ(Route(req, ctx).kind, RouteKind::kClientSocket);

  // Cross-origin → 403 (a LAN page may not drive the runtime).
  RouteRequest foreign = req;
  foreign.origin_header = "http://192.168.1.99:9000";
  RouteDecision d = Route(foreign, ctx);
  EXPECT_EQ(d.kind, RouteKind::kReject);
  EXPECT_EQ(d.status, RejectStatus::kForbidden);

  // No upgrade → not a socket.
  RouteRequest plain = MakeGet("/rt");
  EXPECT_EQ(Route(plain, ctx).kind, RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, HostControlIsLoopbackAndTokenOnly) {
  RouteContext ctx = MakeContext();
  RouteRequest req = MakeGet("/host/control?token=" + ctx.control_token);
  req.is_websocket_upgrade = true;
  req.peer_is_loopback = true;
  EXPECT_EQ(Route(req, ctx).kind, RouteKind::kHostControl);

  // Wrong token → 404 (existence not leaked).
  RouteRequest bad_tok = req;
  bad_tok.raw_path = "/host/control?token=wrong";
  RouteDecision d = Route(bad_tok, ctx);
  EXPECT_EQ(d.kind, RouteKind::kReject);
  EXPECT_EQ(d.status, RejectStatus::kNotFound);

  // A non-loopback peer is always refused, even with the right token.
  RouteRequest remote = req;
  remote.peer_is_loopback = false;
  d = Route(remote, ctx);
  EXPECT_EQ(d.kind, RouteKind::kReject);
  EXPECT_EQ(d.status, RejectStatus::kNotFound);

  // Token must be a CONSTANT-TIME match — a prefix does not pass.
  RouteRequest prefix = req;
  prefix.raw_path = "/host/control?token=" + ctx.control_token.substr(0, 8);
  EXPECT_EQ(Route(prefix, ctx).kind, RouteKind::kReject);

  // A peer that IS the listener's own bound interface address is same-machine
  // (the runtime dialing its own LAN IP from this machine). It passes with the
  // right token even though it is NOT loopback.
  RouteRequest own_bind = req;
  own_bind.peer_is_loopback = false;
  own_bind.peer_is_bound_address = true;
  EXPECT_EQ(Route(own_bind, ctx).kind, RouteKind::kHostControl);

  // ... but only with the right token: same-machine alone is not enough.
  RouteRequest own_bind_bad_tok = own_bind;
  own_bind_bad_tok.raw_path = "/host/control?token=wrong";
  RouteDecision d2 = Route(own_bind_bad_tok, ctx);
  EXPECT_EQ(d2.kind, RouteKind::kReject);
  EXPECT_EQ(d2.status, RejectStatus::kNotFound);
}

TEST(RemoteAccessRouterTest, HostLegParsesHexIdAndGates) {
  RouteContext ctx = MakeContext();
  const std::string leg = "0123456789abcdef0123456789abcdef";
  RouteRequest req = MakeGet("/host/leg/" + leg + "?token=" + ctx.control_token);
  req.is_websocket_upgrade = true;
  req.peer_is_loopback = true;
  RouteDecision d = Route(req, ctx);
  ASSERT_EQ(d.kind, RouteKind::kHostLeg);
  EXPECT_EQ(d.leg_id, leg);

  // Not 32 hex → 404.
  RouteRequest bad_id = req;
  bad_id.raw_path = "/host/leg/xyz?token=" + ctx.control_token;
  EXPECT_EQ(Route(bad_id, ctx).kind, RouteKind::kReject);

  // Non-loopback → 404.
  RouteRequest remote = req;
  remote.peer_is_loopback = false;
  EXPECT_EQ(Route(remote, ctx).kind, RouteKind::kReject);

  // Wrong token → 404, not 401.
  RouteRequest bad_tok = req;
  bad_tok.raw_path = "/host/leg/" + leg + "?token=zzz";
  RouteDecision d2 = Route(bad_tok, ctx);
  EXPECT_EQ(d2.kind, RouteKind::kReject);
  EXPECT_EQ(d2.status, RejectStatus::kNotFound);

  // A peer on the listener's own bound interface address is same-machine and
  // passes the gate; a FOREIGN machine's address is neither loopback nor our
  // bound address, so it still 404s (the non-loopback case above is exactly
  // that — peer_is_bound_address stays false for a foreign peer).
  RouteRequest own_bind = req;
  own_bind.peer_is_loopback = false;
  own_bind.peer_is_bound_address = true;
  RouteDecision d3 = Route(own_bind, ctx);
  ASSERT_EQ(d3.kind, RouteKind::kHostLeg);
  EXPECT_EQ(d3.leg_id, leg);
}

TEST(RemoteAccessRouterTest, ProxyDropsTheQuery) {
  RouteContext ctx = MakeContext();
  RouteDecision d = Route(MakeGet("/v1/assets/x.js?v=123"), ctx);
  ASSERT_EQ(d.kind, RouteKind::kProxy);
  // The query never reaches the upstream.
  EXPECT_EQ(d.upstream_path, "/v1/assets/x.js");
}

TEST(RemoteAccessRouterTest, RelaxedProxyLetsAnyGetThrough) {
  RouteContext ctx = MakeContext();
  ctx.relaxed_proxy = true;  // Debug upstream = vite dev server
  RouteDecision d = Route(MakeGet("/src/main.tsx"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kProxy);
  // A dev server selects the response by query, so relaxed mode forwards it
  // (strict mode never does — see ProxyStripsTheQuery-style cases above).
  d = Route(MakeGet("/src/index.css?import&t=123"), ctx);
  EXPECT_EQ(d.kind, RouteKind::kProxy);
  EXPECT_EQ(d.upstream_path, "/src/index.css?import&t=123");
  // ... but traversal is still rejected even in relaxed mode.
  EXPECT_EQ(Route(MakeGet("/../secret"), ctx).kind, RouteKind::kReject);
}

TEST(RemoteAccessRouterTest, ResponseHeaderFiltering) {
  // Forwarded:
  EXPECT_TRUE(IsForwardableResponseHeader("content-type"));
  EXPECT_TRUE(IsForwardableResponseHeader("cache-control"));
  EXPECT_TRUE(IsForwardableResponseHeader("etag"));
  EXPECT_TRUE(IsForwardableResponseHeader("last-modified"));
  // Dropped:
  EXPECT_FALSE(IsForwardableResponseHeader("content-encoding"));  // decompressed
  EXPECT_FALSE(IsForwardableResponseHeader("content-length"));    // recomputed
  EXPECT_FALSE(IsForwardableResponseHeader("set-cookie"));
  EXPECT_FALSE(IsForwardableResponseHeader("strict-transport-security"));
  EXPECT_FALSE(IsForwardableResponseHeader("alt-svc"));
  EXPECT_FALSE(IsForwardableResponseHeader("connection"));        // hop-by-hop
}

TEST(RemoteAccessRouterTest, RequestHeaderFiltering) {
  EXPECT_TRUE(IsForwardableRequestHeader("if-none-match"));
  EXPECT_TRUE(IsForwardableRequestHeader("if-modified-since"));
  EXPECT_FALSE(IsForwardableRequestHeader("cookie"));
  EXPECT_FALSE(IsForwardableRequestHeader("authorization"));
  EXPECT_FALSE(IsForwardableRequestHeader("user-agent"));
}

TEST(RemoteAccessRouterTest, ConstantTimeEquals) {
  EXPECT_TRUE(ConstantTimeEquals("abc", "abc"));
  EXPECT_FALSE(ConstantTimeEquals("abc", "abd"));
  EXPECT_FALSE(ConstantTimeEquals("abc", "abcd"));
  EXPECT_FALSE(ConstantTimeEquals("abcd", "abc"));
  EXPECT_FALSE(ConstantTimeEquals("", "a"));
}

// --- the build entry the runtime reports over its control socket -----------

TEST(RemoteAccessRouterTest, IsBuildEntryPathIsTheEntryDirectoryOnly) {
  EXPECT_TRUE(IsBuildEntryPath("/v1/b/20260917T103000000Z/", kVersion));
  EXPECT_TRUE(IsBuildEntryPath("/v1/b/1/", kVersion));

  // Proxyable, but not the directory: `Location:` names the entry itself.
  EXPECT_FALSE(IsBuildEntryPath("/v1/b/20260917T103000000Z/index.html", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("/v1/b/20260917T103000000Z", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("/v1/assets/app.js", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("/managed-binaries.json", kVersion));
  // A key with anything but [0-9TZ] in it, an empty key, a second segment.
  EXPECT_FALSE(IsBuildEntryPath("/v1/b/abc/", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("/v1/b//", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("/v1/b/20260917T103000000Z/x/", kVersion));
  // Another interface version's entry: this listener would 404 it.
  EXPECT_FALSE(IsBuildEntryPath("/v2/b/20260917T103000000Z/", kVersion));
  // Not a path on this origin at all.
  EXPECT_FALSE(IsBuildEntryPath("//evil.example/v1/b/1/", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("https://evil.example/v1/b/1/", kVersion));
  EXPECT_FALSE(IsBuildEntryPath("", kVersion));
}

TEST(RemoteAccessRouterTest, EntryPathFromHelloTakesTheReportedEntry) {
  const std::optional<std::string> entry = EntryPathFromHello(
      R"({"hello":1,"entryPath":"/v1/b/20260917T103000000Z/"})", kVersion);
  ASSERT_TRUE(entry.has_value());
  EXPECT_EQ(*entry, "/v1/b/20260917T103000000Z/");
}

TEST(RemoteAccessRouterTest, EntryPathFromHelloRefusesEverythingElse) {
  // A dev runtime has no versioned entry to name: null, not a path.
  EXPECT_FALSE(EntryPathFromHello(R"({"hello":1,"entryPath":null})", kVersion));
  EXPECT_FALSE(EntryPathFromHello(R"({"hello":1})", kVersion));
  // Not a hello: `{"closed":<legId>}` and anything a later revision adds must
  // not be read as one.
  EXPECT_FALSE(EntryPathFromHello(R"({"entryPath":"/v1/b/1/"})", kVersion));
  EXPECT_FALSE(EntryPathFromHello(R"({"closed":"abc"})", kVersion));
  // Not JSON, or not an object.
  EXPECT_FALSE(EntryPathFromHello("", kVersion));
  EXPECT_FALSE(EntryPathFromHello("not json", kVersion));
  EXPECT_FALSE(EntryPathFromHello(R"(["hello"])", kVersion));
  // entryPath present but not a string.
  EXPECT_FALSE(EntryPathFromHello(R"({"hello":1,"entryPath":1})", kVersion));
  // A path that could redirect off this origin or smuggle a response header
  // (the value goes into `Location:`).
  EXPECT_FALSE(EntryPathFromHello(
      R"({"hello":1,"entryPath":"https://evil.example/"})", kVersion));
  EXPECT_FALSE(EntryPathFromHello(
      R"({"hello":1,"entryPath":"//evil.example/"})", kVersion));
  EXPECT_FALSE(EntryPathFromHello(
      // The two-character JSON escapes below DECODE to a real CR LF.
      R"({"hello":1,"entryPath":"/v1/b/1/\r\nX-Injected: 1"})", kVersion));
  EXPECT_FALSE(EntryPathFromHello(
      R"({"hello":1,"entryPath":"/v1/b/../../etc/"})", kVersion));
}

}  // namespace
}  // namespace nexus::remote_access
