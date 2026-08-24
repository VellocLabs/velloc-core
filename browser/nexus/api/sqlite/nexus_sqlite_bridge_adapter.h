#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_BRIDGE_ADAPTER_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_BRIDGE_ADAPTER_H_

#include <map>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/memory/weak_ptr.h"
#include "base/task/sequenced_task_runner.h"
#include "base/threading/sequence_bound.h"
#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_connection.h"
#include "custom_browser/browser/nexus/api/sqlite/sqlite_api.gen.h"
#include "custom_browser/browser/nexus/bridge_response_callback.h"

namespace blink {
class MessagePortDescriptor;
}

namespace nexus {

class NexusBridgeResponseDelegate;

// Concrete adapter implementing the generated `SqliteApiBase` virtuals — the
// `sqlite` bridge capability. Reads/writes host SQLite files from the guest.
//
// Threading: each Handle*OnUI runs on the UI thread. SQLite work runs on a
// PER-FILE base::SequencedTaskRunner (a `sqlite3` handle is not safe across
// threads, and `sql::Database` blocks). A lease-counted registry
// (`files_`, UI-thread-only, so no lock) hands the same runner to every
// operation on the same normalized path — serializing them — while unrelated
// files run in parallel. Connections live on those sequences via
// base::SequenceBound; the registry maps and lease counts are only ever touched
// on UI.
//
// Two connection kinds:
//   - one-shot (`path`): a transient connection in `one_shots_`, opened →
//     used → removed in the reply.
//   - handle (`open`/`close`): a persistent connection in `handles_`, held
//     until close.
// See internal/docs/sqlite/nexus-sqlite-capability.md.
class NexusSqliteBridgeAdapter : public ::nexus::generated::SqliteApiBase {
 public:
  explicit NexusSqliteBridgeAdapter(NexusBridgeResponseDelegate* delegate);
  ~NexusSqliteBridgeAdapter() override;

  NexusSqliteBridgeAdapter(const NexusSqliteBridgeAdapter&) = delete;
  NexusSqliteBridgeAdapter& operator=(const NexusSqliteBridgeAdapter&) = delete;

 private:
  // A live connection plus the file key it leases.
  struct Connection {
    Connection(base::SequenceBound<NexusSqliteConnection> conn,
               base::FilePath key);
    Connection(Connection&&) noexcept;
    Connection& operator=(Connection&&) noexcept;
    Connection(const Connection&) = delete;
    Connection& operator=(const Connection&) = delete;
    ~Connection();

    base::SequenceBound<NexusSqliteConnection> conn;
    base::FilePath key;
  };
  // One per open file: the shared sequence + how many leases hold it.
  struct FileEntry {
    FileEntry();
    ~FileEntry();

    scoped_refptr<base::SequencedTaskRunner> runner;
    int leases = 0;
  };

  // Generated SqliteApiBase overrides — all dispatched on UI.
  void HandleQueryOnUI(::nexus::generated::QueryInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;
  void HandleExecuteOnUI(::nexus::generated::ExecuteInput input,
                         std::vector<blink::MessagePortDescriptor> ports,
                         ::nexus::BridgeResponseCallback done) override;
  void HandleBatchOnUI(::nexus::generated::BatchInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;
  void HandleOpenOnUI(::nexus::generated::OpenInput input,
                      std::vector<blink::MessagePortDescriptor> ports,
                      ::nexus::BridgeResponseCallback done) override;
  void HandleCloseOnUI(::nexus::generated::CloseInput input,
                       std::vector<blink::MessagePortDescriptor> ports,
                       ::nexus::BridgeResponseCallback done) override;

  // Replies (run back on UI, weak-guarded — they touch the registries).
  void ReplyOneShot(::nexus::BridgeResponseCallback done,
                    std::string op_id,
                    SqlResponse response);
  void ReplyOpen(::nexus::BridgeResponseCallback done,
                 std::string handle_id,
                 SqlResponse status);

  // Registry (UI thread only).
  scoped_refptr<base::SequencedTaskRunner> AcquireRunner(
      const base::FilePath& key);
  void ReleaseRunner(const base::FilePath& key);
  std::string MintId();

  // files_ MUST be declared before the connection maps: members destroy in
  // reverse order, so the connections (and any lease-releasing that close does)
  // tear down while files_ is still alive; then files_ is destroyed wholesale.
  std::map<base::FilePath, FileEntry> files_;
  std::map<std::string, Connection> handles_;    // persistent (open/close)
  std::map<std::string, Connection> one_shots_;  // transient (auto-removed)
  int next_id_ = 0;

  base::WeakPtrFactory<NexusSqliteBridgeAdapter> weak_factory_{this};
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_BRIDGE_ADAPTER_H_
