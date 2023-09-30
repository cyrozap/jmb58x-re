# SPDX-License-Identifier: 0BSD

# Copyright (C) 2023 by Forest Crossman <cyrozap@gmail.com>
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


DOC_SOURCES := $(wildcard data/regs-*.yaml)
DOC_TARGETS := $(DOC_SOURCES:data/%.yaml=doc/generated/%.xhtml)


all: doc

doc/generated/%.xhtml: data/%.yaml tools/generate_docs.py
	python3 tools/generate_docs.py -o $@ $<

doc: $(DOC_TARGETS)

clean:
	rm -f $(DOC_TARGETS)


.PHONY: clean doc
