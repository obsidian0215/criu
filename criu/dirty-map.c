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

#define MAX_FILES 32

// Comparator函数用于qsort
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
void sort_dirty_map(struct dirty_map *dm, size_t size) {
    if (dm && size > 1)
        qsort(dm, size, sizeof(struct dirty_map), compare_dirty_map);
}

/**
 * @brief 从candidate.pid文件中读取candidate_list
 *
 * @param dirty_map_dir dirty_map目录的路径
 * @param pid 进程pid
 * @param candidate_list 指向存储candidate_list的指针
 * @param candidate_size 指针，存储candidate_list的大小
 * @return int 成功返回0，失败返回-1并设置errno。
 */
static int load_candidate_list(const char *dirty_map_dir, pid_t pid, unsigned long **candidate_list, size_t *candidate_size) {
    char timestamp_file_path[PATH_MAX];
    void *mmaped = NULL;
    size_t current_size, current_count, required_size;
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
    
    // 如果文件大小不是整数倍的 sizeof(int)，修正
    if (current_size % sizeof(unsigned long) != 0) {
        pr_perror("[Obsidian0215]Invalid timestamp_list file size");
        close(fd);
        return -1;
    }
    
    // 需要映射的总大小为 current_count + 1 个 int
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
    mmaped = mmap(NULL, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmaped == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    close(fd);
    
    *timestamp_list = (unsigned long *)mmaped;
    *ts_list_size = current_count;
    
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
static int load_timestamp_list(const char *dirty_map_dir, pid_t pid, unsigned long **timestamp_list, size_t *ts_list_size) {
    char timestamp_file_path[PATH_MAX];
    void *mmaped = NULL;
    size_t current_size, current_count, required_size;
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
    
    // 如果文件大小不是整数倍的 sizeof(int)，修正
    if (current_size % sizeof(unsigned long) != 0) {
        pr_perror("[Obsidian0215]Invalid timestamp_list file size");
        close(fd);
        return -1;
    }
    
    // 需要映射的总大小为 current_count + 1 个 int
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
    mmaped = mmap(NULL, required_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmaped == MAP_FAILED) {
        perror("mmap");
        close(fd);
        return -1;
    }
    close(fd);
    
    *timestamp_list = (unsigned long *)mmaped;
    *ts_list_size = current_count;
    
    return 0;
}

/**
 * @brief 检查指定的时间戳是否存在于timestamp_list中
 *
 * @param timestamp_list 已映射的时间戳列表指针。
 * @param ts_list_size 时间戳列表的大小。
 * @param timestamp 要检查的时间戳。
 * @return int 返回 1 表示存在，0 表示不存在。
 */
static int is_timestamp_in_list(unsigned long *timestamp_list, size_t ts_list_size, unsigned long timestamp) {
    // 如果timestamp_list为空直接覆盖ts_list内存的原有值
    if (!ts_list_size)
        return 0;
    
    for (size_t i = 0; i < ts_list_size; ++i) {
        if (timestamp_list[i] == timestamp) {
            return 1; // 存在
        }
    }
    return 0; // 不存在
}

/**
 * @brief 将新的时间戳追加到已映射的 timestamp_list 中。
 *
 * @param timestamp_list 已映射的时间戳列表指针，包含预留的一个 int 空间。
 * @param ts_list_size 指向当前时间戳数量的指针。
 * @param new_timestamp 要追加的新的时间戳。
 * @return int 成功返回 0，失败返回 -1 并设置 errno。
 */
static int append_timestamp_to_list(unsigned long *timestamp_list, size_t *ts_list_size, unsigned long new_timestamp) {
    // 将 new_timestamp 写入预留的空间
    timestamp_list[*ts_list_size] = new_timestamp;
    
    // 增加时间戳数量
    (*ts_list_size)++;
    
    // 同步更改到文件
    if (msync(timestamp_list, (*ts_list_size) * sizeof(unsigned long), MS_SYNC) == -1) {
        perror("msync");
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
                struct dirty_map **dm, unsigned long *dm_size) {
    char dm_filepath[PATH_MAX];
    int fd;
    struct stat st;
    void *mapped;
    if (timestamp == 0) {
        *dm = NULL;
        *dm_size = 0;
        return 0;
    }
    
    snprintf(dm_filepath, sizeof(dm_filepath), "%s/%d-%lu.dirtymap", dirty_map_dir, pid, timestamp);
    
    fd = open(dm_filepath, O_RDONLY);
    if (fd == -1) {
        pr_perror("Error opening dirtymap file %s", dm_filepath);
        return -1;
    }
    
    if (fstat(fd, &st) == -1) {
        pr_perror("Error getting size of %s", dm_filepath);
        close(fd);
        return -1;
    }
    
    if (st.st_size == 0) {
        pr_perror("Dirtymap file %s is empty", dm_filepath);
        close(fd);
        return -1;
    }
    
    mapped = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        pr_perror("Error mapping dirtymap file %s", dm_filepath);
        close(fd);
        return -1;
    }
    
    close(fd);
    *dm = (struct dirty_map *)mapped;
    *dm_size = st.st_size / sizeof(struct dirty_map);
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
struct dirty_diffmap* merge_dirty_maps(struct dirty_map *latest_dm, size_t latest_size,
        struct dirty_map *less_latest_dm, size_t less_latest_size, size_t *diffmap_size) {
    size_t max_size, i = 0, j = 0, k = 0;
    struct dirty_diffmap *diffmap, *resized_diffmap;

    // 两个dirty_map都为空，直接返回空diffmap
    if ((latest_dm == NULL || latest_size == 0) && (less_latest_dm == NULL || less_latest_size == 0)) {
        *diffmap_size = 0;
        return NULL;
    }

    // 估算diffmap的最大可能大小
    if (latest_dm && less_latest_dm)
        max_size = latest_size + less_latest_size;
    else if (latest_dm)
        max_size = latest_size;
    else
        max_size = less_latest_size;

    diffmap = malloc(max_size * sizeof(struct dirty_diffmap));
    if (!diffmap) {
        pr_perror("[Obsidian0215] Failed to allocate memory for diffmap");
        // exit(EXIT_FAILURE);
        return NULL;
    }

    if (latest_dm && latest_size > 0 && less_latest_dm && less_latest_size > 0) {
        while (i < latest_size && j < less_latest_size) {
            if (latest_dm[i].address < less_latest_dm[j].address) {
                // 仅在latest_dm中存在
                diffmap[k].address = latest_dm[i].address;
                diffmap[k].heat_level = (latest_dm[i].write_count);
                diffmap[k].heat_trend = (latest_dm[i].write_count);
                i++;
            }
            else if (latest_dm[i].address > less_latest_dm[j].address) {
                // 仅在less_latest_dm中存在
                diffmap[k].address = less_latest_dm[j].address;
                diffmap[k].heat_level = 0;
                diffmap[k].heat_trend = -(less_latest_dm[j].write_count);
                j++;
            }
            else {
                // 同时存在于两个数组中
                diffmap[k].address = latest_dm[i].address;
                diffmap[k].heat_level = (latest_dm[i].write_count);
                diffmap[k].heat_trend = latest_dm[i].write_count - less_latest_dm[j].write_count;
                i++;
                j++;
            }
            k++;
        }

        // 处理剩余的 latest_dm 条目
        while (i < latest_size) {
            diffmap[k].address = latest_dm[i].address;
            diffmap[k].heat_level = (latest_dm[i].write_count);
            diffmap[k].heat_trend = (latest_dm[i].write_count);
            i++;
            k++;
        }

        // 处理剩余的 less_latest_dm 条目
        while (j < less_latest_size) {
            diffmap[k].address = less_latest_dm[j].address;
            diffmap[k].heat_level = 0;
            diffmap[k].heat_trend = -(less_latest_dm[j].write_count);
            j++;
            k++;
        }
    } else if (latest_dm && latest_size > 0) {
        // 仅latest_dm存在，less_latest_dm为空
        for (; i < latest_size; i++, k++) {
            diffmap[k].address = latest_dm[i].address;
            diffmap[k].heat_level = latest_dm[i].write_count;
            diffmap[k].heat_trend = latest_dm[i].write_count;
        }
    } else if (less_latest_dm && less_latest_size > 0) {
        // 仅less_latest_dm存在，latest_dm为空
        for (; j < less_latest_size; j++, k++) {
            diffmap[k].address = less_latest_dm[j].address;
            diffmap[k].heat_level = 0;
            diffmap[k].heat_trend = -(less_latest_dm[j].write_count);
        }
    }

    // 更新实际的diffmap大小
    *diffmap_size = k;

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
// static void debug_show_dirtymap(struct dirty_map *dirtymap, size_t dirtymap_size, pid_t pid) {
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
static void debug_show_diffmap(struct dirty_diffmap *diffmap, size_t diffmap_size, pid_t pid) {
    int i;
    
    if (pr_quelled(LOG_DEBUG) || !diffmap || !diffmap_size)
		return;
    
    pr_debug("Diffmap for pid %d:(size: %ld)\n", pid, diffmap_size);
	for (i = 0; i < diffmap_size; i++) {
		pr_debug("\taddress: %#lx, heat level: %d, heat trend: %d\n", 
            diffmap[i].address, diffmap[i].heat_level, diffmap[i].heat_trend);
	}
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
    
    // 读取timestamp_list.<pid> 文件，初始化timestamp_list和ts_list_size
    ret = load_timestamp_list(dirty_map_dir, pid, &dl->timestamp_list, &dl->ts_list_size);
    if (ret < 0) {
        pr_perror("[Obsidian0215]Failed to read timestamp_list for pid %d", pid);
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
            pr_perror("[Obsidian0215]Error stoping dirty-track for pid %d", pid);
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
                          &dl->latest_dm, &dl->ldm_size);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Failed to map latest dirtymap for pid %d", pid);
            dl->latest_dm = NULL;
            dl->ldm_size = 0;
        } else
            pr_info("[Obsidian0215]successfully loaded %d's latest dirty-map (size: %lu bytes): %p\n", 
                    pid, dl->ldm_size * sizeof(struct dirty_map), dl->latest_dm);
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
                          &dl->less_latest_dm, &dl->lldm_size);
        if (ret < 0) {
            pr_perror("[Obsidian0215]Failed to map less latest dirtymap for pid %d", pid);
            dl->less_latest_dm = NULL;
            dl->lldm_size = 0;
        } else
            pr_info("[Obsidian0215]successfully loaded %d's less-latest dirty-map (size: %lu bytes): %p\n",
                     pid, dl->lldm_size * sizeof(struct dirty_map), dl->less_latest_dm);
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
    // sort_dirty_map(latest_dm, latest_size);
    // sort_dirty_map(less_latest_dm, less_latest_size);
    dl->diffmap = merge_dirty_maps(
        dl->latest_dm, dl->ldm_size,
        dl->less_latest_dm, dl->lldm_size,
        &(dl->diffmap_size)
    );

    debug_show_diffmap(dl->diffmap, dl->diffmap_size, pid);
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
    
    if (dl) {
        // 关闭打开的dirty-track设备fd
        if (dl->dirty_track_fd) {
            close(dl->dirty_track_fd);
            dl->dirty_track_fd = -1;
        }

        // 卸载最新的dirtymap
        if (dl->latest_dm) {
            if (munmap(dl->latest_dm, dl->ldm_size) == -1) {
                pr_perror("[Obsidian0215]Error unmapping latest dirtymap");
            }
            dl->latest_dm = NULL;
            dl->ldm_size = 0;
        }

        // 处理次新的dirtymap
        if (dl->less_latest_dm) {
            if (munmap(dl->less_latest_dm, dl->lldm_size) == -1) {
                pr_perror("[Obsidian0215]Error unmapping second latest dirtymap");
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

int search_candidate_list(struct dirty_log *dl, unsigned long addr) {
    unsigned long *cd_list, left = 0, right = 0;

    // candidate_list为空，无法找到该地址
    if (!dl)
        return 0;
    else {
        cd_list = dl->candidate_list;
        right = dl->candidate_size;
        if (!map || !right)
            return 0;
    }

    while (left < right) {
        unsigned long mid = left + (right - left) / 2;

        if (addr < cd_list[mid]) {
            // 地址在当前范围左侧，缩小右边界
            right = mid;
        } else if (addr > cd_list[mid]) {
            // 地址在当前范围右侧，缩小左边界
            left = mid + 1;
        } else {
            // 找到该地址
            return 1;
        }
    }

    // 未找到该地址
    return 0;

}