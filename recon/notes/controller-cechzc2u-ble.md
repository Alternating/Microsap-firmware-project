# "CECHZC2U PS3 clone" — actually a generic BLE HID gamepad

Analyzed 2026-10-20 on the bench laptop (paired over Bluetooth, read via sysfs hidraw).
Despite being sold as a PS3/CECHZC2U clone, it is **NOT a DualShock 3**. It is a standard
Bluetooth-LE HID gamepad:

- Bus = 0005 (BUS_BLUETOOTH / BLE HOGP), Name = "Gamepad"
- Vendor = 0x1949, Product = 0x0402 (generic cheap-clone ID; NOT Sony 054C:0268)
- Linux binds it to the generic HID path and exposes js0 — no hid-sony / DS3 quirks involved.

## HID report descriptor (decoded)

Two top-level collections:

**Report ID 2 — Consumer Control** (media keys; 10 one-bit usages + padding). Ignore for gameplay.

**Report ID 7 — Gamepad** (the one that matters):

| Offset | Size | Field |
|---|---|---|
| byte 0 | 1 | Report ID = 0x07 |
| byte 1 | 1 | X  — left stick X (0..255, 0x80 center) |
| byte 2 | 1 | Y  — left stick Y |
| byte 3 | 1 | Z  — right stick X |
| byte 4 | 1 | Rz — right stick Y |
| byte 5 | low nibble | Hat switch (0..7 = 8-way D-pad; 8/0xF = neutral) |
| byte 5 | high nibble | padding |
| bytes 6-7 | 16 bits | Buttons 1..16 (bitmask, LE) |
| byte 8 | 1 | Accelerator (RT / R2 analog, 0..255) |
| byte 9 | 1 | Brake (LT / L2 analog, 0..255) |

This is a textbook BLE HID gamepad layout — exactly what bluepad32's generic/BLE HID parser
is built to read by HID *usage* (no per-controller quirk table needed). It is the same BLE
path our `bluepad32-xbox-ble-descriptor-at-setup.patch` fixes (collect the descriptor at
HID_SERVICE_CONNECTED, before the parser is chosen).

## Implications for our firmware

- Our bluepad32 build should handle this pad via the BLE generic path. If it connects but
  gives no input, suspect the same lazy-descriptor race the Xbox pad had (our patch targets
  that) or a GAP-appearance check in `uni_bt_le_on_gap_event_advertising_report` rejecting a
  non-standard advertised appearance.
- To test against Microsap: **unpair it from the laptop first** (a BT device bonds to one host
  at a time), then pair to the Microsap with the verbose-BT build and watch whether bluepad32
  accepts the BLE advertisement.
- Button→bit physical mapping wasn't captured (not needed for bluepad32's usage-based parse);
  grab a live `usbhid-dump`/js0 sample if a specific remap is ever required.
