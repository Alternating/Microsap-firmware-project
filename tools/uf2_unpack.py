#!/usr/bin/env python3
"""Unpack a UF2 file into contiguous raw binary images, one per family/region."""
import struct, sys

MAGIC0, MAGIC1, MAGIC_END = 0x0A324655, 0x9E5D5157, 0x0AB16F30
FAMILIES = {
    0xE48BFF56: "rp2040",
    0xE48BFF57: "absolute",
    0xE48BFF58: "rp2350-arm-ns",
    0xE48BFF59: "rp2350-arm-s",
    0xE48BFF5A: "rp2350-riscv",
}

def main(path):
    data = open(path, "rb").read()
    segments = {}  # family -> {addr: bytes}
    for off in range(0, len(data), 512):
        blk = data[off:off + 512]
        m0, m1, flags, addr, size, blkno, nblks, fam = struct.unpack_from("<8I", blk)
        mend, = struct.unpack_from("<I", blk, 508)
        if m0 != MAGIC0 or m1 != MAGIC1 or mend != MAGIC_END:
            print(f"skip block @0x{off:x}: bad magic")
            continue
        if flags & 0x1000:  # not main flash
            continue
        segments.setdefault(fam, {})[addr] = blk[32:32 + size]
    for fam, blocks in segments.items():
        name = FAMILIES.get(fam, f"fam{fam:08x}")
        addrs = sorted(blocks)
        start, end = addrs[0], addrs[-1] + len(blocks[addrs[-1]])
        img = bytearray(b"\xff" * (end - start))
        for a in addrs:
            img[a - start:a - start + len(blocks[a])] = blocks[a]
        out = f"{path.rsplit('.', 1)[0]}_{name}_0x{start:08x}.bin"
        open(out, "wb").write(img)
        gaps = sum(1 for i, a in enumerate(addrs[:-1]) if addrs[i + 1] != a + len(blocks[a]))
        print(f"{name}: 0x{start:08x}-0x{end:08x} ({end - start} bytes, {len(addrs)} blocks, {gaps} gaps) -> {out}")

if __name__ == "__main__":
    main(sys.argv[1])
