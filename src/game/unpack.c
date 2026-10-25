/* Run-length unpackers for RAM data (tile maps, logo bitmaps). */

#include "game.h"

/*
 * Bytes with a counted code before each run ($8538): 0 ends; $80+n copies
 * the next n bytes; n (1-$7F) writes n bytes counting up from the next
 * byte. A count of 0 means 256. Returns the end of the destination.
 */
uint32_t rle_unpack(uint32_t src, uint32_t dst, uint32_t *src_end)
{
    uint8_t c;
    while ((c = rd8(src++)) != 0) {
        int n = (uint8_t)((c & 0x7F) - 1) + 1, k;
        if (c & 0x80) {
            for (k = 0; k < n; k++)
                wr8(dst++, rd8(src++));
        } else {
            uint8_t v = rd8(src++);
            for (k = 0; k < n; k++)
                wr8(dst++, v++);
        }
    }
    if (src_end)
        *src_end = src;
    return dst;
}

void rle_unpack_68k(void)
{
    uint32_t end;
    cpu.a[4] = rle_unpack(cpu.a[0], cpu.a[4], &end);
    cpu.a[0] = end;
}

/*
 * A word map in two byte planes ($84F0): unpack to $FFFB00, then expand
 * those runs (0 ends a plane; $80+n literal bytes, n repeats of the next
 * byte) into the high bytes, then the low bytes, of the words at $FFEB00.
 */
#define UNPACK_BUFFER    0xFFFB00
#define UNPACK_MAP       0xFFEB00
void map_unpack(uint32_t src)
{
    uint32_t in = UNPACK_BUFFER, out;
    int plane;
    rle_unpack(src, UNPACK_BUFFER, 0);
    for (plane = 0; plane < 2; plane++) {
        uint8_t c;
        out = UNPACK_MAP + plane;
        while ((c = rd8(in++)) != 0) {
            int n = (uint8_t)((c & 0x7F) - 1) + 1, k;
            if (c & 0x80) {
                for (k = 0; k < n; k++, out += 2)
                    wr8(out, rd8(in++));
            } else {
                uint8_t v = rd8(in++);
                for (k = 0; k < n; k++, out += 2)
                    wr8(out, v);
            }
        }
    }
}

void map_unpack_68k(void)
{
    map_unpack(cpu.a[0]);
    cpu.d[0] = 2;
    set_dn(1, 2, 2);
}
