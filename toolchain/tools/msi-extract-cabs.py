#!/usr/bin/env python3
"""Pulls the embedded CAB archives out of a Windows Installer (.msi).

An MSI is an OLE compound file; the actual files are stored in CAB
archives kept as streams inside it. These streams are recognized here by
the CAB signature "MSCF" and saved as <name>.cab; bsdtar or cabextract
can unpack them afterwards.

Needs olefile: toolchain/setup.sh fetches it into toolchain/src/python and
puts that on PYTHONPATH.

Usage: msi-extract-cabs.py <file.msi> <target-directory>
"""

import os
import re
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "..", "python"))
import olefile  # noqa: E402


def main():
    if len(sys.argv) != 3:
        sys.exit("usage: msi-extract-cabs.py <file.msi> <target-directory>")
    msi, outdir = sys.argv[1], sys.argv[2]
    os.makedirs(outdir, exist_ok=True)
    ole = olefile.OleFileIO(msi)
    found = 0
    for entry in ole.listdir(streams=True, storages=False):
        with ole.openstream(entry) as s:
            if s.read(4) != b"MSCF":
                continue
        data = ole.openstream(entry).read()
        # MSI encodes stream names; ASCII is enough for the file name
        name = re.sub(r"[^A-Za-z0-9._-]", "_", "_".join(entry)).strip("_")
        out = os.path.join(outdir, (name or "stream%d" % found) + ".cab")
        with open(out, "wb") as f:
            f.write(data)
        print(out)
        found += 1
    if not found:
        sys.exit("no CAB archives found in %s" % msi)


if __name__ == "__main__":
    main()
