# PS5 power-control -- userspace PoC, NO kernel module.
#
#   make          build ps5_cpu, ps5_df, ps5_gpu, and governors/
#   make clean
#
# Only needs gcc. The binaries talk to the SMU MP1 mailbox directly over PCI
# config space (see smn.h) and must be run as root.

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

TOOLS := ps5_cpu ps5_df ps5_gpu

all: $(TOOLS) governors

$(TOOLS): %: %.c smn.h
	$(CC) $(CFLAGS) -o $@ $<

governors:
	$(MAKE) -C governors

clean:
	rm -f $(TOOLS)
	$(MAKE) -C governors clean

.PHONY: all governors clean
