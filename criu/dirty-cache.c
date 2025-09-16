#include "dirty-cache.h"
#include "cr_options.h"
#include "xmalloc.h"
#include "log.h"
#include "util.h"
#include "page.h"
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mman.h>
#include <string.h>
#include <errno.h>

#undef LOG_PREFIX
#define LOG_PREFIX "dirty-cache: "

/* Forward declarations */
static int dc_expand_internal(dirty_cache_t *c, size_t new_size);

/* Helper functions for cache geometry */
static inline size_t dc_meta_size(size_t num_sets) {
    return num_sets * DC_WAY_NR * sizeof(dc_entry_t);
}

static inline size_t dc_data_offset(size_t num_sets) {
    return dc_meta_size(num_sets);
}

static inline size_t dc_file_size(size_t cache_size, size_t num_sets) {
    return dc_meta_size(num_sets) + cache_size;
}

static inline dc_entry_t *dc_get_set(dirty_cache_t *c, size_t idx) {
    return c->meta + idx * DC_WAY_NR;
}

static inline uint8_t *dc_get_page_data(dirty_cache_t *c, size_t set_idx, size_t way) {
    size_t offset = (set_idx * DC_WAY_NR + way) * PAGE_SIZE;
    return c->data + offset;
}

int dc_init_with_size(dirty_cache_t *c, const char *path, size_t cache_size)
{
    size_t num_sets, num_pages, file_size;
    int ret = -1;

    if (!c || !path) {
        pr_err("Invalid parameters\n");
        return -EINVAL;
    }

    memset(c, 0, sizeof(*c));

    /* Calculate cache geometry */
    num_pages = cache_size / PAGE_SIZE;
    num_sets = num_pages / DC_WAY_NR;
    file_size = dc_file_size(cache_size, num_sets);

    /* Initialize cache configuration */
    c->total_size = cache_size;
    c->num_sets = num_sets;
    c->num_pages = num_pages;

    /* Create and truncate tmpfs file */
    c->fd = open(path, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (c->fd < 0) {
        pr_perror("Failed to create cache file %s", path);
        return -1;
    }

    if (ftruncate(c->fd, file_size)) {
        pr_perror("Failed to truncate cache file to %zu bytes", file_size);
        goto err_close;
    }

    /* Memory-map the file */
    c->base = mmap(NULL, file_size, PROT_READ | PROT_WRITE, MAP_SHARED, c->fd, 0);
    if (c->base == MAP_FAILED) {
        pr_perror("Failed to mmap cache file");
        goto err_close;
    }

    /* Initialize data pointers */
    c->meta = (dc_entry_t *)c->base;
    c->data = (uint8_t *)c->base + dc_data_offset(num_sets);

    /* Initialize cache metadata */
    memset(c->base, 0, file_size);

    /* Set flags */
    c->initialized = true;

    pr_info("Initialized dirty cache: %zu bytes, %zu sets, %zu pages @ %s\n",
            cache_size, num_sets, num_pages, path);
    return 0;

err_close:
    close(c->fd);
    return ret;
}

void dc_fini(dirty_cache_t *c)
{
    size_t file_size;
    if (!c || !c->initialized)
        return;

    /* Sync and cleanup */
    if (c->base && c->base != MAP_FAILED) {
        file_size = dc_file_size(c->total_size, c->num_sets);
        msync(c->base, file_size, MS_SYNC);
        munmap(c->base, file_size);
    }

    if (c->fd >= 0)
        close(c->fd);

    memset(c, 0, sizeof(*c));
}

static int dc_find_way(dc_entry_t *set, uint64_t vaddr, size_t *hit_way)
{
    for (size_t w = 0; w < DC_WAY_NR; w++) {
        if (set[w].valid && set[w].vaddr == vaddr) {
            *hit_way = w;
            return 1;
        }
    }
    return 0;
}

static size_t dc_select_victim_way(dc_entry_t *set)
{
    /* Simple LRU replacement policy */
    for (size_t w = 0; w < DC_WAY_NR; w++) {
        if (!set[w].valid)
            return w; /* Use invalid entry first */
    }

    /* All entries valid, use LRU */
    return set[0].lru ? 0 : 1;
}

int dc_lookup(dirty_cache_t *c, uint64_t vaddr, void *page_out)
{
    size_t set_idx, way;
    dc_entry_t *set, *entry;
    uint8_t *page_data;

    if (!c || !c->initialized || !page_out)
        return DC_INVALID_PARAM;

    set_idx = DC_ADDR_TO_SET(c, vaddr);
    set = dc_get_set(c, set_idx);

    if (dc_find_way(set, vaddr, &way)) {
        entry = &set[way];

        page_data = dc_get_page_data(c, set_idx, way);

        /* Copy data and update LRU */
        memcpy(page_out, page_data, PAGE_SIZE);
        entry->lru = 0;
        set[way ^ 1].lru = 1; /* Update other way's LRU */

        return DC_SUCCESS;
    }

    return DC_NOT_FOUND;
}

void dc_update(dirty_cache_t *c, uint64_t vaddr, const void *page)
{
    size_t set_idx, way;
    dc_entry_t *set, *entry;
    uint8_t *page_data;

    if (!c || !c->initialized || !page)
        return;

    set_idx = DC_ADDR_TO_SET(c, vaddr);
    set = dc_get_set(c, set_idx);

    /* Check for hit */
    if (dc_find_way(set, vaddr, &way)) {
        entry = &set[way];
    } else {
        /* Select victim way */
        way = dc_select_victim_way(set);
        entry = &set[way];
    }

    /* Update entry */
    page_data = dc_get_page_data(c, set_idx, way);
    memcpy(page_data, page, PAGE_SIZE);

    entry->vaddr = vaddr;
    entry->valid = 1;
    entry->lru = 0;
    set[way ^ 1].lru = 1; /* Update other way's LRU */
}

int dc_expand(dirty_cache_t *c, size_t additional_size)
{
    size_t new_size;
    int ret;

    if (!c || !c->initialized)
        return DC_INVALID_PARAM;

    /* Ensure minimum expansion size */
    if (additional_size < DC_MIN_EXPAND) {
        additional_size = DC_MIN_EXPAND;
    }

    new_size = c->total_size + additional_size;

    /* Check maximum size limit */
    if (new_size > DC_MAX_SIZE) {
        pr_warn("Cache expansion would exceed maximum size (%lu MB)\n",
                DC_MAX_SIZE / (1024 * 1024));
        return DC_ERROR;
    }

    /* Expand the cache */
    ret = dc_expand_internal(c, new_size);

    if (ret == 0) {
        pr_info("Expanded cache from %zu to %zu bytes\n",
                c->total_size - additional_size, c->total_size);
    }

    return ret;
}

static int dc_expand_internal(dirty_cache_t *c, size_t new_size)
{
    size_t old_file_size, new_file_size, old_meta_size, new_meta_size;
    size_t new_num_pages, new_num_sets;
    void *new_base;

    /* Calculate new geometry */
    new_num_pages = new_size / PAGE_SIZE;
    new_num_sets = new_num_pages / DC_WAY_NR;

    old_file_size = dc_file_size(c->total_size, c->num_sets);
    new_file_size = dc_file_size(new_size, new_num_sets);

    /* Expand tmpfs file */
    if (ftruncate(c->fd, new_file_size) != 0) {
        pr_perror("Failed to expand tmpfs file");
        return DC_ERROR;
    }

    /* Remap memory */
    new_base = mremap(c->base, old_file_size, new_file_size, MREMAP_MAYMOVE);
    if (new_base == MAP_FAILED) {
        pr_perror("Failed to remap expanded cache");
        return DC_ERROR;
    }

    /* Update cache structure */
    c->base = new_base;
    c->meta = (dc_entry_t *)new_base;
    c->data = (uint8_t *)new_base + dc_data_offset(new_num_sets);
    c->total_size = new_size;
    c->num_sets = new_num_sets;
    c->num_pages = new_num_pages;

    /* Initialize new metadata space */
    if (new_num_sets > c->num_sets) {
        old_meta_size = dc_meta_size(c->num_sets);
        new_meta_size = dc_meta_size(new_num_sets);
        memset((uint8_t *)c->meta + old_meta_size, 0, new_meta_size - old_meta_size);
    }

    return DC_SUCCESS;
}

int dc_sync(dirty_cache_t *c)
{
    size_t file_size;
    if (!c || !c->initialized)
        return DC_INVALID_PARAM;

    file_size = dc_file_size(c->total_size, c->num_sets);

    if (msync(c->base, file_size, MS_SYNC) != 0) {
        pr_perror("Failed to sync cache to disk");
        return DC_ERROR;
    }

    return DC_SUCCESS;
}