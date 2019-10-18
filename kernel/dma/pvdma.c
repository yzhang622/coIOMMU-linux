// SPDX-License-Identifier: GPL-2.0
/*
 * Paravirtualized DMA operations that offers DMA inspection between
 * guest & host.
 */
#include <linux/export.h>
#include <linux/mm.h>
#include <linux/dma-mapping.h>
#include <linux/dma-direct.h>
#include <linux/intel-iommu.h>
#include <linux/bitmap.h>
#include <linux/scatterlist.h>
#include <linux/pci.h>
#include <linux/pvdma.h>
#include <asm/pvdma.h>


#define PVDMA_UPPER_LEVEL_STRIDE	(9)
#define PVDMA_UPPER_LEVEL_MASK		(((u64)1 << PVDMA_UPPER_LEVEL_STRIDE) - 1)
#define PVDMA_PT_LEVEL_STRIDE		(10)
#define PVDMA_PT_LEVEL_MASK		(((u64)1 << PVDMA_PT_LEVEL_STRIDE) - 1)

pvdma_ipt ipt;
void * device_bitmap;

static inline void pvdma_ipt_page_cache_alloc(void)
{
	pvdma_info_cache *c = &ipt.pvdma_ipt_cache;
	void *obj;

	while (c->nobjs < PVDMA_INFO_NR_OBJS) {
		//should we use GFP_ATOMIC or GFP_NOWAIT?
		obj = (void*)__get_free_page(GFP_ATOMIC|GFP_KERNEL|__GFP_ZERO);
		if (!obj)
			break;
		c->objects[c->nobjs++] = obj;
	}
}

static inline void pvdma_ipt_page_cache_free(void)
{
	pvdma_info_cache *c = &ipt.pvdma_ipt_cache;

	while (c->nobjs)
		free_page((unsigned long)c->objects[--c->nobjs]);
}

static void *pvdma_ipt_alloc_page(void)
{
	pvdma_info_cache *c = &ipt.pvdma_ipt_cache;
	void *obj;

	if (!c->nobjs)
		pvdma_ipt_page_cache_alloc();

	if (!c->nobjs) {
		pvdma_dbg("alloc failed.\n");
		return NULL;
	}

	obj = c->objects[--c->nobjs];
	return obj;
}

static inline unsigned int pvdma_level_to_offset(unsigned long pfn,
							unsigned int level)
{
	unsigned int offset;

	if (level == 1)
		return (pfn) & PVDMA_PT_LEVEL_MASK;

	offset = PVDMA_PT_LEVEL_STRIDE + (level - 2) * PVDMA_UPPER_LEVEL_STRIDE;

	return (pfn >> offset) & PVDMA_UPPER_LEVEL_MASK;
}

static inline unsigned int get_ipt_level(void)
{
	unsigned int pfn_width;

	pfn_width = boot_cpu_data.x86_phys_bits - PAGE_SHIFT;

	if (pfn_width <= PVDMA_PT_LEVEL_STRIDE)
		return 1;

	return DIV_ROUND_UP((pfn_width - PVDMA_PT_LEVEL_STRIDE),
						PVDMA_UPPER_LEVEL_STRIDE) + 1;
}

static ipt_leaf_entry *pfn_to_ipt_pte(unsigned long pfn, bool alloc)
{
	ipt_parent_entry *parent_pte;
	void *pt;
	unsigned int index;
	ipt_leaf_entry	*leaf_pte;
	unsigned int level = ipt.ipt_level;
	unsigned int target_level = 1;

	pt = (void *)ipt.ipt_root;

	while (level != target_level) {
		index = pvdma_level_to_offset(pfn, level);
		parent_pte = (ipt_parent_entry *)pt + index;

		if (!parent_pte->present) {
			if (!alloc || !(pt = pvdma_ipt_alloc_page()))
				break;
			parent_pte->pfn = (virt_to_phys(pt) >> 12);
			parent_pte->present = 1;
		}
		pt = (phys_to_virt(parent_pte->pfn << 12));
		level--;
	}

	if (level > target_level) {
		if (alloc)
			pvdma_dbg("IPT allocation failed at level %d for \
					pfn 0x%lx .\n", level, pfn);
		else
			pvdma_dbg("IPT absent at level %d for pfn 0x%lx .\n",
					level, pfn);

		return NULL;
	}

	index = pvdma_level_to_offset(pfn, target_level);
	leaf_pte = (ipt_leaf_entry *)pt + index;

	return leaf_pte;
}

bool is_page_pinned(unsigned long pfn)
{
	ipt_leaf_entry	*leaf_pte;

	leaf_pte = pfn_to_ipt_pte(pfn, false);
	if (leaf_pte == NULL)
		return false;

	return pvdma_test_flag(IPTE_PINNED_FLAG, leaf_pte);
}

static int mark_pfn(unsigned long pfn, bool write)
{
	ipt_leaf_entry *leaf_pte;
	unsigned long flags;
	int ret = 0;

	spin_lock_irqsave(&ipt.ipt_lock, flags);

	leaf_pte = pfn_to_ipt_pte(pfn, true);

	if (leaf_pte == NULL) {
		pvdma_dbg("leaf pte for pfn 0x%lx is NULL.\n", pfn);
		ret = -1;
		goto out;
	}

	if ((atomic_inc_return(&leaf_pte->ipte) & IPTE_MAP_CNT_MASK) ==
						IPTE_MAP_CNT_MAX) {
		pvdma_dbg("%d maps already performed on pfn 0x%lx.\n",
				  IPTE_MAP_CNT_MAX, pfn);
		ret = -1;
		goto out;
	}

	if (write)
		pvdma_set_flag(IPTE_WRITEABLE_FLAG, leaf_pte);

	pvdma_set_flag(IPTE_ACCESSED_FLAG, leaf_pte);

out:
	spin_unlock_irqrestore(&ipt.ipt_lock, flags);

	return ret;
}

static int unmark_pfn(unsigned long pfn, bool clear_accessed)
{
	ipt_leaf_entry *leaf_pte;
	int ret = 0;

	leaf_pte = pfn_to_ipt_pte(pfn, false);
	if (leaf_pte == NULL) {
		pvdma_dbg("leaf pte for pfn 0x%lx is NULL.\n", pfn);
		ret = -1;
		goto out;
	}

	if (!(atomic_read(&leaf_pte->ipte) & IPTE_MAP_CNT_MASK)) {
		pvdma_dbg("map count already zero on pfn 0x%lx.\n", pfn);
		ret = -1;
		goto out;
	}

	if (!(atomic_dec_return(&leaf_pte->ipte) & IPTE_MAP_CNT_MASK)) {
		pvdma_clear_flag(IPTE_WRITEABLE_FLAG, leaf_pte);

		if (unlikely(clear_accessed))
			pvdma_clear_flag(IPTE_ACCESSED_FLAG, leaf_pte);
	}

out:
	return ret;
}

static void unmark_pfns(unsigned long pfn, unsigned long nr_pages,
						bool clear_accessed)
{
	unsigned long count;

	for (count = 0; count < nr_pages; count++)
		unmark_pfn(pfn+count, clear_accessed);
}

static inline unsigned long get_aligned_nrpages(unsigned long offset,
						size_t size)
{
	return PAGE_ALIGN(offset + size) >> PAGE_SHIFT;
}

static inline unsigned short get_pci_device_id(struct device *dev)
{
	struct pci_dev *pdev = to_pci_dev(dev);

	return PCI_DEVID(pdev->bus->number, pdev->devfn);
}

int pin_page_for_device(unsigned long pfn, unsigned short bdf)
{
	return 0;
}

static int pin_and_mark_pfns(unsigned long start_pfn, unsigned short bdf,
					unsigned long nr_pages, bool write)
{
	int ret = 0;
	unsigned long count;

	for (count = 0; count < nr_pages; count++) {
		ret = mark_pfn(start_pfn + count, write);
		if (ret == -1) {
			unmark_pfns(start_pfn, count, true);
			goto out;
		}

		if (!is_page_pinned(start_pfn + count)) {
			ret = pin_page_for_device(start_pfn + count, bdf);
			if (unlikely(ret != 0)) {
				/* Why + 1 here? */
				unmark_pfns(start_pfn, count + 1, true);
				goto out;
			}
		}
	}

out:
	return ret;
}

static int pin_and_mark_sg_list(struct scatterlist *sgl,
			 int nents, unsigned short bdf, bool write)
{
	struct scatterlist *sg;
	unsigned long nr_pages = 0;
	phys_addr_t phys_addr;
	unsigned long pfn;
	int i, ret = 0;

	for_each_sg(sgl, sg, nents, i) {
		BUG_ON(!sg_page(sg));
		phys_addr = sg_phys(sg);
		pfn = phys_addr >> PAGE_SHIFT;
		nr_pages = get_aligned_nrpages(phys_addr & (PAGE_SIZE - 1),
								sg->length);

		ret = pin_and_mark_pfns(pfn, bdf, nr_pages, write);
		if (unlikely(ret != 0))
			break;
	}

	return ret;
}

/* allocate and map a coherent mapping */
static void *pv_dma_alloc(struct device *dev, size_t size,
			   dma_addr_t *dma_addr, gfp_t gfp,
			   unsigned long attrs)
{
	void *cpu_addr = NULL;
	phys_addr_t phys_addr;
	unsigned short bdf = get_pci_device_id(dev);
	unsigned long nr_pages;
	unsigned long pfn;
	int ret;

	cpu_addr = dma_direct_alloc(dev, size, dma_addr, gfp, attrs);

	if (!cpu_addr || is_emulated_dev(bdf))
		return cpu_addr;

	phys_addr = dma_to_phys(dev, *dma_addr);
	nr_pages = get_aligned_nrpages(0, size);
	pfn = phys_addr >> PAGE_SHIFT;

	ret = pin_and_mark_pfns(pfn, bdf, nr_pages, true);

	if (unlikely(ret == -1))
		goto out_free;

	return cpu_addr;

out_free:
	dma_direct_free(dev, size, cpu_addr, *dma_addr, attrs);
	return NULL;
}

/* free a coherent mapping */
static void pv_dma_free(struct device *dev, size_t size, void *cpu_addr,
			dma_addr_t dma_addr, unsigned long attrs)
{
	phys_addr_t phys_addr = dma_to_phys(dev, dma_addr);
	unsigned long pfn = phys_addr >> PAGE_SHIFT;
	unsigned long nr_pages = get_aligned_nrpages(0, size);

	dma_direct_free(dev, size, cpu_addr, dma_addr, attrs);

	if (!is_emulated_dev(get_pci_device_id(dev)))
		unmark_pfns(pfn, nr_pages, false);
}

static dma_addr_t pv_dma_map_page(struct device *dev, struct page *page,
		unsigned long offset, size_t size, enum dma_data_direction dir,
		unsigned long attrs)
{
	dma_addr_t  dma_addr;
	phys_addr_t phys_addr;
	unsigned short bdf = get_pci_device_id(dev);
	unsigned long nr_pages;
	unsigned long pfn;
	int ret;

	dma_addr = dma_direct_map_page(dev, page, offset, size, dir, attrs);
	if ((dma_addr == DMA_MAPPING_ERROR) || is_emulated_dev(bdf))
		return dma_addr;

	phys_addr = dma_to_phys(dev, dma_addr);
	nr_pages = get_aligned_nrpages(phys_addr & (PAGE_SIZE - 1), size);
	pfn = phys_addr >> PAGE_SHIFT;

	ret = pin_and_mark_pfns(pfn, bdf, nr_pages,
			(dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL));
	if (unlikely(ret == -1))
		goto out_unmap;
	return dma_addr;

out_unmap:
	dma_direct_unmap_page(dev, dma_addr, size, dir, attrs);
	return DMA_MAPPING_ERROR;
}

void pv_dma_unmap_page(struct device *dev, dma_addr_t addr, size_t size,
		       enum dma_data_direction dir, unsigned long attrs)
{
	phys_addr_t phys_addr = dma_to_phys(dev, addr);
	unsigned long pfn = phys_addr >> PAGE_SHIFT;
	unsigned long nr_pages =
			get_aligned_nrpages(phys_addr & ~PAGE_MASK, size);

	dma_direct_unmap_page(dev, addr, size, dir, attrs);

	if (!is_emulated_dev(get_pci_device_id(dev)))
		unmark_pfns(pfn, nr_pages, false);
}

static int pv_dma_map_sg(struct device *dev, struct scatterlist *sgl,
			 int nents, enum dma_data_direction dir,
			 unsigned long attrs)
{
	unsigned short bdf = get_pci_device_id(dev);
	int ret;

	nents = dma_direct_map_sg(dev, sgl, nents, dir, attrs);

	if (!nents || is_emulated_dev(bdf))
		return nents;

	ret = pin_and_mark_sg_list(sgl, nents, bdf,
		(dir == DMA_FROM_DEVICE || dir == DMA_BIDIRECTIONAL));

	if (unlikely(ret == -1))
		goto out_unmap;

	return nents;

 out_unmap:
	dma_direct_unmap_sg(dev, sgl, nents, dir,
				attrs | DMA_ATTR_SKIP_CPU_SYNC);
	return 0;
}

static void pv_dma_unmap_sg(struct device *dev, struct scatterlist *sgl,
			    int nents, enum dma_data_direction dir,
			    unsigned long attrs)
{
	struct scatterlist *sg;
	phys_addr_t phys_addr;
	unsigned long pfn;
	unsigned long nr_pages;
	int i;

	dma_direct_unmap_sg(dev, sgl, nents, dir, attrs);

	if (!is_emulated_dev(get_pci_device_id(dev))) {
		for_each_sg(sgl, sg, nents, i) {
			BUG_ON(!sg_page(sg));

			phys_addr = sg_phys(sg);
			pfn = phys_addr >> PAGE_SHIFT;
			nr_pages =
				get_aligned_nrpages(phys_addr & (PAGE_SIZE - 1),
								sg->length);
			unmark_pfns(pfn, nr_pages, false);
		}
	}
}

static const struct dma_map_ops pv_dma_ops = {
	.alloc			= pv_dma_alloc,
	.free			= pv_dma_free,
	.map_sg			= pv_dma_map_sg,
	.unmap_sg       = pv_dma_unmap_sg,
	.map_page		= pv_dma_map_page,
	.unmap_page     = pv_dma_unmap_page,
	.dma_supported = dma_direct_supported,
};

int pvdma_enable(void)
{
	if (pvdma == PV_DMA_FORCE_DISABLE)
		return -1;

	dma_ops = &pv_dma_ops;

	pvdma_ipt_page_cache_alloc();

	ipt.ipt_root = pvdma_ipt_alloc_page();
	ipt.ipt_level = get_ipt_level();
	spin_lock_init(&ipt.ipt_lock);

	return 0;
}

int pvdma_disable(void)
{
	if (pvdma != PV_DMA_ENABLE)
		return -1;

	dma_ops = NULL;
	pvdma_ipt_page_cache_free();

	return 0;
}
EXPORT_SYMBOL(pv_dma_ops);
