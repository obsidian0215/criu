#include "dirty-pages.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>

// 页面类型名称数组
const char* page_type_names[] = {
    "PAGE_PTE",
    "PAGE_PMD",
    "PAGE_PUD",
};

int load_dirty_pages(const char *file_path, struct pstree_item *item) {

    FILE *file = fopen(file_path, "rb");
    struct dirty_page temp_page;
    size_t count = 0, capacity = 10;  // 初始容量
    struct dirty_page *new_ptr;

    if (!file) {
        perror("Failed to open file");
        return -1;
    }

    item->dirty_pages = malloc(capacity * sizeof(struct dirty_page));
    if (!item->dirty_pages) {
        fprintf(stderr, "Memory allocation failed.\n");
        fclose(file);
        return -1;
    }

    while (1) {
        const char *page_type_str = "UNKNOWN";
        size_t read_items = fread(&temp_page, sizeof(struct dirty_page), 1, file);
        if (read_items == 1) {
            if (count >= capacity) {
                capacity *= 2;
                new_ptr = realloc(item->dirty_pages, capacity * sizeof(struct dirty_page));
                if (!new_ptr) {
                    fprintf(stderr, "Memory reallocation failed.\n");
                    fclose(file);
                    return -1;
                }
                item->dirty_pages = new_ptr;
            }
            item->dirty_pages[count++] = temp_page;

            // 验证 page_type 是否有效
            
            if (temp_page.page_type < sizeof(page_type_names) / sizeof(page_type_names[0])) {
                page_type_str = page_type_names[temp_page.page_type];
            }

            // 使用跨平台的格式化宏打印信息
            printf("Page address: 0x%" PRIx64 ", Write count: %" PRIu64 ", Page type: %s\n",
                   temp_page.address, temp_page.write_count, page_type_str);
        } else if (feof(file)) {
            // 文件读取完毕
            break;
        } else {
            // 读取错误
            perror("Error reading dirty_page data");
            free(item->dirty_pages);
            item->dirty_pages = NULL;
            fclose(file);
            return -1;
        }
    }

    fclose(file);
    item->num_dirty_pages = count;
    return 0;
}

void free_dirty_pages(struct pstree_item *item) {
    if (item && item->dirty_pages) {
        free(item->dirty_pages);
        item->dirty_pages = NULL;
        item->num_dirty_pages = 0;
    }
}

void print_dirty_pages(struct pstree_item *item) {
    if (!item || !item->dirty_pages) {
        fprintf(stderr, "No dirty pages to print.\n");
        return;
    }

    for (size_t i = 0; i < item->num_dirty_pages; i++) {
        struct dirty_page *dp = &item->dirty_pages[i];
        const char *page_type_str = "UNKNOWN";

        if (dp->page_type < sizeof(page_type_names) / sizeof(page_type_names[0])) {
            page_type_str = page_type_names[dp->page_type];
        }

        // 使用跨平台的格式化宏打印信息
        printf("Page address: 0x%" PRIx64 ", Write count: %" PRIu64 ", Page type: %s\n",
               dp->address, dp->write_count, page_type_str);
    }
}