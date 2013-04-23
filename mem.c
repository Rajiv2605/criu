#include <unistd.h>
#include <stdio.h>
#include <sys/mman.h>
#include <errno.h>

#include "crtools.h"
#include "mem.h"
#include "parasite-syscall.h"
#include "parasite.h"
#include "page-pipe.h"
#include "page-xfer.h"
#include "log.h"
#include "kerndat.h"

#include "protobuf.h"
#include "protobuf/pagemap.pb-c.h"

#define PME_PRESENT	(1ULL << 63)
#define PME_SWAP	(1ULL << 62)
#define PME_FILE	(1ULL << 61)
#define PME_SOFT_DIRTY	(1Ull << 55)

/*
 * On dump we suck in the whole parent pagemap. Then, when observing
 * a page with soft-dirty bit cleared (i.e. -- not modified) we check
 * this map for this page presense.
 *
 * Since we scan the address space from vaddr 0 to 0xF..F, we can do
 * linear search in parent pagemap and the rover variables helps us
 * do it.
 */

struct mem_snap_ctx {
	unsigned long nr_iovs;
	struct iovec *iovs;
	unsigned long alloc;
	unsigned long rover;
};

#define MEM_SNAP_BATCH	64

static int task_reset_dirty_track(int pid)
{
	int fd, ret;
	char cmd[] = "4";

	if (!opts.mem_snapshot)
		return 0;

	pr_info("Reset %d's dirty tracking\n", pid);
	fd = open_proc_rw(pid, "clear_refs");
	if (fd < 0)
		return -1;

	ret = write(fd, cmd, sizeof(cmd));
	close(fd);

	if (ret < 0) {
		pr_perror("Can't reset %d's dirty memory tracker", pid);
		return -1;
	}

	pr_info(" ... done\n");
	return 0;
}

static struct mem_snap_ctx *mem_snap_init(struct parasite_ctl *ctl)
{
	struct mem_snap_ctx *ctx;
	int p_fd, pm_fd;
	PagemapHead *h;

	if (!opts.mem_snapshot)
		return NULL;

	if (!kerndat_has_dirty_track) {
		pr_err("Kernel doesn't support dirty tracking. "
				"No snapshot available.\n");
		return ERR_PTR(-1);
	}

	p_fd = get_service_fd(PARENT_FD_OFF);
	if (p_fd < 0) {
		pr_debug("Will do full memory dump\n");
		return NULL;
	}

	pm_fd = open_image_at(p_fd, CR_FD_PAGEMAP, O_RSTR, ctl->pid.virt);
	if (pm_fd < 0)
		return ERR_PTR(pm_fd);

	ctx = xmalloc(sizeof(*ctx));
	if (!ctx)
		goto err_cl;

	ctx->nr_iovs = 0;
	ctx->alloc = MEM_SNAP_BATCH;
	ctx->rover = 0;
	ctx->iovs = xmalloc(MEM_SNAP_BATCH * sizeof(struct iovec));
	if (!ctx->iovs)
		goto err_free;

	if (pb_read_one(pm_fd, &h, PB_PAGEMAP_HEAD) < 0)
		goto err_freei;

	pagemap_head__free_unpacked(h, NULL);

	while (1) {
		int ret;
		PagemapEntry *pe;

		ret = pb_read_one_eof(pm_fd, &pe, PB_PAGEMAP);
		if (ret == 0)
			break;
		if (ret < 0)
			goto err_freei;

		ctx->iovs[ctx->nr_iovs].iov_base = decode_pointer(pe->vaddr);
		ctx->iovs[ctx->nr_iovs].iov_len = pe->nr_pages * PAGE_SIZE;
		ctx->nr_iovs++;
		pagemap_entry__free_unpacked(pe, NULL);

		if (ctx->nr_iovs >= ctx->alloc) {
			ctx->iovs = xrealloc(ctx->iovs,
					(ctx->alloc + MEM_SNAP_BATCH) * sizeof(struct iovec));
			if (!ctx->iovs)
				goto err_freei;

			ctx->alloc += MEM_SNAP_BATCH;
		}
	}

	pr_info("Collected parent snap of %lu entries\n", ctx->nr_iovs);
	close(pm_fd);
	return ctx;

err_freei:
	xfree(ctx->iovs);
err_free:
	xfree(ctx);
err_cl:
	close(pm_fd);
	return ERR_PTR(-1);
}

static void mem_snap_close(struct mem_snap_ctx *ctx)
{
	if (ctx) {
		xfree(ctx->iovs);
		xfree(ctx);
	}
}

unsigned int vmas_pagemap_size(struct vm_area_list *vmas)
{
	/*
	 * In the worst case I need one iovec for half of the
	 * pages (e.g. every odd/even)
	 */

	return sizeof(struct parasite_dump_pages_args) +
		(vmas->priv_size + 1) * sizeof(struct iovec) / 2;
}

static inline bool should_dump_page(VmaEntry *vmae, u64 pme)
{
	if (vma_entry_is(vmae, VMA_AREA_VDSO))
		return true;
	/*
	 * Optimisation for private mapping pages, that haven't
	 * yet being COW-ed
	 */
	if (vma_entry_is(vmae, VMA_FILE_PRIVATE) && (pme & PME_FILE))
		return false;
	if (pme & (PME_PRESENT | PME_SWAP))
		return true;

	return false;
}

static int page_in_parent(unsigned long vaddr, u64 map, struct mem_snap_ctx *snap)
{
	/*
	 * Soft-dirty pages should be dumped here
	 */
	if (map & PME_SOFT_DIRTY)
		return 0;

	/*
	 * Non soft-dirty should be present in parent map.
	 * Otherwise pagemap is screwed up.
	 */

	while (1) {
		struct iovec *iov;

		iov = &snap->iovs[snap->rover];
		if ((unsigned long)iov->iov_base > vaddr)
			break;

		if ((unsigned long)iov->iov_base + iov->iov_len > vaddr)
			return 1;

		snap->rover++;
		if (snap->rover >= snap->nr_iovs)
			break;
	}

	pr_warn("Page %lx not in parent snap range (rover %lu).\n"
			"Dumping one, but the pagemap is screwed up.\n",
			vaddr, snap->rover);
	return 0;
}

static int generate_iovs(struct vma_area *vma, int pagemap, struct page_pipe *pp, u64 *map,
		struct mem_snap_ctx *snap)
{
	unsigned long pfn, nr_to_scan;
	unsigned long pages[2] = {};
	u64 aux;

	aux = vma->vma.start / PAGE_SIZE * sizeof(*map);
	if (lseek(pagemap, aux, SEEK_SET) != aux) {
		pr_perror("Can't rewind pagemap file");
		return -1;
	}

	nr_to_scan = vma_area_len(vma) / PAGE_SIZE;
	aux = nr_to_scan * sizeof(*map);
	if (read(pagemap, map, aux) != aux) {
		pr_perror("Can't read pagemap file");
		return -1;
	}

	for (pfn = 0; pfn < nr_to_scan; pfn++) {
		unsigned long vaddr;
		int ret;

		if (!should_dump_page(&vma->vma, map[pfn]))
			continue;

		vaddr = vma->vma.start + pfn * PAGE_SIZE;
		if (snap && page_in_parent(vaddr, map[pfn], snap)) {
			ret = page_pipe_add_hole(pp, vaddr);
			pages[0]++;
		} else {
			ret = page_pipe_add_page(pp, vaddr);
			pages[1]++;
		}

		if (ret)
			return -1;
	}

	pr_info("Pagemap generated: %lu pages %lu holes\n", pages[1], pages[0]);
	return 0;
}

static int parasite_mprotect_seized(struct parasite_ctl *ctl, struct vm_area_list *vma_area_list, bool unprotect)
{
	struct parasite_mprotect_args *args;
	struct parasite_vma_entry *p_vma;
	struct vma_area *vma;

	args = parasite_args_s(ctl, vmas_pagemap_size(vma_area_list));

	p_vma = args->vmas;
	args->nr = 0;

	list_for_each_entry(vma, &vma_area_list->h, list) {
		if (!privately_dump_vma(vma))
			continue;
		if (vma->vma.prot & PROT_READ)
			continue;
		p_vma->start = vma->vma.start;
		p_vma->len = vma_area_len(vma);
		p_vma->prot = vma->vma.prot;
		if (unprotect)
			p_vma->prot |= PROT_READ;
		args->nr++;
		p_vma++;
	}

	return parasite_execute(PARASITE_CMD_MPROTECT_VMAS, ctl);
}

static int __parasite_dump_pages_seized(struct parasite_ctl *ctl,
		struct vm_area_list *vma_area_list, struct cr_fdset *cr_fdset)
{
	struct parasite_dump_pages_args *args;
	u64 *map;
	int pagemap;
	struct page_pipe *pp;
	struct page_pipe_buf *ppb;
	struct vma_area *vma_area;
	int ret = -1;
	struct page_xfer xfer;
	struct mem_snap_ctx *snap;

	pr_info("\n");
	pr_info("Dumping pages (type: %d pid: %d)\n", CR_FD_PAGES, ctl->pid.real);
	pr_info("----------------------------------------\n");

	pr_debug("   Private vmas %lu/%lu pages\n",
			vma_area_list->longest, vma_area_list->priv_size);

	snap = mem_snap_init(ctl);
	if (IS_ERR(snap))
		goto out;

	args = parasite_args_s(ctl, vmas_pagemap_size(vma_area_list));

	map = xmalloc(vma_area_list->longest * sizeof(*map));
	if (!map)
		goto out_snap;

	ret = pagemap = open_proc(ctl->pid.real, "pagemap2");
	if (ret < 0) {
		if (errno != ENOENT)
			goto out_free;

		ret = pagemap = open_proc(ctl->pid.real, "pagemap");
		if (ret < 0)
			goto out_free;
	}

	ret = -1;
	pp = create_page_pipe(vma_area_list->priv_size / 2, args->iovs);
	if (!pp)
		goto out_close;

	list_for_each_entry(vma_area, &vma_area_list->h, list) {
		if (!privately_dump_vma(vma_area))
			continue;

		ret = generate_iovs(vma_area, pagemap, pp, map, snap);
		if (ret < 0)
			goto out_pp;
	}

	debug_show_page_pipe(pp);

	args->off = 0;
	list_for_each_entry(ppb, &pp->bufs, l) {
		ret = parasite_send_fd(ctl, ppb->p[1]);
		if (ret)
			goto out_pp;

		args->nr = ppb->nr_segs;
		args->nr_pages = ppb->pages_in;
		pr_debug("PPB: %d pages %d segs %u pipe %d off\n",
				args->nr_pages, args->nr, ppb->pipe_size, args->off);

		ret = parasite_execute(PARASITE_CMD_DUMPPAGES, ctl);
		if (ret < 0)
			goto out_pp;

		args->off += args->nr;
	}

	ret = open_page_xfer(&xfer, CR_FD_PAGEMAP, ctl->pid.virt);
	if (ret < 0)
		goto out_pp;

	ret = page_xfer_dump_pages(&xfer, pp, 0);

	xfer.close(&xfer);
	task_reset_dirty_track(ctl->pid.real);
out_pp:
	destroy_page_pipe(pp);
out_close:
	close(pagemap);
out_free:
	xfree(map);
out_snap:
	mem_snap_close(snap);
out:
	pr_info("----------------------------------------\n");
	return ret;
}

int parasite_dump_pages_seized(struct parasite_ctl *ctl,
		struct vm_area_list *vma_area_list, struct cr_fdset *cr_fdset)
{
	int ret;

	ret = parasite_mprotect_seized(ctl, vma_area_list, true);
	if (ret) {
		pr_err("Can't dump unprotect vmas with parasite\n");
		return ret;
	}

	ret = __parasite_dump_pages_seized(ctl, vma_area_list, cr_fdset);
	if (ret)
		pr_err("Can't dump page with parasite\n");

	if (parasite_mprotect_seized(ctl, vma_area_list, false)) {
		pr_err("Can't rollback unprotected vmas with parasite\n");
		ret = -1;
	}

	return ret;
}

