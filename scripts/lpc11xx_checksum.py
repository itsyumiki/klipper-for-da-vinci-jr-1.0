#!/usr/bin/env python
# Fix up an LPC11xx firmware image so the boot ROM accepts it
#
# The LPC11xx boot ROM only starts an image whose first eight vector
# table words sum to zero (vector 7 holds the checksum).  This script
# fills in that word, and checks the code read protection (CRP) word.
#
# Copyright (C) 2026  Klipper contributors
#
# This file may be distributed under the terms of the GNU GPLv3 license.
import sys, struct

CRP_OFFSET = 0x2FC
# Values that make the boot ROM lock out debugging/ISP; refuse to emit
# them by accident.
CRP_LOCKING = (0x12345678, 0x87654321, 0x43218765, 0x4E697370)

def main():
    if len(sys.argv) != 2:
        sys.stderr.write("Usage: %s <image.bin>\n" % (sys.argv[0],))
        sys.exit(1)
    fname = sys.argv[1]
    with open(fname, 'rb') as f:
        data = bytearray(f.read())
    if len(data) < 32:
        sys.stderr.write("Image too small to contain a vector table\n")
        sys.exit(1)
    # Checksum: two's complement of the sum of vectors 0-6
    vecs = list(struct.unpack_from("<7I", data, 0))
    csum = (-sum(vecs)) & 0xFFFFFFFF
    struct.pack_into("<I", data, 7 * 4, csum)
    if sum(struct.unpack_from("<8I", data, 0)) & 0xFFFFFFFF:
        sys.stderr.write("Internal error: bad vector checksum\n")
        sys.exit(1)
    # The CRP word must be reserved and must not enable protection
    if len(data) > CRP_OFFSET:
        if len(data) < CRP_OFFSET + 4:
            sys.stderr.write("Image truncates the CRP word\n")
            sys.exit(1)
        crp = struct.unpack_from("<I", data, CRP_OFFSET)[0]
        if crp != 0xFFFFFFFF:
            sys.stderr.write("Unexpected CRP word 0x%08x at 0x%x%s\n" % (
                crp, CRP_OFFSET,
                " (locks the chip!)" if crp in CRP_LOCKING else ""))
            sys.exit(1)
    with open(fname, 'wb') as f:
        f.write(data)

if __name__ == '__main__':
    main()
