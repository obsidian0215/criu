#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>

#include "cr_options.h"
#include "image-desc.h"
#include "image.h"
#include "magic.h"
#include "page-xfer.h"
#include "protobuf.h"
#include "remote-parent.h"
#include "servicefd.h"
#include "images/pagemap.pb-c.h"

static void check_empty(int dirfd)
{
	struct dirent *entry;
	int fd = openat(dirfd, ".", O_RDONLY | O_DIRECTORY);
	DIR *directory = fdopendir(fd);

	assert(directory);
	while ((entry = readdir(directory)))
		assert(!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."));
	assert(!closedir(directory));
}

static struct cr_img *create_image(int dirfd, const char *name)
{
	int fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	struct cr_img *image;

	assert(fd >= 0);
	image = img_from_fd(fd);
	assert(image);
	return image;
}

static void write_test_pagemap(int dirfd, const char *name, int fd_type, u32 common_magic,
			       u32 image_magic, u32 pages_id, PagemapEntry *entries, size_t nr_entries)
{
	PagemapHead head = PAGEMAP_HEAD__INIT;
	struct cr_img *image = create_image(dirfd, name);
	size_t i;

	assert(!write_img(image, &common_magic));
	assert(!write_img(image, &image_magic));
	head.pages_id = pages_id;
	assert(pb_write_one(image, &head, PB_PAGEMAP_HEAD) >= 0);
	for (i = 0; i < nr_entries; i++)
		assert(pb_write_one(image, &entries[i], PB_PAGEMAP) >= 0);
	close_image(image);
}

static void assert_standard_pagemap(int dirfd, const char *name, int fd_type, const u32 *flags,
				    const uint64_t *vaddrs, size_t nr_entries)
{
	PagemapHead *head = NULL;
	PagemapEntry *entry = NULL;
	struct cr_img *image;
	u32 magic;
	size_t i;
	int fd;

	fd = openat(dirfd, name, O_RDONLY | O_CLOEXEC);
	assert(fd >= 0);
	image = img_from_fd(fd);
	assert(image);
	assert(read_img(image, &magic) > 0 && magic == IMG_COMMON_MAGIC);
	assert(read_img(image, &magic) > 0 && magic == imgset_template[fd_type].magic);
	assert(pb_read_one(image, &head, PB_PAGEMAP_HEAD) >= 0);
	assert(head->pages_id == 0);
	pagemap_head__free_unpacked(head, NULL);

	for (i = 0; i < nr_entries; i++) {
		assert(pb_read_one_eof(image, &entry, PB_PAGEMAP) > 0);
		assert(entry->vaddr == vaddrs[i]);
		assert(entry->has_nr_pages && entry->nr_pages == 1);
		assert(entry->has_flags && entry->flags == flags[i]);
		pagemap_entry__free_unpacked(entry, NULL);
		entry = NULL;
	}
	assert(pb_read_one_eof(image, &entry, PB_PAGEMAP) == 0);
	close_image(image);
}

/* Exercise failures after a previous writer is already ready to commit. */
static void test_writer_open_failures(int dirfd)
{
	struct remote_parent_writer *first, *second;
	struct iovec iov = { .iov_base = (void *)PAGE_SIZE, .iov_len = PAGE_SIZE };
	struct dirent *entry;
	struct rlimit saved_limit, limit;
	struct sigaction saved_action, action = { .sa_handler = SIG_IGN };
	unsigned long sequence = 0;
	char name[128], sentinel[4];
	DIR *directory;
	int fd, ret;

	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 30, &first));
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	remote_parent_writer_close(first);
	directory = fdopendir(openat(dirfd, ".", O_RDONLY | O_DIRECTORY));
	assert(directory);
	while ((entry = readdir(directory))) {
		if (strncmp(entry->d_name, ".remote-parent-pagemap-30.img.tmp.", 32))
			continue;
		sequence = strtoul(strrchr(entry->d_name, '.') + 1, NULL, 10);
	}
	assert(!closedir(directory));
	assert(sequence);
	ret = snprintf(name, sizeof(name), ".remote-parent-pagemap-31.img.tmp.%d.%lu",
		       getpid(), sequence + 1);
	assert(ret > 0 && (size_t)ret < sizeof(name));
	fd = openat(dirfd, name, O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, "keep", 4) == 4);
	assert(!close(fd));
	assert(remote_parent_writer_open(CR_FD_PAGEMAP, 31, &second) < 0);
	assert(!second);
	assert(remote_parent_finish(true) < 0);
	fd = openat(dirfd, name, O_RDONLY);
	assert(fd >= 0 && read(fd, sentinel, sizeof(sentinel)) == sizeof(sentinel));
	assert(!memcmp(sentinel, "keep", sizeof(sentinel)));
	assert(!close(fd));
	assert(!unlinkat(dirfd, name, 0));
	check_empty(dirfd);

	/* A real write failure, without a production fault-injection hook. */
	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 30, &first));
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	remote_parent_writer_close(first);
	assert(!getrlimit(RLIMIT_FSIZE, &saved_limit));
	limit = saved_limit;
	limit.rlim_cur = 0;
	sigemptyset(&action.sa_mask);
	assert(!sigaction(SIGXFSZ, &action, &saved_action));
	assert(!setrlimit(RLIMIT_FSIZE, &limit));
	ret = remote_parent_writer_open(CR_FD_PAGEMAP, 31, &second);
	assert(!setrlimit(RLIMIT_FSIZE, &saved_limit));
	assert(!sigaction(SIGXFSZ, &saved_action, NULL));
	assert(ret < 0 && !second);
	assert(remote_parent_finish(true) < 0);
	check_empty(dirfd);

	/* A directory-FD failure must poison the complete transaction too. */
	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 30, &first));
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	remote_parent_writer_close(first);
	assert(!close_service_fd(IMG_FD_OFF));
	ret = remote_parent_writer_open(CR_FD_PAGEMAP, 31, &second);
	assert(install_service_fd(IMG_FD_OFF, dirfd) >= 0);
	assert(ret < 0 && !second);
	assert(remote_parent_finish(true) < 0);
	check_empty(dirfd);
}

static void test_malformed_images(int dirfd)
{
	struct remote_parent_coverage *coverage = NULL;
	PagemapEntry entries[2] = { PAGEMAP_ENTRY__INIT, PAGEMAP_ENTRY__INIT };
	const char *name = "remote-parent-pagemap-10.img";

	entries[0].vaddr = PAGE_SIZE;
	entries[0].has_nr_pages = true;
	entries[0].nr_pages = 1;
	entries[0].has_flags = true;
	entries[0].flags = PE_PRESENT;

	write_test_pagemap(dirfd, name, CR_FD_PAGEMAP, 0, imgset_template[CR_FD_PAGEMAP].magic,
			   0, entries, 1);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));

	write_test_pagemap(dirfd, name, CR_FD_PAGEMAP, IMG_COMMON_MAGIC, 0, 0, entries, 1);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));

	write_test_pagemap(dirfd, name, CR_FD_PAGEMAP, IMG_COMMON_MAGIC,
			   imgset_template[CR_FD_PAGEMAP].magic, 1, entries, 1);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));

	entries[0].nr_pages = 0;
	write_test_pagemap(dirfd, name, CR_FD_PAGEMAP, IMG_COMMON_MAGIC,
			   imgset_template[CR_FD_PAGEMAP].magic, 0, entries, 1);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));

	entries[0].nr_pages = 1;
	entries[0].vaddr = 2 * PAGE_SIZE;
	entries[1] = entries[0];
	entries[1].vaddr = PAGE_SIZE;
	write_test_pagemap(dirfd, name, CR_FD_PAGEMAP, IMG_COMMON_MAGIC,
			   imgset_template[CR_FD_PAGEMAP].magic, 0, entries, 2);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));

	assert(!symlinkat("missing", dirfd, name));
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) < 0);
	assert(!unlinkat(dirfd, name, 0));
	check_empty(dirfd);
}

void test_remote_parent(void)
{
	char path[] = "/tmp/criu-remote-parent.XXXXXX";
	struct remote_parent_writer *first, *second;
	struct remote_parent_coverage *coverage = NULL;
	struct iovec iov = { .iov_base = (void *)PAGE_SIZE, .iov_len = PAGE_SIZE };
	const u32 expected_flags[] = { PE_PRESENT, PE_PARENT, PE_PRESENT };
	const uint64_t expected_vaddrs[] = { PAGE_SIZE, 2 * PAGE_SIZE, 4 * PAGE_SIZE };
	int saved_mode = opts.mode;
	char sentinel[4] = {};
	int dirfd, fd, i;

	assert(mkdtemp(path));
	dirfd = open(path, O_RDONLY | O_DIRECTORY);
	assert(dirfd >= 0);
	assert(install_service_fd(IMG_FD_OFF, dirfd) >= 0);
	opts.mode = CR_PRE_DUMP;
	test_writer_open_failures(dirfd);

	assert(!remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage));
	assert(!coverage);
	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 10, &first));
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	iov.iov_base = (void *)(2 * PAGE_SIZE);
	assert(!remote_parent_writer_record(first, &iov, PE_PARENT));
	iov.iov_base = (void *)(4 * PAGE_SIZE);
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	remote_parent_writer_close(first);
	assert(!remote_parent_coverage_exists(dirfd, CR_FD_PAGEMAP, 10));
	assert(!remote_parent_finish(true));
	assert_standard_pagemap(dirfd, "remote-parent-pagemap-10.img", CR_FD_PAGEMAP,
				expected_flags, expected_vaddrs, 3);
	assert(remote_parent_coverage_open(dirfd, CR_FD_PAGEMAP, 10, &coverage) == 1);
	assert(remote_parent_coverage_contains(coverage, PAGE_SIZE, 2 * PAGE_SIZE));
	assert(!remote_parent_coverage_contains(coverage, PAGE_SIZE, 3 * PAGE_SIZE));
	assert(!remote_parent_coverage_contains(coverage, 3 * PAGE_SIZE, PAGE_SIZE));
	assert(remote_parent_coverage_contains(coverage, 4 * PAGE_SIZE, PAGE_SIZE));
	remote_parent_coverage_close(coverage);
	assert(!unlinkat(dirfd, "remote-parent-pagemap-10.img", 0));
	check_empty(dirfd);

	fd = openat(dirfd, "remote-parent-pagemap-10.img", O_WRONLY | O_CREAT | O_EXCL, 0600);
	assert(fd >= 0 && write(fd, "keep", 4) == 4);
	assert(!close(fd));
	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 10, &first));
	iov.iov_base = (void *)PAGE_SIZE;
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	assert(!remote_parent_writer_open(CR_FD_SHMEM_PAGEMAP, 20, &second));
	assert(!remote_parent_writer_record(second, &iov, PE_PRESENT));
	assert(remote_parent_finish(true) < 0);
	assert(faccessat(dirfd, "remote-parent-shmem-20.img", F_OK, 0) < 0 && errno == ENOENT);
	fd = openat(dirfd, "remote-parent-pagemap-10.img", O_RDONLY);
	assert(fd >= 0 && read(fd, sentinel, sizeof(sentinel)) == sizeof(sentinel));
	assert(!memcmp(sentinel, "keep", sizeof(sentinel)));
	assert(!close(fd));
	assert(!unlinkat(dirfd, "remote-parent-pagemap-10.img", 0));
	check_empty(dirfd);

	for (i = 0; i < 32; i++) {
		assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 10, &first));
		assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
		assert(!remote_parent_finish(false));
		assert(!remote_parent_finish(false));
		check_empty(dirfd);
	}

	assert(!remote_parent_writer_open(CR_FD_PAGEMAP, 10, &first));
	iov.iov_len = 1;
	assert(remote_parent_writer_record(first, &iov, PE_PRESENT) < 0);
	assert(remote_parent_finish(true) < 0);
	check_empty(dirfd);
	iov.iov_len = PAGE_SIZE;

	test_malformed_images(dirfd);

	/* Shared-image coverage uses offsets, including offset zero. */
	assert(!remote_parent_writer_open(CR_FD_SHMEM_PAGEMAP, 40, &first));
	iov.iov_base = NULL;
	assert(!remote_parent_writer_record(first, &iov, PE_PRESENT));
	iov.iov_base = (void *)PAGE_SIZE;
	assert(!remote_parent_writer_record(first, &iov, PE_PARENT));
	assert(!remote_parent_finish(true));
	assert(remote_parent_coverage_open(dirfd, CR_FD_SHMEM_PAGEMAP, 40, &coverage) == 1);
	assert(remote_parent_coverage_contains(coverage, 0, 2 * PAGE_SIZE));
	assert(!remote_parent_coverage_contains(coverage, 0, 3 * PAGE_SIZE));
	remote_parent_coverage_close(coverage);
	assert(!unlinkat(dirfd, "remote-parent-shmem-40.img", 0));
	check_empty(dirfd);

	opts.mode = saved_mode;
	close_service_fd(IMG_FD_OFF);
	assert(!close(dirfd));
	assert(!rmdir(path));
}
