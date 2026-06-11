/* SPDX-License-Identifier: GPL-2.0 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <unistd.h>

#define MP1_DEV "/dev/mp1"
#define SMN_LOCK "/run/lock/ps5-power.lock"
#define SMN_BOOST_STATE "/run/ps5-power.boost"

#ifndef _IO
#define _IOC_NRBITS 8
#define _IOC_TYPEBITS 8
#define _IOC_SIZEBITS 14
#define _IOC_DIRBITS 2
#define _IOC_NRSHIFT 0
#define _IOC_TYPESHIFT (_IOC_NRSHIFT + _IOC_NRBITS)
#define _IOC_SIZESHIFT (_IOC_TYPESHIFT + _IOC_TYPEBITS)
#define _IOC_DIRSHIFT (_IOC_SIZESHIFT + _IOC_SIZEBITS)
#define _IOC_NONE 0U
#define _IOC(dir, type, nr, size) \
	(((dir) << _IOC_DIRSHIFT) | ((type) << _IOC_TYPESHIFT) | \
	 ((nr) << _IOC_NRSHIFT) | ((size) << _IOC_SIZESHIFT))
#define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#endif

#define MP1_IOC_MAGIC 'M'
#define MP1_BOOST_ENTER _IO(MP1_IOC_MAGIC, 1)
#define MP1_BOOST_EXIT _IO(MP1_IOC_MAGIC, 2)

extern int ioctl(int fd, unsigned long request, ...);

static uint32_t read_state(int fd)
{
	char buf[32];
	ssize_t len;

	if (lseek(fd, 0, SEEK_SET) < 0)
		return 0;
	len = read(fd, buf, sizeof(buf) - 1);
	if (len <= 0)
		return 0;
	buf[len] = 0;
	return (uint32_t)strtoul(buf, NULL, 0);
}

static int write_state(int fd, uint32_t state)
{
	if (ftruncate(fd, 0) < 0 || lseek(fd, 0, SEEK_SET) < 0)
		return -1;
	return dprintf(fd, "0x%x\n", state) < 0 ? -1 : 0;
}

static int boost_ioctl(int enable)
{
	int fd, ret;

	fd = open(MP1_DEV, O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror("open " MP1_DEV);
		return -1;
	}

	ret = ioctl(fd, enable ? MP1_BOOST_ENTER : MP1_BOOST_EXIT);
	if (ret < 0)
		perror(enable ? "MP1_BOOST_ENTER" : "MP1_BOOST_EXIT");
	close(fd);
	return ret < 0 ? -1 : 0;
}

static int set_boost(int enable)
{
	int lock_fd, state_fd, ret = 0;

	lock_fd = open(SMN_LOCK, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (lock_fd < 0) {
		perror("open " SMN_LOCK);
		return -1;
	}
	if (flock(lock_fd, LOCK_EX) < 0) {
		perror("flock " SMN_LOCK);
		close(lock_fd);
		return -1;
	}

	state_fd = open(SMN_BOOST_STATE, O_CREAT | O_RDWR | O_CLOEXEC, 0644);
	if (state_fd < 0) {
		perror("open " SMN_BOOST_STATE);
		ret = -1;
		goto out_unlock;
	}

	if (enable) {
		if (!read_state(state_fd) && boost_ioctl(1) < 0)
			ret = -1;
		if (!ret && write_state(state_fd, 0x1) < 0) {
			perror("write " SMN_BOOST_STATE);
			ret = -1;
		}
	} else {
		if (boost_ioctl(0) < 0)
			ret = -1;
		if (!ret && write_state(state_fd, 0) < 0) {
			perror("write " SMN_BOOST_STATE);
			ret = -1;
		}
	}

	close(state_fd);
out_unlock:
	flock(lock_fd, LOCK_UN);
	close(lock_fd);
	return ret;
}

static void usage(const char *prog)
{
	fprintf(stderr, "usage: %s on|off|status\n", prog);
}

int main(int argc, char **argv)
{
	if (argc != 2) {
		usage(argv[0]);
		return 2;
	}

	if (!strcmp(argv[1], "on")) {
		if (set_boost(1))
			return 1;
		puts("boost=on state=0x1");
		return 0;
	}
	if (!strcmp(argv[1], "off")) {
		if (set_boost(0))
			return 1;
		puts("boost=off state=0x0");
		return 0;
	}
	if (!strcmp(argv[1], "status")) {
		int fd = open(SMN_BOOST_STATE, O_RDONLY | O_CLOEXEC);
		uint32_t state = fd < 0 ? 0 : read_state(fd);
		if (fd >= 0)
			close(fd);
		printf("boost_state=0x%x\n", state);
		return 0;
	}

	usage(argv[0]);
	return 2;
}
