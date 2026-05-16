#include <kernel/fs/mbr.h>
#include <kernel/block.h>
#include <kernel/mm/buddy.h>
#include <kernel/mm/heap.h>
#include <kernel/lib/string.h>
#include <kernel/kprintf.h>

/*
 * MBR partition table parser.
 * Reads sector 0, validates 0x55AA signature, creates partition
 * block_device_t slices for each active entry.
 */

#define MBR_SIGNATURE  0xAA55
#define MBR_PART_TABLE 446
#define MBR_PARTS      4

typedef struct __attribute__((packed)) {
	uint8_t  status;        /* 0x80 = bootable */
	uint8_t  chs_first[3];
	uint8_t  type;          /* 0x83=Linux, 0x8E=LVM, 0x82=swap, 0xEE=GPT */
	uint8_t  chs_last[3];
	uint32_t lba_start;
	uint32_t lba_size;
} mbr_entry_t;

typedef struct {
	uint8_t   boot_code[446];
	mbr_entry_t parts[4];
	uint16_t  signature;
} __attribute__((packed)) mbr_t;

typedef struct {
	block_device_t *parent;
	uint64_t        start;
} part_priv_t;

static block_device_t *part_devs;
static part_priv_t    *part_privs;
static int             part_cnt = 0;
static int             part_cap = 0;

static int part_read(block_device_t *dev, uint64_t lba, void *buf, uint64_t count)
{
	part_priv_t *p = dev->priv;
	return p->parent->read(p->parent, p->start + lba, buf, count);
}

static int part_write(block_device_t *dev, uint64_t lba, const void *buf, uint64_t count)
{
	part_priv_t *p = dev->priv;
	return p->parent->write(p->parent, p->start + lba, buf, count);
}

int mbr_probe(block_device_t *dev)
{
	uint8_t *sector = buddy_alloc(0);
	if (!sector) return -1;

	if (dev->read(dev, 0, sector, 1) != 0) {
		buddy_free((uint64_t)sector, 0);
		return -1;
	}

	mbr_t *mbr = (mbr_t *)sector;
	if (mbr->signature != MBR_SIGNATURE) {
		buddy_free((uint64_t)sector, 0);
		return -1;
	}

	int found = 0;
	for (int i = 0; i < MBR_PARTS; i++) {
		mbr_entry_t *e = &mbr->parts[i];
		if (e->lba_size == 0 || e->type == 0) continue;
		if (part_cnt >= part_cap) {
			int nc = part_cap ? part_cap * 2 : 8;
			block_device_t *nd = kmalloc((size_t)nc * sizeof(block_device_t));
			part_priv_t    *np = kmalloc((size_t)nc * sizeof(part_priv_t));
			if (!nd || !np) { kfree(nd); kfree(np); break; }
			if (part_devs) {
				memcpy(nd, part_devs,  (size_t)part_cnt * sizeof(block_device_t));
				memcpy(np, part_privs, (size_t)part_cnt * sizeof(part_priv_t));
				/* fix up priv pointers after move */
				for (int j = 0; j < part_cnt; j++) nd[j].priv = &np[j];
				kfree(part_devs); kfree(part_privs);
			}
			part_devs = nd; part_privs = np; part_cap = nc;
		}

		part_priv_t   *pp  = &part_privs[part_cnt];
		block_device_t *pd = &part_devs[part_cnt];
		pp->parent      = dev;
		pp->start       = e->lba_start;

		memset(pd, 0, sizeof(*pd));
		pd->name[0] = dev->name[0];
		pd->name[1] = dev->name[1];
		pd->name[2] = dev->name[2];
		pd->name[3] = dev->name[3];
		pd->name[4] = dev->name[4];
		pd->name[5] = 'p';
		pd->name[6] = (char)('0' + part_cnt);
		pd->name[7] = '\0';
		pd->size_blocks = e->lba_size;
		pd->read        = part_read;
		pd->write       = part_write;
		pd->priv        = pp;

		block_device_register(pd);
		kprintf("mbr: %s type=0x%02x start=%u size=%u\n",
		        pd->name, e->type, e->lba_start, e->lba_size);
		part_cnt++;
		found++;
	}

	buddy_free((uint64_t)sector, 0);
	return found > 0 ? 0 : -1;
}
