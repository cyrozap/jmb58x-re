#!/usr/bin/env python3
# SPDX-License-Identifier: 0BSD

# Copyright (C) 2022-2023 by Forest Crossman <cyrozap@gmail.com>
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


def split_bits(bits, splits=[]):
    if sum(splits) > 32:
        raise ValueError("Sum of splits > 32: {}".format(sum(splits)))

    sections = []

    prev = 0
    for split in splits:
        sections.append(bits[prev:prev+split])
        prev += split

    sections.append(bits[prev:])

    return sections

def main():
    project_dir = pathlib.Path(__file__).resolve().parents[0]
    default_data_dir = str(project_dir/"data")

    parser = argparse.ArgumentParser()
    parser.add_argument("-d", "--data-dir", type=str, default=default_data_dir, help="The YAML data directory. Default is \"{}\"".format(default_data_dir))
    parser.add_argument("-c", "--ignore-counter", action="store_true", default=False, help="Set this flag to mask off the instruction counter (useful for diffs).")
    parser.add_argument("image", type=str, help="The JMB58x flash image.")
    args = parser.parse_args()

    image = open(args.image, 'rb').read()

    did, vid = struct.unpack_from('<HH', image, 0)
    print("PCI ID: {:04x}:{:04x}".format(vid, did))

    offset = 8
    header_data = image[offset:]

    reg_names = []
    try:
        yaml_path = pathlib.Path(args.data_dir) / "regs-jmb58x.yaml"

        import yaml
        doc = yaml.safe_load(open(yaml_path, 'r'))
        xdata = doc.get('registers', dict()).get('bar5', [])
        for reg in xdata:
            start = reg['start']
            end = reg['end']
            name = reg['name']
            reg_names.append((start, end, name))
    except FileNotFoundError:
        pass
    except ModuleNotFoundError:
        pass

    print("Data        |  Instruction")
    for instr, data in struct.iter_unpack('<II', header_data):
        if args.ignore_counter:
            instr = instr & 0x0fffffff
        instr_split = split_bits("{:032b}".format(instr), [4, 4, 5, 9, 4, 4])
        instr_bin = ' '.join(instr_split)
        instr_hex = ' '.join([hex(int(n, 2)) for n in instr_split])
        info = ""
        if instr & (1 << 24):
            info += "  |  "
            formatted_name = ""
            addr = instr & 0x1fff
            for start, end, name in reg_names:
                if addr == start:
                    formatted_name = " ({})".format(name)
            info += "BAR5[0x{:04x}]{} <= 0x{:08x}".format(addr, formatted_name, data)
        elif instr & (1 << 27):
            info += "  |  "
            info += "Option ROM offset: 0x{:08x}".format(data)
        print("0x{:08x}  |  0x{:08x}  [ {} ]  [ {} ]{}".format(data, instr, instr_bin, instr_hex, info))

        offset += 8

        if instr & (1 << 27):
            break

    if offset < len(image):
        print("Header version: {}.{}.{}.{}".format(*struct.unpack_from('BBBB', image, offset)))


if __name__ == "__main__":
    main()
