#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_sync_history.h"
#include "revlink_sync_manifest.h"
#include "revlink_sync_annotations.h"

static void require_true(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "manifest host test failed: %s\n", message);
        exit(1);
    }
}

static void fill_digest(uint8_t digest[REVLINK_SYNC_SHA256_BYTES], uint8_t seed)
{
    for (size_t index = 0U; index < REVLINK_SYNC_SHA256_BYTES; ++index) {
        digest[index] = (uint8_t)(seed + index);
    }
}

int main(void)
{
    revlink_sync_manifest_t manifest;
    revlink_sync_manifest_init(&manifest);
    uint8_t first_digest[REVLINK_SYNC_SHA256_BYTES];
    fill_digest(first_digest, 0x10U);

    static const uint8_t path[] = "datalog/datalog35.csv";
    require_true(
        revlink_sync_manifest_upsert_at(
            &manifest,
            path,
            sizeof(path) - 1U,
            123456U,
            46568U,
            1785168000U,
            first_digest,
            "datalog35.csv"
        ) == REVLINK_SYNC_OK,
        "initial upsert"
    );
    const revlink_sync_manifest_entry_t *entry =
        revlink_sync_manifest_find(&manifest, path, sizeof(path) - 1U);
    require_true(
        entry != NULL && entry->initial_sync_utc == 1785168000U,
        "find initial entry with trusted Initial sync"
    );
    require_true(
        revlink_sync_manifest_metadata_matches(entry, 123456U, 46568U),
        "exact metadata should be current"
    );
    require_true(
        !revlink_sync_manifest_metadata_matches(entry, 123457U, 46568U),
        "changed device time must not be skipped"
    );
    require_true(
        !revlink_sync_manifest_metadata_matches(entry, 123456U, 46569U),
        "changed size must not be skipped"
    );
    require_true(
        !revlink_sync_manifest_metadata_matches(entry, 0U, 46568U),
        "zero device time must conservatively re-read"
    );

    char serialized[32768];
    size_t serialized_length = 0U;
    require_true(
        revlink_sync_manifest_serialize(
            &manifest,
            serialized,
            sizeof(serialized),
            &serialized_length
        ) == REVLINK_SYNC_OK,
        "serialize"
    );
    revlink_sync_manifest_t parsed;
    require_true(
        revlink_sync_manifest_parse(
            serialized,
            serialized_length,
            &parsed
        ) == REVLINK_SYNC_OK,
        "round-trip parse"
    );
    entry = revlink_sync_manifest_find(&parsed, path, sizeof(path) - 1U);
    require_true(
        entry != NULL
            && memcmp(
                entry->sha256,
                first_digest,
                REVLINK_SYNC_SHA256_BYTES
            ) == 0
            && entry->initial_sync_utc == 1785168000U,
        "round-trip digest and Initial sync"
    );
    require_true(
        revlink_sync_manifest_upsert_at(
            &parsed,
            path,
            sizeof(path) - 1U,
            123456U,
            46568U,
            1785169999U,
            first_digest,
            "datalog35.csv"
        ) == REVLINK_SYNC_OK
            && revlink_sync_manifest_find(
                &parsed,
                path,
                sizeof(path) - 1U
            )->initial_sync_utc == 1785168000U,
        "re-observation preserves original Initial sync"
    );

    uint8_t replacement_digest[REVLINK_SYNC_SHA256_BYTES];
    fill_digest(replacement_digest, 0x80U);
    require_true(
        revlink_sync_manifest_upsert_at(
            &parsed,
            path,
            sizeof(path) - 1U,
            123457U,
            46568U,
            1785169000U,
            replacement_digest,
            "datalog35.csv.sha256-80818283"
        ) == REVLINK_SYNC_OK,
        "conflict replacement"
    );
    require_true(parsed.count == 1U, "upsert must replace by device path");
    entry = revlink_sync_manifest_find(&parsed, path, sizeof(path) - 1U);
    require_true(
        entry != NULL && entry->device_time_raw == 123457U
            && entry->initial_sync_utc == 1785169000U
            && strcmp(
                entry->cache_name,
                "datalog35.csv.sha256-80818283"
            ) == 0,
        "replacement metadata and cache name"
    );

    require_true(
        revlink_sync_manifest_parse(
            serialized,
            serialized_length - 1U,
            &parsed
        ) == REVLINK_SYNC_INVALID_FORMAT,
        "truncated snapshot must be rejected"
    );
    static const char legacy_manifest[] =
        "REVLINK-MANIFEST\t1\n"
        "datalog/datalog35.csv\t123456\t46568\t"
        "101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f\t"
        "datalog35.csv\n";
    require_true(
        revlink_sync_manifest_parse(
            legacy_manifest,
            sizeof(legacy_manifest) - 1U,
            &parsed
        ) == REVLINK_SYNC_OK
            && parsed.entries[0].initial_sync_utc == 0U,
        "v1 manifest migrates without inventing Initial sync"
    );

    revlink_sync_manifest_init(&manifest);
    for (size_t index = 0U; index < REVLINK_SYNC_MANIFEST_CAPACITY; ++index) {
        char generated_path[64];
        char cache_name[48];
        const int path_length = snprintf(
            generated_path,
            sizeof(generated_path),
            "datalog/datalog%03u.csv",
            (unsigned int)index
        );
        snprintf(
            cache_name,
            sizeof(cache_name),
            "datalog%03u.csv",
            (unsigned int)index
        );
        require_true(
            path_length > 0
                && revlink_sync_manifest_upsert(
                    &manifest,
                    (const uint8_t *)generated_path,
                    (size_t)path_length,
                    (uint32_t)index + 1U,
                    (uint32_t)index + 100U,
                    first_digest,
                    cache_name
                ) == REVLINK_SYNC_OK,
            "fill manifest capacity"
        );
    }
    static const uint8_t overflow_path[] = "datalog/overflow.csv";
    require_true(
        revlink_sync_manifest_upsert(
            &manifest,
            overflow_path,
            sizeof(overflow_path) - 1U,
            1U,
            1U,
            first_digest,
            "overflow.csv"
        ) == REVLINK_SYNC_CAPACITY_EXCEEDED,
        "manifest capacity must be bounded"
    );

    size_t required_length = 0U;
    require_true(
        revlink_sync_manifest_serialize(
            &manifest,
            serialized,
            16U,
            &required_length
        ) == REVLINK_SYNC_BUFFER_TOO_SMALL
            && required_length > 16U,
        "serialization capacity must be bounded"
    );

    revlink_sync_history_t *history = calloc(1U, sizeof(*history));
    revlink_sync_history_t *parsed_history =
        calloc(1U, sizeof(*parsed_history));
    require_true(
        history != NULL && parsed_history != NULL,
        "history allocation"
    );
    revlink_sync_history_init(history);
    uint32_t first_sequence = 0U;
    uint32_t repeated_sequence = 0U;
    uint32_t rotated_sequence = 0U;
    require_true(
        revlink_sync_history_record_at(
            history,
            path,
            sizeof(path) - 1U,
            123456U,
            46568U,
            1785168000U,
            first_digest,
            "10111213.csv",
            &first_sequence
        ) == REVLINK_SYNC_OK
            && first_sequence == 1U,
        "record first immutable version"
    );
    require_true(
        revlink_sync_history_record_at(
            history,
            path,
            sizeof(path) - 1U,
            123456U,
            46568U,
            1785169999U,
            first_digest,
            "10111213.csv",
            &repeated_sequence
        ) == REVLINK_SYNC_OK
            && repeated_sequence == first_sequence
            && history->count == 1U
            && history->entries[0].initial_sync_utc == 1785168000U,
        "same path and digest must be idempotent"
    );
    require_true(
        revlink_sync_history_record_at(
            history,
            path,
            sizeof(path) - 1U,
            123457U,
            46568U,
            1785169000U,
            replacement_digest,
            "80818283.csv",
            &rotated_sequence
        ) == REVLINK_SYNC_OK
            && rotated_sequence == 2U
            && history->count == 2U,
        "rotated filename must preserve a second version"
    );
    static const uint8_t renamed_path[] = "datalog/renamed.csv";
    uint32_t renamed_sequence = 0U;
    require_true(
        revlink_sync_history_record(
            history,
            renamed_path,
            sizeof(renamed_path) - 1U,
            123457U,
            46568U,
            replacement_digest,
            "80818283.csv",
            &renamed_sequence
        ) == REVLINK_SYNC_OK
            && renamed_sequence == 3U
            && history->count == 3U,
        "same object under a new path keeps a separate reference"
    );

    char *history_text = malloc(300000U);
    require_true(history_text != NULL, "history serialization allocation");
    size_t history_length = 0U;
    require_true(
        revlink_sync_history_serialize(
            history,
            history_text,
            300000U,
            &history_length
        ) == REVLINK_SYNC_OK,
        "serialize history"
    );
    require_true(
        revlink_sync_history_parse(
            history_text,
            history_length,
            parsed_history
        ) == REVLINK_SYNC_OK
            && parsed_history->count == 3U
            && parsed_history->next_sequence == 4U,
        "parse history"
    );
    const revlink_sync_history_entry_t *rotated =
        revlink_sync_history_find_version(
            parsed_history,
            path,
            sizeof(path) - 1U,
            replacement_digest
        );
    require_true(
        rotated != NULL && rotated->sequence == 2U
            && rotated->initial_sync_utc == 1785169000U
            && strcmp(rotated->object_name, "80818283.csv") == 0,
        "rotation history survives serialization"
    );

    revlink_sync_history_t *other_device =
        calloc(1U, sizeof(*other_device));
    require_true(other_device != NULL, "second device history allocation");
    revlink_sync_history_init(other_device);
    uint32_t other_sequence = 0U;
    require_true(
        revlink_sync_history_record(
            other_device,
            path,
            sizeof(path) - 1U,
            999999U,
            46568U,
            replacement_digest,
            "80818283.csv",
            &other_sequence
        ) == REVLINK_SYNC_OK
            && other_sequence == 1U
            && other_device->count == 1U
            && history->count == 3U,
        "a second device owns an independent version sequence"
    );
    require_true(
        revlink_sync_history_find_version(
            other_device,
            path,
            sizeof(path) - 1U,
            first_digest
        ) == NULL
            && revlink_sync_history_find_version(
                   history,
                   path,
                   sizeof(path) - 1U,
                   replacement_digest
               )->sequence == 2U,
        "device histories never leak entries across namespaces"
    );
    free(other_device);

    require_true(
        revlink_sync_history_parse(
            history_text,
            history_length - 1U,
            parsed_history
        ) == REVLINK_SYNC_INVALID_FORMAT,
        "truncated history must be rejected"
    );
    static const char legacy_history[] =
        "REVLINK-HISTORY\t1\t2\n"
        "1\tdatalog/datalog35.csv\t123456\t46568\t"
        "101112131415161718191a1b1c1d1e1f"
        "202122232425262728292a2b2c2d2e2f\t10111213.csv\n";
    require_true(
        revlink_sync_history_parse(
            legacy_history,
            sizeof(legacy_history) - 1U,
            parsed_history
        ) == REVLINK_SYNC_OK
            && parsed_history->entries[0].initial_sync_utc == 0U,
        "v1 history migrates without inventing Initial sync"
    );
    static const uint8_t spaced_map_path[] =
        "maps/Stage 0 (roundtrip) v400.ptm";
    revlink_sync_manifest_t spaced_manifest;
    revlink_sync_manifest_init(&spaced_manifest);
    require_true(
        revlink_sync_manifest_upsert_at(
            &spaced_manifest,
            spaced_map_path,
            sizeof(spaced_map_path) - 1U,
            1711060350U,
            41469U,
            1785330000U,
            first_digest,
            "10111213.ptm"
        ) == REVLINK_SYNC_OK,
        "map paths may contain safe spaces and parentheses"
    );
    uint32_t spaced_sequence = 0U;
    require_true(
        revlink_sync_history_record_at(
            history,
            spaced_map_path,
            sizeof(spaced_map_path) - 1U,
            1711060350U,
            41469U,
            1785330000U,
            first_digest,
            "10111213.ptm",
            &spaced_sequence
        ) == REVLINK_SYNC_OK,
        "map history accepts safe spaces and parentheses"
    );
    free(history_text);
    free(parsed_history);
    free(history);

    revlink_sync_annotations_t *annotations = calloc(1U, sizeof(*annotations));
    revlink_sync_annotations_t *parsed_annotations =
        calloc(1U, sizeof(*parsed_annotations));
    require_true(
        annotations != NULL && parsed_annotations != NULL,
        "annotation allocation"
    );
    revlink_sync_annotations_init(annotations);
    static const char note[] =
        "Cold start, 8:15 AM\nSend this log to the tuner.";
    require_true(
        revlink_sync_annotations_set(
            annotations,
            first_digest,
            note,
            sizeof(note) - 1U,
            1785168900U
        ) == REVLINK_SYNC_ANNOTATION_OK,
        "save version-scoped note"
    );
    require_true(
        revlink_sync_annotations_find(annotations, replacement_digest) == NULL,
        "different bytes must not inherit a note"
    );
    require_true(
        revlink_sync_annotations_set_map(
            annotations,
            first_digest,
            replacement_digest,
            1785168901U
        ) == REVLINK_SYNC_ANNOTATION_OK,
        "tag datalog with exact cached map version"
    );
    char *annotation_text = malloc(300000U);
    size_t annotation_length = 0U;
    require_true(
        annotation_text != NULL
            && revlink_sync_annotations_serialize(
                annotations,
                annotation_text,
                300000U,
                &annotation_length
            ) == REVLINK_SYNC_ANNOTATION_OK
            && revlink_sync_annotations_parse(
                annotation_text,
                annotation_length,
                parsed_annotations
            ) == REVLINK_SYNC_ANNOTATION_OK,
        "annotation round trip"
    );
    const revlink_sync_annotation_t *saved =
        revlink_sync_annotations_find(parsed_annotations, first_digest);
    require_true(
        saved != NULL
            && saved->updated_at_utc == 1785168901U
            && strcmp(saved->note, note) == 0
            && saved->has_map_sha256
            && memcmp(
                   saved->map_sha256,
                   replacement_digest,
                   REVLINK_SYNC_ANNOTATION_SHA256_BYTES
               ) == 0,
        "note, map tag, and trusted update time survive serialization"
    );
    require_true(
        revlink_sync_annotations_set(
            parsed_annotations,
            first_digest,
            "",
            0U,
            0U
        ) == REVLINK_SYNC_ANNOTATION_OK
            && parsed_annotations->count == 1U
            && parsed_annotations->entries[0].has_map_sha256,
        "empty note preserves a saved map tag"
    );
    require_true(
        revlink_sync_annotations_serialize(
            parsed_annotations,
            annotation_text,
            300000U,
            &annotation_length
        ) == REVLINK_SYNC_ANNOTATION_OK
            && revlink_sync_annotations_parse(
                annotation_text,
                annotation_length,
                annotations
            ) == REVLINK_SYNC_ANNOTATION_OK
            && annotations->count == 1U
            && annotations->entries[0].note[0] == '\0'
            && annotations->entries[0].has_map_sha256,
        "map-only annotation round trip"
    );
    require_true(
        revlink_sync_annotations_set_map(
            annotations,
            first_digest,
            NULL,
            0U
        ) == REVLINK_SYNC_ANNOTATION_OK
            && annotations->count == 0U,
        "removing final map tag removes empty annotation"
    );
    static const char legacy_annotation[] =
        "REVLINK-ANNOTATIONS\t1\n"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "\t123\t1\t41\n";
    require_true(
        revlink_sync_annotations_parse(
            legacy_annotation,
            sizeof(legacy_annotation) - 1U,
            parsed_annotations
        ) == REVLINK_SYNC_ANNOTATION_OK
            && parsed_annotations->count == 1U
            && strcmp(parsed_annotations->entries[0].note, "A") == 0
            && !parsed_annotations->entries[0].has_map_sha256,
        "v1 note annotations remain readable"
    );
    free(annotation_text);
    free(parsed_annotations);
    free(annotations);

    /* ---- device presence ------------------------------------------- */
    /*
     * The portal has to be able to say "the Sidecar still has this, the
     * AccessPort does not" without ever saying it on a guess. These cover the
     * three states and, more importantly, the transitions that must NOT
     * happen: an old manifest must not read as a claim, and a download must
     * not erase what a listing established.
     */
    revlink_sync_manifest_t *presence = calloc(1U, sizeof(*presence));
    require_true(presence != NULL, "presence manifest allocation");
    revlink_sync_manifest_init(presence);
    uint8_t presence_digest[REVLINK_SYNC_SHA256_BYTES];
    fill_digest(presence_digest, 0x40U);
    require_true(
        revlink_sync_manifest_upsert(
            presence,
            (const uint8_t *)"maps/Stage1.ptm",
            15U,
            4242U,
            2048U,
            presence_digest,
            "stage1.ptm"
        ) == REVLINK_SYNC_OK
            && presence->entries[0].presence
                == REVLINK_SYNC_PRESENCE_UNKNOWN,
        "a new entry claims nothing about the device"
    );
    require_true(
        revlink_sync_manifest_set_presence(
            presence,
            (const uint8_t *)"maps/Stage1.ptm",
            15U,
            REVLINK_SYNC_PRESENCE_ON_DEVICE
        )
            && presence->entries[0].presence
                == REVLINK_SYNC_PRESENCE_ON_DEVICE,
        "a listing can record that the device has the file"
    );
    require_true(
        !revlink_sync_manifest_set_presence(
            presence,
            (const uint8_t *)"maps/NeverCached.ptm",
            20U,
            REVLINK_SYNC_PRESENCE_ON_DEVICE
        ),
        "a device file that was never cached is not invented"
    );
    /*
     * Re-reading the file must not reset what the listing recorded. This is
     * the ordering that bit in practice: the listing runs first, the download
     * upserts afterwards, and a memset in the upsert would have quietly
     * cleared every presence flag on every sync.
     */
    require_true(
        revlink_sync_manifest_upsert(
            presence,
            (const uint8_t *)"maps/Stage1.ptm",
            15U,
            4242U,
            2048U,
            presence_digest,
            "stage1.ptm"
        ) == REVLINK_SYNC_OK
            && presence->entries[0].presence
                == REVLINK_SYNC_PRESENCE_ON_DEVICE,
        "re-syncing a file does not discard its presence"
    );
    require_true(
        revlink_sync_manifest_set_presence(
            presence,
            (const uint8_t *)"maps/Stage1.ptm",
            15U,
            REVLINK_SYNC_PRESENCE_ABSENT
        )
            && presence->entries[0].presence == REVLINK_SYNC_PRESENCE_ABSENT,
        "a delete can record that the device no longer has the file"
    );

    char *presence_text = malloc(65536U);
    require_true(presence_text != NULL, "presence buffer allocation");
    size_t presence_length = 0U;
    require_true(
        revlink_sync_manifest_serialize(
            presence,
            presence_text,
            65536U,
            &presence_length
        ) == REVLINK_SYNC_OK,
        "presence manifest serializes"
    );
    revlink_sync_manifest_t *reloaded = calloc(1U, sizeof(*reloaded));
    require_true(reloaded != NULL, "reload allocation");
    require_true(
        revlink_sync_manifest_parse(
            presence_text,
            presence_length,
            reloaded
        ) == REVLINK_SYNC_OK
            && reloaded->count == 1U
            && reloaded->entries[0].presence == REVLINK_SYNC_PRESENCE_ABSENT,
        "presence survives a save and reload"
    );

    /*
     * A manifest written before this column existed must load as UNKNOWN.
     * Reading it as "on device" would keep offering a delete that fails;
     * reading it as "absent" would tell the owner their files are gone.
     */
    static const char legacy_v2_manifest[] =
        "REVLINK-MANIFEST\t2\n"
        "maps/Old.ptm\t7\t11\t0\t"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "\told.ptm\n";
    require_true(
        revlink_sync_manifest_parse(
            legacy_v2_manifest,
            sizeof(legacy_v2_manifest) - 1U,
            reloaded
        ) == REVLINK_SYNC_OK
            && reloaded->count == 1U
            && reloaded->entries[0].presence
                == REVLINK_SYNC_PRESENCE_UNKNOWN,
        "a v2 manifest loads as no-evidence, not as a claim"
    );
    static const char bad_presence[] =
        "REVLINK-MANIFEST\t3\n"
        "maps/Old.ptm\t7\t11\t0\t"
        "0000000000000000000000000000000000000000000000000000000000000000"
        "\told.ptm\t9\n";
    require_true(
        revlink_sync_manifest_parse(
            bad_presence,
            sizeof(bad_presence) - 1U,
            reloaded
        ) == REVLINK_SYNC_INVALID_FORMAT,
        "an unrecognised presence value is refused, not guessed at"
    );
    free(reloaded);
    free(presence_text);
    free(presence);

    /* ---- removing a cached entry ------------------------------------ */
    /*
     * The cache is content-addressed: two paths holding identical bytes share
     * one object on the card. Removing one of them must not remove an object
     * the other still points at, which is why callers ask how many entries
     * carry a digest before they unlink anything.
     */
    revlink_sync_manifest_t *removal = calloc(1U, sizeof(*removal));
    require_true(removal != NULL, "removal manifest allocation");
    revlink_sync_manifest_init(removal);
    uint8_t shared_digest[REVLINK_SYNC_SHA256_BYTES];
    uint8_t lone_digest[REVLINK_SYNC_SHA256_BYTES];
    fill_digest(shared_digest, 0x70U);
    fill_digest(lone_digest, 0x90U);
    require_true(
        revlink_sync_manifest_upsert(
            removal, (const uint8_t *)"maps/A.ptm", 10U,
            1U, 64U, shared_digest, "a.ptm"
        ) == REVLINK_SYNC_OK
        && revlink_sync_manifest_upsert(
            removal, (const uint8_t *)"maps/B.ptm", 10U,
            2U, 64U, shared_digest, "b.ptm"
        ) == REVLINK_SYNC_OK
        && revlink_sync_manifest_upsert(
            removal, (const uint8_t *)"maps/C.ptm", 10U,
            3U, 64U, lone_digest, "c.ptm"
        ) == REVLINK_SYNC_OK
        && removal->count == 3U,
        "three entries, two of them sharing one digest"
    );
    require_true(
        revlink_sync_manifest_digest_users(removal, shared_digest) == 2U
            && revlink_sync_manifest_digest_users(removal, lone_digest) == 1U,
        "digest users are counted"
    );
    require_true(
        revlink_sync_manifest_remove(removal, (const uint8_t *)"maps/A.ptm", 10U)
            && removal->count == 2U
            && revlink_sync_manifest_find(
                   removal, (const uint8_t *)"maps/A.ptm", 10U
               ) == NULL,
        "an entry can be removed"
    );
    require_true(
        revlink_sync_manifest_digest_users(removal, shared_digest) == 1U,
        "the shared object is still referenced after one of its paths goes"
    );
    /*
     * Removal moves the last entry into the gap, so the surviving entries
     * must still be findable by path afterwards — an index-based caller would
     * have silently read the wrong row here.
     */
    require_true(
        revlink_sync_manifest_find(
            removal, (const uint8_t *)"maps/B.ptm", 10U
        ) != NULL
        && revlink_sync_manifest_find(
            removal, (const uint8_t *)"maps/C.ptm", 10U
        ) != NULL,
        "surviving entries remain findable after a removal reorders them"
    );
    require_true(
        !revlink_sync_manifest_remove(
            removal, (const uint8_t *)"maps/Gone.ptm", 13U
        ),
        "removing an uncatalogued path reports that it was not there"
    );
    require_true(
        revlink_sync_manifest_remove(removal, (const uint8_t *)"maps/B.ptm", 10U)
            && revlink_sync_manifest_digest_users(removal, shared_digest) == 0U,
        "the last user of a digest releases it"
    );
    free(removal);

    puts("sync manifest tests PASSED");
    return 0;
}
