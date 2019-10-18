/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PVDMA_H
#define __LINUX_PVDMA_H

#include <linux/dma-direction.h>
#include <linux/init.h>
#include <linux/types.h>

#ifdef CONFIG_PVDMA
extern int pvdma_enable(void);

#define MAX_NUM_DEVICES (1 << 16)
extern void *device_bitmap;

#define is_emulated_dev(bdf) \
	((device_bitmap != NULL) && test_bit(bdf, device_bitmap))

#define pvdma_dbg(fmt, s...) \
	do { \
		printk(KERN_ERR "PVDMA: [%s(), line%d]: " fmt, \
		       __func__, __LINE__, ##s); \
	} while (0)

#else
extern int pvdma_enable(void)
{
	return 0;
}
#endif /* CONFIG_PVDMA */

#endif /* __LINUX_PVDMA_H */
