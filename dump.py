#!/usr/bin/env python3

import argparse
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
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=str, help="The JMB585 flash image.")
    parser.add_argument("-c", "--ignore-counter", action="store_true", default=False, help="Set this flag to mask off the instruction counter (useful for diffs).")
    args = parser.parse_args()

    image = open(args.image, 'rb').read()

    did, vid = struct.unpack_from('<HH', image, 0)
    print("PCI ID: {:04x}:{:04x}".format(vid, did))

    header = image[:0xc00]
    header_data = header[8:]

    print("Data        |  Instruction")
    for instr, data in struct.iter_unpack('<II', header_data):
        if args.ignore_counter:
            instr = instr & 0x0fffffff
        instr_split = split_bits("{:032b}".format(instr), [4, 4, 5, 9, 4, 4])
        instr_bin = ' '.join(instr_split)
        instr_hex = ' '.join([hex(int(n, 2)) for n in instr_split])
        print("0x{:08x}  |  0x{:08x}  [ {} ]  [ {} ]".format(data, instr, instr_bin, instr_hex))


if __name__ == "__main__":
    main()
