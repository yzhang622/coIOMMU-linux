// SPDX-License-Identifier: GPL-2.0

#include <linux/pci.h>
#include <linux/init.h>
#include <linux/dmar.h>
#include <linux/pvdma.h>
#include <linux/dma-direct.h>

#include <asm/iommu.h>
#include <asm/pvdma.h>
#include <asm/dma.h>
#include <asm/kvm_para.h>
#include <asm/xen/swiotlb-xen.h>
#include <asm/iommu_table.h>

int pvdma __read_mostly = PV_DMA_ENABLE;

static int __init setup_pvdma(char *str)
{
	if (*str == ',')
		++str;

	if (!strcmp(str, "off"))
		pvdma = PV_DMA_FORCE_DISABLE;

	return 0;
}
early_param("pvdma", setup_pvdma);

int __init pci_pvdma_detect(void)
{
	if (pvdma == PV_DMA_FORCE_DISABLE)
		return 0;

	if (!kvm_para_available() || iommu_detected)
		pvdma = PV_DMA_FORCE_DISABLE;

	return pvdma;
}

void __init pci_pvdma_late_init(void)
{
	/* An IOMMU turned us off. */
	if (pvdma != PV_DMA_ENABLE)
		return;

	if (pvdma_enable())
		printk(KERN_INFO "PCI-PVDMA: failed to enable PVDMA.\n");
	else
		printk(KERN_INFO "PCI-PVDMA: Using software PVDMA.\n");
}

IOMMU_INIT(pci_pvdma_detect,
	   detect_intel_iommu,
	   NULL,
	   pci_pvdma_late_init);
