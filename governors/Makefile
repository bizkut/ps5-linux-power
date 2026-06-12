# PS5 CPU + GPU load-adaptive governors (userspace PoC, no kernel module).
#   make    build ps5_cpu_gov + ps5_gpu_gov
# They drive the SMU mailbox directly over PCI config space (../smn.h); root.

CC ?= gcc
CPPFLAGS += -idirafter ../include
KERNEL_UAPI ?= /lib/modules/$(shell uname -r)/build/include/uapi
ifneq ($(wildcard $(KERNEL_UAPI)/linux/errno.h),)
CPPFLAGS += -idirafter $(KERNEL_UAPI)
endif
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib/ps5-linux-cpuclock
INSTALL ?= install

GOVS := ps5_cpu_gov ps5_gpu_gov ps5_fan_gov

all: $(GOVS)

$(GOVS): %: %.c ../smn.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	rm -f $(GOVS)

install: all
	$(INSTALL) -d $(DESTDIR)$(LIBDIR)/governors
	$(INSTALL) -m 0755 $(GOVS) run-governors.sh ps5govctl ps5gov-smoke.sh ps5gov-trace.sh ps5gov-fan-validate.sh $(DESTDIR)$(LIBDIR)/governors/
	$(INSTALL) -m 0644 ps5gov.conf $(DESTDIR)$(LIBDIR)/governors/

.PHONY: all clean install
