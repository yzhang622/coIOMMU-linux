/* SPDX-License-Identifier: GPL-2.0 */

#ifndef _ASM_X86_PVDMA_H

#define _ASM_X86_PVDMA_H
#ifdef CONFIG_PVDMA
extern int pvdma;
extern int __init pci_pvdma_detect(void);
extern void __init pci_pvdma_init(void);
extern void __init pci_pvdma_late_init(void);

enum {
	PV_DMA_FORCE_DISABLE = 0,
	PV_DMA_ENABLE,
};

#else
#define pvdma 0
static inline int pci_pvdma_detect(void)
{
	return 0;
}
static inline void pci_pvdma_init(void)
{
}
static inline void pci_pvdma_late_init(void)
{
}
#endif

#endif /* _ASM_X86_PVDMA_H */
