#!/usr/bin/env python3
"""Mini PIO assembler (subset) to verify recon/src/maple.pio against the binary."""
import re, struct, sys

COND = {'': 0, '!x': 1, 'x--': 2, '!y': 3, 'y--': 4, 'x!=y': 5, 'pin': 6, '!osre': 7}
INSRC = {'pins': 0, 'x': 1, 'y': 2, 'null': 3, 'isr': 6, 'osr': 7}
OUTDST = {'pins': 0, 'x': 1, 'y': 2, 'null': 3, 'pindirs': 4, 'pc': 5, 'isr': 6, 'exec': 7}
SETDST = {'pins': 0, 'x': 1, 'y': 2, 'pindirs': 4}
MOVDST = {'pins': 0, 'x': 1, 'y': 2, 'exec': 4, 'pc': 5, 'isr': 6, 'osr': 7}
MOVSRC = {'pins': 0, 'x': 1, 'y': 2, 'null': 3, 'status': 5, 'isr': 6, 'osr': 7}

def num(tok):
    tok = tok.strip()
    return int(tok, 0) if not tok.startswith('0b') else int(tok, 2)

def assemble(lines):
    # pass 1: labels
    labels, pc, insts = {}, 0, []
    for raw in lines:
        line = raw.split(';')[0].strip()
        if not line or line.startswith('.'): continue
        m = re.match(r'^([A-Za-z_][\w]*):\s*(.*)$', line)
        if m:
            labels[m.group(1)] = pc
            line = m.group(2).strip()
            if not line: continue
        insts.append(line); pc += 1
    # pass 2: encode
    out = []
    for pc, line in enumerate(insts):
        delay = 0
        m = re.search(r'\[(\d+)\]\s*$', line)
        if m: delay = int(m.group(1)); line = line[:m.start()].strip()
        t = re.split(r'[,\s]+', line)
        op = t[0]
        if op == 'jmp':
            if len(t) == 2: cond, tgt = '', t[1]
            else: cond, tgt = t[1], t[2]
            addr = labels[tgt] if tgt in labels else num(tgt)
            w = (0 << 13) | (COND[cond] << 5) | addr
        elif op == 'wait':
            pol = num(t[1]); src = {'gpio': 0, 'pin': 1, 'irq': 2, 'jmppin': 3}[t[2]]; idx = num(t[3])
            w = (1 << 13) | (pol << 7) | (src << 5) | idx
        elif op == 'in':
            w = (2 << 13) | (INSRC[t[1]] << 5) | (num(t[2]) & 0x1f)
        elif op == 'out':
            w = (3 << 13) | (OUTDST[t[1]] << 5) | (num(t[2]) & 0x1f)
        elif op == 'pull':
            ifempty = 'ifempty' in t; block = 0 if 'noblock' in t else 1
            w = (4 << 13) | (1 << 7) | (ifempty << 6) | (block << 5)
        elif op == 'push':
            iffull = 'iffull' in t; block = 0 if 'noblock' in t else 1
            w = (4 << 13) | (iffull << 6) | (block << 5)
        elif op == 'mov':
            dst = t[1]; src = t[2]; opn = 0
            if src.startswith('~'): opn = 1; src = src[1:]
            if src.startswith('::'): opn = 2; src = src[2:]
            w = (5 << 13) | (MOVDST[dst] << 5) | (opn << 3) | MOVSRC[src]
        elif op == 'set':
            w = (7 << 13) | (SETDST[t[1]] << 5) | (num(t[2]) & 0x1f)
        else:
            raise SystemExit(f"unhandled op {op!r} in {line!r}")
        out.append(w | (delay << 8))
    return out

src = open(sys.argv[1]).read().splitlines()
progs, cur = {}, None
for line in src:
    s = line.split(';')[0].strip()
    if s.startswith('.program'):
        cur = s.split()[1]; progs[cur] = []
    elif cur is not None:
        progs[cur].append(line)

data = open(sys.argv[2], 'rb').read()
refs = {'maple_tx': (0x5e4b4, 30), 'maple_rx': (0x5e4f8, 24)}
ok = True
for name, (off, n) in refs.items():
    want = list(struct.unpack_from(f'<{n}H', data, off))
    got = assemble(progs[name])
    if got == want:
        print(f"{name}: EXACT MATCH ({n} instructions)")
    else:
        ok = False
        print(f"{name}: MISMATCH (got {len(got)} vs {n} instr)")
        for i in range(max(len(got), len(want))):
            g = got[i] if i < len(got) else None
            w = want[i] if i < len(want) else None
            flag = '' if g == w else '   <<<<'
            print(f"  {i:2d}: got {g:04x} want {w:04x}{flag}" if g is not None and w is not None
                  else f"  {i:2d}: got {g} want {w}{flag}")
sys.exit(0 if ok else 1)
