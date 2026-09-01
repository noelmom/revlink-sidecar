#ifndef REVLINK_BACKUP_H
#define REVLINK_BACKUP_H

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#define REVLINK_BACKUP_TOKEN_HEX_CAPACITY 65U

typedef esp_err_t (*revlink_backup_write_t)(
    void *context,
    const uint8_t *data,
    size_t length
);

typedef struct {
    uint32_t file_count;
    uint32_t device_count;
    uint64_t data_bytes;
    uint64_t archive_bytes;
    uint64_t created_utc;
    char token[REVLINK_BACKUP_TOKEN_HEX_CAPACITY];
} revlink_backup_summary_t;

typedef struct {
    uint32_t restored_files;
    uint32_t identical_files;
    uint32_t conflicting_files;
    uint64_t restored_bytes;
} revlink_backup_restore_result_t;

/*
 * Backups contain only logical user datasets below revlink/devices. Sidecar
 * identity, Wi-Fi credentials, acceptance state, firmware and transient
 * recovery files are intentionally excluded.
 */
esp_err_t revlink_backup_export(
    revlink_backup_write_t write,
    void *context,
    revlink_backup_summary_t *summary
);

esp_err_t revlink_backup_stage_begin(uint64_t archive_bytes);
esp_err_t revlink_backup_stage_write(const uint8_t *data, size_t length);
esp_err_t revlink_backup_stage_commit(revlink_backup_summary_t *summary);
void revlink_backup_stage_abort(void);

/*
 * Merge restore never overwrites an existing path. Identical files are
 * skipped and differing files are reported as conflicts for manual review.
 */
esp_err_t revlink_backup_restore_merge(
    const char *token,
    revlink_backup_restore_result_t *result
);

#endif
