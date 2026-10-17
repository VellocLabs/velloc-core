#include "custom_browser/log/bridge_audit_log.h"

#include <map>
#include <string>
#include <vector>

#include "base/files/file.h"
#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/functional/bind.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_split.h"
#include "base/strings/strcat.h"
#include "base/task/thread_pool.h"
#include "base/test/task_environment.h"
#include "base/threading/platform_thread.h"
#include "build/build_config.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace custom_browser::log {
namespace {

class BridgeAuditLogTest : public testing::Test {
 protected:
  void SetUp() override {
    ASSERT_TRUE(temp_dir_.CreateUniqueTempDir());
    log_file_ = temp_dir_.GetPath().AppendASCII("bridge_audit.log");
    BridgeAuditWriter::Get().SetPathForTesting(log_file_);
  }

  void TearDown() override {
    task_environment_.RunUntilIdle();
    BridgeAuditWriter::Get().SetPathForTesting(base::FilePath());
  }

  std::vector<std::string> ReadLines() {
    std::string contents;
    EXPECT_TRUE(base::ReadFileToString(log_file_, &contents));
    return base::SplitString(contents, "\n", base::KEEP_WHITESPACE,
                             base::SPLIT_WANT_NONEMPTY);
  }

  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_dir_;
  base::FilePath log_file_;
};

// Many producers on different pool threads commit at once: every line lands,
// and each producer's lines keep their commit order.
TEST_F(BridgeAuditLogTest, ConcurrentAppendsLoseNoLine) {
  constexpr int kProducers = 16;
  constexpr int kLinesPerProducer = 200;
  for (int p = 0; p < kProducers; ++p) {
    base::ThreadPool::PostTask(
        FROM_HERE, {base::MayBlock(), base::TaskPriority::USER_BLOCKING},
        base::BindOnce(
            [](int producer) {
              for (int n = 0; n < kLinesPerProducer; ++n) {
                AppendBridgeAuditLine(base::StrCat(
                    {"p=", base::NumberToString(producer),
                     "|n=", base::NumberToString(n)}));
              }
            },
            p));
  }
  task_environment_.RunUntilIdle();

  const std::vector<std::string> lines = ReadLines();
  ASSERT_EQ(lines.size(), static_cast<size_t>(kProducers * kLinesPerProducer));
  std::map<int, int> next_by_producer;
  for (const std::string& line : lines) {
    const std::vector<std::string> fields = base::SplitString(
        line, "|", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
    ASSERT_EQ(fields.size(), 2u) << line;
    int producer = -1;
    int n = -1;
    ASSERT_TRUE(base::StringToInt(fields[0].substr(2), &producer)) << line;
    ASSERT_TRUE(base::StringToInt(fields[1].substr(2), &n)) << line;
    EXPECT_EQ(n, next_by_producer[producer]) << line;
    next_by_producer[producer] = n + 1;
  }
  ASSERT_EQ(next_by_producer.size(), static_cast<size_t>(kProducers));
  for (const auto& [producer, next] : next_by_producer) {
    EXPECT_EQ(next, kLinesPerProducer) << "producer " << producer;
  }
}

// A reader holding the file open (with the usual read/write sharing) does not
// block appends.
TEST_F(BridgeAuditLogTest, AppendsWhileReaderHoldsFile) {
  AppendBridgeAuditLine("first");
  task_environment_.RunUntilIdle();

  base::File reader(log_file_, base::File::FLAG_OPEN | base::File::FLAG_READ);
  ASSERT_TRUE(reader.IsValid());
  for (int n = 0; n < 50; ++n) {
    AppendBridgeAuditLine(base::StrCat({"n=", base::NumberToString(n)}));
  }
  task_environment_.RunUntilIdle();
  reader.Close();

  const std::vector<std::string> lines = ReadLines();
  ASSERT_EQ(lines.size(), 51u);
  EXPECT_EQ(lines.front(), "first");
  EXPECT_EQ(lines.back(), "n=49");
}

#if BUILDFLAG(IS_WIN)
// A holder that denies write sharing only delays the append: the open is
// retried until the holder lets go.
TEST_F(BridgeAuditLogTest, RetriesUntilExclusiveHolderReleases) {
  AppendBridgeAuditLine("first");
  task_environment_.RunUntilIdle();

  base::File holder(log_file_, base::File::FLAG_OPEN | base::File::FLAG_READ |
                                   base::File::FLAG_WIN_EXCLUSIVE_WRITE);
  ASSERT_TRUE(holder.IsValid());
  AppendBridgeAuditLine("second");
  base::PlatformThread::Sleep(kBridgeAuditOpenRetryDelay * 4);
  holder.Close();
  task_environment_.RunUntilIdle();

  EXPECT_EQ(ReadLines(), (std::vector<std::string>{"first", "second"}));
}
#endif  // BUILDFLAG(IS_WIN)

}  // namespace
}  // namespace custom_browser::log
