/* Host tests for staged-map serialization and the auto-apply decision. */
#include "revlink_staged_map.h"

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

static revlink_staged_map_record_t sample_record(void)
{
    revlink_staged_map_record_t record;
    memset(&record, 0, sizeof(record));
    record.kind = REVLINK_STAGED_MAP_KIND_MAP;
    record.size = 262144U;
    for (unsigned int i = 0U; i < REVLINK_STAGED_MAP_SHA256_BYTES; ++i) {
        record.sha256[i] = (uint8_t)(i * 7U + 1U);
    }
    snprintf(record.name, sizeof(record.name), "stage2-93oct.ptm");
    snprintf(
        record.destination,
        sizeof(record.destination),
        "maps/stage2-93oct.ptm"
    );
    snprintf(
        record.target_part_number,
        sizeof(record.target_part_number),
        "AP3-SUB-004"
    );
    snprintf(
        record.target_serial,
        sizeof(record.target_serial),
        "SN-0001-ALPHA"
    );
    return record;
}

static void test_round_trip(void)
{
    const revlink_staged_map_record_t original = sample_record();
    uint8_t buffer[REVLINK_STAGED_MAP_RECORD_BYTES];
    size_t written = 0U;

    CHECK(
        revlink_staged_map_encode(
            &original,
            buffer,
            sizeof(buffer),
            &written
        ) == REVLINK_STAGED_MAP_OK,
        "encode should succeed"
    );
    CHECK(
        written == REVLINK_STAGED_MAP_RECORD_BYTES,
        "encode should fill the whole fixed-size record"
    );

    revlink_staged_map_record_t decoded;
    memset(&decoded, 0xAA, sizeof(decoded));
    CHECK(
        revlink_staged_map_decode(buffer, written, &decoded)
            == REVLINK_STAGED_MAP_OK,
        "decode should succeed"
    );
    CHECK(decoded.kind == original.kind, "kind should survive");
    CHECK(decoded.size == original.size, "size should survive");
    CHECK(
        memcmp(
            decoded.sha256,
            original.sha256,
            REVLINK_STAGED_MAP_SHA256_BYTES
        ) == 0,
        "digest should survive"
    );
    CHECK(strcmp(decoded.name, original.name) == 0, "name should survive");
    CHECK(
        strcmp(decoded.destination, original.destination) == 0,
        "destination should survive"
    );
    CHECK(
        strcmp(decoded.target_part_number, original.target_part_number) == 0,
        "target part number should survive"
    );
    CHECK(
        strcmp(decoded.target_serial, original.target_serial) == 0,
        "target serial should survive"
    );
}

static void test_encoding_is_deterministic(void)
{
    const revlink_staged_map_record_t record = sample_record();
    uint8_t first[REVLINK_STAGED_MAP_RECORD_BYTES];
    uint8_t second[REVLINK_STAGED_MAP_RECORD_BYTES];
    memset(first, 0x11, sizeof(first));
    memset(second, 0x22, sizeof(second));

    CHECK(
        revlink_staged_map_encode(&record, first, sizeof(first), NULL)
            == REVLINK_STAGED_MAP_OK,
        "first encode should succeed"
    );
    CHECK(
        revlink_staged_map_encode(&record, second, sizeof(second), NULL)
            == REVLINK_STAGED_MAP_OK,
        "second encode should succeed"
    );
    CHECK(
        memcmp(first, second, sizeof(first)) == 0,
        "encoding the same record twice must produce identical bytes"
    );
}

static void test_corruption_is_rejected(void)
{
    const revlink_staged_map_record_t record = sample_record();
    uint8_t buffer[REVLINK_STAGED_MAP_RECORD_BYTES];
    revlink_staged_map_record_t decoded;

    /* Every single-byte corruption must be caught by the CRC. */
    const size_t probes[] = {0U, 4U, 8U, 12U, 20U, 60U, 200U, 440U, 480U};
    for (size_t i = 0U; i < sizeof(probes) / sizeof(probes[0]); ++i) {
        CHECK(
            revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
                == REVLINK_STAGED_MAP_OK,
            "encode should succeed before corruption"
        );
        buffer[probes[i]] = (uint8_t)(buffer[probes[i]] ^ 0xFFu);
        const revlink_staged_map_status_t status =
            revlink_staged_map_decode(buffer, sizeof(buffer), &decoded);
        CHECK(
            status != REVLINK_STAGED_MAP_OK,
            "a corrupted record must never decode successfully"
        );
    }

    /* A truncated file is not a valid record. */
    CHECK(
        revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
            == REVLINK_STAGED_MAP_OK,
        "encode should succeed"
    );
    CHECK(
        revlink_staged_map_decode(
            buffer,
            REVLINK_STAGED_MAP_RECORD_BYTES - 1U,
            &decoded
        ) == REVLINK_STAGED_MAP_TRUNCATED,
        "a short read must report truncation"
    );

    /* An all-zero file (freshly created, never written) is not valid. */
    memset(buffer, 0, sizeof(buffer));
    CHECK(
        revlink_staged_map_decode(buffer, sizeof(buffer), &decoded)
            == REVLINK_STAGED_MAP_BAD_MAGIC,
        "an empty record must be rejected by magic"
    );
}

static void test_unpinned_record_is_rejected(void)
{
    revlink_staged_map_record_t record = sample_record();
    uint8_t buffer[REVLINK_STAGED_MAP_RECORD_BYTES];

    record.target_serial[0] = '\0';
    CHECK(
        revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
            == REVLINK_STAGED_MAP_INVALID_ARGUMENT,
        "a record without a target serial must not encode"
    );

    record = sample_record();
    record.target_part_number[0] = '\0';
    CHECK(
        revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
            == REVLINK_STAGED_MAP_INVALID_ARGUMENT,
        "a record without a target part number must not encode"
    );

    record = sample_record();
    record.size = 0U;
    CHECK(
        revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
            == REVLINK_STAGED_MAP_INVALID_ARGUMENT,
        "a zero-length payload must not encode"
    );
}

/*
 * A record whose pin fields were zeroed in place, then re-checksummed, must
 * still be refused at decode. This is the shape a downgrade or a hand-edited
 * file would take, and it is exactly the case that must not be read as
 * "applies to any device".
 */
static void test_blanked_pin_with_valid_crc_is_rejected(void)
{
    const revlink_staged_map_record_t record = sample_record();
    uint8_t buffer[REVLINK_STAGED_MAP_RECORD_BYTES];
    CHECK(
        revlink_staged_map_encode(&record, buffer, sizeof(buffer), NULL)
            == REVLINK_STAGED_MAP_OK,
        "encode should succeed"
    );

    /* Blank the serial field (offset 472, 64 bytes) and repair the CRC. */
    memset(buffer + 472U, 0, 64U);
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0U; i < 536U; ++i) {
        crc ^= buffer[i];
        for (unsigned int bit = 0U; bit < 8U; ++bit) {
            const uint32_t mask = (uint32_t)(-(int32_t)(crc & 1u));
            crc = (crc >> 1) ^ (0xEDB88320u & mask);
        }
    }
    crc = ~crc;
    buffer[536U] = (uint8_t)(crc & 0xFFu);
    buffer[537U] = (uint8_t)((crc >> 8) & 0xFFu);
    buffer[538U] = (uint8_t)((crc >> 16) & 0xFFu);
    buffer[539U] = (uint8_t)((crc >> 24) & 0xFFu);

    revlink_staged_map_record_t decoded;
    CHECK(
        revlink_staged_map_decode(buffer, sizeof(buffer), &decoded)
            == REVLINK_STAGED_MAP_MALFORMED_FIELD,
        "a checksum-valid record with a blank pin must still be refused"
    );
}

/* ------------------------------------------------------------------ */

static revlink_staged_map_apply_context_t ready_context(void)
{
    revlink_staged_map_apply_context_t context;
    memset(&context, 0, sizeof(context));
    context.writes_compiled = true;
    context.consent_enabled = true;
    context.auto_apply_enabled = true;
    context.staged = true;
    context.device_identified = true;
    context.sync_completed_clean = true;
    context.sync_pending = 0U;
    context.transfer_running = false;
    context.recovery_required = false;
    context.already_attempted_this_attach = false;
    snprintf(
        context.target_part_number,
        sizeof(context.target_part_number),
        "AP3-SUB-004"
    );
    snprintf(
        context.target_serial,
        sizeof(context.target_serial),
        "SN-0001-ALPHA"
    );
    snprintf(
        context.attached_part_number,
        sizeof(context.attached_part_number),
        "AP3-SUB-004"
    );
    snprintf(
        context.attached_serial,
        sizeof(context.attached_serial),
        "SN-0001-ALPHA"
    );
    return context;
}

static void test_apply_allowed_when_everything_lines_up(void)
{
    const revlink_staged_map_apply_context_t context = ready_context();
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_ALLOWED,
        "a pinned map on its own device after a clean sync should apply"
    );
}

static void test_each_gate_blocks_independently(void)
{
    revlink_staged_map_apply_context_t context;

    context = ready_context();
    context.writes_compiled = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_WRITES_NOT_COMPILED,
        "compile gate must block"
    );

    context = ready_context();
    context.consent_enabled = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_CONSENT_DISABLED,
        "owner consent must block independently of the compile gate"
    );

    context = ready_context();
    context.auto_apply_enabled = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_AUTO_APPLY_DISABLED,
        "auto-apply preference must block even with consent enabled"
    );

    context = ready_context();
    context.staged = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_NOTHING_STAGED,
        "no staged payload must block"
    );

    context = ready_context();
    context.recovery_required = true;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_RECOVERY_REQUIRED,
        "a transport needing recovery must block"
    );

    context = ready_context();
    context.transfer_running = true;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_TRANSFER_BUSY,
        "an in-flight transfer must block"
    );

    context = ready_context();
    context.already_attempted_this_attach = true;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_ALREADY_ATTEMPTED,
        "only one automatic attempt is permitted per attach"
    );
}

static void test_wrong_device_never_receives_the_map(void)
{
    revlink_staged_map_apply_context_t context;

    /* Same part number, different car. This is the dangerous case. */
    context = ready_context();
    snprintf(
        context.attached_serial,
        sizeof(context.attached_serial),
        "SN-0002-BRAVO"
    );
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH,
        "a different serial must never receive a map staged for another"
    );

    /* Same serial string, different part number. */
    context = ready_context();
    snprintf(
        context.attached_part_number,
        sizeof(context.attached_part_number),
        "AP3-SUB-009"
    );
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH,
        "part number must also match"
    );

    /* A serial that is a prefix of the target must not match. */
    context = ready_context();
    snprintf(
        context.attached_serial,
        sizeof(context.attached_serial),
        "SN-0001"
    );
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH,
        "a prefix of the target serial must not be accepted"
    );

    /* No device identified yet. */
    context = ready_context();
    context.device_identified = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_NO_DEVICE,
        "an unidentified device must block"
    );

    /* Identified, but the identity handshake returned nothing usable. */
    context = ready_context();
    context.attached_serial[0] = '\0';
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_NO_DEVICE,
        "an empty attached serial must block"
    );

    /* An unpinned staged payload applies to nothing. */
    context = ready_context();
    context.target_serial[0] = '\0';
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_UNPINNED,
        "an unpinned staged payload must never auto-apply"
    );
}

static void test_incomplete_sync_defers_the_write(void)
{
    revlink_staged_map_apply_context_t context;

    context = ready_context();
    context.sync_completed_clean = false;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE,
        "a write must wait for a clean sync"
    );

    context = ready_context();
    context.sync_pending = 3U;
    CHECK(
        revlink_staged_map_evaluate_apply(&context)
            == REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE,
        "pending files mean the batch is not finished"
    );

    CHECK(
        revlink_staged_map_apply_decision_is_transient(
            REVLINK_STAGED_MAP_APPLY_SYNC_INCOMPLETE
        ),
        "an incomplete sync is a transient condition"
    );
    CHECK(
        !revlink_staged_map_apply_decision_is_transient(
            REVLINK_STAGED_MAP_APPLY_TARGET_MISMATCH
        ),
        "a target mismatch is a settled refusal, not transient"
    );
}

static void test_null_context_is_refused(void)
{
    CHECK(
        revlink_staged_map_evaluate_apply(NULL)
            != REVLINK_STAGED_MAP_APPLY_ALLOWED,
        "a NULL context must never authorize a write"
    );
}

static void test_decision_names_are_present(void)
{
    for (int i = 0; i <= (int)REVLINK_STAGED_MAP_APPLY_ALREADY_ATTEMPTED;
         ++i) {
        const char *name = revlink_staged_map_apply_decision_name(
            (revlink_staged_map_apply_decision_t)i
        );
        CHECK(
            name != NULL && strcmp(name, "unknown") != 0,
            "every decision should have a reportable name"
        );
    }
}

int main(void)
{
    test_round_trip();
    test_encoding_is_deterministic();
    test_corruption_is_rejected();
    test_unpinned_record_is_rejected();
    test_blanked_pin_with_valid_crc_is_rejected();
    test_apply_allowed_when_everything_lines_up();
    test_each_gate_blocks_independently();
    test_wrong_device_never_receives_the_map();
    test_incomplete_sync_defers_the_write();
    test_null_context_is_refused();
    test_decision_names_are_present();

    if (failures == 0U) {
        printf("staged map host test PASSED (%u checks)\n", checks);
        return 0;
    }
    printf("staged map host test FAILED (%u/%u checks)\n", failures, checks);
    return 1;
}
