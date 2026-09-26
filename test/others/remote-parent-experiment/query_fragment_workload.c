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
#include <unistd.h>

#define REGION_SIZE (8UL * 1024 * 1024)

static const char *state_path;
static const char *result_path;
static unsigned char *region;
static size_t page_size;
static size_t nr_pages;
static bool mutated;

static void die(const char *what)
{
	perror(what);
	_exit(111);
}

static uint64_t initial_value(size_t page)
{
	return 0x4f1bbcdc8b42135dULL ^
	       (0x9e3779b97f4a7c15ULL * (page + 1));
}

static uint64_t expected_value(size_t page)
{
	uint64_t value = initial_value(page);

	if (mutated && !(page & 1))
		value ^= 0x55aa55aa55aa55aaULL;
	return value;
}

static void set_page(size_t page, uint64_t value)
{
	uint64_t *first = (uint64_t *)(region + page * page_size);
	uint64_t *last = (uint64_t *)(region + (page + 1) * page_size -
				      sizeof(value));

	*first = value;
	*last = ~value;
}

static bool verify_region(void)
{
	size_t page;

	for (page = 0; page < nr_pages; page++) {
		uint64_t value = expected_value(page);
		uint64_t *first = (uint64_t *)(region + page * page_size);
		uint64_t *last = (uint64_t *)(region + (page + 1) * page_size -
					      sizeof(value));

		if (*first != value || *last != ~value)
			return false;
	}
	return true;
}

static void write_atomic(const char *path, const char *value)
{
	char tmp[4096];
	int fd;
	int length;
	size_t done = 0;
	size_t size = strlen(value);

	length = snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, getpid());
	if (length < 0 || (size_t)length >= sizeof(tmp)) {
		errno = ENAMETOOLONG;
		die("state path");
	}
	fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
	if (fd < 0)
		die("open temporary state");
	while (done < size) {
		ssize_t written = write(fd, value + done, size - done);

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
	char value[512];
	int length;

	length = snprintf(value, sizeof(value),
			  "pid=%d\nphase=%s\naddress=%" PRIuPTR "\n"
			  "size=%zu\npages=%zu\n",
			  getpid(), phase, (uintptr_t)region, REGION_SIZE,
			  nr_pages);
	if (length < 0 || (size_t)length >= sizeof(value)) {
		errno = EOVERFLOW;
		die("format state");
	}
	write_atomic(state_path, value);
}

static void mutate_alternating_pages(void)
{
	size_t page;

	for (page = 0; page < nr_pages; page += 2)
		set_page(page, initial_value(page) ^ 0x55aa55aa55aa55aaULL);
	mutated = true;
	publish_state("mutated");
}

int main(int argc, char **argv)
{
	sigset_t set;
	size_t page;

	if (argc != 3) {
		fprintf(stderr, "usage: %s STATE RESULT\n", argv[0]);
		return 2;
	}
	state_path = argv[1];
	result_path = argv[2];
	page_size = (size_t)sysconf(_SC_PAGESIZE);
	if (!page_size || REGION_SIZE % page_size) {
		errno = EINVAL;
		die("page size");
	}
	nr_pages = REGION_SIZE / page_size;
	region = mmap(NULL, REGION_SIZE, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (region == MAP_FAILED)
		die("mmap");
	if (madvise(region, REGION_SIZE, MADV_NOHUGEPAGE))
		die("madvise MADV_NOHUGEPAGE");
	for (page = 0; page < nr_pages; page++)
		set_page(page, initial_value(page));

	sigemptyset(&set);
	sigaddset(&set, SIGUSR1);
	sigaddset(&set, SIGTERM);
	if (sigprocmask(SIG_BLOCK, &set, NULL))
		die("sigprocmask");
	publish_state("ready");

	for (;;) {
		int signal;

		if (sigwait(&set, &signal))
			die("sigwait");
		if (signal == SIGUSR1) {
			if (mutated) {
				errno = EALREADY;
				die("duplicate mutation");
			}
			mutate_alternating_pages();
		} else if (signal == SIGTERM) {
			bool good = verify_region();

			write_atomic(result_path, good ? "PASS\n" : "FAIL\n");
			_exit(good ? 0 : 112);
		}
	}
}
