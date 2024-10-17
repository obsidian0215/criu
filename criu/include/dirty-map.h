#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include "int.h"
#include "pid.h"
#include "images/pagemap.pb-c.h"
#include "page.h"
#include "pagemap-cache.h"


struct dirty_log {
    pid_t pid;
	void *dirtymap;

	struct list_head pdm_list;
};

#define INIT_PAGE_DIRTY_MAP { \
    .dirtymap = NULL, \
}

int init_dirty_map_images(pid_t pid, struct dirty_log *dl);
struct page_dirty_map *search_vaddr_in_range_dg(u64 vaddr, struct dirty_log *dl);

#endif