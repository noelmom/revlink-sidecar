#include "revlink_sync_manifest.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define REVLINK_SYNC_MANIFEST_HEADER_V1 "REVLINK-MANIFEST\t1\n"
#define REVLINK_SYNC_MANIFEST_HEADER_V2 "REVLINK-MANIFEST\t2\n"
#define REVLINK_SYNC_MANIFEST_HEADER_V3 "REVLINK-MANIFEST\t3\n"
#define REVLINK_SYNC_MANIFEST_HEADER_V1_LENGTH \
    (sizeof(REVLINK_SYNC_MANIFEST_HEADER_V1) - 1U)
#define REVLINK_SYNC_MANIFEST_HEADER_V2_LENGTH \
    (sizeof(REVLINK_SYNC_MANIFEST_HEADER_V2) - 1U)
#define REVLINK_SYNC_MANIFEST_HEADER_V3_LENGTH \
    (sizeof(REVLINK_SYNC_MANIFEST_HEADER_V3) - 1U)

static bool valid_path_bytes(const uint8_t *value, size_t length)
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

static bool valid_cache_name(const char *value)
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

void revlink_sync_manifest_init(revlink_sync_manifest_t *manifest)
{
    if (manifest != NULL) {
        memset(manifest, 0, sizeof(*manifest));
    }
}

const revlink_sync_manifest_entry_t *revlink_sync_manifest_find(
    const revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length
)
{
    if (manifest == NULL || !valid_path_bytes(path, path_length)) {
        return NULL;
    }
    for (size_t index = 0U; index < manifest->count; ++index) {
        const revlink_sync_manifest_entry_t *entry =
            &manifest->entries[index];
        if (strlen(entry->path) == path_length
            && memcmp(entry->path, path, path_length) == 0) {
            return entry;
        }
    }
    return NULL;
}

bool revlink_sync_manifest_metadata_matches(
    const revlink_sync_manifest_entry_t *entry,
    uint32_t device_time_raw,
    uint32_t size
)
{
    /*
     * A zero device timestamp is not a stable identity. Be conservative and
     * re-read rather than skipping a same-name/same-size file.
     */
    return entry != NULL && device_time_raw != 0U
        && entry->device_time_raw == device_time_raw && entry->size == size;
}

revlink_sync_status_t revlink_sync_manifest_upsert(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *cache_name
)
{
    return revlink_sync_manifest_upsert_at(
        manifest,
        path,
        path_length,
        device_time_raw,
        size,
        0U,
        sha256,
        cache_name
    );
}

revlink_sync_status_t revlink_sync_manifest_upsert_at(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    uint32_t device_time_raw,
    uint32_t size,
    uint64_t initial_sync_utc,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES],
    const char *cache_name
)
{
    if (manifest == NULL || sha256 == NULL || size == 0U
        || !valid_path_bytes(path, path_length)
        || !valid_cache_name(cache_name)) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }

    size_t target = manifest->count;
    for (size_t index = 0U; index < manifest->count; ++index) {
        if (strlen(manifest->entries[index].path) == path_length
            && memcmp(manifest->entries[index].path, path, path_length) == 0) {
            target = index;
            break;
        }
    }
    const bool had_entry = target < manifest->count;
    if (target == manifest->count) {
        if (manifest->count >= REVLINK_SYNC_MANIFEST_CAPACITY) {
            return REVLINK_SYNC_CAPACITY_EXCEEDED;
        }
        ++manifest->count;
    }

    revlink_sync_manifest_entry_t *entry = &manifest->entries[target];
    const bool same_version =
        had_entry
        && memcmp(
            entry->sha256,
            sha256,
            REVLINK_SYNC_SHA256_BYTES
        ) == 0;
    const uint64_t preserved_initial_sync =
        same_version ? entry->initial_sync_utc : initial_sync_utc;
    /*
     * An upsert is a statement about content, not about the device. It runs
     * on the path that has just read the file *from* the AccessPort, so the
     * file was plainly there; carrying the previous value through — or
     * recording ON_DEVICE for a fresh entry — keeps a download from resetting
     * what a listing established moments earlier.
     */
    const revlink_sync_presence_t preserved_presence =
        had_entry ? entry->presence : REVLINK_SYNC_PRESENCE_UNKNOWN;
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->path, path, path_length);
    entry->path[path_length] = '\0';
    entry->device_time_raw = device_time_raw;
    entry->size = size;
    entry->initial_sync_utc = preserved_initial_sync;
    memcpy(entry->sha256, sha256, REVLINK_SYNC_SHA256_BYTES);
    memcpy(entry->cache_name, cache_name, strlen(cache_name) + 1U);
    entry->presence = preserved_presence;
    return REVLINK_SYNC_OK;
}

bool revlink_sync_manifest_set_presence(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length,
    revlink_sync_presence_t presence
)
{
    if (manifest == NULL || !valid_path_bytes(path, path_length)) {
        return false;
    }
    for (size_t index = 0U; index < manifest->count; ++index) {
        revlink_sync_manifest_entry_t *entry = &manifest->entries[index];
        if (strlen(entry->path) == path_length
            && memcmp(entry->path, path, path_length) == 0) {
            entry->presence = presence;
            return true;
        }
    }
    return false;
}

bool revlink_sync_manifest_remove(
    revlink_sync_manifest_t *manifest,
    const uint8_t *path,
    size_t path_length
)
{
    if (manifest == NULL || !valid_path_bytes(path, path_length)) {
        return false;
    }
    for (size_t index = 0U; index < manifest->count; ++index) {
        revlink_sync_manifest_entry_t *entry = &manifest->entries[index];
        if (strlen(entry->path) == path_length
            && memcmp(entry->path, path, path_length) == 0) {
            if (index + 1U < manifest->count) {
                *entry = manifest->entries[manifest->count - 1U];
            }
            memset(
                &manifest->entries[manifest->count - 1U],
                0,
                sizeof(manifest->entries[0])
            );
            --manifest->count;
            return true;
        }
    }
    return false;
}

size_t revlink_sync_manifest_digest_users(
    const revlink_sync_manifest_t *manifest,
    const uint8_t sha256[REVLINK_SYNC_SHA256_BYTES]
)
{
    if (manifest == NULL || sha256 == NULL) {
        return 0U;
    }
    size_t users = 0U;
    for (size_t index = 0U; index < manifest->count; ++index) {
        if (memcmp(
                manifest->entries[index].sha256,
                sha256,
                REVLINK_SYNC_SHA256_BYTES
            ) == 0) {
            ++users;
        }
    }
    return users;
}

const char *revlink_sync_presence_name(revlink_sync_presence_t presence)
{
    switch (presence) {
    case REVLINK_SYNC_PRESENCE_ON_DEVICE:
        return "on-device";
    case REVLINK_SYNC_PRESENCE_ABSENT:
        return "absent";
    case REVLINK_SYNC_PRESENCE_UNKNOWN:
    default:
        return "unknown";
    }
}

revlink_sync_status_t revlink_sync_manifest_serialize(
    const revlink_sync_manifest_t *manifest,
    char *output,
    size_t output_capacity,
    size_t *output_length
)
{
    if (manifest == NULL || output_length == NULL
        || (output == NULL && output_capacity != 0U)
        || manifest->count > REVLINK_SYNC_MANIFEST_CAPACITY) {
        return REVLINK_SYNC_INVALID_ARGUMENT;
    }

    size_t used = 0U;
#define APPEND_BYTES(value, length) \
    do { \
        const size_t append_length = (length); \
        if (append_length > output_capacity - used) { \
            *output_length = used + append_length; \
            return REVLINK_SYNC_BUFFER_TOO_SMALL; \
        } \
        memcpy(output + used, (value), append_length); \
        used += append_length; \
    } while (0)

    APPEND_BYTES(
        REVLINK_SYNC_MANIFEST_HEADER_V3,
        REVLINK_SYNC_MANIFEST_HEADER_V3_LENGTH
    );
    for (size_t index = 0U; index < manifest->count; ++index) {
        const revlink_sync_manifest_entry_t *entry =
            &manifest->entries[index];
        const size_t path_length = strlen(entry->path);
        if (!valid_path_bytes((const uint8_t *)entry->path, path_length)
            || !valid_cache_name(entry->cache_name) || entry->size == 0U) {
            return REVLINK_SYNC_INVALID_ARGUMENT;
        }

        char prefix[REVLINK_SYNC_PATH_CAPACITY + 64U];
        const int prefix_length = snprintf(
            prefix,
            sizeof(prefix),
            "%s\t%u\t%u\t%llu\t",
            entry->path,
            (unsigned int)entry->device_time_raw,
            (unsigned int)entry->size,
            (unsigned long long)entry->initial_sync_utc
        );
        if (prefix_length < 0 || (size_t)prefix_length >= sizeof(prefix)) {
            return REVLINK_SYNC_INVALID_ARGUMENT;
        }
        APPEND_BYTES(prefix, (size_t)prefix_length);

        char digest_text[REVLINK_SYNC_SHA256_BYTES * 2U];
        static const char hex[] = "0123456789abcdef";
        for (size_t byte = 0U; byte < REVLINK_SYNC_SHA256_BYTES; ++byte) {
            digest_text[byte * 2U] = hex[entry->sha256[byte] >> 4U];
            digest_text[byte * 2U + 1U] = hex[entry->sha256[byte] & 0x0fU];
        }
        APPEND_BYTES(digest_text, sizeof(digest_text));
        APPEND_BYTES("\t", 1U);
        APPEND_BYTES(entry->cache_name, strlen(entry->cache_name));
        APPEND_BYTES("\t", 1U);
        APPEND_BYTES(
            entry->presence == REVLINK_SYNC_PRESENCE_ON_DEVICE
                ? "1"
                : entry->presence == REVLINK_SYNC_PRESENCE_ABSENT ? "2" : "0",
            1U
        );
        APPEND_BYTES("\n", 1U);
    }
#undef APPEND_BYTES

    *output_length = used;
    return REVLINK_SYNC_OK;
}

revlink_sync_status_t revlink_sync_manifest_parse(
    const char *input,
    size_t input_length,
    revlink_sync_manifest_t *manifest
)
{
    if (input == NULL || manifest == NULL) {
        return REVLINK_SYNC_INVALID_FORMAT;
    }
    /*
     * v3 adds a presence column. v1 and v2 still load, and their entries come
     * back as UNKNOWN — which is exactly what they are: those files were
     * catalogued before anything recorded whether a listing had seen them.
     */
    unsigned int version = 0U;
    bool version_two = false;
    size_t header_length = 0U;
    if (
        input_length >= REVLINK_SYNC_MANIFEST_HEADER_V3_LENGTH
        && memcmp(
            input,
            REVLINK_SYNC_MANIFEST_HEADER_V3,
            REVLINK_SYNC_MANIFEST_HEADER_V3_LENGTH
        ) == 0
    ) {
        version = 3U;
        version_two = true;
        header_length = REVLINK_SYNC_MANIFEST_HEADER_V3_LENGTH;
    } else if (
        input_length >= REVLINK_SYNC_MANIFEST_HEADER_V2_LENGTH
        && memcmp(
            input,
            REVLINK_SYNC_MANIFEST_HEADER_V2,
            REVLINK_SYNC_MANIFEST_HEADER_V2_LENGTH
        ) == 0
    ) {
        version = 2U;
        version_two = true;
        header_length = REVLINK_SYNC_MANIFEST_HEADER_V2_LENGTH;
    } else if (
        input_length >= REVLINK_SYNC_MANIFEST_HEADER_V1_LENGTH
        && memcmp(
            input,
            REVLINK_SYNC_MANIFEST_HEADER_V1,
            REVLINK_SYNC_MANIFEST_HEADER_V1_LENGTH
        ) == 0
    ) {
        version = 1U;
        header_length = REVLINK_SYNC_MANIFEST_HEADER_V1_LENGTH;
    } else {
        return REVLINK_SYNC_INVALID_FORMAT;
    }

    /*
     * A complete manifest is deliberately bounded, but still much larger
     * than an embedded task stack. Keep parsing transactional without placing
     * the scratch snapshot on the caller's stack.
     */
    revlink_sync_manifest_t *parsed = calloc(1U, sizeof(*parsed));
    if (parsed == NULL) {
        return REVLINK_SYNC_ALLOCATION_FAILED;
    }

    revlink_sync_status_t status = REVLINK_SYNC_OK;
    size_t offset = header_length;
    while (offset < input_length) {
        const char *line = input + offset;
        const char *newline = memchr(line, '\n', input_length - offset);
        if (newline == NULL) {
            status = REVLINK_SYNC_INVALID_FORMAT;
            goto done;
        }
        const size_t line_length = (size_t)(newline - line);
        if (line_length == 0U) {
            status = REVLINK_SYNC_INVALID_FORMAT;
            goto done;
        }

        const char *fields[7];
        size_t lengths[7];
        size_t field_count = 0U;
        size_t field_start = 0U;
        for (size_t index = 0U; index <= line_length; ++index) {
            if (index == line_length || line[index] == '\t') {
                if (field_count >= 7U || index == field_start) {
                    status = REVLINK_SYNC_INVALID_FORMAT;
                    goto done;
                }
                fields[field_count] = line + field_start;
                lengths[field_count] = index - field_start;
                ++field_count;
                field_start = index + 1U;
            }
        }
        const size_t expected_fields =
            version == 3U ? 7U : (version_two ? 6U : 5U);
        const size_t cache_field = version_two ? 5U : 4U;
        const size_t digest_field = version_two ? 4U : 3U;
        if (field_count != expected_fields
            || !valid_path_bytes(
                (const uint8_t *)fields[0],
                lengths[0]
            )
            || lengths[cache_field] >= REVLINK_SYNC_CACHE_NAME_CAPACITY) {
            status = REVLINK_SYNC_INVALID_FORMAT;
            goto done;
        }

        char cache_name[REVLINK_SYNC_CACHE_NAME_CAPACITY];
        memcpy(cache_name, fields[cache_field], lengths[cache_field]);
        cache_name[lengths[cache_field]] = '\0';
        uint32_t device_time_raw = 0U;
        uint32_t size = 0U;
        uint64_t initial_sync_utc = 0U;
        uint8_t digest[REVLINK_SYNC_SHA256_BYTES];
        if (!parse_u32(fields[1], lengths[1], &device_time_raw)
            || !parse_u32(fields[2], lengths[2], &size)
            || size == 0U
            || (version_two
                && !parse_u64(fields[3], lengths[3], &initial_sync_utc))
            || !parse_digest(
                fields[digest_field],
                lengths[digest_field],
                digest
            )
            || !valid_cache_name(cache_name)) {
            status = REVLINK_SYNC_INVALID_FORMAT;
            goto done;
        }
        revlink_sync_presence_t presence = REVLINK_SYNC_PRESENCE_UNKNOWN;
        if (version == 3U) {
            if (lengths[6] != 1U) {
                status = REVLINK_SYNC_INVALID_FORMAT;
                goto done;
            }
            switch (fields[6][0]) {
            case '0':
                presence = REVLINK_SYNC_PRESENCE_UNKNOWN;
                break;
            case '1':
                presence = REVLINK_SYNC_PRESENCE_ON_DEVICE;
                break;
            case '2':
                presence = REVLINK_SYNC_PRESENCE_ABSENT;
                break;
            default:
                status = REVLINK_SYNC_INVALID_FORMAT;
                goto done;
            }
        }
        const revlink_sync_status_t upsert_status =
            revlink_sync_manifest_upsert_at(
                parsed,
                (const uint8_t *)fields[0],
                lengths[0],
                device_time_raw,
                size,
                initial_sync_utc,
                digest,
                cache_name
            );
        if (upsert_status != REVLINK_SYNC_OK) {
            status = upsert_status == REVLINK_SYNC_CAPACITY_EXCEEDED
                ? upsert_status
                : REVLINK_SYNC_INVALID_FORMAT;
            goto done;
        }
        (void)revlink_sync_manifest_set_presence(
            parsed,
            (const uint8_t *)fields[0],
            lengths[0],
            presence
        );
        offset += line_length + 1U;
    }

    *manifest = *parsed;

done:
    free(parsed);
    return status;
}

const char *revlink_sync_status_name(revlink_sync_status_t status)
{
    switch (status) {
    case REVLINK_SYNC_OK:
        return "ok";
    case REVLINK_SYNC_INVALID_ARGUMENT:
        return "invalid argument";
    case REVLINK_SYNC_INVALID_FORMAT:
        return "invalid format";
    case REVLINK_SYNC_CAPACITY_EXCEEDED:
        return "capacity exceeded";
    case REVLINK_SYNC_BUFFER_TOO_SMALL:
        return "buffer too small";
    case REVLINK_SYNC_ALLOCATION_FAILED:
        return "allocation failed";
    default:
        return "unknown";
    }
}
