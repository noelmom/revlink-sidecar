/*
 * Delete is irreversible and the AccessPort offers no undo, so the allowlist
 * is the whole safety story. These tests exist to make widening it loud.
 */
#include "revlink_accessport_protocol.h"

#include <stdio.h>
#include <string.h>

static unsigned int failures;
static unsigned int checks;

#define CHECK(condition, message)                                          \
    do {                                                                   \
        ++checks;                                                          \
        if (!(condition)) {                                                \
            ++failures;                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, (message));     \
        }                                                                  \
    } while (0)

static revlink_ap_status_t validate(const char *path)
{
    return revlink_ap_validate_delete_target(
        (const uint8_t *)path,
        strlen(path)
    );
}

static void test_permitted_targets(void)
{
    const char *allowed[] = {
        "maps/Stage1.ptm",
        "maps/a",
        "maps/name with spaces (and parens).ptm",
        "datalog/datalog35.csv",
        "datalog/datalog1.csv.gz",
        "datalog/x",
    };
    for (size_t i = 0U; i < sizeof(allowed) / sizeof(allowed[0]); ++i) {
        CHECK(
            validate(allowed[i]) == REVLINK_AP_OK,
            "a file directly inside maps/ or datalog/ should be deletable"
        );
    }
}

static void test_other_directories_are_refused(void)
{
    const char *refused[] = {
        "images/startup_screen.fb",  /* writable, but never deletable */
        "images/anything.fb",
        "config/settings.bin",
        "system/log.txt",
        "root.txt",
        "",
    };
    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        CHECK(
            validate(refused[i]) != REVLINK_AP_OK,
            "only maps/ and datalog/ are deletable"
        );
    }
}

static void test_the_directories_themselves_are_refused(void)
{
    /* A bare prefix names the directory, not a file inside it. */
    CHECK(validate("maps/") != REVLINK_AP_OK, "maps/ is not a deletable file");
    CHECK(
        validate("datalog/") != REVLINK_AP_OK,
        "datalog/ is not a deletable file"
    );
    CHECK(validate("maps") != REVLINK_AP_OK, "maps is not a deletable file");
    CHECK(
        validate("datalog") != REVLINK_AP_OK,
        "datalog is not a deletable file"
    );
}

static void test_traversal_and_nesting_are_refused(void)
{
    const char *refused[] = {
        "maps/../images/startup_screen.fb",
        "maps/../../etc/passwd",
        "maps/..",
        "datalog/../maps/Stage1.ptm",
        "maps/sub/Stage1.ptm",
        "datalog/sub/datalog1.csv",
        "maps/sub/",
        "maps\\\\Stage1.ptm",
        "maps/back\\\\slash.ptm",
    };
    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        CHECK(
            validate(refused[i]) != REVLINK_AP_OK,
            "traversal, nesting, and separators must be refused"
        );
    }
}

static void test_prefix_lookalikes_are_refused(void)
{
    /* Directories whose names merely begin with an allowed one. */
    const char *refused[] = {
        "mapsx/Stage1.ptm",
        "maps2/Stage1.ptm",
        "datalogs/datalog1.csv",
        "datalogx/datalog1.csv",
    };
    for (size_t i = 0U; i < sizeof(refused) / sizeof(refused[0]); ++i) {
        CHECK(
            validate(refused[i]) != REVLINK_AP_OK,
            "a directory that only starts with maps or datalog is not allowed"
        );
    }
}

static void test_embedded_nul_is_refused(void)
{
    /* Length is authoritative; a NUL inside the span must not be accepted. */
    const uint8_t path[] = {'m','a','p','s','/','a','\0','b'};
    CHECK(
        revlink_ap_validate_delete_target(path, sizeof(path))
            != REVLINK_AP_OK,
        "an embedded NUL must be refused"
    );
    CHECK(
        revlink_ap_validate_delete_target(NULL, 8U)
            == REVLINK_AP_INVALID_ARGUMENT,
        "a NULL path is an invalid argument"
    );
}

static void test_delete_record_matches_a_download_body(void)
{
    /*
     * PROTOCOL_NOTES records the delete body as byte-identical to a download,
     * differing only in opcode. Build both and prove exactly that, so the
     * builder cannot drift from the capture it came from.
     */
    const uint8_t name[] = "datalog35.csv";
    const uint8_t path[] = "datalog/datalog35.csv";
    uint8_t deleted[256];
    uint8_t downloaded[256];
    size_t deleted_length = 0U;
    size_t downloaded_length = 0U;

    CHECK(
        revlink_ap_build_delete(
            name, sizeof(name) - 1U, path, sizeof(path) - 1U,
            deleted, sizeof(deleted), &deleted_length
        ) == REVLINK_AP_OK,
        "delete record should build"
    );
    CHECK(
        revlink_ap_build_download(
            name, sizeof(name) - 1U, path, sizeof(path) - 1U,
            downloaded, sizeof(downloaded), &downloaded_length
        ) == REVLINK_AP_OK,
        "download record should build"
    );
    CHECK(
        deleted_length == downloaded_length,
        "delete and download records are the same length"
    );

    unsigned int differences = 0U;
    for (size_t i = 0U; i < deleted_length && i < downloaded_length; ++i) {
        if (deleted[i] != downloaded[i]) ++differences;
    }
    /* Opcode differs (two bytes), and the checksum that covers it. */
    CHECK(
        differences > 0U && differences <= 6U,
        "only the opcode and its checksum should differ"
    );
    CHECK(
        revlink_ap_validate_record(deleted, deleted_length) == REVLINK_AP_OK,
        "the delete record should pass record validation"
    );
}

static void test_builder_rejects_bad_arguments(void)
{
    uint8_t output[256];
    size_t length = 0U;
    CHECK(
        revlink_ap_build_delete(
            NULL, 4U, (const uint8_t *)"maps/a", 6U,
            output, sizeof(output), &length
        ) == REVLINK_AP_INVALID_ARGUMENT,
        "a NULL name with a non-zero length is invalid"
    );
    CHECK(
        revlink_ap_build_delete(
            (const uint8_t *)"a", 1U, (const uint8_t *)"maps/a", 6U,
            output, 8U, &length
        ) != REVLINK_AP_OK,
        "a buffer too small to hold the record must not be written"
    );
}

int main(void)
{
    test_permitted_targets();
    test_other_directories_are_refused();
    test_the_directories_themselves_are_refused();
    test_traversal_and_nesting_are_refused();
    test_prefix_lookalikes_are_refused();
    test_embedded_nul_is_refused();
    test_delete_record_matches_a_download_body();
    test_builder_rejects_bad_arguments();

    if (failures == 0U) {
        printf("delete target host test PASSED (%u checks)\n", checks);
        return 0;
    }
    printf("delete target host test FAILED (%u/%u)\n", failures, checks);
    return 1;
}
