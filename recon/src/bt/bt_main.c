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
    // Latency: bluepad32 default link policy allows SNIFF mode, a power-saving schedule that
    // adds 10-30ms of input lag on classic-BT pads (Xbox requests it eagerly). Disallow it;
    // keep role switch. Must land before a pad connects - connection setup reads this default.
    gap_set_default_link_policy_settings(LM_LINK_POLICY_ENABLE_ROLE_SWITCH);
    s_bt_ready = true;
}

// from 0x10008dcc
bool bt_ready(void) { return s_bt_ready; }

// from 0x1000723c — the original pumped the cyw43/btstack run loop here; with
// pico_cyw43_arch_none that happens via cyw43_arch_poll(). // TODO(verify) run-loop integration
void cyw43_poll_init(void) { }
