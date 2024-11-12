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
    // 确保dirty-track LKM的dirty-map目录存在且与输入的dirty_map_dir一致
    fd = open(DT_DEV_PATH, O_RDWR);
    if (ioctl(fd, IOCTL_GET_DIRTY_MAP_PATH, filepath) < 0) {
        fprintf(stderr, "Error to get dirty_map_path for dirty-track\n");
        close(fd);
        return -1;
    } else if (strcmp(filepath, dirty_map_dir) != 0) {
        fprintf(stderr, "Error: dirty_map_dir %s is not the same as the one in dirty-track LKM\n", dirty_map_dir);
        close(fd);
        return -1;
    }
    close(fd);
    // 将filepath清空
    memset(filepath, 0, sizeof(filepath));

	// 打开<dirty_map_dir>/newest-<pid>.img
    ret = snprintf(filepath, sizeof(filepath), "%s/latest-%d.heatmap", dirty_map_dir, pid);
    if (ret < 0 || ret >= sizeof(filepath)) {
        fprintf(stderr, "Error constructing file path for pid %d\n", pid);
        return -1;
    }
    fd = open(filepath, O_RDONLY);
    if (fd == -1) {
        fprintf(stderr, "Error opening dirty-heatmap file\n");
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
    mapped = mmap(NULL, st.st_size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
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
 * @brief 为特定pid进程启动dirty track
 *
 * @param pid 目标进程的真实pid
 * @return int 成功返回0，失败返回-1/errno
 */
int start_dirty_track(int pid) {
    int ret = 0, fd;
    
    fd = open(DT_DEV_PATH, O_RDWR);
    if (fd == -1) {
        fprintf(stderr, "Error opening dirty-track LKM\n");
        return -1;
    }

    ret = ioctl(fd, IOCTL_START_PID, &pid);
    return ret;
}

/**
 * @brief 为特定pid进程销毁其dirty_map
 *
 * @param item 指向per-process结构<pid>的指针
 * @return void
 */
void fini_dirty_map(struct pstree_item *item){
    struct dirty_log *dl = &item->dirty_log;
    
    if (dl) {
        if (dl->dirtymap) {
            msync(dl->dirtymap, dl->dirtymap_size, MS_SYNC);
            munmap(dl->dirtymap, dl->dirtymap_size);
            dl->dirtymap = NULL;
            dl->dirtymap_size = 0;
        }
    }
}

/**
 * @brief 查找dirty_map中包含指定address的dirty_heatmap结构体
 *
 * @param item 指向per-process结构<pid>的指针
 * @param addr 要查找的线性地址
 * @return struct dirty_heatmap * 返回指向dirty_heatmap结构体的指针，如果未找到则返回NULL
 */
struct dirty_heatmap *search_dirty_map(struct pstree_item *item, unsigned long addr) {
    struct dirty_log *dl = &item->dirty_log;
    struct dirty_heatmap *map = dl->dirtymap;
    unsigned long left = 0;
    unsigned long right = dl->dirtymap_size;

    while (left < right) {
        unsigned long mid = left + (right - left) / 2;
        unsigned long range_start = map[mid].start;
        unsigned long range_end = range_start + map[mid].size;

        if (addr < range_start) {
            // 地址在当前范围左侧，缩小右边界
            right = mid;
        } else if (addr >= range_end) {
            // 地址在当前范围右侧，缩小左边界
            left = mid + 1;
        } else {
            // 地址在当前范围内，返回该结构体指针
            return &map[mid];
        }
    }

    // 如果未找到包含地址的范围，返回 NULL
    return NULL;
}