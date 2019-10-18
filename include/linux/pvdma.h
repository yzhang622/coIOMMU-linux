/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __LINUX_PVDMA_H
#define __LINUX_PVDMA_H

#include <linux/dma-direction.h>
#include <linux/init.h>
#include <linux/types.h>

#ifdef CONFIG_PVDMA

#define PVDMA_CMD_GET_DEV_BITMAP 1

typedef union PVDMA_MMMIO_Info {
	uint64_t regs[4];
	struct PVDMA_Info {
		uint64_t ipt_addr;
		uint64_t command;
		uint64_t ipt_level;
		uint64_t gfn_bdf;
	} info;
} PVDMA_MMMIO_Info;

#define PIN_PAGES_IN_BATCH (1UL<<63)

typedef struct pin_pages_info {
	unsigned short	bdf;
	unsigned short	pad[3];
	unsigned long	nr_pages;
	uint64_t	pfn[0];
} pin_pages_info;

#define PVDMA_INFO_NR_OBJS 64

typedef struct {
	int nobjs;
	void *objects[PVDMA_INFO_NR_OBJS];
} pvdma_info_cache;

typedef struct {
	spinlock_t ipt_lock;
	uint64_t *ipt_root;
	unsigned int ipt_level;
	pvdma_info_cache pvdma_ipt_cache;
} pvdma_ipt;

extern pvdma_ipt ipt;

#define IPTE_MAP_CNT_MASK	0xFFFF
#define IPTE_MAP_CNT_MAX	0xFF
#define IPTE_PINNED_FLAG	16
#define IPTE_MAPPED_FLAG	17
#define IPTE_ACCESSED_FLAG	18
#define IPTE_WRITEABLE_FLAG	19
//#define IPTE_DIRTY_FLAG	20

typedef struct {
	atomic_t ipte;
} ipt_leaf_entry;

typedef struct {
	uint64_t present:1;
	uint64_t reserve:11;
	uint64_t pfn:52;
} ipt_parent_entry;

#define pvdma_set_flag(flag, iptep) \
	set_bit(flag, (unsigned long *)iptep)

#define pvdma_clear_flag(flag, iptep) \
	clear_bit(flag, (unsigned long *)iptep)

#define pvdma_test_flag(flag, iptep) \
	test_bit(flag, (unsigned long *)iptep)

#define PVDMA_PIN_SUCCESS	0
#define PVDMA_PIN_FAIL		-1
#define PVDMA_PIN_EMU_DEV	-2

extern int pvdma_enable(void);
extern int pin_page_for_device(unsigned long pfn, unsigned short bdf);
extern int pin_page_list_for_device(pin_pages_info *pin_info);
extern bool is_page_pinned(unsigned long pfn);

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
