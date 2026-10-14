// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/sqlite/nexus_sqlite_connection.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

// Records bridge responses so a test can assert how a dispatched action
// settled (or that it was dropped because the adapter went away).
class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
  int binary_count = 0;
  int error_count = 0;
  base::Value last_success;
  std::string last_error_code;

  void SendSuccessResponse(const std::string& /*id*/,
                           const base::DictValue& /*route*/,
                           base::Value result) override {
    ++success_count;
    last_success = std::move(result);
  }
  void SendBinaryResponse(const std::string& /*id*/,
                          const base::DictValue& /*route*/,
                          base::Value /*result*/,
                          mojo_base::BigBuffer /*data*/,
                          std::string_view /*result_binary_field*/) override {
    ++binary_count;
  }
  void SendErrorResponse(const std::string& /*id*/,
                         const base::DictValue& /*route*/,
                         const std::string& code,
                         const std::string& /*message*/) override {
    ++error_count;
    last_error_code = code;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue SqliteRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "claude");
  route.Set("capability", "sqlite");
  route.Set("action", std::string(action));
  return route;
}

// Seed a small SQLite file the adapter can read back.
base::FilePath SeedDatabase(const base::ScopedTempDir& dir) {
  const base::FilePath db_path = dir.GetPath().AppendASCII("seed.sqlite");
  SqliteOpenOptions options;
  options.mode = SqliteOpenMode::kReadWriteCreate;
  NexusSqliteConnection conn(db_path, options);
  EXPECT_TRUE(conn.Execute("CREATE TABLE t (id INTEGER, name TEXT)",
                           base::ListValue())
                  .ok);
  base::ListValue params;
  params.Append(7);
  params.Append("bob");
  EXPECT_TRUE(conn.Execute("INSERT INTO t VALUES (?, ?)", std::move(params)).ok);
  return db_path;
}

// One-shot query over a path resolves back on UI with rows.
TEST(NexusSqliteBridgeAdapterTest, OneShotQueryResolvesWithRows) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::DictValue payload;
  payload.Set("path", db_path.AsUTF8Unsafe());
  payload.Set("sql", "SELECT id, name FROM t");
  EXPECT_TRUE(adapter->Dispatch("query", "req-1", SqliteRoute("query"), payload,
                                {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
  ASSERT_TRUE(delegate.last_success.is_dict());
  const base::ListValue* rows = delegate.last_success.GetDict().FindList("rows");
  ASSERT_TRUE(rows);
  EXPECT_EQ(rows->size(), 1u);
}

// One-shot execute over a path writes and reports rowsAffected, then reaps the
// transient connection and its file lease.
TEST(NexusSqliteBridgeAdapterTest, OneShotExecuteReportsRowsAffected) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::DictValue payload;
  payload.Set("path", db_path.AsUTF8Unsafe());
  payload.Set("sql", "INSERT INTO t VALUES (8, 'carol')");
  EXPECT_TRUE(adapter->Dispatch("execute", "req-1", SqliteRoute("execute"),
                                payload, {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
  ASSERT_TRUE(delegate.last_success.is_dict());
  EXPECT_EQ(
      delegate.last_success.GetDict().FindInt("rowsAffected").value_or(-1), 1);
  EXPECT_EQ(adapter->OneShotCountForTesting(), 0u);
  EXPECT_EQ(adapter->FileEntryCountForTesting(), 0u);
}

// One-shot batch over a path runs every statement and reaps the connection.
TEST(NexusSqliteBridgeAdapterTest, OneShotBatchViaPathRunsAllStatements) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::ListValue statements;
  {
    base::DictValue s1;
    s1.Set("sql", "INSERT INTO t VALUES (1, 'x')");
    statements.Append(std::move(s1));
    base::DictValue s2;
    s2.Set("sql", "INSERT INTO t VALUES (2, 'y')");
    statements.Append(std::move(s2));
  }
  base::DictValue payload;
  payload.Set("path", db_path.AsUTF8Unsafe());
  payload.Set("statements", std::move(statements));
  EXPECT_TRUE(adapter->Dispatch("batch", "req-1", SqliteRoute("batch"), payload,
                                {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
  EXPECT_EQ(adapter->OneShotCountForTesting(), 0u);
  EXPECT_EQ(adapter->FileEntryCountForTesting(), 0u);
}

// More params than the SQL has placeholders is a graceful error, not a DCHECK
// abort (sql::Statement::Bind* DCHECKs on an out-of-range parameter index).
TEST(NexusSqliteBridgeAdapterTest, TooManyParamsErrorsNotCrash) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::ListValue params;
  params.Append(1);
  params.Append(2);  // one placeholder, two params
  base::DictValue payload;
  payload.Set("path", db_path.AsUTF8Unsafe());
  payload.Set("sql", "SELECT ?");
  payload.Set("params", std::move(params));
  EXPECT_TRUE(adapter->Dispatch("query", "req-1", SqliteRoute("query"), payload,
                                {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.error_count, 1);
  EXPECT_EQ(adapter->OneShotCountForTesting(), 0u);
  EXPECT_EQ(adapter->FileEntryCountForTesting(), 0u);
}

// Opening more than kMaxOpenHandles (64) persistent handles is refused rather
// than growing unbounded.
TEST(NexusSqliteBridgeAdapterTest, OpenHandleLimitEnforced) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  for (int i = 0; i < 64; ++i) {
    base::DictValue payload;
    payload.Set("path", db_path.AsUTF8Unsafe());
    EXPECT_TRUE(adapter->Dispatch("open", "r" + base::NumberToString(i),
                                  SqliteRoute("open"), payload, {}));
  }
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.success_count, 64);
  EXPECT_EQ(delegate.error_count, 0);

  base::DictValue overflow;
  overflow.Set("path", db_path.AsUTF8Unsafe());
  EXPECT_TRUE(adapter->Dispatch("open", "over", SqliteRoute("open"), overflow,
                                {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.error_count, 1);
  EXPECT_EQ(delegate.last_error_code, "SQLITE_TOO_MANY_HANDLES");
}

// A query against an unknown handle is a clean InvalidArgs error.
TEST(NexusSqliteBridgeAdapterTest, UnknownHandleErrors) {
  content::BrowserTaskEnvironment task_environment;
  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::DictValue payload;
  payload.Set("handle", "sqlite:999");
  payload.Set("sql", "SELECT 1");
  EXPECT_TRUE(adapter->Dispatch("query", "req-1", SqliteRoute("query"), payload,
                                {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.error_count, 1);
}

// open → query(handle) → close → query(handle) errors on the closed handle.
TEST(NexusSqliteBridgeAdapterTest, OpenQueryCloseHandleLifecycle) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::DictValue open_payload;
  open_payload.Set("path", db_path.AsUTF8Unsafe());
  EXPECT_TRUE(adapter->Dispatch("open", "r1", SqliteRoute("open"), open_payload,
                                {}));
  task_environment.RunUntilIdle();
  ASSERT_EQ(delegate.success_count, 1);
  ASSERT_TRUE(delegate.last_success.is_dict());
  const std::string* handle_ptr =
      delegate.last_success.GetDict().FindString("handle");
  ASSERT_TRUE(handle_ptr);
  const std::string handle = *handle_ptr;  // copy before last_success churns

  base::DictValue query_payload;
  query_payload.Set("handle", handle);
  query_payload.Set("sql", "SELECT COUNT(*) FROM t");
  EXPECT_TRUE(adapter->Dispatch("query", "r2", SqliteRoute("query"),
                                query_payload, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.success_count, 2);

  base::DictValue close_payload;
  close_payload.Set("handle", handle);
  EXPECT_TRUE(adapter->Dispatch("close", "r3", SqliteRoute("close"),
                                close_payload, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.success_count, 3);

  base::DictValue reuse_payload;
  reuse_payload.Set("handle", handle);
  reuse_payload.Set("sql", "SELECT 1");
  EXPECT_TRUE(adapter->Dispatch("query", "r4", SqliteRoute("query"),
                                reuse_payload, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.error_count, 1);
}

// Tearing the adapter down while a one-shot query is still in flight must not
// crash (the connection lives on its own sequence; the reply is weak-guarded).
TEST(NexusSqliteBridgeAdapterTest, DestroyingAdapterMidOpDoesNotCrash) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath db_path = SeedDatabase(temp_dir);

  RecordingDelegate delegate;
  auto adapter = std::make_unique<NexusSqliteBridgeAdapter>(&delegate);

  base::DictValue payload;
  payload.Set("path", db_path.AsUTF8Unsafe());
  payload.Set("sql", "SELECT id, name FROM t");
  EXPECT_TRUE(adapter->Dispatch("query", "req-1", SqliteRoute("query"), payload,
                                {}));

  adapter.reset();                  // teardown mid-flight
  task_environment.RunUntilIdle();  // must not crash

  EXPECT_EQ(delegate.success_count, 0);  // reply dropped cleanly
}

}  // namespace
}  // namespace nexus
