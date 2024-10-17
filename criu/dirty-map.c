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

#include "dirty-map.h"
#include "xmalloc.h"
#include "protobuf.h"

struct list_head page_dirty_map_list LIST_HEAD_INIT(page_dirty_map_list);

//[Obsidian0215]initial <pid>'s dirty-map
int use_dirty_map(pid_t pid, struct dirty_log *dl){
	return 0;
}
