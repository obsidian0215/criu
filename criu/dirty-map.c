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

/**
 * @brief 为特定pid进程初始化其dirty_map
 *
 * @param item 指向per-process结构<pid>的指针
 * @param dirty_map_dir dirty-map目录的字符串
 * @return int 成功返回0，失败返回-1
 */
int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir){
	struct dirty_log *dl = &item->dirty_log;
	pid_t pid = dl->pid;
    char filepath[PATH_MAX];
	int ret, fd;
    struct stat st;
	void *mapped;

	printf("[Obsidian0215] init dirty-log for pid: %d\n", pid);
	
	// 打开<dirty_map_dir>/newest-<pid>.img
    ret = snprintf(filepath, sizeof(filepath), "%s/newest-%d.img", dirty_map_dir, pid);
    if (ret < 0 || ret >= sizeof(filepath)) {
        fprintf(stderr, "Error constructing file path for pid %d\n", pid);
        return -1;
    }
    fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening dirty-map file\n");
        return -1;
    }

    // 获取dirty-map文件大小
    if (fstat(fd, &st) == -1) {
        fprintf(stderr, "Error getting %s's size\n", filepath);
        close(fd);
        return -1;
    }

    if (st.st_size == 0) {
        fprintf(stderr, "Dirty-map %s is empty\n", filepath);
        close(fd);
        return -1;
    }

    // 将dirty-map映射到criu的内存空间
    mapped = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        fprintf(stderr, "Error mapping dirty-map file\n");
        close(fd);
        return -1;
    }
    close(fd);

    // 更新该进程的struct dirty_log
    dl->dirtymap = (struct dirty_heatmap *)mapped;
    dl->dirtymap_size = st.st_size;

    printf("[Obsidian0215] Successfully mapped dirty-map file %s (size: %lu bytes): 0x%p\n", filepath, dl->dirtymap_size, dl->dirtymap);

	return 0;
}

/**
 * @brief 遍历dirty_map，获取其中最大的write_count
 *
 * @param dl 指向dirty_log结构的指针
 * @return int 成功返回0，失败返回-1
 */
int get_max_write_count(struct dirty_log *dl) {
    unsigned long max_wc = 0, current_wc, i = 0, num_pages;
	if (dl == NULL) {
        fprintf(stderr, "Invalid arguments: dl must not be NULL\n");
        return -1;
    }

    if (dl->dirtymap == NULL || dl->dirtymap_size == 0) {
        fprintf(stderr, "Dirty-map is not initialized or empty\n");
        return -1;
    }

    // 计算脏页的数量
    num_pages = dl->dirtymap_size / sizeof(struct dirty_heatmap);
    // if (num_pages == 0) {
    //     fprintf(stderr, "No dirty pages found in dirtymap\n");
    //     return -1;
    // }

    for (i = 0; i < num_pages; i++) {
        current_wc = dl->dirtymap[i].write_count;
        if (current_wc > max_wc) {
            max_wc = current_wc;
        }
    }

    dl->max_write_count = max_wc;
    return 0;
}

// [Obsidian0215]use <pid>'s dirty-map
int use_dirty_map(pid_t pid, struct dirty_log *dl){
	return 0;
}
