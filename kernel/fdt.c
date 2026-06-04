#include <kernel/fdt.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>

static void *_dtb;
static uint64_t _dtb_phys;
static fdt_info_t _info;
static int _initialized;

/* ── big-endian helpers ──────────────────────────────────── */

static uint32_t be32(const void *p)
{
    const volatile uint8_t *b = (const volatile uint8_t *)p;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) |
           ((uint32_t)b[2] << 8)  |  (uint32_t)b[3];
}

static uint64_t be64(const void *p)
{
    return ((uint64_t)be32(p) << 32) | (uint64_t)be32((const uint8_t *)p + 4);
}

/* ── string helpers ──────────────────────────────────────── */

static int node_name_match(const char *node, const char *name)
{
    /* match "memory@..." against "memory", or exact match */
    while (*name && *node == *name) { node++; name++; }
    if (*name == '\0' && (*node == '\0' || *node == '@'))
        return 1;
    return 0;
}

static void copy_str(char *dst, const void *src, uint32_t len, uint32_t max)
{
    if (len >= max) len = max - 1;
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/* ── low-level property lookup ───────────────────────────── */

const void *fdt_get_prop(const char *path, const char *prop_name, uint32_t *out_len)
{
    if (!_initialized || !_dtb)
        return NULL;

    fdt_header_t *hdr = (fdt_header_t *)_dtb;
    uint8_t *structs = (uint8_t *)_dtb + be32(&hdr->off_dt_struct);
    char *strings    = (char *)_dtb + be32(&hdr->off_dt_strings);
    uint32_t size    = be32(&hdr->size_dt_struct);
    uint32_t off     = 0;

    /* parse path: "/" means root, "/memory" means child named "memory" */
    const char *target = path;
    if (*target == '/') target++;

    int depth = 0;
    int found_depth = -1;  /* depth at which we found the target node */

    while (off < size) {
        uint32_t token = be32(structs + off);
        off += 4;

        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)(structs + off);
            uint32_t namelen = strlen(name) + 1;
            off += (namelen + 3) & ~3u;

            if (found_depth < 0) {
                if (*target == '\0' && depth == 0) {
                    /* looking for root — match the root BEGIN_NODE */
                    found_depth = depth;
                } else if (*target != '\0' && depth == 1) {
                    /* looking for top-level child like "memory" */
                    if (node_name_match(name, target))
                        found_depth = depth;
                }
            }
            depth++;
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (found_depth >= 0 && depth < found_depth)
                return NULL;  /* left the target node */
            break;
        case FDT_PROP: {
            uint32_t len    = be32(structs + off); off += 4;
            uint32_t nameoff = be32(structs + off); off += 4;
            const void *val  = structs + off;
            off += (len + 3) & ~3u;

            if (found_depth >= 0 && depth == found_depth + 1) {
                const char *pname = strings + nameoff;
                if (strcmp(pname, prop_name) == 0) {
                    if (out_len) *out_len = len;
                    return val;
                }
            }
            break;
        }
        case FDT_NOP:
            break;
        case FDT_END:
            return NULL;
        default:
            return NULL;
        }
    }
    return NULL;
}

extern uint64_t boot_initrd_start;
extern uint64_t boot_initrd_size;

/* ── parse DTB into fdt_info_t ───────────────────────────── */

static void parse_info(void)
{
    uint32_t len;
    const void *val;

    /* read root #address-cells and #size-cells (defaults: 2, 1) */
    uint32_t addr_cells = 2, size_cells = 1;
    val = fdt_get_prop("/", "#address-cells", &len);
    if (val && len >= 4) addr_cells = be32(val);
    val = fdt_get_prop("/", "#size-cells", &len);
    if (val && len >= 4) size_cells = be32(val);

    /* /chosen initrd */
    val = fdt_get_prop("chosen", "linux,initrd-start", &len);
    if (val) {
        if (len == 4) boot_initrd_start = be32(val);
        else if (len == 8) boot_initrd_start = be64(val);
        
        val = fdt_get_prop("chosen", "linux,initrd-end", &len);
        if (val) {
            uint64_t end = 0;
            if (len == 4) end = be32(val);
            else if (len == 8) end = be64(val);
            if (end > boot_initrd_start)
                boot_initrd_size = end - boot_initrd_start;
        }
    }

    /* /model */
    val = fdt_get_prop("/", "model", &len);
    if (val)
        copy_str(_info.model, val, len, sizeof(_info.model));
    else
        copy_str(_info.model, "unknown", 7, sizeof(_info.model));

    /* /compatible */
    val = fdt_get_prop("/", "compatible", &len);
    if (val)
        copy_str(_info.compatible, val, len, sizeof(_info.compatible));

    /* /memory reg — may have multiple base+size pairs (Pi 4 4GB: two regions) */
    val = fdt_get_prop("memory", "reg", &len);
    if (val) {
        const uint8_t *p = (const uint8_t *)val;
        uint32_t entry_size = (addr_cells + size_cells) * 4;
        int first = 1;

        _info.mem_total = 0;
        while (len >= entry_size) {
            uint64_t base = (addr_cells == 2) ? be64(p) : be32(p);
            p += addr_cells * 4;
            uint64_t size = (size_cells == 2) ? be64(p) : (uint64_t)be32(p);
            p += size_cells * 4;
            if (size > 0 && first) {
                _info.mem_base = base;
                _info.mem_size = size;
                first = 0;
            }
            if (size > 0) _info.mem_total += size;
            len -= entry_size;
        }
    }

    /* Walk /cpus/cpu@N: count CPUs and read each one's spin-table
     * cpu-release-addr straight from the DTB (no hardcoded addresses). */
    _info.num_cpus = 0;
    for (int i = 0; i < 8; i++) _info.cpu_release[i] = 0;
    {
        fdt_header_t *hdr = (fdt_header_t *)_dtb;
        uint8_t *structs = (uint8_t *)_dtb + be32(&hdr->off_dt_struct);
        char *strings    = (char *)_dtb + be32(&hdr->off_dt_strings);
        uint32_t ssize = be32(&hdr->size_dt_struct);
        uint32_t off = 0;
        int depth = 0;
        int in_cpus = 0;
        int cur_cpu = -1;   /* index of the cpu@N node we are currently inside */

        while (off < ssize) {
            uint32_t token = be32(structs + off);
            off += 4;
            if (token == FDT_BEGIN_NODE) {
                const char *name = (const char *)(structs + off);
                uint32_t nlen = strlen(name) + 1;
                off += (nlen + 3) & ~3u;
                if (depth == 1 && node_name_match(name, "cpus"))
                    in_cpus = 1;
                if (in_cpus && depth == 2 && node_name_match(name, "cpu")) {
                    cur_cpu = (int)_info.num_cpus;
                    _info.num_cpus++;
                }
                depth++;
            } else if (token == FDT_END_NODE) {
                depth--;
                if (in_cpus && depth == 2) cur_cpu = -1;
                if (in_cpus && depth <= 1)  in_cpus = 0;
            } else if (token == FDT_PROP) {
                uint32_t plen  = be32(structs + off);
                uint32_t noff  = be32(structs + off + 4);
                const void *pval = structs + off + 8;
                off += 8;
                if (cur_cpu >= 0 && cur_cpu < 8 &&
                    strcmp(strings + noff, "cpu-release-addr") == 0) {
                    if (plen == 8)      _info.cpu_release[cur_cpu] = be64(pval);
                    else if (plen == 4) _info.cpu_release[cur_cpu] = be32(pval);
                }
                off += (plen + 3) & ~3u;
            } else if (token == FDT_NOP) {
                continue;
            } else {
                break;
            }
        }
    }
    if (_info.num_cpus == 0)
        _info.num_cpus = 1;

    /* PSCI conduit: /psci { method = "hvc" | "smc" }. On real Pi4 the
     * firmware provides PSCI at EL3 via SMC; QEMU virt uses HVC. */
    _info.psci_present = 0;
    _info.psci_use_smc = 0;
    {
        uint32_t plen;
        const void *m = fdt_get_prop("psci", "method", &plen);
        if (m) {
            _info.psci_present = 1;
            if (plen >= 3 && ((const char *)m)[0] == 's')
                _info.psci_use_smc = 1;   /* "smc" */
        }
    }
}

/* ── public API ──────────────────────────────────────────── */

int fdt_init(void *dtb_addr)
{
    if (!dtb_addr)
        return -1;

    fdt_header_t *hdr = (fdt_header_t *)dtb_addr;
    if (be32(&hdr->magic) != FDT_MAGIC) {
    #ifndef __x86_64__
    	kprintf("fdt: bad magic: 0x%x\n", be32(&hdr->magic));
    #endif
    	return -1;
    }
    _dtb = dtb_addr;
    _dtb_phys = (uint64_t)(uintptr_t)dtb_addr;
    _initialized = 1;

    parse_info();

    kprintf("fdt: %s  RAM %d MB  CPUs %d\n",
            _info.model, (int)(_info.mem_total >> 20), (int)_info.num_cpus);

    return 0;
}

fdt_info_t *fdt_get_info(void) { return &_info; }
void       *fdt_get_base(void) { return _dtb; }
uint64_t    fdt_get_phys(void) { return _dtb_phys; }

/* Call after mmu_init to remap _dtb from physical to virtual address */
void fdt_remap(void)
{
    if (!_dtb) return;
    /* Only remap if still using physical address (< 0xffff...0) */
    if ((uint64_t)(uintptr_t)_dtb < 0xffff000000000000ULL) {
#if defined(__aarch64__)
        _dtb = (void *)((uint64_t)(uintptr_t)_dtb + 0xFFFF800000000000ULL);
#endif
    }
}

void fdt_foreach_mem_region(void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    if (!_initialized || !_dtb) return;

    uint32_t len;
    const void *v;
    uint32_t addr_cells = 2, size_cells = 1;
    v = fdt_get_prop("/", "#address-cells", &len);
    if (v && len >= 4) addr_cells = be32(v);
    v = fdt_get_prop("/", "#size-cells", &len);
    if (v && len >= 4) size_cells = be32(v);

    fdt_header_t *hdr  = (fdt_header_t *)_dtb;
    uint8_t      *stru = (uint8_t *)_dtb + be32(&hdr->off_dt_struct);
    char         *strs = (char *)_dtb + be32(&hdr->off_dt_strings);
    uint32_t      ssz  = be32(&hdr->size_dt_struct);
    uint32_t      off  = 0;
    int depth = 0, in_mem = 0;
    uint32_t entry_size = (addr_cells + size_cells) * 4;

    while (off < ssz) {
        uint32_t token = be32(stru + off); off += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)(stru + off);
            uint32_t nlen = strlen(name) + 1;
            off += (nlen + 3) & ~3u;
            if (depth == 1 && node_name_match(name, "memory")) in_mem = 1;
            depth++;
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (in_mem && depth <= 1) in_mem = 0;
            break;
        case FDT_PROP: {
            uint32_t plen    = be32(stru + off); off += 4;
            uint32_t nameoff = be32(stru + off); off += 4;
            const void *val  = stru + off;
            off += (plen + 3) & ~3u;
            if (in_mem && strcmp(strs + nameoff, "reg") == 0) {
                const uint8_t *p = (const uint8_t *)val;
                uint32_t rem = plen;
                while (rem >= entry_size) {
                    uint64_t base = (addr_cells == 2) ? be64(p) : be32(p);
                    p += addr_cells * 4;
                    uint64_t size = (size_cells == 2) ? be64(p) : (uint64_t)be32(p);
                    p += size_cells * 4;
                    rem -= entry_size;
                    if (size > 0) cb(base, size, ud);
                }
            }
            break;
        }
        case FDT_NOP: break;
        case FDT_END: return;
        default:      return;
        }
    }
}

uint32_t fdt_get_size(void)
{
    if (!_dtb) return 0;
    return be32(&((fdt_header_t *)_dtb)->totalsize);
}

void fdt_foreach_node(const char *prefix, void (*cb)(uint64_t, uint64_t, void *), void *ud)
{
    if (!_initialized || !_dtb) return;

    uint32_t addr_cells = 1, size_cells = 1;
    uint32_t len;
    const void *v;
    v = fdt_get_prop("/", "#address-cells", &len);
    if (v && len >= 4) addr_cells = be32(v);
    v = fdt_get_prop("/", "#size-cells", &len);
    if (v && len >= 4) size_cells = be32(v);

    fdt_header_t *hdr  = (fdt_header_t *)_dtb;
    uint8_t      *stru = (uint8_t *)_dtb + be32(&hdr->off_dt_struct);
    char         *strs = (char *)_dtb + be32(&hdr->off_dt_strings);
    uint32_t      ssz  = be32(&hdr->size_dt_struct);
    uint32_t      off  = 0;
    int depth = 0, in_node = 0;
    uint32_t entry_sz = (addr_cells + size_cells) * 4;

    while (off < ssz) {
        uint32_t token = be32(stru + off); off += 4;
        switch (token) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)(stru + off);
            uint32_t nlen = strlen(name) + 1;
            off += (nlen + 3) & ~3u;
            if (depth == 1 && node_name_match(name, prefix)) in_node = 1;
            depth++;
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (in_node && depth <= 1) in_node = 0;
            break;
        case FDT_PROP: {
            uint32_t plen    = be32(stru + off); off += 4;
            uint32_t nameoff = be32(stru + off); off += 4;
            const void *val  = stru + off;
            off += (plen + 3) & ~3u;
            if (in_node && strcmp(strs + nameoff, "reg") == 0 && plen >= entry_sz) {
                const uint8_t *p = (const uint8_t *)val;
                uint64_t base = (addr_cells == 2) ? be64(p) : (uint64_t)be32(p);
                p += addr_cells * 4;
                uint64_t size = (size_cells == 2) ? be64(p) : (uint64_t)be32(p);
                if (size > 0) cb(base, size, ud);
            }
            break;
        }
        case FDT_NOP: break;
        case FDT_END: return;
        default: return;
        }
    }
}
