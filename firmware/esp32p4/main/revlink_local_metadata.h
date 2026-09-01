#ifndef REVLINK_LOCAL_METADATA_H
#define REVLINK_LOCAL_METADATA_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/*
 * Local metadata repair for legacy or temporarily untimed cache records.
 *
 * This never communicates with the attached AccessPort and never changes
 * cached file contents. It updates only zero-valued Initial sync fields in
 * the local microSD ledgers for the last unambiguous device namespace.
 */
esp_err_t revlink_local_metadata_backfill_initial_sync(
    uint64_t initial_sync_utc,
    size_t *manifest_updated,
    size_t *history_updated
);

/*
 * Owner-directed maintenance for one observed AccessPort filename wrap.
 *
 * Existing current datalog versions are ordered datalog20..datalog58,
 * datalog1, datalog2. Missing names are skipped without creating records.
 * Only each current manifest version and its path+digest-matched history entry
 * receive the generated timestamps.
 */
esp_err_t revlink_local_metadata_resequence_wrapped_datalogs(
    uint64_t first_initial_sync_utc,
    uint32_t increment_seconds,
    size_t *manifest_updated,
    size_t *history_updated
);

#endif
