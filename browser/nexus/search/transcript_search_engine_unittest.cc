#include "custom_browser/browser/nexus/search/transcript_search_engine.h"

#include <algorithm>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/files/file_util.h"
#include "base/files/scoped_temp_dir.h"
#include "base/memory/scoped_refptr.h"
#include "base/strings/string_util.h"
#include "base/test/task_environment.h"
#include "custom_browser/browser/nexus/search/transcript_span_index_file.h"
#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

class TranscriptSearchEngineTest : public testing::Test {
 protected:
  void SetUp() override { ASSERT_TRUE(temp_.CreateUniqueTempDir()); }

  base::FilePath ProjectsRoot() { return temp_.GetPath(); }

  base::FilePath WriteTranscript(const std::string& project,
                                 const std::string& session,
                                 const std::string& contents) {
    const base::FilePath dir =
        ProjectsRoot().AppendASCII(project).AppendASCII(session);
    EXPECT_TRUE(base::CreateDirectory(dir));
    const base::FilePath path = dir.AppendASCII("transcript.jsonl");
    EXPECT_TRUE(base::WriteFile(path, contents));
    if (std::find(written_.begin(), written_.end(), path) ==
        written_.end()) {
      written_.push_back(path);
    }
    return path;
  }

  // Default target list: every transcript this test wrote, in write
  // order. The engine searches EXACTLY the order it is handed (the guest
  // owns relevance ranking), so tests that care about order pass their
  // own list via RunOn.
  TranscriptSearchResult Run(TranscriptSearchEngine* engine,
                             const std::string& needle,
                             bool regex = false,
                             bool case_sensitive = false,
                             int32_t max_total = 500) {
    return RunOn(engine, written_, needle, regex, case_sensitive, max_total);
  }

  TranscriptSearchResult RunOn(TranscriptSearchEngine* engine,
                               const std::vector<base::FilePath>& targets,
                               const std::string& needle,
                               bool regex = false,
                               bool case_sensitive = false,
                               int32_t max_total = 500) {
    TranscriptSearchQuery query;
    query.targets = targets;
    query.query = needle;
    query.regex = regex;
    query.case_sensitive = case_sensitive;
    query.max_total = max_total;
    query.generation = engine->BeginQuery();
    return engine->Search(query);
  }

  // The engine fans large searches out across the thread pool, so the
  // fixture must stand one up.
  base::test::TaskEnvironment task_environment_;
  base::ScopedTempDir temp_;
  std::vector<base::FilePath> written_;
};

std::string UserLine(const std::string& uuid, const std::string& text) {
  return R"({"type":"user","uuid":")" + uuid +
         R"(","message":{"content":")" + text + R"("}})" + "\n";
}

std::string AssistantLine(const std::string& uuid, const std::string& text) {
  return R"({"type":"assistant","uuid":")" + uuid +
         R"(","message":{"content":[{"text":")" + text +
         R"(","type":"text"}]}})" + "\n";
}

std::string ToolResultLine(const std::string& uuid, const std::string& text) {
  return R"({"type":"user","uuid":")" + uuid +
         R"(","message":{"content":[{"type":"tool-result","result":")" +
         text + R"(","toolCallId":"c"}]}})" + "\n";
}

TEST_F(TranscriptSearchEngineTest, FindsAndAttributesMatch) {
  WriteTranscript("proj-a", "sess-1",
                  UserLine("u-1", "please rebuild chromium today"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "chromium");
  ASSERT_TRUE(result.success);
  ASSERT_EQ(result.matches.size(), 1u);
  const TranscriptSearchMatch& match = result.matches[0];
  EXPECT_EQ(match.session_id, "sess-1");
  EXPECT_EQ(match.project_dir, "proj-a");
  EXPECT_EQ(match.message_uuid, "u-1");
  EXPECT_EQ(match.span_kind, "userText");
  EXPECT_EQ(match.line_ordinal, 0u);
  EXPECT_EQ(match.snippet.substr(static_cast<size_t>(match.match_start),
                                 static_cast<size_t>(match.match_len)),
            "chromium");
}

TEST_F(TranscriptSearchEngineTest, JsonKeysAndMetaLinesNeverMatch) {
  // "toolCallId" and "type" occur in JSON syntax only; a span-confined
  // search must not see them.
  WriteTranscript("proj-a", "sess-1",
                  R"({"type":"__session_meta","sessionId":"marker-xyz"})"
                  "\n" +
                      ToolResultLine("u-1", "plain result"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_TRUE(Run(engine.get(), "toolCallId").matches.empty());
  EXPECT_TRUE(Run(engine.get(), "sessionId").matches.empty());
  EXPECT_TRUE(Run(engine.get(), "marker-xyz").matches.empty());
  EXPECT_EQ(Run(engine.get(), "plain result").matches.size(), 1u);
}

TEST_F(TranscriptSearchEngineTest, PriorityOrderUserBeforeAssistantBeforeTools) {
  WriteTranscript("proj-a", "sess-1",
                  ToolResultLine("t-1", "needle in tool output") +
                      AssistantLine("a-1", "needle in assistant text") +
                      UserLine("u-1", "needle in user text"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "needle");
  ASSERT_EQ(result.matches.size(), 3u);
  EXPECT_EQ(result.matches[0].span_kind, "userText");
  EXPECT_EQ(result.matches[1].span_kind, "assistantText");
  EXPECT_EQ(result.matches[2].span_kind, "toolResult");
}

TEST_F(TranscriptSearchEngineTest, PerFileCapKeepsUserHits) {
  std::string contents;
  for (int i = 0; i < 12; ++i) {
    contents += ToolResultLine("t-" + std::to_string(i), "cap needle tool");
  }
  for (int i = 0; i < 4; ++i) {
    contents += UserLine("u-" + std::to_string(i), "cap needle user");
  }
  WriteTranscript("proj-a", "sess-1", contents);
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "cap needle");
  ASSERT_EQ(result.matches.size(), 10u);  // per-file cap
  for (size_t i = 0; i < 4; ++i) {
    EXPECT_EQ(result.matches[i].span_kind, "userText") << i;
  }
  for (size_t i = 4; i < 10; ++i) {
    EXPECT_EQ(result.matches[i].span_kind, "toolResult") << i;
  }
}

TEST_F(TranscriptSearchEngineTest, TotalCapSetsTruncated) {
  for (int s = 0; s < 3; ++s) {
    WriteTranscript("proj-a", "sess-" + std::to_string(s),
                    UserLine("u-1", "shared needle") +
                        UserLine("u-2", "shared needle"));
  }
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result =
      Run(engine.get(), "shared needle", false, false, /*max_total=*/3);
  EXPECT_EQ(result.matches.size(), 3u);
  EXPECT_TRUE(result.truncated);
}

std::string SystemLine(const std::string& uuid, const std::string& text) {
  return R"({"type":"system","subtype":"browser_input","uuid":")" + uuid +
         R"(","message":")" + text + R"("})" + "\n";
}

TEST_F(TranscriptSearchEngineTest, FindsSystemNoticesAfterAssistantText) {
  WriteTranscript(
      "proj-a", "sess-1",
      UserLine("u-1", "please continue") +
          AssistantLine("a-1", "approved the plan") +
          SystemLine("s-1",
                     "User has approved exiting plan mode. You can now "
                     "proceed."));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "approved");
  ASSERT_EQ(result.matches.size(), 2u);
  // Assistant text still outranks a system notice.
  EXPECT_EQ(result.matches[0].span_kind, "assistantText");
  EXPECT_EQ(result.matches[1].span_kind, "systemText");
  EXPECT_EQ(result.matches[1].message_uuid, "s-1");
}

TEST_F(TranscriptSearchEngineTest, CaseFoldingLiteral) {
  WriteTranscript("proj-a", "sess-1", UserLine("u-1", "MixedCase Token"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(engine.get(), "mixedcase").matches.size(), 1u);
  EXPECT_TRUE(Run(engine.get(), "mixedcase", false, /*case_sensitive=*/true)
                  .matches.empty());
  EXPECT_EQ(Run(engine.get(), "MixedCase", false, true).matches.size(), 1u);
}

TEST_F(TranscriptSearchEngineTest, RegexMatchAndBadPattern) {
  WriteTranscript("proj-a", "sess-1",
                  UserLine("u-1", "error code E1234 seen"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult ok = Run(engine.get(), "E\\d+", /*regex=*/true);
  ASSERT_EQ(ok.matches.size(), 1u);
  EXPECT_EQ(ok.matches[0].snippet.substr(
                static_cast<size_t>(ok.matches[0].match_start),
                static_cast<size_t>(ok.matches[0].match_len)),
            "E1234");

  TranscriptSearchResult bad = Run(engine.get(), "([", /*regex=*/true);
  EXPECT_FALSE(bad.success);
  EXPECT_EQ(bad.error_code, "SEARCH_BAD_PATTERN");
}

TEST_F(TranscriptSearchEngineTest, SnippetStartsAndEndsOnWholeWords) {
  // The window is a byte count, so without snapping it opens and closes
  // mid-word — "…ndful of nonsense wor" instead of whole words.
  std::string prefix;
  while (prefix.size() < 200) {
    prefix += "alpha bravo charlie delta echo foxtrot golf hotel ";
  }
  std::string suffix;
  while (suffix.size() < 200) {
    suffix += "india juliett kilo lima mike november oscar papa ";
  }
  WriteTranscript("proj-a", "sess-1",
                  UserLine("u-1", prefix + "NEEDLEWORD " + suffix));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "NEEDLEWORD");
  ASSERT_EQ(result.matches.size(), 1u);
  const std::string& snippet = result.matches[0].snippet;
  ASSERT_FALSE(snippet.empty());
  // Whole words at both ends: the fixture is all space-separated words,
  // so a clean cut means the first and last tokens are real words from it.
  const size_t first_space = snippet.find(' ');
  ASSERT_NE(first_space, std::string::npos);
  const std::string first_word = snippet.substr(0, first_space);
  const std::string last_word = snippet.substr(snippet.rfind(' ') + 1);
  EXPECT_NE(prefix.find(first_word + " "), std::string::npos)
      << "snippet opened mid-word: '" << first_word << "'";
  EXPECT_TRUE(suffix.find(last_word + " ") != std::string::npos ||
              last_word == "NEEDLEWORD")
      << "snippet closed mid-word: '" << last_word << "'";
}

TEST_F(TranscriptSearchEngineTest, SnippetUnescapesJson) {
  WriteTranscript("proj-a", "sess-1",
                  UserLine("u-1", R"(hello\nworld \"quoted\" tail)"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "world");
  ASSERT_EQ(result.matches.size(), 1u);
  const TranscriptSearchMatch& match = result.matches[0];
  EXPECT_EQ(match.snippet, "hello world \"quoted\" tail");
  EXPECT_EQ(match.snippet.substr(static_cast<size_t>(match.match_start),
                                 static_cast<size_t>(match.match_len)),
            "world");
}

TEST_F(TranscriptSearchEngineTest, CacheReusedThenTailRescanOnAppend) {
  const base::FilePath path = WriteTranscript(
      "proj-a", "sess-1", UserLine("u-1", "first entry alpha"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(engine.get(), "alpha").matches.size(), 1u);
  EXPECT_EQ(engine->IndexRunsForTesting(), 1);

  // Unchanged file: second query re-reads bytes but never re-indexes.
  EXPECT_EQ(Run(engine.get(), "alpha").matches.size(), 1u);
  EXPECT_EQ(engine->IndexRunsForTesting(), 1);

  // Append: exactly one more index run (the tail), old spans intact.
  std::string more = UserLine("u-2", "second entry beta");
  ASSERT_TRUE(base::AppendToFile(path, more));
  TranscriptSearchResult result = Run(engine.get(), "entry");
  EXPECT_EQ(result.matches.size(), 2u);
  EXPECT_EQ(engine->IndexRunsForTesting(), 2);
  EXPECT_EQ(engine->CachedFileCountForTesting(), 1u);
}

// The sidecar removes the first scan after a restart: a FRESH engine
// (nothing in memory) reuses the file a previous search left behind.
TEST_F(TranscriptSearchEngineTest, SidecarSurvivesAFreshEngine) {
  const base::FilePath path = WriteTranscript(
      "proj-a", "sess-1", UserLine("u-1", "persisted needle here"));
  auto first = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(first.get(), "persisted needle").matches.size(), 1u);
  EXPECT_EQ(first->IndexRunsForTesting(), 1);
  EXPECT_TRUE(base::PathExists(SpanIndexFilePathFor(path)));

  // A new engine has an empty cache, so without the sidecar it would
  // have to scan again.
  auto second = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(second.get(), "persisted needle").matches.size(), 1u);
  EXPECT_EQ(second->IndexRunsForTesting(), 0);
}

// An append after the sidecar was written is caught by the stamps and
// costs a TAIL rescan, which is then persisted.
TEST_F(TranscriptSearchEngineTest, SidecarIsRefreshedAfterAnAppend) {
  const base::FilePath path =
      WriteTranscript("proj-a", "sess-1", UserLine("u-1", "alpha entry"));
  auto first = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(first.get(), "alpha").matches.size(), 1u);

  ASSERT_TRUE(base::AppendToFile(path, UserLine("u-2", "beta entry")));
  auto second = base::MakeRefCounted<TranscriptSearchEngine>();
  // Both entries found, and only the tail was indexed.
  EXPECT_EQ(Run(second.get(), "entry").matches.size(), 2u);
  EXPECT_EQ(second->IndexRunsForTesting(), 1);

  // The refreshed sidecar now covers both, so a third engine scans none.
  auto third = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(third.get(), "entry").matches.size(), 2u);
  EXPECT_EQ(third->IndexRunsForTesting(), 0);
}

TEST_F(TranscriptSearchEngineTest, ShrunkFileTriggersFullRescan) {
  const base::FilePath path = WriteTranscript(
      "proj-a", "sess-1",
      UserLine("u-1", "longer original content here") +
          UserLine("u-2", "second line"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  EXPECT_EQ(Run(engine.get(), "second").matches.size(), 1u);

  // Replace with a shorter file (session delete + recreate).
  ASSERT_TRUE(base::WriteFile(path, UserLine("u-9", "fresh short")));
  TranscriptSearchResult result = Run(engine.get(), "fresh short");
  EXPECT_EQ(result.matches.size(), 1u);
  EXPECT_TRUE(Run(engine.get(), "second").matches.empty());
}

TEST_F(TranscriptSearchEngineTest, SupersededQueryBailsEarly) {
  WriteTranscript("proj-a", "sess-1", UserLine("u-1", "gamma value"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchQuery query;
  query.targets = written_;
  query.query = "gamma";
  query.generation = engine->BeginQuery();
  // A newer query arrives before this one runs.
  engine->BeginQuery();
  TranscriptSearchResult result = engine->Search(query);
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.superseded);
  EXPECT_TRUE(result.matches.empty());
}

// The core of the caller-ordered contract: the engine searches the list
// front-to-back exactly as handed to it. Nothing about the files
// themselves (mtime, name, size) may influence the order.
TEST_F(TranscriptSearchEngineTest, SearchesTargetsInTheOrderGiven) {
  const base::FilePath first =
      WriteTranscript("proj-a", "sess-first", UserLine("u-1", "shared marker"));
  const base::FilePath second = WriteTranscript(
      "proj-b", "sess-second", UserLine("u-2", "shared marker"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();

  TranscriptSearchResult forward =
      RunOn(engine.get(), {first, second}, "shared marker");
  ASSERT_EQ(forward.matches.size(), 2u);
  EXPECT_EQ(forward.matches[0].session_id, "sess-first");
  EXPECT_EQ(forward.matches[1].session_id, "sess-second");

  // Same files, reversed list → reversed results. (Under the old
  // mtime-sorted enumeration this ordering was not the caller's to pick.)
  TranscriptSearchResult reversed =
      RunOn(engine.get(), {second, first}, "shared marker");
  ASSERT_EQ(reversed.matches.size(), 2u);
  EXPECT_EQ(reversed.matches[0].session_id, "sess-second");
  EXPECT_EQ(reversed.matches[1].session_id, "sess-first");
}

// The same order contract across the PARALLEL path: enough targets to
// fan out over several workers, whose completion order is arbitrary.
// The merge is by target slot, so the response must still be exactly the
// caller's order.
TEST_F(TranscriptSearchEngineTest, ParallelFanOutStillReturnsTargetOrder) {
  std::vector<base::FilePath> targets;
  for (int i = 0; i < 16; ++i) {
    targets.push_back(WriteTranscript(
        "proj-a", "sess-" + std::to_string(i),
        UserLine("u-" + std::to_string(i), "fanout marker")));
  }
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();

  TranscriptSearchResult forward = RunOn(engine.get(), targets, "fanout");
  ASSERT_EQ(forward.matches.size(), targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    EXPECT_EQ(forward.matches[i].session_id, "sess-" + std::to_string(i)) << i;
  }

  std::vector<base::FilePath> reversed(targets.rbegin(), targets.rend());
  TranscriptSearchResult back = RunOn(engine.get(), reversed, "fanout");
  ASSERT_EQ(back.matches.size(), targets.size());
  for (size_t i = 0; i < targets.size(); ++i) {
    EXPECT_EQ(back.matches[i].session_id,
              "sess-" + std::to_string(targets.size() - 1 - i))
        << i;
  }
  // Every file indexed exactly once across both runs — the per-entry
  // locking must not have caused a duplicate rebuild.
  EXPECT_EQ(engine->IndexRunsForTesting(), 16);
}

// Only the listed targets are searched — a transcript sitting in the same
// projects root but absent from the list is invisible.
TEST_F(TranscriptSearchEngineTest, UnlistedTranscriptsAreNotSearched) {
  const base::FilePath listed =
      WriteTranscript("proj-a", "sess-1", UserLine("u-1", "orbit marker"));
  WriteTranscript("proj-a", "sess-2", UserLine("u-2", "orbit marker"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();

  TranscriptSearchResult result = RunOn(engine.get(), {listed}, "orbit marker");
  ASSERT_EQ(result.matches.size(), 1u);
  EXPECT_EQ(result.matches[0].session_id, "sess-1");
}

// A path that cannot be opened is skipped and counted, never an error —
// the guest's cached list can name a session deleted a moment ago.
TEST_F(TranscriptSearchEngineTest, MissingTargetIsSkippedNotFatal) {
  const base::FilePath real =
      WriteTranscript("proj-a", "sess-1", UserLine("u-1", "delta marker"));
  const base::FilePath ghost =
      ProjectsRoot().AppendASCII("gone").AppendASCII("transcript.jsonl");
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();

  TranscriptSearchResult result =
      RunOn(engine.get(), {ghost, real}, "delta marker");
  EXPECT_TRUE(result.success);
  EXPECT_EQ(result.skipped_files, 1);
  ASSERT_EQ(result.matches.size(), 1u);
  EXPECT_EQ(result.matches[0].session_id, "sess-1");
}

// Over the 2000-target sanity bound the TAIL is dropped — the guest
// ranked it least relevant — and `truncated` says so.
TEST_F(TranscriptSearchEngineTest, OverlongTargetListDropsTheTail) {
  const base::FilePath real =
      WriteTranscript("proj-a", "sess-1", UserLine("u-1", "tail marker"));
  const base::FilePath ghost =
      ProjectsRoot().AppendASCII("gone").AppendASCII("transcript.jsonl");
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();

  // 2000 unreadable placeholders, then the only real file → cut away.
  std::vector<base::FilePath> tail(2000, ghost);
  tail.push_back(real);
  TranscriptSearchResult dropped = RunOn(engine.get(), tail, "tail marker");
  EXPECT_TRUE(dropped.truncated);
  EXPECT_TRUE(dropped.matches.empty());

  // Same file at the FRONT is inside the bound and is found.
  std::vector<base::FilePath> kept{real};
  kept.insert(kept.end(), 2000, ghost);
  TranscriptSearchResult found = RunOn(engine.get(), kept, "tail marker");
  EXPECT_TRUE(found.truncated);
  EXPECT_EQ(found.matches.size(), 1u);
}

TEST_F(TranscriptSearchEngineTest, EmptyTargetListSucceedsWithNoMatches) {
  WriteTranscript("proj-a", "sess-1", UserLine("u-1", "unreachable"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = RunOn(engine.get(), {}, "unreachable");
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.matches.empty());
  EXPECT_FALSE(result.truncated);
}

// The snippet window is a fixed +-120 BYTES around the hit. Cutting it
// mid-character hands the bridge invalid UTF-8, and base::Value rejects
// that with a DCHECK — i.e. it crashes the debug browser on any search
// whose hit sits inside CJK (or any multi-byte) text.
TEST_F(TranscriptSearchEngineTest, SnippetStaysValidUtf8AcrossTheWindowEdge) {
  // 100x U+4E2D (3 bytes each) then one ASCII byte, so the window's
  // 120-byte reach lands INSIDE a character rather than on a boundary.
  const std::string cjk = []() {
    std::string out;
    for (int i = 0; i < 100; ++i) {
      out += "\xE4\xB8\xAD";
    }
    return out;
  }();
  WriteTranscript("proj-a", "sess-lead",
                  UserLine("u-1", cjk + "x" + "needle-marker"));
  // Same cut on the trailing edge.
  WriteTranscript("proj-a", "sess-trail",
                  UserLine("u-2", std::string("needle-marker") + "x" + cjk));

  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "needle-marker");
  ASSERT_EQ(result.matches.size(), 2u);
  for (const TranscriptSearchMatch& match : result.matches) {
    EXPECT_TRUE(base::IsStringUTF8(match.snippet))
        << "session " << match.session_id << " snippet is not valid UTF-8";
    // The hit itself must still be highlightable within the snippet.
    ASSERT_LE(static_cast<size_t>(match.match_start) +
                  static_cast<size_t>(match.match_len),
              match.snippet.size());
    EXPECT_EQ(match.snippet.substr(static_cast<size_t>(match.match_start),
                                   static_cast<size_t>(match.match_len)),
              "needle-marker");
  }
}

TEST_F(TranscriptSearchEngineTest, EmptyQuerySucceedsWithNoMatches) {
  WriteTranscript("proj-a", "sess-1", UserLine("u-1", "anything"));
  auto engine = base::MakeRefCounted<TranscriptSearchEngine>();
  TranscriptSearchResult result = Run(engine.get(), "");
  EXPECT_TRUE(result.success);
  EXPECT_TRUE(result.matches.empty());
}

TEST(UnescapeSnippetFragmentTest, MapsMatchOffsetsAcrossEscapes) {
  //            0123 456789
  // raw:       ab\ncd\"ef      (escapes: \n at 2, \" at 6)
  const std::string raw = "ab\\ncd\\\"ef";
  int32_t start = 0;
  int32_t len = 0;
  // Match "ef" at raw offsets [8, 10).
  const std::string decoded =
      UnescapeSnippetFragment(raw, 8, 10, &start, &len);
  EXPECT_EQ(decoded, "ab cd\"ef");
  EXPECT_EQ(decoded.substr(static_cast<size_t>(start),
                           static_cast<size_t>(len)),
            "ef");
}

TEST(UnescapeSnippetFragmentTest, DecodesUnicodeEscapes) {
  int32_t start = 0;
  int32_t len = 0;
  const std::string decoded = UnescapeSnippetFragment(
      "pre \\u4f60\\u597d post", 0, 3, &start, &len);
  EXPECT_EQ(decoded, "pre \xE4\xBD\xA0\xE5\xA5\xBD post");
  EXPECT_EQ(start, 0);
  EXPECT_EQ(len, 3);
}

TEST(UnescapeSnippetFragmentTest, RepairsGenuinelyMalformedBytes) {
  // The window-snapping above keeps a well-formed transcript intact, but
  // a file can hold real garbage. The decoder must still return something
  // base::Value accepts — a DCHECK there crashes the debug browser.
  int32_t start = 0;
  int32_t len = 0;
  const std::string decoded = UnescapeSnippetFragment(
      std::string("ok \xE4\xB8 truncated \xFF\xFE tail"), 0, 2, &start, &len);
  EXPECT_TRUE(base::IsStringUTF8(decoded));
  EXPECT_LE(static_cast<size_t>(start) + static_cast<size_t>(len),
            decoded.size());
}

TEST(UnescapeSnippetFragmentTest, SurrogatePairDecodes) {
  int32_t start = 0;
  int32_t len = 0;
  const std::string decoded = UnescapeSnippetFragment(
      "x \\ud83d\\ude00 y", 0, 1, &start, &len);
  EXPECT_EQ(decoded, "x \xF0\x9F\x98\x80 y");
}

}  // namespace
}  // namespace nexus
