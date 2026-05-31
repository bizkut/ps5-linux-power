# Build the kernel modules + userspace tools.
#
#   make            build all .ko modules and userspace tools
#   make clean
#
# Needs kernel build headers at /lib/modules/$(uname -r)/build and gcc.

obj-m += ps5_corepstate_ctl.o      # CPU core P-state control (/dev/ps5cpc)
obj-m += ps5_dfpstate_ctl.o        # DF memory/fabric P-state control (/dev/ps5dfc)
obj-m += ps5_dffreq_probe.o        # read-only FCLK/UCLK-per-DF-state probe

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD  := $(shell pwd)
CC   ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

TOOLS := ps5_cpc pstate_msr ps5_dfc

all: module $(TOOLS)

module:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

ps5_cpc: ps5_cpc.c
	$(CC) $(CFLAGS) -o $@ $<

pstate_msr: pstate_msr.c
	$(CC) $(CFLAGS) -o $@ $<

ps5_dfc: ps5_dfc.c
	$(CC) $(CFLAGS) -o $@ $<

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f $(TOOLS)

.PHONY: all module clean
