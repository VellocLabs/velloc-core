// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#include "custom_browser/browser/nexus/api/fs/nexus_fs_bridge_adapter.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "custom_browser/browser/nexus/api/fs/nexus_fs_watcher.h"
#include "base/strings/string_number_conversions.h"
#include "base/values.h"
#include "build/build_config.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "third_party/blink/public/common/messaging/message_port_descriptor.h"

#if BUILDFLAG(IS_WIN)
#include "base/test/file_path_reparse_point_win.h"
#endif

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
  // Host-pushed events, as (action, payload).
  std::vector<std::pair<std::string, base::Value>> events;

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
  void SendEvent(const base::DictValue& route, base::Value payload) override {
    const std::string* action = route.FindString("action");
    events.emplace_back(action ? *action : std::string(), std::move(payload));
  }
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

// Destroying the adapter while an fs op is still queued on the pool must not
// use-after-free it, and the response must drop cleanly.
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

// The wire key: `realpath {path, resolveLinks: true}` answers the
// link-resolved path, and a payload without it keeps the default form. The
// guest's containment checks send exactly this key; a rename on either side
// would silently reopen the link escape (the default form is what an older
// host answers too, so nothing would fail loudly).
TEST(NexusFsBridgeAdapterTest, RealpathResolveLinksKeyFollowsALink) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir root_dir;
  base::ScopedTempDir outside_dir;
  ASSERT_TRUE(root_dir.CreateUniqueTempDir());
  ASSERT_TRUE(outside_dir.CreateUniqueTempDir());
  const base::FilePath link = root_dir.GetPath().AppendASCII("link");
#if BUILDFLAG(IS_WIN)
  ASSERT_TRUE(base::CreateDirectory(link));
  std::optional<base::test::FilePathReparsePoint> junction =
      base::test::FilePathReparsePoint::Create(link, outside_dir.GetPath());
  ASSERT_TRUE(junction.has_value());
#else
  ASSERT_TRUE(base::CreateSymbolicLink(outside_dir.GetPath(), link));
#endif
  base::FilePath real_outside;
  ASSERT_TRUE(base::NormalizeFilePath(outside_dir.GetPath(), &real_outside));

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, root_dir.GetPath());
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("path", "link");
  payload.Set("resolveLinks", true);
  EXPECT_TRUE(adapter->Dispatch("realpath", "req-1", FsRoute("realpath"),
                                payload, {}));
  task_environment.RunUntilIdle();
  ASSERT_EQ(delegate.success_count, 1);
  ASSERT_TRUE(delegate.last_success.GetIfDict());
  const std::string* resolved =
      delegate.last_success.GetIfDict()->FindString("path");
  ASSERT_TRUE(resolved);
  EXPECT_EQ(real_outside.AsUTF8Unsafe(), *resolved);

#if BUILDFLAG(IS_WIN)
  payload.Remove("resolveLinks");
  EXPECT_TRUE(adapter->Dispatch("realpath", "req-2", FsRoute("realpath"),
                                payload, {}));
  task_environment.RunUntilIdle();
  ASSERT_EQ(delegate.success_count, 2);
  ASSERT_TRUE(delegate.last_success.GetIfDict());
  const std::string* normalized =
      delegate.last_success.GetIfDict()->FindString("path");
  ASSERT_TRUE(normalized);
  EXPECT_EQ(base::MakeAbsoluteFilePath(link).AsUTF8Unsafe(), *normalized);
#endif
}

// fs.watch end to end through the adapter: the watch answers with an id, a
// change arrives as one `fs.onChange` event naming it, and closing the guest
// connection releases the watch (no event after it).
TEST(NexusFsBridgeAdapterTest, WatchPushesOnChangeUntilTheConnectionCloses) {
  content::BrowserTaskEnvironment task_environment(
      base::test::TaskEnvironment::TimeSource::MOCK_TIME);
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());

  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  adapter->set_force_poll_for_testing(true);
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("path", temp_dir.GetPath().AsUTF8Unsafe());
  EXPECT_TRUE(adapter->Dispatch("watch", "req-1", FsRoute("watch"), payload,
                                {}));
  task_environment.RunUntilIdle();
  ASSERT_EQ(delegate.success_count, 1);
  ASSERT_TRUE(delegate.last_success.is_dict());
  const std::string* watch_id =
      delegate.last_success.GetDict().FindString("watchId");
  ASSERT_TRUE(watch_id);
  const std::string id = *watch_id;
  EXPECT_EQ(*delegate.last_success.GetDict().FindString("mode"), "poll");
  EXPECT_EQ(adapter->watch_count_for_testing(), 1u);

  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("touched.txt"), "x"));
  task_environment.FastForwardBy(NexusFsWatchCore::kPollInterval +
                                 NexusFsWatchCore::kDebounce);
  task_environment.RunUntilIdle();
  ASSERT_EQ(delegate.events.size(), 1u);
  EXPECT_EQ(delegate.events[0].first, "onChange");
  const base::DictValue& event = delegate.events[0].second.GetDict();
  EXPECT_EQ(*event.FindString("watchId"), id);
  EXPECT_EQ(*event.FindString("kind"), "created");
  ASSERT_TRUE(event.FindList("paths"));
  EXPECT_EQ(event.FindList("paths")->size(), 1u);

  adapter->OnBridgeConnectionClosed();
  EXPECT_EQ(adapter->watch_count_for_testing(), 0u);
  ASSERT_TRUE(
      base::WriteFile(temp_dir.GetPath().AppendASCII("after.txt"), "x"));
  task_environment.FastForwardBy(NexusFsWatchCore::kPollInterval * 2);
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.events.size(), 1u);
}

TEST(NexusFsBridgeAdapterTest, UnwatchIsIdempotentAndTheCapHolds) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  RecordingDelegate delegate;
  auto adapter =
      std::make_unique<NexusFsBridgeAdapter>(&delegate, temp_dir.GetPath());
  adapter->set_force_poll_for_testing(true);
  task_environment.RunUntilIdle();

  base::DictValue payload;
  payload.Set("path", temp_dir.GetPath().AsUTF8Unsafe());
  for (size_t i = 0; i < NexusFsBridgeAdapter::kMaxWatches; ++i) {
    EXPECT_TRUE(adapter->Dispatch("watch", "req", FsRoute("watch"), payload,
                                  {}));
  }
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.success_count,
            static_cast<int>(NexusFsBridgeAdapter::kMaxWatches));
  EXPECT_TRUE(
      adapter->Dispatch("watch", "req", FsRoute("watch"), payload, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.error_count, 1);
  EXPECT_EQ(delegate.last_error_code, "TOO_MANY_WATCHES");

  base::DictValue unwatch;
  unwatch.Set("watchId", "w1");
  EXPECT_TRUE(
      adapter->Dispatch("unwatch", "req", FsRoute("unwatch"), unwatch, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.last_success.GetDict().FindBool("found"), true);
  EXPECT_TRUE(
      adapter->Dispatch("unwatch", "req", FsRoute("unwatch"), unwatch, {}));
  task_environment.RunUntilIdle();
  EXPECT_EQ(delegate.last_success.GetDict().FindBool("found"), false);
  EXPECT_EQ(adapter->watch_count_for_testing(),
            NexusFsBridgeAdapter::kMaxWatches - 1);
}

}  // namespace
}  // namespace nexus
