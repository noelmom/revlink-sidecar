/*
 * Includes every public revlink_sync header in one translation unit.
 *
 * This is the whole point of the test: revlink_sync_manifest.h and
 * revlink_sync_coordinator.h each used to define a revlink_sync_status_t with
 * different enumerators, so any file that pulled in both failed to compile
 * with "redeclaration of enumerator" — blaming the header rather than the
 * collision. Nothing in the tree happened to include both, so the fault was
 * invisible until someone wrote a perfectly ordinary include and lost an hour
 * to it.
 *
 * A test that merely called the two APIs separately would still pass with the
 * collision present. Compiling them together is the only thing that proves it
 * gone, which is why this file exists mainly to be compiled at all.
 */

#include <stdio.h>
#include <string.h>

#include "revlink_sync_annotations.h"
#include "revlink_sync_coordinator.h"
#include "revlink_sync_history.h"
#include "revlink_sync_manifest.h"
#include "revlink_sync_presence.h"

static int failures;

static void check(int condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

int main(void)
{
    /*
     * Both success codes are reachable by name from here, and they are
     * distinct types. Before the rename these were one identifier that meant
     * two different things depending on which header won.
     */
    const revlink_sync_manifest_status_t store = REVLINK_SYNC_MANIFEST_OK;
    const revlink_sync_coordinator_status_t runtime =
        REVLINK_SYNC_COORDINATOR_OK;
    check(store == REVLINK_SYNC_MANIFEST_OK, "manifest OK is nameable");
    check(runtime == REVLINK_SYNC_COORDINATOR_OK, "coordinator OK is nameable");

    /* Each layer's error codes belong to that layer and say so. */
    check(
        REVLINK_SYNC_MANIFEST_INVALID_FORMAT
            != REVLINK_SYNC_MANIFEST_OK,
        "manifest has its own format error"
    );
    check(
        REVLINK_SYNC_COORDINATOR_TRANSPORT_ERROR
            != REVLINK_SYNC_COORDINATOR_OK,
        "coordinator has its own transport error"
    );

    /* The neighbouring enums that share the REVLINK_SYNC_ prefix must not
     * have been swept up by the rename: these are states and events, not
     * status codes, and they are used far more widely. */
    check(REVLINK_SYNC_IDLE == 0, "sync state enum is intact");
    check(REVLINK_SYNC_QUEUED != REVLINK_SYNC_RUNNING, "sync states are distinct");
    check(REVLINK_SYNC_EVENT_STARTED == 0, "sync event enum is intact");
    check(
        REVLINK_SYNC_ANNOTATION_OK == 0,
        "the annotation status enum is untouched"
    );
    check(
        REVLINK_SYNC_PRESENCE_UNKNOWN == 0,
        "presence still defaults to no evidence"
    );

    /* And the renamed helper still describes the layer it belongs to. */
    check(
        strcmp(revlink_sync_manifest_status_name(REVLINK_SYNC_MANIFEST_OK), "ok")
            == 0,
        "manifest status names still resolve"
    );
    check(
        revlink_sync_state_name(REVLINK_SYNC_IDLE) != NULL,
        "coordinator state names still resolve"
    );

    if (failures == 0) {
        printf("sync_header_pairing_test: all checks passed\n");
    }
    return failures == 0 ? 0 : 1;
}
