# PS5 CPU + GPU load-adaptive governors (userspace PoC, no kernel module).
#   make    build ps5_cpu_gov + ps5_gpu_gov
# They drive the SMU mailbox directly over PCI config space (../smn.h); root.

CC ?= gcc
CFLAGS ?= -O2 -Wall -Wextra

GOVS := ps5_cpu_gov ps5_gpu_gov

all: $(GOVS)

$(GOVS): %: %.c ../smn.h
	$(CC) $(CFLAGS) -o $@ $<

clean:
	rm -f $(GOVS)

.PHONY: all clean
