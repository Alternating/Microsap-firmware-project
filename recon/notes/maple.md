# Maple engine reconstruction notes

Module owner: primary session (not a subagent). Sources: FUN_10000298 (core1 main),
FUN_10001550 (rx), FUN_10001660 (tx), FUN_100013c0 (kbd cond), FUN_100013f4 (mouse cond update),
FUN_10001494/0x1000151c (device-mode switch), FUN_100008f0/0x1000090c; maple.pio byte-exact.

## Confidence

- maple.pio: **EXACT** (assembles byte-identical; tools/pio_check.py passes).
- RX algorithm (bit realignment, 0x84 sync, len*4+5 framing, XOR check): **HIGH** — direct
  transcription.
- TX algorithm (staging, byte-swap per word, <<24 FIFO feed, TXSTALL handshake,
  AP patching with port + subperiph mask): **HIGH**.
- PIO SM configs: **HIGH** (register values read from literal pools; field decode of
  shiftctrl TX 0x50060000 needs a double check against RP2350 datasheet bit positions).
- core1 init sequence order: **HIGH**; some callee identities MED (marked TODO).
- Dispatch: **HIGH** for routing logic; the exact prebuilt response headers (DAT_10001exx
  constants) still need values dumped and inlined (currently placeholders = 0 in maple.c).
- Mouse/keyboard condition micro-functions: MED — FUN_100013f4 clamps axes 0..0x3ff around
  0x200 and accumulates `-4*wheel` into +0xc; exact field naming TBD.
- Command 0x21: GameID notification (OpenMenu/GDEMU). Evidence: gated on VMU sub-addr, calls
  vmu_gameid_store then switches per-game VMU bank iff settings field 8 (gameid mode) set.

## Key facts for the rebuild

- RX=PIO0 SM0 (clkdiv 1.0), TX=PIO1 SM0 (clkdiv 0x0008e700 ≈ 8.902; 150MHz/8.902 ≈ 16.85MHz).
- Both Maple GPIOs idle as inputs with pull-ups, gpio func = PIO1. SDCKB (pin+1) additionally
  flips between PIO1-output (during TX) and SIO-input (idle), managed with the RP2350 GPIO
  coprocessor single-bit ops (gpioc_bit_*).
- Wire byte order: header transmitted LSB-first ([len][src][dst][cmd]); payload words
  byte-swapped on TX. RX stream needs a 2-bit realignment after the start pattern; sync
  signature is (fifo_word & 0xfc) == 0x84.
- The port number (AP bits 7:6) is captured from incoming DEVICE_REQUEST (dst>>6) and patched
  into every response; the sub-peripheral mask @0x20001668 is OR-ed into the sender AP.
- 128KB VMU image + staging buffers live on core1 stack (frame 0x20630 bytes) — core1 stack
  must be >132KB; original uses the default __StackOneBottom region? TODO(verify): linker
  script / PICO_CORE1_STACK_SIZE arrangement — possibly uses most of SRAM deliberately.
- Keyboard events arrive via pico_util queue (12-byte elements, depth 5, spinlock-claimed)
  from core0 (TinyUSB HID keyboard) to core1.
- RX-error path replies with a header from DAT_10001ee8 (likely 0xFC "resend") — dump value.

## Open TODOs

1. Dump and inline the prebuilt response header constants: DAT_10001eb8 (device status),
   DAT_10001edc (all status), DAT_10001ed4/0x1ed8/0x1ec4(=0x07000100 ACK)/0x1ec8/0x1ecc/
   0x1ed0/0x1ee0/0x1ee8, DAT_1000218c..0x21cc cluster (rumble + gameid frames).
2. Verify gpioc OUT vs OE operand mapping of coprocessor_moveto2(..., in_cr0/in_cr4).
3. Resolve FUN_10007b24 (triggered after attach events) — lives in USB module range;
   coordinates with usb agent's findings.
4. EXECCTRL extra bits for TX config ((pin&0x1f)<<24 → EXECCTRL? that's JMP_PIN field
   bits 28:24 = pin! TX program has no `jmp pin` — probably harmless default; verify).
5. The sm[0].pinctrl post-init tweak on RX pio (raw: reg34 = (reg34 & ~0x1f)+2) — reg 0x34
   from SM base = ?? decode against RP2350 PIO register map (likely SM0_PINCTRL low bits =
   OUT_BASE? or it's pio->input_sync_bypass). Verify before rebuild.
