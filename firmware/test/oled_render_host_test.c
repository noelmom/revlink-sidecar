/*
 * Renders the boot splash the way the panel driver does and inspects the
 * result. The point is to make what the display shows checkable without the
 * display: the previous banner could be composed at a width the panel could
 * not hold, and nothing in the build or the tests would have said so — the
 * text simply ran off both edges, because centring a 149-pixel line on a
 * 128-pixel panel starts it at x = -10 and set_pixel drops what falls outside.
 *
 * Run with an argument to dump the frames as ASCII art and look at them.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "revlink_oled_render.h"

static int failures;

static void check(bool condition, const char *what)
{
    if (!condition) {
        printf("FAIL %s\n", what);
        ++failures;
    }
}

static bool pixel(const uint8_t *framebuffer, int x, int y)
{
    if (x < 0 || x >= REVLINK_OLED_WIDTH
        || y < 0 || y >= REVLINK_OLED_HEIGHT) {
        return false;
    }
    const size_t offset =
        (size_t)x + (size_t)(y / 8) * REVLINK_OLED_WIDTH;
    return (framebuffer[offset] & (uint8_t)(1U << (unsigned int)(y & 7)))
        != 0U;
}

/* Columns holding at least one lit pixel in the given row band. */
static void ink_extent(
    const uint8_t *framebuffer,
    int top,
    int bottom,
    int *first,
    int *last
)
{
    *first = -1;
    *last = -1;
    for (int x = 0; x < REVLINK_OLED_WIDTH; ++x) {
        for (int y = top; y <= bottom; ++y) {
            if (pixel(framebuffer, x, y)) {
                if (*first < 0) {
                    *first = x;
                }
                *last = x;
                break;
            }
        }
    }
}

/* Lit columns strictly inside the progress-bar frame. */
static int bar_fill(const uint8_t *framebuffer)
{
    int count = 0;
    for (int x = 9; x <= 118; ++x) {
        if (pixel(framebuffer, x, 55)) {
            ++count;
        }
    }
    return count;
}

static void dump(const uint8_t *framebuffer, const char *caption)
{
    printf("\n%s\n    +", caption);
    for (int x = 0; x < REVLINK_OLED_WIDTH; ++x) {
        putchar('-');
    }
    printf("+\n");
    for (int y = 0; y < REVLINK_OLED_HEIGHT; ++y) {
        printf("%3d |", y);
        for (int x = 0; x < REVLINK_OLED_WIDTH; ++x) {
            putchar(pixel(framebuffer, x, y) ? '#' : ' ');
        }
        printf("|\n");
    }
    printf("    +");
    for (int x = 0; x < REVLINK_OLED_WIDTH; ++x) {
        putchar('-');
    }
    printf("+\n");
}

int main(int argc, char **argv)
{
    (void)argv;
    const bool show = argc > 1;
    uint8_t framebuffer[REVLINK_OLED_BUFFER_SIZE];

    /* The version this test was written against, and the shapes a version
     * can actually take: a plain release, a dirty local build, and the
     * git-describe fallback that filled most of esp_app_desc_t::version. */
    static const char *versions[] = {
        "0.2.1",
        "0.2.10",
        "0.2.1-dirty",
        "0.1.0-10-gd7c5a13",
        "0.1.0-10-gd7c5a13-dirty-and-then-some",
        "",
    };

    for (size_t index = 0U;
         index < sizeof(versions) / sizeof(versions[0]);
         ++index) {
        const char *version = versions[index];
        char banner[REVLINK_OLED_SPLASH_BANNER_CAPACITY];
        int scale = 0;
        revlink_oled_splash_banner(version, banner, sizeof(banner), &scale);

        char label[160];
        (void)snprintf(
            label,
            sizeof(label),
            "version \"%s\" -> banner \"%s\" at scale %d (%d px wide)",
            version,
            banner,
            scale,
            revlink_oled_text_width(banner, scale)
        );

        /* Whatever the version, the banner must fit the panel. This is the
         * property the old layout lacked. */
        check(
            revlink_oled_text_width(banner, scale) <= REVLINK_OLED_WIDTH,
            label
        );
        check(scale == 1 || scale == 2, "scale is one of the two sizes");

        /* Every glyph must exist, or the banner draws as gaps. */
        for (size_t byte = 0U; banner[byte] != '\0'; ++byte) {
            check(
                revlink_oled_glyph_known(banner[byte]),
                "banner uses only characters the font has"
            );
        }

        revlink_oled_draw_splash(framebuffer, 0U, version);

        /* The banner band must carry ink, and all of it must sit inside the
         * panel with a margin — a clipped line would touch column 0 or 127. */
        const int top = scale == 2 ? 37 : 41;
        int first = 0;
        int last = 0;
        ink_extent(framebuffer, top, top + 7 * scale - 1, &first, &last);
        check(first > 0, "banner does not touch the left edge");
        check(last < REVLINK_OLED_WIDTH - 1, "banner does not touch the right edge");

        /* And it must not collide with the wordmark above or the progress
         * bar below. */
        int bar_first = 0;
        int bar_last = 0;
        ink_extent(framebuffer, 54, 57, &bar_first, &bar_last);
        check(bar_first == 8, "progress bar frame still starts at x=8");
        check(top + 7 * scale - 1 < 54, "banner clears the progress bar");
        check(top > 33, "banner clears the REVLINK wordmark");

        if (show) {
            dump(framebuffer, label);
        }
    }

    /* An empty version says so rather than drawing a bare "V". */
    char banner[REVLINK_OLED_SPLASH_BANNER_CAPACITY];
    int scale = 0;
    revlink_oled_splash_banner(NULL, banner, sizeof(banner), &scale);
    check(strcmp(banner, "VUNKNOWN") == 0, "a missing version reads UNKNOWN");

    /* The released version must get the large font — that is the whole point
     * of the change, and a regression here would be silent. */
    revlink_oled_splash_banner("0.2.2", banner, sizeof(banner), &scale);
    check(scale == 2, "a release version is drawn at the large size");

    /* The progress bar still tracks elapsed time. Measure strictly inside
     * the frame: its own left and right verticals sit in these rows too, so
     * the lit extent is 8..119 whether the bar is empty or full. */
    revlink_oled_draw_splash(framebuffer, 0U, "0.2.2");
    check(bar_fill(framebuffer) == 0, "the progress bar starts empty");
    revlink_oled_draw_splash(framebuffer, REVLINK_OLED_SPLASH_MS / 2U, "0.2.2");
    const int half = bar_fill(framebuffer);
    revlink_oled_draw_splash(framebuffer, REVLINK_OLED_SPLASH_MS, "0.2.2");
    const int full = bar_fill(framebuffer);
    check(half > 0 && half < full, "the progress bar fills as time passes");
    check(full == 108, "a finished bar fills the frame without overrunning it");

    if (failures == 0) {
        printf("oled_render_host_test: all checks passed\n");
    }
    return failures == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
