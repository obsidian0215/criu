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

#define INITIAL_HEAT_THRESHOLD 5.0
#define INITIAL_TREND_THRESHOLD 0.10f  /* require non-trivial trend threshold to avoid any negative tiny delta being considered cooling */

// 优化1：指数移动平均和平滑参数
#define DM_EMA_ALPHA 0.3f                 // 指数移动平均平滑因子 (renamed to avoid symbol collision)
#define MAX_ADJUSTMENT_STEP 2.0f         // 最大调整步长
#define MIN_ADJUSTMENT_STEP 0.1f         // 最小调整步长
#define HISTORY_BUFFER_SIZE 10            // 历史数据缓冲区大小

// 优化1：反馈控制参数
#define TARGET_WARM_RATIO 0.05f          // 目标温页比例 (5%)
#define KP_DEFAULT 0.1f                  // PID控制器比例系数
#define KI_DEFAULT 0.01f                 // PID控制器积分系数
#define KD_DEFAULT 0.05f                 // PID控制器微分系数

// Warm promotion & throttle tuning
/* NOTE: `WARM_PROMOTE_REQUIRED_ROUNDS` and the env override `CRIU_ENABLE_WARM_PROMOTE` have been removed
 * from the runtime test grid and environment overrides to simplify the search space. Promotion still
 * occurs using an internal fixed observation window (3 rounds) and is driven by the heat/trend/history
 * logic in `dirty-map.c` (not tunable via env).
 */
/* delta_writes removed; use heat-only logic */
#define WARM_HEAT_THRESHOLD 1.0f         // Max heat (writes/sec) allowed for promotion (normalized)
#define WARM_MAX_RATIO 0.05f             // If warm_size > diffmap_size * WARM_MAX_RATIO, apply throttling
#define WARM_THROTTLE_BATCH_RATIO 0.25f  // Fraction of warm entries to remove when throttling (min 1)

/* Warm score tuning and promotion thresholds */
#define WARM_SCORE_ALPHA 0.20f           // EMA alpha for warm_score update (0..1)
#define WARM_PROMOTE_THRESHOLD 1.00f     // warm_score threshold to consider immediate promotion
#define WARM_DEMOTE_THRESHOLD 0.00f      // warm_score demotion threshold (if score falls below this, candidate is weak)


// Phase2优化：细粒度页面分类（4级分类）
typedef enum {
    PAGE_FREEZING = 0,   // 冰冷页：heat==0 或长期未访问
    PAGE_COLD = 1,       // 冷页：热度为0（真正冷页）
    PAGE_WARM = 2,       // 温页：低热但稳定（谨慎延迟）
    PAGE_HOT = 3         // 热页：高热（合并原 BURNING 行为）
} page_class_t;

#define NUM_PAGE_CLASSES 4

// Phase2优化：页面访问模式
typedef enum {
    PATTERN_UNKNOWN = 0,
    PATTERN_STABLE = 1,    // 稳定：variance小
    PATTERN_BURST = 2,     // 突发：短期内急剧升温
    PATTERN_PERIODIC = 3,  // 周期性：有规律的hot/cold切换
    PATTERN_DECLINING = 4  // 衰减：持续降温
} access_pattern_t;

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

#define ENABLE_HOT_DEFER 1
#define MIN_DEFER_COOLDOWN_ROUNDS 2
#define MIN_DEFER_COUNT_FOR_ENFORCE 2
typedef struct __attribute__((__packed__)) deferred_page
{
    unsigned long address;
    unsigned char count;  // 连续延迟轮数
} deferred_page_t;

// Phase4优化：历史趋势数据（每页最多记录最近N轮）
#define MAX_HISTORY_ROUNDS 5
typedef struct {
    float heat[MAX_HISTORY_ROUNDS];        // 最近N轮的heat (normalized writes/sec)
    int history_count;                      // 实际存储的历史轮数
    int last_round;                         // 上次更新的round号

    // 趋势统计 (基于 heat)
    float mean_heat;                        // heat均值
    float variance_heat;                    // heat方差
    bool is_stable;                         // 是否稳定（方差小）
    bool is_declining;                      // 是否持续下降（heat持续下降）

    /* EMA of decision score (for smoothing). Updated by decision logic. */
    float score_ema;                        // 指数移动平均的 score（初始为0）

    /* warm_score: EMA of 'cooled' evidence. Positive values indicate evidence page is cooling
     * and thus a candidate for promotion to warm_list. Updated each round using:
     *   warm_score = (1 - alpha) * warm_score + alpha * evidence
     * with `alpha` controlled by `WARM_SCORE_ALPHA` (default 0.2).
     */
    float warm_score;
} page_history_t;

// 脏页heatmap信息 (delta_writes removed; use heat only)
typedef struct __attribute__((__packed__)) dirty_diffmap
{
    unsigned long address;          // 页地址
    float heat;       // 热度 (writes/sec)
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
    unsigned long warm_pruned;
    unsigned long warm_decayed;

    /* Multi-round warm candidate cache: addresses seen cooling in previous rounds */
    pthread_mutex_t warm_cand_mutex;
    GHashTable *warm_candidates_prev; /* keys: GSIZE_TO_POINTER(addr) */
    GHashTable *warm_candidates_prev2; /* previous-previous round candidates (for 3-round promotion) */

    // 延迟转储页集合（pre-dump 中跳过的热页）
    pthread_mutex_t deferred_list_mutex;
    GHashTable *deferred_list;       // 页地址集合 (key: vaddr)
    GHashTable *deferred_protected_until; // Key: vaddr, Value: protected-until round (inclusive)
    int current_round;               // 当前 pre-dump 轮次（用于冷却保护）
    unsigned long deferred_size;

    /* 分级延迟队列：为每个页面分类维护 FIFO 队列，用于基于类的分批出队 */
    GQueue *deferred_queues[NUM_PAGE_CLASSES];

    /* 估计风险度量：被延迟页的 heat 之和（用于自适应软预算，避免基于页数的硬上限） */
    float deferred_heat_sum;

    /* urgent force list: addresses selected for forced dump in current pre-dump */
    pthread_mutex_t urgent_force_mutex;
    GHashTable *urgent_force; /* keys: GSIZE_TO_POINTER(addr) */

    /* deferred_ever: set of addresses that were deferred at least once during this run
     * value: GINT_TO_POINTER(flags) where (flags & 1) == 1 means it was later sent after being deferred
     * These counters are used to compute final decision correctness (long-term metric).
     */
    GHashTable *deferred_ever; /* keys: GSIZE_TO_POINTER(addr) */
    unsigned long deferred_unique_total; /* number of distinct addresses deferred */
    unsigned long deferred_sent_after_defer; /* number of those addresses that were later sent */

    /* Adaptive runtime parameters (updated automatically each iteration)
     * - adaptive_p_base: adaptive base probability threshold used by decision_p_threshold
     * - adaptive_soft_ratio: fraction of total heat allowed to be deferred (soft budget)
     * - adaptive_max_defer[class]: per-class max defer rounds (can change over time)
     */
    float adaptive_p_base;
    float adaptive_soft_ratio;
    int adaptive_max_defer[NUM_PAGE_CLASSES];

    // 预测准确率统计（deferred 预测）
    unsigned long predicted_total;
    unsigned long predicted_hit;
    unsigned long predicted_miss;

    // thresholds for warm page selection
    float heat_threshold;
    float trend_threshold;
    float min_heat;

    // Phase2优化：多级阈值
    float threshold_low;      // 冷/温边界
    float threshold_mid;      // 温/热边界
    float threshold_high;     // 热/炽热边界

    // Phase3优化：动态阈值统计（Round 1收集）
    float global_max_heat;        // 全局最大heat
    float global_mean_heat;       // 全局平均heat
    float global_median_heat;     // 全局中位数heat
    float global_p75_heat;        // 75分位数heat
    float global_p90_heat;        // 90分位数heat
    bool stats_collected;         // 是否已收集统计

    // Phase4优化：历史趋势数据存储（地址 → page_history_t*）
    GHashTable *page_history_map;  // 页地址 → 历史数据映射

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
    (log).warm_pruned = 0; \
    (log).warm_decayed = 0; \
    (log).warm_candidates_prev = NULL; \
    (log).warm_candidates_prev2 = NULL; \
    (log).ldm_header = NULL; \
    (log).lldm_header = NULL; \
    (log).historical_stats = NULL; \
    (log).feedback_ctrl = NULL; \
    (log).deferred_heat_sum = 0.0f; \
    (log).urgent_force = NULL; \
    (log).deferred_ever = NULL; \
    (log).deferred_unique_total = 0; \
    (log).deferred_sent_after_defer = 0; \
    (log).adaptive_p_base = 0.60f; \
    (log).adaptive_soft_ratio = 0.05f; \
    (log).adaptive_max_defer[0] = 0; \
    (log).adaptive_max_defer[1] = 1; \
    (log).adaptive_max_defer[2] = 2; \
    (log).adaptive_max_defer[3] = 10; \
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
    (log_ptr)->warm_pruned = 0; \
    (log_ptr)->warm_decayed = 0; \
    (log_ptr)->deferred_list = NULL; \
    (log_ptr)->deferred_size = 0; \
    (log_ptr)->deferred_queues[0] = NULL; \
    (log_ptr)->deferred_queues[1] = NULL; \
    (log_ptr)->deferred_queues[2] = NULL; \
    (log_ptr)->deferred_queues[3] = NULL; \
    (log_ptr)->predicted_total = 0; \
    (log_ptr)->predicted_hit = 0; \
    (log_ptr)->predicted_miss = 0; \
    (log_ptr)->ldm_header = NULL; \
    (log_ptr)->lldm_header = NULL; \
    (log_ptr)->threshold_low = 0.0f; \
    (log_ptr)->threshold_mid = 0.0f; \
    (log_ptr)->threshold_high = 0.0f; \
    (log_ptr)->global_max_heat = 0.0f; \
    (log_ptr)->global_mean_heat = 0.0f; \
    (log_ptr)->global_median_heat = 0.0f; \
    (log_ptr)->global_p75_heat = 0.0f; \
    (log_ptr)->global_p90_heat = 0.0f; \
    (log_ptr)->stats_collected = false; \
    (log_ptr)->page_history_map = NULL; \
    (log_ptr)->historical_stats = NULL; \
    (log_ptr)->feedback_ctrl = NULL; \
    (log_ptr)->deferred_heat_sum = 0.0f; \
    (log_ptr)->urgent_force = NULL; \
    (log_ptr)->deferred_protected_until = NULL; \
    (log_ptr)->current_round = -1; \
    (log_ptr)->deferred_ever = NULL; \
    (log_ptr)->deferred_unique_total = 0; \
    (log_ptr)->deferred_sent_after_defer = 0; \
    (log_ptr)->adaptive_p_base = 0.60f; \
    (log_ptr)->adaptive_soft_ratio = 0.05f; \
    (log_ptr)->adaptive_max_defer[0] = 0; \
    (log_ptr)->adaptive_max_defer[1] = 1; \
    (log_ptr)->adaptive_max_defer[2] = 2; \
    (log_ptr)->adaptive_max_defer[3] = 10; \
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
int search_deferred_list(struct dirty_log *dl, unsigned long addr);
int get_deferred_count(struct dirty_log *dl, unsigned long addr);
void add_deferred_list(struct dirty_log *dl, unsigned long addr, int round);
void del_deferred_list(struct dirty_log *dl, unsigned long addr);

// Phase2优化：页面分类和访问模式分析
page_class_t classify_page(struct dirty_log *dl, struct dirty_diffmap *dhm, int round);
access_pattern_t analyze_access_pattern(struct dirty_log *dl, unsigned long addr);
void update_multi_level_thresholds(struct dirty_log *dl, int round);

// Phase3优化：动态阈值和趋势判断
void collect_global_heat_stats(struct dirty_log *dl);
void update_dynamic_thresholds(struct dirty_log *dl, int round);

// Phase4优化：历史趋势分析
void update_page_history(struct dirty_log *dl, unsigned long vaddr, struct dirty_diffmap *dhm, int round);
page_history_t *get_page_history(struct dirty_log *dl, unsigned long vaddr);
void compute_page_stability(page_history_t *hist);

/* Runtime accessor for warm promotion threshold (reads CRIU_WARM_PROMOTE_THRESHOLD env override if present) */
float warm_promote_threshold_runtime(void);
bool should_skip_with_history(struct dirty_log *dl, unsigned long vaddr, struct dirty_diffmap *dhm, int round, page_class_t page_class);

// Decision model API (moved into dirty-map.c)
float compute_p_next_dirty(struct dirty_log *dl, struct dirty_diffmap *dhm, unsigned long vaddr, bool softdirty, int round, bool parent_missing, int deferred_count, bool is_warm, float *out_score, float *out_p_model);
float decision_p_threshold(struct dirty_log *dl, page_class_t page_class);
bool skip_model_loaded(void);

// Soft-budget / urgent-force support (heat-driven partial recovery)
void enforce_deferred_soft_budget(struct dirty_log *dl);
void add_urgent_force(struct dirty_log *dl, unsigned long addr);
int search_urgent_force(struct dirty_log *dl, unsigned long addr);
void del_urgent_force(struct dirty_log *dl, unsigned long addr);

// Per-class max-defer accessor (uses dl->adaptive_max_defer; no env override)
int get_max_defer_rounds_for_class(struct dirty_log *dl, page_class_t page_class);

#endif