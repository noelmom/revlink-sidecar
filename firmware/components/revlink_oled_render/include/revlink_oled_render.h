#ifndef REVLINK_OLED_RENDER_H
#define REVLINK_OLED_RENDER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The 5x7 font and the framebuffer primitives that draw with it, separated
 * from the SPI panel driver so that what appears on the display can be
 * rendered and inspected on a host. Nothing here touches hardware, allocates,
 * or depends on ESP-IDF.
 */

#define REVLINK_OLED_WIDTH 128
#define REVLINK_OLED_HEIGHT 64
#define REVLINK_OLED_PAGES (REVLINK_OLED_HEIGHT / 8)
#define REVLINK_OLED_BUFFER_SIZE (REVLINK_OLED_WIDTH * REVLINK_OLED_PAGES)
#define REVLINK_OLED_SPLASH_MS 1500U

/* Glyphs are 5 columns wide and advance by 6, so n characters occupy
 * n * 6 * scale - scale pixels. At scale 1 that caps a full-width line at 21
 * characters, which is what the splash banner is bounded to. */
#define REVLINK_OLED_GLYPH_ADVANCE 6
#define REVLINK_OLED_SPLASH_BANNER_CAPACITY 22U

void revlink_oled_set_pixel(
    uint8_t *framebuffer,
    int x,
    int y,
    bool enabled
);

void revlink_oled_fill_rect(
    uint8_t *framebuffer,
    int x,
    int y,
    int width,
    int height,
    bool enabled
);

void revlink_oled_draw_rect(
    uint8_t *framebuffer,
    int x,
    int y,
    int width,
    int height
);

int revlink_oled_text_width(const char *text, int scale);

void revlink_oled_draw_text(
    uint8_t *framebuffer,
    int x,
    int y,
    const char *text,
    int scale
);

void revlink_oled_draw_centered(
    uint8_t *framebuffer,
    int y,
    const char *text,
    int scale
);

/*
 * True when the font has a glyph for this character. Anything else renders as
 * a blank, so a version string full of unknown punctuation would draw as a
 * run of gaps rather than fail — worth asserting against in tests.
 */
bool revlink_oled_glyph_known(char character);

/*
 * The boot splash. `version` is the running firmware version; NULL or empty
 * renders as UNKNOWN rather than as nothing. Long versions are truncated to
 * what the panel can show, never drawn off the edge.
 */
void revlink_oled_draw_splash(
    uint8_t *framebuffer,
    uint32_t elapsed_ms,
    const char *version
);

/*
 * The banner the splash draws for a given version, and the scale it will be
 * drawn at. Exposed so tests can assert the layout without reading pixels.
 */
void revlink_oled_splash_banner(
    const char *version,
    char *output,
    size_t capacity,
    int *scale
);

#ifdef __cplusplus
}
#endif

#endif
