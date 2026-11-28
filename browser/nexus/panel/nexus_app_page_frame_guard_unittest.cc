// Copyright 2026 VellocLabs. All rights reserved.
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/panel/nexus_app_page_frame_guard.h"

#include <memory>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/test/scoped_feature_list.h"
#include "base/test/test_future.h"
#include "chrome/test/base/chrome_render_view_host_test_harness.h"
#include "components/network_hints/common/network_hints.mojom.h"
#include "chrome/test/base/testing_profile.h"
#include "content/public/browser/navigation_throttle.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/test/mock_navigation_handle.h"
#include "content/public/test/mock_navigation_throttle_registry.h"
#include "content/public/test/navigation_simulator.h"
#include "content/public/test/test_navigation_throttle_inserter.h"
#include "content/public/test/test_renderer_host.h"
#include "custom_browser/browser/nexus/nexus_test_support.h"
#include "custom_browser/chromium_hooks/hook_dispatcher.h"
#include "custom_browser/chromium_hooks/hook_registrar.h"
#include "mojo/public/cpp/bindings/binder_map.h"
#include "mojo/public/cpp/bindings/generic_pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"
#include "mojo/public/cpp/bindings/remote.h"
#include "net/base/ip_address.h"
#include "net/base/network_interfaces.h"
#include "services/network/public/cpp/network_service_buildflags.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/features.h"
#include "url/gurl.h"

#if BUILDFLAG(IS_P2P_ENABLED)
#include "services/network/public/mojom/p2p.mojom.h"
#endif

namespace nexus {
namespace {

using ThrottleAction = content::NavigationThrottle::ThrottleAction;

class AppPageFrameGuardTest : public ChromeRenderViewHostTestHarness {
 protected:
  // A regular TestingProfile would build GlicKeyedService and crash without a
  // real ProfileManager.
  TestingProfile::TestingFactories GetTestingFactories() const override {
    return test::GlicDisablingTestingFactories();
  }

  void SetUp() override {
    ChromeRenderViewHostTestHarness::SetUp();
    NavigateAndCommit(GURL("https://guest.test/"));
  }

  content::RenderFrameHost* AddChild(content::RenderFrameHost* parent,
                                     const std::string& name) {
    return content::RenderFrameHostTester::For(parent)->AppendChild(name);
  }

  // The child as the guest adds it: seen by the binder hook at creation.
  content::RenderFrameHost* AddGuestChild(content::RenderFrameHost* parent,
                                          const std::string& name) {
    content::RenderFrameHost* child = AddChild(parent, name);
    AppPageFrames::Note(*child, /*in_nexus_guest=*/true);
    return child;
  }

  ThrottleAction StartNavigation(content::RenderFrameHost* frame,
                                 const GURL& url,
                                 content::RenderFrameHost* initiator = nullptr) {
    content::TestNavigationThrottleInserter inserter(
        web_contents(), base::BindRepeating(&MaybeAddAppPageNavigationThrottle));
    auto simulator =
        content::NavigationSimulator::CreateRendererInitiated(url, frame);
    if (initiator) {
      simulator->SetInitiatorFrame(initiator);
    }
    simulator->Start();
    return simulator->GetLastThrottleCheckResult().action();
  }

};

TEST(AppPageFrameUrlTest, OnlyLocalDocumentsAreAllowed) {
  EXPECT_TRUE(IsAllowedAppPageFrameUrl(GURL("about:srcdoc")));
  EXPECT_TRUE(IsAllowedAppPageFrameUrl(GURL("about:blank")));
  EXPECT_FALSE(IsAllowedAppPageFrameUrl(GURL("https://example.test/?d=1")));
  EXPECT_FALSE(IsAllowedAppPageFrameUrl(GURL("http://127.0.0.1:9/leak")));
  EXPECT_FALSE(IsAllowedAppPageFrameUrl(GURL("data:text/html,x")));
  EXPECT_FALSE(IsAllowedAppPageFrameUrl(GURL("blob:null/1234")));
  EXPECT_FALSE(IsAllowedAppPageFrameUrl(GURL("chrome://settings")));
}

TEST_F(AppPageFrameGuardTest, LatchesOnlyANamedChildInTheGuest) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  content::RenderFrameHost* outside = AddChild(main_rfh(), kAppPageFrameName);
  AppPageFrames::Note(*outside, /*in_nexus_guest=*/false);
  AppPageFrames::Note(*main_rfh(), /*in_nexus_guest=*/true);

  EXPECT_TRUE(AppPageFrames::Contains(*page));
  EXPECT_FALSE(AppPageFrames::Contains(*kernel));
  EXPECT_FALSE(AppPageFrames::Contains(*outside));
  EXPECT_FALSE(AppPageFrames::Contains(*main_rfh()));
}

TEST_F(AppPageFrameGuardTest, FramesBelowAnAppPageAreAppPageFrames) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* nested = AddGuestChild(page, "");
  content::RenderFrameHost* deeper = AddGuestChild(nested, "anything");

  EXPECT_TRUE(AppPageFrames::Contains(*nested));
  EXPECT_TRUE(AppPageFrames::Contains(*deeper));
}

TEST_F(AppPageFrameGuardTest, CancelsARemoteNavigationBeforeItsRequest) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);

  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            StartNavigation(page, GURL("https://example.test/?d=secret")));
  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            StartNavigation(page, GURL("data:text/html,x")));
}

TEST_F(AppPageFrameGuardTest, CancelsANavigationInAFrameTheAppPageAdds) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* nested = AddGuestChild(page, "");

  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            StartNavigation(nested, GURL("https://example.test/?d=secret")));
}

TEST_F(AppPageFrameGuardTest, LetsAnAppPageLoadLocalDocuments) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);

  EXPECT_EQ(ThrottleAction::PROCEED,
            StartNavigation(page, GURL("about:blank")));
}

TEST_F(AppPageFrameGuardTest, CancelsEveryRedirect) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::MockNavigationHandle handle(GURL("about:srcdoc"), page);
  content::MockNavigationThrottleRegistry registry(&handle);
  AppPageNavigationThrottle throttle(registry);

  handle.set_url(GURL("https://example.test/?d=secret"));
  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            throttle.WillRedirectRequest().action());
}

TEST_F(AppPageFrameGuardTest, CancelsANavigationTheAppPageStartsInTheGuest) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);

  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            StartNavigation(main_rfh(), GURL("https://guest.test/?d=secret"),
                            /*initiator=*/page));
}

TEST_F(AppPageFrameGuardTest, ADestroyedAppPageDocumentStillCountsAsInitiator) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  const content::GlobalRenderFrameHostToken token = page->GetGlobalFrameToken();
  content::RenderFrameHostTester::For(page)->Detach();

  content::MockNavigationHandle handle(GURL("https://guest.test/?d=secret"),
                                       main_rfh());
  handle.set_initiator_frame_token(&token.frame_token);
  handle.set_initiator_process_id(
      content::ChildProcessId::FromUnsafeValue(token.child_id));
  EXPECT_TRUE(AppPageFrames::ContainsNavigation(handle));
}

TEST_F(AppPageFrameGuardTest, ADeletedAppPageFrameIsForgotten) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  AppPageFrames* frames = AppPageFrames::FromWebContents(web_contents());
  ASSERT_TRUE(frames);
  ASSERT_EQ(1u, frames->LatchedCountForTesting());

  content::RenderFrameHostTester::For(page)->Detach();
  EXPECT_EQ(0u, frames->LatchedCountForTesting());
}

std::optional<int> GroupFor(content::RenderFrameHost* frame) {
  content::MockNavigationHandle handle(GURL("about:srcdoc"), frame);
  return AppPageFrames::SandboxGroupFor(handle);
}

TEST_F(AppPageFrameGuardTest, AnAppPageGetsAGroupOfItsOwn) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);

  std::optional<int> group = GroupFor(page);
  ASSERT_TRUE(group);
  EXPECT_LE(*group, -2);
  EXPECT_EQ(group, GroupFor(page));
}

TEST_F(AppPageFrameGuardTest, FramesInsideAnAppPageShareItsGroup) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* nested = AddGuestChild(page, "");
  content::RenderFrameHost* renamed = AddGuestChild(nested, kAppPageFrameName);

  EXPECT_EQ(GroupFor(page), GroupFor(nested));
  EXPECT_EQ(GroupFor(page), GroupFor(renamed));
}

TEST_F(AppPageFrameGuardTest, TwoAppPagesGetDifferentGroups) {
  content::RenderFrameHost* first =
      AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* second =
      AddGuestChild(main_rfh(), kAppPageFrameName);

  ASSERT_TRUE(GroupFor(first));
  ASSERT_TRUE(GroupFor(second));
  EXPECT_NE(GroupFor(first), GroupFor(second));
}

TEST_F(AppPageFrameGuardTest, AppPagesOfTwoGuestsGetDifferentGroups) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  std::unique_ptr<content::WebContents> other = CreateTestWebContents();
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      other.get(), GURL("https://guest.test/"));
  content::RenderFrameHost* other_page =
      AddGuestChild(other->GetPrimaryMainFrame(), kAppPageFrameName);

  ASSERT_TRUE(GroupFor(other_page));
  EXPECT_NE(GroupFor(page), GroupFor(other_page));
}

TEST_F(AppPageFrameGuardTest, OtherFramesKeepChromiumGrouping) {
  AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  content::RenderFrameHost* outside = AddChild(main_rfh(), kAppPageFrameName);
  AppPageFrames::Note(*outside, /*in_nexus_guest=*/false);

  EXPECT_FALSE(GroupFor(kernel));
  EXPECT_FALSE(GroupFor(outside));

  std::unique_ptr<content::WebContents> tab = CreateTestWebContents();
  content::NavigationSimulator::NavigateAndCommitFromBrowser(
      tab.get(), GURL("https://example.test/"));
  EXPECT_FALSE(GroupFor(AddChild(tab->GetPrimaryMainFrame(), "")));
}

TEST_F(AppPageFrameGuardTest, ADeletedAppPageFrameLosesItsGroup) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  const std::optional<int> before = GroupFor(page);
  ASSERT_TRUE(before);
  AppPageFrames* frames = AppPageFrames::FromWebContents(web_contents());
  content::RenderFrameHostTester::For(page)->Detach();

  content::RenderFrameHost* reopened =
      AddGuestChild(main_rfh(), kAppPageFrameName);
  ASSERT_TRUE(GroupFor(reopened));
  EXPECT_NE(before, GroupFor(reopened));
  EXPECT_EQ(1u, frames->LatchedCountForTesting());
}

// Keeps only the sandbox grouping provider, so registering touches no
// process-wide hook.
class GroupProviderRecorder : public custom_browser::HookRegistrar {
 public:
  void AddWebUIDataSourceConfiguringCallback(
      std::string_view,
      custom_browser::WebUIDataSourceConfiguringCallback) override {}
  void AddNavigationThrottleProvider(
      custom_browser::NavigationThrottleProviderCallback) override {}
  void AddURLLoaderFactoryInterceptor(
      custom_browser::URLLoaderFactoryInterceptorCallback) override {}
  void AddWebUIRenderFrameBinderMapPopulator(
      custom_browser::WebUIRenderFrameBinderMapPopulatorCallback) override {}
  void AddRenderFrameBinderOverride(
      custom_browser::RenderFrameBinderOverrideCallback) override {}
  void AddRenderFrameInterfaceBinder(
      std::string_view,
      custom_browser::RenderFrameInterfaceBinderCallback) override {}
  void AddBrowserMainLifecycleHook(
      std::unique_ptr<custom_browser::BrowserMainLifecycleHook>) override {}
  void SetBubbleAnchorRectOverride(
      custom_browser::BubbleAnchorRectOverrideCallback) override {}
  void SetIsolatedSandboxGroupProvider(
      custom_browser::IsolatedSandboxGroupProviderCallback cb) override {
    provider = std::move(cb);
  }

  custom_browser::IsolatedSandboxGroupProviderCallback provider;
};

TEST_F(AppPageFrameGuardTest, RegistersTheSandboxGroupProvider) {
  GroupProviderRecorder registrar;
  RegisterAppPageFrameGuardHooks(&registrar);
  ASSERT_TRUE(registrar.provider);
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");

  content::MockNavigationHandle page_handle(GURL("about:srcdoc"), page);
  content::MockNavigationHandle kernel_handle(GURL("about:srcdoc"), kernel);
  EXPECT_EQ(GroupFor(page), registrar.provider.Run(page_handle));
  EXPECT_FALSE(registrar.provider.Run(kernel_handle));
}

TEST_F(AppPageFrameGuardTest, TheDispatcherKeepsChromiumGroupingWithoutAProvider) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::MockNavigationHandle handle(GURL("about:srcdoc"), page);
  custom_browser::GetHookRegistrar()->SetIsolatedSandboxGroupProvider({});

  EXPECT_FALSE(custom_browser::IsolatedSandboxGroupFor(handle));
}

TEST_F(AppPageFrameGuardTest, TheDispatcherAnswersWithTheRegisteredProvider) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  content::MockNavigationHandle page_handle(GURL("about:srcdoc"), page);
  content::MockNavigationHandle kernel_handle(GURL("about:srcdoc"), kernel);
  custom_browser::HookRegistrar* registrar = custom_browser::GetHookRegistrar();
  RegisterAppPageFrameGuardHooks(registrar);

  const std::optional<int> page_group =
      custom_browser::IsolatedSandboxGroupFor(page_handle);
  const std::optional<int> kernel_group =
      custom_browser::IsolatedSandboxGroupFor(kernel_handle);
  registrar->SetIsolatedSandboxGroupProvider({});

  ASSERT_TRUE(page_group);
  EXPECT_EQ(GroupFor(page), page_group);
  EXPECT_FALSE(kernel_group);
}

class AppPageFencedFrameTest : public AppPageFrameGuardTest {
 public:
  AppPageFencedFrameTest() {
    features_.InitAndEnableFeatureWithParameters(
        blink::features::kFencedFrames, {{"implementation_type", "mparch"}});
  }

 private:
  base::test::ScopedFeatureList features_;
};

TEST_F(AppPageFencedFrameTest, AFencedFrameInAnAppPageIsAnAppPageFrame) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHostTester::For(page)->InitializeRenderFrameIfNeeded();
  content::RenderFrameHost* fenced =
      content::RenderFrameHostTester::For(page)->AppendFencedFrame();
  ASSERT_TRUE(fenced);
  AppPageFrames::Note(*fenced, /*in_nexus_guest=*/true);

  EXPECT_TRUE(AppPageFrames::Contains(*fenced));
  EXPECT_EQ(GroupFor(page), GroupFor(fenced));
  EXPECT_EQ(ThrottleAction::CANCEL_AND_IGNORE,
            StartNavigation(fenced, GURL("https://example.test/?d=secret")));
}

TEST_F(AppPageFrameGuardTest, LeavesOtherFramesNavigating) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  ASSERT_TRUE(AppPageFrames::Contains(*page));

  EXPECT_EQ(ThrottleAction::PROCEED,
            StartNavigation(kernel, GURL("https://example.test/")));
  EXPECT_EQ(ThrottleAction::PROCEED,
            StartNavigation(main_rfh(), GURL("https://guest.test/next")));
}

#if BUILDFLAG(IS_P2P_ENABLED)
// Stands in for content's own binder, which the override must replace for an
// app-page frame and leave alone for any other frame.
void BindSentinel(bool* bound,
                  content::RenderFrameHost*,
                  mojo::PendingReceiver<network::mojom::P2PSocketManager>) {
  *bound = true;
}

class NetworkListRecorder
    : public network::mojom::P2PNetworkNotificationClient {
 public:
  void NetworkListChanged(const std::vector<net::NetworkInterface>& networks,
                          const net::IPAddress& ipv4,
                          const net::IPAddress& ipv6) override {
    networks_.SetValue(networks.size());
  }
  base::test::TestFuture<size_t> networks_;
};

TEST_F(AppPageFrameGuardTest, AnAppPageFrameGetsARefusingSocketManager) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  bool sentinel_bound = false;
  mojo::BinderMapWithContext<content::RenderFrameHost*> map;
  map.Add<network::mojom::P2PSocketManager>(
      base::BindRepeating(&BindSentinel, &sentinel_bound));
  OverrideAppPageFrameBinders(*page, map);

  mojo::Remote<network::mojom::P2PSocketManager> manager;
  mojo::GenericPendingReceiver receiver(manager.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(map.TryBind(page, &receiver));
  EXPECT_FALSE(sentinel_bound);

  base::test::TestFuture<const std::vector<net::IPAddress>&> addresses;
  manager->GetHostAddress("stun.example.test", std::nullopt,
                          /*enable_mdns=*/false, addresses.GetCallback());
  EXPECT_TRUE(addresses.Get().empty());

  NetworkListRecorder recorder;
  mojo::Receiver<network::mojom::P2PNetworkNotificationClient> client(
      &recorder);
  manager->StartNetworkNotifications(client.BindNewPipeAndPassRemote());
  EXPECT_EQ(0u, recorder.networks_.Get());

  mojo::Remote<network::mojom::P2PSocket> socket;
  mojo::PendingRemote<network::mojom::P2PSocketClient> socket_client;
  auto socket_client_receiver = socket_client.InitWithNewPipeAndPassReceiver();
  manager->CreateSocket(
      network::P2PSocketType::P2P_SOCKET_UDP, net::IPEndPoint(),
      network::P2PPortRange(), network::P2PHostAndIPEndPoint(),
      net::MutableNetworkTrafficAnnotationTag(), std::nullopt,
      std::move(socket_client), socket.BindNewPipeAndPassReceiver());
  base::RunLoop disconnected;
  socket.set_disconnect_handler(disconnected.QuitClosure());
  disconnected.Run();
  EXPECT_FALSE(socket.is_connected());
  EXPECT_TRUE(manager.is_connected());
}

TEST_F(AppPageFrameGuardTest, OtherFramesKeepTheirSocketManager) {
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  bool sentinel_bound = false;
  mojo::BinderMapWithContext<content::RenderFrameHost*> map;
  map.Add<network::mojom::P2PSocketManager>(
      base::BindRepeating(&BindSentinel, &sentinel_bound));
  OverrideAppPageFrameBinders(*kernel, map);

  mojo::Remote<network::mojom::P2PSocketManager> manager;
  mojo::GenericPendingReceiver receiver(manager.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(map.TryBind(kernel, &receiver));
  EXPECT_TRUE(sentinel_bound);
}
#endif  // BUILDFLAG(IS_P2P_ENABLED)

void BindHintsSentinel(
    bool* bound,
    content::RenderFrameHost*,
    mojo::PendingReceiver<network_hints::mojom::NetworkHintsHandler>) {
  *bound = true;
}

TEST_F(AppPageFrameGuardTest, AnAppPageFrameIgnoresNetworkHints) {
  content::RenderFrameHost* page = AddGuestChild(main_rfh(), kAppPageFrameName);
  bool sentinel_bound = false;
  mojo::BinderMapWithContext<content::RenderFrameHost*> map;
  map.Add<network_hints::mojom::NetworkHintsHandler>(
      base::BindRepeating(&BindHintsSentinel, &sentinel_bound));
  OverrideAppPageFrameBinders(*page, map);

  mojo::Remote<network_hints::mojom::NetworkHintsHandler> hints;
  mojo::GenericPendingReceiver receiver(hints.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(map.TryBind(page, &receiver));
  EXPECT_FALSE(sentinel_bound);

  hints->PrefetchDNS({url::SchemeHostPort(GURL("https://example.test/"))});
  hints->Preconnect(url::SchemeHostPort(GURL("https://example.test/")),
                    /*allow_credentials=*/false);
  hints.FlushForTesting();
  EXPECT_TRUE(hints.is_connected());
}

TEST_F(AppPageFrameGuardTest, OtherFramesKeepTheirNetworkHints) {
  content::RenderFrameHost* kernel = AddGuestChild(main_rfh(), "");
  bool sentinel_bound = false;
  mojo::BinderMapWithContext<content::RenderFrameHost*> map;
  map.Add<network_hints::mojom::NetworkHintsHandler>(
      base::BindRepeating(&BindHintsSentinel, &sentinel_bound));
  OverrideAppPageFrameBinders(*kernel, map);

  mojo::Remote<network_hints::mojom::NetworkHintsHandler> hints;
  mojo::GenericPendingReceiver receiver(hints.BindNewPipeAndPassReceiver());
  ASSERT_TRUE(map.TryBind(kernel, &receiver));
  EXPECT_TRUE(sentinel_bound);
}

}  // namespace
}  // namespace nexus
