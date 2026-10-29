#!/usr/bin/env python3
"""The SNES port's samples (snes/spc/driver.spc) and voice table
(snes/sound.c): the FM voices tools/snes/voices.c rendered, BRR-encoded,
after a square wave for the PSG.

usage: sound.py DIR
Reads DIR/voices.txt; writes DIR/samples.bin (loaded at ARAM $0500),
samples_dir.bin (the DSP's directory at $0300: each sample's start and
loop) and voices.h (per voice: its hash, the reference pitch multiple, its
ADSR and release).
"""
import os
import sys

SAMPLES_AT = 0x0500
ARAM_END = 0xFF00           # (the IPL ROM's page stays clear)


def clamp16(v):
    return max(-32768, min(32767, v))


def decode(nibble, shift, filt, p1, p2):
    """The DSP's decoding of one BRR nibble (fullsnes)."""
    s = nibble - 16 if nibble >= 8 else nibble
    s = (s << shift) >> 1 if shift <= 12 else (-2048 if s < 0 else 0)
    if filt == 1:
        s += p1 + ((-p1) >> 4)
    elif filt == 2:
        s += (p1 << 1) + ((-((p1 << 1) + p1)) >> 5) - p2 + (p2 >> 4)
    elif filt == 3:
        s += (p1 << 1) + ((-(p1 + (p1 << 2) + (p1 << 3))) >> 6) - p2 + (((p2 << 1) + p2) >> 4)
    s = clamp16(s)
    s = ((s << 1) & 0xFFFF)                 # 15 bits, doubled
    s = s - 0x10000 if s & 0x8000 else s
    return s >> 1


def encode_block(pcm, p1, p2, filters):
    """The best (error, shift, filter, nibbles, p1, p2) for 16 samples."""
    best = None
    for filt in filters:
        for shift in range(13):
            q1, q2, err, nib = p1, p2, 0, []
            for v in pcm:
                # predict, then the nibble nearest the remainder
                base = decode(0, shift, filt, q1, q2)
                cands = []
                for n in range(16):
                    d = decode(n, shift, filt, q1, q2)
                    cands.append((abs(d - v), n, d))
                e, n, d = min(cands)
                err += e * e
                nib.append(n)
                q2, q1 = q1, d
                if best is not None and err >= best[0]:
                    break
            else:
                if best is None or err < best[0]:
                    best = (err, shift, filt, nib, q1, q2)
    return best


def brr(pcm, loop_block):
    """BRR blocks for pcm (a multiple of 16 samples), looping from block
    loop_block (filter 0 there, so the loop starts clean)."""
    out = bytearray()
    p1 = p2 = 0
    blocks = len(pcm) // 16
    for b in range(blocks):
        chunk = [v >> 1 for v in pcm[b * 16:b * 16 + 16]]   # (15 bits)
        filters = (0,) if b in (0, loop_block) else (0, 1, 2, 3)
        err, shift, filt, nib, p1, p2 = encode_block(chunk, p1, p2, filters)
        flags = 2 | (1 if b == blocks - 1 else 0)
        out.append(shift << 4 | filt << 2 | flags)
        for k in range(0, 16, 2):
            out.append(nib[k] << 4 | nib[k + 1])
    return out


def main():
    d = sys.argv[1]
    voices = []
    lines = open(os.path.join(d, 'voices.txt')).read().split('\n')
    for k in range(0, len(lines) - 1, 2):
        v = lines[k].split()
        pcm = [int(x) for x in lines[k + 1].split()[1:]]
        voices.append({'hash': int(v[1], 16), 'n': int(v[2]), 'adsr1': int(v[3], 16),
                       'adsr2': int(v[4], 16), 'release': int(v[5]), 'pcm': pcm})
    data = bytearray()
    directory = bytearray()

    def add(blocks_data, loop_block):
        start = SAMPLES_AT + len(data)
        directory.extend(start.to_bytes(2, 'little') + (start + loop_block * 9).to_bytes(2, 'little'))
        data.extend(blocks_data)

    # 0: a square wave, 32 samples a period (1 kHz at pitch $1000)
    add(brr([24000] * 16 + [-24000] * 16, 0), 0)
    for v in voices:
        pcm = v['pcm']
        add(brr(pcm, (len(pcm) - 64) // 16), (len(pcm) - 64) // 16)
    if len(directory) > 0x200:
        sys.exit('sound.py: more than 128 samples')
    if SAMPLES_AT + len(data) > ARAM_END:
        sys.exit('sound.py: the samples need %d bytes, %d fit' % (len(data), ARAM_END - SAMPLES_AT))
    open(os.path.join(d, 'samples.bin'), 'wb').write(data)
    open(os.path.join(d, 'samples_dir.bin'), 'wb').write(directory)
    with open(os.path.join(d, 'voices.h'), 'w') as f:
        f.write('/* The FM voices\' samples (tools/snes/sound.py): sample k + 1 is voice k. */\n')
        f.write('#define VOICES %d\n' % len(voices))
        for name, fmt in (('hash', 'uint16_t'), ('n', 'uint8_t'), ('adsr1', 'uint8_t'),
                          ('adsr2', 'uint8_t'), ('release', 'uint8_t')):
            f.write('static const %s voice_%s[VOICES] = {\n' % (fmt, name))
            vals = ['0x%X' % v[name] for v in voices]
            for k in range(0, len(vals), 12):
                f.write('    %s,\n' % ', '.join(vals[k:k + 12]))
            f.write('};\n')
    print('%d voices, %d bytes of samples' % (len(voices), len(data)))


if __name__ == '__main__':
    main()
