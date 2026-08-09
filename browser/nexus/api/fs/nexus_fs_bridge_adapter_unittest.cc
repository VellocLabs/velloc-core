// Copyright 2026 The Custom Browser Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

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
                         const std::string& /*code*/,
                         const std::string& /*message*/) override {
    ++error_count;
  }
  void SendEvent(const base::DictValue& /*route*/,
                 base::Value /*payload*/) override {}
};

base::DictValue FsRoute(std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "claude");
  route.Set("capability", "fs");
  route.Set("action", std::string(action));
  return route;
}

// Happy path: a dispatched `cwd` hops to the pool and resolves back on UI
// with the working-directory string.
TEST(NexusFsBridgeAdapterTest, CwdResolvesAfterPoolHop) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();  // drain the ctor's CreateDirectory hop

  EXPECT_TRUE(adapter->Dispatch("cwd", "req-1", FsRoute("cwd"),
                                base::DictValue(), {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.success_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
  EXPECT_TRUE(delegate.last_success.is_string());
}

// Regression test for the fs-API teardown use-after-free (same class as the
// chrome.dll!nexus::FileTool::RunGlobOnThreadPool crash). The fs handlers
// used to be posted to base::ThreadPool via base::Unretained(adapter), so a
// port-service teardown (panel close / webview reload / shutdown) racing an
// in-flight fs op dereferenced the freed adapter (`this->fs_host_`) on the
// worker.
//
// The fix hops the work bound to a scoped_refptr<NexusFsHost> (NOT the
// adapter) and replies weak-guarded. Destroying the adapter while the op is
// still queued on the pool must therefore NOT crash, and the response must
// drop cleanly.
TEST(NexusFsBridgeAdapterTest,
     DestroyingAdapterMidOpDropsReplyWithoutUseAfterFree) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  EXPECT_TRUE(adapter->Dispatch("cwd", "req-1", FsRoute("cwd"),
                                base::DictValue(), {}));

  // Tear the adapter down (mirrors ~NexusBridgePortService) while the cwd
  // work is still queued on the pool. The worker holds the host via a
  // scoped_refptr, so the host survives; before the fix the worker
  // dereferenced the freed adapter.
  adapter.reset();

  task_environment.RunUntilIdle();  // must not crash

  // The reply was weak-guarded by the now-dead adapter, so it dropped.
  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.binary_count, 0);
  EXPECT_EQ(delegate.error_count, 0);
}

// Happy path over the BINARY reply path (`readFile` → RespondReadResult →
// done.Binary), which the cwd test (string Success) doesn't exercise: the
// op hops to the pool, reads the file, and settles as a binary response.
TEST(NexusFsBridgeAdapterTest, ReadFileResolvesBinaryAfterPoolHop) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("notes.txt"), "hello"));

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();  // drain the ctor's CreateDirectory hop

  base::DictValue payload;
  payload.Set("path", "notes.txt");
  EXPECT_TRUE(adapter->Dispatch("readFile", "req-1", FsRoute("readFile"),
                                payload, {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.binary_count, 1);
  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.error_count, 0);
}

// Same teardown race as DestroyingAdapterMidOpDropsReplyWithoutUseAfterFree,
// but over the binary (`readFile`) reply path — RespondReadResult /
// done.Binary, distinct from the cwd test's string Success. The worker
// binds a scoped_refptr<NexusFsHost> (not the adapter), so destroying the
// adapter while the read is queued must NOT crash, and the binary reply
// (weak-guarded by the dead adapter) must drop cleanly.
TEST(NexusFsBridgeAdapterTest,
     DestroyingAdapterMidReadFileDropsReplyWithoutUseAfterFree) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("notes.txt"), "hello"));

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("path", "notes.txt");
  EXPECT_TRUE(adapter->Dispatch("readFile", "req-1", FsRoute("readFile"),
                                payload, {}));

  // Tear the adapter down while the ReadFile work is still queued on the
  // pool. The worker keeps the host alive via its scoped_refptr, so the
  // read completes against a live host; the reply drops on the dead adapter.
  adapter.reset();

  task_environment.RunUntilIdle();  // must not crash

  EXPECT_EQ(delegate.binary_count, 0);
  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.error_count, 0);
}

// ---------------------------------------------------------------------------
// Batched routes.
// ---------------------------------------------------------------------------

// readBatch settles over the binary path with the index on the envelope and
// every item's bytes in the single concatenated buffer.
TEST(NexusFsBridgeAdapterTest, ReadBatchResolvesBinaryAfterPoolHop) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("b.txt"), "bb"));

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::ListValue items;
  for (const char* name : {"a.txt", "b.txt"}) {
    base::DictValue item;
    item.Set("path", name);
    items.Append(std::move(item));
  }
  base::DictValue payload;
  payload.Set("items", std::move(items));

  EXPECT_TRUE(adapter->Dispatch("readBatch", "req-1", FsRoute("readBatch"),
                                payload, {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.binary_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
}

TEST(NexusFsBridgeAdapterTest, ReaddirTreeResolvesAsJson) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  ASSERT_TRUE(base::WriteFile(temp_dir.GetPath().AppendASCII("a.txt"), "aaa"));

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("path", ".");
  payload.Set("maxDepth", 2);
  EXPECT_TRUE(adapter->Dispatch("readdirTree", "req-1", FsRoute("readdirTree"),
                                payload, {}));
  task_environment.RunUntilIdle();

  ASSERT_EQ(delegate.success_count, 1);
  EXPECT_EQ(delegate.error_count, 0);
  const base::DictValue* result = delegate.last_success.GetIfDict();
  ASSERT_TRUE(result);
  EXPECT_FALSE(result->FindBool("truncated").value_or(true));
  ASSERT_TRUE(result->FindList("entries"));
  EXPECT_EQ(1u, result->FindList("entries")->size());
}

// Over-cap is rejected, never silently truncated: a short answer to an
// existence check would mislabel real files as missing.
TEST(NexusFsBridgeAdapterTest, StatBatchOverLimitRejectsWithInvalidArgs) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::ListValue paths;
  for (int i = 0; i < 2001; ++i) {
    paths.Append("f.txt");
  }
  base::DictValue payload;
  payload.Set("paths", std::move(paths));

  EXPECT_TRUE(adapter->Dispatch("statBatch", "req-1", FsRoute("statBatch"),
                                payload, {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.error_count, 1);
  EXPECT_EQ(delegate.success_count, 0);
}

// An item with no "path" is rejected outright rather than skipped. The
// codegen array<object> extractor already drops non-dicts, and a silently
// shorter list would shift every later index in the response — attributing
// one session's bytes to another.
TEST(NexusFsBridgeAdapterTest, ReadBatchRejectsAnItemWithoutAPath) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::ListValue items;
  items.Append(base::DictValue());  // no "path"
  base::DictValue payload;
  payload.Set("items", std::move(items));

  EXPECT_TRUE(adapter->Dispatch("readBatch", "req-1", FsRoute("readBatch"),
                                payload, {}));
  task_environment.RunUntilIdle();

  EXPECT_EQ(delegate.error_count, 1);
  EXPECT_EQ(delegate.binary_count, 0);
}

// The teardown race again, but over a BATCH. It matters more than its two
// predecessors: a batch performs O(N) opens on the worker, so the window
// between "adapter destroyed" and "reply runs" is far wider than a single
// read's. Its real value is failing loudly if someone ever binds `this`
// instead of `fs_host_` in the new handler.
TEST(NexusFsBridgeAdapterTest,
     DestroyingAdapterMidReadBatchDropsReplyWithoutUseAfterFree) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  base::ListValue items;
  for (int i = 0; i < 32; ++i) {
    const std::string name = "f" + base::NumberToString(i) + ".txt";
    ASSERT_TRUE(
        base::WriteFile(temp_dir.GetPath().AppendASCII(name), "payload"));
    base::DictValue item;
    item.Set("path", name);
    items.Append(std::move(item));
  }

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("items", std::move(items));
  EXPECT_TRUE(adapter->Dispatch("readBatch", "req-1", FsRoute("readBatch"),
                                payload, {}));

  adapter.reset();
  task_environment.RunUntilIdle();  // must not crash

  EXPECT_EQ(delegate.binary_count, 0);
  EXPECT_EQ(delegate.success_count, 0);
  EXPECT_EQ(delegate.error_count, 0);
}

}  // namespace
}  // namespace nexus
