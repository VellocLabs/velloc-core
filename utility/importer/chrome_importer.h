/* Copyright (c) 2019 The Brave Authors. All rights reserved.
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at https://mozilla.org/MPL/2.0/. */

#ifndef CHROME_UTILITY_IMPORTER_CHROME_IMPORTER_H_
#define CHROME_UTILITY_IMPORTER_CHROME_IMPORTER_H_

#include <stdint.h>

#include <map>
#include <set>
#include <string>
#include <vector>

#include "base/files/file_path.h"
#include "base/values.h"
#include "chrome/utility/importer/importer.h"
#include "components/favicon_base/favicon_usage_data.h"
#include "url/gurl.h"

namespace user_data_importer {
struct ImportedBookmarkEntry;
}  // namespace user_data_importer

namespace sql {
class Database;
}

class ChromeImporter : public Importer {
 public:
  ChromeImporter();
  ChromeImporter(const ChromeImporter&) = delete;
  ChromeImporter& operator=(const ChromeImporter&) = delete;

  // Importer:
  void StartImport(const user_data_importer::SourceProfile& source_profile,
                   uint16_t items,
                   ImporterBridge* bridge) override;

 protected:
  ~ChromeImporter() override;

  void ImportBookmarks();
  void ImportHistory();

  base::FilePath source_path_;

 private:
  // Multiple URLs can share the same favicon; this is a map
  // of URLs -> IconIDs that we load as a temporary step before
  // actually loading the icons.
  using FaviconMap = std::map<int64_t, std::set<GURL>>;

  // Loads the urls associated with the favicons into favicon_map;
  void ImportFaviconURLs(sql::Database* db, FaviconMap* favicon_map);

  // Loads and reencodes the individual favicons.
  void LoadFaviconData(sql::Database* db,
                       const FaviconMap& favicon_map,
                       favicon_base::FaviconUsageDataList* favicons);

  void RecursiveReadBookmarksFolder(
      const base::DictValue* folder,
      const std::vector<std::u16string>& parent_path,
      bool is_in_toolbar,
      std::vector<user_data_importer::ImportedBookmarkEntry>* bookmarks);
};

#endif  // CHROME_UTILITY_IMPORTER_CHROME_IMPORTER_H_
