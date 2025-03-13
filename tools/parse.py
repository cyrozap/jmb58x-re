#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD

# Copyright (C) 2022-2025 by Forest Crossman <cyrozap@gmail.com>
#
# Permission to use, copy, modify, and/or distribute this software for
# any purpose with or without fee is hereby granted.
#
# THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL
# WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED
# WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE
# AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
# DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
# PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
# TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
# PERFORMANCE OF THIS SOFTWARE.


import argparse
import pathlib
import struct


def main() -> None:
    project_dir: pathlib.Path = pathlib.Path(__file__).resolve().parents[1]
    default_data_dir: str = str(project_dir/"data")

    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--data-dir", type=str, default=default_data_dir, help="The YAML data directory. Default is \"{}\"".format(default_data_dir))
    parser.add_argument("-c", "--ignore-counter", action="store_true", default=False, help="Set this flag to mask off the instruction counter (useful for diffs).")
    parser.add_argument("image", type=str, help="The JMB58x flash image.")
    args = parser.parse_args()

    image: bytes = open(args.image, 'rb').read()

    did, vid = struct.unpack_from('<HH', image, 0)
    print("PCI ID: {:04x}:{:04x}".format(vid, did))

    offset: int = 8
    header_data: bytes = image[offset:]

    reg_names: dict[str, list[tuple[int, int, str]]] = {
        "bar5": [],
        "cfg": [],
        "reg": [],
    }
    try:
        yaml_path: pathlib.Path = pathlib.Path(args.data_dir) / "regs-jmb58x.yaml"

        import yaml  # type: ignore[import-untyped]
        doc = yaml.safe_load(open(yaml_path, 'r'))
        registers: dict[str, list] = doc.get('registers', dict())
        for space in reg_names.keys():
            for reg in registers.get(space, []):
                start = reg['start']
                end = reg['end']
                name = reg['name']
                reg_names[space].append((start, end, name))
    except FileNotFoundError:
        pass
    except ModuleNotFoundError:
        pass

    def get_reg_name(space: str, addr: int) -> str:
        for start, end, name in reg_names[space]:
            reg_size = end + 1 - start
            if reg_size <= 4:
                if start == addr:
                    return name
                elif start < addr and addr <= end:
                    return "{}[{}]".format(name, addr - start)
            else:
                if start <= addr and addr <= end:
                    return "{}[{}]".format(name, addr - start)

        return ""

    print("Instruction  |  Data")
    for instr, data in struct.iter_unpack('<II', header_data):
        if args.ignore_counter:
            instr = instr & 0x0fffffff
        info: str = ""

        if instr & (1 << 24):
            info += "  |  "
            addr = instr & 0x1fff
            formatted_name = get_reg_name("bar5", addr)
            if formatted_name:
                formatted_name = " ({})".format(formatted_name)
            info += "BAR5[0x{:04x}]{} <= 0x{:08x}".format(addr, formatted_name, data)
        elif instr & (1 << 25):
            info += "  |  "
            addr = 4 * (instr & 0x3ff)
            formatted_name = get_reg_name("cfg", addr)
            if formatted_name:
                formatted_name = " ({})".format(formatted_name)
            info += "CFG[0x{:03x}]{} <= 0x{:08x}".format(addr, formatted_name, data)
        elif instr & (1 << 26):
            info += "  |  "
            addr = instr & 0x7ffff
            formatted_name = get_reg_name("reg", addr)
            if formatted_name:
                formatted_name = " ({})".format(formatted_name)
            info += "REG[0x{:04x}]{} <= 0x{:08x}".format(addr, formatted_name, data)
        elif instr & (1 << 27):
            info += "  |  "
            info += "Option ROM offset: 0x{:08x}".format(data)

        mask: int = 0
        if instr & (1 << 19):
            bytes_enabled: int = (instr >> 20) & 0xf
            for i in range(4):
                if bytes_enabled & (1 << i):
                    mask |= 0xff << (8 * i)

        print(" 0x{:08x}  |  0x{:08x} (mask: 0x{:08x}){}".format(instr, data, mask, info))

        offset += 8

        if instr & (1 << 27):
            break

    if offset < len(image):
        print("Header version: {}.{}.{}.{}".format(*struct.unpack_from('BBBB', image, offset)))


if __name__ == "__main__":
    main()
