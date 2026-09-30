#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

#define LENGTH (32UL * 1024 * 1024)
static volatile sig_atomic_t change, verify;
static void handler(int sig) { if (sig == SIGUSR1) change = 1; else verify = 1; }
static unsigned char expected(size_t i) { return (unsigned char)((i * 17 + (i >> 12) * 31 + 73) % 251); }
static void mark(const char *name, const char *value) {
	char path[128];
	snprintf(path, sizeof(path), "/state/%s", name);
	FILE *f = fopen(path, "w");
	if (!f || fprintf(f, "%s\n", value) < 0 || fclose(f)) _exit(2);
}
int main(void) {
	if (prctl(PR_SET_THP_DISABLE, 1, 0, 0, 0)) return 2;
	unsigned char *p = mmap(NULL, LENGTH, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (p == MAP_FAILED || madvise(p, LENGTH, MADV_NOHUGEPAGE)) return 2;
	struct sigaction sa = {.sa_handler = handler};
	sigemptyset(&sa.sa_mask);
	if (sigaction(SIGUSR1, &sa, NULL) || sigaction(SIGUSR2, &sa, NULL)) return 2;
	for (size_t i = 0; i < LENGTH; i++) p[i] = expected(i);
	char address[80];
	snprintf(address, sizeof(address), "%p %lu", (void *)p, LENGTH);
	mark("address", address);
	mark("ready", "ready");
	int generation = 0;
	for (;;) {
		if (change && !generation) {
			/* One deliberately dirty page, surrounded by unchanged parent pages. */
			for (size_t i = 4096 * 101; i < 4096 * 102; i++) p[i] ^= 0x5a;
			generation = 1;
			mark("changed", "changed");
		}
		if (verify) {
			if (generation != 1) { mark("result", "FAIL generation"); return 1; }
			for (size_t i = 0; i < LENGTH; i++) {
				unsigned char want = expected(i);
				if (i >= 4096 * 101 && i < 4096 * 102) want ^= 0x5a;
				if (p[i] != want) { mark("result", "FAIL memory"); return 1; }
			}
			mark("result", "PASS 33554432 bytes including changed page");
			for (;;) pause();
		}
		/* Polling prevents a signal arriving just before pause from losing a wakeup. */
		usleep(20000);
	}
}
