/* The picture shown while Mutate is held: a double helix turning about its axis, as the stock
 * firmware draws one (docs/STOCK-UI.md, Mutate). Two strands half a turn apart, a point every 4
 * pixels and 16 degrees along 96 pixels, radius 12, a rung between them every 8 pixels. It turns
 * 200 degrees a second, and it is tilted by two angles under 30 degrees taken from the moment
 * of the press, so every press shows it from another side. After a quarter of a second a line
 * along the bottom grows and shrinks, a second each way. */
#include "app.h"

static int sin127(int degrees) {
    static const int8_t quarter[91] = {
        0,   2,   4,   7,   9,   11,  13,  15,  18,  20,  22,  24,  26,  29,  31,  33,  35,  37,  39,
        41,  43,  46,  48,  50,  52,  54,  56,  58,  60,  62,  63,  65,  67,  69,  71,  73,  75,  76,
        78,  80,  82,  83,  85,  87,  88,  90,  91,  93,  94,  96,  97,  99,  100, 101, 103, 104, 105,
        107, 108, 109, 110, 111, 112, 113, 114, 115, 116, 117, 118, 119, 119, 120, 121, 121, 122, 123,
        123, 124, 124, 125, 125, 125, 126, 126, 126, 127, 127, 127, 127, 127, 127,
    };
    degrees %= 360;
    if (degrees < 0) degrees += 360;
    if (degrees <= 90) return quarter[degrees];
    if (degrees <= 180) return quarter[180 - degrees];
    if (degrees <= 270) return -quarter[degrees - 180];
    return -quarter[360 - degrees];
}

static int cos127(int degrees) { return sin127(degrees + 90); }

static void line(int x0, int y0, int x1, int y1) {
    int dx = x1 > x0 ? x1 - x0 : x0 - x1, dy = y1 > y0 ? y0 - y1 : y1 - y0;
    int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1, error = dx + dy;
    for (;;) {
        ks_oled_pixel(x0, y0, 1);
        if (x0 == x1 && y0 == y1) return;
        int twice = 2 * error;
        if (twice >= dy) {
            error += dy;
            x0 += sx;
        }
        if (twice <= dx) {
            error += dx;
            y0 += sy;
        }
    }
}

static int spin, tilt, lean;    /* about the helix's axis; about the upright; in the display's plane */

/* A point of the helix `along` its axis at `angle` round it, onto the display. */
static void place(int along, int angle, int *px, int *py) {
    int x = along, y = (cos127(angle) * 12 + 63) / 127, z = (sin127(angle) * 12 + 63) / 127;
    int x1 = (cos127(lean) * x - sin127(lean) * y) / 127, y1 = (cos127(lean) * y + sin127(lean) * x) / 127;
    int x2 = (cos127(tilt) * x1 + sin127(tilt) * z) / 127, z2 = (cos127(tilt) * z - sin127(tilt) * x1) / 127;
    *px = KS_OLED_W / 2 + x2;
    *py = KS_OLED_H / 2 + (cos127(spin) * y1 - sin127(spin) * z2) / 127;
}

void helix_draw(uint32_t pressed_at, uint32_t now) {
    uint32_t held = now - pressed_at;
    int x0, y0, x1, y1;
    spin = (int)(held / 5 % 360);
    lean = (int)(pressed_at % 30);
    tilt = (int)((pressed_at >> 5) % 30);
    for (int strand = 0; strand < 360; strand += 180) {
        place(-48, strand, &x0, &y0);
        for (int i = 1; i < 24; i++, x0 = x1, y0 = y1) {
            place(-48 + 4 * i, strand + 16 * i, &x1, &y1);
            line(x0, y0, x1, y1);
        }
    }
    for (int i = 2; i < 24; i += 2) {
        place(-48 + 4 * i, 16 * i, &x0, &y0);
        place(-48 + 4 * i, 16 * i + 180, &x1, &y1);
        line(x0, y0, x1, y1);
    }
    if (held >= 250) {
        uint32_t t = (held - 250) % 2000;
        ks_oled_rect(0, KS_OLED_H - 1, (int)((t < 1000 ? t : 2000 - t) * 127 / 1000), 1, 1);
    }
}
