#ifndef REVLINK_TIME_H
#define REVLINK_TIME_H

#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define REVLINK_TIME_MINIMUM_TRUSTED_UTC 1577836800ULL

typedef enum {
    REVLINK_TIME_UNTRUSTED = 0,
    REVLINK_TIME_CLIENT,
    REVLINK_TIME_NETWORK,
    REVLINK_TIME_RTC,
} revlink_time_source_t;

typedef uint64_t (*revlink_time_monotonic_ms_t)(void *context);

typedef struct {
    void *context;
    revlink_time_monotonic_ms_t monotonic_ms;
} revlink_time_config_t;

typedef struct {
    revlink_time_config_t config;
    atomic_uint_fast64_t observed_utc;
    atomic_uint_fast64_t observed_monotonic_ms;
    atomic_int source;
} revlink_time_service_t;

bool revlink_time_init(
    revlink_time_service_t *service,
    const revlink_time_config_t *config
);

/*
 * Accept a wall-clock observation only from an explicit trusted source.
 * A later observation replaces the previous anchor.
 */
bool revlink_time_observe(
    revlink_time_service_t *service,
    revlink_time_source_t source,
    uint64_t utc_seconds
);

/*
 * Return the current trusted UTC time. Zero means no trusted wall-clock
 * observation has been made since boot.
 */
uint64_t revlink_time_now(
    const revlink_time_service_t *service,
    revlink_time_source_t *source
);

const char *revlink_time_source_name(revlink_time_source_t source);

#ifdef __cplusplus
}
#endif

#endif
