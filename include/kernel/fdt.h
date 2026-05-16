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
#define FDT_MAX_MEM_REGIONS 8
typedef struct {
    uint64_t mem_base;
    uint64_t mem_size;
    uint32_t num_cpus;
    char     model[64];
    char     compatible[64];
    struct {
        uint64_t base;
        uint64_t size;
    } mem_regions[FDT_MAX_MEM_REGIONS];
    int num_mem_regions;
} fdt_info_t;

int         fdt_init(void *dtb_addr);
fdt_info_t *fdt_get_info(void);
void       *fdt_get_base(void);
uint32_t    fdt_get_size(void);

/* low-level property lookup: returns pointer to value, sets *out_len */
const void *fdt_get_prop(const char *path, const char *prop_name, uint32_t *out_len);

#endif
