// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PAGE_SCRIPT_PAGE_SCRIPT_CHANNEL_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PAGE_SCRIPT_PAGE_SCRIPT_CHANNEL_H_

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>

#include "base/containers/span.h"
#include "base/functional/callback.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/values.h"
#include "content/public/browser/devtools_agent_host.h"
#include "content/public/browser/devtools_agent_host_client.h"
#include "content/public/browser/web_contents_observer.h"
#include "custom_browser/browser/nexus/api/page_script/page_script_options.h"

namespace content {
class WebContents;
}  // namespace content

namespace nexus {

// One script channel to one page (F-29): a DevTools session held open on the
// page's WebContents for the channel's life. Its scripts run in a private
// world of the page's main frame; the page's own scripts see that world's
// objects only when the opener asked to expose them.
//
// Contract: UI thread only. Every request answers exactly once; a request
// made after the page is gone answers `no_document`. The delegate outlives
// the channel. Destroying the channel ends the session; init scripts stop
// running on the next document.
//
// Security: the channel is created by a resolved WebContents only — whoever
// resolves the target decides what may be reached. Scripts evaluate in the
// main frame; the bootstrap and init scripts also start in same-process
// subframes, but the bootstrap defines nothing outside the top frame and a
// message is accepted only from a main-frame context (its origin is the
// context's own, as the page reported it when the context was created).
// A page's own scripts cannot call the host, nor hear its messages, unless
// exposed; exposed, every script of the page can read what the host posts.
class PageScriptChannel : public content::DevToolsAgentHostClient,
                          public content::WebContentsObserver {
 public:
  class Delegate {
   public:
    virtual ~Delegate() = default;
    // A message from a channel script: `payload` carries `data` (JSON text),
    // `origin`, `frameUrl` and `world` ('isolated' | 'main').
    virtual void OnPageScriptMessage(PageScriptChannel* channel,
                                     base::DictValue payload) = 0;
    // The page is gone (closed, crashed, or its session ended): the
    // channel answers nothing more and may be deleted by the delegate.
    virtual void OnPageScriptDetached(PageScriptChannel* channel) = 0;
  };

  // A completed request. Exactly one of: a `value` (which may be `none` for
  // undefined), an `exception` (the thrown error's text), or an error code
  // with its params.
  struct Outcome {
    Outcome();
    Outcome(Outcome&&) noexcept;
    Outcome& operator=(Outcome&&) noexcept;
    ~Outcome();

    base::Value value;
    std::optional<std::string> exception;
    std::string error_code;
    base::DictValue error_params;
  };
  using OutcomeCallback = base::OnceCallback<void(Outcome)>;

  // Attaches to `web_contents`; null when the DevTools session could not be
  // opened (`attach_refused`).
  static std::unique_ptr<PageScriptChannel> Attach(
      content::WebContents* web_contents,
      bool expose_to_page,
      Delegate* delegate);

  PageScriptChannel(const PageScriptChannel&) = delete;
  PageScriptChannel& operator=(const PageScriptChannel&) = delete;
  ~PageScriptChannel() override;

  content::WebContents* contents() const { return web_contents(); }

  // Evaluates `code` in the main frame, awaiting a promise, and answers its
  // JSON value or exception.
  void ExecuteScript(const std::string& code,
                     PageScriptWorld world,
                     OutcomeCallback done);
  // Hands `json` to the listeners the private world's scripts registered,
  // then, on an exposed channel, to those the page's own scripts registered;
  // `value` is their count, -1 when no document has run the bootstrap yet.
  void PostMessage(const std::string& json, OutcomeCallback done);
  // Registers `source` to run in the private world at every new document;
  // `value` is the script id.
  void AddInitScript(const std::string& source, OutcomeCallback done);
  // Unregisters a script id this channel issued; `value` is false for an
  // unknown id.
  void RemoveInitScript(const std::string& script_id, OutcomeCallback done);

 private:
  using ResponseCallback =
      base::OnceCallback<void(const base::DictValue* result,
                              const base::DictValue* error)>;

  PageScriptChannel(content::WebContents* web_contents,
                    bool expose_to_page,
                    Delegate* delegate);

  bool Start();

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

  // Sends one command; `on_response` runs with the result or the error, or
  // with neither when nothing could be sent (false).
  bool Send(std::string_view method,
            base::DictValue params,
            ResponseCallback on_response);
  void OnEvent(std::string_view method, const base::DictValue& params);

  // The private world's context in the main frame, creating it when the
  // page has not run the bootstrap there yet.
  void WithIsolatedContext(
      base::OnceCallback<void(std::optional<int> context_id)> then);
  void Evaluate(const std::string& expression,
                std::optional<int> context_id,
                OutcomeCallback done);

  // Ends the session and tells the delegate once.
  void Detach();
  // Answers every open request with no result and no error (`no_document`).
  void FailPending();

  std::string MainFrameId() const;

  raw_ptr<Delegate> delegate_;
  const bool expose_to_page_;
  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  bool detached_ = false;
  int next_id_ = 1;
  std::map<int, ResponseCallback> pending_;
  // The private world's execution context in the main frame, once seen.
  std::optional<int> isolated_context_id_;
  // Every live execution context of the page: which frame it belongs to and
  // its origin, from `Runtime.executionContextCreated`.
  struct ContextInfo {
    std::string frame_id;
    std::string origin;
  };
  std::map<int, ContextInfo> contexts_;
  // Init-script ids this channel issued (`Page.addScriptToEvaluateOnNewDocument`).
  std::set<std::string> init_scripts_;
  base::WeakPtrFactory<PageScriptChannel> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_PAGE_SCRIPT_PAGE_SCRIPT_CHANNEL_H_
