#include "custom_browser/browser/nexus/search/transcript_span_index_file.h"

#include <string>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/search/transcript_span_index.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class SpanIndexFileTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_.CreateUniqueTempDir()); }

  base::FilePath Sidecar() {
    return temp_.GetPath().AppendASCII("transcript.sdi");
  }

  base::ScopedTempDir temp_;
};

TranscriptSpanIndex BuildIndex() {
  const std::string data =
      R"({"type":"user","uuid":"u-1","message":{"content":"first ask"}})"
      "\n"
      R"({"type":"assistant","uuid":"a-1","message":{"content":[)"
      R"({"text":"a reply","type":"text"},)"
      R"({"type":"tool-call","toolName":"Read","args":{"p":"x"}}]}})"
      "\n"
      R"({"type":"system","subtype":"informational","uuid":"s-1",)"
      R"("message":"a notice"})"
      "\n";
  TranscriptSpanIndex index;
  IndexTranscriptChunk(data, &index);
  return index;
}

TEST_F(SpanIndexFileTest, RoundTripsEverySpanKindAndLine) {
  const TranscriptSpanIndex original = BuildIndex();
  ASSERT_TRUE(WriteSpanIndexFile(Sidecar(), original, 4242,
                                 base::Time::FromDeltaSinceWindowsEpoch(
                                     base::Microseconds(1700000000000))));

  TranscriptSpanIndex loaded;
  int64_t size = 0;
  base::Time mtime;
  ASSERT_TRUE(ReadSpanIndexFile(Sidecar(), &loaded, &size, &mtime));

  EXPECT_EQ(size, 4242);
  EXPECT_EQ(mtime.ToDeltaSinceWindowsEpoch().InMicroseconds(), 1700000000000);
  EXPECT_EQ(loaded.indexed_end, original.indexed_end);
  EXPECT_EQ(loaded.next_line_ordinal, original.next_line_ordinal);
  EXPECT_EQ(loaded.user_text.size(), original.user_text.size());
  EXPECT_EQ(loaded.assistant_text.size(), original.assistant_text.size());
  EXPECT_EQ(loaded.system_text.size(), original.system_text.size());
  EXPECT_EQ(loaded.tool_call.size(), original.tool_call.size());
  ASSERT_EQ(loaded.lines.size(), original.lines.size());
  for (size_t i = 0; i < loaded.lines.size(); ++i) {
    EXPECT_EQ(loaded.lines[i].uuid, original.lines[i].uuid) << i;
    EXPECT_EQ(loaded.lines[i].line_begin, original.lines[i].line_begin) << i;
    EXPECT_EQ(loaded.lines[i].line_ordinal, original.lines[i].line_ordinal)
        << i;
  }
  ASSERT_FALSE(loaded.user_text.empty());
  EXPECT_EQ(loaded.user_text[0].begin, original.user_text[0].begin);
  EXPECT_EQ(loaded.user_text[0].end, original.user_text[0].end);
}

TEST_F(SpanIndexFileTest, MissingFileIsNotAnError) {
  TranscriptSpanIndex loaded;
  int64_t size = 0;
  base::Time mtime;
  EXPECT_FALSE(ReadSpanIndexFile(
      temp_.GetPath().AppendASCII("absent.sdi"), &loaded, &size, &mtime));
}

TEST_F(SpanIndexFileTest, RejectsATornWriteInsteadOfTrustingOffsets) {
  ASSERT_TRUE(WriteSpanIndexFile(Sidecar(), BuildIndex(), 10, base::Time()));
  std::string blob;
  ASSERT_TRUE(base::ReadFileToString(Sidecar(), &blob));
  ASSERT_GT(blob.size(), 40u);
  // Half a file — the recorded total no longer matches its length.
  ASSERT_TRUE(base::WriteFile(Sidecar(), blob.substr(0, blob.size() / 2)));

  TranscriptSpanIndex loaded;
  int64_t size = 0;
  base::Time mtime;
  EXPECT_FALSE(ReadSpanIndexFile(Sidecar(), &loaded, &size, &mtime));
}

TEST_F(SpanIndexFileTest, RejectsAForeignFile) {
  ASSERT_TRUE(base::WriteFile(Sidecar(), std::string(512, 'x')));
  TranscriptSpanIndex loaded;
  int64_t size = 0;
  base::Time mtime;
  EXPECT_FALSE(ReadSpanIndexFile(Sidecar(), &loaded, &size, &mtime));
}

TEST_F(SpanIndexFileTest, SidecarSitsBesideTheTranscript) {
  const base::FilePath transcript =
      base::FilePath(FILE_PATH_LITERAL("/tmp/sess/transcript.jsonl"));
  EXPECT_EQ(SpanIndexFilePathFor(transcript).BaseName().AsUTF8Unsafe(),
            "transcript.sdi");
  EXPECT_EQ(SpanIndexFilePathFor(transcript).DirName(), transcript.DirName());
}

}  // namespace
}  // namespace nexus
