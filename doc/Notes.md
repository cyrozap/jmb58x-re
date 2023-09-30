# Reverse Engineering Notes

These notes reflect my current understanding of how everything works.


## Hardware

- No CPU core or firmware as far as I can see--all functionality seems to
  exclusively be performed by hardware.
- There seems to be an SPI interface on the chip for in-circuit programming, but
  I haven't yet figured out how to use it.
- The chip has several GPIO pins, whose functions can be set by the
  configuration sequence.
- 25 MHz crystal oscillator.


## Flash Layout

- The first two bytes are the little-endian PCI device ID.
- The following two bytes are the little-endian PCI vendor ID.
- The next four bytes are never read.
- The configuration sequence starts at byte 8.
- The Option ROM is stored at an arbirtary, absolute offset in the flash, and
  that offset is set by a configuration instruction.

I'm not yet sure if the PCI device/vendor IDs are used as a magic to identify
that the configuration sequence is present, or if it's used to set the IDs in
the chip.


## The Configuration Sequence

The "configuration sequence" is a sequence of pairs of 32-bit little-endian
words. The first word in each pair is the "instruction" word, while the second
is a "data" word. The highest four bits of the instruction word (bits 31-28)
contain a "sequence number", which starts at 1, increments by one for each
instruction word, and simply wraps around to zero after sequence number 15
(0xf). The other bits I'm not so sure of--I think the low 16 bits of the
instruction word are sometimes an address and sometimes data, depending on the
instruction, but I don't yet know for certain. Also, I'm not sure the
instruction word is an "instruction" so much as it is just muxing between some
internal data/configuration ports.


## Reading and Writing Flash over PCIe

JMB58x chips each contain an SPI controller, whose registers are located in the
AHCI Vendor Specific registers range (0xA0 to 0xFF, inclusive) in the MMIO space
of PCI BAR 5. The details of these registers are documented in
[generated/regs-jmb58x.xhtml](generated/regs-jmb58x.xhtml) (run `make doc` in
the root directory of this repository to generate the XHTML documentation from
[../data/regs-jmb58x.yaml](../data/regs-jmb58x.yaml)).

TODO: Explain how to use the SPI controller.
