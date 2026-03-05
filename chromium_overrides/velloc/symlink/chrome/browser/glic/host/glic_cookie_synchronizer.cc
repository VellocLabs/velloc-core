// Copyright 2025 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/glic/host/glic_cookie_synchronizer.h"

#include <iterator>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/check.h"
#include "base/feature_list.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/metrics/user_metrics.h"
#include "base/time/time.h"
#include "chrome/browser/glic/fre/fre_util.h"
#include "chrome/browser/glic/host/auth_controller.h"
#include "chrome/common/chrome_features.h"
#include "components/signin/public/base/consent_level.h"
#include "components/signin/public/base/multilogin_parameters.h"
#include "components/signin/public/base/signin_buildflags.h"
#include "components/signin/public/identity_manager/identity_manager.h"
#include "components/signin/public/identity_manager/set_accounts_in_cookie_result.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/storage_partition.h"
#include "content/public/browser/storage_partition_config.h"
#include "google_apis/gaia/core_account_id.h"
#include "google_apis/gaia/gaia_auth_fetcher.h"
#include "mojo/public/cpp/bindings/callback_helpers.h"
#include "net/cookies/cookie_util.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "services/network/public/cpp/resource_request.h"
#include "services/network/public/cpp/shared_url_loader_factory.h"
#include "services/network/public/cpp/simple_url_loader.h"
#include "services/network/public/mojom/cookie_manager.mojom.h"
#include "services/network/public/mojom/fetch_api.mojom-shared.h"
#include "url/gurl.h"

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
#include "components/signin/public/base/signin_switches.h"
#endif  // BUILDFLAG(ENABLE_DICE_SUPPORT)

namespace glic {

BASE_FEATURE(kGlicCookieSyncTimeout, base::FEATURE_ENABLED_BY_DEFAULT);
BASE_FEATURE_PARAM(base::TimeDelta,
                   kGlicCookieSyncTimeoutDuration,
                   &kGlicCookieSyncTimeout,
                   "glic_cookie_sync_timeout_duration",
                   GlicCookieSynchronizer::kCookieSyncDefaultTimeout);

namespace {

content::StoragePartitionConfig GetGlicMainStoragePartitionConfig(
    content::BrowserContext* browser_context) {
  // This storage partition must match the partition attribute in
  // chrome/browser/resources/glic/glic.html: "persist:glicpart".
  return content::StoragePartitionConfig::Create(browser_context, "glic",
                                                 /*partition_name=*/"glicpart",
                                                 /*in_memory=*/false);
}

content::StoragePartitionConfig GetGlicStoragePartitionConfig(
    content::BrowserContext* browser_context,
    bool use_for_fre) {
  return use_for_fre ? GetFreStoragePartitionConfig(browser_context)
                     : GetGlicMainStoragePartitionConfig(browser_context);
}

bool IsGoogleAuthCookieName(const std::string& cookie_name) {
  // These cookies indicate a signed-in Google web session.
  static constexpr std::string_view kAuthCookieNames[] = {
      "SID",              "HSID",             "SSID",
      "APISID",           "SAPISID",          "LSID",
      "__Secure-1PSID",   "__Secure-3PSID",   "__Secure-1PSIDTS",
      "__Secure-3PSIDTS", "__Secure-1PSIDCC", "__Secure-3PSIDCC",
  };
  for (std::string_view name : kAuthCookieNames) {
    if (cookie_name == name) {
      return true;
    }
  }
  return false;
}

}  // namespace

// Synchronize cookies from the main partition to the webview partition. This
// is an alternative way to sync cookies which supports glic development, and
// won't be part of the launched feature.
class GlicCookieSynchronizer::SyncCookiesForDevelopmentTask {
 public:
  SyncCookiesForDevelopmentTask(content::BrowserContext* browser_context,
                                bool use_for_fre,
                                std::vector<GURL> urls,
                                base::OnceCallback<void(bool)> callback)
      : browser_context_(browser_context),
        use_for_fre_(use_for_fre),
        urls_(std::move(urls)),
        callback_(std::move(callback)) {
    network::mojom::CookieManager* cookie_manager =
        browser_context_->GetDefaultStoragePartition()
            ->GetCookieManagerForBrowserProcess();
    ::net::CookieOptions cookie_options;

    // This isn't a typical cookie access.
    cookie_options.set_do_not_update_access_time();
    // Cookies we need to set are HttpOnly.
    cookie_options.set_include_httponly();
    for (const GURL& url : urls_) {
      ++get_cookie_list_request_count_;
      cookie_manager->GetCookieList(
          url, cookie_options, {},
          mojo::WrapCallbackWithDropHandler(
              base::BindOnce(&SyncCookiesForDevelopmentTask::WriteCookies,
                             GetWeakPtr()),
              base::BindOnce(
                  &SyncCookiesForDevelopmentTask::WriteCookiesDropped,
                  GetWeakPtr())));
    }
  }

  ~SyncCookiesForDevelopmentTask() {
    if (callback_) {
      std::move(callback_).Run(false);
    }
  }

 private:
  void WriteCookiesDropped() {
    --get_cookie_list_request_count_;
    failed_ = true;
    FinishIfComplete();
  }

  void WriteCookies(
      const std::vector<::net::CookieWithAccessResult>& cookies,
      const std::vector<::net::CookieWithAccessResult>& excluded_cookies) {
    --get_cookie_list_request_count_;
    content::StoragePartition* webview_storage =
        browser_context_->GetStoragePartition(
            GetGlicStoragePartitionConfig(browser_context_, use_for_fre_));
    network::mojom::CookieManager* webview_cookie_manager =
        webview_storage->GetCookieManagerForBrowserProcess();

    for (const net::CookieWithAccessResult& cookie_with_access : cookies) {
      const net::CanonicalCookie& cookie = cookie_with_access.cookie;
      if (!cookie_with_access.access_result.status.IsInclude()) {
        continue;
      }
      if (!cookie.Value().empty() && IsGoogleAuthCookieName(cookie.Name())) {
        if (!cookie.IsExpired(base::Time::Now())) {
          found_auth_cookie_ = true;
        }
      }

      net::CookieOptions options;
      // Cookies we need to set are HttpOnly.
      options.set_include_httponly();
      // Permit it to set a SameSite cookie if it wants to.
      options.set_same_site_cookie_context(
          net::CookieOptions::SameSiteCookieContext::MakeInclusive());
      ++set_cookie_request_count_;
      webview_cookie_manager->SetCanonicalCookie(
          cookie, net::cookie_util::SimulatedCookieSource(cookie, "https"),
          options,
          mojo::WrapCallbackWithDropHandler(
              base::BindOnce(&SyncCookiesForDevelopmentTask::SetCookieComplete,
                             GetWeakPtr()),
              base::BindOnce(&SyncCookiesForDevelopmentTask::SetCookieDropped,
                             GetWeakPtr())));
    }
    FinishIfComplete();
  }

  void SetCookieComplete(::net::CookieAccessResult result) {
    --set_cookie_request_count_;
    FinishIfComplete();
  }

  void SetCookieDropped() {
    failed_ = true;
    --set_cookie_request_count_;
    FinishIfComplete();
  }

  void FinishIfComplete() {
    if (set_cookie_request_count_ != 0 || get_cookie_list_request_count_ != 0) {
      return;
    }
    if (!failed_ && found_auth_cookie_) {
      // Cookies look OK locally. Validate that the Google session is actually
      // active by making a request to accounts.google.com. A signed-out
      // session will redirect to the sign-in page.
      ValidateSession();
      return;
    }
    RunCallback();
  }

  void ValidateSession() {
    auto resource_request = std::make_unique<network::ResourceRequest>();
    resource_request->url = GURL("https://accounts.google.com/");
    resource_request->method = "GET";
    resource_request->credentials_mode =
        network::mojom::CredentialsMode::kInclude;

    validation_loader_ = network::SimpleURLLoader::Create(
        std::move(resource_request),
        MISSING_TRAFFIC_ANNOTATION);
    validation_loader_->SetTimeoutDuration(base::Seconds(10));
    validation_loader_->SetAllowHttpErrorResults(true);

    auto url_loader_factory =
        browser_context_->GetDefaultStoragePartition()
            ->GetURLLoaderFactoryForBrowserProcess();
    validation_loader_->DownloadToString(
        url_loader_factory.get(),
        base::BindOnce(
            &SyncCookiesForDevelopmentTask::OnSessionValidationComplete,
            GetWeakPtr()),
        /*max_body_size=*/1024);
  }

  void OnSessionValidationComplete(std::unique_ptr<std::string> body) {
    bool session_valid = true;
    if (validation_loader_) {
      GURL final_url = validation_loader_->GetFinalURL();
      std::string url_str = final_url.spec();
      if (url_str.find("ServiceLogin") != std::string::npos ||
          url_str.find("/v3/signin") != std::string::npos ||
          url_str.find("/signin/identifier") != std::string::npos) {
        session_valid = false;
      }
      validation_loader_.reset();
    }
    if (!session_valid) {
      found_auth_cookie_ = false;
    }
    RunCallback();
  }

  void RunCallback() {
    bool result = !failed_ && found_auth_cookie_;
    std::move(callback_).Run(result);
  }

  base::WeakPtr<SyncCookiesForDevelopmentTask> GetWeakPtr() {
    return weak_ptr_factory_.GetWeakPtr();
  }

  const raw_ptr<content::BrowserContext> browser_context_;
  bool use_for_fre_ = false;
  std::vector<GURL> urls_;
  base::OnceCallback<void(bool)> callback_;
  int set_cookie_request_count_ = 0;
  int get_cookie_list_request_count_ = 0;
  bool failed_ = false;
  bool found_auth_cookie_ = false;
  std::unique_ptr<network::SimpleURLLoader> validation_loader_;
  base::WeakPtrFactory<SyncCookiesForDevelopmentTask> weak_ptr_factory_{this};
};

std::vector<GURL> GetDevelopmentCookieSyncUrls(bool is_google_internal) {
  std::vector<GURL> urls;
  if (is_google_internal) {
    urls.emplace_back("https://login.corp.google.com");
    urls.emplace_back("https://googleplex.com");
  }
  // Consumer Google domains needed for Gemini.
  urls.emplace_back("https://accounts.google.com");
  urls.emplace_back("https://google.com");
  urls.emplace_back("https://gemini.google.com");
  return urls;
}

GlicCookieSynchronizer::GlicCookieSynchronizer(
    content::BrowserContext* context,
    signin::IdentityManager* identity_manager,
    bool use_for_fre)
    : context_(context),
      identity_manager_(identity_manager),
      use_for_fre_(use_for_fre) {
  CHECK(context_);
  observation_.Observe(identity_manager);
}

GlicCookieSynchronizer::~GlicCookieSynchronizer() = default;

std::unique_ptr<GaiaAuthFetcher>
GlicCookieSynchronizer::CreateGaiaAuthFetcherForPartition(
    GaiaAuthConsumer* consumer,
    const gaia::GaiaSource& source) {
  return std::make_unique<GaiaAuthFetcher>(
      consumer, source,
      GetStoragePartition()->GetURLLoaderFactoryForBrowserProcess());
}

network::mojom::CookieManager*
GlicCookieSynchronizer::GetCookieManagerForPartition() {
  return GetStoragePartition()->GetCookieManagerForBrowserProcess();
}

#if BUILDFLAG(ENABLE_DICE_SUPPORT)
network::mojom::DeviceBoundSessionManager*
GlicCookieSynchronizer::GetDeviceBoundSessionManagerForPartition() {
  if (!base::FeatureList::IsEnabled(
          switches::
              kEnableOAuthMultiloginStandardCookiesBindingForGlicPartition)) {
    return nullptr;
  }
  return GetStoragePartition()->GetDeviceBoundSessionManager();
}
#endif  // BUILDFLAG(ENABLE_DICE_SUPPORT)

void GlicCookieSynchronizer::CopyCookiesToWebviewStoragePartition(
    OnWebviewAuth callback) {
  CHECK(!callback.is_null());
  callbacks_.push_back(std::move(callback));

  if (cookie_loader_ || sync_cookies_for_development_task_) {
    // A request is in progress already.
    return;
  }

  if (base::FeatureList::IsEnabled(kGlicCookieSyncTimeout)) {
    timeout_.Start(FROM_HERE, kGlicCookieSyncTimeoutDuration.Get(),
                   base::BindOnce(&GlicCookieSynchronizer::OnTimeout,
                                  base::Unretained(this)));
  }

  if (!GetStoragePartition()) {
    DLOG(ERROR) << "glic webview storage partition does not exist";
    CompleteAuth(false);
    return;
  }

  const bool dev_cookie_sync_enabled =
      base::FeatureList::IsEnabled(
          features::kGlicDevelopmentSyncGoogleCookies);
  const bool is_google_internal =
      identity_manager_ && IsPrimaryAccountGoogleInternal(*identity_manager_);
  if (dev_cookie_sync_enabled) {
    sync_cookies_for_development_task_ =
        std::make_unique<SyncCookiesForDevelopmentTask>(
            context_, use_for_fre_,
            GetDevelopmentCookieSyncUrls(is_google_internal),
            base::BindOnce(
                &GlicCookieSynchronizer::SyncCookiesForDevelopmentComplete,
                GetWeakPtr()));
  } else {
    BeginCookieSync();
  }
}

void GlicCookieSynchronizer::OnIdentityManagerShutdown(
    signin::IdentityManager* identity_manager) {
  observation_.Reset();
  cookie_loader_.reset();
}

void GlicCookieSynchronizer::SyncCookiesForDevelopmentComplete(bool success) {
  sync_cookies_for_development_task_.reset();
  if (!success) {
    CompleteAuth(/*is_success=*/false);
    return;
  }
  // In development cookie-sync mode, skip GAIA multilogin. This allows
  // custom/non-official builds (without Google API keys) to authenticate
  // via copied cookies alone.
  if (base::FeatureList::IsEnabled(
          features::kGlicDevelopmentSyncGoogleCookies)) {
    CompleteAuth(/*is_success=*/true);
    return;
  }
  BeginCookieSync();
}

void GlicCookieSynchronizer::BeginCookieSync() {
  // We only need primary account authentication in the webview.
  CoreAccountId primary_account_id =
      identity_manager_->GetPrimaryAccountId(signin::ConsentLevel::kSignin);
  if (primary_account_id.empty()) {
    DLOG(ERROR) << "can't sync cookies, user not signed in";
    CompleteAuth(false);
    return;
  }
  signin::MultiloginParameters parameters = {
      gaia::MultiloginMode::MULTILOGIN_UPDATE_COOKIE_ACCOUNTS_ORDER,
      {primary_account_id}};
  if (base::FeatureList::IsEnabled(features::kGlicIgnoreOfflineState)) {
    parameters.wait_on_connectivity = false;
  }
  cookie_loader_ =
      identity_manager_->GetAccountsCookieMutator()
          ->SetAccountsInCookieForPartition(
              this, parameters, gaia::GaiaSource::kChromeGlic,
              base::BindOnce(&GlicCookieSynchronizer::OnAuthFinished,
                             GetWeakPtr()));
}

void GlicCookieSynchronizer::OnAuthFinished(
    signin::SetAccountsInCookieResult cookie_result) {
  switch (cookie_result) {
    case signin::SetAccountsInCookieResult::kSuccess:
      CompleteAuth(/*is_success=*/true);
      break;
    case signin::SetAccountsInCookieResult::kTransientError:
      CompleteAuth(/*is_success=*/false);
      break;
    case signin::SetAccountsInCookieResult::kPersistentError:
      CompleteAuth(/*is_success=*/false);
      break;
  }
}

void GlicCookieSynchronizer::OnTimeout() {
  base::RecordAction(
      base::UserMetricsAction("Glic.CookieSynchronizer.Timeout"));
  CompleteAuth(/*is_success=*/false);
}

void GlicCookieSynchronizer::CompleteAuth(bool is_success) {
  timeout_.Stop();
  cookie_loader_.reset();

  std::vector<base::OnceCallback<void(bool)>> callbacks;
  std::swap(callbacks, callbacks_);

  for (auto i = std::make_move_iterator(callbacks.begin());
       i != std::make_move_iterator(callbacks.end()); ++i) {
    (*i).Run(is_success);
  }
}

content::StoragePartition* GlicCookieSynchronizer::GetStoragePartition() {
  content::StoragePartition* partition = context_->GetStoragePartition(
      GetGlicStoragePartitionConfig(context_, use_for_fre_));
  return partition;
}

}  // namespace glic
