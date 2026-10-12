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

// Captures one browser download (click-triggered or URL-initiated) through
// content::DownloadManager, waits up to `budget`, moves the file into the
// session's `downloads/` dir and resolves `done`. Self-owned: deletes itself
// after resolving. Past `budget` it resolves `status: "downloading"`.
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
  void RejectAndFinalize(std::string code, base::DictValue params);
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
