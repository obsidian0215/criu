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
#define IOCTL_CHECK_PID _IOWR(DIRTY_TRACK_MAGIC, 4, struct pid_check)
#define IOCTL_GET_DIRTY_MAP_PATH _IOR(DIRTY_TRACK_MAGIC, 5, char[256])

#define DT_DEV_PATH "/dev/dirty-track"

// 检查pid是否被dirty-track中
struct pid_check {
    pid_t pid;
    bool is_tracked;
};

// 纪录被选择传输的页地址和次数
struct __attribute__((__packed__)) selected_page
{
    unsigned long address;
    unsigned char s_count;  // 被选择转储的次数
};

// 脏页heatmap信息
struct __attribute__((__packed__)) dirty_diffmap
{
    unsigned long address;          // 页地址
    unsigned char heat_level;       // 热度
    char heat_trend;                // 热度变化
};

// 脏页dirtymap信息
struct __attribute__((__packed__)) dirty_map
{
	unsigned long address;
    unsigned int write_count;
};

struct dirty_log {
    pid_t pid;
    int dirty_track_fd;     // dirty-track设备文件描述符
    struct dirty_diffmap *diffmap;
    unsigned long diffmap_size;

    struct {
        int *timestamp_list;
        size_t ts_list_size;

        int latest_timestamp;
        struct dirty_map *latest_dm;
	    unsigned long ldm_size;

        int less_latest_timestamp;
        struct dirty_map *less_latest_dm;
	    unsigned long lldm_size;
    };
};

#define INIT_DIRTY_LOG(log) do { \
    (log).pid = -1; \
    (log).dirty_track_fd = -1; \
    (log).timestamp_list = NULL; \
    (log).ts_list_size = 0; \
    (log).latest_timestamp = 0; \
    (log).latest_dm = NULL; \
    (log).ldm_size = 0; \
    (log).less_latest_timestamp = 0; \
    (log).less_latest_dm = NULL; \
    (log).lldm_size = 0; \
    (log).diffmap = NULL; \
    (log).diffmap_size = 0; \
} while (0)

#define INIT_DIRTY_LOG_PTR(log_ptr) do { \
    (log_ptr)->pid = -1; \
    (log_ptr)->dirty_track_fd = -1; \
    (log_ptr)->timestamp_list = NULL; \
    (log_ptr)->ts_list_size = 0; \
    (log_ptr)->latest_timestamp = 0; \
    (log_ptr)->latest_dm = NULL; \
    (log_ptr)->ldm_size = 0; \
    (log_ptr)->less_latest_timestamp = 0; \
    (log_ptr)->less_latest_dm = NULL; \
    (log_ptr)->lldm_size = 0; \
    (log_ptr)->diffmap = NULL; \
    (log_ptr)->diffmap_size = 0; \
} while (0)

int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir);
void fini_dirty_map(struct pstree_item *item);
int init_dirty_track(struct dirty_log *dl);
int start_dirty_track(struct dirty_log* dl);
int check_dirty_track(struct dirty_log* dl, struct pid_check *pc);
int stop_dirty_track(struct dirty_log* dl);
struct dirty_diffmap *search_dirty_map(struct pstree_item *item, unsigned long addr);

#endif