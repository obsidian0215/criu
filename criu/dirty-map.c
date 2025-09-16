#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <regex.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <linux/falloc.h>
#include <sys/uio.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <math.h>

#include "types.h"
#include "image.h"
#include "cr_options.h"
#include "servicefd.h"
#include "pagemap.h"
#include "restorer.h"
#include "rst-malloc.h"
#include "page-xfer.h"
#include "pstree.h"

#include "dirty-map.h"
#include "dirty-cache.h"  /* 引入脏页缓存 */
#include "xmalloc.h"
#include "protobuf.h"

#define TIMESTAMP_LIST_PREFIX "timestamp_list"
#define WARM_LIST_PREFIX "warm_list"
#define THRESHOLD_PREFIX "threshold"

#define MAX_FILES 32
#define EXPAND_WARM_BATCH 128

// 遍历统计温页预测的准确性
typedef struct {
    struct dirty_log *dl;
    unsigned int hit_warm;
    unsigned int miss_warm;
} traversal_data_t;

// 用于qsort的comparator函数
int compare_dirty_map(const void *a, const void *b) {
    struct dirty_map *dm_a = (struct dirty_map *)a;
    struct dirty_map *dm_b = (struct dirty_map *)b;
    if (dm_a->address < dm_b->address)
        return -1;
    else if (dm_a->address > dm_b->address)
        return 1;
    else
        return 0;
}

// 函数用于排序dirty_map数组
void sort_dirty_map(struct dirty_map *dm, unsigned long size) {
    if (dm && size > 1)
        qsort(dm, size, sizeof(struct dirty_map), compare_dirty_map);
}


// 比较函数，用于GTree排序
gint compare_warm_page(gconstpointer a, gconstpointer b, gpointer user_data) {
    unsigned long addr_a = *(const unsigned long *)a;
    unsigned long addr_b = *(const unsigned long *)b;
    if (addr_a < addr_b)
        return -1;
    else if (addr_a > addr_b)
        return 1;
    else
        return 0;
}

// 回调函数，用于遍历 GTree 并写入文件
static gboolean write_warm_page(gpointer key, gpointer value, gpointer user_data) {
    FILE *f = (FILE *)user_data;
    unsigned long *addr = (unsigned long *)key;
    char *s_count = (char *)value;
    warm_page_t wp;

    wp.address = *addr;
    wp.s_count = *s_count;

    if (fwrite(&wp, sizeof(warm_page_t), 1, f) != 1) {
        perror("[Obsidian0215] fwrite");
        return TRUE;  // 停止遍历
    }
    return FALSE;  // 继续遍历
}

/**
 * @brief 从warm.pid文件中读取warm_list到GTree
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param pid 进程pid
 * @param dl 指向存储warm_list的dirty_log结构体
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int load_warm_list(const char *dirty_map_dir, pid_t pid, struct dirty_log *dl) {
    char warm_list_filepath[PATH_MAX];
    FILE *file = NULL;
    warm_page_t wp;
    unsigned long addr, *new_key;
    char *existing_scount, *new_scount;
    int ret;

    if (!dl) {
        fprintf(stderr, "[Obsidian0215] Invalid dl pointer\n");
        errno = EINVAL;
        return -1;
    }

    // 构造文件路径
    ret = snprintf(warm_list_filepath, sizeof(warm_list_filepath), "%s/%s.%d", dirty_map_dir, WARM_LIST_PREFIX, pid);
    if (ret < 0 || ret >= sizeof(warm_list_filepath)) {
        fprintf(stderr, "[Obsidian0215] Error constructing warm list file path\n");
        errno = EINVAL;
        return -1;
    }

    // 打开文件，不存在时创建一个空文件
    file = fopen(warm_list_filepath, "rb");
    if (!file) {
        if (errno == ENOENT) {
            // 文件不存在，初始化空的GTree
            return 0;
        } else {
            perror("[Obsidian0215] fopen");
            return -1;
        }
    }

    // 加锁
    pthread_mutex_lock(&dl->warm_list_mutex);

    // 读取文件中的warm_page_t记录并插入到GTree
    while (fread(&wp, sizeof(warm_page_t), 1, file) == 1) {
        addr = wp.address;
        existing_scount = g_tree_lookup(dl->warm_list, &addr);
        if (existing_scount) {
            *existing_scount += wp.s_count;
        } else {
            new_key = malloc(sizeof(unsigned long));
            if (!new_key) {
                perror("[Obsidian0215] malloc");
                fclose(file);
                pthread_mutex_unlock(&dl->warm_list_mutex);
                return -1;
            }

            *new_key = addr;
            new_scount = malloc(sizeof(char));
            if (!new_scount) {
                perror("[Obsidian0215] malloc failed");
                free(new_key);
                fclose(file);
                pthread_mutex_unlock(&dl->warm_list_mutex);
                return -1;
            }
            *new_scount = wp.s_count;

            g_tree_insert(dl->warm_list, new_key, new_scount);
            dl->warm_size++;
        }
    }

    if (ferror(file)) {
        perror("[Obsidian0215] fread");
        fclose(file);
        pthread_mutex_unlock(&dl->warm_list_mutex);
        return -1;
    }

    fclose(file);
    pthread_mutex_unlock(&dl->warm_list_mutex);

    return 0;
}

/**
 * @brief 将warm_list保存到warm.pid文件中
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param pid 进程pid
 * @param dl 指向存储warm_list的dirty_log结构体
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int write_warm_list(struct dirty_log *dl, const char *dirty_map_dir) {
    char warm_list_filepath[PATH_MAX];
    pid_t pid;
    FILE *file = NULL;
    int ret = 0;
    // gboolean traverse_status;

    if (!dl) {
        fprintf(stderr, "[Obsidian0215] Invalid dl pointer\n");
        errno = EINVAL;
        return -1;
    }

    pid = dl->pid;
    // 构造文件路径
    ret = snprintf(warm_list_filepath, sizeof(warm_list_filepath), "%s/%s.%d", dirty_map_dir, WARM_LIST_PREFIX, pid);
    if (ret < 0 || ret >= sizeof(warm_list_filepath)) {
        fprintf(stderr, "[Obsidian0215] Error constructing warm list file path\n");
        errno = EINVAL;
        return -1;
    }

    // 打开文件用于写入（覆盖）
    file = fopen(warm_list_filepath, "wb");
    if (!file) {
        perror("[Obsidian0215] fopen");
        return -1;
    }

    // 加锁
    pthread_mutex_lock(&dl->warm_list_mutex);

    // 遍历GTree并写入文件
    g_tree_foreach(dl->warm_list, write_warm_page, file);
    // traverse_status = g_tree_foreach(dl->warm_list, write_warm_page, file);

    // if (!traverse_status) {
    //     fprintf(stderr, "[Obsidian0215] Error during g_tree_foreach\n");
    //     fclose(file);
    //     pthread_mutex_unlock(&dl->warm_list_mutex);
    //     return -1;
    // }

    if (ferror(file)) {
        perror("[Obsidian0215] fwrite");
        fclose(file);
        pthread_mutex_unlock(&dl->warm_list_mutex);
        return -1;
    }

    fclose(file);
    pthread_mutex_unlock(&dl->warm_list_mutex);

    return 0;
}

// /**
//  * @brief 从warm.pid文件中读取warm_list
//  *
//  * @param dirty_map_dir dirty_map目录的路径
//  * @param pid 进程pid
//  * @param warm_list 指向存储warm_list的指针
//  * @param warm_size 指针，存储warm_list的大小
//  * @return int 成功返回0，失败返回-1并设置errno。
//  */
// static int load_warm_list(const char *dirty_map_dir, pid_t pid, struct dirty_log *dl) {
//     char warm_list_filepath[PATH_MAX];
//     void *mapped = NULL;
//     unsigned long current_count, required_size;
//     int fd;
//     struct stat st;

//     if (!dl) {
//         pr_perror("[Obsidian0215]Invalid dl pointer");
//         return -1;
//     }

//     snprintf(warm_list_filepath, sizeof(warm_list_filepath), "%s/%s.%d", dirty_map_dir, WARM_LIST_PREFIX, pid);
//     warm_list_filepath[sizeof(warm_list_filepath) - 1] = '\0';
//     // 打开文件，不存在时创建一个文件
//     fd = open(warm_list_filepath, O_RDWR | O_CREAT, 0666);
//     if (fd == -1) {
//         pr_perror("[Obsidian0215]open %s", warm_list_filepath);
//         return -1;
//     }

//     // 获取文件大小
//     if (fstat(fd, &st) == -1) {
//         pr_perror("[Obsidian0215]fstat");
//         close(fd);
//         return -1;
//     }

//     // 如果文件大小不是整数倍的sizeof(unsigned long)，修正
//     if (st.st_size % sizeof(unsigned long) != 0) {
//         pr_perror("[Obsidian0215]Invalid warm_list file size");
//         close(fd);
//         return -1;
//     }

//     current_count = st.st_size / sizeof(warm_page_t);
//     dl->warm_size = current_count;

//     // 需要映射的总大小为(current_count+EXPAND_WARM_BATCH)个unsigned long
//     // 增加32是用于减小warm_list的扩展次数
//     required_size = (current_count + EXPAND_WARM_BATCH) * sizeof(warm_page_t);

//     // 映射文件到内存
//     mapped = mmap(NULL, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
//     if (mapped == MAP_FAILED) {
//         perror("mmap");
//         close(fd);
//         return -1;
//     }
//     close(fd);

//     dl->warm_list = (warm_page_t *)mapped;
//     dl->warm_max = current_count + EXPAND_WARM_BATCH;

//     return 0;
// }

// /**
//  * @brief 将warm_list更新到对应的文件中
//  *
//  * @param dl 进程dirty-log实例的指针
//  * @return int 成功返回0，失败返回-1并设置errno
//  */
// static int write_warm_list(struct dirty_log *dl, const char *dirty_map_dir) {
//     char warm_list_filepath[PATH_MAX];
//     unsigned long warm_size;
//     int fd;
//     struct stat st;

//     if (!dl) {
//         pr_perror("[Obsidian0215]Invalid dl pointer");
//         return -1;
//     }
//     snprintf(warm_list_filepath, sizeof(warm_list_filepath), "%s/%s.%d", dirty_map_dir, WARM_LIST_PREFIX, dl->pid);
//     warm_list_filepath[sizeof(warm_list_filepath) - 1] = '\0';
//     // 打开文件
//     fd = open(warm_list_filepath, O_RDWR, 0666);
//     if (fd == -1) {
//         pr_perror("[Obsidian0215]open %s", warm_list_filepath);
//         return -1;
//     }
//     // 获取文件大小
//     if (fstat(fd, &st) == -1) {
//         pr_perror("[Obsidian0215]fstat");
//         close(fd);
//         return -1;
//     }
//     warm_size = dl->warm_size * sizeof(unsigned long);
//     // 若当前文件大小不足以容纳warm_list，则扩展文件
//     if (st.st_size < warm_size) {
//         if (ftruncate(fd, warm_size) == -1) {
//             pr_perror("[Obsidian0215]ftruncate");
//             close(fd);
//             return -1;
//         }
//     }

//     // 同步更改到文件
//     if (msync(dl->warm_list, warm_size, MS_SYNC) == -1) {
//         perror("[Obsidian0215]msync");
//         return -1;
//     }

//     // 解除映射
//     if (munmap(dl->warm_list, dl->warm_max * sizeof(unsigned long)) == -1) {
//         pr_perror("[Obsidian0215]Error unmapping warm_list");
//         return -1;
//     }
//     return 0;
// }

/**
 * @brief 从timestamp_list.pid文件中读取timestamp_list
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param pid 进程pid
 * @param timestamp_list 指向存储timestamp_list的指针
 * @param ts_list_size 指针，存储timestamp_list的大小
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int load_timestamp_list(const char *dirty_map_dir, pid_t pid, unsigned long **timestamp_list, unsigned long *ts_list_size) {
    char timestamp_file_path[PATH_MAX];
    void *mapped = NULL;
    unsigned long current_size, current_count, required_size;
    int fd;
    struct stat st;

    snprintf(timestamp_file_path, sizeof(timestamp_file_path), "%s/%s.%d", dirty_map_dir, TIMESTAMP_LIST_PREFIX, pid);
    timestamp_file_path[sizeof(timestamp_file_path) - 1] = '\0';
    // 打开文件，如果不存在则创建并初始化
    fd = open(timestamp_file_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        pr_perror("[Obsidian0215]open %s", timestamp_file_path);
        return -1;
    }

    // 获取文件大小
    if (fstat(fd, &st) == -1) {
        pr_perror("[Obsidian0215]fstat");
        close(fd);
        return -1;
    }

    current_size = st.st_size;
    current_count = current_size / sizeof(unsigned long);

    // 如果文件大小不是整数倍的 sizeof(unsigned long)，修正
    if (current_size % sizeof(unsigned long) != 0) {
        pr_perror("[Obsidian0215]Invalid timestamp_list file size");
        close(fd);
        return -1;
    }

    // 需要映射的总大小为 current_count + 1 个 unsigned long
    required_size = (current_count + 1) * sizeof(unsigned long);

    // 如果当前文件大小小于 required_size，则扩展文件
    if (current_size < required_size) {
        if (ftruncate(fd, required_size) == -1) {
            pr_perror("[Obsidian0215]ftruncate");
            close(fd);
            return -1;
        }
        // // 清零新增加的空间
        // if (current_size == 0)
        //     // 文件刚创建，初始化为 0
        //     memset(&((*timestamp_list)[0]), 0, sizeof(int));
        // else {
            // // 其他情况，确保新的 int 空间为 0
            // int zero = 0;
            // if (pwrite(fd, &zero, sizeof(int), current_size) != sizeof(int)) {
            //     perror("pwrite");
            //     close(fd);
            //     return -1;
            // }
        // }
    }

    // 映射文件到内存
    mapped = mmap(NULL, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        perror("[Obsidian0215]mmap");
        close(fd);
        return -1;
    }
    close(fd);

    *timestamp_list = (unsigned long *)mapped;
    *ts_list_size = current_count;

    return 0;
}

/**
 * @brief 检查指定的时间戳是否存在于timestamp_list中
 *
 * @param timestamp_list 已映射的时间戳列表指针
 * @param ts_list_size 时间戳列表的大小
 * @param timestamp 要检查的时间戳
 * @return int 返回1表示存在，0表示不存在
 */
static int is_timestamp_in_list(unsigned long *timestamp_list, unsigned long ts_list_size, unsigned long timestamp) {
    // 如果timestamp_list为空直接覆盖ts_list内存的原有值
    if (!ts_list_size)
        return 0;

    for (unsigned long i = 0; i < ts_list_size; ++i) {
        if (timestamp_list[i] == timestamp) {
            return 1; // 存在
        }
    }
    return 0; // 不存在
}

/**
 * @brief 将新的时间戳追加到已映射的timestamp_list中
 *
 * @param timestamp_list 已映射的时间戳列表指针
 * @param ts_list_size 指向当前时间戳数量的指针
 * @param new_timestamp 要追加的新的时间戳
 * @return int 成功返回 0，失败返回-1并设置errno
 */
static int append_timestamp_to_list(unsigned long *timestamp_list, unsigned long *ts_list_size, unsigned long new_timestamp) {
    // 将 new_timestamp 写入预留的空间
    timestamp_list[*ts_list_size] = new_timestamp;

    // 增加时间戳数量
    (*ts_list_size)++;

    // 同步更改到文件
    if (msync(timestamp_list, (*ts_list_size) * sizeof(unsigned long), MS_SYNC) == -1) {
        perror("[Obsidian0215]msync");
        return -1;
    }
    return 0;
}

/**
 * @brief 将指定pid和timestamp的dirtymap文件映射到内存中。
 *
 * @param pid 目标进程的pid。
 * @param timestamp 要映射的dirtymap文件的时间戳
 * @param dirty_map_dir dirty_map目录的路径
 * @param dm 指向将存储映射后指针的指针
 * @param dm_size 指向将存储映射大小的指针
 * @return int 成功返回 0，失败返回 -1 并设置errno
 */
static int load_dirtymap(pid_t pid, unsigned long timestamp, const char *dirty_map_dir,
                struct dirty_map **dm, unsigned long *dm_size, dirtymap_header_t **header) {
    char dm_filepath[PATH_MAX];
    int fd;
    struct stat st;
    void *mapped;
    // dirtymap_header_t *tmp_header;

    if (timestamp == 0) {
        *dm = NULL;
        *dm_size = 0;
        return 0;
    }

    snprintf(dm_filepath, sizeof(dm_filepath), "%s/%d-%lu.dirtymap", dirty_map_dir, pid, timestamp);

    fd = open(dm_filepath, O_RDONLY);
    if (fd == -1) {
        pr_perror("[Obsidian0215]Error opening dirtymap %s", dm_filepath);
        return -1;
    }

    if (fstat(fd, &st) == -1) {
        pr_perror("[Obsidian0215]Error getting size of %s", dm_filepath);
        close(fd);
        return -1;
    }

    if (st.st_size == 0 ||
     (st.st_size - sizeof(dirtymap_header_t)) % sizeof(struct dirty_map) != 0) {
        pr_perror("[Obsidian0215]Size of dirtymap %s is invalid", dm_filepath);
        close(fd);
        return -1;
    }

    mapped = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        pr_perror("[Obsidian0215]Error mapping dirtymap %s", dm_filepath);
        close(fd);
        return -1;
    }

    close(fd);
    *header = (dirtymap_header_t *)mapped;
    // header->track_duration_ns = le64toh(tmp_header->total_duration_ns);
    *dm = (struct dirty_map *)((char *)mapped + sizeof(dirtymap_header_t));
    *dm_size = (st.st_size - sizeof(dirtymap_header_t)) / sizeof(struct dirty_map);
    // printf("[Obsidian0215] Successfully loaded dirtymap file %s (size: %lu bytes): %p\n",
    //        dm_filepath, *dm_size * sizeof(struct dirty_map), *dm);
    return 0;
}

/**
 * @brief 合并latest_dm和less_latest_dm，生成dirty_diffmap数组
 *
 * @param latest_dm 最新的dirty_map数组（默认已按地址排序）
 * @param ldm_size 最新dirty_map数组的大小
 * @param less_latest_dm 次新的dirty_map数组（默认已按地址排序）
 * @param lldm_size 次新dirty_map数组的大小
 * @param dirty_map_dir dirty_map目录的路径
 * @param dl 指向dirty_log结构体的指针
 * @return struct dirty_diffmap* 生成的diffmap
 */
struct dirty_diffmap* merge_dirty_maps(struct dirty_log *dl) {
    struct dirty_map * latest_dm = dl->latest_dm, *less_latest_dm = dl->less_latest_dm;
    unsigned long lsize = dl->ldm_size, slsize = dl->lldm_size;
    u64 ltd_ns, lltd_ns;
    unsigned long max_size, i = 0, j = 0, k = 0;
    struct dirty_diffmap *diffmap, *resized_diffmap;
    float pre_heat = 0.0, cur_heat = 0.0;

    // 两个dirty_map都为空，直接返回空diffmap
    if ((!latest_dm || !lsize)
     && (!less_latest_dm || !slsize)) {
        dl->diffmap_size = 0;
        return NULL;
    }

    ltd_ns = !dl->ldm_header ? 0 : dl->ldm_header->track_duration_ns;
    lltd_ns = !dl->lldm_header ? 0 : dl->lldm_header->track_duration_ns;

    BUG_ON(!ltd_ns && (lsize || latest_dm));
    BUG_ON(!lltd_ns && (slsize || less_latest_dm));

    // 更新最小的可能热度
    dl->min_heat = 1.0f / (float)(ltd_ns / 1e9);

    // 估算diffmap的最大可能大小
    if (latest_dm && less_latest_dm)
        max_size = lsize + slsize;
    else if (latest_dm)
        max_size = lsize;
    else
        max_size = slsize;

    diffmap = malloc(max_size * sizeof(struct dirty_diffmap));
    if (!diffmap) {
        pr_perror("[Obsidian0215]Failed to allocate memory for diffmap");
        // exit(EXIT_FAILURE);
        return NULL;
    }

    if (latest_dm && lsize > 0 && less_latest_dm && slsize > 0) {
        while (i < lsize && j < slsize) {
            if (latest_dm[i].address < less_latest_dm[j].address) {
                // 仅在latest_dm中存在
                diffmap[k].address = latest_dm[i].address;
                cur_heat = (float)(latest_dm[i].write_count) / (float)(ltd_ns / 1e9);
                diffmap[k].heat = cur_heat;
                diffmap[k].heat_trend = cur_heat;
                i++;
            }
            else if (latest_dm[i].address > less_latest_dm[j].address) {
                // 仅在less_latest_dm中存在
                diffmap[k].address = less_latest_dm[j].address;
                pre_heat = (float)(less_latest_dm[j].write_count) / (float)(lltd_ns / 1e9);
                diffmap[k].heat = 0.0;
                diffmap[k].heat_trend = -pre_heat;
                j++;
            }
            else {
                // 同时存在于两个数组中
                diffmap[k].address = latest_dm[i].address;
                cur_heat = (float)(latest_dm[i].write_count) / (float)(ltd_ns / 1e9);
                pre_heat = (float)(less_latest_dm[j].write_count) / (float)(lltd_ns / 1e9);
                diffmap[k].heat = cur_heat;
                diffmap[k].heat_trend = cur_heat - pre_heat;
                i++;
                j++;
            }
            k++;
        }

        // 处理剩余的 latest_dm 条目
        while (i < lsize) {
            diffmap[k].address = latest_dm[i].address;
            cur_heat = (float)(latest_dm[i].write_count) / (float)(ltd_ns / 1e9);
            diffmap[k].heat = cur_heat;
            diffmap[k].heat_trend = cur_heat;
            i++;
            k++;
        }

        // 处理剩余的 less_latest_dm 条目
        while (j < slsize) {
            diffmap[k].address = less_latest_dm[j].address;
            diffmap[k].heat = 0.0;
            pre_heat = (float)(less_latest_dm[j].write_count) / (float)(lltd_ns / 1e9);
            diffmap[k].heat_trend = -pre_heat;
            j++;
            k++;
        }
    } else if (latest_dm && lsize > 0) {
        // 仅latest_dm存在，less_latest_dm为空
        for (; i < lsize; i++, k++) {
            diffmap[k].address = latest_dm[i].address;
            cur_heat = (float)(latest_dm[i].write_count) / (float)(ltd_ns / 1e9);
            diffmap[k].heat = cur_heat;
            diffmap[k].heat_trend = cur_heat;
        }
    } else if (less_latest_dm && slsize > 0) {
        // 仅less_latest_dm存在，latest_dm为空
        for (; j < slsize; j++, k++) {
            diffmap[k].address = less_latest_dm[j].address;
            diffmap[k].heat = 0.0;
            pre_heat = (float)(less_latest_dm[j].write_count) / (float)(lltd_ns / 1e9);
            diffmap[k].heat_trend = -pre_heat;
        }
    }

    // 更新实际的diffmap大小
    dl->diffmap_size = k;

    // 如果没有任何条目，释放分配的内存并返回NULL
    if (!k) {
        free(diffmap);
        return NULL;
    }

    // 重新分配内存以节省空间
    resized_diffmap = (struct dirty_diffmap *)realloc(diffmap, k * sizeof(struct dirty_diffmap));
    if (!resized_diffmap && k > 0) {
        pr_perror("[Obsidian0215]re-alloc diffmap failed");
        free(diffmap);
        // exit(EXIT_FAILURE);
        return NULL;
    }

    return resized_diffmap;
}

/**
 * @brief debug输出dirty_map数组
 *
 * @param dirtymap dirty_map数组（默认已按地址排序）
 * @param dirtymap_size dirty_map数组的大小
 * @param pid dirtymap所属的进程PID
 * @return void
 */
// static void debug_show_dirtymap(struct dirty_map *dirtymap, unsigned long dirtymap_size, pid_t pid) {
//     int i;

//     if (pr_quelled(LOG_DEBUG) || !dirtymap || !dirtymap_size)
// 		return;

//     pr_debug("Dirtymap for pid %d:(size: %ld)\n", pid, dirtymap_size);
// 	for (i = 0; i < dirtymap_size; i++) {
// 		pr_debug("\taddress: %#lx, write count: %d\n",
//             dirtymap[i].address, dirtymap[i].write_count);
// 	}
// }

/**
 * @brief debug输出dirty_diffmap数组
 *
 * @param diffmap dirty_diffmap数组（默认已按地址排序）
 * @param diffmap_size dirty_diffmap数组的大小
 * @param pid diffmap所属的进程PID
 * @return void
 */
// static void debug_show_diffmap(struct dirty_diffmap *diffmap, unsigned long diffmap_size, pid_t pid) {
//     int i;

//     if (pr_quelled(LOG_DEBUG) || !diffmap || !diffmap_size)
// 		return;

//     pr_debug("Diffmap for pid %d:(size: %lu)\n", pid, diffmap_size);
// 	for (i = 0; i < diffmap_size; i++) {
// 		pr_debug("\taddress: %#lx, heat: %f, heat trend: %f\n",
//             diffmap[i].address, diffmap[i].heat, diffmap[i].heat_trend);
// 	}
// }

/**
 * @brief 回调函数，用于打印每个 warm_page_t 节点
 *
 * @param key 指向 warm_page_t 的指针
 * @param value 此处为 NULL（根据您的 GTree 初始化方式）
 * @param user_data 额外用户数据，此处未使用
 * @return gboolean 返回 TRUE 以继续遍历，返回 FALSE 以停止遍历
 */
gboolean print_warm_page(gpointer key, gpointer value, gpointer user_data) {
    unsigned long *addr = (unsigned long *)key;
    char *s_count = (char *)value;
    if (addr) {
        pr_info("Address: 0x%lx, s_count: %d\n", *addr, *s_count);
    } else {
        pr_perror("Invalid warm_page_t pointer.\n");
        return TRUE;
    }
    return FALSE; // 继续遍历
}

/**
 * @brief 从thresholds.pid加载dirty_log的温页判断阈值
 *        若不存在则使用预设值初始化
 * @param dl 进程的dirty-log结构体指针
 */
static void load_thresholds(struct dirty_log *dl, const char *dirty_map_dir) {
    char threshold_file_path[PATH_MAX];
    int fd, ret;
    struct stat st;

    if (!dl) {
        pr_perror("[Obsidian0215]load_thresholds: dl is NULL");
        return;
    }

    snprintf(threshold_file_path, sizeof(threshold_file_path), "%s/%s.%d", dirty_map_dir, THRESHOLD_PREFIX, dl->pid);
    threshold_file_path[sizeof(threshold_file_path) - 1] = '\0';
    // 打开文件，如果不存在则创建并初始化
    fd = open(threshold_file_path, O_RDWR | O_CREAT, 0666);
    if (fd == -1) {
        pr_perror("[Obsidian0215]open %s", threshold_file_path);
        return;
    }

    // 获取文件大小
    if (fstat(fd, &st) == -1) {
        pr_perror("[Obsidian0215]fstat");
        close(fd);
        return;
    }

    if (st.st_size != 2 * sizeof(float)) {
        if (!st.st_size) {
            if (ftruncate(fd, 2 * sizeof(float)) == -1) {
                pr_perror("[Obsidian0215]ftruncate");
                close(fd);
                return;
            }
            if (!dl->ldm_header) {
                dl->heat_threshold = INITIAL_HEAT_THRESHOLD;
            } else
                dl->heat_threshold = (float)2 / (float)(dl->ldm_header->track_duration_ns / 1e9);
            dl->trend_threshold = INITIAL_TREND_THRESHOLD;
            return;
        } else {
            pr_perror("[Obsidian0215]Size of thresholds %s is invalid", threshold_file_path);
            close(fd);
            return;
        }
    }

    // read thresholds
    // 重置文件偏移到开头
    if (lseek(fd, 0, SEEK_SET) == -1) {
        pr_perror("[Obsidian0215]lseek");
        close(fd);
        return;
    }

    ret = read(fd, &dl->heat_threshold, sizeof(float));
    if (ret != sizeof(float)) {
        pr_perror("[Obsidian0215]load heat_threshold");
        close(fd);
        return;
    }

    ret = read(fd, &dl->trend_threshold, sizeof(float));
    if (ret != sizeof(float)) {
        pr_perror("[Obsidian0215]load trend_threshold");
        close(fd);
        return;
    }
    close(fd);
}

// 优化1：初始化历史统计信息
static int init_historical_stats(struct dirty_log *dl) {
    if (!dl)
        return -1;

    if (!dl->historical_stats) {
        dl->historical_stats = (historical_stats_t *)xzalloc(sizeof(historical_stats_t));
        if (!dl->historical_stats) {
            pr_perror("[Phase1] Failed to allocate historical_stats");
            return -1;
        }
    }

    // 初始化历史统计信息
    memset(dl->historical_stats->hit_rates, 0, sizeof(float) * HISTORY_BUFFER_SIZE);
    memset(dl->historical_stats->threshold_history, 0, sizeof(float) * HISTORY_BUFFER_SIZE);
    memset(dl->historical_stats->adjustment_factors, 0, sizeof(float) * HISTORY_BUFFER_SIZE);

    dl->historical_stats->history_idx = 0;
    dl->historical_stats->ema_hit_rate = 0.0f;
    dl->historical_stats->learning_rate = 0.1f;
    dl->historical_stats->momentum_factor = 0.8f;
    dl->historical_stats->update_count = 0;

    return 0;
}

// 优化1：初始化反馈控制器
static int init_feedback_controller(struct dirty_log *dl) {
    if (!dl)
        return -1;

    if (!dl->feedback_ctrl) {
        dl->feedback_ctrl = (feedback_controller_t *)xzalloc(sizeof(feedback_controller_t));
        if (!dl->feedback_ctrl) {
            pr_perror("[Phase1] Failed to allocate feedback_ctrl");
            return -1;
        }
    }

    // 初始化反馈控制器参数
    dl->feedback_ctrl->target_warm_ratio = TARGET_WARM_RATIO;
    dl->feedback_ctrl->adjustment_step = 0.5f; // 初始调整步长
    dl->feedback_ctrl->integral_error = 0.0f;
    dl->feedback_ctrl->prev_error = 0.0f;
    dl->feedback_ctrl->kp = KP_DEFAULT;
    dl->feedback_ctrl->ki = KI_DEFAULT;
    dl->feedback_ctrl->kd = KD_DEFAULT;
    dl->feedback_ctrl->new_warm_count = 0;
    dl->feedback_ctrl->total_warm_count = 0;

    return 0;
}

// 优化1：计算指数移动平均命中率
static float calculate_ema_hit_rate(historical_stats_t *stats, float current_hit_rate) {
    if (!stats)
        return current_hit_rate;

    // 指数移动平均计算
    stats->ema_hit_rate = EMA_ALPHA * current_hit_rate + (1.0f - EMA_ALPHA) * stats->ema_hit_rate;
    return stats->ema_hit_rate;
}

// 优化1：计算波动性（用于动态调整步长）
static float calculate_volatility(historical_stats_t *stats) {
    float sum = 0.0f, mean = 0.0f, variance = 0.0f;
    int count = 0, i;

    if (!stats || stats->update_count < 2)
        return 0.0f;

    // 计算最近10次调整因子的平均值和方差
    for (i = 0; i < HISTORY_BUFFER_SIZE; i++) {
        if (stats->adjustment_factors[i] != 0.0f) {
            sum += stats->adjustment_factors[i];
            count++;
        }
    }

    if (count < 2)
        return 0.0f;
    mean = sum / count;

    // 计算方差
    for (i = 0; i < HISTORY_BUFFER_SIZE; i++) {
        if (stats->adjustment_factors[i] != 0.0f) {
            float diff = stats->adjustment_factors[i] - mean;
            variance += diff * diff;
        }
    }

    variance /= count;
    return sqrtf(variance); // 返回标准差作为波动性度量
}

// 优化1：计算动态调整步长
static float calculate_dynamic_step(historical_stats_t *stats, feedback_controller_t *fb) {
    float volatility, adaptive_step;

    if (!stats || !fb)
        return 0.0f;

    volatility = calculate_volatility(stats);
    adaptive_step = fb->adjustment_step * (1.0f + volatility);

    // 限制调整步长在合理范围内
    if (adaptive_step > MAX_ADJUSTMENT_STEP)
        adaptive_step = MAX_ADJUSTMENT_STEP;
    else if (adaptive_step < MIN_ADJUSTMENT_STEP)
        adaptive_step = MIN_ADJUSTMENT_STEP;

    return adaptive_step;
}

// 优化1：更新历史统计信息
static void update_historical_stats(historical_stats_t *stats, float hit_rate,
                                   float threshold, float adjustment_factor) {
    int idx;
    if (!stats)
        return;

    // 更新历史数据（循环缓冲区）
    idx = stats->history_idx;

    stats->hit_rates[idx] = hit_rate;
    stats->threshold_history[idx] = threshold;
    stats->adjustment_factors[idx] = adjustment_factor;

    stats->history_idx = (idx + 1) % HISTORY_BUFFER_SIZE;
    stats->update_count++;
}

// 遍历回调函数，用于统计hit_warm和miss_warm
static gboolean count_warm_pages(gpointer key, gpointer value, gpointer user_data) {
    traversal_data_t *data = (traversal_data_t *)user_data;
    unsigned long addr = *(unsigned long *)key;
    struct dirty_diffmap *dm = search_dirty_map(data->dl, addr);

    if (!dm || dm->heat < data->dl->min_heat) {
        data->hit_warm++;
    } else {
        data->miss_warm++;
    }

    return FALSE; // 继续遍历
}

/**
 * @brief 优化1：改进的温页阈值更新算法（使用指数移动平均）
 *
 * @param dl 进程的dirty-log结构体指针
 */
static void update_thresholds(struct dirty_log *dl) {
    struct dirty_diffmap *dirtymap = dl->diffmap;
    unsigned int hit_warm = 0, miss_warm = 0, new_warm = 0, i;
    float min_heat_threshold = 0.0, min_in_dirtymap = 0.0;
    float current_hit_rate = 0.0f, ema_hit_rate = 0.0f;
    float adjustment_factor = 1.0f, dynamic_step = 0.5f;
    float current_ratio, error, p_term, i_term, d_term, pid_output;
    traversal_data_t data;

    // 检查warm_list和dirtymap是否存在
    if (!dl->warm_list || !dirtymap) {
        pr_info("[Phase1] warm threshold for %d won't update\n", dl->pid);
        return;
    }

    // 初始化min_in_dirtymap
    if (dl->diffmap_size > 0) {
        min_in_dirtymap = dirtymap[0].heat;
    } else {
        // 如果dirtymap为空，无法更新阈值
        pr_info("[Phase1] dirtymap is empty for %d, thresholds not updated\n", dl->pid);
        return;
    }

    // 初始化遍历数据
    data = (traversal_data_t){ .dl = dl, .hit_warm = 0, .miss_warm = 0 };

    // 加锁并遍历warm_list
    pthread_mutex_lock(&dl->warm_list_mutex);
    g_tree_foreach(dl->warm_list, count_warm_pages, &data);
    pthread_mutex_unlock(&dl->warm_list_mutex);

    hit_warm = data.hit_warm;
    miss_warm = data.miss_warm;

    // 计算当前命中率
    if (hit_warm + miss_warm > 0) {
        current_hit_rate = (float)hit_warm / (hit_warm + miss_warm);
    }

    // Phase 1优化：使用指数移动平均计算平滑命中率
    if (dl->historical_stats) {
        ema_hit_rate = calculate_ema_hit_rate(dl->historical_stats, current_hit_rate);
    } else {
        // 初始化历史统计信息
        if (init_historical_stats(dl) == 0) {
            ema_hit_rate = calculate_ema_hit_rate(dl->historical_stats, current_hit_rate);
        } else {
            ema_hit_rate = current_hit_rate; // 降级到当前命中率
        }
    }

    // Phase 1优化：当EMA命中率不高于50%时更新阈值
    if (ema_hit_rate <= 0.5f && miss_warm > 0) {
        // 计算min_heat_threshold
        if (dl->ldm_header && dl->ldm_header->track_duration_ns > 0) {
            min_heat_threshold = 1.0f / ((float)dl->ldm_header->track_duration_ns / 1e9f);
        } else {
            min_heat_threshold = 0.0f;
        }

        // Phase 1优化：计算动态调整步长
        if (dl->feedback_ctrl) {
            dynamic_step = calculate_dynamic_step(dl->historical_stats, dl->feedback_ctrl);
        }

        // Phase 1优化：使用EMA命中率进行调整
        adjustment_factor = ema_hit_rate * 2.0f; // 放大调整因子以提高响应性
        dl->heat_threshold = fmaxf(min_heat_threshold, dl->heat_threshold * adjustment_factor);

        // Phase 1优化：使用动态步长调整trend_threshold
        dl->trend_threshold = dl->trend_threshold + dynamic_step * (0.35f - dl->trend_threshold * 0.65f);

        pr_debug("[Phase1] PID %d: ema_hit_rate=%.3f, dynamic_step=%.3f, new_heat=%.3f, new_trend=%.3f\n",
                dl->pid, ema_hit_rate, dynamic_step, dl->heat_threshold, dl->trend_threshold);
    }

    // 用更新的thresholds选择dirtymap的温页并更新new_warm
    for (i = 0; i < dl->diffmap_size; i++) {
        if (dirtymap[i].heat <= dl->heat_threshold
         && -dirtymap[i].heat_trend > dl->trend_threshold * dirtymap[i].heat
         && !search_warm_list(dl, dirtymap[i].address)) {
            new_warm++;
        }
        if (dirtymap[i].heat < min_in_dirtymap) {
            min_in_dirtymap = dirtymap[i].heat;
        }
    }

    // Phase 1优化：改进的反馈控制逻辑
    if (dl->feedback_ctrl) {
        dl->feedback_ctrl->new_warm_count = new_warm;
        dl->feedback_ctrl->total_warm_count = miss_warm;

        // 计算误差（基于目标温页比例）
        current_ratio = (float)new_warm / dl->diffmap_size;
        error = dl->feedback_ctrl->target_warm_ratio - current_ratio;

        // PID控制器调整
        p_term = dl->feedback_ctrl->kp * error;
        i_term = dl->feedback_ctrl->ki * dl->feedback_ctrl->integral_error;
        d_term = dl->feedback_ctrl->kd * (error - dl->feedback_ctrl->prev_error);

        pid_output = p_term + i_term + d_term;

        // 新温页过少时，适当放宽选择阈值
        if (new_warm < 32 || new_warm < (unsigned int)(0.054f * miss_warm)) {
            if (min_in_dirtymap > min_heat_threshold) {
                dl->heat_threshold = fmaxf(dl->heat_threshold, min_in_dirtymap * (1.0f + pid_output));
            } else {
                dl->trend_threshold = fmaxf(0.1f, dl->trend_threshold * (0.85f - pid_output));
            }
        } else if (new_warm >= 32 && new_warm > (unsigned int)(0.25f * miss_warm)) {
            // 新温页过多时，收紧阈值
            dl->trend_threshold = fminf(2.0f, dl->trend_threshold * (1.15f + pid_output));
            dl->heat_threshold = fmaxf(min_in_dirtymap, dl->heat_threshold * (1.0f - fabsf(pid_output)));
        }

        // 更新积分误差和上一轮误差
        dl->feedback_ctrl->integral_error += error;
        dl->feedback_ctrl->prev_error = error;

        // 限制积分误差范围
        if (dl->feedback_ctrl->integral_error > 1.0f)
            dl->feedback_ctrl->integral_error = 1.0f;
        else if (dl->feedback_ctrl->integral_error < -1.0f)
            dl->feedback_ctrl->integral_error = -1.0f;
    } else {
        // 降级到原有逻辑（如果反馈控制器未初始化）
        if (new_warm < 32 || new_warm < (unsigned int)(0.054f * miss_warm)) {
            if (min_in_dirtymap > min_heat_threshold) {
                dl->heat_threshold = 1.25 * min_in_dirtymap;
            } else {
                dl->trend_threshold = 0.85f * dl->trend_threshold;
            }
        } else if (new_warm >= 32 && new_warm > (unsigned int)(0.25f * miss_warm)) {
            dl->trend_threshold = 1.15f * dl->trend_threshold;
            dl->heat_threshold = fmaxf(min_in_dirtymap, dl->heat_threshold);
        }
    }

    // Phase 1优化：更新历史统计信息
    if (dl->historical_stats) {
        update_historical_stats(dl->historical_stats, ema_hit_rate,
                               dl->heat_threshold, adjustment_factor);
    }
}

/**
 * @brief 将dirty_log的温页判断阈值写入thresholds.pid
 *        若不存在则使用预设值初始化
 * @param dl 进程的dirty-log结构体指针
 */
static int write_thresholds(struct dirty_log *dl, const char *dirty_map_dir) {
    char threshold_file_path[PATH_MAX];
    int fd, ret;
    struct stat st;

    if (!dl) {
        pr_perror("[Obsidian0215]write_thresholds: dl is NULL");
        return -1;
    }

    snprintf(threshold_file_path, sizeof(threshold_file_path), "%s/%s.%d", dirty_map_dir, THRESHOLD_PREFIX, dl->pid);
    threshold_file_path[sizeof(threshold_file_path) - 1] = '\0';
    // 打开文件，如果不存在则创建并初始化
    fd = open(threshold_file_path, O_RDWR, 0666);
    if (fd == -1) {
        pr_perror("[Obsidian0215]open %s", threshold_file_path);
        return -1;
    }

    // 获取文件大小
    if (fstat(fd, &st) == -1) {
        pr_perror("[Obsidian0215]fstat");
        close(fd);
        return -1;
    }

    if (st.st_size != 2 * sizeof(float)) {
        if (ftruncate(fd, 2 * sizeof(float)) == -1) {
            perror("[Obsidian0215]adjust thresholds file size");
            close(fd);
            return -1;
        }
    }

    // write thresholds
    // 重置文件偏移到开头
    if (lseek(fd, 0, SEEK_SET) == -1) {
        pr_perror("[Obsidian0215]lseek");
        close(fd);
        return -1;
    }

    ret = write(fd, &dl->heat_threshold, sizeof(float));
    if (ret != sizeof(float)) {
        pr_perror("[Obsidian0215]write heat_threshold");
        close(fd);
        return -1;
    }

    ret = write(fd, &dl->trend_threshold, sizeof(float));
    if (ret != sizeof(float)) {
        pr_perror("[Obsidian0215]write trend_threshold");
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}


/**
 * @brief 为特定 pid 进程初始化其 dirty_map，读取最新和次新的 dirtymap 文件。
 *
 * @param item 指向 per-process 结构 <pid> 的指针。
 * @param dirty_map_dir dirty_map 目录的字符串。
 * @return int 成功返回 0，失败返回 -1。
 */
int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir){
    struct dirty_log *dl;
    pid_t pid = item->pid->real;
    char pattern[256], timestamp_str[64], *endptr;
    // char current_dirty_map_path[PATH_MAX];
    int ret, len, in_list;
    unsigned long timestamp;
    DIR *dir;
    regex_t regex;
    struct dirent *entry;
    regmatch_t matches[2];
    struct pid_check pc = {.pid = pid, .is_tracked = 0};

    // [Obsidian0215] init dirty-log for pid
    dl = (struct dirty_log *)xzalloc(sizeof(struct dirty_log));
    if (!dl) {
        pr_perror("[Obsidian0215]Failed to allocate memory for dirty-log");
        return -1;
    }
    INIT_DIRTY_LOG_PTR(dl);
    dl->pid = pid;
    item->dl = dl;

    // 打开 DT_DEV_PATH 并验证 dirty_map_dir
    ret = init_dirty_track(dl);
    if (ret) {
        pr_perror("[Obsidian0215]Failed to open dirty_track device %s", DT_DEV_PATH);
        return -1;
    }

    pthread_mutex_init(&dl->warm_list_mutex, NULL);
    dl->warm_list = g_tree_new_full(compare_warm_page, NULL, free, free);
    // 读取warm_list.<pid>文件，初始化warm_list
    ret = load_warm_list(dirty_map_dir, pid, dl);
    if (ret < 0) {
        pr_perror("[Obsidian0215]Failed to load warm_list for pid %d", pid);
        return -1;
    }
    pr_info("[Obsidian0215]Traversing warm_list:\n");
    g_tree_foreach(dl->warm_list, print_warm_page, NULL);
    pr_info("End of warm_list traversal.\n");

    // 读取timestamp_list.<pid>文件，初始化timestamp_list
    ret = load_timestamp_list(dirty_map_dir, pid, &dl->timestamp_list, &dl->ts_list_size);
    if (ret < 0) {
        pr_perror("[Obsidian0215]Failed to load timestamp_list for pid %d", pid);
        return -1;
    }

    // 若timestamp_list不为空则将记录的最后一个timestamp作为less_latest_timestamp
    if (dl->ts_list_size) {
        dl->less_latest_timestamp = dl->timestamp_list[dl->ts_list_size-1];
    } else {
        // 什么都不干，已初始化为0
        // dl->less_latest_timestamp = 0;
    }

    // 停止pid的dirty track以生成dirty-map文件
    ret = ioctl(dl->dirty_track_fd, IOCTL_CHECK_PID, &pc);
    if (!ret && pc.is_tracked) {
        ret = ioctl(dl->dirty_track_fd, IOCTL_STOP_PID, &pid);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Error stopping dirty-track for pid %d", pid);
            close(dl->dirty_track_fd);
            return -1;
        }
    } else if (!ret && !pc.is_tracked) {
        pr_info("[Obsidian0215]pid %d is not tracked by dirty-track LKM\n", pid);
    } else {
        pr_perror("[Obsidian0215]Error checking if pid %d is tracked by dirty-track LKM", pid);
        close(dl->dirty_track_fd);
        return -1;
    }
    // close(dl->dirty_track_fd);

    // 扫描dirty_map_dir，查找新的dirtymap文件
    dir = opendir(dirty_map_dir);
    if (!dir) {
        pr_perror("[Obsidian0215]%s opendir failed", dirty_map_dir);
        return -1;
    }

    // 编译正则表达式以匹配<pid>-<timestamp>.dirtymap
    snprintf(pattern, sizeof(pattern), "^%d-([0-9]+)\\.dirtymap$", pid);
    if (regcomp(&regex, pattern, REG_EXTENDED) != 0) {
        pr_perror("[Obsidian0215]Failed to compile regex: %s", pattern);
        closedir(dir);
        return -1;
    }

    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type != DT_REG)
            continue;

        ret = regexec(&regex, entry->d_name, 2, matches, 0);
        if (ret == 0) {
            // 提取 timestamp
            len = matches[1].rm_eo - matches[1].rm_so;
            if (len <= 0 || len >= 64) {
                pr_perror("[Obsidian0215]Invalid timestamp in file name: %s", entry->d_name);
                continue;
            }
            strncpy(timestamp_str, entry->d_name + matches[1].rm_so, len);
            timestamp_str[len] = '\0';

            timestamp = strtoul(timestamp_str, &endptr, 10);
            if (*endptr != '\0') { // 确保整个字符串都被转换
                pr_perror("[Obsidian0215]Non-numeric characters in timestamp: %s", timestamp_str);
                continue;
            }
            if (timestamp <= 0 || (timestamp == ULONG_MAX && errno == ERANGE)) {
                pr_perror("[Obsidian0215]Invalid timestamp value: %s", timestamp_str);
                continue;
            }

            // 检查 timestamp 是否已在 timestamp_list 中
            in_list = is_timestamp_in_list(dl->timestamp_list, dl->ts_list_size, timestamp);
            if (in_list == 1) {
                continue; // 已存在
            }

            // 找到一个新的timestamp，更新latest_timestamp退出循环
            if (timestamp) {
                dl->latest_timestamp = timestamp;
                break;
            }
        }
    }
    regfree(&regex);
    closedir(dir);

    // 将更新的latest_timestamp追加到timestamp_list
    ret = append_timestamp_to_list(dl->timestamp_list, &dl->ts_list_size, dl->latest_timestamp);
    if (ret < 0) {
        pr_perror("[Obsidian0215]Failed to append latest timestamp %lu to timestamp_list.%d",
                dl->latest_timestamp, pid);
        // 追加失败也继续执行
    } else {
        pr_info("[Obsidian0215]PID %d: latest timestamp = %lu, less-latest timestamp = %lu\n",
                pid, dl->latest_timestamp, dl->less_latest_timestamp);
    }

    // 解除映射的timestamp_list
    if (munmap(dl->timestamp_list, dl->ts_list_size * sizeof(unsigned long)) == -1) {
        pr_perror("[Obsidian0215]Error unmapping timestamp_list");
    }
    dl->timestamp_list = NULL;
    dl->ts_list_size = 0;

    // 映射latest_dm
    if (dl->latest_timestamp) {
        ret = load_dirtymap(pid, dl->latest_timestamp, dirty_map_dir,
                          &dl->latest_dm, &dl->ldm_size, &dl->ldm_header);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Failed to map latest dirtymap for pid %d", pid);
            dl->latest_dm = NULL;
            dl->ldm_size = 0;
        } else {
            pr_info("[Obsidian0215]successfully loaded %d's latest dirty-map: %p\n",
                    pid, dl->latest_dm);
            pr_info("\ttrack_duration: %lu ns, size: %lu bytes\n",
                    dl->ldm_header->track_duration_ns, dl->ldm_size * sizeof(struct dirty_map));
        }
    } else {
        dl->latest_dm = NULL;
        dl->ldm_size = 0;
    }

    // if (dl->latest_dm && dl->ldm_size) {
    //     pr_info("[Obsidian0215]latest dirtymap for pid %d:\n", pid);
    //     debug_show_dirtymap(dl->latest_dm, dl->ldm_size, pid);
    // } else if (!dl->latest_dm || !dl->ldm_size)
    //     pr_info("[Obsidian0215]No latest dirtymap for pid %d\n", pid);

    // 映射less_latest_dm
    if (dl->less_latest_timestamp) {
        ret = load_dirtymap(pid, dl->less_latest_timestamp, dirty_map_dir,
                          &dl->less_latest_dm, &dl->lldm_size, &dl->lldm_header);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Failed to map previous dirtymap for pid %d", pid);
            dl->less_latest_dm = NULL;
            dl->lldm_size = 0;
        } else {
            pr_info("[Obsidian0215]successfully loaded %d's previous dirty-map: %p\n",
                    pid, dl->less_latest_dm);
            pr_info("\ttrack_duration: %lu ns, size: %lu bytes\n",
                    dl->lldm_header->track_duration_ns, dl->lldm_size * sizeof(struct dirty_map));
        }
    } else {
        dl->less_latest_dm = NULL;
        dl->lldm_size = 0;
    }

    // if (dl->less_latest_dm && dl->lldm_size) {
    //     pr_info("[Obsidian0215]less-latest dirtymap for pid %d:\n", pid);
    //     debug_show_dirtymap(dl->less_latest_dm, dl->lldm_size, pid);
    // } else if (!dl->less_latest_dm || !dl->lldm_size)
    //     pr_info("[Obsidian0215]No less-latest dirtymap for pid %d\n", pid);

    // 确保less_latest_dm和latest_dm为升序再生成dirty_diffmap
    // sort_dirty_map(latest_dm, lsize);
    // sort_dirty_map(less_latest_dm, slsize);
    dl->diffmap = merge_dirty_maps(dl);

    // debug_show_diffmap(dl->diffmap, dl->diffmap_size, pid);

    // 加载上次的阈值并更新
    load_thresholds(dl, dirty_map_dir);

    // 优化1：初始化历史统计和反馈控制
    if (init_historical_stats(dl) != 0) {
        pr_warn("[Obsidian0215] Failed to initialize historical stats for pid %d\n", pid);
    }
    if (init_feedback_controller(dl) != 0) {
        pr_warn("[Obsidian0215] Failed to initialize feedback controller for pid %d\n", pid);
    }

    update_thresholds(dl);

    pr_debug("[Obsidian0215] threshold: %f, %f\n", dl->heat_threshold, dl->trend_threshold);
    return 0;
}

/**
 * @brief 为特定pid进程打开dirty-track设备
 *
 * @param dl 目标dirty_log实例
 * @return int 成功返回0，失败返回-1
 */
int init_dirty_track(struct dirty_log *dl) {
    int fd;

    fd = open(DT_DEV_PATH, O_RDWR);
    if (fd == -1) {
        pr_perror("[Obsidian0215]Error opening dirty-track LKM");
        return -1;
    }
    dl->dirty_track_fd = fd;
    return 0;
}

/**
 * @brief 为特定pid进程启动dirty track
 *
 * @param dl 目标dirty_log实例
 * @return int 成功返回0，失败返回-1/errno
 */
int start_dirty_track(struct dirty_log *dl) {
    int ret = 0, fd = dl->dirty_track_fd;

    if (fd == -1 && (fd = open(DT_DEV_PATH, O_RDWR)) == -1) {
        pr_perror("[Obsidian0215]Error opening dirty-track LKM");
        return -1;
    }

    ret = ioctl(fd, IOCTL_START_PID, &dl->pid);
    // close(fd);
    return ret;
}

/**
 * @brief 为特定pid进程启动dirty track
 *
 * @param dl 目标dirty_log实例
 * @return int 成功返回0，失败返回-1/errno
 */
int check_dirty_track(struct dirty_log *dl, struct pid_check *pc) {
    int ret = 0, fd = dl->dirty_track_fd;

    if (fd == -1 && (fd = open(DT_DEV_PATH, O_RDWR)) == -1) {
        pr_perror("[Obsidian0215]Error opening dirty-track LKM");
        return -1;
    }

    ret = ioctl(fd, IOCTL_CHECK_PID, pc);
    // close(fd);
    return ret;
}

/**
 * @brief 为特定pid进程停止dirty track
 *
 * @param dl 目标dirty_log实例
 * @return int 成功返回0，失败返回-1/errno
 */
int stop_dirty_track(struct dirty_log *dl) {
    int ret = 0, fd = dl->dirty_track_fd;

    if (fd == -1 && ((fd = open(DT_DEV_PATH, O_RDWR)) == -1)) {
        pr_perror("[Obsidian0215]Error opening dirty-track LKM");
        return -1;
    }

    ret = ioctl(fd, IOCTL_STOP_PID, &dl->pid);
    // close(fd);
    return ret;
}

/**
 * @brief 为特定pid进程销毁其dirty_map
 *
 * @param item 指向per-process结构<pid>的指针
 * @return void
 */
void fini_dirty_map(struct pstree_item *item){
    struct dirty_log *dl = item->dl;
    unsigned long ldm_mmap_size, lldm_mmap_size;

    if (dl) {
        // 关闭打开的dirty-track设备fd
        if (dl->dirty_track_fd) {
            close(dl->dirty_track_fd);
            dl->dirty_track_fd = -1;
        }

        // 卸载最新的dirtymap
        if (dl->latest_dm) {
            ldm_mmap_size = dl->ldm_size * sizeof(struct dirty_map) + sizeof(dirtymap_header_t);
            if (munmap((void *)dl->ldm_header, ldm_mmap_size)== -1) {
                pr_perror("[Obsidian0215]Error unmapping latest dirtymap");
            }
            dl->latest_dm = NULL;
            dl->ldm_size = 0;
        }

        // 处理次新的dirtymap
        if (dl->less_latest_dm) {
            lldm_mmap_size = dl->lldm_size * sizeof(struct dirty_map) + sizeof(dirtymap_header_t);
            if (munmap((void *)dl->lldm_header, lldm_mmap_size) == -1) {
                pr_perror("[Obsidian0215]Error unmapping second-latest dirtymap");
            }
            dl->less_latest_dm = NULL;
            dl->lldm_size = 0;
        }

        // 处理diffmap
        if (dl->diffmap) {
            free(dl->diffmap);
            dl->diffmap = NULL;
            dl->diffmap_size = 0;
        }

        // 处理warm_list
        if (dl->warm_list) {
            if (write_warm_list(dl, opts.dirty_map_dir)) {
                pr_perror("[Obsidian0215]Error updating warm_list to file");
            }
            g_tree_destroy(dl->warm_list);
            pthread_mutex_destroy(&dl->warm_list_mutex);
            dl->warm_list = NULL;
            dl->warm_size = 0;
        }

        // 处理thresholds
        if (write_thresholds(dl, opts.dirty_map_dir)) {
            pr_perror("[Obsidian0215] Error updating thresholds to file");
        }
        // 优化1：清理历史统计和反馈控制
        if (dl->historical_stats) {
            free(dl->historical_stats);
            dl->historical_stats = NULL;
        }
        if (dl->feedback_ctrl) {
            free(dl->feedback_ctrl);
            dl->feedback_ctrl = NULL;
        }
        /* 销毁脏页缓存 */
        fini_dirty_cache(dl);

        // 释放dl结构体
        free(dl);
        item->dl = NULL;
    }
}

/**
 * @brief 查找dirty_map中包含指定address的dirty_diffmap结构体
 *
 * @param dl <pid>对应dirtylog指针。其中包含已排序的diffmap
 * @param addr 要查找的线性地址
 * @return struct dirty_diffmap* 返回指向dirty_diffmap结构体的指针，如果未找到则返回NULL
 */
struct dirty_diffmap *search_dirty_map(struct dirty_log *dl, unsigned long addr) {
    struct dirty_diffmap *map;
    unsigned long left = 0, right = 0;

    // diffmap为空，直接返回NULL
    if (!dl)
        return NULL;
    else {
        map = dl->diffmap;
        right = dl->diffmap_size;
        if (!map || !right)
            return NULL;
    }

    while (left < right) {
        unsigned long mid = left + (right - left) / 2;

        if (addr < map[mid].address) {
            // 地址在当前范围左侧，缩小右边界
            right = mid;
        } else if (addr > map[mid].address) {
            // 地址在当前范围右侧，缩小左边界
            left = mid + 1;
        } else {
            // 地址在当前范围内，返回该结构体指针
            return &map[mid];
        }
    }
    // 如果未找到包含地址的范围，返回NULL
    return NULL;
}

/**
 * @brief 查找warm_list中包含指定address
 *
 * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
 * @param addr 要查找的线性地址
 * @return int 找到则返回1，如果未找到则返回0
 */
int search_warm_list(struct dirty_log *dl, unsigned long addr) {
    char *found_s_count = NULL;
    unsigned long key_addr = addr;

    if (!dl)
        return 0;

    pthread_mutex_lock(&dl->warm_list_mutex);
    found_s_count = g_tree_lookup(dl->warm_list, &key_addr);
    pthread_mutex_unlock(&dl->warm_list_mutex);

    return (found_s_count != NULL) ? 1 : 0;
}

/**
 * @brief 将指定address插入warm_list中，并保持warm_list升序
 *
 * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
 * @param addr 要插入的线性地址
 * @return void
 */
void inc_warm_list(struct dirty_log *dl, unsigned long addr) {
    char *found_s_count = NULL;
    unsigned long key_addr = addr;
    char *new_s_count = NULL;
    unsigned long *new_key = NULL;

    if (!dl)
        return;

    pthread_mutex_lock(&dl->warm_list_mutex);

    found_s_count = g_tree_lookup(dl->warm_list, &key_addr);
    if (found_s_count) {
        (*found_s_count)++;
        pr_info("[Obsidian0215] Updated 0x%lx in warm_list: %d\n", addr, *found_s_count);
    } else {
        new_key = malloc(sizeof(unsigned long));
        if (!new_key) {
            perror("[Obsidian0215] malloc failed");
            pthread_mutex_unlock(&dl->warm_list_mutex);
            return;
        }
        *new_key = addr;

        new_s_count = malloc(sizeof(char));
        if (!new_s_count) {
            perror("[Obsidian0215] malloc failed");
            free(new_key);
            pthread_mutex_unlock(&dl->warm_list_mutex);
            return;
        }
        *new_s_count = 1;

        g_tree_insert(dl->warm_list, new_key, new_s_count);
        pr_info("[Obsidian0215] Inserted 0x%lx to warm_list\n", addr);
        dl->warm_size++;
    }

    pthread_mutex_unlock(&dl->warm_list_mutex);
}

/**
 * @brief 将warm_list中指定address的s_count减1，若为0则删除保持warm_list升序
 *
 * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
 * @param addr 要操作的线性地址
 * @return void
 */
void sub_warm_list(struct dirty_log *dl, unsigned long addr, bool zero) {
    char *found_s_count = NULL;
    unsigned long key_addr = addr;

    if (!dl)
        return;

    pthread_mutex_lock(&dl->warm_list_mutex);

    found_s_count = g_tree_lookup(dl->warm_list, &key_addr);
    if (found_s_count) {
        if (*found_s_count > 0) {
            if (zero) {
                *found_s_count = 0;
            } else {
                (*found_s_count)--;
            }

            if (*found_s_count == 0) {
                g_tree_remove(dl->warm_list, &key_addr);
                dl->warm_size--;
            }
        }
    }

    pthread_mutex_unlock(&dl->warm_list_mutex);
}

/* 脏页缓存集成函数 - 每个进程独享 */

/**
 * @brief 为特定pid初始化脏页缓存
 *
 * @param dl 进程的dirty_log结构体指针
 * @param cache_dir 缓存目录路径
 * @return int 成功返回0，失败返回-1
 */
int init_dirty_cache(struct dirty_log *dl, const char *cache_dir)
{
    char cache_path[PATH_MAX];
    int ret;

    if (!dl) {
        pr_err("Non dirty_log, cannot initialize dirty-cache\n");
        return -1;
    }

    /* 检查: 仅在同时开启压缩和dirty-map时启用脏页缓存 */
    if (!DC_IS_CACHE_ENABLED()) {
        pr_debug("Dirty cache disabled for pid %d\n", dl->pid);
        dl->cache_enabled = false;
        return 0;
    }

    /* 分配缓存结构体 */
    dl->page_cache = xzalloc(sizeof(dirty_cache_t));
    if (!dl->page_cache) {
        pr_err("Failed to allocate dirty-cache for pid %d\n", dl->pid);
        return -1;
    }

    /* 构建缓存文件路径 */
    snprintf(cache_path, sizeof(cache_path), "%s/%s.%d",
             cache_dir, DC_TMPFS_PREFIX, dl->pid);
    cache_path[sizeof(cache_path) - 1] = '\0';

    /* 初始化缓存 */
    ret = dc_init_with_size(dl->page_cache, cache_path, DCACHE_SIZE);
    if (ret != 0) {
        pr_err("Failed to initialize dirty cache for pid %d\n", dl->pid);
        xfree(dl->page_cache);
        dl->page_cache = NULL;
        dl->cache_enabled = false;
        return -1;
    }


    dl->cache_enabled = true;
    pr_info("Initialized dirty cache for pid %d: %s\n", dl->pid, cache_path);

    return 0;
}

/**
 * @brief 销毁特定pid的脏页缓存
 *
 * @param dl 进程的dirty_log结构体指针
 */
void fini_dirty_cache(struct dirty_log *dl)
{
    if (!dl || !dl->page_cache)
        return;

    dc_fini(dl->page_cache);
    xfree(dl->page_cache);
    dl->page_cache = NULL;
    dl->cache_enabled = false;

    pr_debug("Finalized dirty cache for pid %d\n", dl->pid);
}

/**
 * @brief 从脏页缓存中查找页面
 *
 * @param dl 进程的dirty_log结构体指针
 * @param vaddr 虚拟地址
 * @param page_out 输出页面数据的缓冲区
 * @return int 成功返回DC_SUCCESS，未找到返回DC_NOT_FOUND，错误返回负值
 */
int dirty_cache_lookup_page(struct dirty_log *dl, unsigned long vaddr, void *page_out)
{
    if (!dl || !dl->page_cache || !dl->cache_enabled)
        return DC_NOT_FOUND;

    return dc_lookup(dl->page_cache, vaddr, page_out);
}

/**
 * @brief 更新脏页缓存中的页面
 *
 * @param dl 进程的dirty_log结构体指针
 * @param vaddr 虚拟地址
 * @param page_data 页面数据
 */
void dirty_cache_update_page(struct dirty_log *dl, unsigned long vaddr, const void *page_data)
{
    if (!dl || !dl->page_cache || !dl->cache_enabled || !page_data)
        return;

    dc_update(dl->page_cache, vaddr, page_data);

    pr_debug("Updated dirty cache entry for pid %d, vaddr=%lx\n", dl->pid, vaddr);
}

/**
 * @brief 检查特定进程的脏页缓存是否启用
 *
 * @param dl 进程的dirty_log结构体指针
 * @return bool 启用返回true，否则返回false
 */
bool is_dirty_cache_enabled(struct dirty_log *dl)
{
    return (dl && dl->cache_enabled && dl->page_cache);
}