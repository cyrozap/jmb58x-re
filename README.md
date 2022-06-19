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


## License

Except where otherwise stated:

* All software in this repository (e.g., tools for parsing and generating flash
  images/configuration data, etc.) is made available under the
  [Zero-Clause BSD (0BSD) license][license].
* All copyrightable content that is not software (e.g., reverse engineering
  notes, this README file, etc.) is licensed under the
  [Creative Commons Attribution-ShareAlike 4.0 International License][cc-by-sa].


[license]: LICENSE.txt
[cc-by-sa]: https://creativecommons.org/licenses/by-sa/4.0/
