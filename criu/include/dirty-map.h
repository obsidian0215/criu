#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include "int.h"
#include "pid.h"
#include "page.h"
#include "pagemap-cache.h"

// dirty-track LKM definitions
#define DIRTY_TRACK_MAGIC 'd'
#define IOCTL_SET_DIRTY_MAP_PATH _IOW(DIRTY_TRACK_MAGIC, 1, char[256])
#define IOCTL_START_PID _IOW(DIRTY_TRACK_MAGIC, 2, pid_t)
#define IOCTL_STOP_PID _IOW(DIRTY_TRACK_MAGIC, 3, pid_t)
#define IOCTL_CLEAR_SOFT_DIRTY _IO(DIRTY_TRACK_MAGIC, 4)
#define IOCTL_GET_DIRTY_MAP_PATH _IOR(DIRTY_TRACK_MAGIC, 5, char[256])

#define DT_DEV_PATH "/dev/dirty-track"

// 脏页信息
struct __attribute__((__packed__)) dirty_heatmap{
    // address range
    unsigned long start;
    unsigned int size;

    unsigned char heat_level;       // 热度
    char heat_trend;                // 热度变化
    unsigned char selected;         // 被选择转储及次数
};

struct dirty_log {
    pid_t pid;
	struct dirty_heatmap *dirtymap;
	unsigned long dirtymap_size;
};

#define INIT_DIRTY_LOG(log) do { \
    (log).pid = -1; \
    (log).dirtymap = NULL; \
    (log).dirtymap_size = 0; \
} while (0)

#define INIT_DIRTY_LOG_PTR(log_ptr) do { \
    (log_ptr)->pid = -1; \
    (log_ptr)->dirtymap = NULL; \
    (log_ptr)->dirtymap_size = 0; \
} while (0)

int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir);
int fini_dirty_map(struct pstree_item *item);
int start_dirty_track(int pid);
struct dirty_heatmap *search_dirty_map(struct dirty_log *dl, unsigned long addr);

#endif