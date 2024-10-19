#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include "int.h"
#include "pid.h"
#include "page.h"
#include "pagemap-cache.h"

// 定义页修改频率的比例阈值
#define WRITE_RATIO_COLD 0.05  	// 冷页阈值：< 5%
#define WRITE_RATIO_WARM 0.25  	// 温页阈值：>= 5% 且 < 25%
#define WRITE_RATIO_HOT 0.3   	// 热页阈值：>= 25% 且 < 30%
#define WRITE_RATIO_MAX 0.1   	// 最大阈值：<= 100%

// 脏页信息
struct __attribute__((__packed__)) dirty_page{
	unsigned long address;
    unsigned long write_count;
    unsigned int page_type;
};

struct dirty_log {
    pid_t pid;
	struct dirty_page *dirtymap;
	unsigned long dirtymap_size;
	unsigned long max_write_count;
};

#define INIT_DIRTY_LOG(log) do { \
    (log).pid = -1; \
    (log).dirtymap = NULL; \
    (log).dirtymap_size = 0; \
    (log).max_write_count = 0; \
} while (0)

#define INIT_DIRTY_LOG_PTR(log_ptr) do { \
    (log_ptr)->pid = -1; \
    (log_ptr)->dirtymap = NULL; \
    (log_ptr)->dirtymap_size = 0; \
    (log_ptr)->max_write_count = 0; \
} while (0)

int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir);
int use_dirty_map(pid_t pid, struct dirty_log *dl);

#endif