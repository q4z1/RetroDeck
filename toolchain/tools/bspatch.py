#!/usr/bin/env python3
"""bspatch in Python: applies a BSDIFF40 patch (bsdiff 4.x).

Replaces bspatch.exe from mega65go - the MEGA65 ROM patch 920413.bdf
comes in this format.

Usage: bspatch.py <old-file> <new-file> <patch>
"""

import bz2
import sys


def offtin(b):
    """64-bit number in bsdiff's sign-magnitude representation."""
    v = int.from_bytes(b[:8], "little")
    if v & (1 << 63):
        return -(v & ~(1 << 63))
    return v


def bspatch(old, patch):
    if patch[:8] != b"BSDIFF40":
        raise ValueError("not a BSDIFF40 patch")
    ctrllen = offtin(patch[8:16])
    difflen = offtin(patch[16:24])
    newsize = offtin(patch[24:32])
    if ctrllen < 0 or difflen < 0 or newsize < 0:
        raise ValueError("corrupt patch header")

    ctrl = bz2.decompress(patch[32:32 + ctrllen])
    diff = bz2.decompress(patch[32 + ctrllen:32 + ctrllen + difflen])
    extra = bz2.decompress(patch[32 + ctrllen + difflen:])

    new = bytearray(newsize)
    oldpos = newpos = 0
    cpos = dpos = epos = 0
    while newpos < newsize:
        x = offtin(ctrl[cpos:cpos + 8])
        y = offtin(ctrl[cpos + 8:cpos + 16])
        z = offtin(ctrl[cpos + 16:cpos + 24])
        cpos += 24
        if x < 0 or y < 0 or newpos + x > newsize:
            raise ValueError("corrupt patch (diff)")
        # diff block: bytes of the old file plus the difference
        for i in range(x):
            o = old[oldpos + i] if 0 <= oldpos + i < len(old) else 0
            new[newpos + i] = (diff[dpos + i] + o) & 0xFF
        dpos += x
        newpos += x
        oldpos += x
        if newpos + y > newsize:
            raise ValueError("corrupt patch (extra)")
        # extra block: new bytes, taken over unchanged
        new[newpos:newpos + y] = extra[epos:epos + y]
        epos += y
        newpos += y
        oldpos += z
    return bytes(new)


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: bspatch.py <old-file> <new-file> <patch>")
    with open(sys.argv[1], "rb") as f:
        old = f.read()
    with open(sys.argv[3], "rb") as f:
        patch = f.read()
    new = bspatch(old, patch)
    with open(sys.argv[2], "wb") as f:
        f.write(new)


if __name__ == "__main__":
    main()
