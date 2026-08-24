#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_connection.h"

#include <stdint.h>

#include <limits>
#include <optional>
#include <tuple>
#include <utility>

#include "base/base64.h"
#include "base/containers/span.h"
#include "base/files/file_util.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "sql/statement.h"
#include "sql/transaction.h"

namespace nexus {

namespace {

// Largest integer representable exactly as an IEEE-754 double (2^53 - 1).
constexpr int64_t kMaxSafeInteger = (int64_t{1} << 53) - 1;

// A no-op error callback. Installing ANY callback suppresses sql::Database's
// default DCHECK-on-error behavior — essential when opening a foreign / possibly
// corrupt database, which must fail gracefully rather than crash the debug build.
void SwallowSqlError(int /*sqlite_error*/, sql::Statement* /*stmt*/) {}

sql::DatabaseOptions MakeDbOptions(const SqliteOpenOptions& options) {
  // copy_before_open opens a throwaway copy — always read-only.
  const bool read_only =
      options.copy_before_open || options.mode == SqliteOpenMode::kReadOnly;
  sql::DatabaseOptions db_options;
  db_options.set_read_only(read_only);
  if (read_only) {
    // Don't contend for the advisory lock a foreign writer may hold.
    db_options.set_exclusive_locking(false);
  }
  return db_options;
}

base::FilePath WithSuffix(const base::FilePath& path,
                          base::FilePath::StringViewType suffix) {
  return base::FilePath(path.value() + base::FilePath::StringType(suffix));
}

// Encode one int64 for the wire per the requested bigint policy.
base::Value Int64ToValue(int64_t value, SqliteBigIntMode mode) {
  if (mode == SqliteBigIntMode::kString) {
    return base::Value(base::NumberToString(value));
  }
  if (mode == SqliteBigIntMode::kNumber) {
    return base::Value(static_cast<double>(value));
  }
  // kAuto.
  if (value >= std::numeric_limits<int>::min() &&
      value <= std::numeric_limits<int>::max()) {
    return base::Value(static_cast<int>(value));
  }
  if (value >= -kMaxSafeInteger && value <= kMaxSafeInteger) {
    return base::Value(static_cast<double>(value));
  }
  base::DictValue tagged;
  tagged.Set("$int", base::NumberToString(value));
  return base::Value(std::move(tagged));
}

// Read one column of the current row into a wire value. Call GetColumnType
// first so a NULL in a numeric column round-trips as null, not 0.
base::Value ColumnToValue(sql::Statement& statement,
                          int column,
                          SqliteBigIntMode big_int_mode) {
  switch (statement.GetColumnType(column)) {
    case sql::ColumnType::kNull:
      return base::Value();
    case sql::ColumnType::kInteger:
      return Int64ToValue(statement.ColumnInt64(column), big_int_mode);
    case sql::ColumnType::kFloat:
      return base::Value(statement.ColumnDouble(column));
    case sql::ColumnType::kText:
      return base::Value(statement.ColumnString(column));
    case sql::ColumnType::kBlob: {
      base::DictValue blob;
      blob.Set("$blob", base::Base64Encode(statement.ColumnBlob(column)));
      return base::Value(std::move(blob));
    }
  }
  return base::Value();
}

// Bind positional params (0-based). Returns false + sets `error` on an
// unsupported / malformed param.
bool BindParams(sql::Statement& statement,
                const base::ListValue& params,
                std::string* error) {
  for (size_t i = 0; i < params.size(); ++i) {
    const base::Value& param = params[i];
    const int index = static_cast<int>(i);
    switch (param.type()) {
      case base::Value::Type::NONE:
        statement.BindNull(index);
        break;
      case base::Value::Type::BOOLEAN:
        statement.BindBool(index, param.GetBool());
        break;
      case base::Value::Type::INTEGER:
        statement.BindInt64(index, param.GetInt());
        break;
      case base::Value::Type::DOUBLE:
        statement.BindDouble(index, param.GetDouble());
        break;
      case base::Value::Type::STRING:
        statement.BindString(index, param.GetString());
        break;
      case base::Value::Type::DICT: {
        const base::DictValue& dict = param.GetDict();
        if (const std::string* big_int = dict.FindString("$int")) {
          int64_t parsed = 0;
          if (!base::StringToInt64(*big_int, &parsed)) {
            *error = "invalid $int param at index " + base::NumberToString(index);
            return false;
          }
          statement.BindInt64(index, parsed);
        } else if (const std::string* blob = dict.FindString("$blob")) {
          std::optional<std::vector<uint8_t>> decoded = base::Base64Decode(*blob);
          if (!decoded) {
            *error =
                "invalid $blob param at index " + base::NumberToString(index);
            return false;
          }
          statement.BindBlob(index, std::move(*decoded));
        } else {
          *error = "unsupported object param at index " +
                   base::NumberToString(index);
          return false;
        }
        break;
      }
      default:
        *error =
            "unsupported param type at index " + base::NumberToString(index);
        return false;
    }
  }
  return true;
}

base::Value BuildWriteResult(sql::Database& db) {
  base::DictValue result;
  result.Set("rowsAffected", Int64ToValue(db.GetLastChangeCount(),
                                          SqliteBigIntMode::kAuto));
  // Intentionally NOT GetLastInsertRowId(): Chromium's accessor DCHECKs unless
  // the last statement was an INSERT into a ROWID table, so calling it after a
  // CREATE/UPDATE/DELETE (or a WITHOUT ROWID insert) crashes the debug build.
  // Callers that need the id run `SELECT last_insert_rowid()`.
  return base::Value(std::move(result));
}

}  // namespace

SqlResponse::SqlResponse() = default;
SqlResponse::SqlResponse(SqlResponse&&) noexcept = default;
SqlResponse& SqlResponse::operator=(SqlResponse&&) noexcept = default;
SqlResponse::~SqlResponse() = default;

// static
SqlResponse SqlResponse::MakeOk(base::Value value) {
  SqlResponse response;
  response.ok = true;
  response.value = std::move(value);
  return response;
}

// static
SqlResponse SqlResponse::MakeError(std::string code, std::string message) {
  SqlResponse response;
  response.ok = false;
  response.code = std::move(code);
  response.message = std::move(message);
  return response;
}

SqliteBatchStatement::SqliteBatchStatement() = default;
SqliteBatchStatement::SqliteBatchStatement(SqliteBatchStatement&&) noexcept =
    default;
SqliteBatchStatement& SqliteBatchStatement::operator=(
    SqliteBatchStatement&&) noexcept = default;
SqliteBatchStatement::~SqliteBatchStatement() = default;

NexusSqliteConnection::NexusSqliteConnection(base::FilePath path,
                                             SqliteOpenOptions options)
    : db_(MakeDbOptions(options), sql::Database::Tag("NexusSqlite")) {
  DETACH_FROM_SEQUENCE(sequence_checker_);
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);

  base::FilePath open_path = std::move(path);
  if (options.copy_before_open) {
    if (!temp_dir_.CreateUniqueTempDir()) {
      open_code_ = "SQLITE_CANTOPEN";
      open_message_ = "failed to create temp directory for copy";
      return;
    }
    base::FilePath dest = temp_dir_.GetPath().Append(open_path.BaseName());
    if (!base::CopyFile(open_path, dest)) {
      open_code_ = "SQLITE_CANTOPEN";
      open_message_ = "failed to copy database before open";
      return;
    }
    // Best-effort: copy WAL / shared-memory sidecars if present.
    for (const auto* suffix :
         {FILE_PATH_LITERAL("-wal"), FILE_PATH_LITERAL("-shm")}) {
      base::FilePath sidecar = WithSuffix(open_path, suffix);
      if (base::PathExists(sidecar)) {
        base::CopyFile(sidecar, WithSuffix(dest, suffix));
      }
    }
    open_path = std::move(dest);
  }

  db_.set_error_callback(base::BindRepeating(&SwallowSqlError));
  if (!db_.Open(open_path)) {
    open_code_ = "SQLITE_CANTOPEN";
    open_message_ = "failed to open database";
    return;
  }
  open_ok_ = true;
  if (options.busy_timeout_ms > 0) {
    // Best-effort; a read-only connection accepts this connection PRAGMA.
    const std::string pragma =
        "PRAGMA busy_timeout=" + base::NumberToString(options.busy_timeout_ms);
    std::ignore = db_.Execute(pragma);
  }
}

NexusSqliteConnection::~NexusSqliteConnection() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
}

SqlResponse NexusSqliteConnection::OpenStatus() {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ok_) {
    return SqlResponse::MakeError(open_code_, open_message_);
  }
  return SqlResponse::MakeOk(base::Value(base::DictValue()));
}

SqlResponse NexusSqliteConnection::Query(std::string sql,
                                         base::ListValue params,
                                         SqliteQueryOptions options) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ok_) {
    return SqlResponse::MakeError(open_code_, open_message_);
  }
  sql::Statement statement(db_.GetUniqueStatement(sql));
  if (!statement.is_valid()) {
    return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
  }
  std::string bind_error;
  if (!BindParams(statement, params, &bind_error)) {
    return SqlResponse::MakeError("INVALID_ARGS", std::move(bind_error));
  }

  base::ListValue rows;
  const int column_count = statement.ColumnCount();
  int row_count = 0;
  bool truncated = false;
  while (statement.Step()) {
    if (row_count >= options.max_rows) {
      truncated = true;
      break;
    }
    base::ListValue row;
    for (int column = 0; column < column_count; ++column) {
      row.Append(ColumnToValue(statement, column, options.big_int_mode));
    }
    rows.Append(std::move(row));
    ++row_count;
  }
  // Step() returns false at end-of-rows OR on error; disambiguate.
  if (!truncated && !statement.Succeeded()) {
    return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
  }

  base::DictValue result;
  result.Set("rows", base::Value(std::move(rows)));
  result.Set("rowCount", row_count);
  result.Set("truncated", truncated);
  return SqlResponse::MakeOk(base::Value(std::move(result)));
}

SqlResponse NexusSqliteConnection::Execute(std::string sql,
                                           base::ListValue params) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ok_) {
    return SqlResponse::MakeError(open_code_, open_message_);
  }
  sql::Statement statement(db_.GetUniqueStatement(sql));
  if (!statement.is_valid()) {
    return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
  }
  std::string bind_error;
  if (!BindParams(statement, params, &bind_error)) {
    return SqlResponse::MakeError("INVALID_ARGS", std::move(bind_error));
  }
  if (!statement.Run()) {
    return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
  }
  return SqlResponse::MakeOk(BuildWriteResult(db_));
}

SqlResponse NexusSqliteConnection::Batch(
    std::vector<SqliteBatchStatement> statements,
    bool atomic) {
  DCHECK_CALLED_ON_VALID_SEQUENCE(sequence_checker_);
  if (!open_ok_) {
    return SqlResponse::MakeError(open_code_, open_message_);
  }

  std::optional<sql::Transaction> transaction;
  if (atomic) {
    transaction.emplace(&db_);
    if (!transaction->Begin()) {
      return SqlResponse::MakeError("SQLITE_ERROR",
                                    "failed to begin transaction");
    }
  }

  base::ListValue results;
  for (auto& item : statements) {
    sql::Statement statement(db_.GetUniqueStatement(item.sql));
    if (!statement.is_valid()) {
      // transaction (if any) auto-rolls back on scope exit.
      return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
    }
    std::string bind_error;
    if (!BindParams(statement, item.params, &bind_error)) {
      return SqlResponse::MakeError("INVALID_ARGS", std::move(bind_error));
    }
    if (!statement.Run()) {
      return SqlResponse::MakeError("SQLITE_ERROR", db_.GetErrorMessage());
    }
    results.Append(BuildWriteResult(db_));
  }

  if (atomic && !transaction->Commit()) {
    return SqlResponse::MakeError("SQLITE_ERROR", "failed to commit transaction");
  }

  base::DictValue result;
  result.Set("results", base::Value(std::move(results)));
  return SqlResponse::MakeOk(base::Value(std::move(result)));
}

}  // namespace nexus
