#include <stdio.h>
#include <stdlib.h>

#include "revlink_time.h"

static uint64_t monotonic_ms;

static uint64_t read_monotonic(void *context)
{
    (void)context;
    return monotonic_ms;
}

static void require_true(bool condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAILED: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    revlink_time_service_t service;
    const revlink_time_config_t config = {
        .context = NULL,
        .monotonic_ms = read_monotonic,
    };
    require_true(revlink_time_init(&service, &config), "initialize");
    require_true(
        revlink_time_now(&service, NULL) == 0U,
        "time begins untrusted"
    );
    require_true(
        !revlink_time_observe(
            &service,
            REVLINK_TIME_NETWORK,
            REVLINK_TIME_MINIMUM_TRUSTED_UTC - 1U
        ),
        "reject implausible wall clock"
    );
    require_true(
        revlink_time_observe(
            &service,
            REVLINK_TIME_CLIENT,
            1785169000U
        ),
        "accept local client observation"
    );
    revlink_time_source_t source = REVLINK_TIME_UNTRUSTED;
    require_true(
        revlink_time_now(&service, &source) == 1785169000U
            && source == REVLINK_TIME_CLIENT,
        "report local client source"
    );
    monotonic_ms = 4000U;
    require_true(
        revlink_time_observe(
            &service,
            REVLINK_TIME_NETWORK,
            1785170000U
        ),
        "accept network observation"
    );
    monotonic_ms = 9500U;
    require_true(
        revlink_time_now(&service, &source) == 1785170005U
            && source == REVLINK_TIME_NETWORK,
        "advance trusted time with monotonic clock"
    );
    require_true(
        revlink_time_observe(&service, REVLINK_TIME_RTC, 1785171000U),
        "accept rtc observation"
    );
    require_true(
        revlink_time_now(&service, &source) == 1785171000U
            && source == REVLINK_TIME_RTC,
        "report observation source"
    );
    puts("trusted time tests PASSED");
    return 0;
}
