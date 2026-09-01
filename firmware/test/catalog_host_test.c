#include <stdio.h>
#include <string.h>

#include "revlink_accessport_catalog.h"

static int fail(const char *message)
{
    fprintf(stderr, "catalog_host_test: %s\n", message);
    return 1;
}

int main(void)
{
    if (revlink_accessport_catalog_count()
        != REVLINK_ACCESSPORT_CATALOG_PART_COUNT) {
        return fail("catalog count mismatch");
    }

    const char *previous = NULL;
    size_t family_counts[REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI + 1U] = {0};
    for (size_t index = 0U;
         index < revlink_accessport_catalog_count();
         ++index) {
        revlink_accessport_catalog_entry_t entry = {0};
        if (!revlink_accessport_catalog_entry_at(index, &entry)
            || entry.part_number == NULL
            || entry.family_code == NULL
            || entry.family_name == NULL
            || !entry.read_only_file_sync_supported) {
            return fail("invalid catalog entry");
        }
        if (previous != NULL && strcmp(previous, entry.part_number) >= 0) {
            return fail("catalog is not uniquely sorted");
        }
        previous = entry.part_number;
        ++family_counts[entry.family];

        revlink_accessport_catalog_entry_t lookup = {0};
        if (!revlink_accessport_catalog_lookup(entry.part_number, &lookup)
            || strcmp(lookup.part_number, entry.part_number) != 0
            || lookup.family != entry.family) {
            return fail("catalog entry does not round-trip");
        }
    }

    for (size_t family = 0U;
         family <= REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI;
         ++family) {
        if (family_counts[family] == 0U) {
            return fail("empty supported family");
        }
    }

    revlink_accessport_catalog_entry_t subaru = {0};
    revlink_accessport_catalog_entry_t volkswagen = {0};
    if (!revlink_accessport_catalog_lookup("AP3-SUB-004", &subaru)
        || subaru.family != REVLINK_ACCESSPORT_FAMILY_SUBARU
        || strcmp(subaru.family_name, "Subaru") != 0
        || !revlink_accessport_catalog_lookup("AP3-VLK-002", &volkswagen)
        || volkswagen.family
            != REVLINK_ACCESSPORT_FAMILY_VOLKSWAGEN_AUDI) {
        return fail("live donor classification failed");
    }

    static const char *unknown_values[] = {
        "",
        "AP3-SUB",
        "AP3-SUB-004-EXTRA",
        "ap3-sub-004",
        "AP3-SUB-999",
        "AP3-VLK-007",
    };
    for (size_t index = 0U;
         index < sizeof(unknown_values) / sizeof(unknown_values[0]);
         ++index) {
        if (revlink_accessport_catalog_lookup(
                unknown_values[index],
                NULL
            )) {
            return fail("ambiguous or unknown part was accepted");
        }
    }
    if (revlink_accessport_catalog_entry_at(
            revlink_accessport_catalog_count(),
            &subaru
        )
        || revlink_accessport_catalog_entry_at(0U, NULL)
        || revlink_accessport_catalog_lookup(NULL, NULL)) {
        return fail("invalid arguments were accepted");
    }

    puts("catalog_host_test: 64 parts / 12 families passed");
    return 0;
}
