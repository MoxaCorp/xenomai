/*
 * Copyright (C) 2001,2002,2003 Philippe Gerum <rpm@xenomai.org>.
 *
 * Xenomai is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published
 * by the Free Software Foundation; either version 2 of the License,
 * or (at your option) any later version.
 *
 * Xenomai is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xenomai; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA
 * 02111-1307, USA.
 */
#include <stdarg.h>
#include <linux/miscdevice.h>
#include <linux/device.h>
#include <linux/vmalloc.h>
#include <linux/mm.h>
#include <linux/fs.h>
#include <linux/spinlock.h>
#include <linux/sched.h>
#include <linux/mm.h>
#include <linux/semaphore.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <asm/io.h>
#include <cobalt/kernel/thread.h>
#include <cobalt/kernel/heap.h>
#include <cobalt/kernel/vfile.h>
#include <cobalt/kernel/ppd.h>
#include <cobalt/kernel/vdso.h>
#include <cobalt/kernel/assert.h>

/**
 * @ingroup cobalt_core
 * @defgroup cobalt_core_heap Dynamic memory allocation services
 *
 * The implementation of the memory allocator follows the algorithm
 * described in a USENIX 1988 paper called "Design of a General
 * Purpose Memory Allocator for the 4.3BSD Unix Kernel" by Marshall
 * K. McKusick and Michael J. Karels. You can find it at various
 * locations on the net, including
 * http://docs.FreeBSD.org/44doc/papers/kernmalloc.pdf.  A minor
 * variation allows this implementation to have 'extendable' heaps
 * when needed, with multiple memory extents providing autonomous page
 * address spaces.
 *
 * The data structures hierarchy is as follows:
 *
 * <tt> @verbatim
HEAP {
     block_buckets[]
     extent_queue -------+
}                        |
			 V
		      EXTENT #1 {
			     {static header}
			     page_map[npages]
			     page_array[npages][pagesize]
		      } -+
			 |
			 |
			 V
		      EXTENT #n {
			     {static header}
			     page_map[npages]
			     page_array[npages][pagesize]
		      }
@endverbatim </tt>
 *
 *@{
 */
struct xnheap kheap;		/* System heap */
EXPORT_SYMBOL_GPL(kheap);

static LIST_HEAD(heapq);	/* Heap list for v-file dump */

static int nrheaps;

#ifdef CONFIG_XENO_OPT_VFILE

static struct xnvfile_rev_tag vfile_tag;

static struct xnvfile_snapshot_ops vfile_ops;

struct vfile_priv {
	struct xnheap *curr;
};

struct vfile_data {
	size_t usable_mem;
	size_t used_mem;
	size_t page_size;
	char name[XNOBJECT_NAME_LEN];
};

static struct xnvfile_snapshot vfile = {
	.privsz = sizeof(struct vfile_priv),
	.datasz = sizeof(struct vfile_data),
	.tag = &vfile_tag,
	.ops = &vfile_ops,
};

static int vfile_rewind(struct xnvfile_snapshot_iterator *it)
{
	struct vfile_priv *priv = xnvfile_iterator_priv(it);

	if (list_empty(&heapq)) {
		priv->curr = NULL;
		return 0;
	}

	priv->curr = list_first_entry(&heapq, struct xnheap, next);

	return nrheaps;
}

static int vfile_next(struct xnvfile_snapshot_iterator *it, void *data)
{
	struct vfile_priv *priv = xnvfile_iterator_priv(it);
	struct vfile_data *p = data;
	struct xnheap *heap;

	if (priv->curr == NULL)
		return 0;	/* We are done. */

	heap = priv->curr;
	if (list_is_last(&heap->next, &heapq))
		priv->curr = NULL;
	else
		priv->curr = list_entry(heap->next.next,
					struct xnheap, next);

	p->usable_mem = xnheap_usable_mem(heap);
	p->used_mem = xnheap_used_mem(heap);
	p->page_size = xnheap_page_size(heap);
	knamecpy(p->name, heap->name);

	return 1;
}

static int vfile_show(struct xnvfile_snapshot_iterator *it, void *data)
{
	struct vfile_data *p = data;

	if (p == NULL)
		xnvfile_printf(it, "%9s %9s  %6s  %s\n",
			       "TOTAL", "USED", "PAGESZ", "NAME");
	else
		xnvfile_printf(it, "%9Zu %9Zu  %6Zu  %s\n",
			       p->usable_mem,
			       p->used_mem,
			       p->page_size,
			       p->name);
	return 0;
}

static struct xnvfile_snapshot_ops vfile_ops = {
	.rewind = vfile_rewind,
	.next = vfile_next,
	.show = vfile_show,
};

void xnheap_init_proc(void)
{
	xnvfile_init_snapshot("heap", &vfile, &nkvfroot);
}

void xnheap_cleanup_proc(void)
{
	xnvfile_destroy_snapshot(&vfile);
}

#endif /* CONFIG_XENO_OPT_VFILE */

static void init_extent(struct xnheap *heap, struct xnextent *extent)
{
	caddr_t freepage;
	int n, lastpgnum;

	/* The page area starts right after the (aligned) header. */
	extent->membase = (caddr_t) extent + heap->hdrsize;
	lastpgnum = heap->npages - 1;

	/* Mark each page as free in the page map. */
	for (n = 0, freepage = extent->membase;
	     n < lastpgnum; n++, freepage += heap->pagesize) {
		*((caddr_t *) freepage) = freepage + heap->pagesize;
		extent->pagemap[n].type = XNHEAP_PFREE;
		extent->pagemap[n].bcount = 0;
	}

	*((caddr_t *) freepage) = NULL;
	extent->pagemap[lastpgnum].type = XNHEAP_PFREE;
	extent->pagemap[lastpgnum].bcount = 0;
	extent->memlim = freepage + heap->pagesize;

	/* The first page starts the free list of a new extent. */
	extent->freelist = extent->membase;
}

/*
 */

/**
 * @fn xnheap_init(struct xnheap *heap,void *heapaddr,unsigned long heapsize,unsigned long pagesize)
 * @brief Initialize a memory heap.
 *
 * Initializes a memory heap suitable for time-bounded allocation
 * requests of dynamic memory.
 *
 * @param heap The address of a heap descriptor which will be used to
 * store the allocation data.  This descriptor must always be valid
 * while the heap is active therefore it must be allocated in
 * permanent memory.
 *
 * @param heapaddr The address of the heap storage area. All
 * allocations will be made from the given area in time-bounded
 * mode. Since additional extents can be added to a heap, this
 * parameter is also known as the "initial extent".
 *
 * @param heapsize The size in bytes of the initial extent pointed at
 * by @a heapaddr. @a heapsize must be a multiple of pagesize and
 * lower than 16 Mbytes. @a heapsize must be large enough to contain a
 * dynamically-sized internal header. The following formula gives the
 * size of this header:\n
 *
 * H = heapsize, P=pagesize, M=sizeof(struct pagemap), E=sizeof(struct xnextent)\n
 * hdrsize = ((H - E) * M) / (M + 1)\n
 *
 * This value is then aligned on the next 16-byte boundary. The
 * routine xnheap_overhead() computes the corrected heap size
 * according to the previous formula.
 *
 * @param pagesize The size in bytes of the fundamental memory page
 * which will be used to subdivide the heap internally. Choosing the
 * right page size is important regarding performance and memory
 * fragmentation issues, so it might be a good idea to take a look at
 * http://docs.FreeBSD.org/44doc/papers/kernmalloc.pdf to pick the
 * best one for your needs. In the current implementation, pagesize
 * must be a power of two in the range [ 8 .. 32768 ] inclusive.
 *
 * @return 0 is returned upon success, or one of the following error
 * codes:
 *
 * - -EINVAL is returned whenever a parameter is invalid.
 *
 * @coretags{task-unrestricted}
 */
int xnheap_init(struct xnheap *heap,
		void *heapaddr, unsigned long heapsize, unsigned long pagesize)
{
	unsigned long hdrsize, shiftsize, pageshift;
	struct xnextent *extent;
	spl_t s;

	/*
	 * Perform some parametrical checks first.
	 * Constraints are:
	 * PAGESIZE must be >= 2 ** MINLOG2.
	 * PAGESIZE must be <= 2 ** MAXLOG2.
	 * PAGESIZE must be a power of 2.
	 * HEAPSIZE must be large enough to contain the static part of an
	 * extent header.
	 * HEAPSIZE must be a multiple of PAGESIZE.
	 * HEAPSIZE must be lower than XNHEAP_MAXEXTSZ.
	 */

	if ((pagesize < (1 << XNHEAP_MINLOG2)) ||
	    (pagesize > (1 << XNHEAP_MAXLOG2)) ||
	    (pagesize & (pagesize - 1)) != 0 ||
	    heapsize <= sizeof(struct xnextent) ||
	    heapsize > XNHEAP_MAXEXTSZ || (heapsize & (pagesize - 1)) != 0)
		return -EINVAL;

	/*
	 * Determine the page map overhead inside the given extent
	 * size. We need to reserve 4 bytes in a page map for each
	 * page which is addressable into this extent. The page map is
	 * itself stored in the extent space, right after the static
	 * part of its header, and before the first allocatable page.
	 * pmapsize = (heapsize - sizeof(struct xnextent)) / pagesize *
	 * sizeof(struct xnpagemap). The overall header size is:
	 * static_part + pmapsize rounded to the minimum alignment
	 * size.
	*/
	hdrsize = xnheap_internal_overhead(heapsize, pagesize);

	/* Compute the page shiftmask from the page size (i.e. log2 value). */
	for (pageshift = 0, shiftsize = pagesize;
	     shiftsize > 1; shiftsize >>= 1, pageshift++)
		;	/* Loop */

	heap->pagesize = pagesize;
	heap->pageshift = pageshift;
	heap->extentsize = heapsize;
	heap->hdrsize = hdrsize;
	heap->npages = (heapsize - hdrsize) >> pageshift;

	/*
	 * An extent must contain at least two addressable pages to cope
	 * with allocation sizes between pagesize and 2 * pagesize.
	 */
	if (heap->npages < 2)
		return -EINVAL;

	heap->ubytes = 0;
	heap->maxcont = heap->npages * pagesize;

	INIT_LIST_HEAD(&heap->extents);
	heap->nrextents = 1;
	xnlock_init(&heap->lock);
	memset(heap->buckets, 0, sizeof(heap->buckets));
	extent = heapaddr;
	init_extent(heap, extent);
	list_add_tail(&extent->link, &heap->extents);

	ksformat(heap->name, sizeof(heap->name), "(%p)", heap);

	xnlock_get_irqsave(&nklock, s);
	list_add_tail(&heap->next, &heapq);
	nrheaps++;
	xnvfile_touch_tag(&vfile_tag);
	xnlock_put_irqrestore(&nklock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xnheap_init);

/**
 * @fn xnheap_set_name(struct xnheap *heap,const char *name,...)
 * @brief Set the heap's name string.
 *
 * Set the heap name that will be used in statistic outputs.
 *
 * @param heap The address of a heap descriptor.
 *
 * @param name Name displayed in statistic outputs. This parameter can
 * be a printk()-like format argument list.
 *
 * @coretags{task-unrestricted}
 */
void xnheap_set_name(struct xnheap *heap, const char *name, ...)
{
	va_list args;

	va_start(args, name);
	kvsformat(heap->name, sizeof(heap->name), name, args);
	va_end(args);
}
EXPORT_SYMBOL_GPL(xnheap_set_name);

/**
 * @fn void xnheap_destroy(struct xnheap *heap, void (*flushfn)(struct xnheap *heap, void *extaddr, unsigned long extsize, void *cookie), void *cookie)
 * @brief Destroys a memory heap.
 *
 * Destroys a memory heap.
 *
 * @param heap The descriptor address of the destroyed heap.
 *
 * @param flushfn If non-NULL, the address of a flush routine which
 * will be called for each extent attached to the heap. This routine
 * can be used by the calling code to further release the heap memory.
 *
 * @param cookie If @a flushfn is non-NULL, @a cookie is an opaque
 * pointer which will be passed unmodified to @a flushfn.
 *
 * @coretags{task-unrestricted}
 */
void xnheap_destroy(struct xnheap *heap,
		    void (*flushfn)(struct xnheap *heap,
				    void *extaddr,
				    unsigned long extsize, void *cookie),
		    void *cookie)
{
	struct xnextent *p, *tmp;
	spl_t s;

	xnlock_get_irqsave(&nklock, s);
	list_del(&heap->next);
	nrheaps--;
	xnvfile_touch_tag(&vfile_tag);
	xnlock_put_irqrestore(&nklock, s);

	if (flushfn == NULL)
		return;

	xnlock_get_irqsave(&heap->lock, s);

	if (list_empty(&heap->extents))
		goto done;

	list_for_each_entry_safe(p, tmp, &heap->extents, link) {
		list_del(&p->link);
		heap->nrextents--;
		xnlock_put_irqrestore(&heap->lock, s);
		flushfn(heap, p, heap->extentsize, cookie);
		xnlock_get_irqsave(&heap->lock, s);
	}
done:
	xnlock_put_irqrestore(&heap->lock, s);
}
EXPORT_SYMBOL_GPL(xnheap_destroy);

/*
 * get_free_range() -- Obtain a range of contiguous free pages to
 * fulfill an allocation of 2 ** log2size.  The caller must have
 * acquired the heap lock.
 */

static caddr_t get_free_range(struct xnheap *heap, unsigned long bsize, int log2size)
{
	caddr_t block, eblock, freepage, lastpage, headpage, freehead = NULL;
	unsigned long pagenum, pagecont, freecont;
	struct xnextent *extent;

	if (list_empty(&heap->extents))
		return NULL;

	list_for_each_entry(extent, &heap->extents, link) {
		freepage = extent->freelist;
		while (freepage) {
			headpage = freepage;
			freecont = 0;
			/*
			 * Search for a range of contiguous pages in
			 * the free page list of the current
			 * extent. The range must be 'bsize' long.
			 */
			do {
				lastpage = freepage;
				freepage = *((caddr_t *) freepage);
				freecont += heap->pagesize;
			}
			while (freepage == lastpage + heap->pagesize
			       && freecont < bsize);

			if (freecont >= bsize) {
				/*
				 * Ok, got it. Just update the free
				 * page list, then proceed to the next
				 * step.
				 */
				if (headpage == extent->freelist)
					extent->freelist =
					    *((caddr_t *) lastpage);
				else
					*((caddr_t *) freehead) =
					    *((caddr_t *) lastpage);

				goto splitpage;
			}
			freehead = lastpage;
		}
	}

	return NULL;

splitpage:

	/*
	 * At this point, headpage is valid and points to the first
	 * page of a range of contiguous free pages larger or equal
	 * than 'bsize'.
	 */
	if (bsize < heap->pagesize) {
		/*
		 * If the allocation size is smaller than the standard
		 * page size, split the page in smaller blocks of this
		 * size, building a free list of free blocks.
		 */
		for (block = headpage, eblock =
		     headpage + heap->pagesize - bsize; block < eblock;
		     block += bsize)
			*((caddr_t *) block) = block + bsize;

		*((caddr_t *) eblock) = NULL;
	} else
		*((caddr_t *) headpage) = NULL;

	pagenum = (headpage - extent->membase) >> heap->pageshift;

	/*
	 * Update the page map.  If log2size is non-zero (i.e. bsize
	 * <= 2 * pagesize), store it in the first page's slot to
	 * record the exact block size (which is a power of
	 * two). Otherwise, store the special marker XNHEAP_PLIST,
	 * indicating the start of a block whose size is a multiple of
	 * the standard page size, but not necessarily a power of two.
	 * In any case, the following pages slots are marked as
	 * 'continued' (PCONT).
	 */
	extent->pagemap[pagenum].type = log2size ? : XNHEAP_PLIST;
	extent->pagemap[pagenum].bcount = 1;

	for (pagecont = bsize >> heap->pageshift; pagecont > 1; pagecont--) {
		extent->pagemap[pagenum + pagecont - 1].type = XNHEAP_PCONT;
		extent->pagemap[pagenum + pagecont - 1].bcount = 0;
	}

	return headpage;
}

/**
 * @fn void *xnheap_alloc(struct xnheap *heap, unsigned long size)
 * @brief Allocate a memory block from a memory heap.
 *
 * Allocates a contiguous region of memory from an active memory heap.
 * Such allocation is guaranteed to be time-bounded.
 *
 * @param heap The descriptor address of the heap to get memory from.
 *
 * @param size The size in bytes of the requested block. Sizes lower
 * or equal to the page size are rounded either to the minimum
 * allocation size if lower than this value, or to the minimum
 * alignment size if greater or equal to this value. In the current
 * implementation, with MINALLOC = 8 and MINALIGN = 16, a 7 bytes
 * request will be rounded to 8 bytes, and a 17 bytes request will be
 * rounded to 32.
 *
 * @return The address of the allocated region upon success, or NULL
 * if no memory is available from the specified heap.
 *
 * @coretags{unrestricted}
 */
void *xnheap_alloc(struct xnheap *heap, unsigned long size)
{
	unsigned long pagenum, bsize;
	struct xnextent *extent;
	int log2size, ilog;
	caddr_t block;
	spl_t s;

	if (size == 0)
		return NULL;

	/*
	 * Sizes lower or equal to the page size are rounded either to
	 * the minimum allocation size if lower than this value, or to
	 * the minimum alignment size if greater or equal to this
	 * value.
	 */
	if (size <= heap->pagesize) {
		if (size <= XNHEAP_MINALIGNSZ)
			size =
			    (size + XNHEAP_MINALLOCSZ -
			     1) & ~(XNHEAP_MINALLOCSZ - 1);
		else
			size =
			    (size + XNHEAP_MINALIGNSZ -
			     1) & ~(XNHEAP_MINALIGNSZ - 1);
	} else
		/* Sizes greater than the page size are rounded to a multiple
		   of the page size. */
		size = (size + heap->pagesize - 1) & ~(heap->pagesize - 1);

	/* It becomes more space efficient to directly allocate pages from
	   the free page list whenever the requested size is greater than
	   2 times the page size. Otherwise, use the bucketed memory
	   blocks. */

	if (likely(size <= heap->pagesize * 2)) {
		/* Find the first power of two greater or equal to the rounded
		   size. The log2 value of this size is also computed. */

		for (bsize = (1 << XNHEAP_MINLOG2), log2size = XNHEAP_MINLOG2;
		     bsize < size; bsize <<= 1, log2size++)
			;	/* Loop */

		ilog = log2size - XNHEAP_MINLOG2;

		xnlock_get_irqsave(&heap->lock, s);

		block = heap->buckets[ilog].freelist;
		if (block == NULL) {
			block = get_free_range(heap, bsize, log2size);
			if (block == NULL)
				goto release_and_exit;
			if (bsize <= heap->pagesize)
				heap->buckets[ilog].fcount += (heap->pagesize >> log2size) - 1;
		} else {
			if (bsize <= heap->pagesize)
				--heap->buckets[ilog].fcount;
			if (!XENO_ASSERT(COBALT, !list_empty(&heap->extents)))
				goto oops;
			list_for_each_entry(extent, &heap->extents, link) {
				if ((caddr_t) block >= extent->membase &&
				    (caddr_t) block < extent->memlim)
					goto found;
			}
			XENO_BUG(COBALT);
		oops:
			block = NULL;
			goto release_and_exit;
		found:
			pagenum = ((caddr_t) block - extent->membase) >> heap->pageshift;
			++extent->pagemap[pagenum].bcount;
		}

		heap->buckets[ilog].freelist = *((caddr_t *)block);
		heap->ubytes += bsize;
	} else {
		if (size > heap->maxcont)
			return NULL;

		xnlock_get_irqsave(&heap->lock, s);

		/* Directly request a free page range. */
		block = get_free_range(heap, size, 0);

		if (block)
			heap->ubytes += size;
	}

      release_and_exit:

	xnlock_put_irqrestore(&heap->lock, s);

	return block;
}
EXPORT_SYMBOL_GPL(xnheap_alloc);

/**
 * @fn int xnheap_test_and_free(struct xnheap *heap,void *block,int (*ckfn)(void *block))
 * @brief Test and release a memory block to a memory heap.
 *
 * Releases a memory region to the memory heap it was previously
 * allocated from. Before the actual release is performed, an optional
 * user-defined can be invoked to check for additional criteria with
 * respect to the request consistency.
 *
 * @param heap The descriptor address of the heap to release memory
 * to.
 *
 * @param block The address of the region to be returned to the heap.
 *
 * @param ckfn The address of a user-supplied verification routine
 * which is to be called after the memory address specified by @a
 * block has been checked for validity. The routine is expected to
 * proceed to further consistency checks, and either return zero upon
 * success, or non-zero upon error. In the latter case, the release
 * process is aborted, and @a ckfn's return value is passed back to
 * the caller of this service as its error return code.
 *
 * @warning @a ckfn must not reschedule either directly or indirectly.
 *
 * @return 0 is returned upon success, or -EINVAL is returned whenever
 * the block is not a valid region of the specified heap. Additional
 * return codes can also be defined locally by the @a ckfn routine.
 *
 * @coretags{unrestricted}
 */
int xnheap_test_and_free(struct xnheap *heap, void *block, int (*ckfn) (void *block))
{
	caddr_t freepage, lastpage, nextpage, tailpage, freeptr, *tailptr;
	int log2size, npages, ret, nblocks, xpage, ilog;
	unsigned long pagenum, pagecont, boffset, bsize;
	struct xnextent *extent;
	spl_t s;

	xnlock_get_irqsave(&heap->lock, s);

	/*
	 * Find the extent from which the returned block is
	 * originating from.
	 */
	ret = -EFAULT;
	if (list_empty(&heap->extents))
		goto unlock_and_fail;

	list_for_each_entry(extent, &heap->extents, link) {
		if ((caddr_t)block >= extent->membase &&
		    (caddr_t)block < extent->memlim)
			goto found;
	}

	goto unlock_and_fail;
found:
	/* Compute the heading page number in the page map. */
	pagenum = ((caddr_t)block - extent->membase) >> heap->pageshift;
	boffset = ((caddr_t) block -
		   (extent->membase + (pagenum << heap->pageshift)));

	switch (extent->pagemap[pagenum].type) {
	case XNHEAP_PFREE:	/* Unallocated page? */
	case XNHEAP_PCONT:	/* Not a range heading page? */
	bad_block:
		ret = -EINVAL;
	unlock_and_fail:
		xnlock_put_irqrestore(&heap->lock, s);
		return ret;

	case XNHEAP_PLIST:

		if (ckfn && (ret = ckfn(block)) != 0)
			goto unlock_and_fail;

		npages = 1;

		while (npages < heap->npages &&
		       extent->pagemap[pagenum + npages].type == XNHEAP_PCONT)
			npages++;

		bsize = npages * heap->pagesize;

	free_page_list:

		/* Link all freed pages in a single sub-list. */

		for (freepage = (caddr_t) block,
		     tailpage = (caddr_t) block + bsize - heap->pagesize;
		     freepage < tailpage; freepage += heap->pagesize)
			*((caddr_t *) freepage) = freepage + heap->pagesize;

	free_pages:

		/* Mark the released pages as free in the extent's page map. */

		for (pagecont = 0; pagecont < npages; pagecont++)
			extent->pagemap[pagenum + pagecont].type = XNHEAP_PFREE;

		/* Return the sub-list to the free page list, keeping
		   an increasing address order to favor coalescence. */

		for (nextpage = extent->freelist, lastpage = NULL;
		     nextpage != NULL && nextpage < (caddr_t) block;
		     lastpage = nextpage, nextpage = *((caddr_t *) nextpage))
		  ;	/* Loop */

		*((caddr_t *) tailpage) = nextpage;

		if (lastpage)
			*((caddr_t *) lastpage) = (caddr_t) block;
		else
			extent->freelist = (caddr_t) block;
		break;

	default:

		log2size = extent->pagemap[pagenum].type;
		bsize = (1 << log2size);

		if ((boffset & (bsize - 1)) != 0)	/* Not a block start? */
			goto bad_block;

		if (ckfn && (ret = ckfn(block)) != 0)
			goto unlock_and_fail;

		/*
		 * Return the page to the free list if we've just
		 * freed its last busy block. Pages from multi-page
		 * blocks are always pushed to the free list (bcount
		 * value for the heading page is always 1).
		 */

		ilog = log2size - XNHEAP_MINLOG2;

		if (likely(--extent->pagemap[pagenum].bcount > 0)) {
			/* Return the block to the bucketed memory space. */
			*((caddr_t *) block) = heap->buckets[ilog].freelist;
			heap->buckets[ilog].freelist = block;
			++heap->buckets[ilog].fcount;
			break;
		}

		npages = bsize >> heap->pageshift;

		if (unlikely(npages > 1))
			/*
			 * The simplest case: we only have a single
			 * block to deal with, which spans multiple
			 * pages. We just need to release it as a list
			 * of pages, without caring about the
			 * consistency of the bucket.
			 */
			goto free_page_list;

		freepage = extent->membase + (pagenum << heap->pageshift);
		block = freepage;
		tailpage = freepage;
		nextpage = freepage + heap->pagesize;
		nblocks = heap->pagesize >> log2size;
		heap->buckets[ilog].fcount -= (nblocks - 1);
		XENO_BUGON(COBALT, heap->buckets[ilog].fcount < 0);

		/*
		 * Easy case: all free blocks are laid on a single
		 * page we are now releasing. Just clear the bucket
		 * and bail out.
		 */

		if (likely(heap->buckets[ilog].fcount == 0)) {
			heap->buckets[ilog].freelist = NULL;
			goto free_pages;
		}

		/*
		 * Worst case: multiple pages are traversed by the
		 * bucket list. Scan the list to remove all blocks
		 * belonging to the freed page. We are done whenever
		 * all possible blocks from the freed page have been
		 * traversed, or we hit the end of list, whichever
		 * comes first.
		 */

		for (tailptr = &heap->buckets[ilog].freelist, freeptr = *tailptr, xpage = 1;
		     freeptr != NULL && nblocks > 0; freeptr = *((caddr_t *) freeptr)) {
			if (unlikely(freeptr < freepage || freeptr >= nextpage)) {
				if (unlikely(xpage)) { /* Limit random writes */
					*tailptr = freeptr;
					xpage = 0;
				}
				tailptr = (caddr_t *)freeptr;
			} else {
				--nblocks;
				xpage = 1;
			}
		}

		*tailptr = freeptr;
		goto free_pages;
	}

	heap->ubytes -= bsize;

	xnlock_put_irqrestore(&heap->lock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xnheap_test_and_free);

/**
 * @fn int xnheap_free(struct xnheap *heap, void *block)
 * @brief Release a memory block to a memory heap.
 *
 * Releases a memory region to the memory heap it was previously
 * allocated from.
 *
 * @param heap The descriptor address of the heap to release memory
 * to.
 *
 * @param block The address of the region to be returned to the heap.
 *
 * @return 0 is returned upon success, or one of the following error
 * codes:
 *
 * - -EFAULT is returned whenever the memory address is outside the
 * heap address space.
 *
 * - -EINVAL is returned whenever the memory address does not
 * represent a valid block.
 *
 * @coretags{unrestricted}
 */
int xnheap_free(struct xnheap *heap, void *block)
{
	return xnheap_test_and_free(heap, block, NULL);
}
EXPORT_SYMBOL_GPL(xnheap_free);

/**
 * @fn int xnheap_extend(struct xnheap *heap, void *extaddr, unsigned long extsize)
 * @brief Extend a memory heap.
 *
 * Add a new extent to an existing memory heap.
 *
 * @param heap The descriptor address of the heap to add an extent to.
 *
 * @param extaddr The address of the extent memory.
 *
 * @param extsize The size of the extent memory (in bytes). In the
 * current implementation, this size must match the one of the initial
 * extent passed to xnheap_init().
 *
 * @return 0 is returned upon success, or -EINVAL is returned if
 * @a extsize differs from the initial extent's size.
 *
 * @coretags{unrestricted}
 */
int xnheap_extend(struct xnheap *heap, void *extaddr, unsigned long extsize)
{
	struct xnextent *extent = extaddr;
	spl_t s;

	if (extsize != heap->extentsize)
		return -EINVAL;

	init_extent(heap, extent);
	xnlock_get_irqsave(&heap->lock, s);
	list_add_tail(&extent->link, &heap->extents);
	heap->nrextents++;
	xnlock_put_irqrestore(&heap->lock, s);

	return 0;
}
EXPORT_SYMBOL_GPL(xnheap_extend);

int xnheap_check_block(struct xnheap *heap, void *block)
{
	unsigned long pagenum, boffset;
	int ptype, ret = -EINVAL;
	struct xnextent *extent;
	spl_t s;

	xnlock_get_irqsave(&heap->lock, s);

	/*
	 * Find the extent from which the checked block is
	 * originating from.
	 */
	if (list_empty(&heap->extents))
		goto out;

	list_for_each_entry(extent, &heap->extents, link) {
		if ((caddr_t)block >= extent->membase &&
		    (caddr_t)block < extent->memlim)
			goto found;
	}

	goto out;
found:
	/* Compute the heading page number in the page map. */
	pagenum = ((caddr_t)block - extent->membase) >> heap->pageshift;
	boffset = ((caddr_t)block -
		   (extent->membase + (pagenum << heap->pageshift)));
	ptype = extent->pagemap[pagenum].type;

	/* Raise error if page unallocated or not heading a range. */
	if (ptype != XNHEAP_PFREE && ptype != XNHEAP_PCONT)
		ret = 0;
out:
	xnlock_put_irqrestore(&heap->lock, s);

	return ret;
}
EXPORT_SYMBOL_GPL(xnheap_check_block);

/** @} */
