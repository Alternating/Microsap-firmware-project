// bt_main.c — Bluetooth bring-up: cyw43 + Bluepad32 + BTstack. Path B own-firmware.
// Wraps the init that the original did at 0x10008d14 ("failed to initialise cyw43_arch").
#include "pico/stdlib.h"
#include "pico/cyw43_arch.h"
#include "uni.h"
#include "btstack.h"
#include "bp32_platform.h"

static bool s_bt_ready;

// from 0x10008d14
void bt_init(void)
{
    if (cyw43_arch_init()) {
        printf("failed to initialise cyw43_arch\n");
        return;
    }
    bp32_platform_register();     // install our uni_platform before uni_init
    uni_init(0, NULL);            // starts BTstack + Bluepad32 (async run loop on core0)
    // NOTE: do NOT forbid sniff mode in the link policy. Xbox pads require sniff support and
    // can refuse to connect without it (observed: fresh sync failed on the sniff-off build).
    // Latency is measured via the heartbeat's "bt N/s age" instead before further tuning.
    s_bt_ready = true;
}

// from 0x10008dcc
bool bt_ready(void) { return s_bt_ready; }

// from 0x1000723c — the original pumped the cyw43/btstack run loop here; with
// pico_cyw43_arch_none that happens via cyw43_arch_poll(). // TODO(verify) run-loop integration
void cyw43_poll_init(void) { }
