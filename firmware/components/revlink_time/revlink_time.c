#include "revlink_time.h"

#include <limits.h>
#include <string.h>

bool revlink_time_init(
    revlink_time_service_t *service,
    const revlink_time_config_t *config
)
{
    if (service == NULL || config == NULL || config->monotonic_ms == NULL) {
        return false;
    }
    memset(service, 0, sizeof(*service));
    service->config = *config;
    atomic_init(&service->observed_utc, 0U);
    atomic_init(&service->observed_monotonic_ms, 0U);
    atomic_init(&service->source, REVLINK_TIME_UNTRUSTED);
    return true;
}

bool revlink_time_observe(
    revlink_time_service_t *service,
    revlink_time_source_t source,
    uint64_t utc_seconds
)
{
    if (service == NULL || service->config.monotonic_ms == NULL
        || source == REVLINK_TIME_UNTRUSTED
        || source > REVLINK_TIME_RTC
        || utc_seconds < REVLINK_TIME_MINIMUM_TRUSTED_UTC) {
        return false;
    }
    const uint64_t monotonic =
        service->config.monotonic_ms(service->config.context);
    atomic_store_explicit(
        &service->observed_monotonic_ms,
        monotonic,
        memory_order_relaxed
    );
    atomic_store_explicit(
        &service->observed_utc,
        utc_seconds,
        memory_order_relaxed
    );
    atomic_store_explicit(&service->source, source, memory_order_release);
    return true;
}

uint64_t revlink_time_now(
    const revlink_time_service_t *service,
    revlink_time_source_t *source
)
{
    if (source != NULL) *source = REVLINK_TIME_UNTRUSTED;
    if (service == NULL || service->config.monotonic_ms == NULL) return 0U;
    const revlink_time_source_t observed_source =
        (revlink_time_source_t)atomic_load_explicit(
            &service->source,
            memory_order_acquire
        );
    if (observed_source == REVLINK_TIME_UNTRUSTED) return 0U;
    const uint64_t utc = atomic_load_explicit(
        &service->observed_utc,
        memory_order_relaxed
    );
    const uint64_t anchor = atomic_load_explicit(
        &service->observed_monotonic_ms,
        memory_order_relaxed
    );
    const uint64_t monotonic =
        service->config.monotonic_ms(service->config.context);
    if (monotonic < anchor) return 0U;
    const uint64_t elapsed_seconds = (monotonic - anchor) / 1000U;
    if (utc > UINT64_MAX - elapsed_seconds) return 0U;
    if (source != NULL) *source = observed_source;
    return utc + elapsed_seconds;
}

const char *revlink_time_source_name(revlink_time_source_t source)
{
    switch (source) {
    case REVLINK_TIME_CLIENT:
        return "client";
    case REVLINK_TIME_NETWORK:
        return "network";
    case REVLINK_TIME_RTC:
        return "rtc";
    case REVLINK_TIME_UNTRUSTED:
    default:
        return "untrusted";
    }
}
