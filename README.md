# JMB585 Reverse Engineering

The JMB585 is a PCIe Gen3x2 5-port SATA 6Gb/s host controller.


## Quick start


### Software dependencies

* Python 3


### Procedure

1. Install dependencies.
2. Dump the flash from your JMB585 card.
3. Parse and print the configuration flash with `./dump.py ...`, where `...` is
   the name of the binary you got when you dumped your JMB585 card's flash.


## Reverse engineering notes

See [Notes.md](Notes.md).
