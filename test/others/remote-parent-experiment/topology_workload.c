#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
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

#define REGION_SIZE (32UL * 1024 * 1024)
#define CHILD_SIZE  (16UL * 1024 * 1024)

struct child_ready {
	uintptr_t address;
	size_t size;
};

static const char *mode;
static const char *self_path;
static const char *pid_path;
static const char *state_path;
static const char *result_path;
static unsigned char *region;
static unsigned char *reserved;
static unsigned char *original_region;
static size_t region_size;
static uint64_t seed;
static bool region_zeroed;
static pid_t child_pid = -1;
static uintptr_t child_address;
static size_t child_size;

static void die(const char *what)
{
	perror(what);
	_exit(111);
}

static uint64_t pattern(size_t page, uint64_t value_seed)
{
	return value_seed ^ (0x9e3779b97f4a7c15ULL * (page + 1));
}

static void fill_region(unsigned char *base, size_t size, uint64_t value_seed)
{
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t pages = size / page_size;

	for (size_t page = 0; page < pages; page++) {
		uint64_t value = pattern(page, value_seed);
		uint64_t *first = (uint64_t *)(base + page * page_size);
		uint64_t *last = (uint64_t *)(base + (page + 1) * page_size - sizeof(value));

		*first = value;
		*last = ~value;
	}
}

static bool verify_region(const unsigned char *base, size_t size, uint64_t value_seed)
{
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t pages = size / page_size;

	for (size_t page = 0; page < pages; page++) {
		uint64_t value = pattern(page, value_seed);
		const uint64_t *first = (const uint64_t *)(base + page * page_size);
		const uint64_t *last = (const uint64_t *)(base + (page + 1) * page_size - sizeof(value));

		if (*first != value || *last != ~value)
			return false;
	}
	return true;
}

static bool verify_zero_region(const unsigned char *base, size_t size)
{
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
	size_t pages = size / page_size;

	for (size_t page = 0; page < pages; page++) {
		const uint64_t *first = (const uint64_t *)(base + page * page_size);
		const uint64_t *last = (const uint64_t *)(base + (page + 1) * page_size - sizeof(*last));

		if (*first || *last)
			return false;
	}
	return true;
}

static void write_text_atomic(const char *path, const char *text)
{
	char tmp[4096];
	int ret = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
	int fd;
	size_t len;
	size_t done = 0;

	if (ret < 0 || (size_t)ret >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		die("state path");
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		die("open state tmp");
	len = strlen(text);
	while (done < len) {
		ssize_t written = write(fd, text + done, len - done);

		if (written < 0) {
			if (errno == EINTR)
				continue;
			die("write state");
		}
		if (!written) {
			errno = EIO;
			die("short state write");
		}
		done += (size_t)written;
	}
	if (close(fd))
		die("close state");
	if (rename(tmp, path))
		die("rename state");
}

static void publish_state(const char *phase)
{
	char text[1024];
	int ret = snprintf(text, sizeof(text),
		"pid=%d\nphase=%s\nmode=%s\naddress=%" PRIuPTR "\n"
		"original_address=%" PRIuPTR "\nreserved_address=%" PRIuPTR "\n"
		"size=%zu\nzeroed=%d\nchild=%d\nchild_address=%" PRIuPTR "\nchild_size=%zu\n",
		getpid(), phase, mode, (uintptr_t)region, (uintptr_t)original_region,
		(uintptr_t)reserved, region_size, region_zeroed, child_pid, child_address,
		child_size);

	if (ret < 0 || (size_t)ret >= sizeof(text)) {
		errno = EOVERFLOW;
		die("format state");
	}
	write_text_atomic(state_path, text);
}

static void write_pid(void)
{
	char text[64];
	int ret = snprintf(text, sizeof(text), "%d\n", getpid());

	if (ret < 0 || (size_t)ret >= sizeof(text)) {
		errno = EOVERFLOW;
		die("format pid");
	}
	write_text_atomic(pid_path, text);
}

static void write_result(const char *value)
{
	write_text_atomic(result_path, value);
}

static void allocate_region(size_t size, uint64_t value_seed, bool reserve_target)
{
	region_size = size;
	seed = value_seed;
	region = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		die("mmap region");
	original_region = region;
	fill_region(region, size, value_seed);
	if (reserve_target) {
		reserved = mmap(NULL, size, PROT_NONE,
				MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
		if (reserved == MAP_FAILED)
			die("mmap reserved target");
	}
}

static int child_main(int ready_fd)
{
	struct child_ready ready;
	sigset_t set;
	int sig;

	region_size = CHILD_SIZE;
	seed = 0xc11d000000000003ULL;
	region = mmap(NULL, region_size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		_exit(112);
	fill_region(region, region_size, seed);
	ready.address = (uintptr_t)region;
	ready.size = region_size;
	if (write(ready_fd, &ready, sizeof(ready)) != (ssize_t)sizeof(ready))
		_exit(113);
	close(ready_fd);

	sigemptyset(&set);
	sigaddset(&set, SIGTERM);
	if (sigwait(&set, &sig))
		_exit(114);
	_exit(verify_region(region, region_size, seed) ? 0 : 115);
}

static void create_child(void)
{
	int pipefd[2];
	struct child_ready ready;
	ssize_t got = 0;

	if (pipe2(pipefd, O_CLOEXEC))
		die("pipe2");
	child_pid = fork();
	if (child_pid < 0)
		die("fork");
	if (!child_pid) {
		close(pipefd[0]);
		_exit(child_main(pipefd[1]));
	}
	close(pipefd[1]);
	while (got < (ssize_t)sizeof(ready)) {
		ssize_t ret = read(pipefd[0], (char *)&ready + got, sizeof(ready) - (size_t)got);

		if (ret < 0) {
			if (errno == EINTR)
				continue;
			die("read child startup");
		}
		if (!ret) {
			errno = ECHILD;
			die("short child startup");
		}
		got += ret;
	}
	close(pipefd[0]);
	child_address = ready.address;
	child_size = ready.size;
	publish_state("mutated");
}

static void relocate_region(void)
{
	unsigned char *target = reserved;
	volatile unsigned char *touch;
	unsigned char checksum = 0;
	void *moved;
	size_t page_size = (size_t)sysconf(_SC_PAGESIZE);

	if (munmap(reserved, region_size))
		die("munmap reserved target");
	reserved = NULL;
	moved = mremap(region, region_size, region_size,
		       MREMAP_MAYMOVE | MREMAP_FIXED, target);
	if (moved == MAP_FAILED)
		die("mremap");
	region = moved;
	if (!verify_region(region, region_size, seed)) {
		errno = EILSEQ;
		die("verify relocated region");
	}

	/*
	 * MADV_DONTNEED discards the moved anonymous pages. Read-faulting them
	 * back produces clean zero pages, so a final incremental dump cannot
	 * rely on soft-dirty alone to prove that the old parent covers the new
	 * virtual address range.
	 */
	if (madvise(region, region_size, MADV_DONTNEED))
		die("madvise relocated region");
	touch = region;
	for (size_t offset = 0; offset < region_size; offset += page_size)
		checksum |= touch[offset];
	if (checksum || !verify_zero_region(region, region_size)) {
		errno = EILSEQ;
		die("verify discarded region");
	}
	region_zeroed = true;
	publish_state("mutated");
}

static void finish_parent(void)
{
	bool good = region_zeroed ? verify_zero_region(region, region_size) :
					  verify_region(region, region_size, seed);

	if (child_pid > 0) {
		int status = 0;
		pid_t waited;

		if (kill(child_pid, SIGTERM))
			good = false;
		do {
			waited = waitpid(child_pid, &status, 0);
		} while (waited < 0 && errno == EINTR);
		if (waited != child_pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0)
			good = false;
	}
	write_result(good ? "PASS\n" : "FAIL\n");
	_exit(good ? 0 : 116);
}

static void event_loop(void)
{
	sigset_t set;

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGUSR2);
	sigaddset(&set, SIGHUP);
	sigaddset(&set, SIGTERM);

	for (;;) {
		int sig;

		if (sigwait(&set, &sig))
			die("sigwait");
		if (sig == SIGTERM)
			finish_parent();
		if (sig == SIGUSR1 && !strcmp(mode, "relocate"))
			relocate_region();
		else if (sig == SIGUSR2 && !strcmp(mode, "fork"))
			create_child();
		else if (sig == SIGHUP && !strcmp(mode, "exec")) {
			execl(self_path, self_path, "--exec-stage", pid_path,
			      state_path, result_path, NULL);
			die("exec stage");
		}
	}
}

int main(int argc, char **argv)
{
	bool exec_stage = argc == 5 && !strcmp(argv[1], "--exec-stage");
	sigset_t blocked;

	if (argc != 5) {
		fprintf(stderr, "usage: %s MODE PIDFILE STATE RESULT\n", argv[0]);
		return 2;
	}

	self_path = argv[0];
	mode = exec_stage ? "exec" : argv[1];
	pid_path = argv[2];
	state_path = argv[3];
	result_path = argv[4];
	if (strcmp(mode, "relocate") && strcmp(mode, "exec") && strcmp(mode, "fork")) {
		fprintf(stderr, "invalid mode: %s\n", mode);
		return 2;
	}

	sigemptyset(&blocked);
	sigaddset(&blocked, SIGUSR1);
	sigaddset(&blocked, SIGUSR2);
	sigaddset(&blocked, SIGHUP);
	sigaddset(&blocked, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &blocked, NULL))
		die("sigprocmask");

	allocate_region(REGION_SIZE,
			exec_stage ? 0xec00000000000002ULL : 0xba5e000000000001ULL,
			!exec_stage && !strcmp(mode, "relocate"));
	write_pid();
	publish_state(exec_stage ? "mutated" : "ready");
	event_loop();
	return 0;
}
