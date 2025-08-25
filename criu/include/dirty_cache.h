#ifndef __CR_DIRTY_CACHE_H__
#define __CR_DIRTY_CACHE_H__

#include <stdbool.h>
#include <sys/types.h>
#include "int.h"
#include "common/compiler.h"

/* Cache configuration */
#define DCACHE_SIZE     (32UL << 20)         /* 32MiB default size */
#define DC_WAY_NR         2                  /* 2-way set associative */
#define DC_PAGE_NR        (DCACHE_SIZE / PAGE_SIZE)      /* 8192 */
#define DC_SET_NR         (DC_PAGE_NR / DC_WAY_NR)       /* 4096 */
#define DC_MIN_EXPAND     (8UL << 20)        /* 8MB minimum expansion */
#define DC_MAX_SIZE       (512UL << 20)      /* 512MB maximum size */
#define DC_TMPFS_PREFIX   "dirty_cache"

/* Cache operation results */
enum dc_result {
	DC_SUCCESS = 0,
	DC_ERROR = -1,
	DC_NOT_FOUND = -2,
	DC_CACHE_FULL = -3,
	DC_EXPANSION_NEEDED = -4,
	DC_INVALID_PARAM = -5
};

typedef struct __attribute__((packed)) dc_entry {
    uint64_t vaddr;     /* 虚拟地址，页对齐 */
    uint8_t  valid;     /* 1=有效 0=无效 */
    uint8_t  lru;       /* 0=最近访问 1=最久未访问 */
    uint16_t _rsvd;     /* 对齐填充 */
} dc_entry_t;

typedef struct dirty_cache {
    /* Cache structure - simplified for per-process use */
    int      fd;           /* backing tmpfs file */
    void    *base;         /* mmap 基址 */
    dc_entry_t *meta;      /* 元数据数组指针 */
    uint8_t *data;         /* 页内容指针 */

    /* Cache configuration */
    size_t   total_size;   /* Total cache size */
    size_t   num_sets;     /* Number of cache sets */
    size_t   num_pages;    /* Total number of pages */

    /* Control flags */
    bool     initialized;  /* Initialization status */
} dirty_cache_t;

/* Core API - simplified for per-process use */
int  dc_init(dirty_cache_t *c, const char *path);
int  dc_init_with_size(dirty_cache_t *c, const char *path, size_t cache_size);
void dc_fini(dirty_cache_t *c);
void dc_update(dirty_cache_t *c, uint64_t vaddr, const void *page);
int  dc_lookup(dirty_cache_t *c, uint64_t vaddr, void *page_out);

/* Cache management */
int  dc_expand(dirty_cache_t *c, size_t additional_size);
int  dc_sync(dirty_cache_t *c);

/* Helper macros */
#define DC_ADDR_TO_SET(c, addr) (((addr) >> PAGE_SHIFT) & ((c)->num_sets - 1))
#define DC_IS_CACHE_ENABLED() (opts.use_dirty_map && opts.compress)

#endif /* __CR_DIRTY_CACHE_H__ */
