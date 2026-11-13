// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/network/nexus_proxy_login.h"

#include <string>
#include <utility>

#include "base/functional/bind.h"
#include "base/location.h"
#include "base/memory/weak_ptr.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "chrome/browser/profiles/profile.h"
#include "components/prefs/pref_service.h"
#include "content/public/browser/browser_context.h"
#include "custom_browser/browser/nexus/api/network/nexus_proxy_settings.h"
#include "custom_browser/browser/nexus/nexus_secret_crypt.h"
#include "net/base/host_port_pair.h"

namespace nexus {

namespace {

class StoredProxyLoginDelegate : public content::LoginDelegate {
 public:
  StoredProxyLoginDelegate(
      net::AuthCredentials credentials,
      content::LoginDelegate::LoginAuthRequiredCallback callback)
      : callback_(std::move(callback)) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce(&StoredProxyLoginDelegate::Answer,
                       weak_factory_.GetWeakPtr(), std::move(credentials)));
  }
  ~StoredProxyLoginDelegate() override = default;

 private:
  void Answer(net::AuthCredentials credentials) {
    std::move(callback_).Run(std::move(credentials));
  }

  content::LoginDelegate::LoginAuthRequiredCallback callback_;
  base::WeakPtrFactory<StoredProxyLoginDelegate> weak_factory_{this};
};

}  // namespace

std::optional<net::AuthCredentials> StoredProxyChallengeCredentials(
    const PrefService& prefs,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt) {
  if (!auth_info.is_proxy || !first_auth_attempt) {
    return std::nullopt;
  }
  const std::optional<ProxyAuthAnswer> answer = StoredProxyAuthFor(
      prefs, net::HostPortPair::FromSchemeHostPort(auth_info.challenger));
  if (!answer) {
    return std::nullopt;
  }
  const std::string password =
      NexusSecretCrypt::GetInstance()->Decrypt(answer->sealed_password);
  if (password.empty()) {
    return std::nullopt;
  }
  return net::AuthCredentials(base::UTF8ToUTF16(answer->username),
                              base::UTF8ToUTF16(password));
}

std::unique_ptr<content::LoginDelegate> MaybeAnswerProxyChallenge(
    content::BrowserContext* browser_context,
    const net::AuthChallengeInfo& auth_info,
    bool first_auth_attempt,
    content::LoginDelegate::LoginAuthRequiredCallback* callback) {
  if (!browser_context || !callback || !auth_info.is_proxy) {
    return nullptr;
  }
  const Profile* profile = Profile::FromBrowserContext(browser_context);
  const PrefService* prefs = profile ? profile->GetPrefs() : nullptr;
  if (!prefs) {
    return nullptr;
  }
  std::optional<net::AuthCredentials> credentials =
      StoredProxyChallengeCredentials(*prefs, auth_info, first_auth_attempt);
  if (!credentials) {
    return nullptr;
  }
  return std::make_unique<StoredProxyLoginDelegate>(std::move(*credentials),
                                                    std::move(*callback));
}

}  // namespace nexus
