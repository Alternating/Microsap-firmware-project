// main.c — entry point + core0 control loop. Reconstructed from pico2maple flipped-bt:
// main (0x10000608) and the core0 loop (0x10000308). Core1 runs the Maple engine
// (maple_core1_main, 0x10000298); core0 handles USB/BT input, rumble output, and hotkeys.
#include <stdio.h>
#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/util/queue.h"
#include "board.h"
#include "storage/storage.h"
#include "maple/maple.h"
#include "control.h"
#include "usb/usb_host.h"
#include "bt/bp32_platform.h"

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

    // (No multicore lockout: core1 no longer writes flash at runtime — the internal VMU is
    // RAM-backed for now — so core0 never needs pausing. See vmu_flush_dirty.)
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

        // heartbeat every ~3s so the log shows the loop is alive + current status
        if (time_us_64() - hb > 3000000) {
            hb = time_us_64();
            extern volatile uint32_t g_maple_frames, g_maple_dev_req, g_maple_get_cond;
            extern dc_controller_cond_t g_dc_controller_cond;
            const p2m_gamepad_t *gp = usb_get_gamepad(); if (!gp) gp = bp32_get_gamepad();
            const dc_controller_cond_t *c = &g_dc_controller_cond;
            printf("[hb] usb_pad=%d bt_pad=%d layout=%d | DC frames=%lu devreq=%lu cond=%lu%s"
                   " | out: btn=%04x lt=%02x rt=%02x stick=%02x,%02x\n",
                   usb_pad_connected(), bp32_pad_connected(), g_active_layout,
                   (unsigned long)g_maple_frames, (unsigned long)g_maple_dev_req,
                   (unsigned long)g_maple_get_cond,
                   gp ? " [pad active]" : "",
                   c->buttons, c->ltrig, c->rtrig, c->joyx, c->joyy);
        }

        // Mode button: short press cycles VMU bank; long hold toggles a built-in layout.
        int ev = button_poll(&g_mode_button);   // 0x10003d78
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
