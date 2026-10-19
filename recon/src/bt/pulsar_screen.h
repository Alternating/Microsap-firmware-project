// pulsar_screen.h — screens the dongle draws itself on the Pulsar's VMU, for the wait while the
// link pulls the card (story 025). The same screens the bridge and pulsar-link show, so a player
// sees one design whichever host they use: "NOT READY", "READING CARD", a bar and a block count,
// then "READY". Frames are 48x32, 1 bpp, rows of 6 bytes, leftmost pixel in the high bit, drawn
// the right way up as viewed: they never cross the Maple wire, so nothing has rotated them.
// No BTstack here, so the desktop test can draw them.
#ifndef P2M_PULSAR_SCREEN_H
#define P2M_PULSAR_SCREEN_H

#include <stdint.h>

#define PULSAR_SCREEN_BYTES 192
#define PULSAR_SCREEN_BAR   40       // pixels of fill in a full bar

// done of total blocks pulled. done past total draws full; total 0 draws no bar or count.
void pulsar_screen_loading(uint8_t out[PULSAR_SCREEN_BYTES], unsigned done, unsigned total);
// One line, centred on the screen. Up to 12 characters: A-Z as far as the font goes, 0-9, '/'.
void pulsar_screen_message(uint8_t out[PULSAR_SCREEN_BYTES], const char *line);
// Pixels of the bar filled for done of total, rounded down so a full bar means all of it.
unsigned pulsar_screen_bar_fill(unsigned done, unsigned total);

#endif
