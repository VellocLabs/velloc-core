#include "custom_browser/browser/nexus/tools/tool_session_registry.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/run_loop.h"
#include "base/synchronization/waitable_event.h"
#include "base/test/bind.h"
#include "base/threading/platform_thread.h"
#include "base/threading/thread_restrictions.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/files/nexus_files_bridge_adapter.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/tools/tool_base.h"
#include "mojo/public/cpp/base/big_buffer.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class RecordingDelegate : public NexusBridgeResponseDelegate {
 public:
  int success_count = 0;
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
                          std::string_view /*result_binary_field*/) override {}
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

base::DictValue Route(std::string_view capability, std::string_view action) {
  base::DictValue route;
  route.Set("namespace", "nexus");
  route.Set("capability", std::string(capability));
  route.Set("action", std::string(action));
  return route;
}

base::DictValue Context(std::string_view session_id) {
  base::DictValue context;
  context.Set("sessionId", std::string(session_id));
  return context;
}

void DispatchRead(NexusFilesBridgeAdapter& files,
                  const base::FilePath& file,
                  const base::DictValue& context,
                  const std::string& id) {
  base::DictValue payload;
  payload.Set("file_path", file.AsUTF8Unsafe());
  ASSERT_TRUE(files.Dispatch("read", id, Route("files", "read"), payload, {},
                             &context));
}

TEST(ToolSessionRegistryTest, SessionIsKeyedByTheFrameContext) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\nbeta\n"));

  RecordingDelegate delegate;
  ToolSessionRegistry registry(/*profile=*/nullptr);
  NexusFilesBridgeAdapter files(&delegate, &registry);

  DispatchRead(files, file, Context("s1"), "r1");
  DispatchRead(files, file, Context("s1"), "r2");
  task_environment.RunUntilIdle();
  EXPECT_EQ(2, delegate.success_count);
  EXPECT_EQ(1u, registry.session_count());
  EXPECT_TRUE(registry.HasSession("s1"));

  DispatchRead(files, file, Context("s2"), "r3");
  task_environment.RunUntilIdle();
  EXPECT_EQ(3, delegate.success_count);
  EXPECT_EQ(2u, registry.session_count());
}

TEST(ToolSessionRegistryTest, CallsWithinASessionRunInArrivalOrder) {
  content::BrowserTaskEnvironment task_environment;
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  const base::FilePath file = temp_dir.GetPath().AppendASCII("notes.txt");
  ASSERT_TRUE(base::WriteFile(file, "alpha\n"));

  RecordingDelegate delegate;
  ToolSessionRegistry registry(/*profile=*/nullptr);
  NexusFilesBridgeAdapter files(&delegate, &registry);

  // Park the session's blocking sequence; a read issued behind the parked
  // task must not complete until it is released, while another session's
  // read goes through.
  ToolSession& parked = registry.GetOrCreateSession("s1");
  base::WaitableEvent release;
  parked.blocking_runner()->PostTask(
      FROM_HERE, base::BindLambdaForTesting([&release] {
        base::ScopedAllowBaseSyncPrimitivesForTesting allow_wait;
        release.Wait();
      }));

  DispatchRead(files, file, Context("s1"), "behind");
  DispatchRead(files, file, Context("s2"), "other");
  base::RunLoop().RunUntilIdle();
  // Let the other session's pool task run without waiting for s1's.
  for (int i = 0; i < 50 && delegate.success_count == 0; ++i) {
    base::PlatformThread::Sleep(base::Milliseconds(10));
    base::RunLoop().RunUntilIdle();
  }
  EXPECT_EQ(1, delegate.success_count) << "s2 must not wait behind s1";

  release.Signal();
  task_environment.RunUntilIdle();
  EXPECT_EQ(2, delegate.success_count);
}

TEST(ToolSessionRegistryTest, StopErasesOnlyADeletedSession) {
  content::BrowserTaskEnvironment task_environment;
  ToolSessionRegistry registry(/*profile=*/nullptr);
  registry.GetOrCreateSession("s1");

  registry.OnSessionStopped("s1", OnStopReason::kUserStopped);
  EXPECT_TRUE(registry.HasSession("s1"));

  registry.OnSessionStopped("s1", OnStopReason::kSessionDeleted);
  EXPECT_FALSE(registry.HasSession("s1"));

  // Unknown keys are a no-op.
  registry.OnSessionStopped("never", OnStopReason::kSessionDeleted);
  EXPECT_EQ(0u, registry.session_count());
}

}  // namespace
}  // namespace nexus
