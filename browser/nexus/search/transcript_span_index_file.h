#ifndef CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_FILE_H_
#define CUSTOM_BROWSER_BROWSER_NEXUS_SEARCH_TRANSCRIPT_SPAN_INDEX_FILE_H_

#include <stdint.h>

#include "base/files/file_path.h"
#include "base/time/time.h"
#include "custom_browser/browser/nexus/search/transcript_span_index.h"

namespace nexus {

// On-disk persistence for a transcript's span index — the `.sdi` sidecar.
//
// The index is derived data: it can always be rebuilt by re-scanning the
// transcript. Persisting it only removes that first scan after a restart,
// so the format is chosen for load speed over compactness — fixed-width
// records, no compression, read straight into vectors.
//
// The sidecar is NOT maintained as the transcript grows. Nothing hooks
// the writer; a search is the only thing that ever touches it. Each
// search validates the stamps below against the transcript and, when
// they disagree, brings the index up to date (a tail rescan for an
// append, a full rebuild otherwise) and rewrites the file. A stale or
// corrupt sidecar therefore costs a rebuild, never a wrong result.

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
