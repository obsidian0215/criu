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
#include <ctype.h>
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
#define DEFERRED_LIST_PREFIX "deferred_list"
#define PREDICTION_METRICS_PREFIX "prediction_metrics"
#define THRESHOLD_PREFIX "threshold"

#define MAX_FILES 32

static void rebuild_deferred_queues(struct dirty_log *dl);
static void update_prev_dirty_set(struct dirty_log *dl);
static void decision_model_init(void);
static int decision_model_online_active(void);
static void decision_model_online_snapshot(float *bias, float *scale, unsigned long *updates);
static void decision_model_online_update(float score, float p, float y, page_class_t page_class);

static inline int decision_telemetry_enabled(void) {
    static int enabled = -1;
    const char *env;
    if (enabled != -1)
        return enabled;
    env = getenv("CRIU_DECISION_TELEMETRY");
    /* default: disabled unless explicitly enabled */
    if (!env || !env[0])
        enabled = 0;
    else
        enabled = (env[0] == '0') ? 0 : 1;
    return enabled;
}

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


// 回调函数，用于遍历 deferred_list 并写入文件
static void write_deferred_page(gpointer key, gpointer value, gpointer user_data) {
    FILE *f = (FILE *)user_data;
    deferred_page_t dp;

    dp.address = GPOINTER_TO_SIZE(key);
    dp.count = (unsigned char)GPOINTER_TO_INT(value);

    if (fwrite(&dp, sizeof(dp), 1, f) != 1) {
        perror("[Obsidian0215] fwrite deferred");
    }
}

/**
 * @brief 从deferred_list.pid文件中读取deferred_list到GHashTable
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param pid 进程pid
 * @param dl 指向存储deferred_list的dirty_log结构体
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int load_deferred_list(const char *dirty_map_dir, pid_t pid, struct dirty_log *dl) {
    char deferred_list_filepath[PATH_MAX];
    FILE *file = NULL;
    unsigned long addr;
    struct stat st;
    bool use_new_format = false;
    int ret;

    if (!dl) {
        fprintf(stderr, "[Obsidian0215] Invalid dl pointer\n");
        errno = EINVAL;
        return -1;
    }

    ret = snprintf(deferred_list_filepath, sizeof(deferred_list_filepath), "%s/%s.%d",
                   dirty_map_dir, DEFERRED_LIST_PREFIX, pid);
    if (ret < 0 || ret >= sizeof(deferred_list_filepath)) {
        fprintf(stderr, "[Obsidian0215] Error constructing deferred list file path\n");
        errno = EINVAL;
        return -1;
    }

    file = fopen(deferred_list_filepath, "rb");
    if (!file) {
        if (errno == ENOENT) {
            return 0;
        } else {
            perror("[Obsidian0215] fopen deferred");
            return -1;
        }
    }

    if (fstat(fileno(file), &st) == 0) {
        if (st.st_size == 0) {
            fclose(file);
            return 0;
        }
        if (st.st_size % sizeof(deferred_page_t) == 0)
            use_new_format = true;
        else if (st.st_size % sizeof(unsigned long) == 0)
            use_new_format = false;
        else {
            pr_perror("[Obsidian0215]Invalid deferred_list file size");
            fclose(file);
            return -1;
        }
    }

    pthread_mutex_lock(&dl->deferred_list_mutex);

    if (use_new_format) {
        deferred_page_t dp;
        while (fread(&dp, sizeof(dp), 1, file) == 1) {
            gpointer key = GSIZE_TO_POINTER(dp.address);
            if (!g_hash_table_contains(dl->deferred_list, key)) {
                int count = dp.count ? dp.count : 1;
                g_hash_table_insert(dl->deferred_list, key, GINT_TO_POINTER(count));
                dl->deferred_size++;
            }
        }
    } else {
        while (fread(&addr, sizeof(addr), 1, file) == 1) {
            gpointer key = GSIZE_TO_POINTER(addr);
            if (!g_hash_table_contains(dl->deferred_list, key)) {
                g_hash_table_insert(dl->deferred_list, key, GINT_TO_POINTER(1));
                dl->deferred_size++;
            }
        }
    }

    if (ferror(file)) {
        perror("[Obsidian0215] fread deferred");
        fclose(file);
        pthread_mutex_unlock(&dl->deferred_list_mutex);
        return -1;
    }

    fclose(file);
    pthread_mutex_unlock(&dl->deferred_list_mutex);
    return 0;
}

/**
 * @brief 将deferred_list保存到deferred_list.pid文件中
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param dl 指向存储deferred_list的dirty_log结构体
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int write_deferred_list(struct dirty_log *dl, const char *dirty_map_dir) {
    char deferred_list_filepath[PATH_MAX];
    pid_t pid;
    FILE *file = NULL;
    int ret = 0;

    if (!dl) {
        fprintf(stderr, "[Obsidian0215] Invalid dl pointer\n");
        errno = EINVAL;
        return -1;
    }

    pid = dl->pid;
    ret = snprintf(deferred_list_filepath, sizeof(deferred_list_filepath), "%s/%s.%d",
                   dirty_map_dir, DEFERRED_LIST_PREFIX, pid);
    if (ret < 0 || ret >= sizeof(deferred_list_filepath)) {
        fprintf(stderr, "[Obsidian0215] Error constructing deferred list file path\n");
        errno = EINVAL;
        return -1;
    }

    file = fopen(deferred_list_filepath, "wb");
    if (!file) {
        perror("[Obsidian0215] fopen deferred");
        return -1;
    }

    pthread_mutex_lock(&dl->deferred_list_mutex);
    g_hash_table_foreach(dl->deferred_list, write_deferred_page, file);
    if (ferror(file)) {
        perror("[Obsidian0215] fwrite deferred");
        fclose(file);
        pthread_mutex_unlock(&dl->deferred_list_mutex);
        return -1;
    }

    fclose(file);
    pthread_mutex_unlock(&dl->deferred_list_mutex);
    return 0;
}
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
                /* [Obsidian0215] Snapshots are disjoint intervals, latest_dm already represents the delta. */
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

/*
 * Evaluate prediction accuracy for deferred pages from previous round.
 * NOTE: semantic correction — a "hit" here means the deferred page cooled
 * (i.e., it is NOT dirty in the current diffmap). This aligns the counters
 * with the scripts and the adaptive controller (higher hit% == better defers).
 */
static void update_prediction_from_deferred(struct dirty_log *dl) {
    GHashTableIter iter;
    gpointer key, value;
    int do_online = 0;
    int round = 0;
    page_class_t page_class = PAGE_COLD;
    float online_bias_before = 0.0f;
    float online_scale_before = 0.0f;
    float online_bias_after = 0.0f;
    float online_scale_after = 0.0f;
    unsigned long updates_before = 0;
    unsigned long updates_after = 0;

    if (!dl || !dl->deferred_list)
        return;

    do_online = decision_model_online_active();
    if (dl->current_round > 0)
        round = dl->current_round;
    if (round < 0)
        round = 0;
    if (do_online)
        decision_model_online_snapshot(&online_bias_before, &online_scale_before, &updates_before);

    dl->predicted_total = 0;
    dl->predicted_hit = 0;  /* success: deferred -> cooled */
    dl->predicted_miss = 0; /* miss   : deferred -> still dirty */
    dl->predicted_total_nondefer = 0;
    dl->predicted_hit_nondefer = 0;
    dl->predicted_miss_nondefer = 0;
    dl->predicted_total_all = 0;
    dl->predicted_hit_all = 0;
    dl->predicted_miss_all = 0;

    pthread_mutex_lock(&dl->deferred_list_mutex);
    g_hash_table_iter_init(&iter, dl->deferred_list);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        unsigned long addr = GPOINTER_TO_SIZE(key);
        struct dirty_diffmap *dhm = search_dirty_map(dl, addr);
        int deferred_count = value ? GPOINTER_TO_INT(value) : 0;
        float score = 0.0f;
        float p_model = -1.0f;
        float p_next = 0.0f;
        float p_use = 0.0f;
        float y = 0.0f;
        float cool_thresh = dl->min_heat;
        dl->predicted_total++;
        /* Consider page 'cooled' (success) when no DHM entry or heat <= cool_thresh.
         * Be slightly lenient in early rounds to avoid penalizing first-round-only
         * decisions that rely on a single dirtymap snapshot.
         */
        if (dl->threshold_low > 0.0f)
            cool_thresh = fmaxf(cool_thresh, dl->threshold_low * 0.5f);
        if (round <= 1)
            cool_thresh *= 1.5f;
        if (!dhm || dhm->heat <= cool_thresh || (dhm->heat_trend < 0.0f && dhm->heat <= dl->threshold_mid))
            dl->predicted_hit++;
        else
            dl->predicted_miss++;

        if (do_online) {
            /* Online calibration target: y=1 if cooled, y=0 if still dirty */
            y = (!dhm || dhm->heat <= dl->min_heat) ? 1.0f : 0.0f;
            p_next = compute_p_next_dirty(dl, dhm, addr, false, round, false, deferred_count, false, &score, &p_model);
            p_use = (p_model >= 0.0f) ? p_model : p_next;
            page_class = classify_page(dl, dhm, round);
            decision_model_online_update(score, p_use, y, page_class);
        }
    }
    pthread_mutex_unlock(&dl->deferred_list_mutex);

    /* Evaluate non-deferred pages that were dirty in previous round (prev_dirty_set)
     * If they are still dirty now, count as miss; otherwise count as hit.
     * This extends accuracy beyond deferred-only evaluation.
     */
    if (dl->prev_dirty_set && g_hash_table_size(dl->prev_dirty_set) > 0) {
        GHashTableIter pit;
        gpointer pkey, pval;
        pthread_mutex_lock(&dl->deferred_list_mutex);
        g_hash_table_iter_init(&pit, dl->prev_dirty_set);
        while (g_hash_table_iter_next(&pit, &pkey, &pval)) {
            unsigned long addr = GPOINTER_TO_SIZE(pkey);
            struct dirty_diffmap *dhm_prev = NULL;
            int was_deferred = g_hash_table_lookup(dl->deferred_list, pkey) ? 1 : 0;
            if (was_deferred)
                continue; /* deferred pages evaluated above */
            dl->predicted_total_nondefer++;
            dhm_prev = search_dirty_map(dl, addr);
            if (!dhm_prev || dhm_prev->heat <= dl->min_heat)
                dl->predicted_hit_nondefer++;
            else
                dl->predicted_miss_nondefer++;
        }
        pthread_mutex_unlock(&dl->deferred_list_mutex);
    }

    dl->predicted_total_all = dl->predicted_total + dl->predicted_total_nondefer;
    dl->predicted_hit_all = dl->predicted_hit + dl->predicted_hit_nondefer;
    dl->predicted_miss_all = dl->predicted_miss + dl->predicted_miss_nondefer;

    if (do_online) {
        decision_model_online_snapshot(&online_bias_after, &online_scale_after, &updates_after);
        if (updates_after > updates_before && decision_telemetry_enabled()) {
            pr_info("[ObsidianOnline] updates=%lu bias=%.4f (d=%.4f) scale=%.4f (d=%.4f)\n",
                updates_after,
                online_bias_after, online_bias_after - online_bias_before,
                online_scale_after, online_scale_after - online_scale_before);
        }
    }
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
            } else {
                float dur = (float)dl->ldm_header->track_duration_ns / 1e9;
                if (dur > 0) {
                    dl->heat_threshold = 2.0f / dur;
                    if (dl->heat_threshold > 20.0f) dl->heat_threshold = 20.0f;
                    if (dl->heat_threshold < 1.0f) dl->heat_threshold = 1.0f;
                } else {
                    dl->heat_threshold = INITIAL_HEAT_THRESHOLD;
                }
            }
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

static void update_thresholds(struct dirty_log *dl) {
    const float P_ALPHA = 0.05f; /* step for p_base adaptation */
    const float S_ALPHA = 0.05f; /* step for soft_ratio adaptation */
    const float P_MIN = 0.05f, P_MAX = 0.95f;
    const float S_MIN = 0.01f, S_MAX = 0.5f;
    const float P_TARGET = 0.80f; /* aim for 80% deferred success rate (cooled fraction) */
    float pred_rate = P_TARGET; /* default: no-op when no predictions available */
    float old_p;
    float old_s;
    float new_p;
    float round_boost = 1.0f;
    float hot_boost = 1.0f;
    int round = 1;

    if (!dl)
        return;

    old_p = dl->adaptive_p_base;
    old_s = dl->adaptive_soft_ratio;
    if (dl->predicted_total > 0)
        pred_rate = (float)dl->predicted_hit / (float)dl->predicted_total;

    /* Update adaptive base probability (more aggressive defer when prediction is good) */
    new_p = old_p + P_ALPHA * (pred_rate - P_TARGET);
    if (new_p < P_MIN) new_p = P_MIN;
    if (new_p > P_MAX) new_p = P_MAX;

    round = (dl->current_round > 0) ? dl->current_round : 1;
    if (round <= 1)
        round_boost = 0.90f;
    else if (round == 2)
        round_boost = 0.95f;
    else if (round == 3)
        round_boost = 0.98f;
    else
        round_boost = 1.02f;

    hot_boost = dirtymap_pid_hotness_factor(dl);
    new_p = new_p * round_boost * hot_boost;
    if (new_p < P_MIN) new_p = P_MIN;
    if (new_p > P_MAX) new_p = P_MAX;
    dl->adaptive_p_base = new_p;

    if (dl->diffmap_size > 0 && dl->global_mean_heat > 0.0f) {
        float observed_ratio = dl->deferred_heat_sum / (dl->global_mean_heat * (float)dl->diffmap_size + 1e-6f);
        float new_s = old_s + S_ALPHA * (observed_ratio - old_s);
        if (new_s < S_MIN) new_s = S_MIN;
        if (new_s > S_MAX) new_s = S_MAX;

        if (round <= 1)
            round_boost = 1.20f;
        else if (round == 2)
            round_boost = 1.10f;
        else if (round == 3)
            round_boost = 1.05f;
        else
            round_boost = 1.0f;
        hot_boost = 1.0f + (1.0f - dirtymap_pid_hotness_factor(dl)) * 0.50f;
        new_s = new_s * round_boost * hot_boost;
        if (new_s < S_MIN) new_s = S_MIN;
        if (new_s > S_MAX) new_s = S_MAX;
        dl->adaptive_soft_ratio = new_s;

        if (fabsf(new_p - old_p) > 0.0005f || fabsf(new_s - old_s) > 0.0005f) {
            pr_info("[ObsidianAdaptive] pid=%d adapt_p_base: %.3f->%.3f pred_rate=%.3f, adapt_soft_ratio: %.3f->%.3f observed_ratio=%.3f\n",
                    dl->pid, old_p, new_p, pred_rate, old_s, new_s, observed_ratio);
        }
    } else {
        if (fabsf(new_p - old_p) > 0.0005f) {
            pr_info("[ObsidianAdaptive] pid=%d adapt_p_base: %.3f->%.3f pred_rate=%.3f\n",
                    dl->pid, old_p, new_p, pred_rate);
        }
    }

    /* Per-class adaptive max-defer rounds (PID/round aware). */
    {
        int bonus = 0;
        int round_bonus = (round <= 2) ? 1 : 0;
        int hot_bonus = (dirtymap_pid_hotness_factor(dl) < 0.90f) ? 1 : 0;
        int hot_max, warm_max, cold_max;

        if (pred_rate > 0.85f)
            bonus = 1;
        else if (pred_rate < 0.50f)
            bonus = -1;

        hot_max = 6 + bonus + round_bonus + hot_bonus;
        warm_max = 3 + bonus + round_bonus + hot_bonus;
        cold_max = 1 + ((pred_rate > 0.90f) ? 1 : 0);

        if (hot_max < 4) hot_max = 4;
        if (hot_max > 12) hot_max = 12;
        if (warm_max < 1) warm_max = 1;
        if (warm_max > 6) warm_max = 6;
        if (cold_max < 0) cold_max = 0;
        if (cold_max > 2) cold_max = 2;

        dl->adaptive_max_defer[PAGE_FREEZING] = 0;
        dl->adaptive_max_defer[PAGE_COLD] = cold_max;
        dl->adaptive_max_defer[PAGE_WARM] = warm_max;
        dl->adaptive_max_defer[PAGE_HOT] = hot_max;
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
    bool new_dl = false;
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

    // [Obsidian0215] init or reuse dirty-log for pid
    dl = item->dl;
    if (!dl) {
        dl = (struct dirty_log *)xzalloc(sizeof(struct dirty_log));
        if (!dl) {
            pr_perror("[Obsidian0215]Failed to allocate memory for dirty-log");
            return -1;
        }
        INIT_DIRTY_LOG_PTR(dl);
        dl->pid = pid;
        item->dl = dl;
        new_dl = true;
    } else {
        /* Reuse existing dirty-log to preserve deferred_list across rounds.
         * Reset per-iteration dirtymap fields and stats; keep deferred structures.
         */
        if (dl->latest_dm) {
            unsigned long ldm_mmap_size = dl->ldm_size * sizeof(struct dirty_map) + sizeof(dirtymap_header_t);
            if (munmap((void *)dl->ldm_header, ldm_mmap_size) == -1)
                pr_perror("[Obsidian0215]Error unmapping latest dirtymap");
            dl->latest_dm = NULL;
            dl->ldm_size = 0;
            dl->ldm_header = NULL;
        }
        if (dl->less_latest_dm) {
            unsigned long lldm_mmap_size = dl->lldm_size * sizeof(struct dirty_map) + sizeof(dirtymap_header_t);
            if (munmap((void *)dl->lldm_header, lldm_mmap_size) == -1)
                pr_perror("[Obsidian0215]Error unmapping second-latest dirtymap");
            dl->less_latest_dm = NULL;
            dl->lldm_size = 0;
            dl->lldm_header = NULL;
        }
        if (dl->diffmap) {
            free(dl->diffmap);
            dl->diffmap = NULL;
            dl->diffmap_size = 0;
        }
        dl->latest_timestamp = 0;
        dl->less_latest_timestamp = 0;
        dl->stats_collected = false;
    }


    // 打开 DT_DEV_PATH 并验证 dirty_map_dir
    ret = init_dirty_track(dl);
    if (ret) {
        pr_perror("[Obsidian0215]Failed to open dirty_track device %s", DT_DEV_PATH);
        return -1;
    }

    /* Initialize per-class deferred FIFO queues */
    {
        int qi;
        if (new_dl) {
            for (qi = 0; qi < NUM_PAGE_CLASSES; qi++)
                dl->deferred_queues[qi] = g_queue_new();
        } else {
            for (qi = 0; qi < NUM_PAGE_CLASSES; qi++) {
                if (!dl->deferred_queues[qi])
                    dl->deferred_queues[qi] = g_queue_new();
            }
        }
    }

    /* warm candidate cache deprecated (not used) */

    if (new_dl) {
        pthread_mutex_init(&dl->deferred_list_mutex, NULL);
        dl->deferred_list = g_hash_table_new(g_direct_hash, g_direct_equal);
        dl->deferred_evict_ttl = g_hash_table_new(g_direct_hash, g_direct_equal);
        ret = load_deferred_list(dirty_map_dir, pid, dl);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Failed to load deferred_list for pid %d", pid);
            return -1;
        }
    } else {
        if (!dl->deferred_evict_ttl)
            dl->deferred_evict_ttl = g_hash_table_new(g_direct_hash, g_direct_equal);
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
        // pr_info("[Phase0Timing] LKM stopped tracking PID %d, dirty-map file should be generated\n", pid);
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
    /* Populate global heat stats for enforcement/threshold logic */
    collect_global_heat_stats(dl);
    /* Rebuild deferred queues using current diffmap (preserves deferred_list across rounds) */
    rebuild_deferred_queues(dl);

    /* Compute prediction accuracy based on deferred list from previous round */
    update_prediction_from_deferred(dl);

    /* Snapshot current dirty set for next-round accuracy evaluation */
    update_prev_dirty_set(dl);

    // debug_show_diffmap(dl->diffmap, dl->diffmap_size, pid);

    // 加载上次的阈值并更新
    load_thresholds(dl, dirty_map_dir);

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

    // pr_info("[Phase0Timing] start_dirty_track for PID %d\n", dl->pid);

    if (fd == -1 && (fd = open(DT_DEV_PATH, O_RDWR)) == -1) {
        pr_perror("[Obsidian0215]Error opening dirty-track LKM");
        return -1;
    }

    ret = ioctl(fd, IOCTL_START_PID, &dl->pid);

    if (!ret) {
        // pr_info("[Phase0Timing] LKM started tracking PID %d\n", dl->pid);
    } else {
        pr_err("[Phase0Timing] LKM failed to start tracking PID %d\n", dl->pid);
    }

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

        /* Destroy per-class deferred queues */
        {
            int qi;
            for (qi = 0; qi < NUM_PAGE_CLASSES; qi++) {
                if (dl->deferred_queues[qi]) {
                    g_queue_free(dl->deferred_queues[qi]);
                    dl->deferred_queues[qi] = NULL;
                }
            }
        }

        // 输出本轮 deferred_list 规模，便于脚本统计每轮预测覆盖范围
        pr_info("[ObsidianDef] deferred_total=%lu\n", dl->deferred_size);

        // 处理deferred_list
        if (dl->deferred_list) {
            if (write_deferred_list(dl, opts.dirty_map_dir)) {
                pr_perror("[Obsidian0215]Error updating deferred_list to file");
            }
            g_hash_table_destroy(dl->deferred_list);
            pthread_mutex_destroy(&dl->deferred_list_mutex);
            dl->deferred_list = NULL;
            dl->deferred_size = 0;
        }
        if (dl->deferred_evict_ttl) {
            g_hash_table_destroy(dl->deferred_evict_ttl);
            dl->deferred_evict_ttl = NULL;
        }
        if (dl->prev_dirty_set) {
            g_hash_table_destroy(dl->prev_dirty_set);
            dl->prev_dirty_set = NULL;
        }

        /* Final deferred correctness summary: unique deferred addresses and how many were later sent */
        if (dl->deferred_ever) {
            float final_acc = dl->deferred_unique_total ? (float)(dl->deferred_unique_total - dl->deferred_sent_after_defer) * 100.0f / dl->deferred_unique_total : 0.0f;
            pr_info("[ObsidianDefFinal] deferred_unique_total=%lu deferred_sent_after_defer=%lu final_decision_accuracy=%.2f\n",
                    dl->deferred_unique_total, dl->deferred_sent_after_defer, final_acc);
            g_hash_table_destroy(dl->deferred_ever);
            dl->deferred_ever = NULL;
        }

        // 预测准确率统计（仅日志输出，避免文件读写）
        if (dl->predicted_total > 0) {
            float acc = (float)dl->predicted_hit * 100.0f / (float)dl->predicted_total;
            pr_info("[ObsidianPred] predicted_total=%lu predicted_hit=%lu predicted_miss=%lu predicted_accuracy=%.2f\n",
                    dl->predicted_total, dl->predicted_hit, dl->predicted_miss, acc);
        } else {
            pr_info("[ObsidianPred] predicted_total=0 predicted_hit=0 predicted_miss=0 predicted_accuracy=0.00\n");
        }
        if (dl->predicted_total_all > 0) {
            float acc_nd = dl->predicted_total_nondefer > 0 ?
                (float)dl->predicted_hit_nondefer * 100.0f / (float)dl->predicted_total_nondefer : 0.0f;
            float acc_all = (float)dl->predicted_hit_all * 100.0f / (float)dl->predicted_total_all;
            pr_info("[ObsidianPred2] nondefer_total=%lu nondefer_hit=%lu nondefer_miss=%lu nondefer_acc=%.2f all_total=%lu all_hit=%lu all_miss=%lu all_acc=%.2f\n",
                    dl->predicted_total_nondefer, dl->predicted_hit_nondefer, dl->predicted_miss_nondefer, acc_nd,
                    dl->predicted_total_all, dl->predicted_hit_all, dl->predicted_miss_all, acc_all);
        } else {
            pr_info("[ObsidianPred2] nondefer_total=0 nondefer_hit=0 nondefer_miss=0 nondefer_acc=0.00 all_total=0 all_hit=0 all_miss=0 all_acc=0.00\n");
        }

        // 处理thresholds
        if (write_thresholds(dl, opts.dirty_map_dir)) {
            pr_perror("[Obsidian0215] Error updating thresholds to file");
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
 * @brief 查找deferred_list中包含指定address
 *
 * @param dl <pid>对应dirtylog指针。deferred_list用于记录被延迟转储的页
 * @param addr 要查找的线性地址
 * @return int 找到则返回1，如果未找到则返回0
 */
int search_deferred_list(struct dirty_log *dl, unsigned long addr) {
    return get_deferred_count(dl, addr) > 0 ? 1 : 0;
}

/**
 * @brief 获取deferred_list中指定address的延迟轮次
 *
 * @param dl <pid>对应dirtylog指针
 * @param addr 要查询的线性地址
 * @return int 返回延迟轮次数，未找到返回0
 */
int get_deferred_count(struct dirty_log *dl, unsigned long addr) {
    int count = 0;
    gpointer value = NULL;

    if (!dl || !dl->deferred_list)
        return 0;

    pthread_mutex_lock(&dl->deferred_list_mutex);
    value = g_hash_table_lookup(dl->deferred_list, GSIZE_TO_POINTER(addr));
    if (value)
        count = GPOINTER_TO_INT(value);
    pthread_mutex_unlock(&dl->deferred_list_mutex);

    return count;
}

/**
 * @brief 将指定address加入deferred_list
 *
 * @param dl <pid>对应dirtylog指针
 * @param addr 要加入的线性地址
 */
static void enqueue_deferred_queue_locked(struct dirty_log *dl, unsigned long addr);

/* Rebuild per-class deferred queues and recompute deferred_heat_sum from current diffmap. */
static void rebuild_deferred_queues(struct dirty_log *dl) {
    int qi;
    GHashTableIter iter;
    gpointer key, value;

    if (!dl || !dl->deferred_list)
        return;

    pthread_mutex_lock(&dl->deferred_list_mutex);
    for (qi = 0; qi < NUM_PAGE_CLASSES; qi++) {
        if (!dl->deferred_queues[qi])
            dl->deferred_queues[qi] = g_queue_new();
        else {
            while (!g_queue_is_empty(dl->deferred_queues[qi]))
                g_queue_pop_head(dl->deferred_queues[qi]);
        }
    }

    dl->deferred_heat_sum = 0.0f;
    g_hash_table_iter_init(&iter, dl->deferred_list);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        unsigned long addr = GPOINTER_TO_SIZE(key);
        struct dirty_diffmap *dhm = search_dirty_map(dl, addr);
        page_class_t cls = classify_page(dl, dhm, 0);
        if (cls < 0 || cls >= NUM_PAGE_CLASSES)
            cls = PAGE_COLD;
        g_queue_push_tail(dl->deferred_queues[cls], GSIZE_TO_POINTER(addr));
        if (dhm)
            dl->deferred_heat_sum += dhm->heat;
        else {
            page_history_t *hist = get_page_history(dl, addr);
            if (hist)
                dl->deferred_heat_sum += hist->mean_heat;
        }
    }
    pthread_mutex_unlock(&dl->deferred_list_mutex);
}

/* Rebuild previous-round dirty set from current diffmap for next round accuracy evaluation. */
static void update_prev_dirty_set(struct dirty_log *dl)
{
    unsigned long i;
    if (!dl)
        return;
    if (dl->prev_dirty_set) {
        g_hash_table_destroy(dl->prev_dirty_set);
        dl->prev_dirty_set = NULL;
    }
    dl->prev_dirty_set = g_hash_table_new(g_direct_hash, g_direct_equal);
    if (!dl->diffmap || dl->diffmap_size == 0)
        return;
    for (i = 0; i < dl->diffmap_size; i++) {
        unsigned long addr = dl->diffmap[i].address;
        g_hash_table_insert(dl->prev_dirty_set, GSIZE_TO_POINTER(addr), GINT_TO_POINTER(1));
    }
}

void add_deferred_list(struct dirty_log *dl, unsigned long addr, int round) {
    gpointer key;
    gpointer value;

    if (!dl || !dl->deferred_list)
        return;

    pthread_mutex_lock(&dl->deferred_list_mutex);
    key = GSIZE_TO_POINTER(addr);
    value = g_hash_table_lookup(dl->deferred_list, key);
    if (value) {
        /* No hard cap on defer counts: increment unbounded and rely on heat-based
         * soft-budget enforcement to later force pages if necessary.
         */
        int count = GPOINTER_TO_INT(value) + 1;
        g_hash_table_insert(dl->deferred_list, key, GINT_TO_POINTER(count));
        if (dl->deferred_evict_ttl)
            g_hash_table_remove(dl->deferred_evict_ttl, key);
        if (round >= 0) {
            int protected_until = round + MIN_DEFER_COOLDOWN_ROUNDS - 1;
            if (!dl->deferred_protected_until)
                dl->deferred_protected_until = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_hash_table_insert(dl->deferred_protected_until, key, GINT_TO_POINTER(protected_until));
        }
    } else {
        struct dirty_diffmap *dhm_add = NULL;
        float add_heat = 0.0f;
        g_hash_table_insert(dl->deferred_list, key, GINT_TO_POINTER(1));
        if (dl->deferred_evict_ttl)
            g_hash_table_remove(dl->deferred_evict_ttl, key);
        if (round >= 0) {
            int protected_until = round + MIN_DEFER_COOLDOWN_ROUNDS - 1;
            if (!dl->deferred_protected_until)
                dl->deferred_protected_until = g_hash_table_new(g_direct_hash, g_direct_equal);
            g_hash_table_insert(dl->deferred_protected_until, key, GINT_TO_POINTER(protected_until));
        }
        dl->deferred_size++;
        /* Update deferred_heat_sum: prefer dhm->heat, fallback to historical mean */
        dhm_add = search_dirty_map(dl, addr);
        if (dhm_add)
            add_heat = dhm_add->heat;
        else {
            page_history_t *hist_add = get_page_history(dl, addr);
            if (hist_add)
                add_heat = hist_add->mean_heat;
        }
        dl->deferred_heat_sum += add_heat;

        /* Enqueue into per-class deferred queue (caller holds deferred_list_mutex) */
        {
            page_class_t cls = PAGE_COLD;
            cls = classify_page(dl, dhm_add, 0);
            if (cls < 0 || cls >= NUM_PAGE_CLASSES)
                cls = PAGE_COLD;
            /* caller holds deferred_list_mutex */
            enqueue_deferred_queue_locked(dl, addr);
        }

        /* Track unique deferred addresses for final correctness stats */
        if (!dl->deferred_ever)
            dl->deferred_ever = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
        if (!g_hash_table_contains(dl->deferred_ever, key)) {
            g_hash_table_insert(dl->deferred_ever, key, GINT_TO_POINTER(0)); /* flags=0 -> not yet sent */
            dl->deferred_unique_total++;
        }
    }
    /* Lightweight telemetry: always emit a compact log when a page is deferred (helps diagnostics) */
    if (decision_telemetry_enabled()) {
        gpointer new_v = g_hash_table_lookup(dl->deferred_list, key);
        int new_count = new_v ? GPOINTER_TO_INT(new_v) : 0;
        if (round >= 0) {
            int protected_until = round + MIN_DEFER_COOLDOWN_ROUNDS - 1;
            pr_info("[ObsidianDEFER] pid=%d addr=0x%lx count=%d deferred_size=%lu last_round=%d protected_until=%d\n",
                dl->pid, addr, new_count, dl->deferred_size, round, protected_until);
        } else {
            pr_info("[ObsidianDEFER] pid=%d addr=0x%lx count=%d deferred_size=%lu last_round=%d\n",
                dl->pid, addr, new_count, dl->deferred_size, round);
        }
    }
    pthread_mutex_unlock(&dl->deferred_list_mutex);
}

/**
 * @brief 将指定address从deferred_list移除
 *
 * @param dl <pid>对应dirtylog指针
 * @param addr 要移除的线性地址
 */
void del_deferred_list(struct dirty_log *dl, unsigned long addr) {
    struct dirty_diffmap *dhm_rem = NULL;
    float rem_heat = 0.0f;

    if (!dl || !dl->deferred_list)
        return;

    pthread_mutex_lock(&dl->deferred_list_mutex);
    if (g_hash_table_remove(dl->deferred_list, GSIZE_TO_POINTER(addr))) {
        dl->deferred_size--;
        if (dl->deferred_protected_until)
            g_hash_table_remove(dl->deferred_protected_until, GSIZE_TO_POINTER(addr));
        if (dl->deferred_evict_ttl)
            g_hash_table_remove(dl->deferred_evict_ttl, GSIZE_TO_POINTER(addr));
        /* Subtract deferred heat sum if possible (use dhm or history fallback) */
        dhm_rem = search_dirty_map(dl, addr);
        if (dhm_rem)
            rem_heat = dhm_rem->heat;
        else {
            page_history_t *hist_rem = get_page_history(dl, addr);
            if (hist_rem)
                rem_heat = hist_rem->mean_heat;
        }
        if (dl->deferred_heat_sum > rem_heat)
            dl->deferred_heat_sum -= rem_heat;
        else
            dl->deferred_heat_sum = 0.0f;
    }
    pthread_mutex_unlock(&dl->deferred_list_mutex);
}

/* Enqueue helper: caller must hold deferred_list_mutex */
static void enqueue_deferred_queue_locked(struct dirty_log *dl, unsigned long addr) {
    struct dirty_diffmap *dhm = NULL;
    page_class_t cls = PAGE_COLD;
    if (!dl)
        return;
    dhm = search_dirty_map(dl, addr);
    cls = classify_page(dl, dhm, 0);
    if (cls < 0 || cls >= NUM_PAGE_CLASSES)
        cls = PAGE_COLD;
    if (dl->deferred_queues[cls])
        g_queue_push_tail(dl->deferred_queues[cls], GSIZE_TO_POINTER(addr));
}

/* Urgent-force helpers: mark a deferred page to be forced-sent in the current/next pre-dump. */
void add_urgent_force(struct dirty_log *dl, unsigned long addr) {
    gpointer key = GSIZE_TO_POINTER(addr);
    if (!dl)
        return;

    /* Lazy-init urgent_force set and its mutex under deferred_list_mutex to avoid races */
    pthread_mutex_lock(&dl->deferred_list_mutex);
    if (!dl->urgent_force) {
        dl->urgent_force = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, NULL);
        pthread_mutex_init(&dl->urgent_force_mutex, NULL);
    }
    pthread_mutex_unlock(&dl->deferred_list_mutex);

    pthread_mutex_lock(&dl->urgent_force_mutex);
    g_hash_table_insert(dl->urgent_force, key, GINT_TO_POINTER(1));
    pthread_mutex_unlock(&dl->urgent_force_mutex);
}

int search_urgent_force(struct dirty_log *dl, unsigned long addr) {
    int found = 0;
    if (!dl || !dl->urgent_force)
        return 0;
    pthread_mutex_lock(&dl->urgent_force_mutex);
    found = g_hash_table_lookup(dl->urgent_force, GSIZE_TO_POINTER(addr)) ? 1 : 0;
    pthread_mutex_unlock(&dl->urgent_force_mutex);
    return found;
}

void del_urgent_force(struct dirty_log *dl, unsigned long addr) {
    if (!dl || !dl->urgent_force)
        return;
    pthread_mutex_lock(&dl->urgent_force_mutex);
    if (g_hash_table_remove(dl->urgent_force, GSIZE_TO_POINTER(addr))) {
        /* no-op */
    }
    pthread_mutex_unlock(&dl->urgent_force_mutex);
}

/* Soft-budget enforcement: when the sum of deferred heat exceeds a budget, pick
 * low-heat deferred pages to force-send (partial recovery). Selection order: low heat first.
 */
void enforce_deferred_soft_budget(struct dirty_log *dl) {
    /* Declarations first (C90) */
    float soft_ratio = 0.05f;
    const float EPS = 1e-6f;
    float budget_heat = 0.0f;
    float over = 0.0f; /* queue-based enforcement: candidate array removed */

    if (!dl || !dl->deferred_list || dl->deferred_size == 0)
        return;

    /* Use adaptive soft ratio stored in dirty_log (no env overrides) */
    if (dl && dl->adaptive_soft_ratio > 0.0f)
        soft_ratio = dl->adaptive_soft_ratio;

    if (!(dl->diffmap_size > 0 && dl->global_mean_heat > 0.0f))
        return;

    budget_heat = dl->global_mean_heat * (float)dl->diffmap_size * soft_ratio + EPS;
    over = dl->deferred_heat_sum - budget_heat;
    if (over < 0.0f)
        over = budget_heat * 0.2f; /* still drain a small portion to avoid final-dump spikes */

    /* Queue-based pick: drain a bounded ratio of deferred pages across classes */
    {
        int cap_total;
        int picked_total;
        float removed;
        int class_order[NUM_PAGE_CLASSES];
        float class_weight[NUM_PAGE_CLASSES];
        float class_scale[NUM_PAGE_CLASSES];
        int class_cap[NUM_PAGE_CLASSES];
        int class_picked[NUM_PAGE_CLASSES];
        float round_frac;
        float base_ratio;
        int cap_heat;
        int ci;
        int cls;
        int attempts;
        int max_attempts;
        gpointer addr_p;
        unsigned long a;
        int present;
        struct dirty_diffmap *dhm;
        float h;
        int dc_here;
        int protected_until;
        float score_tmp;
        float p_model_tmp;
        float p_next_tmp;
        float p_thr_tmp;
        page_class_t pcls_tmp;

        round_frac = ((float)dl->current_round) / ((float)dl->current_round + 2.0f);
        if (round_frac < 0.0f) round_frac = 0.0f;
        if (round_frac > 1.0f) round_frac = 1.0f;

        /* Base drain ratio: small early, larger later to avoid last-dump spikes */
        base_ratio = 0.20f + 0.35f * round_frac;
        if (base_ratio < 0.12f) base_ratio = 0.12f;
        if (base_ratio > 0.55f) base_ratio = 0.55f;

        cap_total = (int)(dl->deferred_size * base_ratio);
        if (cap_total < 1) cap_total = 1;
        cap_heat = (int)(over / (dl->global_mean_heat + EPS)) + 1;
        if (cap_heat > cap_total)
            cap_total = cap_heat;
        if (cap_total > (int)dl->deferred_size)
            cap_total = (int)dl->deferred_size;

        picked_total = 0;
        removed = 0.0f;

        /* class priority: cold -> freezing -> warm -> hot (cooler pages first) */
        class_order[0] = PAGE_COLD;
        class_order[1] = PAGE_FREEZING;
        class_order[2] = PAGE_WARM;
        class_order[3] = PAGE_HOT;

        /* per-class drain weights and round scaling (warm/hot drain slower) */
        class_weight[PAGE_FREEZING] = 0.30f;
        class_weight[PAGE_COLD] = 0.30f;
        class_weight[PAGE_WARM] = 0.25f;
        class_weight[PAGE_HOT] = 0.15f;

        class_scale[PAGE_FREEZING] = 1.0f;
        class_scale[PAGE_COLD] = 1.0f;
        class_scale[PAGE_WARM] = 0.7f + 0.3f * round_frac;
        class_scale[PAGE_HOT] = 0.4f + 0.6f * round_frac;

        for (ci = 0; ci < NUM_PAGE_CLASSES; ci++) {
            class_cap[ci] = (int)(cap_total * class_weight[ci] * class_scale[ci]);
            if (class_cap[ci] < 0)
                class_cap[ci] = 0;
            class_picked[ci] = 0;
        }

        for (ci = 0; ci < NUM_PAGE_CLASSES && removed < over && picked_total < cap_total; ci++) {
            cls = class_order[ci];
            attempts = 0;
            max_attempts = (int)dl->deferred_size + 4;
            while (class_picked[cls] < class_cap[cls] && picked_total < cap_total && removed < over && attempts < max_attempts) {
                attempts++;
                addr_p = NULL;

                /* Peek and pop the queue head under deferred_list_mutex to avoid races */
                pthread_mutex_lock(&dl->deferred_list_mutex);
                if (!dl->deferred_queues[cls] || g_queue_is_empty(dl->deferred_queues[cls])) {
                    pthread_mutex_unlock(&dl->deferred_list_mutex);
                    break; /* move to next class */
                }
                addr_p = g_queue_peek_head(dl->deferred_queues[cls]);
                if (!addr_p) {
                    pthread_mutex_unlock(&dl->deferred_list_mutex);
                    break;
                }
                a = (unsigned long) addr_p;
                /* Pop it now to avoid double-picks */
                g_queue_pop_head(dl->deferred_queues[cls]);
                pthread_mutex_unlock(&dl->deferred_list_mutex);

                /* Verify it is still present in deferred_list (might have been removed concurrently) */
                pthread_mutex_lock(&dl->deferred_list_mutex);
                present = g_hash_table_lookup(dl->deferred_list, GSIZE_TO_POINTER(a)) ? 1 : 0;
                pthread_mutex_unlock(&dl->deferred_list_mutex);
                if (!present)
                    continue; /* stale entry */

                /* Respect cooldown/min-count protections: requeue and skip */
                dc_here = get_deferred_count(dl, a);
                protected_until = -1;
                if (dl->deferred_protected_until) {
                    gpointer pr = NULL;
                    pthread_mutex_lock(&dl->deferred_list_mutex);
                    pr = g_hash_table_lookup(dl->deferred_protected_until, GSIZE_TO_POINTER(a));
                    if (pr)
                        protected_until = GPOINTER_TO_INT(pr);
                    pthread_mutex_unlock(&dl->deferred_list_mutex);
                }
                if ((dc_here < MIN_DEFER_COUNT_FOR_ENFORCE) || (protected_until >= 0 && dl->current_round >= 0 && dl->current_round <= protected_until)) {
                    pthread_mutex_lock(&dl->deferred_list_mutex);
                    if (dl->deferred_queues[cls])
                        g_queue_push_tail(dl->deferred_queues[cls], GSIZE_TO_POINTER(a));
                    pthread_mutex_unlock(&dl->deferred_list_mutex);
                    continue;
                }

                /* Prefer evicting low-probability pages first */
                dhm = search_dirty_map(dl, a);
                if (dhm) {
                    score_tmp = 0.0f;
                    p_model_tmp = -1.0f;
                    p_next_tmp = compute_p_next_dirty(dl, dhm, a, false, dl->current_round, false, dc_here, false, &score_tmp, &p_model_tmp);
                    pcls_tmp = classify_page(dl, dhm, dl->current_round);
                    p_thr_tmp = decision_p_threshold(dl, pcls_tmp);
                    if (round_frac < 0.35f && p_next_tmp >= p_thr_tmp && (cls == PAGE_WARM || cls == PAGE_HOT)) {
                        pthread_mutex_lock(&dl->deferred_list_mutex);
                        if (dl->deferred_queues[cls])
                            g_queue_push_tail(dl->deferred_queues[cls], GSIZE_TO_POINTER(a));
                        pthread_mutex_unlock(&dl->deferred_list_mutex);
                        continue;
                    }
                }

                /* Account heat (use dhm or history fallback) */
                h = 0.0f;
                if (dhm)
                    h = dhm->heat;
                else {
                    page_history_t *hist = get_page_history(dl, a);
                    if (hist)
                        h = hist->mean_heat;
                }
                del_deferred_list(dl, a);
                add_urgent_force(dl, a);
                removed += h;
                picked_total++;
                class_picked[cls]++;
                pr_info("[ObsidianQueuePick] pid=%d picked addr=0x%lx class=%d heat=%.3f\n", dl->pid, a, cls, h);
            }
        }
        pr_info("[ObsidianEnforceQueue] pid=%d: picked %d pages to force (heat_removed=%.3f target=%.3f cap=%d ratio=%.3f round=%d progress=%.2f deferred_sum_after=%.3f)\n",
            dl->pid, picked_total, removed, over, cap_total, base_ratio, dl->current_round, round_frac, dl->deferred_heat_sum);
    }
}

/* candidate comparator removed: enforcement now uses per-class FIFO queues */

/* Per-class max defer rounds (non-static so test can query) */
int get_max_defer_rounds_for_class(struct dirty_log *dl, page_class_t page_class) {
    /* Prefer per-dirty-log adaptive per-class limits (automatic, no envs) */
    if (dl) {
        int idx = (int)page_class;
        if (idx >= 0 && idx < 5)
            return dl->adaptive_max_defer[idx];
    }
    /* Fallback defaults: hotter pages are allowed more defer rounds (prefer skipping hot pages) */
    switch (page_class) {
    case PAGE_FREEZING: return 0;
    case PAGE_COLD:     return 1;
    case PAGE_WARM:     return 2;
    case PAGE_HOT:      return 10; /* merged burning allowance */
    default:            return 2;
    }
}

/* Decision model and helpers migrated from mem.c to keep decision state with dirty-map.
 * This encapsulates the logistic scoring model and adaptive threshold computations.
 */

#define NUM_LOGISTIC_FEATURES 12
#define MODEL_LEGACY  0
#define MODEL_ONLINE  1
#define MODEL_OFFLINE 2

typedef struct {
    int inited;
    float W0;
    float W_HEAT;
    float W_TREND;
    float W_DEFER_CNT;
    float W_BURST;
    float W_WARM;
    float W_PARENT;
    float W_DIRTY_FREQ;
    float PAT_DECL_PENALTY;
    float HIST_DECL_PENALTY;
    float SCORE_CLAMP;
    float CONF_K;
    float EMA_ALPHA;
    float DEFERRED_RISK_COEF;
    float CLASS_BIAS[NUM_PAGE_CLASSES];

    /* Predictor selection */
    int model_kind; /* legacy | online | offline */

    /* Logistic skip model (learned) - optional replacement for legacy score->sigmoid.
     * Features order (mapped to decisions.csv columns):
     * 0: heat
     * 1: heat_trend
     * 2: writes_est
     * 3: track_s
     * 4: hist_count
     * 5: hist_mean
     * 6: hist_variance
     * 7: deferred
     * 8: p (legacy sigmoid(score))
     * 9: score (legacy linear score)
     * 10: pthr (adaptive threshold)
     * 11: round
     */
    int use_logistic_model; /* enabled only when a valid model file is loaded */
    float logistic_weights[NUM_LOGISTIC_FEATURES];
    float logistic_intercept;
    float logistic_threshold;

    /* Online calibration (score -> sigmoid(scale*score + bias)) */
    float online_bias;
    float online_scale;
    float online_class_bias[NUM_PAGE_CLASSES];
    float online_class_scale[NUM_PAGE_CLASSES];
    float online_lr;
    float online_l2;
    unsigned long online_updates;
} decision_model_t;

static decision_model_t dm; /* internal model state */

/* Sigmoid helper */
static inline float sigmoidf(float x) {
    return 1.0f / (1.0f + expf(-x));
}

/* candidate comparator removed: enforcement now uses per-class queues */
/* Forward-declare runtime skip-model loader */
static int load_skip_model_from_file(const char *path);

static int parse_model_kind(void) {
    const char *env = getenv("CRIU_PREDICT_MODEL");
    if (!env || !env[0])
        return MODEL_ONLINE;
    if (!strcmp(env, "online"))
        return MODEL_ONLINE;
    if (!strcmp(env, "offline"))
        return MODEL_OFFLINE;
    if (!strcmp(env, "legacy"))
        return MODEL_LEGACY;
    return MODEL_LEGACY;
}

/* Initialize the model defaults (one-time) */
static void decision_model_init(void) {
    int i;
    if (dm.inited)
        return;
    dm.inited = 1;

    dm.W0 = -2.5f;
    dm.W_HEAT = 2.2f;
    dm.W_TREND = 1.3f;
    dm.W_DEFER_CNT = 1.0f;
    dm.W_BURST = 1.5f;
    dm.W_WARM = -0.5f; /* warmer bias: be more conservative for warm pages */
    dm.W_PARENT = 0.0f;
    dm.W_DIRTY_FREQ = 1.0f; /* favor deferring long-lived dirty pages */
    dm.PAT_DECL_PENALTY = 1.6f; /* stronger penalty for declining patterns (favor dump) */
    dm.HIST_DECL_PENALTY = 1.6f; /* stronger penalty for historical decline */
    dm.SCORE_CLAMP = 20.0f;
    dm.CONF_K = 2.0f;
    dm.EMA_ALPHA = 0.3f;
    dm.DEFERRED_RISK_COEF = 1.0f;
    /* Class biases: freezing/cold bias toward lower p_next (favor immediate dump),
     * hot bias toward higher p_next (favor defer) — merged BURNING into HOT */
    dm.CLASS_BIAS[PAGE_FREEZING] = -1.0f;
    dm.CLASS_BIAS[PAGE_COLD] = -0.5f;
    dm.CLASS_BIAS[PAGE_WARM] = -0.2f;
    dm.CLASS_BIAS[PAGE_HOT] = 0.6f; /* merged burning bias */

    dm.model_kind = parse_model_kind();

    /* Offline logistic model state (only used when model_kind == MODEL_OFFLINE) */
    dm.logistic_intercept = 0.0f;
    memset(dm.logistic_weights, 0, sizeof(dm.logistic_weights));
    dm.logistic_threshold = -1.0f;
    dm.use_logistic_model = 0;

    /* Online calibration defaults (only used when model_kind == MODEL_ONLINE) */
    dm.online_bias = 0.0f;
    dm.online_scale = 1.0f;
    dm.online_lr = 0.05f;
    dm.online_l2 = 0.0001f;
    dm.online_updates = 0;
    for (i = 0; i < NUM_PAGE_CLASSES; i++) {
        dm.online_class_bias[i] = 0.0f;
        dm.online_class_scale[i] = 0.0f;
    }
    {
        const char *lr_env = getenv("CRIU_ONLINE_LR");
        const char *l2_env = getenv("CRIU_ONLINE_L2");
        if (lr_env && lr_env[0]) {
            double v = strtod(lr_env, NULL);
            if (v > 0.0)
                dm.online_lr = (float)v;
        }
        if (l2_env && l2_env[0]) {
            double v = strtod(l2_env, NULL);
            if (v >= 0.0)
                dm.online_l2 = (float)v;
        }
    }

    /* Only load an explicitly specified model file when offline model is requested */
    if (dm.model_kind == MODEL_OFFLINE) {
        const char *mf = getenv("CRIU_SKIP_MODEL_FILE");
        if (mf && mf[0]) {
            if (access(mf, R_OK) == 0 && load_skip_model_from_file(mf))
                pr_info("Loaded offline skip model from %s\n", mf);
            else
                pr_info("Offline skip model file not found or failed to load: %s\n", mf);
        }
        if (!dm.use_logistic_model)
            dm.model_kind = MODEL_LEGACY; /* fallback if no model loaded */
    }
}

static int decision_model_online_active(void) {
    decision_model_init();
    return dm.model_kind == MODEL_ONLINE;
}

static void decision_model_online_snapshot(float *bias, float *scale, unsigned long *updates) {
    if (bias)
        *bias = dm.online_bias;
    if (scale)
        *scale = dm.online_scale;
    if (updates)
        *updates = dm.online_updates;
}

static void decision_model_online_update(float score, float p, float y, page_class_t page_class) {
    float err;
    const float max_bias = 10.0f;
    const float max_scale = 10.0f;
    int idx;

    decision_model_init();
    if (dm.model_kind != MODEL_ONLINE)
        return;

    err = p - y;
    dm.online_bias -= dm.online_lr * (err + dm.online_l2 * dm.online_bias);
    dm.online_scale -= dm.online_lr * (err * score + dm.online_l2 * dm.online_scale);

    idx = (int)page_class;
    if (idx < 0 || idx >= NUM_PAGE_CLASSES)
        idx = PAGE_COLD;
    dm.online_class_bias[idx] -= dm.online_lr * (err + dm.online_l2 * dm.online_class_bias[idx]);
    dm.online_class_scale[idx] -= dm.online_lr * (err * score + dm.online_l2 * dm.online_class_scale[idx]);

    if (dm.online_bias > max_bias) dm.online_bias = max_bias;
    if (dm.online_bias < -max_bias) dm.online_bias = -max_bias;
    if (dm.online_scale > max_scale) dm.online_scale = max_scale;
    if (dm.online_scale < -max_scale) dm.online_scale = -max_scale;

    if (dm.online_class_bias[idx] > max_bias) dm.online_class_bias[idx] = max_bias;
    if (dm.online_class_bias[idx] < -max_bias) dm.online_class_bias[idx] = -max_bias;
    if (dm.online_class_scale[idx] > max_scale) dm.online_class_scale[idx] = max_scale;
    if (dm.online_class_scale[idx] < -max_scale) dm.online_class_scale[idx] = -max_scale;

    dm.online_updates++;
}

bool skip_model_loaded(void) {
    decision_model_init();
    if (dm.model_kind == MODEL_ONLINE)
        return true;
    if (dm.model_kind == MODEL_OFFLINE && dm.use_logistic_model)
        return true;
    return false;
}

/* Helper: load a skip_model JSON file (simple robust parser for our schema).
 * Populates dm.logistic_weights[], dm.logistic_intercept and dm.logistic_threshold.
 * Returns 1 on success (weights parsed), 0 on failure.
 */
static int load_skip_model_from_file(const char *path) {
    FILE *f;
    long sz;
    char *buf = NULL;
    char *w = NULL;
    char *b = NULL;
    char *p = NULL;
    char *end = NULL;
    int parsed = 0;
    int idx = 0;
    double val = 0.0;

    f = fopen(path, "r");
    if (!f) return 0;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return 0; }
    sz = ftell(f);
    if (sz <= 0) { fclose(f); return 0; }
    rewind(f);

    buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); return 0; }
    if (fread(buf, 1, (size_t)sz, f) != (size_t)sz) { free(buf); fclose(f); return 0; }
    buf[sz] = '\0';
    fclose(f);

    /* parse weights array */
    w = strstr(buf, "\"weights\"");
    if (w) {
        b = strchr(w, '[');
        if (b) {
            p = b + 1;
            idx = 0;
            while (p && *p && *p != ']' && idx < NUM_LOGISTIC_FEATURES) {
                while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
                if (!*p || *p == ']') break;
                val = strtod(p, &end);
                if (p == end) break;
                dm.logistic_weights[idx++] = (float)val;
                p = end;
            }
            parsed = idx;
        }
    }

    /* parse intercept */
    {
        char *ip = strstr(buf, "\"intercept\"");
        if (ip) {
            char *c = strchr(ip, ':');
            if (c) {
                double v = strtod(c + 1, NULL);
                dm.logistic_intercept = (float)v;
            }
        }
    }

    /* parse best_threshold (optional) */
    {
        char *tp = strstr(buf, "\"best_threshold\"");
        if (tp) {
            char *c = strchr(tp, ':');
            if (c) {
                double v = strtod(c + 1, NULL);
                const char *env_p;
                double env_vd;
                float env_v;

                dm.logistic_threshold = (float)v;

                /* Optional: allow runtime override of model threshold via environment
                 * variable CRIU_SKIP_MODEL_MIN_THRESHOLD to tune conservativeness without
                 * retraining or installing a new model. */
                env_p = getenv("CRIU_SKIP_MODEL_MIN_THRESHOLD");
                if (env_p && env_p[0] != '\0') {
                    env_vd = strtod(env_p, NULL);
                    env_v = (float)env_vd;
                    if (env_v > dm.logistic_threshold) {
                        dm.logistic_threshold = env_v;
                        pr_info("[Obsidian0215] CRIU_SKIP_MODEL_MIN_THRESHOLD override applied: %.3f\n", dm.logistic_threshold);
                    }
                }
            }
        }
    }

    free(buf);
    if (parsed == NUM_LOGISTIC_FEATURES) {
        dm.use_logistic_model = 1;
        return 1;
    }
    return 0;
}

/* Compute probability that page will be dirty in the next iteration.
 * Returns p in [0,1] and optionally writes score to out_score.
 */
float compute_p_next_dirty(struct dirty_log *dl, struct dirty_diffmap *dhm, unsigned long vaddr, bool softdirty, int round, bool parent_missing, int deferred_count, bool is_warm, float *out_score, float *out_p_model) {
    float base;
    float heat_norm;
    float trend_norm;
    int pattern;
    float pat_burst;
    float pat_decl;
    float hist_decl;
    float hist_boost;
    page_history_t *hist;
    float score;
    float old_p;
    float writes_est = 0.0f;
    float track_s = 1.0f;
    page_class_t page_class = PAGE_FREEZING;
    float confidence = 0.0f;
    float deferred_risk = 0.0f;
    float deferred_norm_denom = 1.0f;
    float dirty_freq = 0.0f;
    float pred_rate = -1.0f;
    float w_dirty = 0.0f;
    float p_thr_model;
    float transient_penalty = 0.0f;
    float hist_cv = 0.0f;
    float persistent_penalty = 0.0f;
    int warm_hint = 0;
    /* temporaries for optional logistic model */
    int i;
    float logit;
    float p_logit;

    decision_model_init();

    if (!dhm) {
        if (softdirty) {
            if (round == 1) {
                if (out_score) *out_score = -1.1f;
                return 0.25f;
            } else {
                if (out_score) *out_score = -0.2f;
                return 0.45f;
            }
        } else {
            if (out_score) *out_score = -2.5f;
            return 0.08f;
        }
    }

    base = (dl && dl->threshold_mid > 0.0f) ? dl->threshold_mid : 1.0f;
    heat_norm = dhm->heat / (base + 1e-6f);
    trend_norm = (dhm->heat != 0.0f) ? (dhm->heat_trend / (dhm->heat + 1e-6f)) : 0.0f;

    pattern = analyze_access_pattern(dl, vaddr);
    pat_burst = (pattern == PATTERN_BURST) ? 1.0f : 0.0f;
    pat_decl = (pattern == PATTERN_DECLINING) ? 1.0f : 0.0f;
    hist_decl = 0.0f;
    hist = get_page_history(dl, vaddr);
    if (hist && hist->history_count >= 3 && hist->is_declining)
        hist_decl = 1.0f;
    if (hist)
        dirty_freq = hist->dirty_freq;
    w_dirty = dm.W_DIRTY_FREQ;
    if (dl && dl->predicted_total > 100) {
        pred_rate = (float)dl->predicted_hit / (float)dl->predicted_total;
        if (pred_rate < 0.65f)
            w_dirty *= (pred_rate / 0.65f);
    }

    if (dl && dl->ldm_header && dl->ldm_header->track_duration_ns > 0)
        track_s = (float)dl->ldm_header->track_duration_ns / 1e9f;
    writes_est = dhm->heat * track_s;
    confidence = writes_est / (writes_est + dm.CONF_K);

    page_class = classify_page(dl, dhm, round);

    /* Derive a warm-hint when warm_list is removed: stable low-heat pages are treated as warm. */
    warm_hint = is_warm ? 1 : 0;
    if (!warm_hint && page_class == PAGE_WARM) {
        if (hist && hist->is_stable)
            warm_hint = 1;
        else if (dl && dl->threshold_mid > 0.0f && dhm && dhm->heat > 0.0f) {
            if (dhm->heat < dl->threshold_mid * 0.5f)
                warm_hint = 1;
        }
    }

    if (dl && dl->diffmap_size > 0 && dl->global_mean_heat > 0.0f) {
        deferred_norm_denom = dl->global_mean_heat * (float)dl->diffmap_size + 1e-6f;
        deferred_risk = dl->deferred_heat_sum / deferred_norm_denom;
    }

    score = dm.W0
          + dm.W_HEAT * heat_norm * (0.5f + 0.5f * confidence)
          + dm.W_TREND * trend_norm * confidence
          + dm.W_DEFER_CNT * deferred_count
          + dm.W_BURST * pat_burst
          + dm.W_WARM * (warm_hint ? 1.0f : 0.0f)
          + w_dirty * dirty_freq
          /* NOTE: parent_missing used to be given an explicit weight here (dm.W_PARENT).
           * Recent design changes remove special-casing of parent-missing pages; they are
           * now treated like ordinary pages and evaluated by the general p_next/p_thr
           * decision path in mem.c. Keep parameter for backward compatibility but do not
           * alter the score directly based on parent_missing. */
          - (pat_decl * dm.PAT_DECL_PENALTY)
          - (hist_decl * dm.HIST_DECL_PENALTY);

    score += dm.CLASS_BIAS[page_class];

    /* Persistently dirty pages with low absolute heat: boost p_next using history. */
    hist_boost = 0.0f;
    if (hist && hist->history_count >= 2 && dhm->heat > 0.0f) {
        if (dl && dl->threshold_low > 0.0f && dhm->heat < dl->threshold_low)
            hist_boost = 0.4f;
        else
            hist_boost = 0.2f;
        hist_boost += 0.05f * (float)(hist->history_count - 2);
        if (hist_boost > 0.8f)
            hist_boost = 0.8f;
        score += hist_boost;
    }

    /* Defer risk weighting: penalize transient/bursty pages that appear dirty sporadically. */
    if (hist) {
        if (hist->dirty_freq < 0.35f)
            transient_penalty += (0.35f - hist->dirty_freq) * 1.2f;
        if (hist->mean_heat > 0.0f && hist->variance_heat > 0.0f) {
            hist_cv = sqrtf(hist->variance_heat) / (hist->mean_heat + 1e-6f);
            if (hist_cv > 0.6f)
                transient_penalty += (hist_cv - 0.6f) * 0.5f;
        }
    }
    if (pattern == PATTERN_BURST)
        transient_penalty += 0.5f;
    if (transient_penalty > 0.0f)
        score -= transient_penalty;

    /* Penalize persistently dirty, low-variance pages that are not cooling.
     * This reduces repeated defers that tend to miss in practice.
     */
    if (hist && hist->dirty_freq >= 0.80f) {
        float rel_trend_p = 0.0f;
        if (dhm && dhm->heat > 0.0f)
            rel_trend_p = dhm->heat_trend / (dhm->heat + 1e-6f);
        if (hist_cv < 0.45f && rel_trend_p > -0.10f) {
            persistent_penalty = 0.4f + (hist->dirty_freq - 0.80f) * 1.0f;
            if (persistent_penalty > 1.2f)
                persistent_penalty = 1.2f;
            score -= persistent_penalty;
        }
    }

    score -= dm.DEFERRED_RISK_COEF * deferred_risk;

    if (score > dm.SCORE_CLAMP)
        score = dm.SCORE_CLAMP;
    if (score < -dm.SCORE_CLAMP)
        score = -dm.SCORE_CLAMP;

    if (hist) {
        if (hist->history_count > 0 && dm.EMA_ALPHA > 0.0f && hist->last_round >= 0) {
            float smoothed = dm.EMA_ALPHA * score + (1.0f - dm.EMA_ALPHA) * hist->score_ema;
            hist->score_ema = smoothed;
            score = smoothed;
        } else {
            hist->score_ema = score;
        }
    }

    /* Legacy probability computed from score (kept for telemetry as feature 'p') */
    old_p = sigmoidf(score);

    /* compute adaptive p threshold (used as model feature 'pthr') */
    p_thr_model = decision_p_threshold(dl, page_class);

    /* Offline logistic model: compute p via learned linear model + sigmoid
     * (we keep 'score' unchanged for telemetry and debugging). */
    if (dm.model_kind == MODEL_OFFLINE && dm.use_logistic_model) {
        float feats[NUM_LOGISTIC_FEATURES];
        feats[0] = dhm ? dhm->heat : 0.0f;
        feats[1] = dhm ? dhm->heat_trend : 0.0f;
        feats[2] = writes_est;
        feats[3] = track_s;
        feats[4] = hist ? (float)hist->history_count : 0.0f;
        feats[5] = hist ? hist->mean_heat : 0.0f;
        feats[6] = hist ? hist->variance_heat : 0.0f;
        feats[7] = (float)deferred_count;
        feats[8] = old_p;
        feats[9] = score;
        feats[10] = p_thr_model;
        feats[11] = (float)round;

        logit = dm.logistic_intercept;
        for (i = 0; i < NUM_LOGISTIC_FEATURES; ++i)
            logit += dm.logistic_weights[i] * feats[i];
        p_logit = sigmoidf(logit);

        if (out_p_model) *out_p_model = p_logit;
        if (out_score) *out_score = score;
        return p_logit;
    }

    /* Online calibration: use sigmoid(scale*score + bias) */
    if (dm.model_kind == MODEL_ONLINE) {
        int idx = (int)page_class;
        float scale;
        float bias;
        float p_online;

        if (idx < 0 || idx >= NUM_PAGE_CLASSES)
            idx = PAGE_COLD;
        scale = dm.online_scale + dm.online_class_scale[idx];
        bias = dm.online_bias + dm.online_class_bias[idx];
        p_online = sigmoidf(scale * score + bias);
        if (out_p_model) *out_p_model = p_online;
        if (out_score) *out_score = score;
        return p_online;
    }

    if (out_p_model) *out_p_model = -1.0f;
    if (out_score) *out_score = score;
    return old_p;
}

float dirtymap_pid_hotness_factor(struct dirty_log *dl) {
    float factor = 1.0f;
    float ratio;
    if (!dl || !dl->stats_collected || dl->global_median_heat <= 0.0f)
        return 1.0f;
    ratio = dl->global_mean_heat / (dl->global_median_heat + 1e-6f);
    if (ratio > 1.0f) {
        factor = 1.0f - fminf(0.25f, (ratio - 1.0f) * 0.15f);
    } else {
        factor = 1.0f + fminf(0.15f, (1.0f - ratio) * 0.15f);
    }
    if (factor < 0.80f) factor = 0.80f;
    if (factor > 1.20f) factor = 1.20f;
    return factor;
}

float decision_p_threshold(struct dirty_log *dl, page_class_t page_class) {
    float base = (dl && dl->adaptive_p_base > 0.0f) ? dl->adaptive_p_base : 0.65f;
    const float min_p = 0.05f;
    const float max_p = 0.95f;
    float soft_ratio = (dl && dl->adaptive_soft_ratio > 0.0f) ? dl->adaptive_soft_ratio : 0.05f;
    const float k = 2.0f;
    float factor = 1.0f;
    float deferred_util = 0.0f;
    float class_factor = 1.0f; /* declared up-front for C90 compliance */
    float round_factor = 1.0f;
    float hot_factor = 1.0f;
    float p = 0.0f;
    /* If a logistic model is active and contains a trained best_threshold, prefer it
     * as a minimum (conservative) decision threshold so the runtime respects model bias. */
    if (dm.model_kind == MODEL_OFFLINE && dm.use_logistic_model && dm.logistic_threshold > 0.0f) {
        if (dm.logistic_threshold > base) {
            pr_info("[Obsidian0215] decision_p_threshold: model best_threshold=%.3f overrides base %.3f\n", dm.logistic_threshold, base);
            base = dm.logistic_threshold;
        }
    }

    if (dl && dl->diffmap_size > 0 && dl->global_mean_heat > 0.0f) {
        float budget_heat = dl->global_mean_heat * (float)dl->diffmap_size * soft_ratio + 1e-6f;
        deferred_util = dl->deferred_heat_sum / budget_heat;
        factor = 1.0f / (1.0f + k * deferred_util);
    }

    class_factor = 1.0f;
    switch (page_class) {
    case PAGE_HOT:
        class_factor = 0.6f; break; /* merged burning behavior: favor defer */
    case PAGE_WARM:
        class_factor = 1.05f; break; /* warm: be a bit more conservative */
    case PAGE_COLD:
        class_factor = 1.1f; break;  /* cold (heat==0): prefer dump/skip */
    case PAGE_FREEZING:
        class_factor = 1.3f; break;  /* freezing: make defer hard */
    default:
        class_factor = 1.0f; break;
    }

    /* Round-aware adjustment: be more aggressive early, conservative later. */
    if (dl && dl->current_round > 0) {
        int r = dl->current_round;
        if (r <= 1)
            round_factor = 0.85f;
        else if (r == 2)
            round_factor = 0.90f;
        else if (r == 3)
            round_factor = 0.95f;
        else
            round_factor = 1.05f;
    }

    /* PID hotness factor: hotter processes get a lower threshold (more defer budget). */
    hot_factor = dirtymap_pid_hotness_factor(dl);

    p = base * factor * class_factor * round_factor * hot_factor;
    if (p < min_p) p = min_p;
    if (p > max_p) p = max_p;

    return p;
}

/**
 * @brief Phase0优化：更新多级阈值
 *
 * @param dl dirty_log结构
 * @param round 当前轮次
 */
void update_multi_level_thresholds(struct dirty_log *dl, int round) {
    // Phase0优化：使用动态阈值替代固定阈值
    update_dynamic_thresholds(dl, round);
}

/**
 * @brief Phase0优化：分类页面为5个级别（语义修正）
 *
 * 新语义：
 * - PAGE_COLD: heat == 0（真正无活动的冷页）
 * - PAGE_FREEZING: 低热且**强烈降温**（relative trend <= -FREEZING_DECLINE_REL） —— 优先 DUMP（避免把正在降温的页延迟）
 * - PAGE_WARM: 低热但稳定（低热稳定应视为 warm，需要谨慎延迟）
 * - PAGE_HOT: 高热（合并原 PAGE_BURNING 行为）
 *
 * @param dl dirty_log结构
 * @param dhm dirty_diffmap条目（可为NULL）
 * @param round 当前轮次
 * @return page_class_t 页面类别
 */
page_class_t classify_page(struct dirty_log *dl, struct dirty_diffmap *dhm, int round) {
    float heat;

    if (!dl || !dhm)
        return PAGE_COLD; /* treat missing dhm as cold by default */

    heat = dhm->heat;

    // 更新阈值（如果还未设置）
    if (dl->threshold_low == 0.0f || dl->threshold_mid == 0.0f) {
        update_multi_level_thresholds(dl, round);
    }

    /* New freezing detection: use relative trend (trend/heat) to identify actively cooling pages */
    {
        const float FREEZING_DECLINE_REL = 0.6f; /* relative decline threshold (tunable in code) */
        float rel_trend = (dhm->heat > 0.0f) ? dhm->heat_trend / (dhm->heat + 1e-6f) : 0.0f;

        if (heat == 0.0f) {
            return PAGE_COLD;
        } else if (heat < dl->threshold_low) {
            /* Low-heat bucket: decide between FREEZING (actively cooling) vs WARM (low stable) */
            if (rel_trend <= -FREEZING_DECLINE_REL) {
                return PAGE_FREEZING;
            } else {
                return PAGE_WARM;
            }
        } else if (heat < dl->threshold_mid) {
            return PAGE_WARM;
        } else {
            return PAGE_HOT; /* merge burning into hot */
        }
    }
}

/**
 * @brief Phase0优化：分析页面的访问模式
 *
 * @param dl dirty_log结构
 * @param addr 页面地址
 * @return access_pattern_t 访问模式
 */
access_pattern_t analyze_access_pattern(struct dirty_log *dl, unsigned long addr) {
    struct dirty_diffmap *dhm;
    float heat;
    float trend;

    if (!dl || !dl->diffmap)
        return PATTERN_UNKNOWN;

    dhm = search_dirty_map(dl, addr);
    if (!dhm)
        return PATTERN_UNKNOWN;

    heat = dhm->heat;
    trend = dhm->heat_trend;

    // 简化版模式识别
    // 1. STABLE: 热度高但趋势接近0
    if (heat > dl->threshold_mid && fabsf(trend) < (0.15f * heat)) {
        return PATTERN_STABLE;
    }

    // 2. BURST: 短期内急剧升温
    if (trend > 0 && trend > (1.5f * heat)) {
        return PATTERN_BURST;
    }

    // 3. DECLINING: 持续降温
    if (trend < 0 && fabsf(trend) > (0.5f * heat)) {
        return PATTERN_DECLINING;
    }

    // 4. PERIODIC: 需要更多历史数据，暂时无法判断
    // TODO: 实现多窗口历史分析

    return PATTERN_UNKNOWN;
}

/**
 * @brief Phase0优化：收集全局heat统计（Round 1执行）
 *
 * @param dl dirty_log结构
 */
void collect_global_heat_stats(struct dirty_log *dl) {
    unsigned long i;
    float *heats = NULL;
    unsigned long valid_count = 0;
    float sum = 0.0f;

    if (!dl || !dl->diffmap || dl->diffmap_size == 0) {
        pr_warn("[Phase0Stats] Cannot collect stats: diffmap empty\n");
        return;
    }

    // 已经收集过了
    if (dl->stats_collected) {
        return;
    }

    // 分配临时数组存储所有heat值（用于排序）
    heats = malloc(dl->diffmap_size * sizeof(float));
    if (!heats) {
        pr_perror("[Phase0Stats] Failed to allocate memory for heat array");
        return;
    }

    // 收集所有非零heat值
    for (i = 0; i < dl->diffmap_size; i++) {
        float h = dl->diffmap[i].heat;
        if (h > 0.0f) {
            heats[valid_count] = h;
            sum += h;
            if (h > dl->global_max_heat) {
                dl->global_max_heat = h;
            }
            valid_count++;
        }
    }

    if (valid_count == 0) {
        pr_warn("[Phase0Stats] No valid heat data collected\n");
        free(heats);
        return;
    }

    // 计算平均值
    dl->global_mean_heat = sum / (float)valid_count;

    // 排序以计算百分位数
    // 简单冒泡排序（数据量不大）
    for (i = 0; i < valid_count - 1; i++) {
        unsigned long j;
        for (j = 0; j < valid_count - i - 1; j++) {
            if (heats[j] > heats[j + 1]) {
                float temp = heats[j];
                heats[j] = heats[j + 1];
                heats[j + 1] = temp;
            }
        }
    }

    // 计算百分位数
    dl->global_median_heat = heats[valid_count / 2];
    dl->global_p75_heat = heats[(valid_count * 3) / 4];
    dl->global_p90_heat = heats[(valid_count * 9) / 10];

    dl->stats_collected = true;

    // pr_info("[Phase0Stats] PID=%d Heat statistics collected: n=%lu, max=%.2f, mean=%.2f, median=%.2f, p75=%.2f, p90=%.2f\n",
    // 		dl->pid, valid_count, dl->global_max_heat, dl->global_mean_heat,
    // 		dl->global_median_heat, dl->global_p75_heat, dl->global_p90_heat);

    free(heats);
}

/**
 * @brief Phase0优化：使用动态阈值更新分类边界
 *
 * @param dl dirty_log结构
 * @param round 当前轮次
 */
void update_dynamic_thresholds(struct dirty_log *dl, int round) {
    // Phase0: Initialize hot threshold from the first dirtymap median and then
    // dynamically adjust it over subsequent rounds to expand/contract candidate sets.
    float base = 2.0f;
    float aggression;

    if (!dl)
        return;

    /* If we haven't collected global stats yet and have a diffmap, collect them now. */
    if (!dl->stats_collected && dl->diffmap && dl->diffmap_size > 0) {
        collect_global_heat_stats(dl);
    }

    /* Use half the median heat from first dirtymap as base when available; otherwise fall back */
    if (dl->stats_collected && dl->global_median_heat > 0.0f) {
        /* baseline: half of median heat (user requirement: first dirtymap median / 2) */
        base = dl->global_median_heat * 0.5f;
    } else {
        base = INITIAL_HEAT_THRESHOLD; /* fallback when no stats */
    }

    /* Aggression schedule: start at half-median for round 1 and then slightly relax threshold
     * over rounds to allow more pages to become candidates as history accumulates.
     */
    if (round == 1) {
        aggression = 1.0f;  /* threshold_mid = half-median */
    } else if (round == 2) {
        aggression = 0.95f;
    } else if (round == 3) {
        aggression = 0.90f;
    } else if (round == 4) {
        aggression = 0.85f;
    } else {
        aggression = 0.80f;
    }

    /* Compute 3-level thresholds in heat units (writes/sec)
     * low < mid < high
     */
    dl->threshold_mid = base * aggression;
    dl->threshold_low = dl->threshold_mid * 0.3f;
    dl->threshold_high = dl->threshold_mid * 2.5f;

    /* Safety clamps */
    if (dl->threshold_mid < 0.0001f)
        dl->threshold_mid = INITIAL_HEAT_THRESHOLD;

    /* Diagnostic info for troubleshooting */
    pr_info("[Phase0Dynamic] PID=%d Round=%d: base(half_median)=%.6f aggression=%.3f thresholds=[%.6f, %.6f, %.6f]\n",
            dl->pid, round, base, aggression, dl->threshold_low, dl->threshold_mid, dl->threshold_high);
}
// =============================================================================
// Phase0优化：历史趋势分析和保守Skip策略
// =============================================================================

/**
 * 获取页面历史数据（如不存在则创建）
 */
page_history_t *get_page_history(struct dirty_log *dl, unsigned long vaddr)
{
    page_history_t *hist;

    if (!dl || !dl->page_history_map) {
        return NULL;
    }

    hist = g_hash_table_lookup(dl->page_history_map, GSIZE_TO_POINTER(vaddr));
    if (!hist) {
        hist = xzalloc(sizeof(page_history_t));
        if (hist) {
            hist->history_count = 0;
            hist->last_round = -1;
            hist->is_stable = false;
            hist->is_declining = false;
            hist->score_ema = 0.0f; /* init EMA */
            g_hash_table_insert(dl->page_history_map, GSIZE_TO_POINTER(vaddr), hist);
        }
    }

    return hist;
}

/* Push a heat sample into history (rolling FIFO). */
static void push_history_heat(page_history_t *hist, float heat)
{
    int i;
    if (!hist)
        return;
    if (hist->history_count < MAX_HISTORY_ROUNDS) {
        hist->heat[hist->history_count] = heat;
        hist->history_count++;
    } else {
        for (i = 0; i < MAX_HISTORY_ROUNDS - 1; i++) {
            hist->heat[i] = hist->heat[i + 1];
        }
        hist->heat[MAX_HISTORY_ROUNDS - 1] = heat;
    }
}

/**
 * 更新页面历史数据
 * 记录最近 MAX_HISTORY_ROUNDS 轮的 heat（以及写入估计）
 */
void update_page_history(struct dirty_log *dl, unsigned long vaddr, struct dirty_diffmap *dhm, int round)
{
    page_history_t *hist;
    int gap, i;

    if (!dl || !dhm || round < 0) return;

    // 首次使用，创建hash表
    if (!dl->page_history_map) {
        dl->page_history_map = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, free);
    }

    hist = get_page_history(dl, vaddr);
    if (!hist) {
        pr_err("Failed to get page_history_t for vaddr=%lx\n", vaddr);
        return;
    }

    // 允许非连续round：用“缺失轮次=未修改(heat=0)”近似全历史
    if (hist->last_round >= 0) {
        if (round <= hist->last_round) {
            /* round回退或重复，重置历史避免污染 */
            hist->history_count = 0;
        } else {
            gap = round - hist->last_round - 1;
            for (i = 0; i < gap; i++)
                push_history_heat(hist, 0.0f);
        }
    }

    // 滚动存储（FIFO），记录本轮 heat
    push_history_heat(hist, dhm->heat);

    hist->last_round = round;

    // 更新统计特征
    compute_page_stability(hist);

}

/**
 * 计算页面稳定性特征
 * - 均值、方差
 * - 是否稳定（方差小）
 * - 是否持续下降
 */
void compute_page_stability(page_history_t *hist)
{
    int n, i;
    float sum, var_sum, cv;
    int dirty_cnt = 0;

    if (!hist || hist->history_count < 2) {
        hist->is_stable = false;
        hist->is_declining = false;
        return;
    }

    n = hist->history_count;

    // 计算均值 (基于 heat)
    sum = 0.0f;
    for (i = 0; i < n; i++) {
        sum += hist->heat[i];
        if (hist->heat[i] > 0.0f)
            dirty_cnt++;
    }
    hist->mean_heat = sum / n;
    hist->dirty_freq = n > 0 ? (float)dirty_cnt / (float)n : 0.0f;

    // 计算方差 (基于 heat)
    var_sum = 0.0f;
    for (i = 0; i < n; i++) {
        float diff = hist->heat[i] - hist->mean_heat;
        var_sum += diff * diff;
    }
    hist->variance_heat = var_sum / n;

    // 判断稳定性：变异系数 < 0.3
    cv = (hist->mean_heat > 0) ? sqrtf(hist->variance_heat) / hist->mean_heat : 1.0f;
    hist->is_stable = (cv < 0.3f);

    // 判断下降趋势：最近3次为非增序列 (降或持平)
    hist->is_declining = false;
    if (n >= 3) {
        if (hist->heat[n - 3] >= hist->heat[n - 2] && hist->heat[n - 2] >= hist->heat[n - 1])
            hist->is_declining = true;
    }
}

/**
 * Phase0: 基于历史趋势的保守Skip决策
 * 只有满足所有条件才skip：
 * 1. 页面class >= HOT
 * 2. 有足够的历史数据（至少3轮）
 * 3. heat（或写入估计）呈下降或稳定趋势
 * 4. 当前 heat 低于历史平均值
 * 5. Round >= 2（早期不skip，等待历史积累）
 */
bool should_skip_with_history(struct dirty_log *dl, unsigned long vaddr, struct dirty_diffmap *dhm, int round, page_class_t page_class)
{
    page_history_t *hist;
    float track_s = 1.0f;
    float writes_est = 0.0f;

    // 条件1：必须是HOT（合并原 BURNING 行为）
    if (page_class < PAGE_HOT) {
        return false;
    }

    // 条件5：早期round不skip（避免15.6%准确率的灾难）
    if (round < 2) {
        return false;
    }

    // 条件2：必须有历史数据
    if (!dl || !dhm) {
        return false;
    }

    hist = get_page_history(dl, vaddr);
    if (!hist || hist->history_count < 3) {
        return false;
    }

    // 条件3：必须是下降或稳定趋势
    if (!hist->is_stable && !hist->is_declining) {
        return false;
    }

    // 条件4：当前heat必须低于或接近历史平均heat
    // 允许略高于平均（1.2倍以内），因为可能有小幅波动
    if (dhm->heat > hist->mean_heat * 1.2f) {
        return false;
    }

    // 额外条件：如果写入量仍然很高（>20 writes估计），更保守
    track_s = (dl && dl->ldm_header && dl->ldm_header->track_duration_ns > 0) ? ((float)dl->ldm_header->track_duration_ns / 1e9f) : 1.0f;
    writes_est = dhm->heat * track_s;
    if (writes_est > 20.0f) {
        // 要求连续下降
        if (!hist->is_declining) {
            return false;
        }
        // 且当前热度必须显著低于历史平均（0.8倍以内）
        if (dhm->heat > hist->mean_heat * 0.8f) {
            return false;
        }
    }

    // 通过所有检查，可以skip
    return true;
}