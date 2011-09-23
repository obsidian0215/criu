#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdbool.h>
#include <limits.h>

#include <sys/param.h>
#include <sys/types.h>
#include <sys/ptrace.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <dirent.h>

#include <fcntl.h>

#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <sys/ptrace.h>
#include <sys/user.h>
#include <sys/wait.h>

#include "compiler.h"
#include "types.h"
#include "list.h"
#include "util.h"

#include "crtools.h"

static char big_buffer[PATH_MAX];

void printk(const char *format, ...)
{
	va_list params;

	va_start(params, format);
	vfprintf(stdout, format, params);
	va_end(params);
}

int ptrace_show_area_r(pid_t pid, void *addr, long bytes)
{
	unsigned long w, i;
	if (bytes & (sizeof(long) - 1))
		return -1;
	for (w = 0; w < bytes / sizeof(long); w++) {
		unsigned long *a = addr;
		unsigned long v;
		v = ptrace(PTRACE_PEEKDATA, pid, a + w, NULL);
		if (v == -1U && errno)
			goto err;
		else {
			unsigned char *c = (unsigned char *)&v;
			for (i = sizeof(v)/sizeof(*c); i > 0; i--)
				printk("%02x ", c[i - 1]);
			printk("  ");
		}
	}
	printk("\n");
	return 0;
err:
	return -2;
}

int ptrace_show_area(pid_t pid, void *addr, long bytes)
{
	unsigned long w, i;
	if (bytes & (sizeof(long) - 1))
		return -1;
	printk("%016lx: ", (unsigned long)addr);
	for (w = 0; w < bytes / sizeof(long); w++) {
		unsigned long *a = addr;
		unsigned long v;
		v = ptrace(PTRACE_PEEKDATA, pid, a + w, NULL);
		if (v == -1U && errno)
			goto err;
		else {
			unsigned char *c = (unsigned char *)&v;
			for (i = 0; i < sizeof(v)/sizeof(*c); i++)
				printk("%02x ", c[i]);
			printk("  ");
		}
	}
	printk("\n");
	return 0;
err:
	return -2;
}

int ptrace_peek_area(pid_t pid, void *dst, void *addr, long bytes)
{
	unsigned long w;
	if (bytes & (sizeof(long) - 1))
		return -1;
	for (w = 0; w < bytes / sizeof(long); w++) {
		unsigned long *d = dst, *a = addr;
		d[w] = ptrace(PTRACE_PEEKDATA, pid, a + w, NULL);
		if (d[w] == -1U && errno)
			goto err;
	}
	return 0;
err:
	return -2;
}

int ptrace_poke_area(pid_t pid, void *src, void *addr, long bytes)
{
	unsigned long w;
	if (bytes & (sizeof(long) - 1))
		return -1;
	for (w = 0; w < bytes / sizeof(long); w++) {
		unsigned long *s = src, *a = addr;
		if (ptrace(PTRACE_POKEDATA, pid, a + w, s[w]))
			goto err;
	}
	return 0;
err:
	return -2;
}

void printk_registers(user_regs_struct_t *regs)
{
	printk("ip     : %16lx cs     : %16lx ds     : %16lx\n"
	       "es     : %16lx fs     : %16lx gs     : %16lx\n"
	       "sp     : %16lx ss     : %16lx flags  : %16lx\n"
	       "ax     : %16lx cx     : %16lx dx     : %16lx\n"
	       "si     : %16lx di     : %16lx bp     : %16lx\n"
	       "bx     : %16lx r8     : %16lx r9     : %16lx\n"
	       "r10    : %16lx r11    : %16lx r12    : %16lx\n"
	       "r13    : %16lx r14    : %16lx r15    : %16lx\n"
	       "orig_ax: %16lx fs_base: %16lx gs_base: %16lx\n\n",
	       regs->ip, regs->cs, regs->ds,
	       regs->es, regs->fs, regs->gs,
	       regs->sp, regs->ss, regs->flags,
	       regs->ax, regs->cx, regs->dx,
	       regs->si, regs->di, regs->bp,
	       regs->bx, regs->r8, regs->r9,
	       regs->r10, regs->r11, regs->r12,
	       regs->r13, regs->r14, regs->r15,
	       regs->orig_ax, regs->fs_base, regs->gs_base);
}

void printk_siginfo(siginfo_t *siginfo)
{
	printk("si_signo %d si_errno %d si_code %d\n",
	       siginfo->si_signo, siginfo->si_errno, siginfo->si_code);
}

void printk_vma(struct vma_area *vma_area)
{
	if (!vma_area)
		return;

	printk("s: %16lx e: %16lx l: %4liK p: %4x f: %4x fd: %4d pid: %4d dev:%02x:%02x:%04lx vf: %s st: %s spc: %s\n",
	       vma_area->vma.start, vma_area->vma.end,
	       (vma_area->vma.end - vma_area->vma.start) >> 10,
	       vma_area->vma.prot,
	       vma_area->vma.flags,
	       vma_area->vma.fd,
	       vma_area->vma.pid,
	       vma_area->vma.dev_maj,
	       vma_area->vma.dev_min,
	       vma_area->vma.ino,
	       vma_area->vm_file_fd < 0 ? "n" : "y",
	       !vma_area->vma.status ? "--" :
	       ((vma_area->vma.status & VMA_FILE_PRIVATE) ? "FP" :
		((vma_area->vma.status & VMA_FILE_SHARED) ? "FS" :
		 ((vma_area->vma.status & VMA_ANON_SHARED) ? "AS" :
		  ((vma_area->vma.status & VMA_ANON_PRIVATE) ? "AP" : "--")))),
	       !vma_area->vma.status ? "--" :
	       ((vma_area->vma.status & VMA_AREA_STACK) ? "stack" :
		((vma_area->vma.status & VMA_AREA_VSYSCALL) ? "vsyscall" :
		 ((vma_area->vma.status & VMA_AREA_VDSO) ? "vdso" : "n"))));
}

int unseize_task(pid_t pid)
{
	return ptrace(PTRACE_DETACH, pid, NULL, NULL);
}

int seize_task(pid_t pid)
{
	siginfo_t si;
	int status;
	int ret = 0;

	jerr_rc(ptrace(PTRACE_SEIZE, pid, NULL,
		       (void *)(unsigned long)PTRACE_SEIZE_DEVEL), ret, err);
	jerr_rc(ptrace(PTRACE_INTERRUPT, pid, NULL, NULL), ret, err);

	ret = -10;
	if (wait4(pid, &status, __WALL, NULL) != pid)
		goto err;

	ret = -20;
	if (!WIFSTOPPED(status))
		goto err;

	jerr_rc(ptrace(PTRACE_GETSIGINFO, pid, NULL, &si), ret, err_cont);

	ret = -30;
	if ((si.si_code >> 8) != PTRACE_EVENT_STOP)
		goto err_cont;

	jerr_rc(ptrace(PTRACE_SETOPTIONS, pid, NULL,
		       (void *)(unsigned long)PTRACE_O_TRACEEXIT), ret, err_cont);

err:
	return ret;

err_cont:
	continue_task(pid);
	goto err;
}

int reopen_fd_as(int new_fd, int old_fd)
{
	if (old_fd != new_fd) {
		int tmp = dup2(old_fd, new_fd);
		if (tmp < 0)
			return tmp;
		close(old_fd);
	}

	return new_fd;
}

int parse_maps(pid_t pid, struct list_head *vma_area_list)
{
	struct vma_area *vma_area = NULL;
	u64 start, end, pgoff;
	char map_files_path[64];
	char maps_path[64];
	unsigned long ino;
	char r,w,x,s;
	int dev_maj, dev_min;
	int ret = -1;

	DIR *map_files_dir = NULL;
	FILE *maps = NULL;

	snprintf(maps_path, sizeof(maps_path), "/proc/%d/maps", pid);
	maps = fopen(maps_path, "r");
	if (!maps) {
		pr_perror("Can't open: %s\n", maps_path);
		goto err;
	}

	snprintf(map_files_path, sizeof(map_files_path),
		 "/proc/%d/map_files", pid);

	/*
	 * It might be a problem in kernel, either
	 * I'm debugging it on old kernel ;)
	 */
	map_files_dir = opendir(map_files_path);
	if (!map_files_dir)
		pr_warning("Crap, can't open %s, old kernel?\n",
			   map_files_path);

	while (fgets(big_buffer, sizeof(big_buffer), maps)) {
		char vma_file_path[16+16+2];
		struct stat st_buf;

		ret = sscanf(big_buffer, "%lx-%lx %c%c%c%c %lx %02x:%02x %lu",
			     &start, &end, &r, &w, &x, &s, &pgoff, &dev_maj,
			     &dev_min, &ino);
		if (ret != 10) {
			pr_error("Can't parse: %s", big_buffer);
			return -1;
		}

		vma_area = alloc_vma_area();
		if (!vma_area)
			return -1;

		/* Figure out if it's file mapping */
		snprintf(vma_file_path, sizeof(vma_file_path), "%lx-%lx", start, end);

		if (map_files_dir) {
			/*
			 * Note that we "open" it in dumper process space
			 * so later we might refer to it via /proc/self/fd/vm_file_fd
			 * if needed.
			 */
			vma_area->vm_file_fd = openat(dirfd(map_files_dir),
						      vma_file_path, O_RDONLY);
			if (vma_area->vm_file_fd < 0) {
				if (errno != ENOENT) {
					pr_perror("Failed opening %s/%s\n",
						  map_files_path,
						  vma_file_path);
					goto err;
				}
			}
		}

		vma_area->vma.pid	= pid;
		vma_area->vma.start	= start;
		vma_area->vma.end	= end;
		vma_area->vma.pgoff	= pgoff;

		vma_area->vma.ino	= ino;
		vma_area->vma.dev_maj	= dev_maj;
		vma_area->vma.dev_min	= dev_min;

		vma_area->vma.prot	= PROT_NONE;

		if (r == 'r')
			vma_area->vma.prot |= PROT_READ;
		if (w == 'w')
			vma_area->vma.prot |= PROT_WRITE;
		if (x == 'x')
			vma_area->vma.prot |= PROT_EXEC;

		if (s == 's')
			vma_area->vma.flags = MAP_SHARED;
		else if (s == 'p')
			vma_area->vma.flags = MAP_PRIVATE;

		vma_area->vma.status = 0;

		if (strstr(big_buffer, "[stack]"))
			vma_area->vma.status |= VMA_AREA_REGULAR | VMA_AREA_STACK;
		else if (strstr(big_buffer, "[vsyscall]"))
			vma_area->vma.status |= VMA_AREA_VSYSCALL;
		else if (strstr(big_buffer, "[vdso]"))
			vma_area->vma.status |= VMA_AREA_VDSO;
		else if (strstr(big_buffer, "[heap]"))
			vma_area->vma.status |= VMA_AREA_REGULAR | VMA_AREA_HEAP;
		else
			vma_area->vma.status = VMA_AREA_REGULAR;

		/*
		 * Some mapping hints for restore, we save this on
		 * disk and restore might need to analyze it.
		 */
		if (vma_area->vm_file_fd >= 0) {

			if (fstat(vma_area->vm_file_fd, &st_buf) < 0) {
				pr_perror("Failed fstat on %s%s\n",
					  map_files_path,
					  vma_file_path);
				goto err;
			}
			if (!S_ISREG(st_buf.st_mode)) {
				pr_error("Can't handle non-regular "
					 "mapping on %s%s\n",
					  map_files_path,
					  vma_file_path);
				goto err;
			}

			/*
			 * /dev/zero stands for anon-shared mapping
			 * otherwise it's some file mapping.
			 */
			if (MAJOR(st_buf.st_dev) == 0) {
				if (!(vma_area->vma.flags & MAP_SHARED))
					goto err_bogus_mapping;
				vma_area->vma.status |= VMA_ANON_SHARED;
				vma_area->shmid = st_buf.st_ino;
			} else {
				if (vma_area->vma.flags & MAP_PRIVATE)
					vma_area->vma.status |= VMA_FILE_PRIVATE;
				else
					vma_area->vma.status |= VMA_FILE_SHARED;
			}
		} else {
			/*
			 * No file but mapping -- anonymous one.
			 */
			if (vma_area->vma.flags & MAP_SHARED)
				goto err_bogus_mapping;
			else
				vma_area->vma.status |= VMA_ANON_PRIVATE;
		}

		list_add_tail(&vma_area->list, vma_area_list);
	}

	vma_area = NULL;
	ret = 0;

err:
	if (maps)
		fclose(maps);

	if (map_files_dir)
		closedir(map_files_dir);

	xfree(vma_area);
	return ret;

err_bogus_mapping:
	pr_error("Bogus mapping %lx-%lx\n",
		 vma_area->vma.start,
		 vma_area->vma.end);
	goto err;
}
