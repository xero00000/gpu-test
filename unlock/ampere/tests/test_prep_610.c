// SPDX-License-Identifier: GPL-2.0-only
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "layout_610.h"
#include "prep_610.h"

static void put16(unsigned char *p, unsigned int v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
}

static void put32(unsigned char *p, unsigned int v)
{
	p[0] = v & 0xff;
	p[1] = (v >> 8) & 0xff;
	p[2] = (v >> 16) & 0xff;
	p[3] = (v >> 24) & 0xff;
}

static void put64(unsigned char *p, unsigned long long v)
{
	unsigned int i;

	for (i = 0; i < 8; i++)
		p[i] = (unsigned char)(v >> (8 * i));
}

int main(void)
{
	unsigned char elf[256];
	unsigned char image[64];
	unsigned char sigs[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
	unsigned char meta[GU_WPR_META_SIZE];
	struct gpu_unlock_span span;
	struct gpu_unlock_radix_plan radix;
	struct gpu_unlock_booter_hdr hdr = {
		.num_apps = 1,
		.patch_loc = 16,
		.num_sigs = 2,
		.sig_bytes = 8,
	};
	struct gpu_unlock_layout_in in = {
		.usable_fb = 24ull << 30,
		.vga_base = (24ull << 30) - (1024ull * 1024),
		.frts_size = 1024ull * 1024,
		.boot_size = 4096,
		.image_size = 64ull * 1024,
		.nonwpr_size = 1,
		.heap_request = 1024ull * 1024,
	};
	struct gpu_unlock_layout_out layout;
	struct gpu_unlock_wpr_args args = {
		.fb_size = in.usable_fb, .vga_offset = in.vga_base,
		.frts_size = in.frts_size, .signature_dma = 0x1000,
		.signature_size = 0xfa00, .radix_dma = 0x2000,
		.image_size = in.image_size, .boot_dma = 0x3000,
		.boot = { .code_offset = 256, .data_offset = 512, .image_size = 4096 },
	};

	assert(gpu_unlock_fuse_version(0) == 0);
	assert(gpu_unlock_fuse_version(1) == 1);
	assert(gpu_unlock_fuse_version(0x8) == 4);

	memset(image, 0, sizeof(image));
	assert(!gpu_unlock_booter_patch(image, sizeof(image), &hdr, sigs,
					sizeof(sigs), 0));
	assert(!memcmp(image + 16, sigs + 4, 4));
	assert(gpu_unlock_booter_patch(image, sizeof(image), &hdr, sigs,
				       sizeof(sigs), 2) == -ERANGE);

	memset(elf, 0, sizeof(elf));
	elf[0] = 0x7f;
	elf[1] = 'E';
	elf[2] = 'L';
	elf[3] = 'F';
	elf[4] = 2;
	elf[5] = 1;
	elf[6] = 1;
	put32(elf + 20, 1);
	put16(elf + 52, 64);
	put64(elf + 40, 64);
	put16(elf + 58, 64);
	put16(elf + 60, 2);
	put16(elf + 62, 1);
	put32(elf + 64, 0);
	put64(elf + 64 + 24, 220);
	put64(elf + 64 + 32, 4);
	put32(elf + 128, 9);
	put64(elf + 128 + 24, 192);
	put64(elf + 128 + 32, 16);
	memcpy(elf + 192, ".fwimage", 8);
	elf[200] = 0;
	memcpy(elf + 220, "name", 4);
	assert(!gpu_unlock_elf_section(elf, sizeof(elf), ".fwimage", &span));
	assert(span.offset == 220 && span.size == 4);
	assert(gpu_unlock_elf_section(elf, sizeof(elf), ".missing", &span) == -ENOENT);

	assert(!gpu_unlock_radix_plan(4096, &radix));
	assert(radix.pages[0] == 1 && radix.pages[3] == 1);
	assert(!gpu_unlock_radix_plan(4097, &radix));
	assert(radix.pages[3] == 2);
	assert(gpu_unlock_radix_plan(~(u64)0, &radix) == -EOVERFLOW);
	/* A name that ends at the string-table boundary must include its NUL. */
	put64(elf + 128 + 32, 8);
	assert(gpu_unlock_elf_section(elf, sizeof(elf), ".fwimage", &span) == -EINVAL);
	put64(elf + 128 + 32, 16);
	put32(elf + 128, 0);
	assert(gpu_unlock_elf_section(elf, sizeof(elf), ".fwimage", &span) == -EINVAL);
	assert(gpu_unlock_dma_cmd(true) == ((6u << 8) | (1u << 4) | (1u << 2)));
	assert(gpu_unlock_dma_cmd(false) == (6u << 8));

	assert(!gpu_unlock_layout(&in, &layout));
	assert(!gpu_unlock_wpr_meta_pack(meta, &layout, &args));
	assert(meta[0] == 0xb3 && meta[7] == 0xdc);
	assert(meta[72] == 0x00 && meta[73] == 0x10);
	assert(meta[49] == 1 && meta[57] == 2);
	assert(!memcmp(meta + 184, (unsigned char[]){0,0,0xf0,0xff,5,0,0,0}, 8));
	args.image_size = ~0ull;
	assert(gpu_unlock_wpr_meta_pack(meta, &layout, &args) == -EINVAL);
	{
		unsigned char desc[84] = {0}, page[GU_RADIX_PAGE];
		u64 addresses[] = {0x123456000ull, 0xdeadbe000ull};
		struct gpu_unlock_boot_desc boot;
		const u32 words[] = {5,20480,2176,22656,16,0,0,0,0,2048,2048,4096,6144,10496,1,0,0,0,0,24576,0};

		for (unsigned int i=0; i<21; i++) put32(desc+i*4,words[i]);
		assert(!gpu_unlock_boot_desc_parse(desc, sizeof(desc), 24576, &boot));
		assert(boot.code_offset == 6144 && boot.data_offset == 2048 && !boot.manifest_offset);
		assert(gpu_unlock_boot_desc_parse(desc, 83, 24576, &boot) == -EINVAL);
		put32(desc+48, ~0u);
		assert(gpu_unlock_boot_desc_parse(desc, 84, 24576, &boot) == -ERANGE);
		assert(!gpu_unlock_radix_page(page, addresses, 2));
		assert(page[3] == 0x23 && page[4] == 1 && page[12] == 0xd);
		for (unsigned int i=16; i<sizeof(page); i++) assert(page[i] == 0);
		addresses[0]++;
		assert(gpu_unlock_radix_page(page, addresses, 2) == -EINVAL);
		assert(gpu_unlock_radix_page(page, addresses, 513) == -EINVAL);
	}

	puts("prep checks passed");
	return 0;
}
