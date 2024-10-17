#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/falloc.h>
#include <sys/uio.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>

#include "types.h"
#include "image.h"
#include "cr_options.h"
#include "servicefd.h"
#include "pagemap.h"
#include "restorer.h"
#include "rst-malloc.h"
#include "page-xfer.h"
#include "pstree.h"

#include "dirty-map.h"
#include "xmalloc.h"
#include "protobuf.h"

// [Obsidian0215]initial <pid>'s dirty-map
int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir){
	struct dirty_log *dl = &item->dirty_log;
	pid_t pid = dl->pid;

	printf("[Obsidian0215] init dirty-log for pid: %d\n", pid);
	
	// 打开<dirty_map_dir>/newest-<pid>.img
    char filepath[PATH_MAX];
    int ret = snprintf(filepath, sizeof(filepath), "%s/newest-%d.img", dirty_map_dir, pid);
    if (ret < 0 || ret >= sizeof(filepath)) {
        fprintf(stderr, "Error constructing file path for pid %d\n", pid);
        return -1;
    }
    int fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        perror("Error opening dirty map file");
        return -1;
    }

    // 获取dirty-map文件大小
    struct stat st;
    if (fstat(fd, &st) == -1) {
        perror("Error getting %s's size", filepath);
        close(fd);
        return -1;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "Dirty-map %s is empty\n", filepath);
        close(fd);
        return -1;
    }

    // 将dirty-map映射到criu的内存空间
    void *mapped = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("Error mapping dirty-map file");
        close(fd);
        return -1;
    }
    close(fd);

    // 更新该进程的struct dirty_log
    dl->dirtymap = (struct dirty_page *)mapped;
    dl->dirtymap_size = st.st_size;

    printf("[Obsidian0215] Successfully mapped dirty-map file %s (size: %lu bytes): 0x%p\n", filepath, dl->dirtymap_size, dl->dirtymap);

	return 0;
}

// [Obsidian0215]use <pid>'s dirty-map
int use_dirty_map(pid_t pid, struct dirty_log *dl){
	return 0;
}
