#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_CONNECTION_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_CONNECTION_H_

#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/sequence_checker.h"
#include "base/values.h"
#include "sql/database.h"

namespace nexus {

// Uniform result of every SQLite operation. Built entirely on the DB's
// sequence and forwarded verbatim by the adapter's reply — the adapter never
// re-touches the database. Move-only (holds a base::Value).
struct SqlResponse {
  SqlResponse();
  SqlResponse(SqlResponse&&) noexcept;
  SqlResponse& operator=(SqlResponse&&) noexcept;
  SqlResponse(const SqlResponse&) = delete;
  SqlResponse& operator=(const SqlResponse&) = delete;
  ~SqlResponse();

  static SqlResponse MakeOk(base::Value value);
  static SqlResponse MakeError(std::string code, std::string message);

  bool ok = false;
  base::Value value;      // success payload
  std::string code;       // error tag, e.g. "SQLITE_ERROR" (empty on success)
  std::string message;    // human-readable detail
};

enum class SqliteOpenMode {
  kReadOnly,          // 'ro'  — fails if the file is missing
  kReadWrite,         // 'rw'  — fails if the file is missing
  kReadWriteCreate,   // 'rwc' — creates the file if missing
};

enum class SqliteBigIntMode {
  kAuto,    // number when |v| <= 2^53-1, else { $int: "<decimal>" }
  kNumber,  // always a JSON number (documented precision loss past 2^53)
  kString,  // always a decimal string
};

struct SqliteOpenOptions {
  SqliteOpenMode mode = SqliteOpenMode::kReadOnly;
  // Copy the main db (+ -wal / -shm sidecars) to a private temp dir and open
  // the copy read-only. Decouples from a foreign process (e.g. Codex) that may
  // hold the file open, possibly in WAL mode. Implies read-only.
  bool copy_before_open = false;
  int busy_timeout_ms = 2000;
};

struct SqliteQueryOptions {
  int max_rows = 10000;
  SqliteBigIntMode big_int_mode = SqliteBigIntMode::kAuto;
};

// One statement in a batch. Move-only (holds a base::ListValue).
struct SqliteBatchStatement {
  SqliteBatchStatement();
  SqliteBatchStatement(SqliteBatchStatement&&) noexcept;
  SqliteBatchStatement& operator=(SqliteBatchStatement&&) noexcept;
  SqliteBatchStatement(const SqliteBatchStatement&) = delete;
  SqliteBatchStatement& operator=(const SqliteBatchStatement&) = delete;
  ~SqliteBatchStatement();

  std::string sql;
  base::ListValue params;
};

// A single SQLite connection. SEQUENCE-AFFINE: every method (including the
// constructor and destructor) must run on one base::SequencedTaskRunner — the
// adapter owns it via base::SequenceBound and one runner per file. `sql::Database`
// blocks and its handle is not safe across threads, so this class is never
// touched from the UI thread.
//
// The constructor opens the database; failures are recorded and surfaced by
// OpenStatus() and by each operation (which returns the open error rather than
// running). A non-DCHECKing error callback is installed so a corrupt / hostile
// foreign database cannot crash the debug build.
class NexusSqliteConnection {
 public:
  NexusSqliteConnection(base::FilePath path, SqliteOpenOptions options);
  ~NexusSqliteConnection();

  NexusSqliteConnection(const NexusSqliteConnection&) = delete;
  NexusSqliteConnection& operator=(const NexusSqliteConnection&) = delete;

  // { } on success, or the open error. Used by the `open` action to decide
  // handle-vs-error without running a statement.
  SqlResponse OpenStatus();

  SqlResponse Query(std::string sql,
                    base::ListValue params,
                    SqliteQueryOptions options);
  SqlResponse Execute(std::string sql, base::ListValue params);
  SqlResponse Batch(std::vector<SqliteBatchStatement> statements, bool atomic);

 private:
  // Holds the copy when options.copy_before_open is set; deleted on destruction.
  base::ScopedTempDir temp_dir_;
  sql::Database db_;
  bool open_ok_ = false;
  std::string open_code_;
  std::string open_message_;
  SEQUENCE_CHECKER(sequence_checker_);
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_API_SQLITE_NEXUS_SQLITE_CONNECTION_H_
