#include "revlink_status_oled.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_check.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_log_level.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "qrcode.h"
#include "revlink_oled_render.h"
#include "revlink_status_model.h"

/* One source of truth for the panel geometry: the renderer's. */
#define OLED_WIDTH REVLINK_OLED_WIDTH
#define OLED_HEIGHT REVLINK_OLED_HEIGHT
#define OLED_PAGES REVLINK_OLED_PAGES
#define OLED_BUFFER_SIZE REVLINK_OLED_BUFFER_SIZE
#define OLED_SPI_HOST SPI2_HOST
#define OLED_PIN_SCLK GPIO_NUM_23
#define OLED_PIN_MOSI GPIO_NUM_22
#define OLED_PIN_CS GPIO_NUM_21
#define OLED_PIN_DC GPIO_NUM_20
#define OLED_PIN_RESET GPIO_NUM_2
#define OLED_CLOCK_HZ (8 * 1000 * 1000)
#define OLED_TASK_PERIOD_MS 100U
#define OLED_SPLASH_MS REVLINK_OLED_SPLASH_MS
#define OLED_HEADER_CYCLE_TICKS 150U
#define OLED_SIDECAR_WINDOW_TICKS 30U
#define OLED_CONNECTED_SSID_CAPACITY 33U
#define OLED_NETWORK_BADGE_CAPACITY 13U
#define OLED_NETWORK_BADGE_VISIBLE_CHARS 12U
#define OLED_QR_TIMEOUT_MS 30000U
#define OLED_LOCAL_URL_TIMEOUT_MS 20000U
#define OLED_QR_MAX_VERSION 4
#define OLED_QR_MAX_MODULES 33
#define OLED_QR_BITMAP_SIZE \
    ((OLED_QR_MAX_MODULES * OLED_QR_MAX_MODULES + 7) / 8)

#if CONFIG_REVLINK_STATUS_OLED_ROTATE_180
#define OLED_SEGMENT_REMAP_COMMAND 0xa0
#define OLED_COM_SCAN_COMMAND 0xc0
#else
#define OLED_SEGMENT_REMAP_COMMAND 0xa1
#define OLED_COM_SCAN_COMMAND 0xc8
#endif

static const char *TAG = "revlink_oled";
static spi_device_handle_t oled_spi;
static uint8_t framebuffer[OLED_BUFFER_SIZE];
static uint8_t previous_framebuffer[OLED_BUFFER_SIZE];
static revlink_status_model_t status_model;
static portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
static bool oled_ready;
static bool hotspot_visible;
static char hotspot_ssid[20];
static char hotspot_password[16];
static bool hotspot_qr_visible;
static int64_t hotspot_qr_started_us;
static uint8_t hotspot_qr_size;
static uint8_t hotspot_qr_bitmap[OLED_QR_BITMAP_SIZE];
static bool local_url_visible;
static int64_t local_url_started_us;
static char local_url_hostname[64];
static char connected_network_ssid[OLED_CONNECTED_SSID_CAPACITY];
static revlink_oled_storage_state_t storage_display_state =
    REVLINK_OLED_STORAGE_NORMAL;
static uint32_t storage_format_seconds;

typedef struct {
    uint8_t size;
    uint8_t bitmap[OLED_QR_BITMAP_SIZE];
} hotspot_qr_render_t;


/*
 * The font and the primitives that draw with it live in revlink_oled_render,
 * where they can be rendered and inspected on a host. These bind them to this
 * module's framebuffer so the drawing code below reads as it always has.
 */
static void set_pixel(int x, int y, bool enabled)
{
    revlink_oled_set_pixel(framebuffer, x, y, enabled);
}

static void fill_rect(int x, int y, int width, int height, bool enabled)
{
    revlink_oled_fill_rect(framebuffer, x, y, width, height, enabled);
}

static void draw_rect(int x, int y, int width, int height)
{
    revlink_oled_draw_rect(framebuffer, x, y, width, height);
}

static int text_width(const char *text, int scale)
{
    return revlink_oled_text_width(text, scale);
}

static void draw_text(int x, int y, const char *text, int scale)
{
    revlink_oled_draw_text(framebuffer, x, y, text, scale);
}

static void draw_centered(int y, const char *text, int scale)
{
    revlink_oled_draw_centered(framebuffer, y, text, scale);
}

static void draw_splash(uint32_t elapsed_ms)
{
    const esp_app_desc_t *description = esp_app_get_description();
    revlink_oled_draw_splash(
        framebuffer,
        elapsed_ms,
        description != NULL ? description->version : NULL
    );
}

static void draw_wifi_icon(int x, int y)
{
    set_pixel(x, y + 2, true);
    set_pixel(x + 1, y + 1, true);
    fill_rect(x + 2, y, 3, 1, true);
    set_pixel(x + 5, y + 1, true);
    set_pixel(x + 6, y + 2, true);

    set_pixel(x + 1, y + 4, true);
    fill_rect(x + 2, y + 3, 3, 1, true);
    set_pixel(x + 5, y + 4, true);

    set_pixel(x + 3, y + 6, true);
}

static void draw_status(
    const revlink_status_view_t *view,
    uint32_t animation_tick,
    const char *network_badge
)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    const uint32_t header_phase =
        animation_tick % OLED_HEADER_CYCLE_TICKS;
    const char *header =
        header_phase
                >= OLED_HEADER_CYCLE_TICKS - OLED_SIDECAR_WINDOW_TICKS
            ? "SIDECAR"
            : "REVLINK";
    draw_text(2, 3, header, 1);
    if (network_badge != NULL && network_badge[0] != '\0') {
        const int badge_text_width = text_width(network_badge, 1);
        const int badge_x = OLED_WIDTH - badge_text_width - 11;
        draw_wifi_icon(badge_x, 3);
        draw_text(badge_x + 9, 3, network_badge, 1);
    }
    fill_rect(2, 12, 124, 1, true);

    const int headline_scale = text_width(view->headline, 2) <= 124 ? 2 : 1;
    draw_centered(18, view->headline, headline_scale);
    char detail[24] = {0};
    const char *detail_text = view->detail;
    if (view->kind == REVLINK_STATUS_WIFI_RECONNECTING) {
        (void)snprintf(
            detail,
            sizeof(detail),
            "RETRY %u SEC",
            (unsigned int)view->countdown_seconds
        );
        detail_text = detail;
    }
    draw_centered(headline_scale == 2 ? 35 : 30, detail_text, 1);

    if (view->kind == REVLINK_STATUS_READY) {
        draw_centered(42, "READY", 2);
        draw_centered(57, "TO SYNC", 1);
        return;
    }

    if (view->show_progress) {
        draw_rect(8, 46, 112, 8);
        if (view->progress_indeterminate) {
            const int travel = 94;
            const int position = (int)(animation_tick % (uint32_t)(travel * 2));
            const int x = position <= travel ? position : travel * 2 - position;
            fill_rect(10 + x, 48, 16, 4, true);
        } else {
            const int width =
                (int)((uint32_t)view->progress_percent * 108U / 100U);
            fill_rect(10, 48, width, 4, true);
        }
        draw_centered(57, view->footer, 1);
        return;
    }

    draw_centered(51, view->footer, 1);
}

static void exact_copy(char *target, size_t capacity, const char *source);

static void network_badge(
    revlink_network_state_t state,
    const char *connected_ssid,
    char *badge,
    size_t badge_capacity
)
{
    if (badge == NULL || badge_capacity == 0U) {
        return;
    }
    badge[0] = '\0';
    switch (state) {
    case REVLINK_NETWORK_HOTSPOT_STARTING:
    case REVLINK_NETWORK_HOTSPOT_READY:
        exact_copy(badge, badge_capacity, "LOCAL");
        return;
    case REVLINK_NETWORK_CLIENT_READY: {
        if (connected_ssid == NULL || connected_ssid[0] == '\0') {
            exact_copy(badge, badge_capacity, "WIFI");
            return;
        }
        const size_t length = strnlen(
            connected_ssid,
            OLED_CONNECTED_SSID_CAPACITY
        );
        if (length <= OLED_NETWORK_BADGE_VISIBLE_CHARS) {
            exact_copy(badge, badge_capacity, connected_ssid);
            return;
        }
        const size_t prefix_length = OLED_NETWORK_BADGE_VISIBLE_CHARS - 3U;
        memcpy(badge, connected_ssid, prefix_length);
        memcpy(badge + prefix_length, "...", 4U);
        return;
    }
    case REVLINK_NETWORK_SEARCHING:
    case REVLINK_NETWORK_CONNECTING:
    case REVLINK_NETWORK_RECONNECTING:
        exact_copy(badge, badge_capacity, "WIFI");
        return;
    default:
        return;
    }
}

static void exact_copy(char *target, size_t capacity, const char *source)
{
    if (target == NULL || capacity == 0U) {
        return;
    }
    size_t index = 0U;
    if (source != NULL) {
        for (; index + 1U < capacity && source[index] != '\0'; ++index) {
            target[index] = source[index];
        }
    }
    target[index] = '\0';
}

static void draw_hotspot(const char *ssid, const char *password)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_text(2, 2, "REVLINK", 1);
    draw_text(76, 2, "SETUP", 1);
    fill_rect(2, 12, 124, 1, true);
    draw_centered(18, ssid, 1);
    draw_centered(31, "PASSWORD", 1);
    draw_rect(15, 41, 98, 17);
    draw_centered(46, password, 1);
}

static void draw_local_url(const char *hostname)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_text(2, 2, "REVLINK", 1);
    draw_text(100, 2, "WIFI", 1);
    fill_rect(2, 12, 124, 1, true);
    draw_centered(17, "OPEN IN BROWSER", 1);
    draw_centered(31, hostname, 1);
    draw_centered(43, ".local", 1);
    draw_centered(56, "PRESS TO CLOSE", 1);
}

static void draw_storage_recovery(
    revlink_oled_storage_state_t state,
    uint32_t seconds_remaining
)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    draw_text(2, 2, "REVLINK", 1);
    draw_text(82, 2, "STORAGE", 1);
    fill_rect(2, 12, 124, 1, true);

    switch (state) {
    case REVLINK_OLED_STORAGE_MISSING:
        draw_centered(18, "SD CARD MISSING", 1);
        draw_centered(33, "INSERT A CARD", 1);
        draw_centered(49, "THEN RESTART", 1);
        break;
    case REVLINK_OLED_STORAGE_UNREADABLE:
        draw_centered(17, "SD UNREADABLE", 1);
        draw_centered(31, "DOUBLE-PRESS BOOT", 1);
        draw_centered(47, "TO FORMAT", 2);
        break;
    case REVLINK_OLED_STORAGE_CONFIRM_FORMAT: {
        char countdown[24] = {0};
        (void)snprintf(
            countdown,
            sizeof(countdown),
            "CONFIRM IN %u SEC",
            (unsigned int)seconds_remaining
        );
        draw_centered(16, "ERASE SD CARD?", 1);
        draw_centered(29, "ALL DATA WILL BE LOST", 1);
        draw_centered(42, "DOUBLE-PRESS BOOT", 1);
        draw_centered(55, countdown, 1);
        break;
    }
    case REVLINK_OLED_STORAGE_FORMATTING:
        draw_centered(20, "FORMATTING SD", 2);
        draw_centered(44, "DO NOT POWER OFF", 1);
        break;
    case REVLINK_OLED_STORAGE_FORMAT_COMPLETE:
        draw_centered(18, "FORMAT COMPLETE", 1);
        draw_centered(34, "STORAGE READY", 2);
        draw_centered(56, "RESTARTING", 1);
        break;
    case REVLINK_OLED_STORAGE_FORMAT_FAILED:
        draw_centered(18, "FORMAT FAILED", 2);
        draw_centered(43, "CHECK SD CARD", 1);
        draw_centered(55, "THEN TRY AGAIN", 1);
        break;
    case REVLINK_OLED_STORAGE_ERROR:
    default:
        draw_centered(18, "STORAGE ERROR", 2);
        draw_centered(43, "CHECK SD CARD", 1);
        draw_centered(55, "THEN RESTART", 1);
        break;
    }
}

static bool qr_bitmap_module(
    const uint8_t *bitmap,
    uint8_t size,
    int x,
    int y
)
{
    if (bitmap == NULL || x < 0 || y < 0 || x >= size || y >= size) {
        return false;
    }
    const size_t bit = (size_t)y * size + (size_t)x;
    return (bitmap[bit / 8U] & (uint8_t)(1U << (bit % 8U))) != 0U;
}

static void draw_hotspot_qr(const uint8_t *bitmap, uint8_t size)
{
    memset(framebuffer, 0, sizeof(framebuffer));
    if (bitmap == NULL || size == 0U) {
        return;
    }

    /*
     * A version-3 Wi-Fi symbol is 29 modules. At 2x it uses nearly the full
     * display height and scans much more reliably than 1px modules. The OLED
     * background supplies a one-module light quiet zone. Larger symbols fall
     * back to the QR-standard four-module quiet zone at 1x.
     */
    const int scale = (int)size * 2 + 4 <= OLED_HEIGHT ? 2 : 1;
    const int quiet_modules = scale == 2 ? 1 : 4;
    const int side = ((int)size + quiet_modules * 2) * scale;
    const int origin_x = (OLED_WIDTH - side) / 2;
    const int origin_y = (OLED_HEIGHT - side) / 2;

    fill_rect(origin_x, origin_y, side, side, true);
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (qr_bitmap_module(bitmap, size, x, y)) {
                fill_rect(
                    origin_x + (x + quiet_modules) * scale,
                    origin_y + (y + quiet_modules) * scale,
                    scale,
                    scale,
                    false
                );
            }
        }
    }
}

static void capture_qr(
    esp_qrcode_handle_t qrcode,
    void *user_data
)
{
    hotspot_qr_render_t *render = user_data;
    if (qrcode == NULL || render == NULL) {
        return;
    }
    const int size = esp_qrcode_get_size(qrcode);
    if (size <= 0 || size > OLED_QR_MAX_MODULES) {
        return;
    }

    render->size = (uint8_t)size;
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            if (!esp_qrcode_get_module(qrcode, x, y)) {
                continue;
            }
            const size_t bit = (size_t)y * (size_t)size + (size_t)x;
            render->bitmap[bit / 8U] |=
                (uint8_t)(1U << (bit % 8U));
        }
    }
}

static bool append_qr_escaped(
    char *target,
    size_t capacity,
    size_t *length,
    const char *source
)
{
    if (target == NULL || length == NULL || source == NULL) {
        return false;
    }
    for (size_t index = 0U; source[index] != '\0'; ++index) {
        const char character = source[index];
        if (character == '\\' || character == ';'
            || character == ',' || character == ':') {
            if (*length + 2U >= capacity) {
                return false;
            }
            target[(*length)++] = '\\';
        } else if (*length + 1U >= capacity) {
            return false;
        }
        target[(*length)++] = character;
    }
    target[*length] = '\0';
    return true;
}

static esp_err_t oled_transmit(bool data, const uint8_t *bytes, size_t length)
{
    gpio_set_level(OLED_PIN_DC, data ? 1 : 0);
    spi_transaction_t transaction = {
        .length = length * 8U,
        .tx_buffer = bytes,
    };
    return spi_device_polling_transmit(oled_spi, &transaction);
}

static esp_err_t oled_command(uint8_t command)
{
    return oled_transmit(false, &command, 1U);
}

static esp_err_t oled_reset_and_configure(void)
{
    gpio_set_level(OLED_PIN_RESET, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(OLED_PIN_RESET, 1);
    vTaskDelay(pdMS_TO_TICKS(20));

    static const uint8_t init_commands[] = {
        0xae, 0xd5, 0x80, 0xa8, 0x3f, 0xd3, 0x00, 0x40,
        0xad, 0x8b, OLED_SEGMENT_REMAP_COMMAND, OLED_COM_SCAN_COMMAND,
        0xda, 0x12, 0x81, 0x9f,
        0xd9, 0x22, 0xdb, 0x35, 0xa4, 0xa6, 0xaf,
    };
    for (size_t index = 0U; index < sizeof(init_commands); ++index) {
        ESP_RETURN_ON_ERROR(oled_command(init_commands[index]), TAG, "OLED init");
    }
    return ESP_OK;
}

static esp_err_t oled_flush(void)
{
    for (uint8_t page = 0U; page < OLED_PAGES; ++page) {
        ESP_RETURN_ON_ERROR(oled_command((uint8_t)(0xb0U + page)), TAG, "");
        ESP_RETURN_ON_ERROR(oled_command(0x02U), TAG, "");
        ESP_RETURN_ON_ERROR(oled_command(0x10U), TAG, "");
        ESP_RETURN_ON_ERROR(
            oled_transmit(
                true,
                framebuffer + (size_t)page * OLED_WIDTH,
                OLED_WIDTH
            ),
            TAG,
            ""
        );
    }
    return ESP_OK;
}

static esp_err_t oled_initialize(void)
{
    const gpio_config_t output_config = {
        .pin_bit_mask = (1ULL << OLED_PIN_DC) | (1ULL << OLED_PIN_RESET),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&output_config), TAG, "GPIO init");

    const spi_bus_config_t bus_config = {
        .mosi_io_num = OLED_PIN_MOSI,
        .miso_io_num = -1,
        .sclk_io_num = OLED_PIN_SCLK,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = OLED_WIDTH,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_initialize(OLED_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO),
        TAG,
        "SPI bus init"
    );
    const spi_device_interface_config_t device_config = {
        .clock_speed_hz = OLED_CLOCK_HZ,
        .mode = 0,
        .spics_io_num = OLED_PIN_CS,
        .queue_size = 1,
    };
    ESP_RETURN_ON_ERROR(
        spi_bus_add_device(OLED_SPI_HOST, &device_config, &oled_spi),
        TAG,
        "SPI device init"
    );

    ESP_RETURN_ON_ERROR(
        oled_reset_and_configure(),
        TAG,
        "OLED controller init"
    );
    memset(framebuffer, 0, sizeof(framebuffer));
    memset(previous_framebuffer, 0xff, sizeof(previous_framebuffer));
    return oled_flush();
}

static void oled_task(void *context)
{
    (void)context;
    const int64_t started_us = esp_timer_get_time();
    uint32_t tick = 0U;

    while (true) {
        const uint32_t elapsed_ms =
            (uint32_t)((esp_timer_get_time() - started_us) / 1000);
        if (elapsed_ms < OLED_SPLASH_MS) {
            draw_splash(elapsed_ms);
        } else {
            revlink_status_model_t snapshot;
            bool show_hotspot;
            bool show_hotspot_qr;
            bool show_local_url;
            revlink_oled_storage_state_t storage_state;
            uint32_t format_seconds;
            char ssid[sizeof(hotspot_ssid)];
            char password[sizeof(hotspot_password)];
            char hostname[sizeof(local_url_hostname)];
            char station_ssid[sizeof(connected_network_ssid)];
            char badge[OLED_NETWORK_BADGE_CAPACITY] = {0};
            uint8_t qr_size;
            uint8_t qr_bitmap[sizeof(hotspot_qr_bitmap)];
            portENTER_CRITICAL(&status_lock);
            snapshot = status_model;
            if (hotspot_qr_visible
                && (esp_timer_get_time() - hotspot_qr_started_us) / 1000
                    >= OLED_QR_TIMEOUT_MS) {
                hotspot_qr_visible = false;
            }
            if (local_url_visible
                && (esp_timer_get_time() - local_url_started_us) / 1000
                    >= OLED_LOCAL_URL_TIMEOUT_MS) {
                local_url_visible = false;
            }
            show_hotspot = hotspot_visible;
            show_hotspot_qr = hotspot_qr_visible;
            show_local_url = local_url_visible;
            storage_state = storage_display_state;
            format_seconds = storage_format_seconds;
            memcpy(ssid, hotspot_ssid, sizeof(ssid));
            memcpy(password, hotspot_password, sizeof(password));
            memcpy(hostname, local_url_hostname, sizeof(hostname));
            memcpy(
                station_ssid,
                connected_network_ssid,
                sizeof(station_ssid)
            );
            qr_size = hotspot_qr_size;
            memcpy(qr_bitmap, hotspot_qr_bitmap, sizeof(qr_bitmap));
            portEXIT_CRITICAL(&status_lock);
            if (storage_state != REVLINK_OLED_STORAGE_NORMAL) {
                draw_storage_recovery(storage_state, format_seconds);
            } else if (show_hotspot_qr) {
                draw_hotspot_qr(qr_bitmap, qr_size);
            } else if (show_hotspot) {
                draw_hotspot(ssid, password);
            } else if (show_local_url) {
                draw_local_url(hostname);
            } else {
                const revlink_status_view_t view =
                    revlink_status_model_view(&snapshot);
                network_badge(
                    snapshot.network.state,
                    station_ssid,
                    badge,
                    sizeof(badge)
                );
                draw_status(&view, tick, badge);
            }
            memset(password, 0, sizeof(password));
            memset(qr_bitmap, 0, sizeof(qr_bitmap));
        }

        const bool frame_changed =
            memcmp(framebuffer, previous_framebuffer, sizeof(framebuffer)) != 0;
        if (frame_changed) {
            const esp_err_t refresh_status = oled_flush();
            if (refresh_status == ESP_OK) {
                memcpy(
                    previous_framebuffer,
                    framebuffer,
                    sizeof(previous_framebuffer)
                );
            }
        }
        ++tick;
        vTaskDelay(pdMS_TO_TICKS(OLED_TASK_PERIOD_MS));
    }
}

esp_err_t revlink_status_oled_start(void)
{
    if (oled_ready) {
        return ESP_OK;
    }
    revlink_status_model_init(&status_model);
    ESP_RETURN_ON_ERROR(oled_initialize(), TAG, "display init");
    if (xTaskCreate(oled_task, "revlink_oled", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    oled_ready = true;
    ESP_LOGI(
        TAG,
        "SH1106 status display ready: SCLK=%d MOSI=%d CS=%d DC=%d RESET=%d",
        OLED_PIN_SCLK,
        OLED_PIN_MOSI,
        OLED_PIN_CS,
        OLED_PIN_DC,
        OLED_PIN_RESET
    );
    return ESP_OK;
}

void revlink_status_oled_boot_complete(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_boot_complete(&status_model, true);
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_update_device(
    const revlink_device_snapshot_t *snapshot
)
{
    if (!oled_ready || snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_device(&status_model, snapshot);
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_update_sync(
    const revlink_sync_snapshot_t *snapshot
)
{
    if (!oled_ready || snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_sync(&status_model, snapshot);
    if (
        snapshot->state == REVLINK_SYNC_QUEUED
        || snapshot->state == REVLINK_SYNC_RUNNING
        || snapshot->state == REVLINK_SYNC_CANCELLING
    ) {
        local_url_visible = false;
    }
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_update_network(
    const revlink_network_snapshot_t *snapshot,
    const char *connected_ssid
)
{
    if (!oled_ready || snapshot == NULL) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_network(&status_model, snapshot);
    if (
        snapshot->state == REVLINK_NETWORK_CLIENT_READY
        && connected_ssid != NULL
    ) {
        exact_copy(
            connected_network_ssid,
            sizeof(connected_network_ssid),
            connected_ssid
        );
    } else {
        connected_network_ssid[0] = '\0';
    }
    if (snapshot->state != REVLINK_NETWORK_CLIENT_READY) {
        local_url_visible = false;
    }
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_set_vehicle(const char *vehicle)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_vehicle(&status_model, vehicle);
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_set_accessport_identity(
    const char *vehicle,
    const char *part_number
)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    revlink_status_model_set_vehicle(&status_model, vehicle);
    revlink_status_model_set_part_number(&status_model, part_number);
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_show_hotspot(
    const char *ssid,
    const char *password
)
{
    if (!oled_ready || ssid == NULL || password == NULL) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    exact_copy(hotspot_ssid, sizeof(hotspot_ssid), ssid);
    exact_copy(
        hotspot_password,
        sizeof(hotspot_password),
        password
    );
    hotspot_visible = true;
    hotspot_qr_visible = false;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_hide_hotspot(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    hotspot_visible = false;
    hotspot_qr_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_restore_hotspot(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    hotspot_visible =
        hotspot_ssid[0] != '\0' && hotspot_password[0] != '\0';
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_clear_hotspot(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    memset(hotspot_password, 0, sizeof(hotspot_password));
    memset(hotspot_ssid, 0, sizeof(hotspot_ssid));
    memset(hotspot_qr_bitmap, 0, sizeof(hotspot_qr_bitmap));
    hotspot_visible = false;
    hotspot_qr_visible = false;
    hotspot_qr_size = 0U;
    hotspot_qr_started_us = 0;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

esp_err_t revlink_status_oled_show_hotspot_qr(void)
{
    if (!oled_ready) {
        return ESP_ERR_INVALID_STATE;
    }

    char ssid[sizeof(hotspot_ssid)] = {0};
    char password[sizeof(hotspot_password)] = {0};
    revlink_network_state_t network_state;
    portENTER_CRITICAL(&status_lock);
    exact_copy(ssid, sizeof(ssid), hotspot_ssid);
    exact_copy(password, sizeof(password), hotspot_password);
    network_state = status_model.network.state;
    portEXIT_CRITICAL(&status_lock);

    if (ssid[0] == '\0' || password[0] == '\0'
        || (network_state != REVLINK_NETWORK_HOTSPOT_STARTING
            && network_state != REVLINK_NETWORK_HOTSPOT_READY)) {
        memset(password, 0, sizeof(password));
        return ESP_ERR_INVALID_STATE;
    }

    char payload[96] = "WIFI:T:WPA;S:";
    size_t length = strlen(payload);
    bool payload_ok = append_qr_escaped(
        payload,
        sizeof(payload),
        &length,
        ssid
    );
    if (payload_ok && length + 3U < sizeof(payload)) {
        memcpy(payload + length, ";P:", 3U);
        length += 3U;
        payload[length] = '\0';
    } else {
        payload_ok = false;
    }
    if (payload_ok) {
        payload_ok = append_qr_escaped(
            payload,
            sizeof(payload),
            &length,
            password
        );
    }
    if (payload_ok && length + 2U < sizeof(payload)) {
        memcpy(payload + length, ";;", 3U);
    } else {
        payload_ok = false;
    }
    if (!payload_ok) {
        memset(password, 0, sizeof(password));
        memset(payload, 0, sizeof(payload));
        return ESP_ERR_INVALID_SIZE;
    }

    hotspot_qr_render_t render = {0};
    esp_qrcode_config_t qr_config = {
        .display_func_with_cb = capture_qr,
        .max_qrcode_version = OLED_QR_MAX_VERSION,
        .qrcode_ecc_level = ESP_QRCODE_ECC_LOW,
        .user_data = &render,
    };

    /*
     * The upstream generator logs its input at INFO level. Suppress that tag
     * around this synchronous call so the temporary Wi-Fi credential can
     * never enter logs.
     */
    const esp_log_level_t previous_qr_log_level =
        esp_log_level_get("QRCODE");
    esp_log_level_set("QRCODE", ESP_LOG_NONE);
    const esp_err_t qr_status = esp_qrcode_generate(&qr_config, payload);
    esp_log_level_set("QRCODE", previous_qr_log_level);
    memset(password, 0, sizeof(password));
    memset(payload, 0, sizeof(payload));

    if (qr_status != ESP_OK || render.size == 0U) {
        memset(&render, 0, sizeof(render));
        return qr_status == ESP_OK ? ESP_FAIL : qr_status;
    }

    portENTER_CRITICAL(&status_lock);
    memcpy(hotspot_qr_bitmap, render.bitmap, sizeof(hotspot_qr_bitmap));
    hotspot_qr_size = render.size;
    hotspot_qr_started_us = esp_timer_get_time();
    hotspot_visible = true;
    hotspot_qr_visible = true;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
    memset(&render, 0, sizeof(render));
    return ESP_OK;
}

void revlink_status_oled_hide_hotspot_qr(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    hotspot_qr_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

bool revlink_status_oled_hotspot_qr_visible(void)
{
    if (!oled_ready) {
        return false;
    }
    portENTER_CRITICAL(&status_lock);
    const bool visible = hotspot_qr_visible;
    portEXIT_CRITICAL(&status_lock);
    return visible;
}

esp_err_t revlink_status_oled_show_local_url(const char *hostname)
{
    if (!oled_ready || hostname == NULL || hostname[0] == '\0') {
        return ESP_ERR_INVALID_STATE;
    }

    portENTER_CRITICAL(&status_lock);
    const bool network_ready =
        status_model.network.state == REVLINK_NETWORK_CLIENT_READY;
    const bool sync_active =
        status_model.sync.state == REVLINK_SYNC_QUEUED
        || status_model.sync.state == REVLINK_SYNC_RUNNING
        || status_model.sync.state == REVLINK_SYNC_CANCELLING;
    if (network_ready && !sync_active) {
        exact_copy(
            local_url_hostname,
            sizeof(local_url_hostname),
            hostname
        );
        local_url_started_us = esp_timer_get_time();
        local_url_visible = true;
        hotspot_qr_visible = false;
    }
    portEXIT_CRITICAL(&status_lock);
    return network_ready && !sync_active ? ESP_OK : ESP_ERR_INVALID_STATE;
}

void revlink_status_oled_hide_local_url(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

bool revlink_status_oled_local_url_visible(void)
{
    if (!oled_ready) {
        return false;
    }
    portENTER_CRITICAL(&status_lock);
    const bool visible = local_url_visible;
    portEXIT_CRITICAL(&status_lock);
    return visible;
}

void revlink_status_oled_show_storage_error(
    revlink_oled_storage_state_t state
)
{
    if (!oled_ready || state == REVLINK_OLED_STORAGE_NORMAL
        || state == REVLINK_OLED_STORAGE_CONFIRM_FORMAT
        || state == REVLINK_OLED_STORAGE_FORMATTING) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    storage_display_state = state;
    storage_format_seconds = 0U;
    hotspot_qr_visible = false;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_show_storage_format_warning(
    uint32_t seconds_remaining
)
{
    if (!oled_ready || seconds_remaining == 0U) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    storage_display_state = REVLINK_OLED_STORAGE_CONFIRM_FORMAT;
    storage_format_seconds = seconds_remaining;
    hotspot_qr_visible = false;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_show_storage_formatting(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    storage_display_state = REVLINK_OLED_STORAGE_FORMATTING;
    storage_format_seconds = 0U;
    hotspot_qr_visible = false;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_show_storage_format_complete(void)
{
    if (!oled_ready) {
        return;
    }
    portENTER_CRITICAL(&status_lock);
    storage_display_state = REVLINK_OLED_STORAGE_FORMAT_COMPLETE;
    storage_format_seconds = 0U;
    hotspot_qr_visible = false;
    local_url_visible = false;
    portEXIT_CRITICAL(&status_lock);
}

void revlink_status_oled_show_storage_format_failed(void)
{
    revlink_status_oled_show_storage_error(
        REVLINK_OLED_STORAGE_FORMAT_FAILED
    );
}
