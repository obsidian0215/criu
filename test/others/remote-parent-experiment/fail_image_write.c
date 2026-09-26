/* Test-only LD_PRELOAD shim; never linked into CRIU. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <limits.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/uio.h>
#include <unistd.h>

static const char *fd_basename(int fd, char *path, size_t size)
{
	char proc[64];
	char *base;
	ssize_t length;

	snprintf(proc, sizeof(proc), "/proc/self/fd/%d", fd);
	length = readlink(proc, path, size - 1);
	if (length < 0)
		return NULL;
	path[length] = '\0';
	base = strrchr(path, '/');
	return base ? base + 1 : path;
}

static bool is_standard_pagemap(const char *base)
{
	size_t length = strlen(base);

	return !strncmp(base, "pagemap-", 8) && length > 12 &&
	       !strcmp(base + length - 4, ".img");
}

static bool is_custom_parent(const char *base)
{
	return !strncmp(base, "remote-parent-", 14) &&
	       strstr(base, ".img") != NULL;
}

static bool selected_image(int fd)
{
	const char *mode = getenv("CRIU_TEST_FAIL_IMAGE");
	const char *base;
	char path[PATH_MAX];

	if (!mode)
		return false;
	base = fd_basename(fd, path, sizeof(path));
	if (!base)
		return false;
	if (!strcmp(mode, "inventory"))
		return !strcmp(base, "inventory.img");
	if (!strcmp(mode, "pagemap"))
		return is_standard_pagemap(base);
	if (!strcmp(mode, "pages"))
		return !strncmp(base, "pages-", 6) && strstr(base, ".img") != NULL;
	if (!strcmp(mode, "remote-parent"))
		return is_custom_parent(base);
	if (!strcmp(mode, "source-parent"))
		return is_standard_pagemap(base) || is_custom_parent(base);
	return false;
}

static void fault_message(const char *operation)
{
	ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
	char message[128];
	int length;

	length = snprintf(message, sizeof(message),
			  "TEST_FAULT: image %s rejected with ENOSPC\n", operation);
	if (real_write && length > 0)
		real_write(STDERR_FILENO, message, (size_t)length);
}

ssize_t write(int fd, const void *data, size_t size)
{
	ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");

	if (!real_write) {
		errno = EIO;
		return -1;
	}
	if (selected_image(fd)) {
		fault_message("write");
		errno = ENOSPC;
		return -1;
	}
	return real_write(fd, data, size);
}

ssize_t writev(int fd, const struct iovec *iov, int count)
{
	ssize_t (*real_writev)(int, const struct iovec *, int) = dlsym(RTLD_NEXT, "writev");

	if (!real_writev) {
		errno = EIO;
		return -1;
	}
	if (selected_image(fd)) {
		fault_message("writev");
		errno = ENOSPC;
		return -1;
	}
	return real_writev(fd, iov, count);
}

static void disconnect_now(int fd)
{
	ssize_t (*real_write)(int, const void *, size_t) = dlsym(RTLD_NEXT, "write");
	static const char message[] = "TEST_FAULT: page server disconnected\n";

	if (real_write)
		real_write(STDERR_FILENO, message, sizeof(message) - 1);
	shutdown(fd, SHUT_RDWR);
	_exit(73);
}

ssize_t recv(int fd, void *data, size_t size, int flags)
{
	ssize_t (*real_recv)(int, void *, size_t, int) = dlsym(RTLD_NEXT, "recv");
	const char *mode = getenv("CRIU_TEST_DISCONNECT");
	struct command_frame {
		uint32_t cmd;
		uint64_t nr_pages;
		uint64_t vaddr;
		uint64_t dst_id;
	};
	static struct command_frame frame;
	static size_t received;
	ssize_t ret;

	if (!real_recv) {
		errno = EIO;
		return -1;
	}
	ret = real_recv(fd, data, size, flags);
	if (mode && !strcmp(mode, "close") && ret > 0 &&
	    size == sizeof(frame) - received) {
		memcpy((char *)&frame + received, data, (size_t)ret);
		received += (size_t)ret;
		if (received == sizeof(frame)) {
			received = 0;
			if ((frame.cmd & 0xffff) == 0x1023 ||
			    (frame.cmd & 0xffff) == 0x1024)
				disconnect_now(fd);
		}
	}
	return ret;
}

ssize_t splice(int in, off_t *in_off, int out, off_t *out_off,
	       size_t size, unsigned int flags)
{
	ssize_t (*real_splice)(int, off_t *, int, off_t *, size_t, unsigned int);
	const char *mode = getenv("CRIU_TEST_DISCONNECT");
	static size_t received;
	int type;
	socklen_t length = sizeof(type);
	bool socket_input = !getsockopt(in, SOL_SOCKET, SO_TYPE, &type, &length);
	bool disconnect = mode && !strcmp(mode, "transfer") && socket_input;
	ssize_t ret;

	real_splice = dlsym(RTLD_NEXT, "splice");
	if (!real_splice) {
		errno = EIO;
		return -1;
	}
	if (selected_image(out)) {
		fault_message("splice");
		errno = ENOSPC;
		return -1;
	}
	if (disconnect && received >= 8192)
		disconnect_now(in);
	ret = real_splice(in, in_off, out, out_off, size, flags);
	if (disconnect && ret > 0)
		received += (size_t)ret;
	return ret;
}
