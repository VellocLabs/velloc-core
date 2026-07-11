#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_DOWNLOAD_CAPTURE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_DOWNLOAD_CAPTURE_H_

#include <optional>
#include <string>

#include "base/files/file_path.h"
#include "base/memory/raw_ptr.h"
#include "base/memory/ref_counted.h"
#include "base/time/time.h"
#include "base/timer/timer.h"
#include "components/download/public/common/download_item.h"
#include "content/public/browser/download_manager.h"
#include "custom_browser/browser/nexus/tools/nexus_tool_callback.h"

namespace nexus {

// Captures a single browser download — either click-triggered (the model
// can't see the URL) or URL-initiated — waits for it to finish up to a
// wall-clock `budget`, relocates the completed file into the session's
// workspace `downloads/` dir, and resolves the tool `done` callback.
//
// Why native (not CDP): every download, however it's triggered (link
// click, JS `window.location`, `blob:`, form POST, `Content-Disposition:
// attachment`), flows through `content::DownloadManager`. Observing it
// in-process is the same code path a human click takes — undetectable,
// streams to disk (no in-memory buffering of large files), and gives us
// exact creation/progress/completion events. This is strictly better
// than browser-use's CDP `Browser.setDownloadBehavior` +
// `Page.downloadWillBegin` approach for our architecture (which also
// falls back to a JS `arrayBuffer()` path that loads whole files into
// memory — we never do that).
//
// Lifetime: self-managed. `ArmForClickCapture` / `BindStartedItem` take a
// self-reference that's released (via a posted task) only after the
// callback resolves, so the object outlives all its observer/timer
// callbacks and deletes itself afterwards. No owner plumbing needed.
//
// Big files: if the download is still running when `budget` elapses, the
// callback resolves with `status: "downloading"` + byte progress instead
// of blocking the agent indefinitely; the browser keeps streaming in the
// background and the model re-checks the path later.
class DownloadCapture : public content::DownloadManager::Observer,
                        public download::DownloadItem::Observer,
                        public base::RefCounted<DownloadCapture> {
 public:
  static scoped_refptr<DownloadCapture> Create(
      content::DownloadManager* manager,
      base::FilePath dest_dir,
      std::optional<std::string> filename,
      base::TimeDelta budget,
      ToolCallback done);

  DownloadCapture(const DownloadCapture&) = delete;
  DownloadCapture& operator=(const DownloadCapture&) = delete;

  // Click-capture mode. Starts observing the manager for the first NEW
  // download and arms a `arm_window` "did a download even start?" timer.
  // MUST be called BEFORE the triggering click is dispatched.
  void ArmForClickCapture(base::TimeDelta arm_window);

  // URL mode. Binds the item returned by `DownloadManager::DownloadUrl`'s
  // OnStarted callback. `started_ok` is false when the start failed.
  void BindStartedItem(download::DownloadItem* item, bool started_ok);

  // content::DownloadManager::Observer:
  void OnDownloadCreated(content::DownloadManager* manager,
                         download::DownloadItem* item) override;
  void ManagerGoingDown(content::DownloadManager* manager) override;

  // download::DownloadItem::Observer:
  void OnDownloadUpdated(download::DownloadItem* item) override;
  void OnDownloadDestroyed(download::DownloadItem* item) override;

 private:
  friend class base::RefCounted<DownloadCapture>;

  DownloadCapture(content::DownloadManager* manager,
                  base::FilePath dest_dir,
                  std::optional<std::string> filename,
                  base::TimeDelta budget,
                  ToolCallback done);
  ~DownloadCapture() override;

  // Begin observing a concrete item + start the completion budget timer.
  void StartObservingItem(download::DownloadItem* item);
  void OnArmWindowElapsed();
  void OnBudgetElapsed();
  void OnItemComplete();
  // Runs off-sequence on a MayBlock pool thread; returns whether the move
  // succeeded. Static so it never touches `this` off the UI thread.
  static bool MoveFileBlocking(base::FilePath src, base::FilePath dest);
  void OnMoveDone(double bytes,
                  base::FilePath src,
                  base::FilePath dest,
                  bool moved);
  void ResolveInProgress();
  void RejectAndFinalize(std::string code, std::string message);
  void StopObserving();
  void Finalize();

  raw_ptr<content::DownloadManager> manager_;
  base::FilePath dest_dir_;
  std::optional<std::string> filename_;
  base::TimeDelta budget_;
  ToolCallback done_;

  raw_ptr<download::DownloadItem> item_ = nullptr;
  bool observing_manager_ = false;
  bool observing_item_ = false;
  bool completing_ = false;
  bool finalized_ = false;

  base::OneShotTimer arm_timer_;
  base::OneShotTimer budget_timer_;

  // Keep-alive while active; released via a posted task in Finalize().
  scoped_refptr<DownloadCapture> self_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_TOOLS_BROWSER_TOOL_DOWNLOAD_CAPTURE_H_
