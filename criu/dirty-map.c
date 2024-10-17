#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <linux/falloc.h>
#include <sys/uio.h>
#include <limits.h>

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

// [Obsidian0215]initial <pid>'s dirty-map
int init_dirty_map(struct pstree_item *item, const char *dirty_map_dir){
	struct dirty_log *dl = &item->dirty_log;
	pid_t pid = dl->pid;

	return 0;
}

// [Obsidian0215]use <pid>'s dirty-map
int use_dirty_map(pid_t pid, struct dirty_log *dl){
	return 0;
}
