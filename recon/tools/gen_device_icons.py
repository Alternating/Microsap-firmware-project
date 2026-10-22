#!/usr/bin/env python3
# gen_device_icons.py — build-time generator for the OLED connected-device icons.
# Matches the original pico2maple look: the indicator sits in the right-hand status column
# (stacked VMU / bank# / DEVICE / SD) at pages 4-5. Bluetooth is the text "BT"; keyboard and
# mouse are small sprites. Each icon is 32x16 1bpp in SSD1306 page format: 32 bytes page0
# (top 8 rows) + 32 bytes page1 (bottom 8 rows) = g_icon_dev_*[64], blitted at 0x4CC/0x5CC.
# Prints an ASCII preview to eyeball, then the C arrays. Stdlib only.

W, H = 32, 16

def new_canvas():
    return [[0] * W for _ in range(H)]

def px(cv, x, y):
    if 0 <= x < W and 0 <= y < H:
        cv[y][x] = 1

def hline(cv, x0, x1, y):
    if x0 > x1: x0, x1 = x1, x0
    for x in range(x0, x1 + 1): px(cv, x, y)

def vline(cv, x, y0, y1):
    if y0 > y1: y0, y1 = y1, y0
    for y in range(y0, y1 + 1): px(cv, x, y)

def rect(cv, x0, y0, x1, y1):
    if x0 > x1: x0, x1 = x1, x0
    if y0 > y1: y0, y1 = y1, y0
    hline(cv, x0, x1, y0); hline(cv, x0, x1, y1)
    vline(cv, x0, y0, y1); vline(cv, x1, y0, y1)

def stamp(cv, art, ox, oy):
    # art is a list of equal-length strings, '#' = on.
    for dy, row in enumerate(art):
        for dx, ch in enumerate(row):
            if ch == '#': px(cv, ox + dx, oy + dy)

LETTER_B = ["#####.", "#....#", "#....#", "#####.", "#....#", "#....#", "#####."]
LETTER_T = ["#######", "...#...", "...#...", "...#...", "...#...", "...#...", "...#..."]

def create_bt():   # text "BT" centred in the right column
    cv = new_canvas()
    stamp(cv, LETTER_B, 9, 5)     # cols 9..14
    stamp(cv, LETTER_T, 17, 5)    # cols 17..23
    return cv

def create_pad():  # gamepad (not important per ref, kept simple), centred
    cv = new_canvas()
    rect(cv, 6, 5, 25, 11)
    for cx, cy in [(6, 5), (25, 5), (6, 11), (25, 11)]: cv[cy][cx] = 0  # round corners
    hline(cv, 9, 13, 8); vline(cv, 11, 6, 10)                          # d-pad plus
    for bx, by in [(19, 7), (21, 9)]:                                  # two 2x2 buttons
        px(cv, bx, by); px(cv, bx + 1, by); px(cv, bx, by + 1); px(cv, bx + 1, by + 1)
    return cv

def create_kbd():  # keyboard: rounded body, 3 key rows, spacebar (matches the ref sprite)
    cv = new_canvas()
    rect(cv, 6, 4, 25, 12)
    for cx, cy in [(6, 4), (25, 4), (6, 12), (25, 12)]: cv[cy][cx] = 0
    for y in (6, 8):
        for x in (9, 12, 15, 18, 21): px(cv, x, y)
    hline(cv, 11, 20, 10)   # spacebar
    return cv

def create_mouse():  # mouse: narrow rounded body, button seam, scroll dot (matches the ref)
    cv = new_canvas()
    rect(cv, 13, 3, 18, 13)
    for cx, cy in [(13, 3), (18, 3), (13, 13), (18, 13)]: cv[cy][cx] = 0
    vline(cv, 15, 4, 7); vline(cv, 16, 4, 7)   # centre seam (two cols = button split)
    px(cv, 15, 5); px(cv, 16, 5)               # scroll dot cluster
    return cv

def preview(cv):
    return '\n'.join(''.join('#' if p else '.' for p in row) for row in cv)

def pack(cv):
    out = []
    for page in range(2):
        for x in range(W):
            b = 0
            for bit in range(8):
                if cv[page * 8 + bit][x]: b |= 1 << bit
            out.append(b)
    return out

def c_array(data):
    lines = []
    for i in range(0, len(data), 12):
        lines.append('    ' + ', '.join(f'0x{b:02x}' for b in data[i:i + 12]))
    return ',\n'.join(lines)

def main():
    for name, fn in [('bt', create_bt), ('pad', create_pad),
                     ('kbd', create_kbd), ('mouse', create_mouse)]:
        cv = fn()
        print(name)
        print(preview(cv))
        print()
        print(f'const uint8_t g_icon_dev_{name}[64] = {{')
        print(c_array(pack(cv)))
        print('};  // 32B page0, 32B page1 -> right column pages 4/5')
        print()

if __name__ == '__main__':
    main()
