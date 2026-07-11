// Copyright 2026 The Chromium Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "custom_browser/browser/nexus/api/llm/llm_line_splitter.h"

#include "testing/gtest/include/gtest/gtest.h"

namespace nexus {
namespace {

TEST(LineSplitterTest, EmitsNoLinesUntilNewlineSeen) {
  LineSplitter s;
  EXPECT_TRUE(s.Feed("partial line with no terminator").empty());
  EXPECT_FALSE(s.empty());
}

TEST(LineSplitterTest, SingleLineLf) {
  LineSplitter s;
  auto lines = s.Feed("hello\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "hello");
  EXPECT_TRUE(s.empty());
}

TEST(LineSplitterTest, SingleLineCrlfIsStripped) {
  LineSplitter s;
  auto lines = s.Feed("hello\r\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "hello");
}

TEST(LineSplitterTest, MultipleLinesInOneFeed) {
  LineSplitter s;
  auto lines = s.Feed("one\ntwo\nthree\n");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "one");
  EXPECT_EQ(lines[1], "two");
  EXPECT_EQ(lines[2], "three");
}

TEST(LineSplitterTest, LineStraddlingTwoFeeds) {
  LineSplitter s;
  EXPECT_TRUE(s.Feed("hel").empty());
  EXPECT_TRUE(s.Feed("lo").empty());
  auto lines = s.Feed(", world\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "hello, world");
}

TEST(LineSplitterTest, EmptyLineBetweenContentLinesEmitsEmptyString) {
  LineSplitter s;
  // SSE uses "\n\n" as an event separator — LineSplitter emits the blank
  // in between as an empty string so the consumer can use it as the event
  // boundary.
  auto lines = s.Feed("first\n\nsecond\n");
  ASSERT_EQ(lines.size(), 3u);
  EXPECT_EQ(lines[0], "first");
  EXPECT_EQ(lines[1], "");
  EXPECT_EQ(lines[2], "second");
}

TEST(LineSplitterTest, TrailingBytesRemainBuffered) {
  LineSplitter s;
  auto lines = s.Feed("complete\nincomplete");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "complete");
  EXPECT_FALSE(s.empty());

  EXPECT_EQ(s.DrainRemaining(), "incomplete");
  EXPECT_TRUE(s.empty());
  EXPECT_EQ(s.DrainRemaining(), "");
}

TEST(LineSplitterTest, DrainRemainingDoesNotStripTrailingCr) {
  // DrainRemaining is only invoked when the stream ends without a newline
  // — the raw tail is what callers want to see. A dangling "\r" is treated
  // as data, not a partial CRLF terminator.
  LineSplitter s;
  EXPECT_TRUE(s.Feed("tail\r").empty());
  EXPECT_EQ(s.DrainRemaining(), "tail\r");
}

TEST(LineSplitterTest, CrlfStraddlingFeeds) {
  LineSplitter s;
  EXPECT_TRUE(s.Feed("hello\r").empty());
  auto lines = s.Feed("\n");
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], "hello");
}

TEST(LineSplitterTest, ByteByByteFeedingReassemblesCorrectly) {
  LineSplitter s;
  const std::string_view input = "alpha\nbeta\n";
  std::vector<std::string> collected;
  for (char c : input) {
    for (auto& line : s.Feed(std::string_view(&c, 1))) {
      collected.push_back(std::move(line));
    }
  }
  ASSERT_EQ(collected.size(), 2u);
  EXPECT_EQ(collected[0], "alpha");
  EXPECT_EQ(collected[1], "beta");
}

TEST(LineSplitterTest, BinaryDataPassesThroughUnmodified) {
  LineSplitter s;
  auto lines = s.Feed(std::string_view("a\0b\nc", 5));
  ASSERT_EQ(lines.size(), 1u);
  EXPECT_EQ(lines[0], std::string("a\0b", 3));
  EXPECT_EQ(s.DrainRemaining(), "c");
}

}  // namespace
}  // namespace nexus
