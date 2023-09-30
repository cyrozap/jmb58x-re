# JMB58x Reverse Engineering

The JMicron JMB582 and JMB585 are SATA 6Gb/s host controllers with PCIe Gen3
interfaces. The JMB582 has one PCIe Gen3 lane and two SATA ports, while the
JMB585 has two PCIe Gen3 lanes and five SATA ports.


## Quick start


### Software dependencies

* Python 3
* Documentation generator:
  * [lxml][lxml]
  * [Python-Markdown][python-markdown]
  * [PyYAML][pyyaml]


### Procedure

1. Install dependencies.
2. Dump the flash from your JMB585 card.
3. Parse and print the configuration flash with `./tools/parse.py ...`, where
   `...` is the name of the binary you got when you dumped your JMB585 card's
   flash.
4. Run `make doc` to generate XHTML documentation in
   [doc/generated](doc/generated).


### Obtaining a flash image

In addition to dumping the flash of a JMB58x device, a flash image and
proprietary flash tool for JMB58x devices can be downloaded from
[here][station-drivers].


## Reverse engineering notes

See [doc/Notes.md](doc/Notes.md).


## License

Except where otherwise stated:

* All software in this repository (e.g., tools for parsing and generating flash
  images/configuration data, etc.) is made available under the
  [Zero-Clause BSD (0BSD) license][license].
* All copyrightable content that is not software (e.g., reverse engineering
  notes, this README file, etc.) is licensed under the
  [Creative Commons Attribution-ShareAlike 4.0 International License][cc-by-sa].


[lxml]: https://lxml.de/
[python-markdown]: https://python-markdown.github.io/
[pyyaml]: https://pyyaml.org/
[station-drivers]: https://www.station-drivers.com/index.php/en/component/remository/Drivers/Jmicron/JMB585-Sata-Controller/Jmicron-JMB585-Sata-Controller-Firmware-Version-255.00.00.20/lang,en-gb/
[license]: LICENSE.txt
[cc-by-sa]: https://creativecommons.org/licenses/by-sa/4.0/
