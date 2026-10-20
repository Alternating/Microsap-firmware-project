# Hardware-verified button map — standard layout (3), Xbox BT pad

Verified 2026-10-19 by live guided sweep: each input held on a paired Xbox Wireless
Controller while reading the firmware's Dreamcast condition block over the debug UART.
`btn` values are the DC active-low button mask as transmitted to the console.

| # | Xbox input        | bluepad32 raw          | Dreamcast output              | Observed            |
|---|-------------------|------------------------|-------------------------------|---------------------|
| 1 | A                 | BUTTON_A (0x0001)      | A (mask bit 2)                | btn=fffb            |
| 2 | B                 | BUTTON_B (0x0002)      | B (bit 1)                     | btn=fffd            |
| 3 | X                 | BUTTON_X (0x0004)      | X (bit 10)                    | btn=fbff            |
| 4 | Y                 | BUTTON_Y (0x0008)      | Y (bit 9)                     | btn=fdff            |
| 5 | Menu              | MISC_BUTTON_START      | Start (bit 3)                 | btn=fff7            |
| 6 | D-pad Up          | DPAD_UP                | Up (bit 4)                    | btn=ffef            |
| 7 | D-pad Down        | DPAD_DOWN              | Down (bit 5)                  | btn=ffdf            |
| 8 | D-pad Left        | DPAD_LEFT              | Left (bit 6)                  | btn=ffbf            |
| 9 | D-pad Right       | DPAD_RIGHT             | Right (bit 7)                 | btn=ff7f            |
|10 | LB                | SHOULDER_L (0x0010)    | right trigger full (digital)  | rt=ff               |
|11 | RB                | SHOULDER_R (0x0020)    | Z (bit 8)                     | btn=feff            |
|12 | LT (analog)       | brake                  | left trigger analog           | lt=00..ff           |
|13 | RT (analog)       | throttle               | C (bit 0) when pull > 199     | btn=fffe            |
|14 | Stick left/right  | axis_x                 | joyx 0x00..0xff (0 = left)    | stick=00,.. at full |
|15 | Stick up/down     | axis_y                 | joyy 0x00..0xff (0 = up)      | stick=..,00 at full |
|16 | View              | MISC_BUTTON_SELECT     | none — hotkey modifier; gameplay output suppressed while held | btn stays ffff, raw presses visible |

The LB/RT→trigger/C cross-routing is faithful to the original firmware's standard layout
(verified against the binary's mapper, case 3 of FUN_1000098c).

## Known caveats
- **Per-connection input loss (open):** on some Bluetooth connections the upper face
  buttons (X/Y) and shoulders/triggers are absent from the parsed report entirely
  (raw button word never shows them). Reconnecting the pad restores them. Suspected
  dual-mode (BR/EDR vs BLE) parse-path difference in bluepad32; the BLE path carries our
  descriptor-at-setup patch. Workaround: reconnect. Diagnose with the BX= raw dump.
- **Stick drift passthrough:** this pad rests around 0x8a,0x83 — outside the firmware's
  center-snap window (0x7e..0x82, matching the original). Faithful behavior; widen the
  snap window if drift bothers gameplay.
- Hotkeys while View is held: A=std layout, B=all-buttons, X=twin-stick, Y=fight-stick,
  DpadRight=custom, DpadLeft=racing, RB/LB=VMU bank +/-, DpadDown=rumble toggle.
