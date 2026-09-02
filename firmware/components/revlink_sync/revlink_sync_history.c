#include "revlink_sync_history.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define HISTORY_HEADER_V1_PREFIX "REVLINK-HISTORY\t1\t"
#define HISTORY_HEADER_V2_PREFIX "REVLINK-HISTORY\t2\t"
#define HISTORY_HEADER_V1_PREFIX_LENGTH \
    (sizeof(HISTORY_HEADER_V1_PREFIX) - 1U)
#define HISTORY_HEADER_V2_PREFIX_LENGTH \
    (sizeof(HISTORY_HEADER_V2_PREFIX) - 1U)

static bool valid_path(const uint8_t *value, size_t length)
{
    if (value == NULL || length == 0U
        || length >= REVLINK_SYNC_PATH_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const uint8_t byte = value[index];
        const bool allowed =
            (byte >= 'a' && byte <= 'z')
            || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9')
            || byte == '/' || byte == '.' || byte == '_' || byte == '-'
            || byte == ' ' || byte == '(' || byte == ')';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool valid_object_name(const char *value)
{
    if (value == NULL) {
        return false;
    }
    const size_t length = strlen(value);
    if (length == 0U || length >= REVLINK_SYNC_CACHE_NAME_CAPACITY) {
        return false;
    }
    for (size_t index = 0U; index < length; ++index) {
        const unsigned char byte = (unsigned char)value[index];
        const bool allowed =
            (byte >= 'a' && byte <= 'z')
            || (byte >= 'A' && byte <= 'Z')
            || (byte >= '0' && byte <= '9')
            || byte == '.' || byte == '_' || byte == '-';
        if (!allowed) {
            return false;
        }
    }
    return true;
}

static bool parse_u32(const char *value, size_t length, uint32_t *result)
{
    if (value == NULL || result == NULL || length == 0U || length > 10U) {
        return false;
    }
    uint32_t parsed = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9') {
            return false;
        }
        const uint32_t digit = (uint32_t)(value[index] - '0');
        if (parsed > (UINT32_MAX - digit) / 10U) {
            return false;
        }
        parsed = parsed * 10U + digit;
    }
    *result = parsed;
    return true;
}

static bool parse_u64(const char *value, size_t length, uint64_t *result)
{
    if (value == NULL || result == NULL || length == 0U || length > 20U) {
        return false;
    }
    uint64_t parsed = 0U;
    for (size_t index = 0U; index < length; ++index) {
        if (value[index] < '0' || value[index] > '9') return false;
        const uint64_t digit = (uint64_t)(value[index] - '0');
        if (parsed > (UINT64_MAX - digit) / 10U) return false;
        parsed = parsed * 10U + digit;
    }
    *result = parsed;
    return true;
}

static int hex_value(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

static bool parse_digest(
    const char *value,
    size_t length,
    uint8_t digest[REVLINK_SYNC_SHA256_BYTES]
)
{
    if (value == NULL || digest == NULL
        || length != REVLINK_SYNC_SHA256_BYTES * 2U) {
        return false;
    }
    for (size_t index = 0U; index < REVLINK_SYNC_SHA256_BYTES; ++index) {
        const int high = hex_value(value[index * 2U]);
        const int low = hex_value(value[index * 2U + 1U]);
        if (high < 0 || low < 0) {
            return false;
        }
        digest[index] = (uint8_t)((high << 4U) | low);
    }
    return true;
}

void revlink_sync_history_init(revlink_sync_history_t *history)
{
    if (history != NULL) {
        memset(history, 0, sizeof(*history));
        history->next_sequence = 1U;
    }
}

const revlink_sync_history_entry_t *revlink_sync_history_find_version(
    const revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES]
)
{
    if (history == NULL || sha256 == NULL
        || !valid_path(path, path_length)) {
        return NULL;
    }
    for (size_t index = 0U; index < history->count; ++index) {
        const revlink_sync_history_entry_t *entry = &history->entries[index];
        if (strlen(entry->path) == path_length
            && memcmp(entry->path, path, path_length) == 0
            && memcmp(
                entry->sha256,
                sha256,
                REVLINK_SYNC_SHA256_BYTES
            ) == 0) {
            return entry;
        }
    }
    return NULL;
}

revlink_sync_manifest_status_t revlink_sync_history_record(
    revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *object_name,
    uint32_t *sequence
)
{
    return revlink_sync_history_record_at(
        history,
        path,
        path_length,
        device_time_raw,
        size,
        0U,
        sha256,
        object_name,
        sequence
    );
}

revlink_sync_manifest_status_t revlink_sync_history_record_at(
    revlink_sync_history_t *history,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    uint64_t initial_sync_utc,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *object_name,
    uint32_t *sequence
)
{
    if (history == NULL || sha256 == NULL || sequence == NULL || size == 0U
        || !valid_path(path, path_length)
        || !valid_object_name(object_name)
        || history->next_sequence == 0U) {
        return REVLINK_SYNC_MANIFEST_INVALID_ARGUMENT;
    }
    const revlink_sync_history_entry_t *existing =
        revlink_sync_history_find_version(
            history,
            path,
            path_length,
            sha256
        );
    if (existing != NULL) {
        *sequence = existing->sequence;
        return REVLINK_SYNC_MANIFEST_OK;
    }
    if (history->count >= REVLINK_SYNC_HISTORY_CAPACITY
        || history->next_sequence == UINT32_MAX) {
        return REVLINK_SYNC_MANIFEST_CAPACITY_EXCEEDED;
    }

    revlink_sync_history_entry_t *entry = &history->entries[history->count++];
    memset(entry, 0, sizeof(*entry));
    entry->sequence = history->next_sequence++;
    memcpy(entry->path, path, path_length);
    entry->path[path_length] = '\0';
    entry->device_time_raw = device_time_raw;
    entry->size = size;
    entry->initial_sync_utc = initial_sync_utc;
    memcpy(entry->sha256, sha256, REVLINK_SYNC_SHA256_BYTES);
    memcpy(entry->object_name, object_name, strlen(object_name) + 1U);
    *sequence = entry->sequence;
    return REVLINK_SYNC_MANIFEST_OK;
}

revlink_sync_manifest_status_t revlink_sync_history_serialize(
    const revlink_sync_history_t *history,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (history == NULL || output_length == NULL
        || (output == NULL && output_capacity != 0U)
        || history->count > REVLINK_SYNC_HISTORY_CAPACITY
        || history->next_sequence == 0U) {
        return REVLINK_SYNC_MANIFEST_INVALID_ARGUMENT;
    }
    size_t used = 0U;
#define APPEND(value, length) \
    do { \
        const size_t append_length = (length); \
        if (append_length > output_capacity - used) { \
            *output_length = used + append_length; \
            return REVLINK_SYNC_MANIFEST_BUFFER_TOO_SMALL; \
        } \
        memcpy(output + used, (value), append_length); \
        used += append_length; \
    } while (0)

    char header[64];
    const int header_length = snprintf(
        header,
        sizeof(header),
        "%s%u\n",
        HISTORY_HEADER_V2_PREFIX,
        (unsigned int)history->next_sequence
    );
    if (header_length < 0 || (size_t)header_length >= sizeof(header)) {
        return REVLINK_SYNC_MANIFEST_INVALID_ARGUMENT;
    }
    APPEND(header, (size_t)header_length);

    static const char hex[] = "0123456789abcdef";
    for (size_t index = 0U; index < history->count; ++index) {
        const revlink_sync_history_entry_t *entry = &history->entries[index];
        if (entry->sequence == 0U || entry->size == 0U
            || !valid_path(
                (const uint8_t *)entry->path,
                strlen(entry->path)
            )
            || !valid_object_name(entry->object_name)) {
            return REVLINK_SYNC_MANIFEST_INVALID_ARGUMENT;
        }
        char prefix[REVLINK_SYNC_PATH_CAPACITY + 80U];
        const int prefix_length = snprintf(
            prefix,
            sizeof(prefix),
            "%u\t%s\t%u\t%u\t%llu\t",
            (unsigned int)entry->sequence,
            entry->path,
            (unsigned int)entry->device_time_raw,
            (unsigned int)entry->size,
            (unsigned long long)entry->initial_sync_utc
        );
        if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) {
            return REVLINK_SYNC_MANIFEST_INVALID_ARGUMENT;
        }
        APPEND(prefix, (size_t)prefix_length);
        char digest[REVLINK_SYNC_SHA256_BYTES * 2U];
        for (size_t byte = 0U; byte < REVLINK_SYNC_SHA256_BYTES; ++byte) {
            digest[byte * 2U] = hex[entry->sha256[byte] >> 4U];
            digest[byte * 2U + 1U] = hex[entry->sha256[byte] & 0x0fU];
        }
        APPEND(digest, sizeof(digest));
        APPEND("\t", 1U);
        APPEND(entry->object_name, strlen(entry->object_name));
        APPEND("\n", 1U);
    }
#undef APPEND
    *output_length = used;
    return REVLINK_SYNC_MANIFEST_OK;
}

revlink_sync_manifest_status_t revlink_sync_history_parse(
    const char *input,
    size_t input_length,
    revlink_sync_history_t *history
)
{
    if (input == NULL || history == NULL) {
        return REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
    }
    bool version_two = false;
    size_t header_prefix_length = 0U;
    if (
        input_length > HISTORY_HEADER_V2_PREFIX_LENGTH
        && memcmp(
            input,
            HISTORY_HEADER_V2_PREFIX,
            HISTORY_HEADER_V2_PREFIX_LENGTH
        ) == 0
    ) {
        version_two = true;
        header_prefix_length = HISTORY_HEADER_V2_PREFIX_LENGTH;
    } else if (
        input_length > HISTORY_HEADER_V1_PREFIX_LENGTH
        && memcmp(
            input,
            HISTORY_HEADER_V1_PREFIX,
            HISTORY_HEADER_V1_PREFIX_LENGTH
        ) == 0
    ) {
        header_prefix_length = HISTORY_HEADER_V1_PREFIX_LENGTH;
    } else {
        return REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
    }
    const char *header_newline = memchr(input, '\n', input_length);
    uint32_t encoded_next = 0U;
    if (header_newline == NULL
        || !parse_u32(
            input + header_prefix_length,
            (size_t)(header_newline - input)
                - header_prefix_length,
            &encoded_next
        )
        || encoded_next == 0U) {
        return REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
    }

    revlink_sync_history_t *parsed = calloc(1U, sizeof(*parsed));
    if (parsed == NULL) {
        return REVLINK_SYNC_MANIFEST_ALLOCATION_FAILED;
    }
    parsed->next_sequence = encoded_next;
    revlink_sync_manifest_status_t status = REVLINK_SYNC_MANIFEST_OK;
    size_t offset = (size_t)(header_newline - input) + 1U;
    uint32_t highest_sequence = 0U;
    while (offset < input_length) {
        const char *line = input + offset;
        const char *newline = memchr(line, '\n', input_length - offset);
        if (newline == NULL || newline == line) {
            status = REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
            goto done;
        }
        const size_t line_length = (size_t)(newline - line);
        const char *fields[7] = {0};
        size_t lengths[7] = {0};
        size_t count = 0U;
        size_t start = 0U;
        for (size_t index = 0U; index <= line_length; ++index) {
            if (index == line_length || line[index] == '\t') {
                if (count >= 7U || index == start) {
                    status = REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
                    goto done;
                }
                fields[count] = line + start;
                lengths[count] = index - start;
                ++count;
                start = index + 1U;
            }
        }
        uint32_t sequence = 0U;
        uint32_t device_time_raw = 0U;
        uint32_t size = 0U;
        uint64_t initial_sync_utc = 0U;
        uint8_t digest[REVLINK_SYNC_SHA256_BYTES];
        char object_name[REVLINK_SYNC_CACHE_NAME_CAPACITY];
        const size_t expected_fields = version_two ? 7U : 6U;
        const size_t digest_field = version_two ? 5U : 4U;
        const size_t object_field = version_two ? 6U : 5U;
        if (count != expected_fields
            || !parse_u32(fields[0], lengths[0], &sequence)
            || sequence == 0U || sequence <= highest_sequence
            || !valid_path((const uint8_t *)fields[1], lengths[1])
            || !parse_u32(fields[2], lengths[2], &device_time_raw)
            || !parse_u32(fields[3], lengths[3], &size) || size == 0U
            || (version_two
                && !parse_u64(fields[4], lengths[4], &initial_sync_utc))
            || !parse_digest(
                fields[digest_field],
                lengths[digest_field],
                digest
            )
            || lengths[object_field] >= sizeof(object_name)) {
            status = REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
            goto done;
        }
        memcpy(
            object_name,
            fields[object_field],
            lengths[object_field]
        );
        object_name[lengths[object_field]] = '\0';
        if (!valid_object_name(object_name)
            || parsed->count >= REVLINK_SYNC_HISTORY_CAPACITY) {
            status = parsed->count >= REVLINK_SYNC_HISTORY_CAPACITY
                ? REVLINK_SYNC_MANIFEST_CAPACITY_EXCEEDED
                : REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
            goto done;
        }
        revlink_sync_history_entry_t *entry =
            &parsed->entries[parsed->count++];
        entry->sequence = sequence;
        memcpy(entry->path, fields[1], lengths[1]);
        entry->path[lengths[1]] = '\0';
        entry->device_time_raw = device_time_raw;
        entry->size = size;
        entry->initial_sync_utc = initial_sync_utc;
        memcpy(entry->sha256, digest, sizeof(digest));
        memcpy(entry->object_name, object_name, strlen(object_name) + 1U);
        highest_sequence = sequence;
        offset += line_length + 1U;
    }
    if (highest_sequence >= parsed->next_sequence) {
        status = REVLINK_SYNC_MANIFEST_INVALID_FORMAT;
        goto done;
    }
    *history = *parsed;

done:
    free(parsed);
    return status;
}
