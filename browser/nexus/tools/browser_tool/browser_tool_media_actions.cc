#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <string>
#include <utility>

#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/profiles/profile.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/media_session.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "services/media_session/public/cpp/media_metadata.h"
#include "services/media_session/public/cpp/media_position.h"
#include "services/media_session/public/mojom/media_session.mojom.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

}  // namespace

// Phase 13: `media_play` / `media_pause` / `media_seek` collapsed into
// a single `media_control` tool with an `action` discriminator. Three
// tools became one; one extra field traded for two fewer tools is a
// catalog-complexity win.
void BrowserTool::RunMediaControlOnUI(
    ::nexus::generated::BrowserMediaControlInput input, ToolCallback done) {
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "media_control", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  apc::Actions actions;
  apc::MediaControlAction* media =
      actions.add_actions()->mutable_media_control();
  media->set_tab_id(*resolved_tab_id);

  // The codegen extractor doesn't enforce string enums today, so the
  // body is the live check. `action` is a required field so the
  // extractor already rejects a missing key.
  if (input.action == "play") {
    media->mutable_play();
  } else if (input.action == "pause") {
    media->mutable_pause();
  } else if (input.action == "seek") {
    if (!input.seek_time_ms.has_value()) {
      std::move(done).Reject("invalid_args", ReasonParams("seek_time_required"));
      return;
    }
    // Negative seek positions are clamped to 0 — the proto doc says
    // "should be non-negative" and the renderer rejects negatives, so
    // catching it here gives a cleaner result than a generic actor
    // error.
    const int32_t ms = *input.seek_time_ms;
    media->mutable_seek()->set_seek_time_milliseconds(
        static_cast<int64_t>(ms < 0 ? 0 : ms));
  } else {
    {
      base::DictValue reject_params = ReasonParams("media_action");
      reject_params.Set("value", input.action);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
    return;
  }
  RunActorAction(std::move(actions), "media_control", std::move(done),
                  *resolved_tab_id);
}

void BrowserTool::RunMediaStateOnUI(::nexus::generated::BrowserMediaStateInput input,
                                    ToolCallback done) {
  // Synchronous read: no actor task, no PerformActions round-trip,
  // no APC fetch. We just resolve the target WebContents and read
  // MediaSession's three sync accessors.
  //
  // Phase 13: dropped tab_id from the schema; always reads the active tab.
  std::optional<int32_t> resolved_tab_id =
      ResolveTabIdOrReject(std::nullopt, GetProfile(), "media_state", done, this);
  if (!resolved_tab_id.has_value()) {
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(*resolved_tab_id).Get();
  if (!tab || !tab->GetContents()) {
    {
      base::DictValue reject_params;
      reject_params.Set("tool", "media_state");
      reject_params.Set("tab_id", *resolved_tab_id);
      std::move(done).Reject("tab_not_found", std::move(reject_params));
    }
    return;
  }

  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "media_state");
  result.Set("tab_id", *resolved_tab_id);

  // `GetIfExists` returns null when no media has ever been associated
  // with the tab — common before the user presses play. We surface
  // this as `has_session:false` (NOT an error) so the LLM can branch
  // without hitting an exception path.
  content::MediaSession* session =
      content::MediaSession::GetIfExists(tab->GetContents());
  if (!session) {
    result.Set("has_session", false);
    std::move(done).Resolve(std::move(result));
    return;
  }
  result.Set("has_session", true);

  // Playback state (playing/paused). The mojom enum has only those
  // two values today; we stringify for LLM legibility.
  if (auto info = session->GetMediaSessionInfoSync()) {
    const char* state =
        info->playback_state ==
                media_session::mojom::MediaPlaybackState::kPlaying
            ? "playing"
            : "paused";
    result.Set("playback_state", state);
  }

  // Position / duration / rate. Pages that don't call
  // `navigator.mediaSession.setPositionState()` won't populate this —
  // we just omit the fields rather than emit fake zeros, so the LLM
  // can tell "I don't know the duration" from "duration is 0".
  if (auto position = session->GetMediaSessionPosition()) {
    result.Set(
        "position_ms",
        static_cast<int>(
            position->GetPosition().InMilliseconds()));
    result.Set(
        "duration_ms",
        static_cast<int>(position->duration().InMilliseconds()));
    result.Set("playback_rate", position->playback_rate());
    if (position->end_of_media()) {
      result.Set("end_of_media", true);
    }
  }

  // Page-supplied metadata. UTF-16 → UTF-8 because base::Value strings
  // are UTF-8. Empty fields are omitted so the LLM doesn't waste
  // attention on blanks; pages frequently set only a subset.
  const media_session::MediaMetadata& meta =
      session->GetMediaSessionMetadata();
  auto set_if_nonempty = [&](std::string_view key,
                             const std::u16string& value) {
    if (!value.empty()) {
      result.Set(key, base::UTF16ToUTF8(value));
    }
  };
  set_if_nonempty("title", meta.title);
  set_if_nonempty("artist", meta.artist);
  set_if_nonempty("album", meta.album);
  set_if_nonempty("source_title", meta.source_title);

  std::move(done).Resolve(std::move(result));
}

}  // namespace nexus
