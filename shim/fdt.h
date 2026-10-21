#pragma once
#include <stdint.h>

struct fdt_range {
	uint64_t base, size;
};

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* DRAM ranges from /memory "reg"; returns the number stored. */
int fdt_memory(uintptr_t fdt, struct fdt_range *out, int max);
