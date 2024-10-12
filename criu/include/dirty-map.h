#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include "int.h"
#include "pid.h"
#include "images/pagemap.pb-c.h"
#include "page.h"
#include "pagemap-cache.h"


typedef struct page_dirty_map {
	unsigned long vaddr;
	unsigned int nr_pages;
	unsigned int flags;
	int *dirtymap;

	struct list_head list;
} pdm_t;

struct dirty_log {
    pid_t pid;

	struct cr_img *dli;
	pdm_t *pdm;
	
	pdm_t **pdme;
	int nr_pdms;
	int cur_pdm;

	struct list_head pdm_list;
};

#define INIT_PAGE_DIRTY_MAP { \
    .dli = NULL, \
    .vaddr = NULL, \
    .nr_pages = 0, \
    .flags = 0, \
    .dirtymap = NULL; \
}

int init_dirty_map_images(pid_t pid, struct dirty_log *dl);
struct page_dirty_map *search_vaddr_in_range_dg(u64 vaddr, struct dirty_log *dl);
int update_dirty_map(pdm_t *pdm);

#endif