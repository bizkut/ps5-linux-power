obj-m += ps5_smu.o

KDIR ?= /lib/modules/$(shell uname -r)/build
PWD := $(shell pwd)
KERNEL_UAPI ?= $(KDIR)/include/uapi
USER_CFLAGS ?=
ifneq ($(wildcard $(KERNEL_UAPI)/linux/errno.h),)
USER_CFLAGS += -idirafter $(KERNEL_UAPI)
endif

all:
	$(MAKE) -C $(KDIR) M=$(PWD) modules

userspace:
	$(CC) $(USER_CFLAGS) $(CFLAGS) -Wall -Wextra -o ps5_smuctl ps5_smuctl.c

clean:
	$(MAKE) -C $(KDIR) M=$(PWD) clean
	rm -f ps5_smuctl
