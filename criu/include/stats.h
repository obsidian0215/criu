#ifndef __CR_STATS_H__
#define __CR_STATS_H__

enum {
	TIME_FREEZING,
	TIME_FROZEN,
	TIME_MEMDUMP,
	TIME_MEMWRITE,
	TIME_IRMAP_RESOLVE,

	DUMP_TIME_NR_STATS,
};

enum {
	TIME_FORK,
	TIME_RESTORE,
	TIME_READ_PAGES,
	TIME_COMPARE_PAGES,
	TIME_COPY_PAGES,
	TIME_PAGE_XFER,
	TIME_PREPARE_NS,
	TIME_RESTORE_NS,
	TIME_RESTORE_CGROUP,
	TIME_RESTORE_FILES,
	TIME_RESTORE_PIDS,
	TIME_RESTORE_SOCKETS,
	TIME_RESTORE_CREDS,
	TIME_RESTORE_MNTNS,
	TIME_RESTORE_VMAS,

	RESTORE_TIME_NS_STATS,
};

extern void timing_start(int t);
extern void timing_stop(int t);

/*
 * Conditional timing macros for fine-grained phase timing.
 * Only performs timing when STATS_EXPORT env var is set to avoid overhead.
 */
#define timing_start_if_enabled(t) do { \
	if (getenv("STATS_EXPORT")) timing_start(t); \
} while (0)

#define timing_stop_if_enabled(t) do { \
	if (getenv("STATS_EXPORT")) timing_stop(t); \
} while (0)

enum {
	CNT_PAGES_SCANNED,
	CNT_PAGES_SKIPPED_PARENT,
	CNT_PAGES_WRITTEN,
	CNT_PAGES_LAZY,
	CNT_PAGE_PIPES,
	CNT_PAGE_PIPE_BUFS,

	CNT_SHPAGES_SCANNED,
	CNT_SHPAGES_SKIPPED_PARENT,
	CNT_SHPAGES_WRITTEN,

	DUMP_CNT_NR_STATS,
};

enum {
	CNT_PAGES_COMPARED,
	CNT_PAGES_SKIPPED_COW,
	CNT_PAGES_RESTORED,

	CNT_READ_PAGES_USEC,
	CNT_COMPARE_PAGES_USEC,
	CNT_COPY_PAGES_USEC,
	CNT_PAGE_XFER_USEC,
	CNT_PREPARE_NS_USEC,
	CNT_RESTORE_NS_USEC,
	CNT_RESTORE_CGROUP_USEC,
	CNT_RESTORE_FILES_USEC,
	CNT_RESTORE_PIDS_USEC,
	CNT_RESTORE_SOCKETS_USEC,
	CNT_RESTORE_CREDS_USEC,
	CNT_RESTORE_MNTNS_USEC,
	CNT_RESTORE_VMAS_USEC,

	RESTORE_CNT_NR_STATS,
};

extern void cnt_add(int c, unsigned long val);
extern void cnt_sub(int c, unsigned long val);

#define DUMP_STATS    1
#define RESTORE_STATS 2

extern int init_stats(int what);
extern void write_stats(int what);

#endif /* __CR_STATS_H__ */
