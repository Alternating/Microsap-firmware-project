// main.c — entry point + core0 control loop. Reconstructed from pico2maple flipped-bt:
// main (0x10000608) and the core0 loop (0x10000308). Core1 runs the Maple engine
// (maple_core1_main, 0x10000298); core0 handles USB/BT input, rumble output, and hotkeys.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "pico/bootrom.h"
#include "hardware/watchdog.h"
#include "pico/multicore.h"
#include "pico/flash.h"
#include "pico/util/queue.h"
#include "board.h"
#include "storage/storage.h"
#include "maple/maple.h"
#include "control.h"
#include "usb/usb_host.h"
#include "bt/bp32_platform.h"
#include "oled/oled.h"              // OLED_DEV_* + oled_draw_device_icon

// cross-module entry points
extern void maple_core1_main(void);                 // 0x10000298 (runs on core1)
extern void usb_host_init(void);                     // 0x10007f08
extern void usb_host_task(void);                     // 0x1000726c (tuh_task + poll)
extern bool usb_pad_connected(void);                 // 0x100072c8
extern void controller_poll(void);                   // 0x100002b0
extern void rumble_tick(void);                        // around 0x10007c70 (forward DC rumble)
extern void bt_init(void);                            // 0x10008d14
extern bool bt_ready(void);                           // 0x10008dcc
extern void cyw43_poll_init(void);                    // 0x1000723c
extern void p2m_board_init(void);                 // 0x10008140 (board.c)

// hotkey/layout plumbing
extern void dc_set_device_mode(int func, int sub);    // 0x10001494 (announce kbd/mouse/pad)
extern void dc_reset_device_mode(void);               // 0x1000151c
extern void oled_draw_layout_icon(unsigned layout);   // 0x10003800
extern void oled_refresh_kick(void);                  // 0x10002eb8
extern void oled_service(void);                       // core0 owns the OLED push (oled.c)

// Shared flags/globals (addresses from the 0x10000308 literal pool).
extern volatile uint8_t g_core1_ready;   // @*DAT_100005e0 (set by core1 after Maple init)
static p2m_button_t g_mode_button;        // the physical mode/select button (button_init 0x3d50)
extern queue_t g_kbd_queue;               // @0x10000664 (12-byte elements, depth 0x14)

// from 0x10000608
int main(void)
{
    stdio_init_all();                     // 0x1000d844 (UART0 on GPIO0/1 @115200)
    sleep_ms(300);                        // let a serial terminal attach before the first prints
    puts("\n=== Starting pico2maple (Midwest Mods) ===");

    p2m_board_init();                     // 0x10008140: GPIO15 layout sense
    printf("[boot] pin layout: %s  (Maple base GPIO%u)\n",
           g_layout_alternate ? "ALTERNATE (GPIO15 grounded)" : "standard", g_pins.maple_pin);

    button_init(&g_mode_button, 15);      // 0x10003d50  // TODO(verify @0x10000608: button pin)
    usb_host_init();                      // 0x10007f08
    queue_init(&g_kbd_queue, 12, 0x14);   // 0x100091f8 + 0x1000a3d0
    puts("[boot] USB host + kbd queue up; launching core1 (Maple engine)");

    // Opt core0 into flash_safe_execute parking, so core1's settings_save can pause us
    // during its flash write (core1 opts in likewise for btstack's link-key saves).
    flash_safe_execute_core_init();
    multicore_launch_core1(maple_core1_main);   // 0x1000dd74 + 0x1000de64

    // fall into the core0 control loop (originally a separate function at 0x10000308)
    {   // wait for the Maple engine, but don't hang silently if core1 stalls
        uint64_t t = time_us_64();
        while (!g_core1_ready) {
            if (time_us_64() - t > 5000000) { puts("[boot] WARNING: core1 not ready after 5s"); t = time_us_64(); }
            tight_loop_contents();
        }
    }
    puts("[boot] core1 ready (Maple engine initialized)");

    bt_init();                            // 0x10008d14 ("failed to initialise cyw43_arch" on err)
    {   // BT is optional: time out instead of hanging so USB + Maple still work if BT fails
        uint64_t t = time_us_64();
        while (!bt_ready() && time_us_64() - t < 3000000) tight_loop_contents();
        puts(bt_ready() ? "[boot] Bluetooth ready" : "[boot] WARNING: Bluetooth not ready (USB + Maple still OK)");
    }
    cyw43_poll_init();                    // 0x1000723c

    control_init();                       // load settings + custom map now that SD/flash are up
    puts("[boot] control init done; entering main loop");

    // 300ms USB settle window before first poll (from the time_us_64 gate at 0x10000308 head)
    uint64_t t0 = time_us_64();
    while (time_us_64() - t0 < 300000 && !usb_pad_connected())
        usb_host_task();

    uint64_t hb = time_us_64();
    for (;;) {
        usb_host_task();                  // 0x1000726c
        controller_poll();                // 0x100002b0 (reads pads, runs custom+layout mapping)
        rumble_tick();                    // 0x10007c70 (forward DC vibration to USB/BT pad)
        oled_service();                   // present a frame core1 or the layout toggle kicked
        sd_write_queue_service();         // write one VMU page core1 queued for the SD card

        // connected-device indicator: show the active input source (BT / pad / kbd / mouse)
        // in the OLED top-middle band; only redraw when it changes.
        {
            static int last_dev = -1;
            int dev = OLED_DEV_NONE;
            if (bp32_pad_connected())                   dev = OLED_DEV_BT;
            else {
                unsigned k = usb_host_kinds();
                if (xinput_pad_connected() || (k & 4u)) dev = OLED_DEV_PAD;
                else if (k & 1u)                        dev = OLED_DEV_KBD;
                else if (k & 2u)                        dev = OLED_DEV_MOUSE;
            }
            if (dev != last_dev) { last_dev = dev; oled_draw_device_icon(dev); }
        }

        // heartbeat every ~3s so the log shows the loop is alive + current status
        if (time_us_64() - hb > 3000000) {
            hb = time_us_64();
            extern volatile uint32_t g_maple_frames, g_maple_dev_req, g_maple_get_cond;
            extern volatile uint32_t g_maple_rx_words, g_maple_rx_resync, g_maple_cond_sent, g_maple_devreq_sent, g_maple_last_miss;
            extern volatile uint32_t g_bp32_reports;
            extern volatile uint64_t g_bp32_last_report_us;
            extern const char *pulsar_link_heartbeat(void);
            static uint32_t prev_reports;
            uint32_t rate = (g_bp32_reports - prev_reports) / 3;
            prev_reports = g_bp32_reports;
            uint32_t age_ms = g_bp32_last_report_us
                ? (uint32_t)((time_us_64() - g_bp32_last_report_us) / 1000) : 0;
            if (age_ms > 9999) age_ms = 9999;
            extern dc_controller_cond_t g_dc_controller_cond;
            const p2m_gamepad_t *gp = usb_get_gamepad();
            if (!gp) gp = xinput_get_gamepad();
            if (!gp) gp = bp32_get_gamepad();
            const dc_controller_cond_t *c = &g_dc_controller_cond;
            extern uint8_t g_storage_status;
            printf("[hb] sd=%u usb_pad=%d xpad=%d bt_pad=%d layout=%d | DC rxw=%lu rsync=%lu frames=%lu "
                   "devreq=%lu/%lu cond=%lu/%lu miss=%06lx%s | bt %lu/s age=%lums | out: btn=%04x lt=%02x rt=%02x stick=%02x,%02x s2=%02x,%02x | pulsar=%s\n",
                   g_storage_status, usb_pad_connected(), xinput_pad_connected(), bp32_pad_connected(), g_active_layout,
                   (unsigned long)g_maple_rx_words, (unsigned long)g_maple_rx_resync,
                   (unsigned long)g_maple_frames, (unsigned long)g_maple_dev_req,
                   (unsigned long)g_maple_devreq_sent,
                   (unsigned long)g_maple_get_cond, (unsigned long)g_maple_cond_sent,
                   (unsigned long)g_maple_last_miss,
                   gp ? " [pad active]" : "",
                   (unsigned long)rate, (unsigned long)age_ms,
                   c->buttons, c->ltrig, c->rtrig, c->joyx, c->joyy, c->joy2x, c->joy2y, pulsar_link_heartbeat());
        }

        // Serial console commands (debug UART): CLI reflash/reboot without touching BOOTSEL.
        //   P2M_BOOTSEL -> reboot into the USB bootloader (mass-storage; flash.sh copies the UF2)
        //   P2M_REBOOT  -> plain reboot (re-runs boot + self-test)
        {
            static char cmd[24];
            static unsigned cmdlen;
            int ch = getchar_timeout_us(0);
            if (ch >= 0) {
                if (ch == '\n' || ch == '\r') {
                    cmd[cmdlen] = 0;
                    cmdlen = 0;
                    if (strcmp(cmd, "P2M_BOOTSEL") == 0) {
                        puts("[cmd] rebooting into BOOTSEL (USB drive mode)");
                        sleep_ms(50);
                        reset_usb_boot(0, 0);
                    } else if (strcmp(cmd, "P2M_BT_CLEAR") == 0) {
                        extern void uni_bt_del_keys_safe(void);
                        uni_bt_del_keys_safe();      // wipe all stored BT bonds (fresh-sync state)
                        puts("[cmd] BT bonds cleared - put the pad in sync mode to re-pair");
                    } else if (strcmp(cmd, "P2M_BT_SCAN") == 0) {
                        extern void uni_bt_start_scanning_and_autoconnect_safe(void);
                        uni_bt_start_scanning_and_autoconnect_safe();
                        puts("[cmd] BT scanning/pairing window forced open");
                    } else if (strcmp(cmd, "P2M_VMU_DIR") == 0) {
                        extern uint8_t *g_vmu_image_dbg;
                        extern volatile uint32_t g_maple_blk_writes, g_maple_last_wspec;
                        extern volatile uint32_t g_maple_blk_reads, g_maple_blk_read_errs,
                                                 g_maple_last_rspec, g_maple_minfo_reqs;
                        printf("[vmu] writes=%lu last_wspec=%08lx\n",
                               (unsigned long)g_maple_blk_writes, (unsigned long)g_maple_last_wspec);
                        printf("[vmu] reads=%lu errs=%lu last_rspec=%08lx minfo=%lu\n",
                               (unsigned long)g_maple_blk_reads, (unsigned long)g_maple_blk_read_errs,
                               (unsigned long)g_maple_last_rspec, (unsigned long)g_maple_minfo_reqs);
                        if (g_vmu_image_dbg) {
                            const uint8_t *dir = g_vmu_image_dbg + 253 * 512;  // directory block
                            for (int e = 0; e < 4; e++) {
                                const uint8_t *d = dir + e * 32;
                                printf("[vmu] dir%d: %02x %02x loc=%02x%02x name=%.12s\n",
                                       e, d[0], d[1], d[3], d[2], (const char *)d + 4);
                            }
                            const uint8_t *fat = g_vmu_image_dbg + 254 * 512;
                            int used = 0;
                            for (int b = 0; b < 512; b += 2)
                                if (!(fat[b] == 0xfc && fat[b+1] == 0xff)) used++;
                            printf("[vmu] fat non-free entries=%d\n", used);
                        }
                    } else if (strcmp(cmd, "P2M_REBOOT") == 0) {
                        puts("[cmd] rebooting");
                        sleep_ms(50);
                        watchdog_reboot(0, 0, 50);
                    }
                } else if (cmdlen < sizeof cmd - 1) {
                    cmd[cmdlen++] = (char)ch;
                } else {
                    cmdlen = 0;    // overlong garbage: drop
                }
            }
        }

        // Mode button DISABLED: it was wired to GPIO15 (a guess; original pin is a TODO),
        // which on the Maple Tap is the floating layout-sense pin. Noise on the floating
        // input produced phantom presses: random VMU bank switches ("memory card removed/
        // recognized" on screen) plus settings flash writes that park core0 (BT) mid-game.
        // Controller hotkeys (View+RB/LB etc.) provide the same functions.
        int ev = 0; (void)g_mode_button;        // TODO: map the real button pin from the schematic
        if (ev == 1) {
            maple_request_bank_switch(+1);       // 0x100008f0
        } else if (ev == 2) {
            // layout toggle path (0x10000308: 0x10001494 + icon 0x10003800)
            dc_reset_device_mode();              // 0x1000151c
            oled_draw_layout_icon(0);
            oled_refresh_kick();                 // 0x10002eb8
        }
    }
}
