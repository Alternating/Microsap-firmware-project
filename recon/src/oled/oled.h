// oled.h — SSD1306 128x64 status display + VMU LCD mirror. Reconstructed from
// pico2maple flipped-bt (0x10002c18–0x10003938). Alt layout: I2C on GPIO4(SDA)/5(SCL).
#ifndef P2M_OLED_H
#define P2M_OLED_H

#include <stdint.h>
#include <stdbool.h>

// One-time bring-up: alloc framebuffer, init I2C + panel, start the refresh DMA + alarm.
// `rotated` selects the flipped panel orientation (the repo's "rotated_screen" builds).
void oled_init(bool rotated);                 // from 0x10002d64

// Push the current framebuffer to the panel (blocking I2C).
void oled_present(void);
void oled_refresh_kick(void);                 // from 0x10002eb8 (now presents the framebuffer)

// Upscale the 48x32 1bpp VMU LCD into the framebuffer region. from 0x10002ef4
void vmu_lcd_upscale(const uint8_t *lcd_fb);  // from 0x10002ef4

// Status glyph painters (write directly into the framebuffer page rows).
void oled_draw_bank(unsigned bank);           // from 0x10003188 (0..9, 10="GID")
void oled_draw_sd_icon(bool dirty);           // from 0x100033b8 (dirty-flush indicator)
void oled_draw_storage_icon(bool internal);   // from 0x10003544 (internal flash vs SD)
void oled_draw_layout_icon(unsigned layout);  // from 0x10003800
void oled_draw_attached_icon(bool attached);  // from 0x10003938

#endif
