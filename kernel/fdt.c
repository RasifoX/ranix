#include <kernel/fdt.h>
#include <kernel/kprintf.h>
#include <kernel/lib/string.h>

static void *_dtb;
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
        uint64_t total = 0;
        int first = 1;

        while (len >= entry_size) {
            uint64_t base, size;
            if (addr_cells == 2)
                base = be64(p);
            else
                base = be32(p);
            p += addr_cells * 4;

            if (size_cells == 2)
                size = be64(p);
            else
                size = (uint64_t)be32(p);
            p += size_cells * 4;

            if (first) {
                _info.mem_base = base;
                first = 0;
            }
            total += size;
            len -= entry_size;
        }
        _info.mem_size = total;
    }

    /* count cpus by walking struct block for /cpus/cpu@N nodes */
    _info.num_cpus = 0;
    {
        fdt_header_t *hdr = (fdt_header_t *)_dtb;
        uint8_t *structs = (uint8_t *)_dtb + be32(&hdr->off_dt_struct);
        uint32_t ssize = be32(&hdr->size_dt_struct);
        uint32_t off = 0;
        int depth = 0;
        int in_cpus = 0;

        while (off < ssize) {
            uint32_t token = be32(structs + off);
            off += 4;
            if (token == FDT_BEGIN_NODE) {
                const char *name = (const char *)(structs + off);
                uint32_t nlen = strlen(name) + 1;
                off += (nlen + 3) & ~3u;
                if (depth == 1 && node_name_match(name, "cpus"))
                    in_cpus = 1;
                if (in_cpus && depth == 2 && node_name_match(name, "cpu"))
                    _info.num_cpus++;
                depth++;
            } else if (token == FDT_END_NODE) {
                depth--;
                if (in_cpus && depth <= 1)
                    in_cpus = 0;
            } else if (token == FDT_PROP) {
                uint32_t plen = be32(structs + off); off += 8;
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
    _initialized = 1;

    parse_info();

    kprintf("fdt: %s  RAM %d MB  CPUs %d\n",
            _info.model, (int)(_info.mem_size >> 20), (int)_info.num_cpus);

    return 0;
}

fdt_info_t *fdt_get_info(void) { return &_info; }
void       *fdt_get_base(void) { return _dtb; }

uint32_t fdt_get_size(void)
{
    if (!_dtb) return 0;
    return be32(&((fdt_header_t *)_dtb)->totalsize);
}
