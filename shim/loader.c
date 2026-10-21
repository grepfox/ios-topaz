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
 * The ADT's RAM size comes from mkadt.py; the real DRAM layout is what ABL
 * put in /memory. Find the (merged, contiguous) range that holds phys_base
 * and shrink mem_size to fit it. 0: phys_base is not in any range.
 */
static uint64_t mem_fit(uintptr_t dtb, uint64_t base, uint64_t size)
{
	struct fdt_range r[16];
	int n = fdt_memory(dtb, r, 16);
	for (int i = 0; i < n && i < 8; i++) {
		char label[] = "MEM0";
		label[3] = '0' + i;
		con_hex2(label, "base=", (uint32_t)(r[i].base >> 4), "size=", (uint32_t)(r[i].size >> 4));
	}
	if (n == 0) {
		con_str("MEM", "no /memory in DTB, ADT size kept");
		return size;
	}
	uint64_t end = 0;
	for (int i = 0; i < n; i++)
		if (base >= r[i].base && base < r[i].base + r[i].size)
			end = r[i].base + r[i].size;
	if (!end)
		return 0;
	for (int grew = 1; grew;) {	/* banks reported as separate but adjacent ranges */
		grew = 0;
		for (int i = 0; i < n; i++)
			if (r[i].base == end) {
				end += r[i].size;
				grew = 1;
			}
	}
	if (end - base < size) {
		size = (end - base) & ~0x1ffffffUL;	/* 32 MiB */
		con_hex2("MEM", "clamped to=", (uint32_t)(size >> 4), "(x16)", 0);
	}
	return size;
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

	uint64_t pa_of_va_base = p->phys_base - p->virt_base;	/* pa = va + this */
	uint64_t mem_size = p->mem_size;
#if defined(MEM_FROM_FDT)
	mem_size = mem_fit(dtb, p->phys_base, mem_size);
	if (!mem_size)
		return "kernel phys_base not in DTB /memory";
#endif
	/* values >> 4 so that 64-bit addresses fit the 32-bit fields */
	con_hex2("PHYS", "base>>4=", (uint32_t)(p->phys_base >> 4), "size>>4=", (uint32_t)(mem_size >> 4));
	con_hex("VIRT", p->virt_base);
	con_hex("ENTRY", p->entry_va);

	/* The payload must not overlap where the kernel goes. */
	if (rd_end > p->phys_base && rd_start < p->phys_base + mem_size)
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
	if (mem_size != p->mem_size)
		adt_set_u64(adt_pa, p->adt_size, "dram-size", mem_size);
#endif
	uintptr_t ba_pa = (adt_pa + p->adt_size + PAGE_16K - 1) & ~(PAGE_16K - 1);
	uintptr_t top = ba_pa + PAGE_16K;

	zero64(ba_pa, PAGE_16K);
	volatile struct boot_args *ba = (volatile struct boot_args *)ba_pa;
	ba->Revision = 2;
	ba->Version = 2;
	ba->virtBase = p->virt_base;
	ba->physBase = p->phys_base;
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
