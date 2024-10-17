#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include "int.h"
#include "pid.h"
#include "page.h"
#include "pagemap-cache.h"

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
};

#define INIT_PAGE_DIRTY_MAP { \
    .pid = -1,	\
    .dirtymap = NULL, \
	.dirtymap_size = 0, \
}

int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir);
int use_dirty_map(pid_t pid, struct dirty_log *dl);

#endif