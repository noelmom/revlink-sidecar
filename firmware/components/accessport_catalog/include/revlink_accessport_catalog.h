#ifndef REVLINK_ACCESSPORT_CATALOG_H
#define REVLINK_ACCESSPORT_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_ACCESSPORT_CATALOG_REVISION "projectportaccess-2026-07-28"
#define REVLINK_ACCESSPORT_CATALOG_PART_COUNT 64U

typedef enum {
    REVLINK_ACCESSPORT_FAMILY_ACTIVATION = 0,
    REVLINK_ACCESSPORT_FAMILY_AU_SUBARU,
    REVLINK_ACCESSPORT_FAMILY_BMW,
    REVLINK_ACCESSPORT_FAMILY_FORD,
    REVLINK_ACCESSPORT_FAMILY_FORD_PERFORMANCE,
    REVLINK_ACCESSPORT_FAMILY_HONDA,
    REVLINK_ACCESSPORT_FAMILY_MAZDA,
    REVLINK_ACCESSPORT_FAMILY_MITSUBISHI,
    REVLINK_ACCESSPORT_FAMILY_NISSAN,
    REVLINK_ACCESSPORT_FAMILY_PORSCHE,
    REVLINK_ACCESSPORT_FAMILY_SUBARU,
    REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI,
} revlink_accessport_family_t;

typedef struct {
    const char *part_number;
    const char *family_code;
    const char *family_name;
    revlink_accessport_family_t family;
    bool read_only_file_sync_supported;
} revlink_accessport_catalog_entry_t;

/*
 * Looks up an exact, authoritative AccessPort part number. Matching is
 * deliberately case-sensitive and does not accept prefixes, suffixes, or
 * family-only guesses.
 */
bool revlink_accessport_catalog_lookup(
    const char *part_number,
    revlink_accessport_catalog_entry_t *entry
);

size_t revlink_accessport_catalog_count(void);

bool revlink_accessport_catalog_entry_at(
    size_t index,
    revlink_accessport_catalog_entry_t *entry
);

#ifdef __cplusplus
}
#endif

#endif
