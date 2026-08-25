#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_bridge_adapter.h"

#include <utility>

#include "base/functional/bind.h"
#include "base/strings/strcat.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/sequenced_task_runner.h"
#include "base/task/task_traits.h"
#include "base/task/thread_pool.h"
#include "base/values.h"
#include "build/build_config.h"

namespace nexus {

using ::nexus::generated::BatchInput;
using ::nexus::generated::CloseInput;
using ::nexus::generated::ExecuteInput;
using ::nexus::generated::OpenInput;
using ::nexus::generated::QueryInput;

namespace {

// A frame that opens dozens of connections and never closes them is a bug;
// this bounds the leak rather than letting it grow unbounded.
constexpr size_t kMaxOpenHandles = 64;

// A guest firing one-shots faster than they reply (or on unbounded distinct
// paths) would otherwise spin up an unbounded number of transient connections
// and per-file sequences. Bound the in-flight set the same way `open` is
// bounded by kMaxOpenHandles.
constexpr size_t kMaxInFlightOneShots = 64;

// Absolute ceiling on rows returned in one query, regardless of maxRows — a
// serialize-to-JSON bridge can't stream an unbounded table.
constexpr int kHardMaxRows = 100000;

// Forward a connection result over the bridge. A free function (touches only
// `done` + `response`, never the adapter) so it is safe to run even if the
// adapter was torn down while the work was in flight; `done` weak-guards its
// owning handler and drops cleanly in that case.
void SendSqlResponse(BridgeResponseCallback done, SqlResponse response) {
  if (response.ok) {
    std::move(done).Success(std::move(response.value));
  } else {
    std::move(done).Error(std::move(response.code), std::move(response.message));
  }
}

SqliteOpenMode ParseMode(const std::string* mode, SqliteOpenMode fallback) {
  if (mode) {
    if (*mode == "ro") {
      return SqliteOpenMode::kReadOnly;
    }
    if (*mode == "rw") {
      return SqliteOpenMode::kReadWrite;
    }
    if (*mode == "rwc") {
      return SqliteOpenMode::kReadWriteCreate;
    }
  }
  return fallback;
}

SqliteBigIntMode ParseBigIntMode(const std::string* mode) {
  if (mode) {
    if (*mode == "number") {
      return SqliteBigIntMode::kNumber;
    }
    if (*mode == "string") {
      return SqliteBigIntMode::kString;
    }
  }
  return SqliteBigIntMode::kAuto;
}

SqliteOpenOptions ParseOpenOptions(const base::DictValue& payload,
                                   SqliteOpenMode default_mode) {
  SqliteOpenOptions options;
  options.mode = ParseMode(payload.FindString("mode"), default_mode);
  options.copy_before_open =
      payload.FindBool("copyBeforeOpen").value_or(false);
  options.busy_timeout_ms = payload.FindInt("busyTimeoutMs").value_or(2000);
  return options;
}

SqliteQueryOptions ParseQueryOptions(const base::DictValue& payload) {
  SqliteQueryOptions options;
  int max_rows = payload.FindInt("maxRows").value_or(options.max_rows);
  max_rows = std::max(1, std::min(max_rows, kHardMaxRows));
  options.max_rows = max_rows;
  options.big_int_mode = ParseBigIntMode(payload.FindString("bigIntMode"));
  return options;
}

base::ListValue CloneParams(const base::DictValue& payload) {
  const base::ListValue* params = payload.FindList("params");
  return params ? params->Clone() : base::ListValue();
}

bool ParseStatements(const base::DictValue& payload,
                     std::vector<SqliteBatchStatement>* out,
                     std::string* error) {
  const base::ListValue* statements = payload.FindList("statements");
  if (!statements) {
    *error = "missing 'statements'";
    return false;
  }
  for (const base::Value& entry : *statements) {
    if (!entry.is_dict()) {
      *error = "each statement must be an object";
      return false;
    }
    const base::DictValue& dict = entry.GetDict();
    const std::string* sql = dict.FindString("sql");
    if (!sql) {
      *error = "each statement needs 'sql'";
      return false;
    }
    SqliteBatchStatement item;
    item.sql = *sql;
    if (const base::ListValue* params = dict.FindList("params")) {
      item.params = params->Clone();
    }
    out->push_back(std::move(item));
  }
  return true;
}

// Canonicalize `path` into the key that selects a file's serialization runner.
// Two spellings of the same file MUST map to the same key, or writes to it stop
// being serialized. We fold ASCII case on Windows (its file system is
// case-insensitive, so 'A.DB' and 'a.db' are one file) — a pure-string
// operation. We intentionally do NOT resolve '..'/symlinks or make the path
// absolute here: that requires filesystem I/O (base::MakeAbsoluteFilePath wraps
// a MAY_BLOCK ScopedBlockingCall) and this runs on the UI thread; paths already
// arrive absolute from the guest, so the realistic aliasing is case.
base::FilePath NormalizeKey(const std::string& path) {
  base::FilePath normalized =
      base::FilePath::FromUTF8Unsafe(path).NormalizePathSeparators();
#if BUILDFLAG(IS_WIN)
  return base::FilePath::FromUTF8Unsafe(
      base::ToLowerASCII(normalized.AsUTF8Unsafe()));
#else
  return normalized;
#endif
}

}  // namespace

NexusSqliteBridgeAdapter::Connection::Connection(
    base::SequenceBound<NexusSqliteConnection> conn,
    base::FilePath key)
    : conn(std::move(conn)), key(std::move(key)) {}
NexusSqliteBridgeAdapter::Connection::Connection(Connection&&) noexcept =
    default;
NexusSqliteBridgeAdapter::Connection&
NexusSqliteBridgeAdapter::Connection::operator=(Connection&&) noexcept = default;
NexusSqliteBridgeAdapter::Connection::~Connection() = default;

NexusSqliteBridgeAdapter::FileEntry::FileEntry() = default;
NexusSqliteBridgeAdapter::FileEntry::~FileEntry() = default;

NexusSqliteBridgeAdapter::NexusSqliteBridgeAdapter(
    NexusBridgeResponseDelegate* delegate)
    : ::nexus::generated::SqliteApiBase(delegate) {}

NexusSqliteBridgeAdapter::~NexusSqliteBridgeAdapter() = default;

template <typename AsyncOp>
void NexusSqliteBridgeAdapter::StartOneShot(const base::FilePath& key,
                                            SqliteOpenOptions open_options,
                                            BridgeResponseCallback done,
                                            AsyncOp async_op) {
  if (one_shots_.size() >= kMaxInFlightOneShots) {
    std::move(done).Error("SQLITE_TOO_MANY_OPS",
                          "too many in-flight sqlite operations");
    return;
  }
  scoped_refptr<base::SequencedTaskRunner> runner = AcquireRunner(key);
  const std::string op_id = MintId();
  Connection connection{
      base::SequenceBound<NexusSqliteConnection>(runner, key, open_options),
      key};
  Connection& stored =
      one_shots_.emplace(op_id, std::move(connection)).first->second;
  std::move(async_op)(stored.conn)
      .Then(base::BindOnce(&NexusSqliteBridgeAdapter::ReplyOneShot,
                           weak_factory_.GetWeakPtr(), std::move(done), op_id));
}

void NexusSqliteBridgeAdapter::HandleQueryOnUI(
    QueryInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    BridgeResponseCallback done) {
  const base::DictValue& payload = input.payload;
  const std::string* sql = payload.FindString("sql");
  if (!sql) {
    std::move(done).InvalidArgs("missing 'sql'");
    return;
  }
  base::ListValue params = CloneParams(payload);
  SqliteQueryOptions query_options = ParseQueryOptions(payload);

  if (const std::string* handle = payload.FindString("handle")) {
    auto it = handles_.find(*handle);
    if (it == handles_.end()) {
      std::move(done).InvalidArgs("unknown handle");
      return;
    }
    it->second.conn.AsyncCall(&NexusSqliteConnection::Query)
        .WithArgs(*sql, std::move(params), query_options)
        .Then(base::BindOnce(&SendSqlResponse, std::move(done)));
    return;
  }

  const std::string* path = payload.FindString("path");
  if (!path) {
    std::move(done).InvalidArgs("need 'path' or 'handle'");
    return;
  }
  const SqliteOpenOptions open_options =
      ParseOpenOptions(payload, SqliteOpenMode::kReadOnly);
  const base::FilePath key = NormalizeKey(*path);
  StartOneShot(
      key, open_options, std::move(done),
      [sql = *sql, params = std::move(params), query_options](
          base::SequenceBound<NexusSqliteConnection>& conn) mutable {
        return conn.AsyncCall(&NexusSqliteConnection::Query)
            .WithArgs(std::move(sql), std::move(params), query_options);
      });
}

void NexusSqliteBridgeAdapter::HandleExecuteOnUI(
    ExecuteInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    BridgeResponseCallback done) {
  const base::DictValue& payload = input.payload;
  const std::string* sql = payload.FindString("sql");
  if (!sql) {
    std::move(done).InvalidArgs("missing 'sql'");
    return;
  }
  base::ListValue params = CloneParams(payload);

  if (const std::string* handle = payload.FindString("handle")) {
    auto it = handles_.find(*handle);
    if (it == handles_.end()) {
      std::move(done).InvalidArgs("unknown handle");
      return;
    }
    it->second.conn.AsyncCall(&NexusSqliteConnection::Execute)
        .WithArgs(*sql, std::move(params))
        .Then(base::BindOnce(&SendSqlResponse, std::move(done)));
    return;
  }

  const std::string* path = payload.FindString("path");
  if (!path) {
    std::move(done).InvalidArgs("need 'path' or 'handle'");
    return;
  }
  const SqliteOpenOptions open_options =
      ParseOpenOptions(payload, SqliteOpenMode::kReadWrite);
  const base::FilePath key = NormalizeKey(*path);
  StartOneShot(
      key, open_options, std::move(done),
      [sql = *sql, params = std::move(params)](
          base::SequenceBound<NexusSqliteConnection>& conn) mutable {
        return conn.AsyncCall(&NexusSqliteConnection::Execute)
            .WithArgs(std::move(sql), std::move(params));
      });
}

void NexusSqliteBridgeAdapter::HandleBatchOnUI(
    BatchInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    BridgeResponseCallback done) {
  const base::DictValue& payload = input.payload;
  std::vector<SqliteBatchStatement> statements;
  std::string parse_error;
  if (!ParseStatements(payload, &statements, &parse_error)) {
    std::move(done).InvalidArgs(parse_error);
    return;
  }
  const bool atomic = payload.FindBool("atomic").value_or(true);

  if (const std::string* handle = payload.FindString("handle")) {
    auto it = handles_.find(*handle);
    if (it == handles_.end()) {
      std::move(done).InvalidArgs("unknown handle");
      return;
    }
    it->second.conn.AsyncCall(&NexusSqliteConnection::Batch)
        .WithArgs(std::move(statements), atomic)
        .Then(base::BindOnce(&SendSqlResponse, std::move(done)));
    return;
  }

  const std::string* path = payload.FindString("path");
  if (!path) {
    std::move(done).InvalidArgs("need 'path' or 'handle'");
    return;
  }
  const SqliteOpenOptions open_options =
      ParseOpenOptions(payload, SqliteOpenMode::kReadWrite);
  const base::FilePath key = NormalizeKey(*path);
  StartOneShot(
      key, open_options, std::move(done),
      [statements = std::move(statements), atomic](
          base::SequenceBound<NexusSqliteConnection>& conn) mutable {
        return conn.AsyncCall(&NexusSqliteConnection::Batch)
            .WithArgs(std::move(statements), atomic);
      });
}

void NexusSqliteBridgeAdapter::HandleOpenOnUI(
    OpenInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    BridgeResponseCallback done) {
  const base::DictValue& payload = input.payload;
  const std::string* path = payload.FindString("path");
  if (!path) {
    std::move(done).InvalidArgs("missing 'path'");
    return;
  }
  if (handles_.size() >= kMaxOpenHandles) {
    std::move(done).Error("SQLITE_TOO_MANY_HANDLES",
                          "too many open sqlite connections");
    return;
  }
  const SqliteOpenOptions open_options =
      ParseOpenOptions(payload, SqliteOpenMode::kReadOnly);
  const base::FilePath key = NormalizeKey(*path);
  scoped_refptr<base::SequencedTaskRunner> runner = AcquireRunner(key);
  const std::string handle_id = MintId();
  Connection connection{
      base::SequenceBound<NexusSqliteConnection>(runner, key, open_options),
      key};
  Connection& stored =
      handles_.emplace(handle_id, std::move(connection)).first->second;
  stored.conn.AsyncCall(&NexusSqliteConnection::OpenStatus)
      .Then(base::BindOnce(&NexusSqliteBridgeAdapter::ReplyOpen,
                           weak_factory_.GetWeakPtr(), std::move(done),
                           handle_id));
}

void NexusSqliteBridgeAdapter::HandleCloseOnUI(
    CloseInput input,
    std::vector<blink::MessagePortDescriptor> /*ports*/,
    BridgeResponseCallback done) {
  const std::string* handle = input.payload.FindString("handle");
  if (!handle) {
    std::move(done).InvalidArgs("missing 'handle'");
    return;
  }
  auto it = handles_.find(*handle);
  if (it == handles_.end()) {
    std::move(done).InvalidArgs("unknown handle");
    return;
  }
  const base::FilePath key = it->second.key;
  handles_.erase(it);  // drops SequenceBound → connection destroyed on sequence
  ReleaseRunner(key);
  std::move(done).SuccessVoid();
}

void NexusSqliteBridgeAdapter::ReplyOneShot(BridgeResponseCallback done,
                                            std::string op_id,
                                            SqlResponse response) {
  SendSqlResponse(std::move(done), std::move(response));
  auto it = one_shots_.find(op_id);
  if (it == one_shots_.end()) {
    return;
  }
  const base::FilePath key = it->second.key;
  one_shots_.erase(it);
  ReleaseRunner(key);
}

void NexusSqliteBridgeAdapter::ReplyOpen(BridgeResponseCallback done,
                                         std::string handle_id,
                                         SqlResponse status) {
  if (status.ok) {
    base::DictValue result;
    result.Set("handle", handle_id);
    std::move(done).Success(base::Value(std::move(result)));
    return;
  }
  // Open failed — tear down the handle and release its lease.
  auto it = handles_.find(handle_id);
  if (it != handles_.end()) {
    const base::FilePath key = it->second.key;
    handles_.erase(it);
    ReleaseRunner(key);
  }
  std::move(done).Error(std::move(status.code), std::move(status.message));
}

scoped_refptr<base::SequencedTaskRunner> NexusSqliteBridgeAdapter::AcquireRunner(
    const base::FilePath& key) {
  FileEntry& entry = files_[key];
  if (!entry.runner) {
    entry.runner = base::ThreadPool::CreateSequencedTaskRunner(
        {base::MayBlock(), base::TaskPriority::USER_VISIBLE,
         base::TaskShutdownBehavior::SKIP_ON_SHUTDOWN});
  }
  ++entry.leases;
  return entry.runner;
}

void NexusSqliteBridgeAdapter::ReleaseRunner(const base::FilePath& key) {
  auto it = files_.find(key);
  if (it == files_.end()) {
    return;
  }
  if (--it->second.leases <= 0) {
    files_.erase(it);
  }
}

std::string NexusSqliteBridgeAdapter::MintId() {
  return base::StrCat({"sqlite:", base::NumberToString(++next_id_)});
}

}  // namespace nexus
