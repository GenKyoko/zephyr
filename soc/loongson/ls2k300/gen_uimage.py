#!/usr/bin/env python3
# Copyright (c) 2026 Zephyr Project Contributors
# SPDX-License-Identifier: Apache-2.0
#
# Create a u-boot legacy uImage from a built Zephyr image.
#
# The 2K0300 vendor u-boot boots its kernel with the legacy image flow:
#
#     sf probe; sf read ${fdt_addr} dtb
#     ext4load mmc 0:${syspart} ${loadaddr} /boot/uImage
#     bootm
#
# u-boot names the LoongArch architecture "loongarch" in its image
# architecture table, so a plain legacy uImage works for LoongArch as well
# (there is no IH_ARCH_LOONGARCH symbol, the enum member is IH_ARCH_LA).
#
# The uImage header is built here instead of with mkimage so that the image
# can be produced by the Zephyr build without requiring the u-boot host tools.

import argparse
import binascii
import struct
import sys
import time

IH_MAGIC = 0x27051956
IH_OS_LINUX = 5
IH_ARCH_LA = 27
IH_TYPE_KERNEL = 2
IH_COMP_NONE = 0
IH_HDR_SIZE = 64

PT_LOAD = 1

# LoongArch address windows: the cached/uncached aliases of DDR are
# 0x9000_0000_0000_0000 / 0x8000_0000_0000_0000 while the legacy uImage header
# only stores 32 bit addresses. u-boot converts them back with map_sysmem()
# (PHYS_TO_CACHED), so the header must carry the physical address, i.e. the
# window selector bits masked off.
TO_PHYS_MASK = 0x0000FFFFFFFFFFFF
MAX32 = 0xFFFFFFFF


def elf_load_info(path):
    """Return (entry point, lowest load address) of an ELF image."""
    with open(path, "rb") as f:
        d = f.read()

    if d[:4] != b"\x7fELF":
        sys.exit("%s: not an ELF file" % path)

    is64 = d[4] == 2
    endian = "<" if d[5] == 1 else ">"

    if is64:
        entry = struct.unpack_from(endian + "Q", d, 0x18)[0]
        phoff = struct.unpack_from(endian + "Q", d, 0x20)[0]
        phentsize = struct.unpack_from(endian + "H", d, 0x36)[0]
        phnum = struct.unpack_from(endian + "H", d, 0x38)[0]
        addr_fmt, paddr_off = "Q", 0x18
    else:
        entry = struct.unpack_from(endian + "I", d, 0x18)[0]
        phoff = struct.unpack_from(endian + "I", d, 0x1C)[0]
        phentsize = struct.unpack_from(endian + "H", d, 0x2A)[0]
        phnum = struct.unpack_from(endian + "H", d, 0x2C)[0]
        addr_fmt, paddr_off = "I", 0x0C

    load = None
    for i in range(phnum):
        off = phoff + i * phentsize
        if struct.unpack_from(endian + "I", d, off)[0] != PT_LOAD:
            continue
        paddr = struct.unpack_from(endian + addr_fmt, d, off + paddr_off)[0]
        load = paddr if load is None else min(load, paddr)

    if load is None:
        sys.exit("%s: no PT_LOAD segment found" % path)

    return entry, load


def make_header(data, load, entry, name, timestamp):
    name = name.encode()[:31].ljust(32, b"\0")
    dcrc = binascii.crc32(data) & 0xFFFFFFFF

    hdr = struct.pack(">IIIIII", IH_MAGIC, 0, timestamp, len(data), load, entry)
    hdr += struct.pack(">I", dcrc)
    hdr += bytes([IH_OS_LINUX, IH_ARCH_LA, IH_TYPE_KERNEL, IH_COMP_NONE])
    hdr += name
    assert len(hdr) == IH_HDR_SIZE

    hcrc = binascii.crc32(hdr) & 0xFFFFFFFF
    hdr = hdr[:4] + struct.pack(">I", hcrc) + hdr[8:]
    return hdr


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--elf", required=True, help="path to zephyr.elf")
    ap.add_argument("--bin", required=True, help="path to zephyr.bin")
    ap.add_argument("--output", required=True, help="path of the uImage to write")
    ap.add_argument("--name", default="Zephyr", help="image name stored in the header")
    args = ap.parse_args()

    entry, load = elf_load_info(args.elf)

    load_phys = load & TO_PHYS_MASK
    entry_phys = entry & TO_PHYS_MASK

    for what, addr in (("load", load_phys), ("entry", entry_phys)):
        if addr > MAX32:
            sys.exit("%s: %s address 0x%x cannot be stored in the 32 bit "
                     "uImage header" % (args.elf, what, addr))

    with open(args.bin, "rb") as f:
        data = f.read()

    hdr = make_header(data, load_phys, entry_phys, args.name, int(time.time()))

    with open(args.output, "wb") as f:
        f.write(hdr)
        f.write(data)

    print("uImage: %s" % args.output)
    print("  load 0x%08x (virtual 0x%016x)" % (load_phys, load))
    print("  entry 0x%08x (virtual 0x%016x)" % (entry_phys, entry))
    print("  size %u bytes" % len(data))


if __name__ == "__main__":
    main()
