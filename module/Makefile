# SPDX-License-Identifier: GPL-2.0-only
# Out-of-tree build: make [KVER=<version>] [KDIR=<kernel build tree>]
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build

all: modules

removablefs_config.h: configure
	./configure $(KDIR)

modules: removablefs_config.h
	$(MAKE) -C $(KDIR) M=$(CURDIR) modules

clean:
	$(MAKE) -C $(KDIR) M=$(CURDIR) clean
	rm -f removablefs_config.h

.PHONY: all modules clean
