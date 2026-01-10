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

// dirty-track LKM definitions
#define DIRTY_TRACK_MAGIC 'd'
#define IOCTL_SET_DIRTY_MAP_PATH _IOW(DIRTY_TRACK_MAGIC, 1, char[256])
#define IOCTL_START_PID _IOW(DIRTY_TRACK_MAGIC, 2, pid_t)
#define IOCTL_STOP_PID _IOW(DIRTY_TRACK_MAGIC, 3, pid_t)
#define IOCTL_CHECK_PID _IOWR(DIRTY_TRACK_MAGIC, 4, struct pid_check)
#define IOCTL_GET_DIRTY_MAP_PATH _IOR(DIRTY_TRACK_MAGIC, 5, char[256])

#define DT_DEV_PATH "/dev/dirty-track"

#define INITIAL_HEAT_THRESHOLD 10.0
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

// 页面访问模式 (Phase 1优化)
enum page_access_pattern {
    PATTERN_UNKNOWN = 0,
    PATTERN_COLD,       // 几乎不被修改
    PATTERN_COLD_STABLE,// 稳定不被修改（用于可靠剔除）
    PATTERN_WARMING,    // 修改频率正在上升
    PATTERN_HOT_STABLE, // 持续高频修改（应跳过，final dump传输）
    PATTERN_COOLING,    // 修改频率正在下降
    PATTERN_OSCILLATING,// 访问模式不稳定（波动极大）
};

// Predump 阶段划分 (Phase 1优化)
enum predump_phase {
    PHASE_INITIAL,      // 初始阶段（iteration 1-2）
    PHASE_OPTIMIZATION, // 优化阶段（iteration 3 到 N-1）
    PHASE_CONVERGENCE,  // 收敛阶段（最后1-2次）
};

// 纪录被选择传输的页地址和次数
typedef struct __attribute__((__packed__)) warm_page
{
    unsigned long address;
    char s_count;             // 累计被选择转储的次数（保留兼容性）
    char consecutive_count;   // 连续出现次数（核心指标）
    char last_seen_iter;      // 最后一次出现的迭代轮次
    char consecutive_cooling; // 连续变冷次数 (优化4)
} warm_page_t;

// [Obsidian0215] VMA 级统计信息（用于分区块局部优化）
typedef struct {
    unsigned long start;
    unsigned long end;

    // 当前 VMA 的命中率统计 (Temporary)
    unsigned int tmp_hit;
    unsigned int tmp_miss;

    // 平滑统计量
    float current_ema_hit_rate;
    struct historical_stats *historical_stats;
} vma_stats_t;

// 脏页diffmap信息
typedef struct __attribute__((__packed__)) dirty_diffmap
{
    unsigned long address;          // 页地址
    float heat;                     // 热度
    float heat_trend;               // 热度变化

    // 访问模式相关统计
    float volatility;               // 热度波动（标准化）
    unsigned int consecutive_appear;// 连续出现在diffmap中的次数
    unsigned int total_appear;      // 累计出现次数
    enum page_access_pattern pattern; // 分类模式
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

// Phase1 优化：历史统计信息收集
typedef struct historical_stats {
    float ema_hit_rate;                             // 平滑命中率 (EMA)
    float last_heat_threshold;                      // 上一轮使用的热度阈值
    float last_adjustment_factor;                   // 上一轮计算的调整因子
    float learning_rate;                            // 自适应学习率 (基于命中率波动)
    float ema_alpha;                                // EMA alpha 系数
    float adjustment_factors[HISTORY_BUFFER_SIZE];  // 历史调整因子缓冲区
    int history_idx;                                // 缓冲区当前索引
    float momentum_factor;                          // 动量因子
    int update_count;                               // 更新次数
    float hit_rates[HISTORY_BUFFER_SIZE];           // 历史命中率缓冲区
    float threshold_history[HISTORY_BUFFER_SIZE];   // 历史阈值缓冲区
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

    // diffmap and corresponding timestamp(file)
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
    unsigned long search_cursor;      // [实验] 顺序扫描游标
    // warm_page_t *warm_list;
    GTree *warm_list;
    unsigned long warm_size;
    unsigned long warm_max;

    GTree *skip_list;                   // [Obsidian0215] 被跳过的页地址 (addr -> NULL)
    unsigned long skip_size;

    // [Obsidian0215] Phase 1 性能优化和预测指标
    int current_predump_iter;           // 当前 pre-dump 迭代轮次
    float heat_threshold;               // 当前全局热度阈值 (Dynamic)
    float trend_threshold;              // 当前全局趋势阈值 (Dynamic)
    float min_heat;                     // 热度底噪阈值 (Static, derived from duration)
    float current_ema_hit_rate;         // 当前全局 EMA 命中率

    // [Obsidian0215] 收敛和反馈控制
    float warm_set_overlap_ratio;       // 工作集重叠率（收敛判定核心指标）
    unsigned long prev_warm_size;       // 上一轮 warm_list 的大小
    int max_consecutive_scount;         // 历史观察到的最大连续出现次数
    historical_stats_t *historical_stats;
    feedback_controller_t *feedback_ctrl;
    GTree *vma_stats_tree;              // VMA 粒度的统计信息树 (GTree<start_addr, vma_stats_t>)
};

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
    (log_ptr)->historical_stats = NULL; \
    (log_ptr)->feedback_ctrl = NULL; \
    (log_ptr)->vma_stats_tree = NULL; \
    (log_ptr)->current_predump_iter = 0; \
    (log_ptr)->search_cursor = 0; \
} while (0)

struct pstree_item;
struct vm_area_list;

int init_dirty_map(struct pstree_item *item, struct vm_area_list *vmas, const char *dirty_map_dir);
void fini_dirty_map(struct pstree_item *item);
int init_dirty_track(struct dirty_log *dl);
int start_dirty_track(struct dirty_log* dl);
int check_dirty_track(struct dirty_log* dl, struct pid_check *pc);
int stop_dirty_track(struct dirty_log* dl);
struct dirty_diffmap *search_dirty_map(struct dirty_log *dl, unsigned long addr);
int search_warm_list(struct dirty_log *dl, unsigned long addr);
warm_page_t *search_warm_list_entry(struct dirty_log *dl, unsigned long addr);
void inc_warm_list(struct dirty_log *dl, unsigned long addr);
void sub_warm_list(struct dirty_log *dl, unsigned long addr, bool zero);
void inc_skip_list(struct dirty_log *dl, unsigned long addr);

// Convergence and timing helpers
enum predump_phase get_predump_phase(struct dirty_log *dl, int max_predumps);
void update_warm_set_overlap(struct dirty_log *dl);
int get_max_consecutive_scount(struct dirty_log *dl);
int export_convergence_metrics(struct dirty_log *dl, const char *dirty_map_dir);

#endif
