// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SINGLE_WEB_WINDOW_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SINGLE_WEB_WINDOW_H_

#include <optional>
#include <string>

#include "ui/base/mojom/window_show_state.mojom-shared.h"
#include "ui/gfx/geometry/rect.h"

class BrowserWindowInterface;
class GURL;
class Profile;

namespace nexus {

struct SingleWebWindowParams {
  SingleWebWindowParams();
  SingleWebWindowParams(const SingleWebWindowParams&);
  SingleWebWindowParams& operator=(const SingleWebWindowParams&);
  ~SingleWebWindowParams();

  // Empty: the window shows its page's own title.
  std::u16string title;
  // Absent: the platform picks the bounds.
  std::optional<gfx::Rect> bounds;
  // With `bounds`: whether its origin was asked for, or only its size.
  bool origin_specified = false;
  ui::mojom::WindowShowState show_state = ui::mojom::WindowShowState::kNormal;
  bool focus = true;
};

// Opens `url` in a new single-web window (kSingleWeb, F-29) of `profile`:
// one tab, never session-restored. Returns null when no window can be
// created now. Contract: UI thread; `url` already passed the caller's own
// scheme check.
BrowserWindowInterface* OpenSingleWebWindow(Profile* profile,
                                            const GURL& url,
                                            const SingleWebWindowParams& params);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_WEB_WINDOW_SINGLE_WEB_WINDOW_H_
