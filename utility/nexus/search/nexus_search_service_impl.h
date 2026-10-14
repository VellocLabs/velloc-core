// Copyright 2026 VellocLabs
// Use of this source code is governed by the Apache License, Version 2.0,
// which can be found in the LICENSE file.

#ifndef CUSTOM_BROWSER_UTILITY_NEXUS_SEARCH_NEXUS_SEARCH_SERVICE_IMPL_H_
#define CUSTOM_BROWSER_UTILITY_NEXUS_SEARCH_NEXUS_SEARCH_SERVICE_IMPL_H_

#include <vector>

#include "base/files/file_path.h"
#include "base/memory/scoped_refptr.h"
#include "base/task/sequenced_task_runner.h"
#include "custom_browser/browser/nexus/mojom/nexus_search.mojom.h"
#include "custom_browser/browser/nexus/search/transcript_search_engine.h"
#include "mojo/public/cpp/bindings/pending_receiver.h"
#include "mojo/public/cpp/bindings/receiver.h"

namespace nexus {

// Utility-process `nexus.mojom.NexusSearchService`. Owns the
// `TranscriptSearchEngine`, its cache and every `.sdi` read and write. Searches
// run on a task-runner sequence, not the mojo thread, so a newer query can
// arrive and cancel the running one.
class NexusSearchServiceImpl : public mojom::NexusSearchService {
 public:
  explicit NexusSearchServiceImpl(
      mojo::PendingReceiver<mojom::NexusSearchService> receiver);
  ~NexusSearchServiceImpl() override;

  NexusSearchServiceImpl(const NexusSearchServiceImpl&) = delete;
  NexusSearchServiceImpl& operator=(const NexusSearchServiceImpl&) = delete;

  // mojom::NexusSearchService:
  void QueryTranscripts(const std::vector<base::FilePath>& targets,
                        const std::string& query,
                        bool regex,
                        bool case_sensitive,
                        int32_t max_results,
                        QueryTranscriptsCallback callback) override;

 private:
  mojo::Receiver<mojom::NexusSearchService> receiver_;
  scoped_refptr<TranscriptSearchEngine> engine_;
  // One sequence for every query, so two keystrokes never overlap and the
  // engine's span cache needs no locking. WithBaseSyncPrimitives: the
  // engine fans a search out across a few pool workers and joins them on
  // a barrier, so this sequence blocks briefly by design.
  scoped_refptr<base::SequencedTaskRunner> search_task_runner_;
};

}  // namespace nexus

#endif  // CUSTOM_BROWSER_UTILITY_NEXUS_SEARCH_NEXUS_SEARCH_SERVICE_IMPL_H_
