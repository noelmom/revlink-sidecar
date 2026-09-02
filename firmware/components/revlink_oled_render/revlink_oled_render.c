#include "revlink_oled_render.h"

#include <stdio.h>
#include <string.h>

typedef struct {
    char character;
    uint8_t columns[5];
} glyph_t;

static const glyph_t font[] = {
    {' ', {0x00, 0x00, 0x00, 0x00, 0x00}},
    {'-', {0x08, 0x08, 0x08, 0x08, 0x08}},
    {'.', {0x00, 0x60, 0x60, 0x00, 0x00}},
    {'0', {0x3e, 0x51, 0x49, 0x45, 0x3e}},
    {'1', {0x00, 0x42, 0x7f, 0x40, 0x00}},
    {'2', {0x42, 0x61, 0x51, 0x49, 0x46}},
    {'3', {0x21, 0x41, 0x45, 0x4b, 0x31}},
    {'4', {0x18, 0x14, 0x12, 0x7f, 0x10}},
    {'5', {0x27, 0x45, 0x45, 0x45, 0x39}},
    {'6', {0x3c, 0x4a, 0x49, 0x49, 0x30}},
    {'7', {0x01, 0x71, 0x09, 0x05, 0x03}},
    {'8', {0x36, 0x49, 0x49, 0x49, 0x36}},
    {'9', {0x06, 0x49, 0x49, 0x29, 0x1e}},
    {'A', {0x7e, 0x11, 0x11, 0x11, 0x7e}},
    {'B', {0x7f, 0x49, 0x49, 0x49, 0x36}},
    {'C', {0x3e, 0x41, 0x41, 0x41, 0x22}},
    {'D', {0x7f, 0x41, 0x41, 0x22, 0x1c}},
    {'E', {0x7f, 0x49, 0x49, 0x49, 0x41}},
    {'F', {0x7f, 0x09, 0x09, 0x09, 0x01}},
    {'G', {0x3e, 0x41, 0x49, 0x49, 0x7a}},
    {'H', {0x7f, 0x08, 0x08, 0x08, 0x7f}},
    {'I', {0x00, 0x41, 0x7f, 0x41, 0x00}},
    {'J', {0x20, 0x40, 0x41, 0x3f, 0x01}},
    {'K', {0x7f, 0x08, 0x14, 0x22, 0x41}},
    {'L', {0x7f, 0x40, 0x40, 0x40, 0x40}},
    {'M', {0x7f, 0x02, 0x0c, 0x02, 0x7f}},
    {'N', {0x7f, 0x04, 0x08, 0x10, 0x7f}},
    {'O', {0x3e, 0x41, 0x41, 0x41, 0x3e}},
    {'P', {0x7f, 0x09, 0x09, 0x09, 0x06}},
    {'Q', {0x3e, 0x41, 0x51, 0x21, 0x5e}},
    {'R', {0x7f, 0x09, 0x19, 0x29, 0x46}},
    {'S', {0x46, 0x49, 0x49, 0x49, 0x31}},
    {'T', {0x01, 0x01, 0x7f, 0x01, 0x01}},
    {'U', {0x3f, 0x40, 0x40, 0x40, 0x3f}},
    {'V', {0x1f, 0x20, 0x40, 0x20, 0x1f}},
    {'W', {0x3f, 0x40, 0x38, 0x40, 0x3f}},
    {'X', {0x63, 0x14, 0x08, 0x14, 0x63}},
    {'Y', {0x07, 0x08, 0x70, 0x08, 0x07}},
    {'Z', {0x61, 0x51, 0x49, 0x45, 0x43}},
    {'a', {0x20, 0x54, 0x54, 0x54, 0x78}},
    {'b', {0x7f, 0x48, 0x44, 0x44, 0x38}},
    {'c', {0x38, 0x44, 0x44, 0x44, 0x20}},
    {'d', {0x38, 0x44, 0x44, 0x48, 0x7f}},
    {'e', {0x38, 0x54, 0x54, 0x54, 0x18}},
    {'f', {0x08, 0x7e, 0x09, 0x01, 0x02}},
    {'g', {0x0c, 0x52, 0x52, 0x52, 0x3e}},
    {'h', {0x7f, 0x08, 0x04, 0x04, 0x78}},
    {'i', {0x00, 0x44, 0x7d, 0x40, 0x00}},
    {'j', {0x20, 0x40, 0x44, 0x3d, 0x00}},
    {'k', {0x7f, 0x10, 0x28, 0x44, 0x00}},
    {'l', {0x00, 0x41, 0x7f, 0x40, 0x00}},
    {'m', {0x7c, 0x04, 0x18, 0x04, 0x78}},
    {'n', {0x7c, 0x08, 0x04, 0x04, 0x78}},
    {'o', {0x38, 0x44, 0x44, 0x44, 0x38}},
    {'p', {0x7c, 0x14, 0x14, 0x14, 0x08}},
    {'q', {0x08, 0x14, 0x14, 0x18, 0x7c}},
    {'r', {0x7c, 0x08, 0x04, 0x04, 0x08}},
    {'s', {0x48, 0x54, 0x54, 0x54, 0x20}},
    {'t', {0x04, 0x3f, 0x44, 0x40, 0x20}},
    {'u', {0x3c, 0x40, 0x40, 0x20, 0x7c}},
    {'v', {0x1c, 0x20, 0x40, 0x20, 0x1c}},
    {'w', {0x3c, 0x40, 0x30, 0x40, 0x3c}},
    {'x', {0x44, 0x28, 0x10, 0x28, 0x44}},
    {'y', {0x0c, 0x50, 0x50, 0x50, 0x3c}},
    {'z', {0x44, 0x64, 0x54, 0x4c, 0x44}},
};

static const uint8_t *glyph_columns(char character)
{
    for (size_t index = 0U; index < sizeof(font) / sizeof(font[0]); ++index) {
        if (font[index].character == character) {
            return font[index].columns;
        }
    }
    return font[0].columns;
}

bool revlink_oled_glyph_known(char character)
{
    for (size_t index = 0U; index < sizeof(font) / sizeof(font[0]); ++index) {
        if (font[index].character == character) {
            return true;
        }
    }
    return false;
}

void revlink_oled_set_pixel(uint8_t *framebuffer, int x, int y, bool enabled)
{
    if (framebuffer == NULL
        || x < 0 || x >= REVLINK_OLED_WIDTH
        || y < 0 || y >= REVLINK_OLED_HEIGHT) {
        return;
    }
    const size_t offset =
        (size_t)x + (size_t)(y / 8) * REVLINK_OLED_WIDTH;
    const uint8_t mask = (uint8_t)(1U << (unsigned int)(y & 7));
    if (enabled) {
        framebuffer[offset] |= mask;
    } else {
        framebuffer[offset] &= (uint8_t)~mask;
    }
}

void revlink_oled_fill_rect(
    uint8_t *framebuffer,
    int x,
    int y,
    int width,
    int height,
    bool enabled
)
{
    for (int row = y; row < y + height; ++row) {
        for (int column = x; column < x + width; ++column) {
            revlink_oled_set_pixel(framebuffer, column, row, enabled);
        }
    }
}

void revlink_oled_draw_rect(
    uint8_t *framebuffer,
    int x,
    int y,
    int width,
    int height
)
{
    revlink_oled_fill_rect(framebuffer, x, y, width, 1, true);
    revlink_oled_fill_rect(framebuffer, x, y + height - 1, width, 1, true);
    revlink_oled_fill_rect(framebuffer, x, y, 1, height, true);
    revlink_oled_fill_rect(framebuffer, x + width - 1, y, 1, height, true);
}

int revlink_oled_text_width(const char *text, int scale)
{
    return text == NULL
        ? 0
        : (int)strlen(text) * REVLINK_OLED_GLYPH_ADVANCE * scale - scale;
}

void revlink_oled_draw_text(
    uint8_t *framebuffer,
    int x,
    int y,
    const char *text,
    int scale
)
{
    if (text == NULL || scale < 1) {
        return;
    }
    while (*text != '\0') {
        const uint8_t *columns = glyph_columns(*text);
        for (int column = 0; column < 5; ++column) {
            for (int row = 0; row < 7; ++row) {
                if ((columns[column] & (1U << row)) != 0U) {
                    revlink_oled_fill_rect(
                        framebuffer,
                        x + column * scale,
                        y + row * scale,
                        scale,
                        scale,
                        true
                    );
                }
            }
        }
        x += REVLINK_OLED_GLYPH_ADVANCE * scale;
        ++text;
    }
}

void revlink_oled_draw_centered(
    uint8_t *framebuffer,
    int y,
    const char *text,
    int scale
)
{
    revlink_oled_draw_text(
        framebuffer,
        (REVLINK_OLED_WIDTH - revlink_oled_text_width(text, scale)) / 2,
        y,
        text,
        scale
    );
}

static void draw_portal_mark(uint8_t *framebuffer, int x, int y, int phase)
{
    revlink_oled_draw_rect(framebuffer, x, y, 17, 17);
    revlink_oled_draw_rect(framebuffer, x + 3, y + 3, 11, 11);
    revlink_oled_fill_rect(framebuffer, x + 8, y + 6, 3, 8, false);
    revlink_oled_fill_rect(framebuffer, x + 10, y + 11, 7, 3, true);
    revlink_oled_fill_rect(framebuffer, x + 13, y + 14, 4, 3, true);
    const int scan = phase % 15;
    revlink_oled_fill_rect(framebuffer, x + 1, y + 1 + scan, 15, 1, true);
}

void revlink_oled_splash_banner(
    const char *version,
    char *output,
    size_t capacity,
    int *scale
)
{
    if (output == NULL || capacity == 0U) {
        return;
    }
    /*
     * "V" and the version, nothing else. The previous banner read
     * "FIRMWARE 0.2.1", and the label cost enough width to force the whole
     * line down to the 7-pixel font. Under a wordmark that already says
     * REVLINK, the label carried no information the version did not.
     *
     * esp_app_desc_t::version is 32 bytes and a git-describe fallback fills
     * most of it, so this truncates rather than trusting it to be short. The
     * capacity is what a full-width line holds at scale 1, which is why a
     * truncated banner still cannot be drawn off the edge of the panel.
     */
    (void)snprintf(
        output,
        capacity,
        "V%s",
        version != NULL && version[0] != '\0' ? version : "UNKNOWN"
    );
    if (scale != NULL) {
        /*
         * Prefer the 14-pixel font, and fall back only when the version is
         * too long for it. This is the same rule the status screen applies to
         * its headline.
         */
        *scale =
            revlink_oled_text_width(output, 2) <= REVLINK_OLED_WIDTH - 4
                ? 2 : 1;
    }
}

void revlink_oled_draw_splash(
    uint8_t *framebuffer,
    uint32_t elapsed_ms,
    const char *version
)
{
    if (framebuffer == NULL) {
        return;
    }
    memset(framebuffer, 0, REVLINK_OLED_BUFFER_SIZE);
    const int phase = (int)(elapsed_ms / 80U);
    draw_portal_mark(framebuffer, 10, 18, phase);
    revlink_oled_draw_text(framebuffer, 34, 20, "REVLINK", 2);

    /*
     * The firmware version, not a fixed greeting. Nothing can be drawn while
     * the board is being flashed — the application is not running, the ROM
     * bootloader is, and the panel simply holds whatever frame was left on it.
     * The first thing it draws afterwards is therefore the only chance to show
     * that anything changed, so make it say which build is now running, and
     * make it large enough to read across a workbench.
     */
    char banner[REVLINK_OLED_SPLASH_BANNER_CAPACITY];
    int scale = 1;
    revlink_oled_splash_banner(version, banner, sizeof(banner), &scale);
    revlink_oled_draw_centered(
        framebuffer,
        scale == 2 ? 37 : 41,
        banner,
        scale
    );

    revlink_oled_fill_rect(framebuffer, 8, 55, 112, 2, false);
    revlink_oled_draw_rect(framebuffer, 8, 54, 112, 4);
    const int width = (int)((elapsed_ms * 108U) / REVLINK_OLED_SPLASH_MS);
    revlink_oled_fill_rect(
        framebuffer,
        10,
        55,
        width > 108 ? 108 : width,
        2,
        true
    );
}
