#include "custom_browser/ui/web_ui/nexus_file_drop_policy.h"

#include "base/files/file_path.h"
#include "content/public/common/drop_data.h"
#include "testing/gtest/include/gtest/gtest.h"
#include "url/gurl.h"

namespace custom_browser {
namespace {

TEST(NexusFileDropPolicyTest, EmptyDragIsRefused) {
  EXPECT_FALSE(IsNexusFileDrop(content::DropData()));
}

TEST(NexusFileDropPolicyTest, OsFileDragIsAdmitted) {
  content::DropData data;
  data.filenames.emplace_back(base::FilePath(FILE_PATH_LITERAL("a.pdf")),
                              base::FilePath(FILE_PATH_LITERAL("a.pdf")));
  EXPECT_TRUE(IsNexusFileDrop(data));
}

TEST(NexusFileDropPolicyTest, FileSystemFileDragIsAdmitted) {
  content::DropData data;
  content::DropData::FileSystemFileInfo info;
  info.url = GURL("filesystem:https://x.test/temporary/a.png");
  info.size = 10;
  data.file_system_files.push_back(info);
  EXPECT_TRUE(IsNexusFileDrop(data));
}

TEST(NexusFileDropPolicyTest, TextHtmlAndUrlDragsAreRefused) {
  content::DropData text;
  text.text = u"hello";
  EXPECT_FALSE(IsNexusFileDrop(text));

  content::DropData html;
  html.html = u"<b>hi</b>";
  EXPECT_FALSE(IsNexusFileDrop(html));

  content::DropData url;
  url.url_infos.emplace_back(GURL("https://example.com"), std::u16string());
  EXPECT_FALSE(IsNexusFileDrop(url));
}

}  // namespace
}  // namespace custom_browser
