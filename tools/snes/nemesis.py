"""Nemesis decompression, as src/game/nemesis.c does it ($855E)."""


def decode(rom, src):
    """The tiles at ROM address src, as Mega Drive 4bpp bytes."""
    header = rom[src] << 8 | rom[src + 1]
    xor_mode = header >> 15
    rows = (header & 0x7FFF) * 8
    src += 2

    # code table: per 8-bit lookup, (code length, run - 1, value)
    table = [None] * 256
    b = rom[src]
    src += 1
    while b != 0xFF:
        value = b & 0x0F
        while True:
            b = rom[src]
            src += 1
            if b >= 0x80:
                break
            run, length = b >> 4 & 7, b & 0x0F
            shift = 8 - length
            code = rom[src] << shift
            src += 1
            for k in range(1 << shift):
                table[code + k] = (length, run, value)

    out = bytearray()
    bits, avail = rom[src] << 8 | rom[src + 1], 16
    src += 2
    row = prev = pixels = 0

    def refill():
        nonlocal bits, avail, src
        if avail < 9:
            avail += 8
            bits = (bits << 8 | rom[src]) & 0xFFFF
            src += 1

    while True:
        peek = (bits >> (avail - 8)) & 0xFF
        if peek < 0xFC:
            length, count, value = table[peek]
            avail -= length
            refill()
        else:
            # escape: 6-bit marker, then 3 bits of run and 4 of value
            avail -= 6
            refill()
            avail -= 7
            inline = bits >> avail
            value, count = inline & 0x0F, (inline & 0x70) >> 4
            refill()
        for _ in range(count + 1):
            row = (row << 4 | value) & 0xFFFFFFFF
            pixels += 1
            if pixels == 8:
                if xor_mode:
                    prev ^= row
                    row = prev
                out += row.to_bytes(4, 'big')
                rows -= 1
                if rows == 0:
                    return bytes(out)
                row = pixels = 0
