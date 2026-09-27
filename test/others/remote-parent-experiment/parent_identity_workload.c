#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

static unsigned char *tracked;
static unsigned char *control;
static size_t page_size;
static unsigned char generation = 0x31;
static const char *state_path;
static const char *expected_path;
static sigset_t waitset;

static void die(const char *message)
{
	perror(message);
	_exit(111);
}

static bool page_is(const unsigned char *page, unsigned char value)
{
	size_t index;

	for (index = 0; index < page_size; index++) {
		if (page[index] != value)
			return false;
	}
	return true;
}

static unsigned char *map_page(uintptr_t fixed)
{
	void *address = NULL;
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	if (getenv("PARENT_IDENTITY_FIXED")) {
#ifdef MAP_FIXED_NOREPLACE
		address = (void *)fixed;
		flags |= MAP_FIXED_NOREPLACE;
#else
		errno = ENOTSUP;
		return MAP_FAILED;
#endif
	}

	return mmap(address, page_size, PROT_READ | PROT_WRITE, flags, -1, 0);
}

static unsigned char read_expected(void)
{
	char buffer[32];
	char *end;
	long value;
	int fd;
	ssize_t length;

	fd = open(expected_path, O_RDONLY | O_CLOEXEC);
	if (fd < 0)
		die("open expected");
	length = read(fd, buffer, sizeof(buffer) - 1);
	if (length <= 0)
		die("read expected");
	if (close(fd))
		die("close expected");
	buffer[length] = '\0';
	value = strtol(buffer, &end, 16);
	if (end == buffer || value < 0 || value > 0xff)
		die("parse expected");
	return (unsigned char)value;
}

static void write_state(const char *phase, const char *detail)
{
	char buffer[512];
	int fd;
	int length;

	length = snprintf(buffer, sizeof(buffer),
			  "%s pid=%d tracked=%p control=%p generation=%02x %s\n",
			  phase, getpid(), tracked, control, generation,
			  detail ? detail : "");
	if (length < 0 || (size_t)length >= sizeof(buffer))
		die("snprintf");
	fd = open(state_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
	if (fd < 0)
		die("open state");
	if (write(fd, buffer, (size_t)length) != length)
		die("write state");
	if (fsync(fd))
		die("fsync state");
	if (close(fd))
		die("close state");
}

int main(int argc, char **argv)
{
	int signal_number;

	if (argc != 3) {
		fprintf(stderr, "usage: %s STATE_FILE EXPECTED_FILE\n", argv[0]);
		return 2;
	}
	if (setsid() < 0)
		die("setsid");
	state_path = argv[1];
	expected_path = argv[2];
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 3;

	sigemptyset(&waitset);
	sigaddset(&waitset, SIGUSR1);
	sigaddset(&waitset, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &waitset, NULL))
		die("sigprocmask");

	tracked = map_page(UINT64_C(0x500000000000));
	control = map_page(UINT64_C(0x500000010000));
	if (tracked == MAP_FAILED || control == MAP_FAILED)
		die("mmap");
	memset(tracked, generation, page_size);
	memset(control, 0x72, page_size);
	write_state("READY", "");

	for (;;) {
		if (sigwait(&waitset, &signal_number))
			die("sigwait");
		if (signal_number == SIGUSR1) {
			generation = 0x62;
			memset(tracked, generation, page_size);
			memset(control, 0x83, page_size);
			write_state("GEN2", "");
		} else if (signal_number == SIGUSR2) {
			unsigned char expected = read_expected();
			bool ok = page_is(tracked, expected) &&
				  page_is(control, expected == 0x62 ? 0x83 : 0x72);
			write_state(ok ? "PASS" : "FAIL",
				    ok ? "" : "remote-parent-generation-mismatch");
			_exit(ok ? 0 : 1);
		}
	}
}
