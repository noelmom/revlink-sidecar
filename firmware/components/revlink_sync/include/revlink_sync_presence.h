#ifndef REVLINK_SYNC_PRESENCE_H
#define REVLINK_SYNC_PRESENCE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Whether a cached file still exists on the AccessPort.
 *
 * Three states, not two, because "we did not see it" and "it is not there"
 * are different claims and only one of them is ever evidence. A listing that
 * was interrupted, refused, or never taken says nothing about what is on the
 * device, and reporting that as absence would be a lie the portal would then
 * repeat to the owner. UNKNOWN is deliberately zero so that a manifest
 * written before this field existed loads as "no evidence" rather than as an
 * accidental claim in either direction.
 *
 * This lives in a header of its own so that surfaces which only need to
 * report presence — the portal projection, for one — do not have to include
 * the whole manifest interface. revlink_sync_manifest.h and
 * revlink_sync_coordinator.h each define an unrelated revlink_sync_status_t,
 * and pulling both into one translation unit does not compile.
 */
typedef enum {
    REVLINK_SYNC_PRESENCE_UNKNOWN = 0,
    REVLINK_SYNC_PRESENCE_ON_DEVICE,
    REVLINK_SYNC_PRESENCE_ABSENT,
} revlink_sync_presence_t;

const char *revlink_sync_presence_name(revlink_sync_presence_t presence);

#ifdef __cplusplus
}
#endif

#endif
