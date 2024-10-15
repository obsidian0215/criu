#ifndef __DIRTY_PAGES_H__
#define __DIRTY_PAGES_H__

#include <stdint.h>
#include <stddef.h>
#include "pstree.h"
// 页面类型定义
#define PAGE_PTE 0  // 4KB
#define PAGE_PMD 1  // 2MB
#define PAGE_PUD 2  // 1GB（当前设计下不会被使用）

// 页面类型名称数组
extern const char* page_type_names[];

// 脏页信息结构体，使用 packed 属性防止填充
struct __attribute__((__packed__)) dirty_page {
    uint64_t address;
    uint64_t write_count;
    uint32_t page_type;
};


// 函数声明
int load_dirty_pages(const char *file_path, struct pstree_item *item);
void free_dirty_pages(struct pstree_item *item);
void print_dirty_pages(struct pstree_item *item);
#endif // __DIRTY_PAGES_H__
