#include "revlink_sync_annotations.h"

#include <stdbool.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ANNOTATION_HEADER "REVLINK-ANNOTATIONS\t2\n"
#define ANNOTATION_HEADER_LENGTH (sizeof(ANNOTATION_HEADER) - 1U)
#define LEGACY_ANNOTATION_HEADER "REVLINK-ANNOTATIONS\t1\n"
#define LEGACY_ANNOTATION_HEADER_LENGTH \
    (sizeof(LEGACY_ANNOTATION_HEADER) - 1U)

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') return value - '0';
    if (value >= 'a' && value <= 'f') return value - 'a' + 10;
    if (value >= 'A' && value <= 'F') return value - 'A' + 10;
    return -1;
}

static bool parse_hex(
    const char *text,
    size_t text_length,
    uint8_t *output,
    size_t output_length
)
{
    if (text == NULL || output == NULL || text_length != output_length * 2U) {
        return false;
    }
    for (size_t index = 0U; index < output_length; ++index) {
        const int high = hex_value(text[index * 2U]);
        const int low = hex_value(text[index * 2U + 1U]);
        if (high < 0 || low < 0) return false;
        output[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

static bool parse_u64(const char *text, size_t length, uint64_t *output)
{
    if (text == NULL || output == NULL || length == 0U || length > 20U) {
        return false;
    }
    uint64_t value = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (text[index] < '0' || text[index] > '9') return false;
        const uint64_t digit = (uint64_t)(text[index] - '0');
        if (value > (UINT64_MAX - digit) / 10U) return false;
        value = value * 10U + digit;
    }
    *output = value;
    return true;
}

static bool valid_note(const char *note, size_t length)
{
    if (note == NULL || length >= REVLINK_SYNC_NOTE_CAPACITY) return false;
    for (size_t index = 0U; index < length; ++index) {
        if (note[index] == '\0' || note[index] == '\r') return false;
    }
    return true;
}

void revlink_sync_annotations_init(revlink_sync_annotations_t *annotations)
{
    if (annotations != NULL) memset(annotations, 0, sizeof(*annotations));
}

const revlink_sync_annotation_t *revlink_sync_annotations_find(
    const revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES]
)
{
    if (annotations == NULL || sha256 == NULL) return NULL;
    for (size_t index = 0U; index < annotations->count; ++index) {
        if (memcmp(
                annotations->entries[index].sha256,
                sha256,
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES
            ) == 0) {
            return &annotations->entries[index];
        }
    }
    return NULL;
}

revlink_sync_annotation_status_t revlink_sync_annotations_set(
    revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const char *note,
    size_t note_length,
    uint64_t updated_at_utc
)
{
    if (annotations == NULL || sha256 == NULL
        || !valid_note(note, note_length)) {
        return REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT;
    }
    size_t target = annotations->count;
    for (size_t index = 0U; index < annotations->count; ++index) {
        if (memcmp(
                annotations->entries[index].sha256,
                sha256,
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES
            ) == 0) {
            target = index;
            break;
        }
    }
    if (note_length == 0U) {
        if (target < annotations->count) {
            if (annotations->entries[target].has_map_sha256) {
                annotations->entries[target].note[0] = '\0';
                annotations->entries[target].updated_at_utc = updated_at_utc;
                return REVLINK_SYNC_ANNOTATION_OK;
            }
            --annotations->count;
            if (target < annotations->count) {
                memmove(
                    &annotations->entries[target],
                    &annotations->entries[target + 1U],
                    (annotations->count - target)
                        * sizeof(annotations->entries[0])
                );
            }
            memset(
                &annotations->entries[annotations->count],
                0,
                sizeof(annotations->entries[0])
            );
        }
        return REVLINK_SYNC_ANNOTATION_OK;
    }
    if (target == annotations->count) {
        if (annotations->count >= REVLINK_SYNC_ANNOTATION_CAPACITY) {
            return REVLINK_SYNC_ANNOTATION_CAPACITY_EXCEEDED;
        }
        ++annotations->count;
    }
    revlink_sync_annotation_t *entry = &annotations->entries[target];
    const bool preserve_map = entry->has_map_sha256;
    uint8_t preserved_map[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
    if (preserve_map) {
        memcpy(preserved_map, entry->map_sha256, sizeof(preserved_map));
    }
    memset(entry, 0, sizeof(*entry));
    memcpy(
        entry->sha256,
        sha256,
        REVLINK_SYNC_ANNOTATION_SHA256_BYTES
    );
    if (preserve_map) {
        entry->has_map_sha256 = true;
        memcpy(entry->map_sha256, preserved_map, sizeof(entry->map_sha256));
    }
    memcpy(entry->note, note, note_length);
    entry->note[note_length] = '\0';
    entry->updated_at_utc = updated_at_utc;
    return REVLINK_SYNC_ANNOTATION_OK;
}

revlink_sync_annotation_status_t revlink_sync_annotations_set_map(
    revlink_sync_annotations_t *annotations,
    const uint8_t sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    const uint8_t map_sha256[REVLINK_SYNC_ANNOTATION_SHA256_BYTES],
    uint64_t updated_at_utc
)
{
    if (annotations == NULL || sha256 == NULL) {
        return REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT;
    }
    size_t target = annotations->count;
    for (size_t index = 0U; index < annotations->count; ++index) {
        if (memcmp(
                annotations->entries[index].sha256,
                sha256,
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES
            ) == 0) {
            target = index;
            break;
        }
    }
    if (map_sha256 == NULL) {
        if (target < annotations->count) {
            revlink_sync_annotation_t *entry = &annotations->entries[target];
            entry->has_map_sha256 = false;
            memset(entry->map_sha256, 0, sizeof(entry->map_sha256));
            entry->updated_at_utc = updated_at_utc;
            if (entry->note[0] == '\0') {
                --annotations->count;
                if (target < annotations->count) {
                    memmove(
                        &annotations->entries[target],
                        &annotations->entries[target + 1U],
                        (annotations->count - target)
                            * sizeof(annotations->entries[0])
                    );
                }
                memset(
                    &annotations->entries[annotations->count],
                    0,
                    sizeof(annotations->entries[0])
                );
            }
        }
        return REVLINK_SYNC_ANNOTATION_OK;
    }
    if (target == annotations->count) {
        if (annotations->count >= REVLINK_SYNC_ANNOTATION_CAPACITY) {
            return REVLINK_SYNC_ANNOTATION_CAPACITY_EXCEEDED;
        }
        memset(
            &annotations->entries[target],
            0,
            sizeof(annotations->entries[target])
        );
        memcpy(
            annotations->entries[target].sha256,
            sha256,
            REVLINK_SYNC_ANNOTATION_SHA256_BYTES
        );
        ++annotations->count;
    }
    revlink_sync_annotation_t *entry = &annotations->entries[target];
    entry->has_map_sha256 = true;
    memcpy(
        entry->map_sha256,
        map_sha256,
        REVLINK_SYNC_ANNOTATION_SHA256_BYTES
    );
    entry->updated_at_utc = updated_at_utc;
    return REVLINK_SYNC_ANNOTATION_OK;
}

revlink_sync_annotation_status_t revlink_sync_annotations_serialize(
    const revlink_sync_annotations_t *annotations,
    char *output,
    size_t capacity,
    size_t *output_length
)
{
    if (annotations == NULL || output == NULL || output_length == NULL
        || annotations->count > REVLINK_SYNC_ANNOTATION_CAPACITY) {
        return REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT;
    }
    size_t used = 0U;
#define APPEND(value, length) do { \
    const size_t count = (length); \
    if (count > capacity - used) { \
        *output_length = used + count; \
        return REVLINK_SYNC_ANNOTATION_BUFFER_TOO_SMALL; \
    } \
    memcpy(output + used, (value), count); \
    used += count; \
} while (0)
    APPEND(ANNOTATION_HEADER, ANNOTATION_HEADER_LENGTH);
    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < annotations->count; ++index) {
        const revlink_sync_annotation_t *entry = &annotations->entries[index];
        const size_t note_length = strlen(entry->note);
        if (!valid_note(entry->note, note_length)
            || (note_length == 0U && !entry->has_map_sha256)) {
            return REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT;
        }
        char digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U];
        for (
            size_t byte = 0U;
            byte < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
            ++byte
        ) {
            digest[byte * 2U] = hex[entry->sha256[byte] >> 4U];
            digest[byte * 2U + 1U] = hex[entry->sha256[byte] & 0x0fU];
        }
        APPEND(digest, sizeof(digest));
        char prefix[48];
        const int prefix_length = snprintf(
            prefix,
            sizeof(prefix),
            "\t%" PRIu64 "\t%u\t",
            entry->updated_at_utc,
            (unsigned int)note_length
        );
        if (prefix_length <= 0 || (size_t)prefix_length >= sizeof(prefix)) {
            return REVLINK_SYNC_ANNOTATION_INVALID_ARGUMENT;
        }
        APPEND(prefix, (size_t)prefix_length);
        for (size_t byte = 0U; byte < note_length; ++byte) {
            const char encoded[2] = {
                hex[(uint8_t)entry->note[byte] >> 4U],
                hex[(uint8_t)entry->note[byte] & 0x0fU],
            };
            APPEND(encoded, sizeof(encoded));
        }
        APPEND("\t", 1U);
        if (entry->has_map_sha256) {
            char map_digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES * 2U];
            for (
                size_t byte = 0U;
                byte < REVLINK_SYNC_ANNOTATION_SHA256_BYTES;
                ++byte
            ) {
                map_digest[byte * 2U] =
                    hex[entry->map_sha256[byte] >> 4U];
                map_digest[byte * 2U + 1U] =
                    hex[entry->map_sha256[byte] & 0x0fU];
            }
            APPEND(map_digest, sizeof(map_digest));
        } else {
            APPEND("-", 1U);
        }
        APPEND("\n", 1U);
    }
#undef APPEND
    *output_length = used;
    return REVLINK_SYNC_ANNOTATION_OK;
}

revlink_sync_annotation_status_t revlink_sync_annotations_parse(
    const char *input,
    size_t input_length,
    revlink_sync_annotations_t *annotations
)
{
    if (input == NULL || annotations == NULL) {
        return REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
    }
    const bool legacy =
        input_length >= LEGACY_ANNOTATION_HEADER_LENGTH
        && memcmp(
               input,
               LEGACY_ANNOTATION_HEADER,
               LEGACY_ANNOTATION_HEADER_LENGTH
           ) == 0;
    if (!legacy
        && (input_length < ANNOTATION_HEADER_LENGTH
            || memcmp(input, ANNOTATION_HEADER, ANNOTATION_HEADER_LENGTH)
                != 0)) {
        return REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
    }
    revlink_sync_annotations_t *parsed = calloc(1U, sizeof(*parsed));
    if (parsed == NULL) return REVLINK_SYNC_ANNOTATION_ALLOCATION_FAILED;
    revlink_sync_annotation_status_t status =
        REVLINK_SYNC_ANNOTATION_OK;
    size_t offset = legacy
        ? LEGACY_ANNOTATION_HEADER_LENGTH : ANNOTATION_HEADER_LENGTH;
    while (offset < input_length) {
        const char *line = input + offset;
        const char *newline = memchr(line, '\n', input_length - offset);
        if (newline == NULL || newline == line) {
            status = REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
            break;
        }
        const size_t line_length = (size_t)(newline - line);
        const char *tabs[4] = {0};
        size_t tab_count = 0U;
        for (size_t index = 0U; index < line_length; ++index) {
            if (line[index] == '\t' && tab_count < 4U) {
                tabs[tab_count++] = line + index;
            }
        }
        uint8_t digest[REVLINK_SYNC_ANNOTATION_SHA256_BYTES];
        uint64_t updated = 0U;
        uint64_t encoded_length = 0U;
        const char *note_end = legacy ? newline : tabs[3];
        if (tab_count != (legacy ? 3U : 4U)
            || !parse_hex(line, (size_t)(tabs[0] - line), digest, sizeof(digest))
            || !parse_u64(
                tabs[0] + 1,
                (size_t)(tabs[1] - tabs[0] - 1),
                &updated
            )
            || !parse_u64(
                tabs[1] + 1,
                (size_t)(tabs[2] - tabs[1] - 1),
                &encoded_length
            )
            || (legacy && encoded_length == 0U)
            || encoded_length >= REVLINK_SYNC_NOTE_CAPACITY
            || note_end == NULL
            || (size_t)(note_end - tabs[2] - 1) != encoded_length * 2U) {
            status = REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
            break;
        }
        char note[REVLINK_SYNC_NOTE_CAPACITY];
        if (!parse_hex(
                tabs[2] + 1,
                encoded_length * 2U,
                (uint8_t *)note,
                (size_t)encoded_length
            )) {
            status = REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
            break;
        }
        note[encoded_length] = '\0';
        status = revlink_sync_annotations_set(
            parsed,
            digest,
            note,
            (size_t)encoded_length,
            updated
        );
        if (status == REVLINK_SYNC_ANNOTATION_OK && !legacy) {
            const char *map_text = tabs[3] + 1;
            const size_t map_length = (size_t)(newline - map_text);
            uint8_t map_digest[
                REVLINK_SYNC_ANNOTATION_SHA256_BYTES
            ];
            if (map_length == 1U && map_text[0] == '-') {
                /* No linked map. */
            } else if (!parse_hex(
                           map_text,
                           map_length,
                           map_digest,
                           sizeof(map_digest)
                       )) {
                status = REVLINK_SYNC_ANNOTATION_INVALID_FORMAT;
            } else {
                status = revlink_sync_annotations_set_map(
                    parsed,
                    digest,
                    map_digest,
                    updated
                );
            }
        }
        if (status != REVLINK_SYNC_ANNOTATION_OK) break;
        offset += line_length + 1U;
    }
    if (status == REVLINK_SYNC_ANNOTATION_OK) *annotations = *parsed;
    free(parsed);
    return status;
}
