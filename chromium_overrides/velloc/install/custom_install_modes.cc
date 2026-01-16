// Copyright 2016 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Brand-specific constants and install modes for the custom browser.

#include "custom_browser/chromium_overrides/Velloc/install/custom_install_modes.h"

#include <stdlib.h>

#include "chrome/install_static/buildflags.h"
#include "chrome/install_static/install_modes.h"

namespace install_static {

const wchar_t kCompanyPathName[] = L"";

const wchar_t kProductPathName[] =
    BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT_WIDE);

const size_t kProductPathNameLength = _countof(kProductPathName) - 1;

const char kSafeBrowsingName[] = BUILDFLAG(CUSTOM_BRANDING_PATH_COMPONENT);

}  // namespace install_static
