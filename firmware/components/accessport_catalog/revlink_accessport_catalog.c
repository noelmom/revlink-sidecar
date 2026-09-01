#include "revlink_accessport_catalog.h"

#include <string.h>

typedef struct {
    const char *code;
    const char *name;
    revlink_accessport_family_t family;
} family_description_t;

typedef struct {
    const char *part_number;
    revlink_accessport_family_t family;
} part_record_t;

static const family_description_t FAMILIES[] = {
    {"APA", "AccessPort Activation", REVLINK_ACCESSPORT_FAMILY_ACTIVATION},
    {"AU", "Australian Subaru", REVLINK_ACCESSPORT_FAMILY_AU_SUBARU},
    {"BMW", "BMW", REVLINK_ACCESSPORT_FAMILY_BMW},
    {"FOR", "Ford", REVLINK_ACCESSPORT_FAMILY_FORD},
    {
        "FRP",
        "Ford Performance",
        REVLINK_ACCESSPORT_FAMILY_FORD_PERFORMANCE,
    },
    {"HON", "Honda", REVLINK_ACCESSPORT_FAMILY_HONDA},
    {"MAZ", "Mazda", REVLINK_ACCESSPORT_FAMILY_MAZDA},
    {"MIT", "Mitsubishi", REVLINK_ACCESSPORT_FAMILY_MITSUBISHI},
    {"NIS", "Nissan", REVLINK_ACCESSPORT_FAMILY_NISSAN},
    {"POR", "Porsche", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"SUB", "Subaru", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {
        "VLK",
        "Volkswagen / Audi",
        REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI,
    },
};

/*
 * Published AccessPort part numbers mapped to the vehicle family each is sold
 * for. Assembled by hand from vendor-published product identifiers and
 * reviewed entry by entry; see docs/ACCESSPORT_CATALOG.md.
 *
 * Contains no vendor software, firmware, calibration, or map content.
 *
 * The table is sorted by part number so lookup stays bounded and
 * allocation-free on the product firmware.
 */
static const part_record_t PARTS[] = {
    {"AP3-APA-001", REVLINK_ACCESSPORT_FAMILY_ACTIVATION},
    {"AP3-AU-SUB-003", REVLINK_ACCESSPORT_FAMILY_AU_SUBARU},
    {"AP3-AU-SUB-004", REVLINK_ACCESSPORT_FAMILY_AU_SUBARU},
    {"AP3-AU-SUB-006", REVLINK_ACCESSPORT_FAMILY_AU_SUBARU},
    {"AP3-BMW-001", REVLINK_ACCESSPORT_FAMILY_BMW},
    {"AP3-BMW-002", REVLINK_ACCESSPORT_FAMILY_BMW},
    {"AP3-FOR-001", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-002", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-003", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-004", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-005", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-006", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-007", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-008", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-009", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-010", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-011", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-012", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-013", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FOR-014", REVLINK_ACCESSPORT_FAMILY_FORD},
    {"AP3-FRP-001", REVLINK_ACCESSPORT_FAMILY_FORD_PERFORMANCE},
    {"AP3-HON-001", REVLINK_ACCESSPORT_FAMILY_HONDA},
    {"AP3-HON-002", REVLINK_ACCESSPORT_FAMILY_HONDA},
    {"AP3-HON-003", REVLINK_ACCESSPORT_FAMILY_HONDA},
    {"AP3-HON-004", REVLINK_ACCESSPORT_FAMILY_HONDA},
    {"AP3-MAZ-002", REVLINK_ACCESSPORT_FAMILY_MAZDA},
    {"AP3-MIT-002", REVLINK_ACCESSPORT_FAMILY_MITSUBISHI},
    {"AP3-NIS-005", REVLINK_ACCESSPORT_FAMILY_NISSAN},
    {"AP3-NIS-006", REVLINK_ACCESSPORT_FAMILY_NISSAN},
    {"AP3-NIS-007", REVLINK_ACCESSPORT_FAMILY_NISSAN},
    {"AP3-NIS-008", REVLINK_ACCESSPORT_FAMILY_NISSAN},
    {"AP3-POR-001", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-002", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-003", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-004", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-005", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-006", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-007", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-008", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-009", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-010", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-011", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-012", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-013", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-014", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-015", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-016", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-018", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-019", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-020", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-POR-021", REVLINK_ACCESSPORT_FAMILY_PORSCHE},
    {"AP3-SUB-001", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-002", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-003", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-004", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-005", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-006", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-SUB-007", REVLINK_ACCESSPORT_FAMILY_SUBARU},
    {"AP3-VLK-001", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
    {"AP3-VLK-002", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
    {"AP3-VLK-003", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
    {"AP3-VLK-004", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
    {"AP3-VLK-005", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
    {"AP3-VLK-006", REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI},
};

_Static_assert(
    sizeof(PARTS) / sizeof(PARTS[0]) == REVLINK_ACCESSPORT_CATALOG_PART_COUNT,
    "AccessPort catalog count is stale"
);

static const family_description_t *family_description(
    revlink_accessport_family_t family
)
{
    for (size_t index = 0U;
         index < sizeof(FAMILIES) / sizeof(FAMILIES[0]);
         ++index) {
        if (FAMILIES[index].family == family) {
            return &FAMILIES[index];
        }
    }
    return NULL;
}

static bool copy_entry(
    const part_record_t *record,
    revlink_accessport_catalog_entry_t *entry
)
{
    const family_description_t *description =
        family_description(record->family);
    if (description == NULL) {
        return false;
    }
    if (entry != NULL) {
        *entry = (revlink_accessport_catalog_entry_t){
            .part_number = record->part_number,
            .family_code = description->code,
            .family_name = description->name,
            .family = record->family,
            .read_only_file_sync_supported = true,
        };
    }
    return true;
}

bool revlink_accessport_catalog_lookup(
    const char *part_number,
    revlink_accessport_catalog_entry_t *entry
)
{
    if (part_number == NULL || part_number[0] == '\0') {
        return false;
    }
    size_t lower = 0U;
    size_t upper = sizeof(PARTS) / sizeof(PARTS[0]);
    while (lower < upper) {
        const size_t middle = lower + (upper - lower) / 2U;
        const int comparison = strcmp(part_number, PARTS[middle].part_number);
        if (comparison == 0) {
            return copy_entry(&PARTS[middle], entry);
        }
        if (comparison < 0) {
            upper = middle;
        } else {
            lower = middle + 1U;
        }
    }
    return false;
}

size_t revlink_accessport_catalog_count(void)
{
    return sizeof(PARTS) / sizeof(PARTS[0]);
}

bool revlink_accessport_catalog_entry_at(
    size_t index,
    revlink_accessport_catalog_entry_t *entry
)
{
    if (index >= revlink_accessport_catalog_count() || entry == NULL) {
        return false;
    }
    return copy_entry(&PARTS[index], entry);
}
