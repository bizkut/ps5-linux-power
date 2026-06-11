# PS5 CPU + GPU load-adaptive governors (userspace PoC, no kernel module).
#   make    build ps5_cpu_gov + ps5_gpu_gov
# They drive the SMU mailbox directly over PCI config space (../smn.h); root.

CC ?= gcc
CPPFLAGS += -idirafter ../include
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
LIBDIR ?= $(PREFIX)/lib/ps5-linux-cpuclock
INSTALL ?= install

GOVS := ps5_cpu_gov ps5_gpu_gov

all: $(GOVS)

$(GOVS): %: %.c ../smn.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

clean:
	rm -f $(GOVS)

install: all
	$(INSTALL) -d $(DESTDIR)$(LIBDIR)/governors
	$(INSTALL) -m 0755 $(GOVS) run-governors.sh ps5govctl $(DESTDIR)$(LIBDIR)/governors/
	$(INSTALL) -m 0644 ps5gov.conf $(DESTDIR)$(LIBDIR)/governors/

.PHONY: all clean install
