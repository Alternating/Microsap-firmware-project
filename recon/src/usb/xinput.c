// xinput.c — USB Xbox controller host glue (tusb_xinput <-> p2m gamepad).
// Pattern derived from joypad-ai/joypad-os (USBRetro), Copyright 2024 Robert Dale Smith,
// Apache-2.0; rewritten for Pico2Maple Path B against Ryzee119/tusb_xinput.
#include <stdio.h>
#include <string.h>
#include "pico/stdlib.h"
#include "tusb.h"
#include "xinput_host.h"
#include "usb_host.h"            // pulls in config/mapping.h (p2m_gamepad_t)

static p2m_gamepad_t s_xpad;
static bool    s_xpad_connected;
static uint8_t s_addr, s_inst;

static void xpad_neutral(void)
{
    memset(s_xpad.g, 0, sizeof s_xpad.g);
    s_xpad.g[0x11] = s_xpad.g[0x12] = s_xpad.g[0x13] = s_xpad.g[0x14] = 0x80;
}

// TinyUSB hook: register app-level host class drivers (the xinput driver).
usbh_class_driver_t const *usbh_app_driver_get_cb(uint8_t *driver_count)
{
    *driver_count = 1;
    return &usbh_xinput_driver;
}

static void map_pad(const xinput_gamepad_t *pad)
{
    xpad_neutral();
    uint16_t b = pad->wButtons;
    if (b & XINPUT_GAMEPAD_A)              s_xpad.g[0x00] = 0xff;
    if (b & XINPUT_GAMEPAD_B)              s_xpad.g[0x01] = 0xff;
    if (b & XINPUT_GAMEPAD_X)              s_xpad.g[0x04] = 0xff;
    if (b & XINPUT_GAMEPAD_Y)              s_xpad.g[0x05] = 0xff;
    if (b & XINPUT_GAMEPAD_START)          s_xpad.g[0x07] = 0xff;
    if (b & XINPUT_GAMEPAD_DPAD_UP)        s_xpad.g[0x09] = 0xff;
    if (b & XINPUT_GAMEPAD_DPAD_DOWN)      s_xpad.g[0x0a] = 0xff;
    if (b & XINPUT_GAMEPAD_DPAD_LEFT)      s_xpad.g[0x0b] = 0xff;
    if (b & XINPUT_GAMEPAD_DPAD_RIGHT)     s_xpad.g[0x0c] = 0xff;
    s_xpad.g[0x0d] = pad->bRightTrigger;   // RT analog
    s_xpad.g[0x0e] = pad->bLeftTrigger;    // LT analog
    if (b & XINPUT_GAMEPAD_RIGHT_SHOULDER) s_xpad.g[0x0f] = 0xff;
    if (b & XINPUT_GAMEPAD_LEFT_SHOULDER)  s_xpad.g[0x10] = 0xff;
    // sticks: DC convention 0x00 = left/up, 0x80 = center (xinput +Y is up -> invert Y)
    s_xpad.g[0x11] = (uint8_t)(((int32_t)pad->sThumbLX + 32768) >> 8);
    s_xpad.g[0x12] = (uint8_t)((32767 - (int32_t)pad->sThumbLY) >> 8);
    s_xpad.g[0x13] = (uint8_t)(((int32_t)pad->sThumbRX + 32768) >> 8);
    s_xpad.g[0x14] = (uint8_t)((32767 - (int32_t)pad->sThumbRY) >> 8);
    if (b & XINPUT_GAMEPAD_BACK)           s_xpad.g[P2M_MISC] |= P2M_MISC_SELECT;
    if (b & XINPUT_GAMEPAD_GUIDE)          s_xpad.g[P2M_MISC] |= P2M_MISC_SYSTEM;
}

void tuh_xinput_mount_cb(uint8_t dev_addr, uint8_t instance, const xinputh_interface_t *xinput_itf)
{
    switch (xinput_itf->type) {
    case XBOX360_WIRED:
        tuh_xinput_set_led(dev_addr, instance, 1, true);
        /* fallthrough */
    case XBOXONE:
    case XBOXOG:
        s_addr = dev_addr; s_inst = instance;
        s_xpad_connected = true;
        xpad_neutral();
        printf("[xinput] pad mounted (type %d)\n", (int)xinput_itf->type);
        break;
    case XBOX360_WIRELESS:   // wait for the connect packet (xid_itf->connected in report cb)
    default:
        break;
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_report_received_cb(uint8_t dev_addr, uint8_t instance,
                                   xinputh_interface_t const *xid_itf, uint16_t len)
{
    (void)len;
    if (xid_itf->connected && xid_itf->new_pad_data) {
        if (!s_xpad_connected) {              // adopt (e.g. 360-wireless connect packet)
            s_addr = dev_addr; s_inst = instance;
            s_xpad_connected = true;
            printf("[xinput] pad connected\n");
        }
        if (s_addr == dev_addr && s_inst == instance)
            map_pad(&xid_itf->pad);
    } else if (!xid_itf->connected && s_xpad_connected &&
               s_addr == dev_addr && s_inst == instance) {
        s_xpad_connected = false;             // 360-wireless pad powered off
        xpad_neutral();
        printf("[xinput] pad disconnected\n");
    }
    tuh_xinput_receive_report(dev_addr, instance);
}

void tuh_xinput_umount_cb(uint8_t dev_addr, uint8_t instance)
{
    if (s_xpad_connected && s_addr == dev_addr && s_inst == instance) {
        s_xpad_connected = false;
        xpad_neutral();
        printf("[xinput] pad unmounted\n");
    }
}

bool xinput_pad_connected(void) { return s_xpad_connected; }
const p2m_gamepad_t *xinput_get_gamepad(void) { return s_xpad_connected ? &s_xpad : NULL; }
void xinput_set_rumble(uint8_t strength)
{
    if (s_xpad_connected)
        tuh_xinput_set_rumble(s_addr, s_inst, strength, strength, false);
}
