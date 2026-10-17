// maple.h — Maple bus engine interface, reconstructed from pico2maple flipped-bt (build "Mar 4 2026")
// Evidence addresses refer to the analyzed image pico2maple_flipped-bt_rp2350-arm-s_0x10000000.bin.
#ifndef P2M_MAPLE_H
#define P2M_MAPLE_H

#include <stdint.h>
#include <stdbool.h>
#include "hardware/pio.h"

// ---- Maple protocol constants ------------------------------------------------
// Wire frame (as seen by this firmware, LSB byte of the 32-bit header first):
//   [0]=length (payload words), [1]=sender AP, [2]=recipient AP, [3]=command,
//   then length*4 payload bytes (each 32-bit word byte-swapped on TX),
//   then 1 XOR checksum byte over header+payload.  (rx: 0x10001550, tx: 0x10001660)
#define MAPLE_CMD_DEVICE_REQUEST   0x01
#define MAPLE_CMD_ALL_STATUS_REQ   0x02
#define MAPLE_CMD_RESET            0x03
#define MAPLE_CMD_DEVICE_STATUS    0x05  // response
#define MAPLE_CMD_ALL_STATUS       0x06  // response
#define MAPLE_CMD_ACK              0x07  // response (header constant 0x07000100 @core1)
#define MAPLE_CMD_DATA_TRANSFER    0x08  // response
#define MAPLE_CMD_GET_CONDITION    0x09
#define MAPLE_CMD_GET_MEDIA_INFO   0x0A
#define MAPLE_CMD_BLOCK_READ       0x0B
#define MAPLE_CMD_BLOCK_WRITE      0x0C
#define MAPLE_CMD_GET_LAST_ERROR   0x0D  // write-complete / sync
#define MAPLE_CMD_SET_CONDITION    0x0E
#define MAPLE_CMD_GAMEID           0x21  // OpenMenu/GDEMU GameID notification (custom; dispatch @0x10001c1c)

// Maple function codes (host byte order as compared in the dispatch, 0x10000298)
#define MAPLE_FUNC_CONTROLLER 0x001
#define MAPLE_FUNC_STORAGE    0x002
#define MAPLE_FUNC_LCD        0x004
#define MAPLE_FUNC_CLOCK      0x008
#define MAPLE_FUNC_KEYBOARD   0x040
#define MAPLE_FUNC_VIBRATION  0x100
#define MAPLE_FUNC_MOUSE      0x200

// Sub-peripheral present/enable mask (global @0x20001668; bit0=VMU, bit1=Puru Puru).
// The same byte patches the sender-AP sub-bits on every TX (0x10001660).
#define P2M_SUBPERIPH_VMU     0x01
#define P2M_SUBPERIPH_RUMBLE  0x02

// Dreamcast controller condition (@0x20005e2c), FUNC_CONTROLLER GET_CONDITION payload:
//   u32 func_be; u16 buttons (active low); u8 rtrig, ltrig; u8 joyx, joyy, joy2x, joy2y;
typedef struct __attribute__((packed)) {
    uint32_t func;       // 0x01000000 (FUNC_CONTROLLER big-endian), set in core1 init
    uint16_t buttons;    // active-low: C=1,B=2,A=4,Start=8,Up=0x10,Down=0x20,Left=0x40,
                         // Right=0x80,Z=0x100,Y=0x200,X=0x400,D=0x800,DPAD2=0x1000..0x8000
    uint8_t  rtrig, ltrig;
    uint8_t  joyx, joyy; // 0x80 centered
    uint8_t  joy2x, joy2y;
} dc_controller_cond_t;

// Mouse condition (@0x20005e68): u32 func; u16 buttons; u16 dx, dy, wheel (0x200 centered,
// clamped 0..0x3ff by 0x100013f4); reset to 0x200,0x200,0x200 after each GET_CONDITION reply.
typedef struct __attribute__((packed)) {
    uint32_t func;
    uint16_t buttons;
    uint16_t dx, dy, wheel;  // 10-bit, 0x200 = no motion
    int16_t  accum_fraction; // @+0xc, decremented by wheel*4 (0x100013f4) TODO(verify @0x100013f4)
} dc_mouse_cond_t;

// A TX request: 32-bit header + optional payload pointer (payload length = header LSB words).
typedef struct {
    uint32_t header;        // [len][src AP][dst AP][cmd] — src/dst sub-bits patched at TX time
    const uint8_t *payload; // length*4 bytes, each u32 sent byte-swapped
} maple_frame_t;

// PIO context: {PIO pio; uint sm; uint offset;} — rx={pio0,0,off_rx}, tx={pio1,0,off_tx}
typedef struct {
    PIO pio;
    uint sm;
    uint offset;
} maple_pio_ctx_t;

// Core1 entry: full Maple engine (init + protocol loop). from 0x10000298 (FUN_10000298)
void maple_core1_main(void);

// Receive one frame into the RX buffer (@0x2000e970). Returns 0 on good checksum,
// -1 on bad checksum/none. Busy-polls the PIO0 RX FIFO. from 0x10001550
int maple_rx_frame(maple_pio_ctx_t *rx);

// Transmit one frame then return the bus to RX mode. from 0x10001660
void maple_tx_frame(maple_pio_ctx_t *tx, const maple_frame_t *frame);

// Request a VMU bank switch from the hotkey/mapper side (+1/-1). from 0x100008f0
void maple_request_bank_switch(int dir);

// Periodic SD hot-plug attach/detach tick (storage function announce). from 0x1000090c
void maple_storage_hotplug_tick(void);

#endif
