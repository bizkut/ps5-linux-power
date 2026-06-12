# PS5 power-control -- userspace PoC, NO kernel module.
#
#   make          build ps5_cpu, ps5_df, ps5_gpu, ps5_boost, and governors/
#   make clean
#
# Only needs gcc. The binaries talk to the SMU MP1 mailbox directly over PCI
# config space (see smn.h) and must be run as root.

CC ?= gcc
CPPFLAGS += -idirafter include
KERNEL_UAPI ?= /lib/modules/$(shell uname -r)/build/include/uapi
ifneq ($(wildcard $(KERNEL_UAPI)/linux/errno.h),)
CPPFLAGS += -idirafter $(KERNEL_UAPI)
endif
CFLAGS ?= -O2 -Wall -Wextra
PREFIX ?= /usr/local
BINDIR ?= $(PREFIX)/bin
LIBDIR ?= $(PREFIX)/lib/ps5-linux-cpuclock
SYSCONFDIR ?= /etc
CONFIGDIR ?= $(SYSCONFDIR)/ps5-linux-cpuclock
SYSTEMD_DIR ?= /etc/systemd/system
INSTALL ?= install

TOOLS := ps5_cpu ps5_df ps5_gpu ps5_boost

all: $(TOOLS) governors

$(TOOLS): %: %.c smn.h
	$(CC) $(CPPFLAGS) $(CFLAGS) -o $@ $<

governors:
	$(MAKE) -C governors

clean:
	rm -f $(TOOLS)
	$(MAKE) -C governors clean

install: all
	$(INSTALL) -d $(DESTDIR)$(BINDIR)
	$(INSTALL) -d $(DESTDIR)$(LIBDIR)
	$(INSTALL) -m 0755 $(TOOLS) $(DESTDIR)$(LIBDIR)/
	$(INSTALL) -m 0755 governors/ps5govctl $(DESTDIR)$(BINDIR)/ps5govctl
	$(MAKE) -C governors install DESTDIR="$(DESTDIR)" PREFIX="$(PREFIX)" LIBDIR="$(LIBDIR)"

install-systemd: install
	$(INSTALL) -d $(DESTDIR)$(CONFIGDIR)
	@if [ ! -e "$(DESTDIR)$(CONFIGDIR)/ps5gov.conf" ]; then \
		$(INSTALL) -m 0644 governors/ps5gov.conf $(DESTDIR)$(CONFIGDIR)/ps5gov.conf; \
	fi
	$(INSTALL) -d $(DESTDIR)$(SYSTEMD_DIR)
	$(INSTALL) -m 0644 governors/ps5gov.service $(DESTDIR)$(SYSTEMD_DIR)/ps5gov.service

.PHONY: all governors clean install install-systemd
