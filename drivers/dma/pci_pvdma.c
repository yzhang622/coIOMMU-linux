#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/init.h>
#include <linux/sizes.h>
#include <linux/pvdma.h>


#define PCI_PVDMA_DEVICE_NAME "pvdma-pci"

#define PCI_VENDOR_ID_QEMU               0x1234
#define PCI_DEVICE_ID_PVDMA              0xabcd

PVDMA_MMMIO_Info *pvdma_mmio_info;

static struct pci_device_id pci_pvdma_ids[] = {
	{ PCI_DEVICE(PCI_VENDOR_ID_QEMU, PCI_DEVICE_ID_PVDMA), },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, pci_pvdma_ids);

int pin_page_list_for_device(pin_pages_info *pin_info)
{
	int ret = 0;
	u64 pin_data;
	u64 count;

	if (!pvdma_mmio_info) {
		pvdma_dbg("pvdma_mmio_info is NULL.\n");
		return PVDMA_PIN_FAIL;
	}

	pin_data = (__pa(pin_info)) | PIN_PAGES_IN_BATCH;

	pvdma_mmio_info->info.gfn_bdf = pin_data;

	if (test_bit(pin_info->bdf, device_bitmap))
		return PVDMA_PIN_EMU_DEV;

	for (count = 0; count < pin_info->nr_pages; count++) {
		if (!is_page_pinned(pin_info->pfn[count])) {
			ret = PVDMA_PIN_FAIL;
			break;
		}
	}

	return ret;
}

int pin_page_for_device(unsigned long pfn, unsigned short bdf)
{
	int ret;
	u64 gfn_bdf;

	if (!pvdma_mmio_info) {
		pvdma_dbg("pvdma_mmio_info is NULL.\n");
		return -1;
	}

	gfn_bdf = (pfn << 16) | bdf;

	pvdma_mmio_info->info.gfn_bdf = gfn_bdf;

	if (test_bit(bdf, device_bitmap))
		ret = PVDMA_PIN_EMU_DEV;
	else if (!is_page_pinned(pfn))
		ret = PVDMA_PIN_FAIL;
	else
		ret = PVDMA_PIN_SUCCESS;

	return ret;
}

static unsigned char pvdma_get_revision(struct pci_dev *dev)
{
	u8 revision;

	pci_read_config_byte(dev, PCI_REVISION_ID, &revision);
	return revision;
}

static int pci_pvdma_probe(struct pci_dev *dev, const struct pci_device_id *id)
{
	u8 revision;
	int ret;

	ret = pci_enable_device(dev);
	if (ret) {
		pvdma_dbg("pci_enable_device() failed, ret=%d\n", ret);
		return ret;
	}

	revision = pvdma_get_revision(dev);
	if (revision != 0x10)
		return -ENODEV;

	pvdma_mmio_info = pci_iomap(dev, 0, sizeof(PVDMA_MMMIO_Info));
	pvdma_mmio_info->info.ipt_addr = __pa(ipt.ipt_root);
	pvdma_mmio_info->info.command = PVDMA_CMD_GET_DEV_BITMAP;
	pvdma_mmio_info->info.ipt_level = ipt.ipt_level;

	device_bitmap = pci_iomap(dev, 2, (1<<16)/8);
	if (device_bitmap)
		pvdma_dbg("device bitmap initialized at 0x%lx.\n", device_bitmap);

	return 0;
}

static void pci_pvdma_remove(struct pci_dev *dev)
{
	/* clean up any allocated resources and stuff here.
	 * like call release_region();
	 */
}

static struct pci_driver pci_pvdma_driver = {
	.name = PCI_PVDMA_DEVICE_NAME,
	.id_table = pci_pvdma_ids,
	.probe = pci_pvdma_probe,
	.remove = pci_pvdma_remove,
};

static int __init pci_pvdma_init(void)
{
	return pci_register_driver(&pci_pvdma_driver);
}

static void __exit pci_pvdma_exit(void)
{
	pci_unregister_driver(&pci_pvdma_driver);
}

MODULE_LICENSE("GPL");

module_init(pci_pvdma_init);
module_exit(pci_pvdma_exit);
