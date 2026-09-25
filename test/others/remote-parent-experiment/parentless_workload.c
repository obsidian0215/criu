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
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static size_t page_size;
static unsigned char *hidden;
static unsigned char *stable;
static unsigned char *remapped;
static unsigned char *new_mapping;
static const char *state_path;
static pid_t child_pid = -1;
static sigset_t waitset;

static void die(const char *message)
{
	perror(message);
	_exit(111);
}

static void fill_page(unsigned char *page, unsigned char value)
{
	memset(page, value, page_size);
}

static bool check_page(const unsigned char *page, unsigned char value)
{
	size_t index;

	for (index = 0; index < page_size; index++) {
		if (page[index] != value)
			return false;
	}
	return true;
}

static void write_state(const char *phase, const char *detail)
{
	char buffer[1024];
	int fd;
	int length;

	length = snprintf(buffer, sizeof(buffer),
			  "%s pid=%d sid=%d child=%d hidden=%p stable=%p remapped=%p new=%p %s\n",
			  phase, getpid(), getsid(0), child_pid, hidden, stable,
			  remapped, new_mapping, detail ? detail : "");
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

static int child_loop(void)
{
	unsigned char *child_page;
	int signal_number;

	child_page = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			  MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (child_page == MAP_FAILED)
		return 20;
	fill_page(child_page, 0xc3);
	if (mprotect(child_page, page_size, PROT_READ))
		return 21;
	for (;;) {
		if (sigwait(&waitset, &signal_number))
			return 22;
		if (signal_number != SIGUSR2)
			continue;
		return check_page(child_page, 0xc3) ? 0 : 23;
	}
}

static void mutate_after_predump(void)
{
	void *replacement;

	/* This page was intentionally skipped by a read-mode pre-dump. */
	if (mprotect(hidden, page_size, PROT_READ))
		die("mprotect hidden");

	/* Ordinary dirty-page control. */
	fill_page(stable, 0x6b);

	/* Mapping replacement at the same virtual address. */
	if (munmap(remapped, page_size))
		die("munmap remapped");
	replacement = mmap(remapped, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
	if (replacement == MAP_FAILED || replacement != remapped)
		die("mmap fixed replacement");
	fill_page(remapped, 0xd4);
	if (mprotect(remapped, page_size, PROT_READ))
		die("mprotect remapped");

	/* Brand-new VMA after the pre-dump. */
	new_mapping = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			   MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (new_mapping == MAP_FAILED)
		die("mmap new mapping");
	fill_page(new_mapping, 0xe5);
	if (mprotect(new_mapping, page_size, PROT_READ))
		die("mprotect new mapping");

	/* Brand-new process after the pre-dump. */
	child_pid = fork();
	if (child_pid < 0)
		die("fork");
	if (!child_pid)
		_exit(child_loop());

	write_state("MUTATED", "");
}

static void verify_after_restore(void)
{
	int status;
	bool ok = true;

	ok &= check_page(hidden, 0xa5);
	ok &= check_page(stable, 0x6b);
	ok &= check_page(remapped, 0xd4);
	ok &= new_mapping && check_page(new_mapping, 0xe5);
	if (child_pid <= 0 || kill(child_pid, SIGUSR2))
		ok = false;
	if (child_pid > 0) {
		if (waitpid(child_pid, &status, 0) != child_pid ||
		    !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			ok = false;
	}
	write_state(ok ? "PASS" : "FAIL", ok ? "" : "memory-or-child-mismatch");
	_exit(ok ? 0 : 1);
}

int main(int argc, char **argv)
{
	unsigned char *base;
	int signal_number;

	if (argc != 2) {
		fprintf(stderr, "usage: %s STATE_FILE\n", argv[0]);
		return 2;
	}
	if (setsid() < 0)
		die("setsid");
	state_path = argv[1];
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size)
		return 3;

	sigemptyset(&waitset);
	sigaddset(&waitset, SIGUSR1);
	sigaddset(&waitset, SIGUSR2);
	if (sigprocmask(SIG_BLOCK, &waitset, NULL))
		die("sigprocmask");

	base = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE,
		    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (base == MAP_FAILED)
		die("mmap base");
	hidden = base;
	stable = base + page_size;
	fill_page(hidden, 0xa5);
	fill_page(stable, 0x5a);
	if (mprotect(hidden, page_size, PROT_NONE))
		die("mprotect initial hidden");

	remapped = mmap(NULL, page_size, PROT_READ | PROT_WRITE,
			MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (remapped == MAP_FAILED)
		die("mmap remapped");
	fill_page(remapped, 0x11);

	write_state("READY", "");
	for (;;) {
		if (sigwait(&waitset, &signal_number))
			die("sigwait");
		if (signal_number == SIGUSR1)
			mutate_after_predump();
		else if (signal_number == SIGUSR2)
			verify_after_restore();
	}
}
