# SPDX-License-Identifier: GPL-2.0-only OR MIT
KVER ?= $(shell uname -r)
KDIR ?= /lib/modules/$(KVER)/build
PWD  := $(shell pwd)

# Automatically enable LLVM=1 if the target kernel was compiled with Clang
ifneq ($(shell grep -s "CONFIG_CC_IS_CLANG=y" $(KDIR)/.config),)
LLVM ?= 1
export LLVM
endif

all: modules

modules:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean

install: modules
	$(MAKE) -C $(KDIR) M=$(PWD) modules_install
	depmod -a

.PHONY: all modules clean install

