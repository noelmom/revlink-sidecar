#include "revlink_usb_control.h"

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    EVENT_DATA_OFF = 1,
    EVENT_DATA_ON,
    EVENT_VBUS_OFF,
    EVENT_VBUS_ON,
    EVENT_DELAY,
} event_kind_t;

typedef struct {
    event_kind_t kind;
    uint32_t value;
} event_t;

typedef struct {
    event_t events[24];
    size_t event_count;
    bool fail_next_data;
    bool fail_next_vbus;
} fake_driver_t;

static void record(
    fake_driver_t *driver,
    event_kind_t kind,
    uint32_t value
)
{
    assert(driver->event_count
        < sizeof(driver->events) / sizeof(driver->events[0]));
    driver->events[driver->event_count++] = (event_t){
        .kind = kind,
        .value = value,
    };
}

static bool set_data(void *context, bool enabled)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    record(
        driver,
        enabled ? EVENT_DATA_ON : EVENT_DATA_OFF,
        0U
    );
    if (driver->fail_next_data) {
        driver->fail_next_data = false;
        return false;
    }
    return true;
}

static bool set_vbus(void *context, bool enabled)
{
    fake_driver_t *driver = (fake_driver_t *)context;
    record(
        driver,
        enabled ? EVENT_VBUS_ON : EVENT_VBUS_OFF,
        0U
    );
    if (driver->fail_next_vbus) {
        driver->fail_next_vbus = false;
        return false;
    }
    return true;
}

static void delay_ms(void *context, uint32_t delay)
{
    record((fake_driver_t *)context, EVENT_DELAY, delay);
}

static revlink_usb_control_t make_control(fake_driver_t *driver)
{
    revlink_usb_control_t control;
    const revlink_usb_control_config_t config = {
        .driver_context = driver,
        .set_data_connected = set_data,
        .set_vbus_enabled = set_vbus,
        .delay = delay_ms,
        .vbus_settle_ms = 100U,
        .data_settle_ms = 20U,
        .isolate_settle_ms = 5U,
    };
    assert(
        revlink_usb_control_init(&control, &config)
            == REVLINK_USB_CONTROL_OK
    );
    return control;
}

static void assert_event(
    const fake_driver_t *driver,
    size_t index,
    event_kind_t kind,
    uint32_t value
)
{
    assert(index < driver->event_count);
    assert(driver->events[index].kind == kind);
    assert(driver->events[index].value == value);
}

static void test_invalid_configuration(void)
{
    revlink_usb_control_t control = {0};
    assert(
        revlink_usb_control_init(&control, NULL)
            == REVLINK_USB_CONTROL_INVALID_ARGUMENT
    );
    assert(
        revlink_usb_control_init(
            NULL,
            &(revlink_usb_control_config_t){0}
        ) == REVLINK_USB_CONTROL_INVALID_ARGUMENT
    );
    assert(
        revlink_usb_control_attach(
            &control,
            REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK
        ) == REVLINK_USB_CONTROL_INVALID_STATE
    );
}

static void test_initialization_is_fail_safe(void)
{
    fake_driver_t driver = {0};
    revlink_usb_control_t control = make_control(&driver);
    assert(driver.event_count == 3U);
    assert_event(&driver, 0U, EVENT_DATA_OFF, 0U);
    assert_event(&driver, 1U, EVENT_DELAY, 5U);
    assert_event(&driver, 2U, EVENT_VBUS_OFF, 0U);

    const revlink_usb_control_snapshot_t snapshot =
        revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_ISOLATED);
    assert(!snapshot.data_connected);
    assert(!snapshot.vbus_enabled);
}

static void test_supplied_vbus_attach_and_isolate_order(void)
{
    fake_driver_t driver = {0};
    revlink_usb_control_t control = make_control(&driver);
    driver.event_count = 0U;

    assert(
        revlink_usb_control_attach(
            &control,
            REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK
        ) == REVLINK_USB_CONTROL_OK
    );
    assert(driver.event_count == 4U);
    assert_event(&driver, 0U, EVENT_VBUS_ON, 0U);
    assert_event(&driver, 1U, EVENT_DELAY, 100U);
    assert_event(&driver, 2U, EVENT_DATA_ON, 0U);
    assert_event(&driver, 3U, EVENT_DELAY, 20U);

    revlink_usb_control_snapshot_t snapshot =
        revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_ATTACHED);
    assert(snapshot.data_connected);
    assert(snapshot.vbus_enabled);

    driver.event_count = 0U;
    assert(
        revlink_usb_control_isolate(&control)
            == REVLINK_USB_CONTROL_OK
    );
    assert(driver.event_count == 3U);
    assert_event(&driver, 0U, EVENT_DATA_OFF, 0U);
    assert_event(&driver, 1U, EVENT_DELAY, 5U);
    assert_event(&driver, 2U, EVENT_VBUS_OFF, 0U);

    snapshot = revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_ISOLATED);
    assert(!snapshot.data_connected);
    assert(!snapshot.vbus_enabled);
}

static void test_external_vbus_never_enables_supply(void)
{
    fake_driver_t driver = {0};
    revlink_usb_control_t control = make_control(&driver);
    driver.event_count = 0U;

    assert(
        revlink_usb_control_attach(
            &control,
            REVLINK_USB_VBUS_EXTERNALLY_POWERED
        ) == REVLINK_USB_CONTROL_OK
    );
    assert(driver.event_count == 3U);
    assert_event(&driver, 0U, EVENT_VBUS_OFF, 0U);
    assert_event(&driver, 1U, EVENT_DATA_ON, 0U);
    assert_event(&driver, 2U, EVENT_DELAY, 20U);

    const revlink_usb_control_snapshot_t snapshot =
        revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_ATTACHED);
    assert(snapshot.data_connected);
    assert(!snapshot.vbus_enabled);
    assert(
        snapshot.vbus_mode == REVLINK_USB_VBUS_EXTERNALLY_POWERED
    );
}

static void test_driver_failure_latches_fault_and_can_recover(void)
{
    fake_driver_t driver = {0};
    revlink_usb_control_t control = make_control(&driver);
    driver.event_count = 0U;
    driver.fail_next_data = true;

    assert(
        revlink_usb_control_attach(
            &control,
            REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK
        ) == REVLINK_USB_CONTROL_DRIVER_ERROR
    );
    revlink_usb_control_snapshot_t snapshot =
        revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_FAULT);
    assert(!snapshot.data_connected);
    assert(!snapshot.vbus_enabled);
    assert(
        revlink_usb_control_attach(
            &control,
            REVLINK_USB_VBUS_SUPPLIED_BY_REVLINK
        ) == REVLINK_USB_CONTROL_INVALID_STATE
    );

    assert(
        revlink_usb_control_force_isolate(&control)
            == REVLINK_USB_CONTROL_OK
    );
    snapshot = revlink_usb_control_snapshot(&control);
    assert(snapshot.state == REVLINK_USB_LINK_ISOLATED);
}

static void test_isolate_is_idempotent(void)
{
    fake_driver_t driver = {0};
    revlink_usb_control_t control = make_control(&driver);
    driver.event_count = 0U;
    assert(
        revlink_usb_control_isolate(&control)
            == REVLINK_USB_CONTROL_OK
    );
    assert(driver.event_count == 0U);
}

int main(void)
{
    test_invalid_configuration();
    test_initialization_is_fail_safe();
    test_supplied_vbus_attach_and_isolate_order();
    test_external_vbus_never_enables_supply();
    test_driver_failure_latches_fault_and_can_recover();
    test_isolate_is_idempotent();
    puts("revlink USB-control host tests passed");
    return 0;
}
