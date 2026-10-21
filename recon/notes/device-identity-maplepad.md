# Controller device-identity reference (from MaplePad collaborators)

Source: Discord screenshots from the MaplePad project (Xerxes3rd / YveltalGriffin aka mackieks),
shared 2026-10 by a project collaborator to improve controller compatibility. MaplePad is an
open-source Dreamcast VMU/controller firmware; this is its `AllInfoPacket` device-identity code.

## The reply the Dreamcast wants (GET_ALL_STATUS / "all device info", cmd 6, 192 bytes)

A full all-device-info reply = the 112-byte device-info body + an 80-byte **FreeDeviceStatus**
string. cluoma's pico2maple (and our reconstruction) OMITS the FreeDeviceStatus entirely — the
string below is absent from the binary. Most games accept just a correct ProductName, but some
(notably **Outtrigger**) require the full identity (all buttons + six axes + the version string)
before they will use the controller.

### Standard Dreamcast Controller (HKT-7700)
- Func = FUNC_CONTROLLER
- FuncData[0] = `0x000f06fe`  (standard button/axis capability — matches our verified layout-3 word)
- AreaCode = 0xff, ConnectorDirection = 0
- ProductName = "Dreamcast Controller          "  (30 chars, space-padded)
- ProductLicense = "Produced By or Under License From SEGA ENTERPRISES,LTD.     "
- StandbyPower = 430, MaxPower = 500
- FreeDeviceStatus = "Version 1.010,1998/09/28,315-6211-AB    ,Analog Module : The 4th Edition.5/8  +DF"
- If a second analog stick is enabled: `FuncData[0] |= CONTROLLER_SECOND_STICK_AXES`
  (i.e. the capability word gains the extra-axes bit so the console knows about 6 axes).

### PantherDC Controller
- Identical, except:
  - FuncData[0] = `0x003fffff`  ("PantherDC always advertises all buttons and six axes")
  - ProductName = "Dreamcast PantherDC Controller"
- Reporting as PantherDC is a compatibility trick for demanding games; per the authors,
  "all games are totally fine with that."

## What this means for our firmware

1. **Add the FreeDeviceStatus string** to our ALL_STATUS (cmd 6) reply for the main device.
   cluoma's firmware never answered ALL_STATUS for the controller (only the VMU); real
   controllers do, with the version string. This is the Outtrigger-class compatibility fix.
2. Our standard capability word `0x000f06fe` is confirmed correct.
3. Optional future: a "report as PantherDC" identity mode (0x003fffff + the PantherDC name) as a
   user toggle for maximum game compatibility.
4. CONTROLLER_SECOND_STICK_AXES: when we present a second stick (twin-stick layout), the
   capability word should gain the extra-axes bit so six axes are advertised.

Also noted by the authors: MaplePad can attach TWO sub-peripherals to one controller/arcade
stick — consistent with our dual-VMU work.
