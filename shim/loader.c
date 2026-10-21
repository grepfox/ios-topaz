/*
 * Milestone 4: act as iBoot for xnu.
 *
 * mkpayload.py already resolved the kernel's chained fixups and built the
 * ADT, so this only has to: find the payload in the ABL ramdisk, copy the
 * segments to phys_base + (vmaddr - virt_base), place the ADT and
 * boot_args behind the kernel, and jump to the entry point with the MMU
 * and caches off, x0 = boot_args (physical).
 */
#include <stdint.h>
#include "console.h"
#include "fdt.h"
#include "loader.h"
#include "target.h"

#define BOOT_LINE_LENGTH	608
#define PAGE_16K		0x4000UL

struct payload_seg {
	uint64_t vmaddr, vmsize, file_off, file_size;
};

struct payload {
	char magic[8];
	uint32_t version, nsegs;
	uint64_t virt_base, entry_va, adt_off, adt_size, kernel_end;
	uint64_t phys_base, mem_size;
	char cmdline[BOOT_LINE_LENGTH];
	struct payload_seg segs[];
};

/* pexpert/pexpert/arm64/boot.h, Revision/Version 2 */
struct boot_video {
	uint64_t v_baseAddr, v_display, v_rowBytes, v_width, v_height, v_depth;
};

struct boot_args {
	uint16_t Revision, Version;
	uint64_t virtBase, physBase, memSize, topOfKernelData;
	struct boot_video Video;
	uint32_t machineType;
	uint64_t deviceTreeP;
	uint32_t deviceTreeLength;
	char CommandLine[BOOT_LINE_LENGTH];
	uint64_t bootFlags, memSizeActual;
};

_Static_assert(__builtin_offsetof(struct payload, segs) == 0x2a8, "payload header layout");
_Static_assert(__builtin_offsetof(struct boot_args, Video) == 40, "boot_args layout");
_Static_assert(__builtin_offsetof(struct boot_args, CommandLine) == 108, "boot_args layout");
_Static_assert(sizeof(struct boot_args) == 736, "boot_args layout");

/* MMU off: everything is Device memory, so copy in aligned 8-byte words. */
static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	volatile const uint64_t *s = (volatile const uint64_t *)src;
	for (uint64_t i = 0; i < (n + 7) / 8; i++)
		d[i] = s[i];
}

static void zero64(uintptr_t dst, uint64_t n)
{
	volatile uint64_t *d = (volatile uint64_t *)dst;
	for (uint64_t i = 0; i < n / 8; i++)
		d[i] = 0;
}

static int magic_ok(const char *m)
{
	static const char want[] = "SWEETXNU";
	for (int i = 0; i < 8; i++)
		if (m[i] != want[i])
			return 0;
	return 1;
}

typedef void (*xnu_entry_t)(uint64_t boot_args_pa);

#if defined(MEM_FROM_FDT)
/*
 * The ADT's RAM layout comes from mkadt.py; the real DRAM layout is what ABL
 * put in /memory (several ranges, with holes where TZ/hyp memory is not
 * listed). Merge adjacent ranges and give xnu the largest contiguous block
 * at or above MEM_MIN_BASE (above every fixed carve-out), 32 MiB aligned.
 * A block of at least 1 GiB that starts below 4 GiB wins (UFS and the
 * display want DMA memory there), else the largest one; xnu takes physBase
 * from boot_args, so the kernel can go anywhere.
 * Kernel 0145 log: taking only the block that held 0xb0000000 left xnu
 * with 12288 pages (192 MiB) and dyld's first vm_allocate never finished.
 */
#define MEM_MIN_BASE	0xb0000000UL
#define MEM_ALIGN	0x2000000UL
static struct fdt_range mem_r[16];
static int mem_n;

static int mem_pick(uintptr_t dtb, uint64_t *base, uint64_t *size)
{
	mem_n = fdt_memory(dtb, mem_r, 16);
	for (int i = 0; i < mem_n && i < 8; i++) {
		char label[] = "MEM0";
		label[3] = '0' + i;
		con_hex2(label, "base>>4=", (uint32_t)(mem_r[i].base >> 4), "size>>4=", (uint32_t)(mem_r[i].size >> 4));
	}
	if (mem_n == 0) {
		con_str("MEM", "no /memory in DTB, ADT layout kept");
		return 1;
	}
	uint64_t best_lo = 0, best_size = 0;
	int best_low = 0;
	for (int i = 0; i < mem_n; i++) {
		/* the block that starts at range i: grow through adjacent ranges */
		uint64_t lo = mem_r[i].base, end = lo + mem_r[i].size;
		int prev = 0;
		for (int j = 0; j < mem_n; j++)
			if (mem_r[j].base + mem_r[j].size == lo)
				prev = 1;
		if (prev)
			continue;	/* not the start of a block */
		for (int grew = 1; grew;) {
			grew = 0;
			for (int j = 0; j < mem_n; j++)
				if (mem_r[j].base == end) {
					end += mem_r[j].size;
					grew = 1;
				}
		}
		if (lo < MEM_MIN_BASE)
			lo = MEM_MIN_BASE;
		lo = (lo + MEM_ALIGN - 1) & ~(MEM_ALIGN - 1);
		end &= ~(MEM_ALIGN - 1);
		int low = lo < 0x100000000UL && end > lo && end - lo >= 0x40000000UL;
		if (end > lo && (low > best_low || (low == best_low && end - lo > best_size))) {
			best_lo = lo;
			best_size = end - lo;
			best_low = low;
		}
	}
	if (!best_size)
		return 0;
	*base = best_lo;
	*size = best_size;
	con_hex2("MEM", "pick base>>4=", (uint32_t)(best_lo >> 4), "size>>4=", (uint32_t)(best_size >> 4));
	return 1;
}

/* Append " sweetmem=BASE+SIZE,..." (hex) so the kernel log shows ABL's /memory */
static void mem_cmdline(volatile char *cl, int max)
{
	static const char hex[] = "0123456789abcdef";
	int n = 0;
	while (n < max - 1 && cl[n])
		n++;
	const char *tag = " sweetmem=";
	for (int i = 0; tag[i] && n < max - 1; i++)
		cl[n++] = tag[i];
	for (int i = 0; i < mem_n && n < max - 40; i++) {
		uint64_t v[2] = { mem_r[i].base, mem_r[i].size };
		for (int k = 0; k < 2; k++) {
			int started = 0;
			for (int sh = 60; sh >= 0; sh -= 4) {
				int d = (v[k] >> sh) & 0xf;
				if (d || started || sh == 0) {
					cl[n++] = hex[d];
					started = 1;
				}
			}
			cl[n++] = k ? ',' : '+';
		}
	}
	cl[n] = 0;
}

/* ADT property: char name[32], u32 length, value. Device memory: 32-bit accesses only. */
static void adt_set_u64(uintptr_t adt, uint64_t len, const char *name, uint64_t v)
{
	for (uintptr_t a = adt; a + 44 <= adt + len; a += 4) {
		volatile const char *s = (volatile const char *)a;
		int i = 0;
		while (name[i] && s[i] == name[i])
			i++;
		if (name[i] || s[i])
			continue;
		if (*(volatile uint32_t *)(a + 32) != 8)
			continue;
		*(volatile uint32_t *)(a + 36) = (uint32_t)v;
		*(volatile uint32_t *)(a + 40) = (uint32_t)(v >> 32);
		return;
	}
}
#endif

/* build.sh appends the payload to the Image at the first 4K boundary after _end */
extern char _end[];

const char *loader_boot(uintptr_t dtb)
{
	uint64_t rd_start, rd_end;
	uintptr_t emb = ((uintptr_t)_end + 0xfff) & ~0xfffUL;

	/*
	 * topaz: the ABL only passes the vendor_boot ramdisk for a v4 boot
	 * image, so the payload rides behind the shim in the kernel Image.
	 * Its exact size is unknown here; the kernel memory check below only
	 * needs an upper bound, and the Image is far below phys_base.
	 */
	if (magic_ok((const char *)emb)) {
		rd_start = emb;
		rd_end = emb + 0x8000000;
		con_hex("EMBED", emb);
	} else {
		if (!fdt_chosen_u64(dtb, "linux,initrd-start", &rd_start) ||
		    !fdt_chosen_u64(dtb, "linux,initrd-end", &rd_end))
			return "no initrd in /chosen";
		con_hex2("INITRD", "lo=", rd_start, "hi=", rd_end);
	}

	/*
	 * Boot image v4 (topaz ABL ignores v1/v2): ABL concatenates the
	 * vendor_boot ramdisk(s) and ours, so the payload is somewhere inside
	 * the initrd and not necessarily 8-byte aligned. Find it and move it
	 * down to an aligned address; the vendor ramdisk below it is unused.
	 */
	uintptr_t hit = 0;
	for (uintptr_t a = rd_start; a + 8 <= rd_end; a++)
		if (magic_ok((const char *)a)) {
			hit = a;
			break;
		}
	if (!hit)
		return "no SWEETXNU payload";
	if (hit != rd_start) {
		uintptr_t dst = (rd_start + 7) & ~7UL;
		volatile uint8_t *d = (volatile uint8_t *)dst;
		volatile const uint8_t *src = (volatile const uint8_t *)hit;

		con_hex2("PAYLOAD", "at=", hit, "to=", dst);
		for (uint64_t i = 0; i < rd_end - hit; i++)
			d[i] = src[i];
		rd_end = dst + (rd_end - hit);
		rd_start = dst;
	}
	const struct payload *p = (const struct payload *)rd_start;
	if (p->version != 1 || (p->adt_off & 7) || (rd_start & 7))
		return "bad payload header";

	uint64_t phys_base = p->phys_base, mem_size = p->mem_size;
#if defined(MEM_FROM_FDT)
	if (!mem_pick(dtb, &phys_base, &mem_size))
		return "no usable RAM block in DTB /memory";
#endif
	uint64_t pa_of_va_base = phys_base - p->virt_base;	/* pa = va + this */
	/* values >> 4 so that 64-bit addresses fit the 32-bit fields */
	con_hex2("PHYS", "base>>4=", (uint32_t)(phys_base >> 4), "size>>4=", (uint32_t)(mem_size >> 4));
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > phys_base && rd_start < phys_base + mem_size)
		return "ramdisk overlaps kernel memory";

	for (uint32_t i = 0; i < p->nsegs; i++) {
		const struct payload_seg *s = &p->segs[i];
		uintptr_t dst = s->vmaddr + pa_of_va_base;
		copy64(dst, rd_start + s->file_off, s->file_size);
		if (s->vmsize > s->file_size)
			zero64(dst + ((s->file_size + 7) & ~7UL), s->vmsize - ((s->file_size + 7) & ~7UL));
	}
	con_hex2("SEGS", "n=", p->nsegs, "end=", p->kernel_end + pa_of_va_base);

	/* ADT, then boot_args, behind the kernel; everything below topOfKernelData is ours. */
	uintptr_t adt_pa = p->kernel_end + pa_of_va_base;
	copy64(adt_pa, rd_start + p->adt_off, p->adt_size);
#if defined(MEM_FROM_FDT)
	adt_set_u64(adt_pa, p->adt_size, "dram-base", phys_base);
	adt_set_u64(adt_pa, p->adt_size, "dram-size", mem_size);
#endif
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = phys_base;
	ba->memSize = mem_size;
	ba->topOfKernelData = top;
	ba->Video.v_baseAddr = (uintptr_t)con_fb;	/* FB_BASE, or where vayu's ABL scans out */
	ba->Video.v_display = 1;
	ba->Video.v_rowBytes = FB_STRIDE;
	ba->Video.v_width = FB_W;
	ba->Video.v_height = FB_H;
	ba->Video.v_depth = 32;
	ba->deviceTreeP = adt_pa - pa_of_va_base;	/* xnu reads it with the MMU on: a VA */
	ba->deviceTreeLength = (uint32_t)p->adt_size;
	for (int i = 0; i < BOOT_LINE_LENGTH; i++)
		ba->CommandLine[i] = p->cmdline[i];
#if defined(MEM_FROM_FDT)
	mem_cmdline(ba->CommandLine, BOOT_LINE_LENGTH);
#endif
	ba->memSizeActual = mem_size;
	con_hex2("BOOTARG", "pa=", ba_pa, "top=", top);

	uintptr_t entry_pa = p->entry_va + pa_of_va_base;
	con_hex("JUMP", entry_pa);
	con_str("XNU", "jumping (fbcon clears screen)");
#if defined(MEM_FROM_FDT)
	/* vayu bring-up: 5 s to photograph MEM0.. before the kernel console draws over them */
	{
		uint64_t f, t0, t;
		__asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
		__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t0));
		do
			__asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(t));
		while (t - t0 < 5 * f);
	}
#endif

	/* Everything was written with the MMU and caches off; drop any stale I-cache lines too. */
	__asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");
	((xnu_entry_t)entry_pa)(ba_pa);
	return "xnu returned";
}
