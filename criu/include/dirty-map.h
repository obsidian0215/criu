#ifndef __CR_DIRTY_MAP_H__
#define __CR_DIRTY_MAP_H__

#include <stdbool.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <endian.h> // 用于字节序转换
#include <glib.h>
#include <pthread.h>
#include "int.h"
#include "pid.h"
#include "page.h"
#include "pagemap-cache.h"
#include "dirty-cache.h"  // 引入脏页缓存

// dirty-track LKM definitions
#define DIRTY_TRACK_MAGIC 'd'
#define IOCTL_SET_DIRTY_MAP_PATH _IOW(DIRTY_TRACK_MAGIC, 1, char[256])
#define IOCTL_START_PID _IOW(DIRTY_TRACK_MAGIC, 2, pid_t)
#define IOCTL_STOP_PID _IOW(DIRTY_TRACK_MAGIC, 3, pid_t)
#define IOCTL_CHECK_PID _IOWR(DIRTY_TRACK_MAGIC, 4, struct pid_check)
#define IOCTL_GET_DIRTY_MAP_PATH _IOR(DIRTY_TRACK_MAGIC, 5, char[256])

#define DT_DEV_PATH "/dev/dirty-track"

#define INITIAL_HEAT_THRESHOLD 5.0
#define INITIAL_TREND_THRESHOLD 0.0

// 优化1：指数移动平均和平滑参数
#define EMA_ALPHA 0.3f                    // 指数移动平均平滑因子
#define MAX_ADJUSTMENT_STEP 2.0f         // 最大调整步长
#define MIN_ADJUSTMENT_STEP 0.1f         // 最小调整步长
#define HISTORY_BUFFER_SIZE 10            // 历史数据缓冲区大小

// 优化1：反馈控制参数
#define TARGET_WARM_RATIO 0.05f          // 目标温页比例 (5%)
#define KP_DEFAULT 0.1f                  // PID控制器比例系数
#define KI_DEFAULT 0.01f                 // PID控制器积分系数
#define KD_DEFAULT 0.05f                 // PID控制器微分系数

// 检查pid是否被dirty-track中
struct pid_check {
    pid_t pid;
    bool is_tracked;
};

// 纪录被选择传输的页地址和次数
typedef struct __attribute__((__packed__)) warm_page
{
    unsigned long address;
    char s_count;  // 被选择转储的次数
} warm_page_t;

// 脏页heatmap信息
typedef struct __attribute__((__packed__)) dirty_diffmap
{
    unsigned long address;          // 页地址
    float heat;       // 热度
    float heat_trend;                // 热度变化
} ddm_t;

// 脏页dirtymap信息
typedef struct __attribute__((__packed__)) dirty_map
{
	unsigned long address;
    unsigned int write_count;
} dm_t;

typedef struct {
    u64 track_duration_ns;
} dirtymap_header_t;

// Phase 1优化：历史统计信息结构
typedef struct {
    float hit_rates[HISTORY_BUFFER_SIZE];           // 历史命中率
    float threshold_history[HISTORY_BUFFER_SIZE];   // 历史阈值
    float adjustment_factors[HISTORY_BUFFER_SIZE];  // 历史调整因子
    int history_idx;                                 // 历史数据索引
    float ema_hit_rate;                             // 指数移动平均命中率
    float learning_rate;                            // 学习率
    float momentum_factor;                          // 动量因子
    int update_count;                               // 更新次数
} historical_stats_t;

// Phase 1优化：反馈控制结构
typedef struct {
    float target_warm_ratio;                        // 目标温页比例
    float adjustment_step;                          // 当前调整步长
    float integral_error;                           // 积分误差
    float prev_error;                               // 上一轮误差
    float kp, ki, kd;                               // PID控制器参数
    unsigned int new_warm_count;                    // 新温页数量
    unsigned int total_warm_count;                  // 总温页数量
} feedback_controller_t;

struct dirty_log {
    pid_t pid;
    int dirty_track_fd;     // dirty-track设备文件描述符
    struct dirty_diffmap *diffmap;
    unsigned long diffmap_size;

    // dirty-map and corresponding timestamp(file)
    struct {
        unsigned long *timestamp_list;
        unsigned long ts_list_size;

        unsigned long latest_timestamp;
        struct dirty_map *latest_dm;
	    unsigned long ldm_size;
        dirtymap_header_t *ldm_header;

        unsigned long less_latest_timestamp;
        struct dirty_map *less_latest_dm;
	    unsigned long lldm_size;
        dirtymap_header_t *lldm_header;
    };

    // list for addresses of warm pages in pre-dump
    pthread_mutex_t warm_list_mutex;  // 互斥锁保护warm_list及相关字段
    // warm_page_t *warm_list;
    GTree *warm_list;
    unsigned long warm_size;

    // thresholds for warm page selection
    float heat_threshold;
    float trend_threshold;
    float min_heat;

    // 脏页缓存 - 每个进程独享
    dirty_cache_t *page_cache;       // 页面缓存实例
    bool cache_enabled;              // 缓存启用状态

    // 优化1：历史统计和学习
    historical_stats_t *historical_stats;           // 历史统计信息
    feedback_controller_t *feedback_ctrl;           // 反馈控制
};

#define INIT_DIRTY_LOG(log) do { \
    (log).pid = -1; \
    (log).dirty_track_fd = -1; \
    (log).timestamp_list = NULL; \
    (log).ts_list_size = 0; \
    (log).latest_timestamp = 0; \
    (log).latest_dm = NULL; \
    (log).ldm_size = 0; \
    (log).less_latest_timestamp = 0; \
    (log).less_latest_dm = NULL; \
    (log).lldm_size = 0; \
    (log).diffmap = NULL; \
    (log).diffmap_size = 0; \
    (log).warm_list = NULL; \
    (log).warm_size = 0; \
    (log).ldm_header = NULL; \
    (log).lldm_header = NULL; \
    (log).page_cache = NULL; \
    (log).cache_enabled = false; \
    (log).historical_stats = NULL; \
    (log).feedback_ctrl = NULL; \
} while (0)

#define INIT_DIRTY_LOG_PTR(log_ptr) do { \
    (log_ptr)->pid = -1; \
    (log_ptr)->dirty_track_fd = -1; \
    (log_ptr)->timestamp_list = NULL; \
    (log_ptr)->ts_list_size = 0; \
    (log_ptr)->latest_timestamp = 0; \
    (log_ptr)->latest_dm = NULL; \
    (log_ptr)->ldm_size = 0; \
    (log_ptr)->less_latest_timestamp = 0; \
    (log_ptr)->less_latest_dm = NULL; \
    (log_ptr)->lldm_size = 0; \
    (log_ptr)->diffmap = NULL; \
    (log_ptr)->diffmap_size = 0; \
    (log_ptr)->warm_list = NULL; \
    (log_ptr)->warm_size = 0; \
    (log_ptr)->ldm_header = NULL; \
    (log_ptr)->lldm_header = NULL; \
    (log_ptr)->page_cache = NULL; \
    (log_ptr)->cache_enabled = false; \
    (log_ptr)->historical_stats = NULL; \
    (log_ptr)->feedback_ctrl = NULL; \
} while (0)

int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir);
void fini_dirty_map(struct pstree_item *item);
int init_dirty_track(struct dirty_log *dl);
int start_dirty_track(struct dirty_log* dl);
int check_dirty_track(struct dirty_log* dl, struct pid_check *pc);
int stop_dirty_track(struct dirty_log* dl);
struct dirty_diffmap *search_dirty_map(struct dirty_log *dl, unsigned long addr);
int search_warm_list(struct dirty_log *dl, unsigned long addr);
void inc_warm_list(struct dirty_log *dl, unsigned long addr);
void sub_warm_list(struct dirty_log *dl, unsigned long addr, bool zero);

/* 脏页缓存集成API - 每个进程独享 */
int init_dirty_cache(struct dirty_log *dl, const char *cache_dir);
void fini_dirty_cache(struct dirty_log *dl);
int dirty_cache_lookup_page(struct dirty_log *dl, unsigned long vaddr, void *page_out);
void dirty_cache_update_page(struct dirty_log *dl, unsigned long vaddr, const void *page_data);
bool is_dirty_cache_enabled(struct dirty_log *dl);

#endif