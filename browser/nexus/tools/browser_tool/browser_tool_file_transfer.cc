#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool.h"

#include <utility>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/utf_string_conversions.h"
#include "base/task/thread_pool.h"
#include "base/time/time.h"
#include "chrome/browser/profiles/profile.h"
#include "components/download/public/common/download_interrupt_reasons.h"
#include "components/download/public/common/download_item.h"
#include "components/download/public/common/download_url_parameters.h"
#include "components/tabs/public/tab_interface.h"
#include "content/public/browser/download_manager.h"
#include "content/public/browser/render_frame_host.h"
#include "content/public/browser/web_contents.h"
#include "custom_browser/browser/nexus/tools/browser_tool/browser_tool_internal.h"
#include "custom_browser/browser/nexus/tools/browser_tool/download_capture.h"
#include "custom_browser/browser/nexus/tools/tool_session.h"
#include "custom_browser/browser/nexus/upload/nexus_file_chooser_interceptor.h"
#include "net/traffic_annotation/network_traffic_annotation.h"
#include "url/gurl.h"

namespace nexus {

using namespace browser_tool_internal;  // NOLINT(build/namespaces)

namespace {

namespace apc = optimization_guide::proto;

// Traffic annotation for URL-mode BrowserDownload. Click-mode downloads
// are page-initiated and carry the page's own annotation; only the
// programmatic `DownloadManager::DownloadUrl` path needs one here.
constexpr net::NetworkTrafficAnnotationTag kDownloadTrafficAnnotation =
    net::DefineNetworkTrafficAnnotation("nexus_browser_download", R"(
      semantics {
        sender: "Nexus AI Agent Platform"
        description:
          "When the user's AI agent runs the BrowserDownload tool with a "
          "direct URL, the browser fetches that file using the active tab's "
          "session (cookies/credentials) and saves it into the agent's local "
          "workspace so the agent can read it."
        trigger:
          "The agent explicitly invokes the BrowserDownload tool with a url."
        data:
          "The requested URL and the active tab's cookies/credentials for "
          "that site."
        destination: WEBSITE
      }
      policy {
        cookies_allowed: YES
        cookies_store: "user"
        setting:
          "Controlled by the Nexus agent feature; only fires on an explicit "
          "agent tool call."
        policy_exception_justification:
          "Downloads are initiated only by explicit agent tool calls, "
          "mirroring a user clicking a download link."
      })");

}  // namespace

void BrowserTool::RunDownloadOnUI(::nexus::generated::BrowserDownloadInput input,
                                  ToolCallback done) {
  // Exactly one of index|url. (index = click to trigger a download whose
  // URL we can't see; url = direct download using the page's session.)
  const bool has_index = input.index.has_value();
  const bool has_url = input.url.has_value() && !input.url->empty();
  if (has_index == has_url) {
    std::move(done).Reject("invalid_args", ReasonParams("download_mode"));
    return;
  }

  // Downloads land in the session workspace's `downloads/` folder so the
  // file tools (which sandbox to the session cwd) can read them.
  ::nexus::ToolSession* session = GetSession();
  const base::FilePath cwd = session ? session->cwd() : base::FilePath();
  if (cwd.empty()) {
    std::move(done).Reject("no_workspace", ReasonParams("download"));
    return;
  }
  const base::FilePath dest_dir = cwd.Append(FILE_PATH_LITERAL("downloads"));

  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  if (!manager) {
    std::move(done).Reject("download_failed", ReasonParams("manager_unavailable"));
    return;
  }

  // Bound the wait on completion by the per-action budget; DownloadCapture
  // returns an in-progress result (rather than blocking) if a large file
  // is still streaming when this elapses.
  const base::TimeDelta budget = action_timeout_;

  // Optional caller filename — basename only, to block path traversal.
  std::optional<std::string> filename;
  if (input.filename && !input.filename->empty()) {
    filename = base::FilePath::FromUTF8Unsafe(*input.filename)
                   .BaseName()
                   .AsUTF8Unsafe();
  }

  if (has_url) {
    // Associate the download with the active tab so its cookies/auth apply
    // (the whole point of an in-browser download vs. a bare fetch).
    std::optional<int32_t> tab_id =
        ResolveTabIdOrReject(std::nullopt, GetProfile(), "download", done, this);
    if (!tab_id.has_value()) {
      return;
    }
    tabs::TabInterface* tab = tabs::TabHandle(*tab_id).Get();
    content::WebContents* contents = tab ? tab->GetContents() : nullptr;
    content::RenderFrameHost* rfh =
        contents ? contents->GetPrimaryMainFrame() : nullptr;
    if (!rfh) {
      std::move(done).Reject("no_active_tab", ReasonParams("download_page"));
      return;
    }
    GURL url(*input.url);
    if (!url.is_valid()) {
      {
      base::DictValue reject_params = ReasonParams("invalid_url");
      reject_params.Set("url", *input.url);
      std::move(done).Reject("invalid_args", std::move(reject_params));
    }
      return;
    }
    // Derive a name from the URL when the caller didn't supply one.
    std::string name = filename.value_or(url.ExtractFileName());
    if (name.empty()) {
      name = "download";
    }
    // We force the download straight into <cwd>/downloads/ (so it streams
    // there with no post-completion move and no "Save As" prompt), but the
    // download writer won't create that directory and a freshly-created
    // session workspace doesn't have it yet — without this the transfer
    // interrupts with FILE_FAILED. Create the dir on a blocking pool, then
    // resume on the UI thread to start the download. Match nexus_fs_host's
    // ctor pattern (hop to thread_pool for the blocking CreateDirectory).
    base::ThreadPool::PostTaskAndReplyWithResult(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
        base::BindOnce(
            [](base::FilePath dir) { return base::CreateDirectory(dir); },
            dest_dir),
        base::BindOnce(&BrowserTool::StartUrlDownloadAfterDirReady,
                       weak_ptr_factory_.GetWeakPtr(), *tab_id, *input.url,
                       dest_dir, std::move(name), budget, std::move(done)));
    return;
  }

  // index mode: arm the capture, then click the element to trigger the
  // download. The click's own post-action observation is discarded (sink
  // callback) — DownloadCapture resolves the model's `done` when the
  // download the click produces completes (or the budget elapses, or no
  // download starts within the arm window).
  const NodeRegistry::Entry* entry =
      LookupNode(*input.index, std::nullopt, done, "download");
  if (!entry) {
    return;
  }

  scoped_refptr<DownloadCapture> capture = DownloadCapture::Create(
      manager, dest_dir, std::move(filename), budget, std::move(done));
  // 15s window for the click to actually start a download (covers the
  // actor round-trip + the page's navigation/JS that kicks it off).
  capture->ArmForClickCapture(base::Seconds(15));

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "download", ToolCallback(),
                 entry->tab_id);
}

void BrowserTool::StartUrlDownloadAfterDirReady(int32_t tab_id,
                                                std::string url,
                                                base::FilePath dest_dir,
                                                std::string name,
                                                base::TimeDelta budget,
                                                ToolCallback done,
                                                bool dir_ok) {
  if (!dir_ok) {
    std::move(done).Reject("download_failed", ReasonParams("downloads_dir"));
    return;
  }
  // Re-resolve everything on the UI thread — pointers can't safely cross the
  // mkdir thread hop, and the tab/manager may have changed while we waited.
  content::DownloadManager* manager =
      GetProfile() ? GetProfile()->GetDownloadManager() : nullptr;
  if (!manager) {
    std::move(done).Reject("download_failed", ReasonParams("manager_unavailable"));
    return;
  }
  tabs::TabInterface* tab = tabs::TabHandle(tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  content::RenderFrameHost* rfh =
      contents ? contents->GetPrimaryMainFrame() : nullptr;
  if (!rfh) {
    std::move(done).Reject("no_active_tab", ReasonParams("download_page_gone"));
    return;
  }
  const base::FilePath dest =
      dest_dir.Append(base::FilePath::FromUTF8Unsafe(name));

  std::unique_ptr<download::DownloadUrlParameters> params =
      rfh->CreateDownloadUrlParameters(GURL(url), kDownloadTrafficAnnotation);
  params->set_file_path(dest);
  params->set_prompt(false);

  scoped_refptr<DownloadCapture> capture = DownloadCapture::Create(
      manager, dest_dir, std::move(name), budget, std::move(done));
  params->set_callback(base::BindOnce(
      [](scoped_refptr<DownloadCapture> cap, download::DownloadItem* item,
         download::DownloadInterruptReason reason) {
        cap->BindStartedItem(item,
                             reason == download::DOWNLOAD_INTERRUPT_REASON_NONE);
      },
      capture));
  manager->DownloadUrl(std::move(params),
                       /*blob_url_loader_factory=*/nullptr);
}

void BrowserTool::RunUploadOnUI(::nexus::generated::BrowserUploadInput input,
                                ToolCallback done) {
  // The file <input> (or the control that opens the picker). Its home tab
  // is the upload target.
  const NodeRegistry::Entry* entry =
      LookupNode(input.index, std::nullopt, done, "upload");
  if (!entry) {
    return;
  }

  // Resolve the local file path: absolute used as-is; relative resolves
  // against the session workspace (same sandbox the file tools use).
  base::FilePath path = base::FilePath::FromUTF8Unsafe(input.file_path);
  if (!path.IsAbsolute()) {
    ::nexus::ToolSession* session = GetSession();
    const base::FilePath cwd = session ? session->cwd() : base::FilePath();
    if (cwd.empty()) {
      std::move(done).Reject("no_workspace", ReasonParams("file_path"));
      return;
    }
    path = cwd.Append(path);
  }

  tabs::TabInterface* tab = tabs::TabHandle(entry->tab_id).Get();
  content::WebContents* contents = tab ? tab->GetContents() : nullptr;
  if (!contents) {
    std::move(done).Reject("no_active_tab", ReasonParams("target_tab_gone"));
    return;
  }

  // Stage the file for the next chooser on this tab, then click the input.
  // The guarded hook in Browser::RunFileChooser answers the chooser the
  // click opens with `path` (no native dialog). The click's post-action
  // observation flows back to the model so it can confirm the attachment.
  // NOTE: existence isn't checked here (UI thread); a missing file simply
  // yields a failed attachment the model can see in the next observation.
  NexusPendingUpload::Arm(contents, {path});

  apc::Actions actions;
  apc::ClickAction* click = actions.add_actions()->mutable_click();
  click->mutable_target()->set_content_node_id(entry->content_node_id);
  click->mutable_target()
      ->mutable_document_identifier()
      ->set_serialized_token(entry->document_id);
  click->set_tab_id(entry->tab_id);
  click->set_click_type(apc::ClickAction::LEFT);
  click->set_click_count(apc::ClickAction::SINGLE);
  RunActorAction(std::move(actions), "upload", std::move(done), entry->tab_id);
}

}  // namespace nexus
