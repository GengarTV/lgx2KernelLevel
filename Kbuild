# SPDX-License-Identifier: GPL-2.0-only OR MIT
obj-m += gc550.o
gc550-y := kernel/gc550-core.o \
           kernel/gc550-cmd.o \
           kernel/gc550-video.o \
           kernel/gc550-audio.o

ifneq ($(CONFIG_GC550_DIAG),0)
ccflags-y += -DCONFIG_GC550_DIAG=1
gc550-y += kernel/gc550-diag.o
endif

