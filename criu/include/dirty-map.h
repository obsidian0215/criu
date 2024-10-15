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




#endif