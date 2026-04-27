# SPDX-License-Identifier: GPL-2.0-or-later
# Out-of-tree kernel module — i.MX audio tap (V3.2.2 NPU tap)
#
# Yocto module.bbclass provides KERNEL_SRC pointing to the target's
# kernel sysroot. Fall back to KDIR or running kernel for manual builds.

obj-m := imx-audio-tap.o

ccflags-y += -I$(src)

SRC := $(shell pwd)

ifeq ($(KERNEL_SRC),)
KERNEL_SRC ?= $(KDIR)
endif
ifeq ($(KERNEL_SRC),)
KERNEL_SRC := /lib/modules/$(shell uname -r)/build
endif

all:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules

clean:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) clean
	rm -f Module.markers Module.symvers modules.order

modules_install:
	$(MAKE) -C $(KERNEL_SRC) M=$(SRC) modules_install
