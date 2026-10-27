// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_PROVIDER_SIGN_IN_WINDOWS_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_PROVIDER_SIGN_IN_WINDOWS_H_

#include <cstdint>
#include <map>
#include <memory>
#include <string>

#include "base/memory/raw_ptr.h"
#include "base/memory/weak_ptr.h"
#include "base/supports_user_data.h"

class GURL;
class Profile;

namespace nexus {

// Contract: the sign-in windows of a profile's subscription providers
// (F-29). UI thread; owned by its Profile (user data). One single-web
// window per provider, titled "Sign in to {provider}"; a new one replaces
// the earlier. A PKCE login's window captures the loopback redirect
// (kNexusOAuthRedirectUri), shows "Finishing sign-in" while the
// NexusOAuthManager exchanges the code, then the result. A device-code
// login's window shows the verification page, then the result the manager
// reports. Success closes the window kSignInSuccessHold later; failure
// leaves it open. Closing a device-code login's window by hand cancels the
// login.
class ProviderSignInWindows : public base::SupportsUserData::Data {
 public:
  static ProviderSignInWindows* GetForProfile(Profile* profile);

  explicit ProviderSignInWindows(Profile* profile);
  ~ProviderSignInWindows() override;

  ProviderSignInWindows(const ProviderSignInWindows&) = delete;
  ProviderSignInWindows& operator=(const ProviderSignInWindows&) = delete;

  // Opens `auth_url` (NexusOAuthManager::BeginLogin) armed to capture the
  // loopback redirect. False when no window could be opened.
  bool OpenForRedirect(const std::string& provider_id, const GURL& auth_url);

  // Opens the verification page of a started device-code login. False when
  // no window could be opened.
  bool OpenForDeviceCode(const std::string& provider_id,
                         const GURL& verification_url);

  // Closes `provider_id`'s window without cancelling anything: a new login
  // is about to start.
  void Discard(const std::string& provider_id);

  // The device-code login's end (NexusOAuthManager::DeviceCodeDoneCallback).
  void OnDeviceCodeDone(const std::string& provider_id,
                        bool ok,
                        std::string error);

  base::WeakPtr<ProviderSignInWindows> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

 private:
  class Window;

  Window* Open(const std::string& provider_id,
               const GURL& url,
               bool device_code);
  // `provider_id`'s window if it is still the one numbered `serial`.
  Window* Find(const std::string& provider_id, uint64_t serial);

  void OnRedirectCaptured(const std::string& provider_id,
                          uint64_t serial,
                          const GURL& url);
  void OnAuthCodeExchanged(const std::string& provider_id,
                           uint64_t serial,
                           bool ok,
                           std::string error);
  void OnWindowClosed(const std::string& provider_id, uint64_t serial);

  raw_ptr<Profile> profile_;
  std::map<std::string, std::unique_ptr<Window>> windows_;
  uint64_t next_serial_ = 1;
  base::WeakPtrFactory<ProviderSignInWindows> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_PROVIDERS_PROVIDER_SIGN_IN_WINDOWS_H_
