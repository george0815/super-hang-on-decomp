#!/usr/bin/env python3
"""Fill in a LoROM image's header checksum and its complement."""
import sys

path = sys.argv[1]
rom = bytearray(open(path, 'rb').read())
rom[0x7FDC:0x7FE0] = b'\xFF\xFF\x00\x00'
total = sum(rom) & 0xFFFF
rom[0x7FDC:0x7FE0] = bytes([~total & 0xFF, (~total >> 8) & 0xFF, total & 0xFF, total >> 8])
open(path, 'wb').write(rom)
