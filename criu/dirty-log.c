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

#include "dirty-log.h"
#include "xmalloc.h"
#include "protobuf.h"

struct list_head page_dirty_map_list LIST_HEAD_INIT(page_dirty_map_list);

//[Obsidian0215]initial <pid>'s dirty-log
int init_dirty_log_images(pid_t pid, struct dirty_log *dl){

	int dfd;

    dl->pid = pid;
	//[Obsidian0215]load a previous dirtylog img or create a new img
	pr_info("Try to load previous dirtylog images\n");

	dfd = get_service_fd(DIRTY_LOG);
	dl->dli = open_image_at(dfd, CR_FD_DIRTY_LOG, O_RDWR, pid);

	if (!dl->dli) {
		pr_info("Falied to load previous dirtylog images, create a new image\n");
		dl->dli = open_image_at(dfd, CR_FD_DIRTY_LOG, O_WRONLY|O_CREAT, pid);
		
		if (!dli) {
			pr_perror("Can't create a dirtylog image for %d", pid);
			return -1;
		}

        dl->pdme = xzalloc(sizeof(*dl->pdme));
        if (!dl->pdme)
            return -1;

        dl->pdm = *dl->pdme;
        dl->nr_pdms = 0;
        dl->curr_pdm = -1;
		dl->pdm->list = LIST_HEAD_INIT(page_dirty_map_list);

        return 0;
	}


	return 0;
}

int update_dirty_map(pdm_t *pdm) {

}

struct page_dirty_map *search_vaddr_in_range_dg(u64 vaddr, struct dirty_log *dl) {
	struct page_dirty_map *pdm_temp;

	pdm_temp = dl->pdm;

	if(vaddr >= pdm_temp->vaddr && vaddr <= (pdm_temp->vaddr + pdm_temp->nr_pages * PAGE_SIZE)) {

	}
	else if (!dl->nr_pdms) {

		}
	else {
		pdm->temp = xzalloc();
	}

}
