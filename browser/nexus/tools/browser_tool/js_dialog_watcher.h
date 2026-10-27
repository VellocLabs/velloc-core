// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_WATCHER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_WATCHER_H_

// Tracks the native JavaScript dialog of one tab over a DevTools client
// (`Page.javascriptDialogOpening` / `Page.javascriptDialogClosed`) and answers
// it with `Page.handleJavaScriptDialog` (F-21). UI thread.
//
// Security: the attached client also keeps a background tab's dialogs live
// instead of auto-answered, so the model, not Chromium, decides them.

#include <cstdint>
#include <map>
#include <optional>
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
#include "custom_browser/browser/nexus/tools/browser_tool/js_dialog.h"

namespace nexus {

class JsDialogWatcher : public content::DevToolsAgentHostClient,
                        public content::WebContentsObserver {
 public:
  class Delegate {
   public:
    virtual void OnJsDialogOpened(int32_t tab_id) = 0;
    virtual void OnJsDialogClosed(int32_t tab_id) = 0;

   protected:
    virtual ~Delegate() = default;
  };

  // `ok` is false when the host is gone or rejected the answer.
  using AnswerCallback = base::OnceCallback<void(bool ok)>;

  JsDialogWatcher(content::WebContents* web_contents,
                  int32_t tab_id,
                  Delegate* delegate);
  ~JsDialogWatcher() override;

  JsDialogWatcher(const JsDialogWatcher&) = delete;
  JsDialogWatcher& operator=(const JsDialogWatcher&) = delete;

  // Attaches and enables the Page domain. False when the tab has no host.
  bool Start();

  int32_t tab_id() const { return tab_id_; }

  // The dialog open now, if any.
  OpenJsDialog* open_dialog() {
    return open_dialog_ ? &*open_dialog_ : nullptr;
  }

  // Accepts or dismisses the open dialog; an accepted prompt sends
  // `prompt_text` or, without it, the default value.
  void Answer(bool accept, AnswerCallback done);

  // content::DevToolsAgentHostClient:
  void DispatchProtocolMessage(content::DevToolsAgentHost* agent_host,
                               base::span<const uint8_t> message) override;
  void AgentHostClosed(content::DevToolsAgentHost* agent_host) override;

  // content::WebContentsObserver:
  void WebContentsDestroyed() override;

 private:
  void Send(std::string_view method,
            base::DictValue params,
            AnswerCallback on_response);
  void Detach();

  const int32_t tab_id_;
  const raw_ptr<Delegate> delegate_;
  scoped_refptr<content::DevToolsAgentHost> agent_host_;
  std::optional<OpenJsDialog> open_dialog_;
  int next_id_ = 1;
  std::map<int, AnswerCallback> pending_;
  base::WeakPtrFactory<JsDialogWatcher> weak_ptr_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_JS_DIALOG_WATCHER_H_
