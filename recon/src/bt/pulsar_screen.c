// pulsar_screen.c — the loading screens on the Pulsar's VMU (pulsar_screen.h).
#include <string.h>
#include "pulsar_screen.h"

#define W        48
#define H        32
#define ROW      (W / 8)
#define ADVANCE  4                  // 3-pixel glyph and a gap: 12 characters across

// The bar's outline spans columns BAR_L..BAR_R and rows BAR_T..BAR_B; the fill goes inside.
#define BAR_L    3
#define BAR_R    (BAR_L + PULSAR_SCREEN_BAR + 1)
#define BAR_T    19
#define BAR_B    23

// 3x5 glyphs, one row per entry, bit 2 the left pixel. Only what the screens spell.
static const struct { char c; uint8_t rows[5]; } k_font[] = {
    { 'A', { 2, 5, 7, 5, 5 } }, { 'C', { 7, 4, 4, 4, 7 } }, { 'D', { 6, 5, 5, 5, 6 } },
    { 'E', { 7, 4, 7, 4, 7 } }, { 'G', { 7, 4, 5, 5, 7 } }, { 'I', { 7, 2, 2, 2, 7 } },
    { 'N', { 6, 5, 5, 5, 5 } }, { 'O', { 7, 5, 5, 5, 7 } }, { 'R', { 6, 5, 6, 5, 5 } },
    { 'T', { 7, 2, 2, 2, 2 } }, { 'Y', { 5, 5, 2, 2, 2 } },
    { '0', { 7, 5, 5, 5, 7 } }, { '1', { 2, 6, 2, 2, 7 } }, { '2', { 7, 1, 7, 4, 7 } },
    { '3', { 7, 1, 3, 1, 7 } }, { '4', { 5, 5, 7, 1, 1 } }, { '5', { 7, 4, 7, 1, 7 } },
    { '6', { 7, 4, 7, 5, 7 } }, { '7', { 7, 1, 1, 2, 2 } }, { '8', { 7, 5, 7, 5, 7 } },
    { '9', { 7, 5, 7, 1, 7 } }, { '/', { 1, 1, 2, 4, 4 } },
};

static void px(uint8_t *f, unsigned x, unsigned y)
{
    if (x < W && y < H) f[y * ROW + x / 8] |= (uint8_t)(0x80u >> (x % 8));
}

static const uint8_t *glyph(char c)
{
    for (unsigned i = 0; i < sizeof k_font / sizeof k_font[0]; i++)
        if (k_font[i].c == c) return k_font[i].rows;
    return NULL;                    // a space, or a letter no screen uses yet
}

// One line centred across the screen, its top at row y.
static void text(uint8_t *f, unsigned y, const char *s)
{
    unsigned n = (unsigned)strlen(s);
    if (n > W / ADVANCE) n = W / ADVANCE;
    unsigned x0 = (W - (n * ADVANCE - 1)) / 2;
    for (unsigned i = 0; i < n; i++) {
        const uint8_t *g = glyph(s[i]);
        if (!g) continue;
        for (unsigned dy = 0; dy < 5; dy++)
            for (unsigned dx = 0; dx < 3; dx++)
                if (g[dy] & (4u >> dx)) px(f, x0 + i * ADVANCE + dx, y + dy);
    }
}

unsigned pulsar_screen_bar_fill(unsigned done, unsigned total)
{
    if (!total) return 0;
    if (done > total) done = total;
    return (unsigned)((uint32_t)PULSAR_SCREEN_BAR * done / total);  // 40 x 256 fits easily
}

void pulsar_screen_loading(uint8_t out[PULSAR_SCREEN_BYTES], unsigned done, unsigned total)
{
    memset(out, 0, PULSAR_SCREEN_BYTES);
    text(out, 3, "NOT READY");
    text(out, 11, "READING CARD");
    if (!total) return;
    if (done > total) done = total;
    for (unsigned x = BAR_L; x <= BAR_R; x++) { px(out, x, BAR_T); px(out, x, BAR_B); }
    unsigned fill = pulsar_screen_bar_fill(done, total);
    for (unsigned y = BAR_T; y <= BAR_B; y++) {
        px(out, BAR_L, y);
        px(out, BAR_R, y);
        for (unsigned x = BAR_L + 1; x <= BAR_L + fill; x++) px(out, x, y);
    }
    char count[13];
    unsigned d = done > 999 ? 999 : done, t = total > 999 ? 999 : total;
    unsigned n = 0;
    char tmp[3]; unsigned k;
    for (k = 0; k == 0 || d; d /= 10) tmp[k++] = (char)('0' + d % 10);
    while (k) count[n++] = tmp[--k];
    count[n++] = '/';
    for (k = 0; k == 0 || t; t /= 10) tmp[k++] = (char)('0' + t % 10);
    while (k) count[n++] = tmp[--k];
    count[n] = '\0';
    text(out, 26, count);
}

void pulsar_screen_message(uint8_t out[PULSAR_SCREEN_BYTES], const char *line)
{
    memset(out, 0, PULSAR_SCREEN_BYTES);
    text(out, (H - 5) / 2, line);
}
