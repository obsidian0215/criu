#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/mman.h>
#include <sys/prctl.h>

int main(int argc, char **argv)
{
	unsigned char *memory;
	FILE *state;
	long page_size = sysconf(_SC_PAGESIZE);
	size_t size = 256 * page_size;
	size_t i;

	if (argc != 2 || page_size <= 0)
		return 1;
	memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
		      MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (memory == MAP_FAILED)
		return 1;
	for (i = 0; i < size; i++)
		memory[i] = (i / page_size * 17 + i % 251) & 255;
	if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0))
		return 1;
	state = fopen(argv[1], "w");
	if (!state)
		return 1;
	fprintf(state, "%d %p %zu\n", getpid(), memory, size);
	if (fclose(state))
		return 1;
	for (;;)
		pause();
}
