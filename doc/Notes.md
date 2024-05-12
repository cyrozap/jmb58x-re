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

- The "configuration sequence" is a sequence of pairs of 32-bit little-endian
  words.
- The first word in each pair is the "instruction" word, while the second is a
  "data" word.
- The highest four bits of the instruction word (bits 31-28) contain a
  "sequence number", which starts at 1, increments by one for each instruction
  word, and simply wraps around to zero after sequence number 15 (0xf).
- The next highest four bits of the instruction word (bits 27-24) indicate the
  memory space to write the data word to.
  - 1: PCI BAR5 MMIO
  - 2: PCI configuration space
  - 4: Internal registers
  - 8: Option ROM flash address register (appears to only access this one
    register)
- The next highest four bits of the instruction word (bits 23-20) are the byte
  enable bits for the data word.
  - If a bit is set, the corresponding byte from the data word is written to the
    memory space.
  - If a bit is cleared, the corresponding byte from the data word is not
    written to the memory space.
  - Bit 20 corresponds to the byte at the lowest address in the data word.
  - Bit 23 corresponds to the byte at the highest address in the data word.
- The next highest bit of the instruction word (bit 19) is the write enable.
  - If this bit is set, the write to the memory space is enabled according to
    the byte enable bits.
  - If this bit is cleared, the write to the memory space is disabled
    completely.
- The remaining 19 bits of the instruction word (bits 18-0) are the address to
  write the data word to in the indicated memory space.
  - Writes to PCI BAR5 MMIO use byte addresses and are 32-bit word-aligned (all
    addresses are a multiple of four).
  - Writes to PCI configuration space use register indices (not byte addresses).
  - Writes to internal registers have various addressing modes depending on the
    address:
    - Writes below 0x1000 use byte addresses and are 32-bit word-aligned (all
      addresses are a multiple of four).
    - Writes at or above 0x1000 use register indices (not byte addresses).
- The configuration sequence is terminated by a write to the Option ROM flash
  address register (memory space 8).


## Reading and Writing Flash over PCIe

JMB58x chips each contain an SPI controller, whose registers are located in the
AHCI Vendor Specific registers range (0xA0 to 0xFF, inclusive) in the MMIO space
of PCI BAR 5. The details of these registers are documented in
`generated/regs-jmb58x.xhtml` (run `make doc` in the root directory of this
repository to generate the XHTML documentation from
[`../data/regs-jmb58x.yaml`](../data/regs-jmb58x.yaml)). Additionally, a live
preview of the generated register documentation can be found
[here][htmlpreview]).

TODO: Explain how to use the SPI controller.


## Configuration and Debug over SPI

In addition to the SPI controller port connected to the flash, the JMB58x also
has an SPI peripheral port that can be used by another SPI controller to access
the three memory spaces inside the JMB58x: PCI BAR5 MMIO, PCI configuration
space, and internal registers.

Unlike the controller port, which is on GPIO pins 1-5, this port appears to use
GPIO pins 12-15, and is only available after being configured by either the host
over PCIe or the configuration sequence in flash. The specific register writes
needed to enable this are currently unknown.

Once the port is available, commands can be sent to it to read and write the
previosuly-identified memory spaces, as well as to perform several other
functions. The commands I've found so far are defined as follows:

- Write/Read Memory
  - Command
    - `B`: Command byte
      - `Bits [7:4]`: Memory space type
        - 1: PCI BAR5 MMIO
        - 2: PCI configuration space
        - 4: Internal registers
      - `Bits [3:1]`: 1
      - `Bit [0]`: Read-not-Write
        - 0: Write
        - 1: Read
    - `3B`: Byte/write enable flags and 19-bit address to write/read
      - `B`: Byte/write enable flags and high address bits
        - Write:
          - `Bits [7:4]`: Byte enable for data bytes `[3:0]`
          - `Bit [3]`: Write enable
        - Read:
          - `Bits [7:3]`: Zero
        - `Bits [2:0]`: Address bits `[18:16]`
      - `B`: Address bits `[15:8]`
      - `B`: Address bits `[7:0]`
    - `<I`: Data word (writes only)
  - Response
    - `B`: Dummy byte (always zero)
    - `<I`: Data word
- Read ID
  - Command
    - `B`: 0x9F
  - Response
    - `<I`: Device ID (should be 0x197b0585)

There are also several commands that generate responses from the chip, but for
which I don't know their functions. Further investigation of these commands is
needed:

- 0xE1 through 0xEC


[htmlpreview]: https://htmlpreview.github.io/?https://github.com/cyrozap/jmb58x-re/blob/master/tools/doc-preview.html
