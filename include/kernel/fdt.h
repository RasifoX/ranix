#ifndef _KERNEL_FDT_H
#define _KERNEL_FDT_H

#include <uapi/types.h>

#define FDT_MAGIC       0xD00DFEED

#define FDT_BEGIN_NODE  0x00000001
#define FDT_END_NODE    0x00000002
#define FDT_PROP        0x00000003
#define FDT_NOP         0x00000004
#define FDT_END         0x00000009

typedef struct {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version;
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
} fdt_header_t;

/* parsed system info from DTB */
typedef struct {
    uint64_t mem_base;
    uint64_t mem_size;       /* first region only (legacy fallback) */
    uint64_t mem_total;      /* sum of all /memory regions */
    uint32_t num_cpus;
    /* Per-CPU secondary-boot info read straight from the DTB (no hardcoding):
     * cpu_release[i] = spin-table release address (0 if cpu uses PSCI). */
    uint64_t cpu_release[8];
    uint32_t psci_present;   /* 1 if a /psci node exists */
    uint32_t psci_use_smc;   /* 1 if /psci method = "smc", 0 = "hvc" */
    char     model[64];
    char     compatible[64];
} fdt_info_t;

int         fdt_init(void *dtb_addr);
fdt_info_t *fdt_get_info(void);
void        fdt_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud);
void       *fdt_get_base(void);
uint32_t    fdt_get_size(void);
uint64_t    fdt_get_phys(void);
void        fdt_remap(void);

/* low-level property lookup: returns pointer to value, sets *out_len */
const void *fdt_get_prop(const char *path, const char *prop_name, uint32_t *out_len);

/* Walk all top-level nodes whose name starts with prefix; call cb(base, size, ud) for each "reg" */
void fdt_foreach_node(const char *prefix, void (*cb)(uint64_t, uint64_t, void *), void *ud);

#endif
