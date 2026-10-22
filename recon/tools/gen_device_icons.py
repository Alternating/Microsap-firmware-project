#!/usr/bin/env python3
# gen_device_icons.py — build-time generator for the OLED connected-device icons.
# Draws four 24x16 1bpp icons (bt, pad, kbd, mouse), prints an ASCII preview, and emits the
# SSD1306 page-format C arrays (const uint8_t g_icon_dev_*[48] = 24B page0 + 24B page1) that
# get pasted into recon/src/assets/p2m_assets.c. Stdlib only. SSD1306 page byte = 8 vertical
# pixels, LSB = top row of the page.

def px(cv, x, y):
    if 0 <= x < 24 and 0 <= y < 16:
        cv[y][x] = 1

def hline(cv, x0, x1, y):
    if x0 > x1:
        x0, x1 = x1, x0
    for x in range(x0, x1 + 1):
        px(cv, x, y)

def vline(cv, x, y0, y1):
    if y0 > y1:
        y0, y1 = y1, y0
    for y in range(y0, y1 + 1):
        px(cv, x, y)

def line(cv, x0, y0, x1, y1):
    dx = abs(x1 - x0)
    dy = abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    x, y = x0, y0
    while True:
        px(cv, x, y)
        if x == x1 and y == y1:
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x += sx
        if e2 < dx:
            err += dx
            y += sy

def rect(cv, x0, y0, x1, y1):
    if x0 > x1:
        x0, x1 = x1, x0
    if y0 > y1:
        y0, y1 = y1, y0
    hline(cv, x0, x1, y0)
    hline(cv, x0, x1, y1)
    vline(cv, x0, y0, y1)
    vline(cv, x1, y0, y1)

def create_bt_icon():
    cv = [[0] * 24 for _ in range(16)]
    vline(cv, 11, 1, 14)
    line(cv, 11, 1, 15, 5)
    line(cv, 15, 5, 11, 8)
    line(cv, 11, 8, 15, 11)
    line(cv, 15, 11, 11, 15)
    line(cv, 7, 5, 15, 11)
    line(cv, 7, 11, 15, 5)
    return cv

def create_pad_icon():
    cv = [[0] * 24 for _ in range(16)]
    rect(cv, 2, 4, 21, 12)
    for cx, cy in [(2, 4), (21, 4), (2, 12), (21, 12)]:
        cv[cy][cx] = 0
    hline(cv, 4, 8, 8)
    vline(cv, 6, 6, 10)
    for bx, by in [(16, 7), (18, 9)]:
        px(cv, bx, by)
        px(cv, bx + 1, by)
        px(cv, bx, by + 1)
        px(cv, bx + 1, by + 1)
    return cv

def create_kbd_icon():
    cv = [[0] * 24 for _ in range(16)]
    rect(cv, 1, 4, 22, 12)
    for y in (6, 8, 10):
        for x in (4, 7, 10, 13, 16, 19):
            px(cv, x, y)
    hline(cv, 8, 15, 11)
    return cv

def create_mouse_icon():
    cv = [[0] * 24 for _ in range(16)]
    rect(cv, 8, 2, 15, 14)
    for cx, cy in [(8, 2), (15, 2), (8, 14), (15, 14)]:
        cv[cy][cx] = 0
    vline(cv, 11, 3, 7)
    px(cv, 11, 4)
    px(cv, 12, 4)
    return cv

def preview_icon(cv):
    return '\n'.join(''.join('#' if p else '.' for p in row) for row in cv)

def pack_to_ssd1306(cv):
    result = []
    for page in range(2):
        for x in range(24):
            byte_val = 0
            for b in range(8):
                if cv[page * 8 + b][x]:
                    byte_val |= 1 << b
            result.append(byte_val)
    return result

def format_c_array(data):
    lines = []
    for i in range(0, len(data), 12):
        chunk = data[i:i + 12]
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in chunk))
    return ',\n'.join(lines)

def main():
    icons = [
        ('bt', create_bt_icon),
        ('pad', create_pad_icon),
        ('kbd', create_kbd_icon),
        ('mouse', create_mouse_icon),
    ]
    for name, creator in icons:
        cv = creator()
        print(name)
        print(preview_icon(cv))
        print()
        packed = pack_to_ssd1306(cv)
        print(f'const uint8_t g_icon_dev_{name}[48] = {{')
        print(format_c_array(packed))
        print('};  // 24B page0, 24B page1')
        print()

if __name__ == '__main__':
    main()
