# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree kernel module — i.MX audio tap (V3.2.2 NPU tap)

obj-m := imx-audio-tap.o

ccflags-y += -I$(src)

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

modules_install:
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
