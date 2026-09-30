#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <unistd.h>

static volatile sig_atomic_t change_requested;
static void request_change(int signal_number)
{
    (void)signal_number;
    change_requested = 1;
}

int main(int argc, char **argv)
{
    if (argc != 4)
        return 1;
    long page_size = sysconf(_SC_PAGESIZE);
    size_t size = strtoull(argv[2], NULL, 10) * 1024 * 1024;
    size_t page_count = size / page_size;
    size_t changed_pages = page_count * strtoul(argv[3], NULL, 10) / 100;
    unsigned char *memory = mmap(NULL, size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (memory == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    if (prctl(PR_SET_PTRACER, PR_SET_PTRACER_ANY, 0, 0, 0)) {
        perror("PR_SET_PTRACER");
        return 1;
    }
    for (size_t page = 0; page < page_count; page++)
        memset(memory + page * page_size, (page * 17 + 3) & 255, page_size);
    struct sigaction action = { .sa_handler = request_change };
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL))
        return 1;
    unsigned int epoch = 0;
    for (;;) {
        FILE *state = fopen(argv[1], "w");
        if (!state)
            return 1;
        fprintf(state, "%d %p %zu %u\n", getpid(), memory, size, epoch);
        if (fclose(state))
            return 1;
        while (!change_requested)
            usleep(1000);
        change_requested = 0;
        epoch++;
        for (size_t page = 0; page < changed_pages; page++)
            memset(memory + page * page_size, (page * 17 + 3 + epoch) & 255, page_size);
    }
}
