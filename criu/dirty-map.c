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
#include "xmalloc.h"
#include "protobuf.h"

#define TIMESTAMP_LIST_PREFIX "timestamp_list"
#define WARM_LIST_PREFIX "warm_list"
#define THRESHOLD_PREFIX "threshold"

#define MAX_FILES 32
#define EXPAND_WARM_BATCH 128

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
    const warm_page_t *wp_a = a;
    const warm_page_t *wp_b = b;
    if (wp_a->address < wp_b->address)
        return -1;
    else if (wp_a->address > wp_b->address)
        return 1;
    else
        return 0;
}

// 插入函数，用于GTree的key和value
gpointer duplicate_warm_page(gpointer key, gpointer value, gpointer user_data) {
    warm_page_t *wp = malloc(sizeof(warm_page_t));
    if (wp) {
        wp->address = ((warm_page_t *)key)->address;
        wp->s_count = ((warm_page_t *)key)->s_count;
    }
    return wp;
}

// 回调函数，用于遍历 GTree 并写入文件
static gboolean write_warm_page(gpointer key, gpointer value, gpointer user_data) {
    FILE *f = (FILE *)user_data;
    warm_page_t *wp = (warm_page_t *)key;

    if (fwrite(wp, sizeof(warm_page_t), 1, f) != 1) {
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
        warm_page_t *existing = g_tree_lookup(dl->warm_list, &wp);
        if (existing) {
            existing->s_count += wp.s_count;
        } else {
            warm_page_t *new_wp = malloc(sizeof(warm_page_t));
            if (!new_wp) {
                perror("[Obsidian0215] malloc");
                fclose(file);
                pthread_mutex_unlock(&dl->warm_list_mutex);
                return -1;
            }
            memcpy(new_wp, &wp, sizeof(warm_page_t));
            g_tree_insert(dl->warm_list, new_wp, NULL);
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
static void debug_show_diffmap(struct dirty_diffmap *diffmap, unsigned long diffmap_size, pid_t pid) {
    int i;

    if (pr_quelled(LOG_DEBUG) || !diffmap || !diffmap_size)
		return;

    pr_debug("Diffmap for pid %d:(size: %lu)\n", pid, diffmap_size);
	for (i = 0; i < diffmap_size; i++) {
		pr_debug("\taddress: %#lx, heat: %f, heat trend: %f\n",
            diffmap[i].address, diffmap[i].heat, diffmap[i].heat_trend);
	}
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

// 定义遍历数据结构
typedef struct {
    struct dirty_log *dl;
    unsigned int hit_warm;
    unsigned int miss_warm;
} traversal_data_t;

// 遍历回调函数，用于统计hit_warm和miss_warm
static gboolean count_warm_pages(gpointer key, gpointer value, gpointer user_data) {
    traversal_data_t *data = (traversal_data_t *)user_data;
    warm_page_t *wp = (warm_page_t *)key;

    if (!search_dirty_map(data->dl, wp->address)) {
        data->hit_warm++;
    } else {
        data->miss_warm++;
    }

    return TRUE; // 继续遍历
}

/**
 * @brief 更新dirty_log中判断温页的阈值
 *
 * @param dl 进程的dirty-log结构体指针
 */
static void update_thresholds(struct dirty_log *dl) {
    struct dirty_diffmap *dirtymap = dl->diffmap;
    unsigned int hit_warm = 0, miss_warm = 0, new_warm = 0;
    float min_heat_threshold = 0.0, min_in_dirtymap = 0.0, new_heat_threshold = 0.0;
    int i;
    traversal_data_t data;

    // 检查warm_list和dirtymap是否存在
    if (!dl->warm_list || !dirtymap) {
        pr_info("[Obsidian0215] warm threshold for %d won't update\n", dl->pid);
        return;
    }

    // 初始化min_in_dirtymap
    if (dl->diffmap_size > 0) {
        min_in_dirtymap = dirtymap[0].heat;
    } else {
        // 如果dirtymap为空，无法更新阈值
        pr_info("[Obsidian0215] dirtymap is empty for %d, thresholds not updated\n", dl->pid);
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

    // 当命中率不高于50%时(不包括空warm_list)，更新thresholds
    if (hit_warm <= miss_warm && miss_warm > 0) {
        // 计算min_heat_threshold
        if (dl->ldm_header && dl->ldm_header->track_duration_ns > 0) {
            min_heat_threshold = 1.0f / ((float)dl->ldm_header->track_duration_ns / 1e9f);
        } else {
            min_heat_threshold = 0.0f; // 默认值或其他处理
        }

        // 更新heat_threshold
        new_heat_threshold = (float)hit_warm / (hit_warm + miss_warm);
        dl->heat_threshold = fmaxf(min_heat_threshold, dl->heat_threshold * new_heat_threshold);

        // 更新trend_threshold
        dl->trend_threshold = 0.35f + 0.65f * dl->trend_threshold;
    }

    // 用更新的thresholds选择dirtymap的温页并更新new_warm
    for (i = 0 ; i < dl->diffmap_size; i++) {
        if (dirtymap[i].heat <= dl->heat_threshold
         && -dirtymap[i].heat_trend > dl->trend_threshold * dirtymap[i].heat
         && !search_warm_list(dl, dirtymap[i].address)) {
            new_warm++;
        }
        if (dirtymap[i].heat < min_in_dirtymap) {
            min_in_dirtymap = dirtymap[i].heat;
        }
    }

    // 新温页过少(<32页或5%)时，适当放宽选择阈值
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

// /**
//  * @brief 更新dirty_log中判断温页的阈值
//  *
//  * @param dl 进程的dirty-log结构体指针
//  */
// static void update_thresholds(struct dirty_log *dl) {
//     struct dirty_diffmap *dirtymap = dl->diffmap;
//     warm_page_t *wl = dl->warm_list;
//     unsigned int hit_warm = 0, miss_warm = 0, new_warm = 0;
//     float min_heat_threshold = 0.0, min_in_dirtymap = 0.0;
//     int i;

//     if (!wl || !dirtymap) {
//         pr_info("[Obsidian0215] warm threshold for %d won't update\n", dl->pid);
//         return;
//     }
//     min_in_dirtymap = dirtymap[0].heat;

//     // 遍历warm_list，更新hit_warm和miss_warm
//     for (i = 0 ; i < dl->warm_size; i++) {
//         if (!search_dirty_map(dl, wl[i].address)) {
//             hit_warm++;
//         } else {
//             miss_warm++;
//         }
//     }

//     // 当命中率不高于50%时(不包括空warm_list)，更新thresholds
//     if (hit_warm <= miss_warm && miss_warm > 0) {
//         min_heat_threshold = 1.0 / (float)(dl->ldm_header->track_duration_ns / 1e9);
//         dl->heat_threshold = max(min_heat_threshold, dl->heat_threshold * (float)hit_warm / (float)(hit_warm + miss_warm));
//         dl->trend_threshold = 0.55 + 0.45 * dl->trend_threshold;
//     }

//     // 用更新的thresholds选择dirtymap的温页并更新new_warm
//     for (i = 0 ; i < dl->diffmap_size; i++) {
//         if (dirtymap[i].heat <= dl->heat_threshold
//          && -dirtymap[i].heat_trend > dl->trend_threshold * dirtymap[i].heat
//          && !search_warm_list(dl, dirtymap[i].address)) {
//             new_warm++;
//         }
//         if (dirtymap[i].heat < min_in_dirtymap) {
//             min_in_dirtymap = dirtymap[i].heat;
//         }
//     }

//     // 新温页过少(<32页或5%)时，适当放宽选择阈值
//     if (new_warm < 32 || new_warm < 0.054 * miss_warm) {
//         if (min_in_dirtymap > min_heat_threshold) {
//             dl->heat_threshold = min_in_dirtymap;
//         } else {
//             dl->trend_threshold = 0.9025 * dl->trend_threshold;
//         }
//     } else if (new_warm >= 32 && new_warm > 0.25 * miss_warm) {
//         dl->trend_threshold = 1.06 * dl->trend_threshold;
//         dl->heat_threshold = max(min_in_dirtymap, (float)0.925 * dl->heat_threshold);
//     }
// }

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
    dl->warm_list = g_tree_new_full(compare_warm_page, NULL, free, NULL);
    // 读取warm_list.<pid>文件，初始化warm_list
    ret = load_warm_list(dirty_map_dir, pid, dl);
    if (ret < 0) {
        pr_perror("[Obsidian0215]Failed to load warm_list for pid %d", pid);
        return -1;
    }

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
    //     // pr_info("[Obsidian0215]latest dirtymap for pid %d:\n", pid);
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
    //     // pr_info("[Obsidian0215]less-latest dirtymap for pid %d:\n", pid);
    //     debug_show_dirtymap(dl->less_latest_dm, dl->lldm_size, pid);
    // } else if (!dl->less_latest_dm || !dl->lldm_size)
    //     pr_info("[Obsidian0215]No less-latest dirtymap for pid %d\n", pid);

    // 生成dirty_diffmap先保证两个dirtymap都按升序排列
    // 使用升序的less_latest_dm和latest_dm生成dirty_diffmap
    // sort_dirty_map(latest_dm, lsize);
    // sort_dirty_map(less_latest_dm, slsize);
    dl->diffmap = merge_dirty_maps(dl);

    debug_show_diffmap(dl->diffmap, dl->diffmap_size, pid);

    // 加载上次的阈值并更新
    load_thresholds(dl, dirty_map_dir);
    update_thresholds(dl);

    pr_debug("[Obsidian] threshold: %f, %f\n", dl->heat_threshold, dl->trend_threshold);
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
            pr_perror("[Obsidian0215]Error updating thresholds to file");
        }
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
    warm_page_t key = { .address = addr, .s_count = 0 };
    warm_page_t *found = NULL;

    if (!dl)
        return 0;

    pthread_mutex_lock(&dl->warm_list_mutex);

    found = g_tree_lookup(dl->warm_list, &key);

    pthread_mutex_unlock(&dl->warm_list_mutex);

    return (found != NULL) ? 1 : 0;
}

/**
 * @brief 将指定address插入warm_list中，并保持warm_list升序
 *
 * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
 * @param addr 要插入的线性地址
 * @return void
 */
void inc_warm_list(struct dirty_log *dl, unsigned long addr) {
    warm_page_t key = { .address = addr, .s_count = 0 };
    warm_page_t *found = NULL;
    warm_page_t *new_wp = NULL;

    if (!dl)
        return;

    pthread_mutex_lock(&dl->warm_list_mutex);

    found = g_tree_lookup(dl->warm_list, &key);
    if (found) {
        found->s_count++;
        // pr_info("[Obsidian0215] Updated 0x%lx in warm_list: %d\n", addr, found->s_count);
    } else {
        new_wp = malloc(sizeof(warm_page_t));
        if (!new_wp) {
            perror("[Obsidian0215] malloc failed");
            pthread_mutex_unlock(&dl->warm_list_mutex);
            return;
        }
        new_wp->address = addr;
        new_wp->s_count = 1;

        g_tree_insert(dl->warm_list, new_wp, NULL);
        // pr_info("[Obsidian0215] Inserted 0x%lx to warm_list\n", addr);
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
    warm_page_t key = { .address = addr, .s_count = 0 };
    warm_page_t *found = NULL;

    if (!dl)
        return;

    pthread_mutex_lock(&dl->warm_list_mutex);

    found = g_tree_lookup(dl->warm_list, &key);
    if (found) {
        if (found->s_count > 0) {
            if (zero) {
                found->s_count = 0;
            } else {
                found->s_count--;
            }

            if (found->s_count == 0) {
                g_tree_remove(dl->warm_list, &key);
                dl->warm_size--;
            }
        }
    }

    pthread_mutex_unlock(&dl->warm_list_mutex);
}

// /**
//  * @brief 查找warm_list中包含指定address
//  *
//  * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
//  * @param addr 要查找的线性地址
//  * @return int 找到则返回1，如果未找到则返回0
//  */
// int search_warm_list(struct dirty_log *dl, unsigned long addr) {
//     unsigned long left = 0, right = 0;
//     warm_page_t *cd_list;

//     // warm_list为空，无法找到该地址
//     if (!dl)
//         return 0;
//     else {
//         cd_list = dl->warm_list;
//         right = dl->warm_size;
//         if (!cd_list || !right)
//             return 0;
//     }

//     while (left < right) {
//         unsigned long mid = left + (right - left) / 2;

//         if (addr < cd_list[mid].address) {
//             // 地址在当前范围左侧，缩小右边界
//             right = mid;
//         } else if (addr > cd_list[mid].address) {
//             // 地址在当前范围右侧，缩小左边界
//             left = mid + 1;
//         } else {
//             // 找到该地址
//             return 1;
//         }
//     }
//     // 未找到该地址
//     return 0;
// }

// /**
//  * @brief 将指定address插入warm_list中，并保持warm_list升序
//  *
//  * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
//  * @param addr 要插入的线性地址
//  * @return void
//  */
// void inc_warm_list(struct dirty_log *dl, unsigned long addr) {
//     char warm_list_filepath[PATH_MAX];
//     unsigned long mid, left = 0, right, new_max, new_size;
//     void *new_map;
//     int fd;

//     if (!dl || !dl->warm_list)
//         return;

//     right = dl->warm_size;
//     // 二分查找插入位置
//     while (left < right) {
//         mid = left + (right - left) / 2;
//         if (dl->warm_list[mid].address < addr)
//             left = mid + 1;
//         else
//             right = mid;
//     }

//     // 检查地址是否已存在, 如果存在则直接增加记录，无需插入
//     if (left < dl->warm_size && dl->warm_list[left].address == addr) {
//         dl->warm_list[left].s_count++;
//         return;
//     }

//     // 检查是否需要扩展
//     if (dl->warm_size >= dl->warm_max) {
//         pr_info("[Obsidian0215] need expand warm_list");
//         new_max = dl->warm_max + EXPAND_WARM_BATCH;
//         if (new_max > dl->diffmap_size) {
//             new_max = dl->diffmap_size;
//         }
//         new_size = new_max * sizeof(warm_page_t);

//         snprintf(warm_list_filepath, sizeof(warm_list_filepath),
//              "%s/%s.%d", opts.dirty_map_dir, WARM_LIST_PREFIX, dl->pid);
//         warm_list_filepath[sizeof(warm_list_filepath) - 1] = '\0';
//          // 扩展文件大小以匹配新的映射范围
//         fd = open(warm_list_filepath, O_RDWR);
//         if (fd == -1) {
//             perror("[Obsidian0215]Error opening warm list file");
//             return;
//         }
//         if (ftruncate(fd, new_size) == -1) {
//             perror("[Obsidian0215]ftruncate");
//             close(fd);
//             return;
//         }
//         close(fd);

//         // 重新映射文件
//         new_map = mremap(dl->warm_list, dl->warm_max * sizeof(warm_page_t), new_size, MREMAP_MAYMOVE);
//         if (new_map == MAP_FAILED) {
//             perror("[Obsidian0215]mremap");
//             return;
//         }

//         dl->warm_list = (warm_page_t *)new_map;
//         dl->warm_max = new_max;
//     }

//     // 如果插入位置是末尾，直接赋值无需移动
//     if (left == dl->warm_size) {
//         dl->warm_list[left].address = addr;
//         dl->warm_list[left].s_count = 1;
//         dl->warm_size++;
//     } else {
//         // 移动元素以腾出插入位置
//         memmove(&dl->warm_list[left + 1], &dl->warm_list[left],
//                 (dl->warm_size - left) * sizeof(warm_page_t));
//         dl->warm_list[left].address = addr;
//         dl->warm_list[left].s_count = 1;
//         dl->warm_size++;
//     }
// }

// /**
//  * @brief 将指定address从warm_list中删除，并保持warm_list升序
//  *
//  * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
//  * @param addr 要删除的线性地址
//  * @return void
//  */
// static void del_in_warm_list(struct dirty_log *dl, unsigned long index) {

//     if (!dl || !dl->warm_list || index >= dl->warm_size)
//         return;

//     // 删除的是最后一个元素，直接减少大小
//     if (index == dl->warm_size - 1) {
//         dl->warm_size--;
//     } else {
//         // 移动元素以覆盖删除的位置
//         memmove(&dl->warm_list[index], &dl->warm_list[index + 1],
//                 (dl->warm_size - index - 1) * sizeof(warm_page_t));
//         dl->warm_size--;
//     }
// }

// /**
//  * @brief 将warm_list中指定address的s_count减1，若为0则删除保持warm_list升序
//  *
//  * @param dl <pid>对应dirtylog指针。其中包含已排序的warm_list
//  * @param addr 要操作的线性地址
//  * @return void
//  */
// void sub_warm_list(struct dirty_log *dl, unsigned long addr, bool zero) {
//     unsigned long mid, left = 0, right;

//     if (!dl || !dl->warm_list)
//         return;

//     right = dl->warm_size;

//     // 二分查找目标地址
//     while (left < right) {
//         mid = left + (right - left) / 2;
//         if (dl->warm_list[mid].address < addr)
//             left = mid + 1;
//         else
//             right = mid;
//     }

//     // 检查是否找到地址
//     if (left >= dl->warm_size || dl->warm_list[left].address != addr)
//         return;

//     if (dl->warm_list[left].s_count > 0) {
//         if (zero) {
//             dl->warm_list[left].s_count = 0;
//         } else {
//             dl->warm_list[left].s_count--;
//         }
//         if (dl->warm_list[left].s_count == 0) {
//             del_in_warm_list(dl, addr);
//         }
//     } else {
//         // s_count为0，删除该元素
//         del_in_warm_list(dl, addr);
//     }
// }