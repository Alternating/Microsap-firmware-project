#!/usr/bin/env python3
"""Render a P4 (binary) PBM as ASCII and emit an SSD1306 page-format 1024-byte C array."""
import sys

def load_pbm(path):
    data = open(path, 'rb').read()
    assert data[:2] == b'P4', 'expected binary PBM (P4)'
    # parse header: P4 <w> <h> then raw bits (rows MSB-first, padded to byte per row)
    idx = 2
    vals = []
    while len(vals) < 2:
        while idx < len(data) and data[idx] in b' \t\n\r':
            idx += 1
        if data[idx:idx+1] == b'#':
            while data[idx] not in b'\n':
                idx += 1
            continue
        start = idx
        while data[idx] not in b' \t\n\r':
            idx += 1
        vals.append(int(data[start:idx]))
    idx += 1  # single whitespace after header
    w, h = vals
    bits = data[idx:]
    rowbytes = (w + 7) // 8
    px = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            byte = bits[y * rowbytes + (x >> 3)]
            # PBM: 1 = black. OLED: 1 = lit (white). Invert so lit pixels = logo ink.
            px[y][x] = 1 - ((byte >> (7 - (x & 7))) & 1)
    return w, h, px

def preview(w, h, px):
    for y in range(0, h, 2):
        line = ''
        for x in range(0, w, 1):
            top = px[y][x]
            bot = px[y + 1][x] if y + 1 < h else 0
            line += ('█' if top and bot else '▀' if top else '▄' if bot else ' ')
        print(line)

def to_ssd1306(w, h, px):
    """8 pages x 128 bytes; byte = 8 vertical pixels, bit0 = top of the page."""
    out = bytearray(w * h // 8)
    for page in range(h // 8):
        for x in range(w):
            b = 0
            for bit in range(8):
                if px[page * 8 + bit][x]:
                    b |= 1 << bit
            out[page * w + x] = b
    return bytes(out)

if __name__ == '__main__':
    w, h, px = load_pbm(sys.argv[1])
    preview(w, h, px)
    if len(sys.argv) > 2 and sys.argv[2] == '--c':
        blob = to_ssd1306(w, h, px)
        print(f"\n// {len(blob)} bytes")
        for i in range(0, len(blob), 16):
            print('    ' + ', '.join(f'0x{b:02x}' for b in blob[i:i+16]) + ',')
