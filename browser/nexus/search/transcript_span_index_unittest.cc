#include "custom_browser/browser/nexus/search/transcript_span_index.h"

#include <string>
#include <string_view>

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

std::string_view Slice(std::string_view data, const TranscriptSpan& span) {
  return data.substr(static_cast<size_t>(span.begin),
                     static_cast<size_t>(span.end - span.begin));
}

TranscriptSpanIndex IndexOf(std::string_view data) {
  TranscriptSpanIndex index;
  IndexTranscriptChunk(data, &index);
  return index;
}

TEST(TranscriptSpanIndexTest, PlainStringUserContent) {
  const std::string data =
      R"({"type":"user","uuid":"u-1","message":{"role":"user","content":"hello chromium"},"isMeta":false})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.user_text[0]), "hello chromium");
  ASSERT_EQ(index.lines.size(), 1u);
  EXPECT_EQ(index.lines[0].uuid, "u-1");
  EXPECT_EQ(index.lines[0].line_ordinal, 0u);
  EXPECT_EQ(index.indexed_end, data.size());
}

TEST(TranscriptSpanIndexTest, AssistantTextAndToolCallBlocks) {
  // Real writer shape: text block puts `text` BEFORE `type`.
  const std::string data =
      R"({"type":"assistant","uuid":"a-1","message":{"role":"assistant","content":[)"
      R"({"text":"I will inspect the docs","type":"text"},)"
      R"({"type":"tool-call","toolCallId":"c1","toolName":"Read","args":{"file_path":"D:\\proj\\x.md"}}]}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.assistant_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.assistant_text[0]), "I will inspect the docs");
  ASSERT_EQ(index.tool_call.size(), 2u);
  EXPECT_EQ(Slice(data, index.tool_call[0]), "Read");
  EXPECT_EQ(Slice(data, index.tool_call[1]),
            R"({"file_path":"D:\\proj\\x.md"})");
  EXPECT_TRUE(index.user_text.empty());
}

TEST(TranscriptSpanIndexTest, UserRowThatIsToolResultGetsNoUserSpan) {
  const std::string data =
      R"({"type":"user","uuid":"u-2","message":{"role":"user","content":[)"
      R"({"type":"tool-result","result":"Todos modified","toolCallId":"c1"}]},"toolUseResult":"dup"})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  EXPECT_TRUE(index.user_text.empty());
  ASSERT_EQ(index.tool_result.size(), 1u);
  EXPECT_EQ(Slice(data, index.tool_result[0]), "Todos modified");
}

TEST(TranscriptSpanIndexTest, StructuredToolResultUsesWholeValueRange) {
  const std::string data =
      R"({"type":"user","uuid":"u-3","message":{"content":[)"
      R"({"type":"tool-result","result":{"rows":[1,2],"note":"ok"},"toolCallId":"c2"}]}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.tool_result.size(), 1u);
  EXPECT_EQ(Slice(data, index.tool_result[0]),
            R"({"rows":[1,2],"note":"ok"})");
}

// The non-message records a transcript carries — a `custom-title` action
// record (and, in pre-2026-08-18 files, legacy `__session_meta` snapshots) —
// are not indexable; neither is an attachment or an `isMeta` user line. The
// indexer allow-lists `user` / `assistant` / `system`, so their TYPE names are
// never consulted — only their physical lines count for the ordinals.
TEST(TranscriptSpanIndexTest, MetaAndAttachmentAndIsMetaLinesSkipped) {
  const std::string data =
      R"({"type":"custom-title","sessionId":"s","customTitle":"Named","updatedAt":1})"
      "\n"
      R"({"type":"attachment","uuid":"at-1","attachment":{"content":"skill listing text"}})"
      "\n"
      R"({"type":"user","uuid":"u-4","isMeta":true,"message":{"content":"injected context"}})"
      "\n"
      R"({"type":"user","uuid":"u-5","message":{"content":"real question"}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.user_text[0]), "real question");
  ASSERT_EQ(index.lines.size(), 1u);
  // Ordinals count every physical line, so the surviving row is line 3.
  EXPECT_EQ(index.lines[0].line_ordinal, 3u);
  EXPECT_EQ(index.lines[0].uuid, "u-5");
}

// A `system` line keeps its text in a TOP-LEVEL `message` string, not in
// `message.content`. Skipping those lines made real notices — "User has
// approved exiting plan mode…" — unfindable.
TEST(TranscriptSpanIndexTest, SystemLineMessageIsSearchable) {
  const std::string data =
      R"({"type":"system","subtype":"browser_input","uuid":"s-1",)"
      R"("message":"User has approved exiting plan mode. You can now proceed."})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.system_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.system_text[0]),
            "User has approved exiting plan mode. You can now proceed.");
  ASSERT_EQ(index.lines.size(), 1u);
  EXPECT_EQ(index.lines[0].uuid, "s-1");
  // It is its own kind, not folded into user or assistant text.
  EXPECT_TRUE(index.user_text.empty());
  EXPECT_TRUE(index.assistant_text.empty());
}

// The same key on a user/assistant row is an OBJECT and must not be
// mistaken for system text.
TEST(TranscriptSpanIndexTest, ObjectMessageIsNotSystemText) {
  const std::string data =
      R"({"type":"user","uuid":"u-1","message":{"content":"plain ask"}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  EXPECT_TRUE(index.system_text.empty());
  ASSERT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.user_text[0]), "plain ask");
}

// An isMeta system row is injected context (createLocalizedSystemMessage
// defaults isMeta:true), not conversation — skip it exactly as an isMeta
// user/assistant row is skipped, so it is neither a searchable span nor a
// jump anchor pointing at a line the guest does not render.
TEST(TranscriptSpanIndexTest, SystemLineWithIsMetaIsSkipped) {
  const std::string data =
      R"({"type":"system","subtype":"informational","isMeta":true,)"
      R"("uuid":"s-2","message":"Injected context the user never sees."})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  EXPECT_TRUE(index.system_text.empty());
  EXPECT_TRUE(index.lines.empty());
}

TEST(TranscriptSpanIndexTest, EscapedQuotesInsideValues) {
  const std::string data =
      R"({"type":"user","uuid":"u-6","message":{"content":"say \"hi\\\" there\" now"}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.user_text[0]),
            R"(say \"hi\\\" there\" now)");
}

TEST(TranscriptSpanIndexTest, CrlfLines) {
  const std::string data =
      "{\"type\":\"user\",\"uuid\":\"u-7\",\"message\":{\"content\":\"crlf line\"}}\r\n"
      "{\"type\":\"user\",\"uuid\":\"u-8\",\"message\":{\"content\":\"second\"}}\r\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.user_text.size(), 2u);
  EXPECT_EQ(Slice(data, index.user_text[0]), "crlf line");
  EXPECT_EQ(Slice(data, index.user_text[1]), "second");
  EXPECT_EQ(index.indexed_end, data.size());
}

TEST(TranscriptSpanIndexTest, TrailingUnterminatedLineDeferred) {
  const std::string complete =
      R"({"type":"user","uuid":"u-9","message":{"content":"done line"}})"
      "\n";
  const std::string partial = R"({"type":"user","uuid":"u-10","mess)";
  TranscriptSpanIndex index = IndexOf(complete + partial);
  EXPECT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(index.indexed_end, complete.size());
  EXPECT_EQ(index.next_line_ordinal, 1u);
}

TEST(TranscriptSpanIndexTest, TailAppendResumesAtOrdinalAndOffset) {
  const std::string first =
      R"({"type":"user","uuid":"u-11","message":{"content":"first"}})"
      "\n";
  const std::string second =
      R"({"type":"assistant","uuid":"a-2","message":{"content":[{"text":"reply","type":"text"}]}})"
      "\n";
  TranscriptSpanIndex index;
  IndexTranscriptChunk(first, &index);
  ASSERT_EQ(index.indexed_end, first.size());
  IndexTranscriptChunk(second, &index);
  const std::string all = first + second;
  ASSERT_EQ(index.assistant_text.size(), 1u);
  EXPECT_EQ(Slice(all, index.assistant_text[0]), "reply");
  ASSERT_EQ(index.lines.size(), 2u);
  EXPECT_EQ(index.lines[1].line_ordinal, 1u);
  EXPECT_EQ(index.indexed_end, all.size());
}

TEST(TranscriptSpanIndexTest, MalformedLineContributesNothing) {
  const std::string data =
      "{\"type\":\"user\",\"message\":{\"content\":\"broken\n"
      R"({"type":"user","uuid":"u-12","message":{"content":"good"}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.user_text.size(), 1u);
  EXPECT_EQ(Slice(data, index.user_text[0]), "good");
  EXPECT_EQ(index.lines.size(), 1u);
}

TEST(TranscriptSpanIndexTest, AnthropicSpellingsAlsoClassify) {
  const std::string data =
      R"({"type":"assistant","uuid":"a-3","message":{"content":[)"
      R"({"type":"tool_use","name":"Edit","input":{"path":"a.txt"}},)"
      R"({"type":"thinking","thinking":"secret plan"}]}})"
      "\n"
      R"({"type":"user","uuid":"u-13","message":{"content":[)"
      R"({"type":"tool_result","content":"edited ok"}]}})"
      "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.tool_call.size(), 2u);
  EXPECT_EQ(Slice(data, index.tool_call[0]), "Edit");
  ASSERT_EQ(index.tool_result.size(), 1u);
  EXPECT_EQ(Slice(data, index.tool_result[0]), "edited ok");
  // Thinking is not searchable.
  EXPECT_TRUE(index.assistant_text.empty());
}

TEST(TranscriptSpanIndexTest, HugeSingleLine) {
  std::string big(5 * 1024 * 1024, 'x');
  const std::string data =
      R"({"type":"user","uuid":"u-14","message":{"content":[)"
      R"({"type":"tool-result","result":")" +
      big + R"("}]}})" + "\n";
  TranscriptSpanIndex index = IndexOf(data);
  ASSERT_EQ(index.tool_result.size(), 1u);
  EXPECT_EQ(index.tool_result[0].end - index.tool_result[0].begin,
            big.size());
}

}  // namespace
}  // namespace nexus
