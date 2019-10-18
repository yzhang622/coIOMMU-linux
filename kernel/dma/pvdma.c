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

void * device_bitmap;

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

static void unmark_pfns(unsigned long pfn, unsigned long nr_pages)
{
}


static int pin_and_mark_pfns(unsigned long start_pfn, unsigned short bdf,
					unsigned long nr_pages, bool write)
{
	return 0;
}


static int pin_and_mark_sg_list(struct scatterlist *sgl,
			 int nents, unsigned short bdf, bool write)
{
	return 0;
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
		unmark_pfns(pfn, nr_pages);
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
		unmark_pfns(pfn, nr_pages);
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
			unmark_pfns(pfn, nr_pages);
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

	return 0;
}

int pvdma_disable(void)
{

	if (pvdma != PV_DMA_ENABLE)
		return -1;

	dma_ops = NULL;

	return 0;
}
EXPORT_SYMBOL(pv_dma_ops);
