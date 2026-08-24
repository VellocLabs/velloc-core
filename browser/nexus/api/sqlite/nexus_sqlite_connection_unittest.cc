// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_connection.h"

#include <vector>

#include "base/base64.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/test/task_environment.h"
#include "base/values.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

SqliteOpenOptions ModeOptions(SqliteOpenMode mode) {
  SqliteOpenOptions options;
  options.mode = mode;
  return options;
}

// Create + populate a small table via a read-write-create connection, then
// read it back read-only and check every column type round-trips, including a
// NULL preserved as none (not 0).
TEST(NexusSqliteConnectionTest, QueryReturnsTypedRows) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("typed.sqlite");

  const std::vector<uint8_t> blob_bytes = {0xDE, 0xAD, 0xBE, 0xEF};
  {
    NexusSqliteConnection writer(db_path,
                                 ModeOptions(SqliteOpenMode::kReadWriteCreate));
    ASSERT_TRUE(writer.OpenStatus().ok);
    ASSERT_TRUE(
        writer
            .Execute("CREATE TABLE t (id INTEGER, name TEXT, score REAL, "
                     "data BLOB, opt TEXT)",
                     base::ListValue())
            .ok);
    base::ListValue params;
    params.Append(42);
    params.Append("alice");
    params.Append(3.5);
    base::DictValue blob;
    blob.Set("$blob", base::Base64Encode(blob_bytes));
    params.Append(std::move(blob));
    params.Append(base::Value());  // opt = NULL
    SqlResponse insert =
        writer.Execute("INSERT INTO t VALUES (?, ?, ?, ?, ?)", std::move(params));
    ASSERT_TRUE(insert.ok) << insert.message;
    EXPECT_EQ(insert.value.GetDict().FindInt("rowsAffected").value_or(-1), 1);
  }

  NexusSqliteConnection reader(db_path, ModeOptions(SqliteOpenMode::kReadOnly));
  ASSERT_TRUE(reader.OpenStatus().ok);
  SqlResponse result = reader.Query(
      "SELECT id, name, score, data, opt FROM t", base::ListValue(), {});
  ASSERT_TRUE(result.ok) << result.message;

  const base::DictValue& dict = result.value.GetDict();
  EXPECT_EQ(dict.FindInt("rowCount").value_or(-1), 1);
  EXPECT_EQ(dict.FindBool("truncated").value_or(true), false);
  const base::ListValue* rows = dict.FindList("rows");
  ASSERT_TRUE(rows);
  ASSERT_EQ(rows->size(), 1u);
  const base::ListValue& row = (*rows)[0].GetList();
  ASSERT_EQ(row.size(), 5u);
  EXPECT_EQ(row[0].GetInt(), 42);
  EXPECT_EQ(row[1].GetString(), "alice");
  EXPECT_EQ(row[2].GetDouble(), 3.5);
  ASSERT_TRUE(row[3].is_dict());
  EXPECT_EQ(*row[3].GetDict().FindString("$blob"),
            base::Base64Encode(blob_bytes));
  EXPECT_TRUE(row[4].is_none());  // NULL, not 0
}

// A value past 2^53 must survive as a tagged { $int: "<decimal>" }, and a bound
// $int param must round-trip.
TEST(NexusSqliteConnectionTest, BigIntRoundTrips) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("bigint.sqlite");

  NexusSqliteConnection conn(db_path,
                             ModeOptions(SqliteOpenMode::kReadWriteCreate));
  ASSERT_TRUE(conn.Execute("CREATE TABLE t (n INTEGER)", base::ListValue()).ok);
  base::ListValue params;
  base::DictValue big;
  big.Set("$int", "9223372036854775807");  // INT64_MAX, well past 2^53
  params.Append(std::move(big));
  ASSERT_TRUE(conn.Execute("INSERT INTO t VALUES (?)", std::move(params)).ok);

  SqlResponse result = conn.Query("SELECT n FROM t", base::ListValue(), {});
  ASSERT_TRUE(result.ok) << result.message;
  const base::ListValue& row = (*result.value.GetDict().FindList("rows"))[0].GetList();
  ASSERT_TRUE(row[0].is_dict());
  EXPECT_EQ(*row[0].GetDict().FindString("$int"), "9223372036854775807");
}

// Invalid SQL is a graceful SQLITE_ERROR, not a crash.
TEST(NexusSqliteConnectionTest, InvalidSqlReturnsError) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("err.sqlite");

  NexusSqliteConnection conn(db_path,
                             ModeOptions(SqliteOpenMode::kReadWriteCreate));
  SqlResponse result =
      conn.Query("SELECT * FROM does_not_exist", base::ListValue(), {});
  EXPECT_FALSE(result.ok);
  EXPECT_EQ(result.code, "SQLITE_ERROR");
}

// A non-SQLite file must be rejected gracefully (no DCHECK crash from the
// default error handler).
TEST(NexusSqliteConnectionTest, CorruptFileRejectedGracefully) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("garbage.sqlite");
  ASSERT_TRUE(base::WriteFile(db_path, "this is definitely not a sqlite file"));

  NexusSqliteConnection conn(db_path, ModeOptions(SqliteOpenMode::kReadOnly));
  SqlResponse result = conn.Query("SELECT 1", base::ListValue(), {});
  EXPECT_FALSE(result.ok);
}

// An atomic batch that fails partway leaves the database unchanged.
TEST(NexusSqliteConnectionTest, AtomicBatchRollsBackOnFailure) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("batch.sqlite");

  NexusSqliteConnection conn(db_path,
                             ModeOptions(SqliteOpenMode::kReadWriteCreate));
  ASSERT_TRUE(conn.Execute("CREATE TABLE t (id INTEGER)", base::ListValue()).ok);

  std::vector<SqliteBatchStatement> statements;
  {
    SqliteBatchStatement good;
    good.sql = "INSERT INTO t VALUES (1)";
    statements.push_back(std::move(good));
    SqliteBatchStatement bad;
    bad.sql = "INSERT INTO nope VALUES (2)";  // no such table → fails
    statements.push_back(std::move(bad));
  }
  SqlResponse batch = conn.Batch(std::move(statements), /*atomic=*/true);
  EXPECT_FALSE(batch.ok);

  // The good insert must have rolled back.
  SqlResponse count = conn.Query("SELECT COUNT(*) FROM t", base::ListValue(), {});
  ASSERT_TRUE(count.ok) << count.message;
  const base::ListValue& row = (*count.value.GetDict().FindList("rows"))[0].GetList();
  EXPECT_EQ(row[0].GetInt(), 0);
}

// copyBeforeOpen reads a snapshot even while the original path is what the
// caller names.
TEST(NexusSqliteConnectionTest, CopyBeforeOpenReadsData) {
  base::test::TaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = temp_dir.GetPath().AppendASCII("copy.sqlite");
  {
    NexusSqliteConnection writer(db_path,
                                 ModeOptions(SqliteOpenMode::kReadWriteCreate));
    ASSERT_TRUE(
        writer.Execute("CREATE TABLE t (id INTEGER)", base::ListValue()).ok);
    ASSERT_TRUE(writer.Execute("INSERT INTO t VALUES (5)", base::ListValue()).ok);
  }

  SqliteOpenOptions options;
  options.mode = SqliteOpenMode::kReadOnly;
  options.copy_before_open = true;
  NexusSqliteConnection reader(db_path, options);
  ASSERT_TRUE(reader.OpenStatus().ok);
  SqlResponse result = reader.Query("SELECT id FROM t", base::ListValue(), {});
  ASSERT_TRUE(result.ok) << result.message;
  const base::ListValue& row = (*result.value.GetDict().FindList("rows"))[0].GetList();
  EXPECT_EQ(row[0].GetInt(), 5);
}

}  // namespace
}  // namespace nexus
