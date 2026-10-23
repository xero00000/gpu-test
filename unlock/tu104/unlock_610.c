// SPDX-License-Identifier: GPL-2.0-only
/*
 * Pre-driver V67 unlock, run before the stock driver binds: native V2 FWSEC WITH_LOADER,
 * Turing Booter BOOT_DIRECT and the pinned compute/cleanup chain. Both Falcons
 * must halt and WPR2 must be down before releasing any firmware-visible DMA.
 */
#include <linux/aperture.h>
#include <linux/unaligned.h>
#include <linux/delay.h>
#include <linux/dma-mapping.h>
#include <linux/firmware.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/slab.h>
#include <linux/timekeeping.h>

#include "diagnostic.h"
#include "gfw_eval.h"
#include "prep_610.h"
#include "unlock_610.h"
#include "vbios_610.h"

#define SEC2_BASE 0x00840000u
#define SEC2_FBIF 0x00840600u
#define SEC2_FALCON2 0x00841000u
/* SS0 holds eight 4-bit fields; SS1 stores only its low 4 bits (0x2 when
 * cold). Both get the same per-field value 8. */
#define GU_SS0_UNLOCK 0x88888888u
#define GU_SS1_UNLOCK 0x8u
#define BL_FILE "gpu-unlock-tu104/610.43.03/gsp_bl_tu102.bin"
#define ROM_FILE "gpu-unlock-tu104/610.43.03/board-rom-tu104.bin"
#define BOOTER_MAGIC 0x314c5442u
#define BOOTER_FILE "gpu-unlock-tu104/610.43.03/booter_load_tu102.bin"
#define V67_FILE "gpu-unlock-tu104/610.43.03/v67-tu102-610.43.03.bin"
#define GSP_FILE "gpu-unlock-tu104/610.43.03/gsp_tu10x.bin"
#define BOOT_FILE "gpu-unlock-tu104/610.43.03/gsp_boot_tu102.bin"
#define BOOT_DESC_FILE "gpu-unlock-tu104/610.43.03/gsp_boot_desc_tu102.bin"

struct unlock_transaction_page {
	void *cpu;
	dma_addr_t dma;
};

struct unlock_transaction_work {
	struct pci_dev *pdev;
	void __iomem *bar;
	resource_size_t bar_len;
	u16 original_cmd;
	bool cmd_valid;
	bool enabled;
	bool region;
	bool mapped;
	bool master;
	const struct firmware *booter_fw;
	const struct firmware *v67_fw;
	const struct firmware *gsp_fw;	/* borrowed from gpu_unlock_610_shared */
	const struct firmware *boot_fw;
	struct gpu_unlock_span image_span, sig_span;
	struct gpu_unlock_layout_in layout_in;
	struct gpu_unlock_layout_out layout;
	struct gpu_unlock_boot_desc boot_desc;
	struct gpu_unlock_radix_plan radix;
	struct unlock_transaction_page *pages;
	u32 page_count, pages_allocated;
	void *boot_cpu;
	dma_addr_t boot_dma;
	void *booter_cpu;
	dma_addr_t booter_dma;
	u32 booter_image_size;
	struct gpu_unlock_booter_hdr booter_hdr;
	void *v67_cpu;
	dma_addr_t v67_dma;
	void *stock_cpu;
	dma_addr_t stock_dma;
	u64 stock_size;
	void *meta_cpu;
	dma_addr_t meta_dma;
	u64 *streaming_ptr;
	u64 streaming_before;
	u64 coherent_before;
	bool streaming_set;
	bool coherent_set;
	unsigned int signature_calls;
	bool published;
	bool masks_saved;
	bool firmware_started;
	bool quiesced;
	bool retained;
	u32 plm_entry;
	u32 bars[6];
	void *fwsec_cpu;
	dma_addr_t fwsec_dma;
	u32 fwsec_size;
	u32 last_offset, last_value;
	bool last_valid;
	u32 last_pci_offset, last_pci_value;
	int last_pci_error;
	bool last_pci_cap;
};

static int unlock_transaction_result(struct unlock_transaction_work *work, int error,
			  const char *function, unsigned int line)
{
	if (error)
	{
		dev_err(&work->pdev->dev,
			"failure unlock_610.c:%u %s error=%d last_mmio=%#x value=%#x valid=%u mapped=%u master=%u firmware_started=%u quiesced=%u pages=%u/%u\n",
			line, function, error, work->last_offset, work->last_value,
			work->last_valid, work->mapped, work->master, work->firmware_started,
			work->quiesced, work->pages_allocated, work->page_count);
		dev_err(&work->pdev->dev, "last PCI sample: capability=%u offset=%#x value=%#x result=%d\n",
			work->last_pci_cap, work->last_pci_offset, work->last_pci_value, work->last_pci_error);
	}
	return error;
}
#define GU_RESULT(result) unlock_transaction_result(work, (result), __func__, __LINE__)

static int diag_pci_word(struct unlock_transaction_work *work, bool cap, int offset, u16 *value)
{
	int ret;
	*value = 0xffff;
	ret = cap ? pcie_capability_read_word(work->pdev, offset, value) :
		pci_read_config_word(work->pdev, offset, value);
	work->last_pci_cap = cap;
	work->last_pci_offset = offset;
	work->last_pci_value = *value;
	work->last_pci_error = ret;
	return ret;
}

static int diag_pci_dword(struct unlock_transaction_work *work, bool cap, int offset, u32 *value)
{
	int ret;
	*value = ~0u;
	ret = cap ? pcie_capability_read_dword(work->pdev, offset, value) :
		pci_read_config_dword(work->pdev, offset, value);
	work->last_pci_cap = cap;
	work->last_pci_offset = offset;
	work->last_pci_value = *value;
	work->last_pci_error = ret;
	return ret;
}

static void *diag_dma_alloc(struct unlock_transaction_work *work, const char *name,
			   size_t size, dma_addr_t *dma, unsigned int flags)
{
	void *memory = dma_alloc_coherent(&work->pdev->dev, size, dma, flags);
	if (!memory)
		dev_err(&work->pdev->dev, "DMA allocation failed: asset=%s bytes=%zu pages_allocated=%u requested=%u\n",
			name, size, work->pages_allocated, work->page_count);
	return memory;
}

/* CONFIG_FW_CACHE attaches name records to devres even though this is not
 * devm_request_firmware(). release_firmware() does not remove them, and
 * really_probe() refuses a device with any existing devres. Scope each
 * synchronous request so only its cache records are released; the returned
 * firmware remains owned by the caller.
 */
static int request_scoped(struct device *dev, const struct firmware **fw, const char *name)
{
	void *group;
	int ret, released;

	group = devres_open_group(dev, NULL, GFP_KERNEL);
	if (!group)
		return -ENOMEM;
	dev_info(dev, "asset request: %s\n", name);
	ret = request_firmware(fw, name, dev);
	released = devres_release_group(dev, group);
	dev_info(dev, "asset result: %s error=%d bytes=%zu\n",
		name, ret, ret ? (size_t)0 : (*fw)->size);
	dev_info(dev, "asset cache cleanup: %s devres_released=%d\n", name, released);
	return ret;
}

static int request_asset(struct unlock_transaction_work *work, const struct firmware **fw,
			 const char *name)
{
	return GU_RESULT(request_scoped(&work->pdev->dev, fw, name));
}

static int check_devres_empty(struct unlock_transaction_work *work, const char *phase)
{
	struct device *dev = &work->pdev->dev;
	unsigned long flags;
	bool empty;

	/* The caller also holds the device lock across the whole transaction. */
	spin_lock_irqsave(&dev->devres_lock, flags);
	empty = list_empty(&dev->devres_head);
	spin_unlock_irqrestore(&dev->devres_lock, flags);
	dev_info(dev, "driver probe resources: phase=%s empty=%u\n", phase, empty);
	return GU_RESULT(empty ? 0 : -EBUSY);
}

static int read_bar(struct unlock_transaction_work *work, u32 offset, u32 *value)
{
	work->last_offset = offset;
	work->last_valid = false;
	if (!work->mapped || offset > work->bar_len || work->bar_len - offset < 4)
		return GU_RESULT(-EINVAL);
	*value = readl(work->bar + offset);
	work->last_value = *value;
	work->last_valid = true;
	return 0;
}

static void write_bar(struct unlock_transaction_work *work, u32 offset, u32 value)
{
	writel(value, work->bar + offset);
}

static int poll_us(struct unlock_transaction_work *work, u32 offset, u32 mask, u32 expect,
		   unsigned int budget_us, bool timeout_is_failure)
{
	ktime_t deadline = ktime_add_us(ktime_get(), budget_us);
	u32 value = 0;

	for (;;) {
		if (read_bar(work, offset, &value) || value == ~0u ||
		    (value & 0xffff0000u) == 0xbadf0000u) {
			dev_err(&work->pdev->dev, "poll inaccessible: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return GU_RESULT(-EIO);
		}
		if ((value & mask) == expect)
			return 0;
		if (ktime_compare(ktime_get(), deadline) >= 0) {
			/* Console loglevel hides info. A tolerated timeout must not
			 * look like the failure that stopped the boot. */
			if (!timeout_is_failure) {
				dev_info(&work->pdev->dev, "poll timeout tolerated: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
					offset, value, mask, expect, budget_us);
				return -ETIMEDOUT;
			}
			dev_err(&work->pdev->dev, "poll timeout: reg=%#x last=%#x mask=%#x expected=%#x budget_us=%u\n",
				offset, value, mask, expect, budget_us);
			return GU_RESULT(-ETIMEDOUT);
		}
		usleep_range(50, 100);
	}
}

struct falcon_eng {
	u32 base;
	u32 fbif;
	u32 falcon2;
};

static bool inaccessible(u32 value)
{
	return value == ~0u || (value & 0xffff0000u) == 0xbadf0000u;
}

static int falcon_reset(struct unlock_transaction_work *work, const struct falcon_eng *eng)
{
	u32 engine, hwcfg, chip;
	unsigned int i;
	int ret;

	/* 610 kflcnPreResetWait_GA102 permits this timeout (HW bug 3419321). */
	dev_info(&work->pdev->dev, "Falcon reset begin: base=%#x\n", eng->base);
	ret = poll_us(work, eng->base + 0xf4, 1u << 31, 1u << 31, 1000, false);
	if (ret && ret != -ETIMEDOUT)
		return GU_RESULT(ret);
	if (ret)
		dev_info(&work->pdev->dev, "Falcon pre-reset timeout tolerated per HW bug 3419321\n");
	if (read_bar(work, eng->base + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-ENODEV);
	write_bar(work, eng->base + 0x3c0, engine | 1);
	for (i = 0; i < 10; i++)
		readl(work->bar + eng->base + 0x3c0);
	write_bar(work, eng->base + 0x3c0, engine & ~1u);
	for (i = 0; i < 10; i++)
		readl(work->bar + eng->base + 0x3c0);
	ret = poll_us(work, eng->base + 0xf4, 1u << 12, 0, 1000000, true);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, eng->base + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	if (read_bar(work, 0, &chip) || inaccessible(chip))
		return GU_RESULT(-ENODEV);
	write_bar(work, eng->base + 0x84, chip);
	dev_info(&work->pdev->dev, "Falcon reset done: base=%#x hwcfg=%#x chip=%#x\n", eng->base, hwcfg, chip);
	return 0;
}

/* Turing port-loading follows NVIDIA s_imemCopyTo_TU102 and
 * s_dmemCopyTo_TU102, not Ampere BOOT_FROM_HS/BROM DMA programming. */
static void port_imem(struct unlock_transaction_work *work, u32 base, u32 dest,
                      const u8 *src, u32 bytes, bool secure, u32 tag)
{
 u32 i;
 write_bar(work,base+0x180,dest | (1u<<24) | (secure ? 1u<<28 : 0));
 for (i=0;i<bytes;i+=4) {
  if (!(i&255)) write_bar(work,base+0x188,(tag+i)>>8);
  write_bar(work,base+0x184,get_unaligned_le32(src+i));
 }
 write_bar(work,base+0x180,0);
}
static void port_dmem(struct unlock_transaction_work *work, u32 base,
                      const u8 *src, u32 bytes)
{
 u32 i;
 write_bar(work,base+0x1c0,1u<<24);
 for (i=0;i<bytes;i+=4) write_bar(work,base+0x1c4,get_unaligned_le32(src+i));
 write_bar(work,base+0x1c0,0);
}
static int start_wait(struct unlock_transaction_work *work, u32 base)
{
 u32 cpuctl;
 dma_wmb();
 if (read_bar(work,base+0x100,&cpuctl) || inaccessible(cpuctl)) return GU_RESULT(-ENODEV);
 work->firmware_started=true;
 if (cpuctl&(1u<<6)) write_bar(work,base+0x130,2);
 else write_bar(work,base+0x100,2);
 return poll_us(work,base+0x100,0x10,0x10,5000000,true);
}
static int fwsec_with_loader(struct unlock_transaction_work *work,
                            const struct falcon_eng *gsp,
                            const struct gpu_unlock_fwsec *fw, dma_addr_t image)
{
 const struct firmware *bl=NULL;
 u32 desc[21]={0},hwcfg,trans,dest;
 int ret=request_asset(work,&bl,BL_FILE);
 if (ret) return ret;
 if (bl->size!=768) {ret=-EINVAL;goto out;}
 ret=falcon_reset(work,gsp);
 if (ret) goto out;
 if (read_bar(work,gsp->fbif+0x24,&trans) || inaccessible(trans)) {ret=-ENODEV;goto out;}
 /* HWCFG is +0x108; HWCFG2 is +0xf4. */
 if (read_bar(work,gsp->base+0x108,&hwcfg) || inaccessible(hwcfg) || (hwcfg&0x1ff)<2) {ret=-ENODEV;goto out;}
 dest=((hwcfg&0x1ff)-2)<<8;
 write_bar(work,gsp->fbif+0x24,trans|(1u<<7));
 write_bar(work,gsp->base+0x10c,0);
 if (read_bar(work,gsp->fbif+4*4,&trans) || inaccessible(trans)) {ret=-ENODEV;goto out;}
 write_bar(work,gsp->fbif+4*4,(trans&~7u)|5u); /* TU102 dev_fbif_v4: TARGET=1, PHYSICAL bit 2; retain other fields */
 desc[8]=4;desc[9]=lower_32_bits(image);desc[10]=upper_32_bits(image);
 desc[11]=fw->imem_pa;desc[12]=fw->ns_size;desc[13]=fw->sec_pa;desc[14]=fw->sec_size;
 desc[15]=0;desc[16]=lower_32_bits(image+fw->data_off);desc[17]=upper_32_bits(image+fw->data_off);desc[18]=fw->dmem_size;
 port_dmem(work,gsp->base,(const u8 *)desc,sizeof(desc));
 port_imem(work,gsp->base,dest,bl->data,512,false,0xfd00);
 write_bar(work,gsp->base+0x104,0xfd00);
 write_bar(work,gsp->base+0x40,0);write_bar(work,gsp->base+0x44,0);
 dev_info(&work->pdev->dev,"TU104 FWSEC WITH_LOADER: code_dma=%pad data=%#x ns=%#x sec=%#x+%#x BL_dst=%#x\n",&image,fw->data_off,fw->ns_size,fw->sec_pa,fw->sec_size,dest);
 ret=start_wait(work,gsp->base);
out:
 release_firmware(bl);
 return GU_RESULT(ret);
}

static int load_booter(struct unlock_transaction_work *work)
{
	const u8 *data;
	u32 magic, version, image_size, sig_bytes;
	struct gpu_unlock_booter_hdr hdr;
	int ret;

	ret = request_asset(work, &work->booter_fw, BOOTER_FILE);
	if (ret)
		return GU_RESULT(ret);
	data = work->booter_fw->data;
	if (work->booter_fw->size < 72)
		return GU_RESULT(-EINVAL);
	memcpy(&magic, data, sizeof(magic));
	memcpy(&version, data + 4, sizeof(version));
	if (magic != BOOTER_MAGIC || version != 1)
		return GU_RESULT(-EINVAL);
	memcpy(&hdr, data + 8, sizeof(hdr));
	memcpy(&image_size, data + 8 + sizeof(hdr), sizeof(image_size));
	memcpy(&sig_bytes, data + 12 + sizeof(hdr), sizeof(sig_bytes));
	if (!hdr.ucode_id || hdr.num_apps != 1 || sig_bytes != hdr.sig_bytes ||
	    image_size != GU_BOOTER_IMAGE_SIZE ||
	    work->booter_fw->size != 72u + image_size + sig_bytes)
		return GU_RESULT(-EINVAL);
	/* Only this pinned production BOOT_FROM_HS geometry is implemented. */
	if (hdr.ucode_id != 13 || hdr.engine_id != 1 || hdr.num_sigs != 1 ||
	    sig_bytes != GU_BOOTER_SIG_TOTAL || hdr.os_code_off != 0 ||
	    hdr.os_code_size != 256 || hdr.os_data_off != 34048 ||
	    hdr.os_data_size != 25088 || hdr.app_code_off != 256 ||
	    hdr.app_code_size != 33792 || hdr.app_data_off != 256 ||
	    hdr.app_data_size != 0 || hdr.patch_loc != 34560)
		return GU_RESULT(-EINVAL);
	work->booter_hdr = hdr;
	work->booter_image_size = image_size;
	/* Allocation follows the ownership and DMA-mask snapshot, not asset lookup. */
	return 0;
}

static int patch_booter(struct unlock_transaction_work *work)
{
 /* The pinned TU102-family Booter has one 16-byte production signature;
  * it does not use Ampere's revocation-fuse signature selection. */
 return GU_RESULT(gpu_unlock_booter_patch(work->booter_cpu,work->booter_image_size,
  &work->booter_hdr,work->booter_fw->data+72+work->booter_image_size,16,0));
}

static void free_coherent(struct unlock_transaction_work *work)
{
	u32 i;

	for (i = 0; i < work->pages_allocated; i++)
		dma_free_coherent(&work->pdev->dev, GU_RADIX_PAGE,
				  work->pages[i].cpu, work->pages[i].dma);
	kvfree(work->pages);
	work->pages = NULL;
	work->pages_allocated = 0;
	if (work->boot_cpu)
		dma_free_coherent(&work->pdev->dev, work->boot_desc.image_size,
				  work->boot_cpu, work->boot_dma);
	work->boot_cpu = NULL;
	if (work->fwsec_cpu) {
		dma_free_coherent(&work->pdev->dev, work->fwsec_size,
				  work->fwsec_cpu, work->fwsec_dma);
		work->fwsec_cpu = NULL;
	}
	if (work->booter_cpu)
		dma_free_coherent(&work->pdev->dev, GU_BOOTER_IMAGE_SIZE,
				  work->booter_cpu, work->booter_dma);
	if (work->v67_cpu)
		dma_free_coherent(&work->pdev->dev, GU_V67_SIZE, work->v67_cpu,
				  work->v67_dma);
	if (work->stock_cpu)
		dma_free_coherent(&work->pdev->dev, work->stock_size,
				  work->stock_cpu, work->stock_dma);
	if (work->meta_cpu)
		dma_free_coherent(&work->pdev->dev, GU_WPR_META_SIZE,
				  work->meta_cpu, work->meta_dma);
	work->booter_cpu = NULL;
	work->v67_cpu = NULL;
	work->stock_cpu = NULL;
	work->meta_cpu = NULL;
}

static int unlock_transaction_unwind(struct unlock_transaction_work *work)
{
	u16 now, want;
	int ret = 0;

	/* A timeout never authorizes freeing firmware-visible memory. */
	if (work->firmware_started && !work->quiesced) {
		work->retained = true;
		return GU_RESULT(-EBUSY);
	}
	if (work->master) {
		pci_clear_master(work->pdev);
		work->master = false;
	}
	if (work->mapped) {
		pci_iounmap(work->pdev, work->bar);
		work->bar = NULL;
		work->mapped = false;
	}
	if (work->region) {
		pci_release_region(work->pdev, 0);
		work->region = false;
	}
	if (work->enabled) {
		pci_disable_device(work->pdev);
		work->enabled = false;
	}
	free_coherent(work);
	if (work->booter_fw) {
		release_firmware(work->booter_fw);
		work->booter_fw = NULL;
	}
	if (work->v67_fw) {
		release_firmware(work->v67_fw);
		work->v67_fw = NULL;
	}
	if (work->boot_fw) {
		release_firmware(work->boot_fw);
		work->boot_fw = NULL;
	}
	/* Match the driver core's entry condition, not just BAR/mask cleanup.
	 * Never release unrelated resources to make this check pass.
	 */
	if (check_devres_empty(work, "handoff"))
		ret = -EBUSY;
	if (work->streaming_set && work->streaming_ptr) {
		if (work->pdev->dev.dma_mask != work->streaming_ptr)
			return GU_RESULT(-EIO);
		if (dma_set_mask(&work->pdev->dev, work->streaming_before) ||
		    *work->streaming_ptr != work->streaming_before)
			ret = -EIO;
		dev_info(&work->pdev->dev, "restore streaming mask: actual=%#llx expected=%#llx\n",
			(unsigned long long)*work->streaming_ptr, (unsigned long long)work->streaming_before);
	}
	if (work->coherent_set) {
		if (dma_set_coherent_mask(&work->pdev->dev, work->coherent_before) ||
		    work->pdev->dev.coherent_dma_mask != work->coherent_before)
			ret = -EIO;
		dev_info(&work->pdev->dev, "restore coherent mask: actual=%#llx expected=%#llx\n",
			(unsigned long long)work->pdev->dev.coherent_dma_mask,
			(unsigned long long)work->coherent_before);
	}
	if (!work->cmd_valid)
		return GU_RESULT(ret);
	if (diag_pci_word(work, false, PCI_COMMAND, &now) || now == 0xffff)
		return GU_RESULT(-ENODEV);
	want = gpu_unlock_command_restore(work->original_cmd, now);
	if (want != now && pci_write_config_word(work->pdev, PCI_COMMAND, want))
		return GU_RESULT(-EIO);
	if (diag_pci_word(work, false, PCI_COMMAND, &now))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "restore PCI command: actual=%#x original=%#x mask=%#x\n",
		now, work->original_cmd, GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER);
	if ((now & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)) !=
	    (work->original_cmd & (GU_PCI_COMMAND_MEMORY | GU_PCI_COMMAND_MASTER)))
		return GU_RESULT(-EIO);
	return GU_RESULT(ret);
}

static int unlock_transaction_assets(struct unlock_transaction_work *work)
{
	const struct firmware *desc;
	int ret;

	ret = check_devres_empty(work, "entry");
	if (ret)
		return ret;
	ret = request_asset(work, &work->v67_fw, V67_FILE);
	if (ret)
		return GU_RESULT(ret);
	if (work->v67_fw->size != GU_V67_SIZE)
		return GU_RESULT(-EBADMSG);
	ret = gpu_unlock_elf_section(work->gsp_fw->data, work->gsp_fw->size,
				    ".fwimage", &work->image_span);
	if (ret)
		return GU_RESULT(ret);
	ret = gpu_unlock_elf_section(work->gsp_fw->data, work->gsp_fw->size,
				    ".fwsignature_tu10x", &work->sig_span);
	if (ret)
		return GU_RESULT(ret);
	if (!work->image_span.size || work->image_span.size > 128ull << 20 ||
	    work->sig_span.size != 4096)
		return GU_RESULT(-EINVAL);
	ret = gpu_unlock_radix_plan(work->image_span.size, &work->radix);
	if (ret)
		return GU_RESULT(ret);
	ret = request_asset(work, &work->boot_fw, BOOT_FILE);
	if (ret)
		return GU_RESULT(ret);
	if (work->boot_fw->size != 4096)
		return GU_RESULT(-EINVAL);
	ret = request_asset(work, &desc, BOOT_DESC_FILE);
	if (ret)
		return GU_RESULT(ret);
	ret = gpu_unlock_boot_desc_parse(desc->data, desc->size,
					work->boot_fw->size, &work->boot_desc);
	dev_info(&work->pdev->dev, "asset geometry: image=%llu signature=%llu boot=%u code=%#x data=%#x manifest=%#x parse=%d\n",
		(unsigned long long)work->image_span.size, (unsigned long long)work->sig_span.size,
		work->boot_desc.image_size, work->boot_desc.code_offset, work->boot_desc.data_offset,
		work->boot_desc.manifest_offset, ret);
	release_firmware(desc);
	return GU_RESULT(ret ? ret : load_booter(work));
}

static int unlock_transaction_layout(struct unlock_transaction_work *work)
{
 struct gpu_unlock_layout_in *in=&work->layout_in;
 u32 raw,vga,chip,scale,mag;
 int ret;
 if (read_bar(work,0,&chip) || chip!=0x164000a1 ||
     read_bar(work,0x00100ce0,&raw) || inaccessible(raw) ||
     read_bar(work,0x00625f04,&vga) || inaccessible(vga)) return GU_RESULT(-ENODEV);
 scale=raw&15;mag=(raw>>4)&63;
 in->usable_fb=(u64)mag<<(scale+20);
 if ((raw&(1u<<30)) || in->usable_fb!=(8ull<<30)) return GU_RESULT(-ERANGE);
 in->vga_base=in->usable_fb-(1ull<<20);
 if (vga&(1u<<3)) {
  in->vga_base=(u64)(vga>>8)<<16;
  if (in->vga_base<in->usable_fb-(1ull<<20)) in->vga_base=in->usable_fb-0x20000;
 }
 if (in->vga_base>=in->usable_fb) return GU_RESULT(-ERANGE);
 /* NVIDIA's TU104 HAL does not use GA100 MMU_LOCK reservations. The
  * captured middle-of-FB lock is the unscripted region, not a top carveout. */
 in->mmu_lock_valid=false;
 in->frts_size=1ull<<20;in->boot_size=work->boot_desc.image_size;
 in->image_size=work->image_span.size;in->nonwpr_size=1ull<<20;
 in->heap_request=105ull<<20;
 ret=gpu_unlock_layout(in,&work->layout);
 if (!ret && work->layout.nonwpr_start<in->usable_fb-(256ull<<20)) ret=-ERANGE;
 dev_info(&work->pdev->dev,"TU104 layout: memory_range=%#x fb=%#llx frts=%#llx start=%#llx end=%#llx result=%d\n",raw,(unsigned long long)in->usable_fb,(unsigned long long)work->layout.frts_offset,(unsigned long long)work->layout.nonwpr_start,(unsigned long long)work->layout.wpr_end,ret);
 return GU_RESULT(ret);
}

static int unlock_transaction_radix(struct unlock_transaction_work *work)
{
	const struct gpu_unlock_radix_plan *r = &work->radix;
	u64 *addresses;
	u32 i, level, first = 0;
	int ret = 0;

	work->page_count = r->table_pages + r->pages[3];
	work->pages = kvcalloc(work->page_count, sizeof(*work->pages), GFP_KERNEL);
	addresses = kvcalloc(work->page_count, sizeof(*addresses), GFP_KERNEL);
	if (!work->pages || !addresses) {
		kvfree(addresses);
		return GU_RESULT(-ENOMEM);
	}
	for (i = 0; i < work->page_count; i++) {
		struct unlock_transaction_page *page = &work->pages[i];

		page->cpu = diag_dma_alloc(work, "radix-page", GU_RADIX_PAGE,
					       &page->dma, GFP_KERNEL);
		if (!page->cpu) {
			ret = -ENOMEM;
			goto out;
		}
		work->pages_allocated++;
		addresses[i] = page->dma;
		memset(page->cpu, 0, GU_RADIX_PAGE);
		if (i >= r->table_pages) {
			u64 offset = (i - r->table_pages) * GU_RADIX_PAGE;
			size_t n = min_t(u64, GU_RADIX_PAGE, work->image_span.size - offset);

			memcpy(page->cpu, work->gsp_fw->data + work->image_span.offset + offset, n);
		}
	}
	for (level = 0; level < 3; level++) {
		u32 child = first + r->pages[level];

		for (i = 0; i < r->pages[level]; i++) {
			u32 at = i * (GU_RADIX_PAGE / 8);
			u32 n = min_t(u64, GU_RADIX_PAGE / 8, r->pages[level + 1] - at);

			ret = gpu_unlock_radix_page(work->pages[first + i].cpu,
						    addresses + child + at, n);
			if (ret)
				goto out;
		}
		first = child;
	}
out:
	kvfree(addresses);
	return GU_RESULT(ret);
}

static int unlock_transaction_claim(struct unlock_transaction_work *work)
{
	u16 command, pmcsr;
	u32 cap;
	unsigned int i;
	int ret;

	dev_info(&work->pdev->dev, "claim: power=%d pm_cap=%#x flags=%#x\n",
		work->pdev->current_state, work->pdev->pm_cap, work->pdev->dev_flags);
	if (work->pdev->current_state != PCI_D0 ||
	    (work->pdev->pm_cap &&
	     (diag_pci_word(work, false, work->pdev->pm_cap + PCI_PM_CTRL, &pmcsr) ||
	      (pmcsr & PCI_PM_CTRL_STATE_MASK) != PCI_D0)))
		return GU_RESULT(-EOPNOTSUPP);
	if ((work->pdev->dev_flags & PCI_DEV_FLAGS_NO_FLR_RESET) ||
	    !pci_is_pcie(work->pdev) ||
	    diag_pci_dword(work, true, PCI_EXP_DEVCAP, &cap) ||
	    !(cap & PCI_EXP_DEVCAP_FLR))
		return GU_RESULT(-EOPNOTSUPP);
	if (diag_pci_word(work, false, PCI_COMMAND, &command) || command == 0xffff)
		return GU_RESULT(-ENODEV);
	work->original_cmd = command;
	work->cmd_valid = true;
	if (command & PCI_COMMAND_MASTER)
		return GU_RESULT(-EBUSY);
	dev_info(&work->pdev->dev, "claim: command=%#x capability=%#x BAR_mask=%#x\n",
		command, cap, pci_select_bars(work->pdev, IORESOURCE_MEM));
	/* Only BAR0 is mapped. On the display card the firmware framebuffer keeps
	 * BAR1 until just before the FLR, as it does while the stock driver boots.
	 */
	ret = pci_request_region(work->pdev, 0, "gpu_unlock");
	if (ret)
		return GU_RESULT(ret);
	work->region = true;
	if (!(command & PCI_COMMAND_MEMORY)) {
		ret = pci_enable_device_mem(work->pdev);
		if (ret)
			return GU_RESULT(ret);
		work->enabled = true;
	}
	work->bar_len = pci_resource_len(work->pdev, 0);
	if (work->bar_len < SEC2_FALCON2 + 0x1000)
		return GU_RESULT(-EINVAL);
	for (i = 0; i < ARRAY_SIZE(work->bars); i++)
		if (diag_pci_dword(work, false, PCI_BASE_ADDRESS_0 + i * 4, &work->bars[i]))
			return GU_RESULT(-EIO);
	ret = pci_save_state(work->pdev);
	if (ret)
		return GU_RESULT(ret);
	work->bar = pci_iomap(work->pdev, 0, 0);
	if (!work->bar)
		return GU_RESULT(-ENOMEM);
	work->mapped = true;
	return 0;
}

static int unlock_transaction_dma(struct unlock_transaction_work *work)
{
	struct device *dev = &work->pdev->dev;

	if (!dev->dma_mask || work->masks_saved)
		return GU_RESULT(-EINVAL);
	work->masks_saved = true;
	work->streaming_ptr = dev->dma_mask;
	work->streaming_before = *dev->dma_mask;
	work->coherent_before = dev->coherent_dma_mask;
	dev_info(dev, "DMA masks: streaming=%#llx coherent=%#llx requested=%#llx\n",
		(unsigned long long)work->streaming_before, (unsigned long long)work->coherent_before,
		(unsigned long long)DMA_BIT_MASK(47));
	work->streaming_set = true;
	if (GU_DIAG(dma_set_mask(dev, DMA_BIT_MASK(47))))
		return GU_RESULT(-EIO);
	work->coherent_set = true;
	if (GU_DIAG(dma_set_coherent_mask(dev, DMA_BIT_MASK(47))))
		return GU_RESULT(-EIO);
	work->booter_cpu = diag_dma_alloc(work, "booter", work->booter_image_size,
					      &work->booter_dma, GFP_KERNEL);
	if (!work->booter_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->booter_cpu, work->booter_fw->data + 72, work->booter_image_size);
	work->v67_cpu = diag_dma_alloc(work, "v67", GU_V67_SIZE, &work->v67_dma, GFP_KERNEL);
	work->meta_cpu = diag_dma_alloc(work, "metadata", GU_WPR_META_SIZE, &work->meta_dma, GFP_KERNEL);
	if (!work->v67_cpu || !work->meta_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->v67_cpu, work->v67_fw->data, GU_V67_SIZE);
	work->stock_size = work->sig_span.size;
	work->stock_cpu = diag_dma_alloc(work, "stock-signature", work->stock_size, &work->stock_dma, GFP_KERNEL);
	work->boot_cpu = diag_dma_alloc(work, "boot-image", work->boot_desc.image_size, &work->boot_dma, GFP_KERNEL);
	if (!work->stock_cpu || !work->boot_cpu)
		return GU_RESULT(-ENOMEM);
	memcpy(work->stock_cpu, work->gsp_fw->data + work->sig_span.offset, work->stock_size);
	memcpy(work->boot_cpu, work->boot_fw->data, work->boot_desc.image_size);
	{
		int ret = patch_booter(work);

		return GU_RESULT(ret ? ret : unlock_transaction_radix(work));
	}
}

static int unlock_transaction_fwsec(struct unlock_transaction_work *work)
{
	static const struct falcon_eng gsp = {
		.base = 0x00110000, .fbif = 0x00110600, .falcon2 = 0x00111000,
	};
	size_t mapped = 0;
	u8 *rom = NULL;
	struct gpu_unlock_fwsec fw;
	u32 scratch, wpr_lo, wpr_hi;
	const struct firmware *expected_rom=NULL;
	u16 command;
	u64 frts = work->layout.frts_offset;
	void *image = NULL;
	dma_addr_t image_dma = 0;
	int ret;

	/* kgspExtractVbiosFromRom_TU102 reads NV_PROM_DATA, up to 1 MiB.
	 * pci_map_rom stops at the PCI last-image flag and hides the VN extension. */
	dev_info(&work->pdev->dev, "FWSEC: reading PROM at %#x\n", 0x300000);
	if (work->bar_len < 0x300000 + 0x1a)
		return GU_RESULT(-ENODEV);
	mapped = min_t(size_t, 1024 * 1024, work->bar_len - 0x300000);
	rom = kvmalloc(mapped, GFP_KERNEL);
	if (!rom)
		return GU_RESULT(-ENOMEM);
	memcpy_fromio(rom, work->bar + 0x300000, mapped);
	dev_info(&work->pdev->dev, "FWSEC: PROM bytes=%zu signature=%#x\n",
		 mapped, (unsigned int)(rom[0] | (rom[1] << 8)));
	ret=request_asset(work,&expected_rom,ROM_FILE);
 if (ret) goto out;
 if (expected_rom->size!=mapped || memcmp(rom,expected_rom->data,mapped)) ret=-EBADMSG;
 release_firmware(expected_rom);expected_rom=NULL;
 if (ret) goto out;
	ret = gpu_unlock_vbios_fwsec(rom, mapped, &fw);
	if (ret) {
		dev_err(&work->pdev->dev, "FWSEC-FRTS not found in board ROM (%d)\n", ret);
		goto out;
	}
	if (fw.engine_mask != 0x400) {
		dev_err(&work->pdev->dev, "FWSEC: unsupported GSP engine mask=%#x\n", fw.engine_mask);
		ret = -EOPNOTSUPP;
		goto out;
	}
	image = diag_dma_alloc(work, "fwsec", fw.image_size, &image_dma, GFP_KERNEL);
	if (!image) {
		ret = -ENOMEM;
		goto out;
	}
	work->fwsec_cpu = image;
	work->fwsec_dma = image_dma;
	work->fwsec_size = fw.image_size;
	memset(image,0,fw.image_size);
	memcpy(image,rom+fw.image_off,0x9df0);
	ret = gpu_unlock_fwsec_patch_frts(image, &fw, frts);
	if (ret)
		goto out;
	pci_set_master(work->pdev);
	work->master = true;
	if (diag_pci_word(work, false, PCI_COMMAND, &command) ||
	    command == 0xffff || !(command & PCI_COMMAND_MASTER)) {
		ret = -EIO;
		goto out;
	}
	ret = fwsec_with_loader(work,&gsp,&fw,image_dma);
	if (ret)
		goto out;
	if (read_bar(work, 0x00001400 + 0x0e * 4, &scratch)) {
		ret = -EIO;
		goto out;
	}
	dev_info(&work->pdev->dev, "FWSEC result: scratch=%#x expected_hi16=0\n", scratch);
	if (scratch >> 16) {
		ret = -EIO;
		goto out;
	}
	if (read_bar(work, 0x001fa824, &wpr_lo) || wpr_lo == ~0u ||
	    read_bar(work, 0x001fa828, &wpr_hi) || wpr_hi == ~0u) {
		ret = -EIO;
		goto out;
	}
	dev_info(&work->pdev->dev, "FWSEC WPR2: low=%#x high=%#x expected_page=%#llx end=%#llx\n",
		wpr_lo, wpr_hi, (unsigned long long)(frts >> 12),
		(unsigned long long)work->layout.wpr_end);
	/* Match NVIDIA's initialized-WPR2 check as well as the expected low
	 * address. Never publish Booter metadata for disabled/alien WPR2. */
	if (!(wpr_hi >> 4) || (wpr_hi >> 4) < (wpr_lo >> 4) ||
	    ((u64)(wpr_hi >> 4) << 12) >= work->layout.wpr_end ||
	    (wpr_lo >> 4) != (u32)(frts >> 12))
		ret = -EIO;
out:
	/* Ownership transfers to work before publication, including failure paths. */
	kvfree(rom);
	return GU_RESULT(ret);
}

static int unlock_transaction_gfw(struct unlock_transaction_work *work)
{
	ktime_t deadline = ktime_add_us(ktime_get(), GU_GFW_TIMEOUT_US);
	int assessed = -EAGAIN;

	for (;;) {
		struct gpu_unlock_gfw_sample sample = { };
		u16 vendor = 0xffff;

		if (read_bar(work, GU_GFW_PLM_OFFSET, &sample.plm) ||
		    read_bar(work, GU_GFW_BOOT_OFFSET, &sample.boot) ||
		    read_bar(work, GU_GFW_CPUCTL_OFFSET, &sample.cpuctl))
			return GU_RESULT(-EIO);
		if (!diag_pci_word(work, false, PCI_VENDOR_ID, &vendor) &&
		    vendor != 0xffff)
			sample.config_ok = true;
		assessed = gpu_unlock_gfw_assess(&sample);
		if (!assessed) {
			u32 ss0, ss1, wpr_hi;

			if (read_bar(work, GU_FEAT_PLM_OFFSET, &work->plm_entry) ||
			    read_bar(work, GU_FEAT_SS0_OFFSET, &ss0) || ss0 == ~0u ||
			    read_bar(work, GU_FEAT_SS1_OFFSET, &ss1) || ss1 == ~0u ||
			    read_bar(work, 0x001fa828, &wpr_hi) || inaccessible(wpr_hi))
				return GU_RESULT(-ENODEV);
			dev_info(&work->pdev->dev, "entry: plm=%#x ss0=%#x ss1=%#x wpr2_hi=%#x\n",
				 work->plm_entry, ss0, ss1, wpr_hi);
			/* An open PLM means the card was already unlocked or not cold. */
			if (work->plm_entry == ~0u)
				return GU_RESULT(-EALREADY);
			/* WPR2 is down from power-on until FWSEC-FRTS runs. An enabled
			 * WPR2 means a driver has already used the card this power
			 * cycle; starting FWSEC and Booter again from here is untested.
			 */
			if (wpr_hi >> 4) {
				dev_err(&work->pdev->dev, "card already initialised since power on (WPR2 enabled); power off first\n");
				return GU_RESULT(-EBUSY);
			}
			return 0;
		}
		if (assessed != -EAGAIN)
			return GU_RESULT(assessed);
		if (ktime_compare(ktime_get(), deadline) >= 0)
			return GU_RESULT(-ETIMEDOUT);
		usleep_range(1000, 2000);
	}
}

static int unlock_transaction_boot(struct unlock_transaction_work *work)
{
 const struct falcon_eng sec2={.base=SEC2_BASE,.fbif=SEC2_FBIF,.falcon2=SEC2_FALCON2};
 const u8 *image=work->booter_cpu;
 int ret;
 u32 trans,mailbox;
 if (!work->published || !work->master || !work->firmware_started) return GU_RESULT(-EINVAL);
 ret=falcon_reset(work,&sec2);if(ret)return ret;
 if(read_bar(work,SEC2_FBIF+0x24,&trans)||inaccessible(trans))return GU_RESULT(-ENODEV);
 write_bar(work,SEC2_FBIF+0x24,trans|(1u<<7));write_bar(work,SEC2_BASE+0x10c,0);
 port_imem(work,SEC2_BASE,0,image,0x100,false,0);
 port_imem(work,SEC2_BASE,0x100,image+0x100,0x8400,true,0x100);
 port_dmem(work,SEC2_BASE,image+0x8500,0x6200);
 write_bar(work,SEC2_BASE+0x104,0);
 write_bar(work,SEC2_BASE+0x40,lower_32_bits(work->meta_dma));
 write_bar(work,SEC2_BASE+0x44,upper_32_bits(work->meta_dma));
 dev_info(&work->pdev->dev,"TU104 BOOT_DIRECT: metadata_dma=%pad signature_dma=%pad\n",&work->meta_dma,&work->v67_dma);
 ret=start_wait(work,SEC2_BASE);
 if(!read_bar(work,SEC2_BASE+0x40,&mailbox))dev_info(&work->pdev->dev,"TU104 Booter halt: mailbox0=%#x result=%d\n",mailbox,ret);
 return GU_RESULT(ret);
}

static void put_le64(u8 *dest, u64 value)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		dest[i] = (u8)(value >> (8 * i));
}

static int unlock_transaction_signature(struct unlock_transaction_work *work)
{
	u8 *meta = work->meta_cpu;

	if (!meta || !work->stock_cpu || !work->pages_allocated ||
	    work->signature_calls >= 2)
		return GU_RESULT(-EINVAL);
	if (work->signature_calls == 0) {
		struct gpu_unlock_wpr_args args = {
			.fb_size = work->layout_in.usable_fb,
			.vga_offset = work->layout_in.vga_base,
			.frts_size = work->layout_in.frts_size,
			.signature_dma = work->v67_dma, .signature_size = GU_V67_SIZE,
			.radix_dma = work->pages[0].dma,
			.image_size = work->image_span.size,
			.boot_dma = work->boot_dma, .boot = work->boot_desc,
		};
		int ret = gpu_unlock_wpr_meta_pack(meta, &work->layout, &args);

		if (ret)
			return GU_RESULT(ret);
		dma_wmb();
		work->published = true;
	} else {
		put_le64(meta + 72, work->stock_dma);
		put_le64(meta + 80, work->stock_size);
		dma_wmb();
	}
	work->signature_calls++;
	return 0;
}

static int unlock_transaction_selector(struct unlock_transaction_work *work)
{
 u32 ss0,ss1,plm;
 if(read_bar(work,GU_FEAT_SS0_OFFSET,&ss0)||read_bar(work,GU_FEAT_SS1_OFFSET,&ss1)||read_bar(work,GU_FEAT_PLM_OFFSET,&plm))return GU_RESULT(-ENODEV);
 dev_info(&work->pdev->dev,"TU104 payload result: PLM=%#x SS0=%#x SS1=%#x\n",plm,ss0,ss1);
 /* The pinned Turing chain writes both selectors and restores FECS PLM.
  * Do not try host writes through a protected PLM if the payload failed. */
 return GU_RESULT(ss0==GU_SS0_UNLOCK && ss1==GU_SS1_UNLOCK && plm==0xffffff8f ? 0 : -EIO);
}

/* Reset SEC2 after FLR and leave it halted with reset released. Do not reset
 * GSP here: stock RM first requires completed GFW and a halted GSP Falcon.
 * ksec2ResetHw_TU102 ends with RESET_FALSE. Leaving RESET_TRUE makes later
 * host reads return FECS_PRI_RESET (0xbadf12 in bits 31:8). RM's scrub wait
 * treats that value as scrubbing-pending and does not finish.
 */
static int hold_sec2_reset(struct unlock_transaction_work *work, const char *phase)
{
	unsigned int j;
	u32 engine, before, first = 0, hwcfg;
	int ret;

	if (read_bar(work, SEC2_BASE + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-ENODEV);
	before = engine;
	dev_info(&work->pdev->dev, "engine reset assert: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, SEC2_BASE, before, before | 1);
	write_bar(work, SEC2_BASE + 0x3c0, engine | 1);
	/* 610 ksec2ResetHw_TU102 requires ten register cycles for signal
	 * propagation. A stale zero during those cycles is not evidence that
	 * the assertion failed. No CPU start and no reset retry.
	 */
	for (j = 0; j < 10; j++) {
		if (read_bar(work, SEC2_BASE + 0x3c0, &engine) ||
		    inaccessible(engine)) {
			dev_err(&work->pdev->dev, "engine reset inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, SEC2_BASE, j + 1, engine);
			return GU_RESULT(-EIO);
		}
		if (!j)
			first = engine;
	}
	dev_info(&work->pdev->dev, "engine reset propagation: phase=%s base=%#x first=%#x tenth=%#x\n",
		 phase, SEC2_BASE, first, engine);
	ret = poll_us(work, SEC2_BASE + 0x3c0, 1, 1, 1000, true);
	dev_info(&work->pdev->dev, "engine reset asserted: phase=%s base=%#x error=%d last=%#x\n",
		 phase, SEC2_BASE, ret, work->last_value);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, SEC2_BASE + 0x3c0, &engine) || inaccessible(engine))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "engine reset release: phase=%s base=%#x before=%#x write=%#x\n",
		 phase, SEC2_BASE, engine, engine & ~1u);
	write_bar(work, SEC2_BASE + 0x3c0, engine & ~1u);
	for (j = 0; j < 10; j++) {
		if (read_bar(work, SEC2_BASE + 0x3c0, &engine) ||
		    inaccessible(engine)) {
			dev_err(&work->pdev->dev, "engine reset release inaccessible: phase=%s base=%#x cycle=%u value=%#x\n",
				phase, SEC2_BASE, j + 1, engine);
			return GU_RESULT(-EIO);
		}
	}
	ret = poll_us(work, SEC2_BASE + 0x3c0, 1, 0, 1000, true);
	if (ret)
		return GU_RESULT(ret);
	/* NV_PFALCON_FALCON_HWCFG2 bit 12 clears when memory scrubbing finishes.
	 * A PRI error fails this wait instead of being handed to RM.
	 */
	ret = poll_us(work, SEC2_BASE + 0xf4, 1u << 12, 0, 1000000, true);
	if (ret)
		return GU_RESULT(ret);
	if (read_bar(work, SEC2_BASE + 0xf4, &hwcfg) || inaccessible(hwcfg))
		return GU_RESULT(-EIO);
	dev_info(&work->pdev->dev, "engine reset released: phase=%s base=%#x engine=%#x hwcfg2=%#x\n",
		 phase, SEC2_BASE, engine, hwcfg);
	return 0;
}

static int unlock_transaction_reset(struct unlock_transaction_work *work)
{
 u32 gsp,sec2,lo,hi,ss0,ss1,plm;
 u16 cmd,pending;
 int ret;
 if(work->signature_calls!=2)return GU_RESULT(-EPROTO);
 /* The native Turing payload clears WPR2 and halts through signed Booter
  * cleanup. Match the known pre-OS Turing handoff: sanitize SEC2, not PF-FLR.
  * Never clear WPR registers from the host or free DMA on a timeout. */
 pci_clear_master(work->pdev);work->master=false;
 if(diag_pci_word(work,false,PCI_COMMAND,&cmd)||cmd==0xffff||(cmd&PCI_COMMAND_MASTER))return GU_RESULT(-EIO);
 if(read_bar(work,0x110100,&gsp)||inaccessible(gsp)||!(gsp&0x10)||read_bar(work,SEC2_BASE+0x100,&sec2)||inaccessible(sec2)||!(sec2&0x10))return GU_RESULT(-EBUSY);
 ret=hold_sec2_reset(work,"TU104 native-halt handoff");if(ret)return ret;
 if(read_bar(work,SEC2_BASE+0x100,&sec2)||inaccessible(sec2)||!(sec2&0x10))return GU_RESULT(-EBUSY);
 write_bar(work,SEC2_BASE+0x40,0);write_bar(work,SEC2_BASE+0x44,0);
 if(diag_pci_word(work,true,PCI_EXP_DEVSTA,&pending)||pending==0xffff||(pending&PCI_EXP_DEVSTA_TRPND))return GU_RESULT(-EBUSY);
 if(read_bar(work,0x001fa824,&lo)||inaccessible(lo)||read_bar(work,0x001fa828,&hi)||inaccessible(hi)||
 read_bar(work,GU_FEAT_SS0_OFFSET,&ss0)||read_bar(work,GU_FEAT_SS1_OFFSET,&ss1)||read_bar(work,GU_FEAT_PLM_OFFSET,&plm))return GU_RESULT(-ENODEV);
 dev_info(&work->pdev->dev,"TU104 handoff: GSP=%#x SEC2=%#x BME=0 pending=0 WPR=%#x:%#x SS0=%#x SS1=%#x PLM=%#x\n",gsp,sec2,lo,hi,ss0,ss1,plm);
 if(hi>>4 || lo!=0x1ffffe00 || ss0!=GU_SS0_UNLOCK || ss1!=GU_SS1_UNLOCK || plm!=0xffffff8f)return GU_RESULT(-EBUSY);
 work->quiesced=true;
 return 0;
}

int gpu_unlock_610_load_shared(struct pci_dev *pdev, struct gpu_unlock_610_shared *shared)
{
	return request_scoped(&pdev->dev, &shared->gsp, GSP_FILE);
}

void gpu_unlock_610_release_shared(struct gpu_unlock_610_shared *shared)
{
	release_firmware(shared->gsp);
	shared->gsp = NULL;
}

int gpu_unlock_610_unlock(struct pci_dev *pdev, const struct gpu_unlock_610_shared *shared)
{
	struct unlock_transaction_work *work;
	int ret, cleanup;

	work = kzalloc(sizeof(*work), GFP_KERNEL);
	if (!work)
		return -ENOMEM;
	work->pdev = pci_dev_get(pdev);
	work->gsp_fw = shared->gsp;
	ret = unlock_transaction_assets(work);
	if (!ret)
		ret = unlock_transaction_claim(work);
	if (!ret)
		ret = unlock_transaction_layout(work);
	if (!ret)
		ret = unlock_transaction_dma(work);
	if (!ret)
		ret = unlock_transaction_gfw(work);
	if (!ret)
		ret = unlock_transaction_fwsec(work);
	if (!ret)
		ret = unlock_transaction_signature(work);	/* publish V67 */
	if (!ret)
		ret = unlock_transaction_boot(work);
	if (!ret)
		ret = unlock_transaction_signature(work);	/* restore stock */
	if (!ret)
		ret = unlock_transaction_selector(work);
	if (!ret)
		ret = unlock_transaction_reset(work);
	/* Never leave the V67 signature published for a later stock boot. */
	if (ret && work->signature_calls == 1)
		unlock_transaction_signature(work);
	cleanup = unlock_transaction_unwind(work);
	if (!ret)
		ret = cleanup;
	if (work->retained) {
		/* Firmware may still DMA into these buffers: keep them, the device
		 * reference and this module until the next power off. */
		dev_err(&pdev->dev, "firmware did not stop; keeping its DMA buffers until power off\n");
		__module_get(THIS_MODULE);
		return ret;
	}
	pci_dev_put(work->pdev);
	kfree(work);
	return ret;
}

MODULE_FIRMWARE(BOOTER_FILE);
MODULE_FIRMWARE(V67_FILE);
MODULE_FIRMWARE(GSP_FILE);
MODULE_FIRMWARE(BOOT_FILE);
MODULE_FIRMWARE(BOOT_DESC_FILE);

MODULE_FIRMWARE(BL_FILE);
MODULE_FIRMWARE(ROM_FILE);
