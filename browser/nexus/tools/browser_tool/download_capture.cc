#include "custom_browser/browser/nexus/tools/browser_tool/download_capture.h"

#include <utility>

#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/logging.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/thread_pool.h"
#include "base/values.h"

namespace nexus {

// static
scoped_refptr<DownloadCapture> DownloadCapture::Create(
    content::DownloadManager* manager,
    base::FilePath dest_dir,
    std::optional<std::string> filename,
    base::TimeDelta budget,
    ToolCallback done) {
  return base::WrapRefCounted(new DownloadCapture(
      manager, std::move(dest_dir), std::move(filename), budget,
      std::move(done)));
}

DownloadCapture::DownloadCapture(content::DownloadManager* manager,
                                 base::FilePath dest_dir,
                                 std::optional<std::string> filename,
                                 base::TimeDelta budget,
                                 ToolCallback done)
    : manager_(manager),
      dest_dir_(std::move(dest_dir)),
      filename_(std::move(filename)),
      budget_(budget),
      done_(std::move(done)) {}

DownloadCapture::~DownloadCapture() = default;

void DownloadCapture::ArmForClickCapture(base::TimeDelta arm_window) {
  self_ = this;  // stay alive until Finalize().
  if (!manager_) {
    RejectAndFinalize("download_failed",
                      "the browser download manager is unavailable.");
    return;
  }
  manager_->AddObserver(this);
  observing_manager_ = true;
  arm_timer_.Start(FROM_HERE, arm_window,
                   base::BindOnce(&DownloadCapture::OnArmWindowElapsed,
                                  base::Unretained(this)));
}

void DownloadCapture::BindStartedItem(download::DownloadItem* item,
                                      bool started_ok) {
  self_ = this;  // stay alive until Finalize().
  if (!started_ok || !item) {
    RejectAndFinalize("download_failed",
                      "the download could not be started (bad URL, blocked, "
                      "or no network).");
    return;
  }
  StartObservingItem(item);
}

void DownloadCapture::OnDownloadCreated(content::DownloadManager* manager,
                                        download::DownloadItem* item) {
  // Capture the first NEW download created after arming; ignore the rest.
  if (finalized_ || item_) {
    return;
  }
  arm_timer_.Stop();
  StartObservingItem(item);
}

void DownloadCapture::ManagerGoingDown(content::DownloadManager* manager) {
  if (manager == manager_) {
    // Avoid touching the dying manager during teardown.
    if (observing_manager_) {
      manager_->RemoveObserver(this);
      observing_manager_ = false;
    }
    manager_ = nullptr;
  }
  RejectAndFinalize("download_failed",
                    "the browser download manager shut down before the "
                    "download completed.");
}

void DownloadCapture::StartObservingItem(download::DownloadItem* item) {
  item_ = item;
  item_->AddObserver(this);
  observing_item_ = true;
  // We have our item; stop watching the manager for further creations.
  if (observing_manager_ && manager_) {
    manager_->RemoveObserver(this);
    observing_manager_ = false;
  }
  budget_timer_.Start(FROM_HERE, budget_,
                      base::BindOnce(&DownloadCapture::OnBudgetElapsed,
                                     base::Unretained(this)));
  // The item may already be in a terminal state.
  OnDownloadUpdated(item_);
}

void DownloadCapture::OnDownloadUpdated(download::DownloadItem* item) {
  if (finalized_ || completing_ || item != item_) {
    return;
  }
  switch (item_->GetState()) {
    case download::DownloadItem::COMPLETE:
      OnItemComplete();
      break;
    case download::DownloadItem::CANCELLED:
      RejectAndFinalize("download_cancelled", "the download was cancelled.");
      break;
    case download::DownloadItem::INTERRUPTED:
      RejectAndFinalize(
          "download_failed",
          base::StrCat({"the download was interrupted (reason code ",
                        base::NumberToString(
                            static_cast<int>(item_->GetLastReason())),
                        ")."}));
      break;
    case download::DownloadItem::IN_PROGRESS:
    default:
      // Keep waiting; the budget timer bounds the wait.
      break;
  }
}

void DownloadCapture::OnDownloadDestroyed(download::DownloadItem* item) {
  if (item != item_) {
    return;
  }
  // The item is going away — don't RemoveObserver on a dead pointer.
  observing_item_ = false;
  item_ = nullptr;
  if (!completing_) {
    RejectAndFinalize("download_failed",
                      "the download was removed before it completed.");
  }
}

void DownloadCapture::OnItemComplete() {
  completing_ = true;
  budget_timer_.Stop();
  const base::FilePath src = item_->GetTargetFilePath();
  const double bytes = static_cast<double>(item_->GetReceivedBytes());
  // Destination: dest_dir + (caller filename | source basename). Take only
  // the basename of any caller-supplied filename to block path traversal.
  base::FilePath name =
      (filename_ && !filename_->empty())
          ? base::FilePath::FromUTF8Unsafe(*filename_).BaseName()
          : src.BaseName();
  const base::FilePath dest = dest_dir_.Append(name);

  if (src == dest) {
    // URL mode already streamed straight into the workspace.
    OnMoveDone(bytes, src, dest, /*moved=*/true);
    return;
  }
  base::ThreadPool::PostTaskAndReplyWithResult(
      FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_VISIBLE},
      base::BindOnce(&DownloadCapture::MoveFileBlocking, src, dest),
      base::BindOnce(&DownloadCapture::OnMoveDone, this, bytes, src, dest));
}

// static
bool DownloadCapture::MoveFileBlocking(base::FilePath src,
                                       base::FilePath dest) {
  if (!base::CreateDirectory(dest.DirName())) {
    return false;
  }
  return base::Move(src, dest);
}

void DownloadCapture::OnMoveDone(double bytes,
                                 base::FilePath src,
                                 base::FilePath dest,
                                 bool moved) {
  if (finalized_) {
    return;
  }
  const base::FilePath final_path = moved ? dest : src;
  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "download");
  result.Set("status", "complete");
  result.Set("path", final_path.AsUTF8Unsafe());
  result.Set("filename", final_path.BaseName().AsUTF8Unsafe());
  result.Set("bytes", bytes);
  if (!moved) {
    result.Set("note",
               "the file finished downloading but could not be relocated into "
               "the workspace downloads folder; the `path` above is its actual "
               "location.");
  }
  std::move(done_).Resolve(std::move(result));
  Finalize();
}

void DownloadCapture::OnArmWindowElapsed() {
  if (finalized_ || item_) {
    return;
  }
  RejectAndFinalize(
      "no_download",
      "clicking that element did not start a download. It may not be a "
      "download trigger — verify it's a download link/button from the latest "
      "observation, or pass a direct `url` to BrowserDownload instead.");
}

void DownloadCapture::OnBudgetElapsed() {
  if (finalized_ || completing_) {
    return;
  }
  ResolveInProgress();
}

void DownloadCapture::ResolveInProgress() {
  if (finalized_) {
    return;
  }
  base::DictValue result;
  result.Set("ok", true);
  result.Set("action", "download");
  result.Set("status", "downloading");
  if (item_) {
    result.Set("path", item_->GetTargetFilePath().AsUTF8Unsafe());
    const int64_t received = item_->GetReceivedBytes();
    const int64_t total = item_->GetTotalBytes();
    result.Set("received_bytes", static_cast<double>(received));
    if (total > 0) {
      result.Set("total_bytes", static_cast<double>(total));
      result.Set("percent", static_cast<int>((received * 100) / total));
    }
  }
  result.Set("note",
             "the file is still downloading in the background. Do NOT re-issue "
             "BrowserDownload — re-check the `path` above with the file tools "
             "in a little while; it will appear once the transfer finishes.");
  std::move(done_).Resolve(std::move(result));
  Finalize();
}

void DownloadCapture::RejectAndFinalize(std::string code,
                                        std::string message) {
  if (finalized_) {
    return;
  }
  std::move(done_).Reject(std::move(code), std::move(message));
  Finalize();
}

void DownloadCapture::StopObserving() {
  arm_timer_.Stop();
  budget_timer_.Stop();
  if (observing_manager_ && manager_) {
    manager_->RemoveObserver(this);
  }
  observing_manager_ = false;
  if (observing_item_ && item_) {
    item_->RemoveObserver(this);
  }
  observing_item_ = false;
}

void DownloadCapture::Finalize() {
  if (finalized_) {
    return;
  }
  finalized_ = true;
  StopObserving();
  // Release the self-reference on the next loop turn so `this` isn't
  // deleted while still unwinding one of our own observer/timer callbacks.
  if (self_) {
    base::SequencedTaskRunner::GetCurrentDefault()->PostTask(
        FROM_HERE,
        base::BindOnce([](scoped_refptr<DownloadCapture>) {}, std::move(self_)));
  }
}

}  // namespace nexus
