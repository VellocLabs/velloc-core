#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_FILE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_FILE_H_

#include <stdint.h>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/search/transcript_span_index.h"

namespace nexus {

// On-disk `.sdi` sidecar for a transcript's span index: derived data, laid out
// for load speed. Updated only by a search, which validates the stamps below
// and rescans or rebuilds on mismatch, so a stale sidecar never gives a wrong
// result.

/** `<dir>/transcript.jsonl` → `<dir>/transcript.sdi`. */
base::FilePath SpanIndexFilePathFor(const base::FilePath& transcript);

// Writes `index` plus the stamps of the transcript it describes. Returns
// false if the file could not be written; the caller carries on with the
// in-memory index, since the sidecar is only a cache.
bool WriteSpanIndexFile(const base::FilePath& sidecar,
                        const TranscriptSpanIndex& index,
                        int64_t source_size,
                        base::Time source_mtime);

// Loads a sidecar into `*index` and reports the transcript stamps it was
// built from, so the caller can decide whether it is still current.
// Returns false — leaving `*index` untouched — on a missing file, a
// foreign/older format, or a torn write.
bool ReadSpanIndexFile(const base::FilePath& sidecar,
                       TranscriptSpanIndex* index,
                       int64_t* source_size,
                       base::Time* source_mtime);

}  // namespace nexus

#endif  // CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_FILE_H_
