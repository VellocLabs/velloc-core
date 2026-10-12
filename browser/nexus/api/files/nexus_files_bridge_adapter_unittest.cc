#include "custom_browser/browser/nexus/api/files/nexus_files_bridge_adapter.h"

#include <string>
#include <utility>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/threading/scoped_blocking_call.h"
#include "base/values.h"
#include "content/public/test/browser_task_environment.h"
#include "custom_browser/browser/nexus/api/browser/nexus_browser_bridge_adapter.h"
#include "custom_browser/browser/nexus/nexus_bridge_response_delegate.h"
#include "custom_browser/browser/nexus/tools/tool_session_registry.h"
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
                          std::string_view /*result_binary_field*/) override {}
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

// The result envelope every host tool call answers with: `{data, …}`.
const std::string& DataText(const base::Value& result) {
  static const std::string kEmpty;
  const base::DictValue* dict = result.GetIfDict();
  if (!dict) {
    return kEmpty;
  }
  const std::string* data = dict->FindString("data");
  return data ? *data : kEmpty;
}

class NexusFilesBridgeAdapterTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    file_ = temp_dir_.GetPath().AppendASCII("notes.txt");
    ASSERT_TRUE(base::WriteFile(file_, "alpha\nbeta\n"));
  }

  content::BrowserTaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  base::FilePath file_;
  RecordingDelegate delegate_;
  ToolSessionRegistry registry_{/*profile=*/nullptr};
  NexusFilesBridgeAdapter files_{&delegate_, &registry_};
};

TEST_F(NexusFilesBridgeAdapterTest, ReadAnswersWithTheToolEnvelope) {
  base::DictValue payload;
  payload.Set("file_path", file_.AsUTF8Unsafe());
  const base::DictValue context = Context("s1");
  ASSERT_TRUE(files_.Dispatch("read", "r1", Route("files", "read"), payload,
                              {}, &context));
  task_environment_.RunUntilIdle();

  ASSERT_EQ(1, delegate_.success_count);
  EXPECT_EQ(0, delegate_.error_count);
  EXPECT_NE(std::string::npos, DataText(delegate_.last_success).find("1\talpha"))
      << delegate_.last_success;
}

TEST_F(NexusFilesBridgeAdapterTest, ToolErrorsKeepTheirCode) {
  base::DictValue payload;
  payload.Set("file_path",
              temp_dir_.GetPath().AppendASCII("missing.txt").AsUTF8Unsafe());
  const base::DictValue context = Context("s1");
  ASSERT_TRUE(files_.Dispatch("read", "r1", Route("files", "read"), payload,
                              {}, &context));
  task_environment_.RunUntilIdle();

  EXPECT_EQ(0, delegate_.success_count);
  ASSERT_EQ(1, delegate_.error_count);
  EXPECT_FALSE(delegate_.last_error_code.empty());
}

TEST_F(NexusFilesBridgeAdapterTest, MissingArgumentsAreInvalidArgs) {
  base::DictValue payload;  // no file_path
  const base::DictValue context = Context("s1");
  ASSERT_TRUE(files_.Dispatch("read", "r1", Route("files", "read"), payload,
                              {}, &context));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1, delegate_.error_count);
  EXPECT_EQ("INVALID_ARGS", delegate_.last_error_code);
  EXPECT_EQ(0u, registry_.session_count())
      << "a rejected frame never creates a session";
}

TEST_F(NexusFilesBridgeAdapterTest, EntryNeverBlocksTheUiThread) {
  base::DictValue payload;
  payload.Set("file_path", file_.AsUTF8Unsafe());
  const base::DictValue context = Context("s1");
  {
    // Any blocking call on the dispatch path would trip this in a debug
    // build; the body runs on the session's sequence afterwards.
    base::ScopedDisallowBlocking disallow_blocking;
    ASSERT_TRUE(files_.Dispatch("read", "r1", Route("files", "read"),
                                payload, {}, &context));
  }
  task_environment_.RunUntilIdle();
  EXPECT_EQ(1, delegate_.success_count);
}

TEST_F(NexusFilesBridgeAdapterTest, SecondReadInASessionAnswersFromTheLedger) {
  base::DictValue payload;
  payload.Set("file_path", file_.AsUTF8Unsafe());
  const base::DictValue context = Context("s1");
  ASSERT_TRUE(files_.Dispatch("read", "r1", Route("files", "read"), payload,
                              {}, &context));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(1, delegate_.success_count);
  EXPECT_NE(std::string::npos, DataText(delegate_.last_success).find("1	alpha"));

  // The same session's second read of an unchanged file is answered from
  // the session's read ledger, not from the disk: the session resolved by
  // the frame context carries that state.
  ASSERT_TRUE(files_.Dispatch("read", "r2", Route("files", "read"), payload,
                              {}, &context));
  task_environment_.RunUntilIdle();
  ASSERT_EQ(2, delegate_.success_count);
  // The ledger answers with the unchanged notice; the runtime renders the
  // sentence the model reads.
  const base::DictValue* notice =
      delegate_.last_success.GetDict().FindDict("data")->FindDict("notice");
  ASSERT_TRUE(notice);
  EXPECT_EQ("unchanged", *notice->FindString("kind"));
  EXPECT_EQ(1u, registry_.session_count());
}

TEST_F(NexusFilesBridgeAdapterTest, TheHostServesNoToolNamedRoutes) {
  // No `browser.<ToolName>` route: a tool name is not an action on any
  // family.
  NexusBrowserBridgeAdapter browser(&delegate_, &registry_);
  base::DictValue legacy;
  legacy.Set("file_path", file_.AsUTF8Unsafe());
  EXPECT_FALSE(browser.Dispatch("Read", "r1", Route("browser", "Read"), legacy,
                                {}, nullptr));
  EXPECT_FALSE(browser.Dispatch("BrowserClick", "r2",
                                Route("browser", "BrowserClick"), legacy, {},
                                nullptr));
  EXPECT_FALSE(files_.Dispatch("Read", "r3", Route("files", "Read"), legacy,
                               {}, nullptr));
  task_environment_.RunUntilIdle();
  EXPECT_EQ(0, delegate_.success_count);
  EXPECT_EQ(0, delegate_.error_count);
  EXPECT_EQ(0u, registry_.session_count());
}

}  // namespace
}  // namespace nexus
