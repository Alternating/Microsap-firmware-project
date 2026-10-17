#!/usr/bin/env python3
"""Generate recon/src/assets/p2m_assets.{c,h} from binary rodata/data regions."""
import struct, os

BIN = '/projects/Pico2maple-decomp/pico2maple_flipped-bt_rp2350-arm-s_0x10000000.bin'
OUT = '/projects/Pico2maple-decomp/recon/src/assets'
data = open(BIN, 'rb').read()
BASE = 0x10000000

def flash(addr, n):
    return data[addr - BASE: addr - BASE + n]

def ram_copy(ram_addr, n):
    """Read the flash image of a .data RAM address."""
    off = ram_addr - 0x20000188 + 0xa0198
    return data[off:off + n]

# name, kind('flash'|'ram'), addr, len, comment
ASSETS = [
    ('g_vmu_root_template',   'flash', 0x1005e528, 0x400, 'VMU root-block template (@0x1005e528, used by vmu_format_bank 0x100022d0)'),
    ('g_glyph_bank10_gid',    'flash', 0x1005ee88, 0x40,  'bank glyph: GID/bank10 strip a (DAT_10003394)'),
    ('g_glyph_bank9',         'flash', 0x1005eec8, 0x40,  'bank glyph 9 (DAT_10003398)'),
    ('g_glyph_bank8',         'flash', 0x1005ef08, 0x40,  'bank glyph 8 (DAT_10003390)'),
    ('g_glyph_bank7',         'flash', 0x1005ef48, 0x40,  'bank glyph 7 (DAT_1000338c)'),
    ('g_glyph_bank6',         'flash', 0x1005ef88, 0x40,  'bank glyph 6 (DAT_10003388)'),
    ('g_glyph_bank5',         'flash', 0x1005efc8, 0x40,  'bank glyph 5 (DAT_10003384)'),
    ('g_glyph_bank4',         'flash', 0x1005f008, 0x40,  'bank glyph 4 (DAT_100033b4)'),
    ('g_glyph_bank3',         'flash', 0x1005f048, 0x40,  'bank glyph 3 (DAT_100033b0)'),
    ('g_glyph_bank2',         'flash', 0x1005f088, 0x3f,  'bank glyph 2 (DAT_100033ac)'),
    ('g_glyph_bank0_a',       'flash', 0x1005f0c7, 0x20,  'bank 0 label strip a (DAT_100033a4)'),
    ('g_glyph_bank0_b',       'flash', 0x1005f0e7, 0x20,  'bank 0 label strip b (DAT_100033a8)'),
    ('g_glyph_bank1_a',       'flash', 0x1005f107, 0x20,  'bank 1 label strip a (DAT_1000339c)'),
    ('g_glyph_bank1_b',       'flash', 0x1005f127, 0x40,  'bank 1 label strip b (DAT_100033a0)'),
    ('g_icon_storage_int',    'flash', 0x1005ea28, 0x20,  'storage icon internal (DAT_100037f8)'),
    ('g_icon_storage_sd',     'flash', 0x1005ea48, 0x20,  'storage icon SD (DAT_100037fc)'),
    ('g_icon_sd_state',       'flash', 0x1005ea68, 0x20,  'SD state icon (DAT_10003540)'),
    ('g_icon_layout_racing_a','flash', 0x1005e927, 0x20,  'layout icon strips (DAT_10003920/24: mode 4 racing)'),
    ('g_icon_layout_racing_b','flash', 0x1005e947, 0x40,  'layout icon mode 4 strip b (DAT_10003924)'),
    ('g_icon_layout_std_a',   'flash', 0x1005e967, 0x20,  'layout icon mode 1 strip a (DAT_10003928)'),
    ('g_icon_layout_std_b',   'flash', 0x1005e987, 0x40,  'layout icon mode 1 strip b (DAT_1000392c)'),
    ('g_icon_layout_ts_a',    'flash', 0x1005e9a7, 0x20,  'layout icon mode 2 strip a (DAT_10003930)'),
    ('g_icon_layout_ts_b',    'flash', 0x1005e9c7, 0x40,  'layout icon mode 2 strip b (DAT_10003934)'),
    ('g_icon_layout_alt',     'flash', 0x1005ea08, 0x20,  'layout icon mode 3/default strip (DAT_1000391c)'),
    ('g_icon_attach_on_a',    'flash', 0x1005f147, 0x20,  'device-attached icon on strip a (DAT_100039b4)'),
    ('g_icon_attach_off_a',   'flash', 0x1005f187, 0x20,  'device-attached icon off strip a (DAT_100039ac)'),
    ('g_icon_attach_off_b',   'flash', 0x1005f1a7, 0x40,  'device-attached icon off strip b (DAT_100039b0)'),
]

os.makedirs(OUT, exist_ok=True)

c = open(os.path.join(OUT, 'p2m_assets.c'), 'w')
h = open(os.path.join(OUT, 'p2m_assets.h'), 'w')
h.write('// Auto-generated from pico2maple_flipped-bt (tools/gen_assets.py). Do not edit.\n')
h.write('#ifndef P2M_ASSETS_H\n#define P2M_ASSETS_H\n#include <stdint.h>\n\n')
c.write('// Auto-generated from pico2maple_flipped-bt (tools/gen_assets.py). Do not edit.\n')
c.write('#include "p2m_assets.h"\n\n')

for name, kind, addr, n, comment in ASSETS:
    blob = flash(addr, n) if kind == 'flash' else ram_copy(addr, n)
    h.write(f'extern const uint8_t {name}[{n:#x}];  // {comment}\n')
    c.write(f'// {comment}\nconst uint8_t {name}[{n:#x}] = {{\n')
    for i in range(0, len(blob), 16):
        c.write('    ' + ', '.join(f'0x{b:02x}' for b in blob[i:i+16]) + ',\n')
    c.write('};\n\n')

# sample cfg text (rodata, NUL-terminate)
txt = flash(0x100a16f4, 0xa1da7 - 0xa16f4)
h.write('extern const char g_cfg_sample_text[];  // embedded sample cfg (@0x100a16f4)\n')
c.write('// embedded sample controller-map cfg (@0x100a16f4)\nconst char g_cfg_sample_text[] =\n')
for line in txt.decode('latin1').split('\n'):
    esc = line.replace('\\', '\\\\').replace('"', '\\"')
    c.write(f'    "{esc}\\n"\n')
c.write('    ;\n')
h.write('\n#endif\n')
c.close(); h.close()
print('generated', os.path.join(OUT, 'p2m_assets.c'))
