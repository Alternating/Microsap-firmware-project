// maple_frames.h — Dreamcast Maple bus response frame headers, decoded from the pico2maple
// core1 dispatch literal pool (0x10001eb8.. / 0x1000218c..). Header word is little-endian
// [byte0=length in 32-bit words][byte1=sender AP][byte2=recipient AP][byte3=command].
// The maple_tx path patches the AP bytes with the live port number + sub-peripheral mask, so
// these constants carry the *template* src/dst (01 = main device, 02 = first sub-peripheral).
#ifndef P2M_MAPLE_FRAMES_H
#define P2M_MAPLE_FRAMES_H

// --- response command headers (payload length baked into byte0) ---
#define MAPLE_HDR_ACK_MAIN          0x07000100u  // cmd 07 DEVICE_REPLY/ACK, 0 words, main
#define MAPLE_HDR_ACK_SUB           0x07000200u  // cmd 07 ACK, 0 words, sub-peripheral
#define MAPLE_HDR_DEVICE_STATUS_MAIN 0x0500201cu // cmd 05 DEVICE_STATUS, 28 words; src AP 0x20
                                                 // (main peripheral present — the DC requires this)
#define MAPLE_HDR_DEVICE_STATUS_SUB  0x0500021cu // cmd 05 DEVICE_STATUS, 28 words, sub
#define MAPLE_HDR_ALL_STATUS        0x06000130u  // cmd 06 ALL_STATUS, 48 words (192B), src 0x01 (VMU)
#define MAPLE_HDR_ALL_STATUS_MAIN   0x06002030u  // cmd 06 ALL_STATUS, 48 words, src 0x20 (controller)
#define MAPLE_HDR_DEVICE_STATUS_VMU  0x0500011cu // cmd 05 DEVICE_STATUS, 28 words, src 0x01 (VMU slot 1)
#define MAPLE_HDR_DATA_BLOCK_READ   0x08000182u  // cmd 08 DATA_TRANSFER, 130 words (VMU block), src 0x01
#define MAPLE_HDR_DATA_SHORT_VMU    0x08000102u  // cmd 08, 2 words, src 0x01 (LCD media info / VMU cond)
#define MAPLE_HDR_DATA_MEDIA_INFO   0x08000107u  // cmd 08, 7 words, src 0x01 (storage media info)
#define MAPLE_HDR_DATA_RUMBLE_MEDIA 0x08000202u  // cmd 08, 2 words, sub (Puru Puru media info)
#define MAPLE_HDR_DATA_RUMBLE_COND  0x08000203u  // cmd 08, 3 words, sub (Puru Puru condition)
#define MAPLE_HDR_DATA_COND_MOUSE   0x0500011cu  // mouse GET_CONDITION reuses DEVICE_STATUS len

// --- error / no-data replies ---
#define MAPLE_HDR_ERR_MAIN          0xfc002000u  // cmd fc (function/file error), main
#define MAPLE_HDR_ERR_SUB           0xfc000200u  // cmd fc, sub-peripheral

// --- payload buffer globals the dispatch points each frame at (RAM, runtime-built) ---
// g_devinfo_payload   @0x20006b20  controller DEVICE_STATUS body (112B, rebuilt on layout switch)
// g_devinfo_allstatus @0x20006b28  ALL_STATUS extension
// g_subperiph_mask    @0x20001668  sub-peripheral present mask (also patched into sender AP)
// rx frame area       @0x2000e974
// vmu media/block resp buffers in the vmu_state_t (passed per-call)
// g_dc_mouse_cond     @0x20005e68
// g_storage_status    @0x2000fd6b

#endif
