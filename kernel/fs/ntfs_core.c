/*
 * NTFS volume reader. Written from Microsoft's public documentation (MS-FSCC,
 * MS-XCA for LZNT1, the Windows reparse tag table) and independent format
 * descriptions. There is no code path here that writes: the only way to a device
 * is the read callback given to ntfs_open. $LogFile is never looked at, so a
 * dirty volume is read as it stands.
 *
 * Every number below comes from the disk and is checked before it is used as a
 * size, an offset or a count. A record or block that fails a check gives an error
 * rather than a guess, because a wrong answer here is a silently bad copy.
 */
#include <kernel/fs/ntfs_core.h>

#ifdef NTFS_HOST
#include <stdlib.h>
#include <string.h>
void *ntfs_host_alloc(size_t n);    /* the harness fails some of these on purpose */
#define NTFS_ALLOC(n) ntfs_host_alloc(n)
#define NTFS_FREE(p)  free(p)
#else
#include <kernel/lib/string.h>
#include <kernel/mm/heap.h>
#define NTFS_ALLOC(n) kmalloc(n)
#define NTFS_FREE(p)  kfree(p)
#endif

/* attribute types */
#define AT_STANDARD_INFO   0x10u
#define AT_ATTRIBUTE_LIST  0x20u
#define AT_FILE_NAME       0x30u
#define AT_VOLUME_NAME     0x60u
#define AT_VOLUME_INFO     0x70u
#define AT_DATA            0x80u
#define AT_INDEX_ROOT      0x90u
#define AT_INDEX_ALLOC     0xA0u
#define AT_BITMAP          0xB0u
#define AT_REPARSE         0xC0u
#define AT_END             0xFFFFFFFFu

/* attribute flags */
#define AF_COMPRESSED_MASK 0x00FFu
#define AF_ENCRYPTED       0x4000u
#define AF_SPARSE          0x8000u

/* MFT record flags */
#define RF_IN_USE   0x0001u
#define RF_DIR      0x0002u

/* file attributes */
#define FA_REPARSE          0x00000400u
#define FA_OFFLINE          0x00001000u
#define FA_ENCRYPTED        0x00004000u
#define FA_RECALL_ON_OPEN   0x00040000u
#define FA_RECALL_ON_ACCESS 0x00400000u
#define FA_DIR_INDEX        0x10000000u

/* reparse tags */
#define TAG_SYMLINK     0xA000000Cu
#define TAG_JUNCTION    0xA0000003u
#define TAG_LX_SYMLINK  0xA000001Du
#define TAG_WOF         0x80000017u
#define TAG_DEDUP       0x80000013u
#define TAG_CLOUD_MASK  0xFFFF0FFFu
#define TAG_CLOUD       0x9000001Au

#define MREF_NO(r)   ((r) & 0x0000FFFFFFFFFFFFULL)
#define MREF_SEQ(r)  ((uint16_t)((r) >> 48))

#define LCN_HOLE          (~0ULL)
#define MAX_RUNS          (1u << 22)            /* 4 M extents, 96 MB of table */
#define MAX_LIST_BYTES    (1u << 20)            /* an $ATTRIBUTE_LIST: NTFS caps it far lower */
#define MAX_BITMAP_BYTES  (1u << 20)            /* 8 M index blocks */
#define MAX_RESIDENT      (1u << 16)
#define MAX_REPARSE_BYTES (16u * 1024)          /* the format's own limit */
#define MAX_DIR_ENTRIES   (1u << 22)
#define MAX_NAMES_BYTES   (1u << 28)
#define MAX_BAD_BLOCKS    256u
#define MAX_READ_SECTORS  256u                  /* one device command, 128 KB */
#define MAX_POS           (1ULL << 62)

#define RC_SLOTS      128u
#define STREAM_SLOTS  8u
#define DIR_SLOTS     4u

typedef struct {
	uint64_t vcn, lcn, len;
} ntfs_run_t;

typedef struct {
	uint64_t    id;          /* unique per open, keys the decompression cache */
	uint64_t    mft;
	uint64_t    size, init, alloc;
	uint64_t    end_vcn;     /* first VCN no run covers yet */
	uint64_t    used;        /* cache age */
	uint32_t    flags;
	uint32_t    cu_shift;
	uint32_t    nruns, cap, hint;
	uint8_t     resident, started, comp, unreadable;
	uint32_t    res_len;
	uint8_t    *res;
	ntfs_run_t *runs;
} ntfs_stream_t;

typedef struct {
	uint64_t mref;
	uint32_t name;           /* offset in names */
	uint32_t longname;       /* offset in names, or ~0 */
	uint8_t  is_dir, is_reparse, hidden;
} dir_ent_t;

struct ntfs_dir {
	uint64_t   mft;
	uint64_t   used;
	dir_ent_t *ents;
	uint32_t   n, cap;       /* n: slots; shown: those not hidden as duplicates */
	char      *names;
	uint32_t   names_len, names_cap;
	uint32_t  *hash;         /* open addressing, entry index + 1 */
	uint32_t   hash_mask;
	uint32_t  *shown;        /* visible entry number to slot */
	uint32_t   nshown;
	int        damaged;
};

struct ntfs_vol {
	ntfs_read_fn rd;
	void        *dev;
	uint64_t     dev_sectors;
	uint32_t     sector_size, cluster_size, rec_size;
	uint64_t     total_clusters, mft_lcn;
	uint64_t     max_vcn;
	ntfs_volinfo_t info;
	ntfs_stream_t mft;
	uint8_t     *rec_a, *rec_b;     /* stream_open owns these two; never kept across it */
	uint8_t     *rc;                /* decoded records, RC_SLOTS of them */
	uint64_t     rc_tag[RC_SLOTS];  /* record number + 1 */
	uint8_t      sec[512];
	ntfs_stream_t *streams[STREAM_SLOTS];
	ntfs_dir_t   *dirs[DIR_SLOTS];
	uint64_t     clock, next_id;
	uint8_t     *cu_in, *cu_out;
	uint32_t     cu_cap;
	uint64_t     cu_stream, cu_idx;
	int          cu_valid;
	uint8_t     *scratch;           /* MAX_REPARSE_BYTES, for reparse data */
	char         name[NTFS_LONGNAME_MAX];
};

/* ── little-endian access, nothing here assumes alignment ──────── */

static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static uint32_t le32(const uint8_t *p)
{
	return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t le64(const uint8_t *p) { return (uint64_t)le32(p) | (uint64_t)le32(p + 4) << 32; }

static int is_pow2(uint64_t v) { return v && !(v & (v - 1)); }

static void *zalloc(size_t n)
{
	void *p = NTFS_ALLOC(n);
	if (p) memset(p, 0, n);
	return p;
}

/* ── device ────────────────────────────────────────────────────── */

static int vol_read(ntfs_vol_t *v, uint64_t off, void *buf, uint64_t len)
{
	uint8_t *out = buf;
	if (len > (v->dev_sectors << 9) || off > (v->dev_sectors << 9) - len) return NTFS_EIO;
	while (len) {
		uint64_t lba = off >> 9;
		uint32_t in  = (uint32_t)(off & 511);
		uint64_t n;
		if (in || len < 512) {
			if (v->rd(v->dev, lba, v->sec, 1) != 0) return NTFS_EIO;
			n = 512 - in;
			if (n > len) n = len;
			memcpy(out, v->sec + in, (size_t)n);
		} else {
			uint64_t secs = len >> 9;
			if (secs > MAX_READ_SECTORS) secs = MAX_READ_SECTORS;
			if (v->rd(v->dev, lba, out, (uint32_t)secs) != 0) return NTFS_EIO;
			n = secs << 9;
		}
		out += n; off += n; len -= n;
	}
	return 0;
}

/* ── protected records ─────────────────────────────────────────── */

/* Undo the update sequence of a "FILE" or "INDX" block in memory. The last word of
 * every stride was swapped for a check value so a torn write shows; a mismatch
 * means the block is not one whole write and is refused. */
static int fixup(uint8_t *b, uint32_t size, const char *magic)
{
	if (memcmp(b, magic, 4) != 0) return NTFS_EIO;
	uint32_t uoff = le16(b + 4), ucnt = le16(b + 6);
	if (ucnt < 2 || (uoff & 1) || uoff < 8) return NTFS_EIO;
	uint32_t blocks = ucnt - 1;
	if (size % blocks) return NTFS_EIO;
	uint32_t stride = size / blocks;
	if (stride < 512 || !is_pow2(stride)) return NTFS_EIO;
	if ((uint64_t)uoff + 2ULL * ucnt > stride - 2) return NTFS_EIO;
	for (uint32_t i = 1; i <= blocks; i++) {
		uint8_t *tail = b + i * stride - 2;
		if (tail[0] != b[uoff] || tail[1] != b[uoff + 1]) return NTFS_EIO;
		tail[0] = b[uoff + 2 * i];
		tail[1] = b[uoff + 2 * i + 1];
	}
	return 0;
}

static int rec_read(ntfs_vol_t *v, uint64_t no, uint8_t *out);

/* ── attributes ────────────────────────────────────────────────── */

typedef struct {
	const uint8_t *p;     /* the attribute header */
	uint32_t len, type;
	uint32_t name_len;    /* UTF-16 units */
	const uint8_t *name;
	uint32_t flags;
	uint16_t id;
	uint8_t  nonres;
	/* resident */
	const uint8_t *val;
	uint32_t val_len;
	/* non-resident */
	uint64_t lowest, highest;
} attr_t;

/* Bounds of the attribute area of a fixed-up record. */
static int rec_area(const uint8_t *r, uint32_t size, uint32_t *start, uint32_t *end)
{
	uint32_t first = le16(r + 20), used = le32(r + 24);
	if (used > size) return NTFS_EIO;
	if (first < 42 || (first & 7) || first + 8 > used) return NTFS_EIO;
	*start = first;
	*end = used;
	return 0;
}

/* The attribute at *pos: 1 and advance, 0 at the end marker, negative if malformed.
 * After a 1 every pointer in a stays inside the record. */
static int attr_next(const uint8_t *r, uint32_t *pos, uint32_t end, attr_t *a)
{
	uint32_t o = *pos;
	if (o + 4 > end) return NTFS_EIO;
	uint32_t type = le32(r + o);
	if (type == AT_END) return 0;
	if (o + 24 > end) return NTFS_EIO;
	uint32_t len = le32(r + o + 4);
	if (len < 24 || (len & 7) || len > end - o) return NTFS_EIO;

	const uint8_t *p = r + o;
	memset(a, 0, sizeof(*a));
	a->p = p; a->len = len; a->type = type;
	a->nonres = p[8];
	a->name_len = p[9];
	a->flags = le16(p + 12);
	a->id = le16(p + 14);
	if (a->nonres > 1) return NTFS_EIO;
	if (a->name_len) {
		uint32_t no = le16(p + 10);
		if (no + 2 * a->name_len > len) return NTFS_EIO;
		a->name = p + no;
	}
	if (!a->nonres) {
		uint32_t vl = le32(p + 16), vo = le16(p + 20);
		if (vo < 24 || vo > len || vl > len - vo) return NTFS_EIO;
		a->val = p + vo;
		a->val_len = vl;
	} else {
		if (len < 64) return NTFS_EIO;
		uint32_t mp = le16(p + 32);
		if (mp < 64 || mp > len) return NTFS_EIO;
		a->lowest  = le64(p + 16);
		a->highest = le64(p + 24);
	}
	*pos = o + len;
	return 1;
}

static int name_eq(const attr_t *a, const uint8_t *name, uint32_t nlen)
{
	if (a->name_len != nlen) return 0;
	return !nlen || memcmp(a->name, name, 2 * (size_t)nlen) == 0;
}

/* ── run lists ─────────────────────────────────────────────────── */

static int run_push(ntfs_stream_t *s, uint64_t vcn, uint64_t lcn, uint64_t len)
{
	if (s->nruns == s->cap) {
		uint32_t cap = s->cap ? s->cap * 2 : 16;
		if (cap > MAX_RUNS) return NTFS_EFBIG;
		ntfs_run_t *nr = NTFS_ALLOC(cap * sizeof(*nr));
		if (!nr) return NTFS_ENOMEM;
		if (s->runs) memcpy(nr, s->runs, s->nruns * sizeof(*nr));
		NTFS_FREE(s->runs);
		s->runs = nr;
		s->cap = cap;
	}
	ntfs_run_t *r = &s->runs[s->nruns++];
	r->vcn = vcn; r->lcn = lcn; r->len = len;
	return 0;
}

/* Decode the mapping pairs of a non-resident attribute into s. Each pair is a
 * header nibble-sized length and a signed LCN delta; no delta bytes is a hole. */
static int runs_decode(ntfs_vol_t *v, ntfs_stream_t *s, const attr_t *a)
{
	const uint8_t *p = a->p;
	uint32_t pos = le16(p + 32), end = a->len;
	uint64_t vcn = a->lowest;
	int64_t  lcn = 0;

	if (vcn != s->end_vcn) return NTFS_EIO;
	for (;;) {
		if (pos >= end) return NTFS_EIO;
		uint8_t h = p[pos++];
		if (!h) break;
		uint32_t ln = h & 0xF, on = h >> 4;
		if (!ln || ln > 8 || on > 8 || ln + on > end - pos) return NTFS_EIO;
		uint64_t len = 0;
		for (uint32_t i = 0; i < ln; i++) len |= (uint64_t)p[pos + i] << (8 * i);
		pos += ln;
		if ((int64_t)len <= 0 || len > v->max_vcn || vcn > v->max_vcn - len) return NTFS_EIO;
		uint64_t at = LCN_HOLE;
		if (on) {
			uint64_t d = 0;
			for (uint32_t i = 0; i < on; i++) d |= (uint64_t)p[pos + i] << (8 * i);
			pos += on;
			if (on < 8 && (d >> (8 * on - 1)) & 1) d |= ~0ULL << (8 * on);   /* sign-extend */
			int64_t sd = (int64_t)d;
			if (sd > (int64_t)(1ULL << 62) || sd < -(int64_t)(1ULL << 62)) return NTFS_EIO;
			lcn += sd;      /* lcn stays in [0, total_clusters) between steps */
			if (lcn < 0 || (uint64_t)lcn >= v->total_clusters ||
			    len > v->total_clusters - (uint64_t)lcn)
				return NTFS_EIO;
			at = (uint64_t)lcn;
		}
		int r = run_push(s, vcn, at, len);
		if (r) return r;
		vcn += len;
	}
	s->end_vcn = vcn;
	return 0;
}

static const ntfs_run_t *run_find(ntfs_stream_t *s, uint64_t vcn)
{
	uint32_t i = s->hint;
	if (i < s->nruns && s->runs[i].vcn <= vcn && vcn - s->runs[i].vcn < s->runs[i].len)
		return &s->runs[i];
	if (i + 1 < s->nruns && s->runs[i + 1].vcn <= vcn &&
	    vcn - s->runs[i + 1].vcn < s->runs[i + 1].len) {
		s->hint = i + 1;
		return &s->runs[i + 1];
	}
	uint32_t lo = 0, hi = s->nruns;
	while (lo < hi) {
		uint32_t mid = lo + (hi - lo) / 2;
		const ntfs_run_t *r = &s->runs[mid];
		if (vcn < r->vcn) hi = mid;
		else if (vcn - r->vcn >= r->len) lo = mid + 1;
		else { s->hint = mid; return r; }
	}
	return NULL;
}

/* ── streams ───────────────────────────────────────────────────── */

static void stream_clear(ntfs_stream_t *s)
{
	NTFS_FREE(s->runs);
	NTFS_FREE(s->res);
	memset(s, 0, sizeof(*s));
}

/* Take one attribute record of the stream. Sizes live in the record with the lowest
 * VCN, later extents add runs only. */
static int stream_take(ntfs_vol_t *v, ntfs_stream_t *s, const attr_t *a, int hdr_only)
{
	if (!a->nonres) {
		if (s->started) return NTFS_EIO;
		s->started = 1;
		s->resident = 1;
		s->size = s->init = s->alloc = a->val_len;
		s->flags = a->flags;
		if (hdr_only || !a->val_len) return 0;
		if (a->val_len > MAX_RESIDENT) return NTFS_EIO;
		s->res = NTFS_ALLOC(a->val_len);
		if (!s->res) return NTFS_ENOMEM;
		memcpy(s->res, a->val, a->val_len);
		s->res_len = a->val_len;
		return 0;
	}
	if (s->resident) return NTFS_EIO;
	if (!s->started) {
		const uint8_t *p = a->p;
		if (a->lowest) return NTFS_EIO;
		s->started = 1;
		s->alloc = le64(p + 40);
		s->size  = le64(p + 48);
		s->init  = le64(p + 56);
		s->flags = a->flags;
		if (s->size > MAX_POS || s->alloc > MAX_POS || s->init > MAX_POS) return NTFS_EIO;
		/* the data lives in the allocation: a size past it reads as endless zeros */
		if (s->size > s->alloc) return NTFS_EIO;
		if (s->init > s->size) s->init = s->size;
		s->comp = (s->flags & AF_COMPRESSED_MASK) != 0;
		if (s->comp) {
			uint32_t shift = p[34];
			/* compression units are the cluster count of a power of two */
			if (!shift || shift > 16 ||
			    (uint64_t)v->cluster_size << shift > (1u << 20))
				return NTFS_EIO;
			s->cu_shift = shift;
		}
	}
	return hdr_only ? 0 : runs_decode(v, s, a);
}

static long stream_read(ntfs_vol_t *v, ntfs_stream_t *s, uint64_t off, void *buf, uint64_t len);

/* The value of a resident or non-resident attribute, in a fresh buffer. */
static int attr_value(ntfs_vol_t *v, const attr_t *a, uint32_t max, uint8_t **out, uint32_t *out_len)
{
	ntfs_stream_t t;
	memset(&t, 0, sizeof(t));
	int r = stream_take(v, &t, a, 0);
	if (r) { stream_clear(&t); return r; }
	if (t.size > max) { stream_clear(&t); return NTFS_EFBIG; }
	uint32_t n = (uint32_t)t.size;
	uint8_t *b = NTFS_ALLOC(n ? n : 1);
	if (!b) { stream_clear(&t); return NTFS_ENOMEM; }
	long got = stream_read(v, &t, 0, b, n);
	stream_clear(&t);
	if (got != (long)n) { NTFS_FREE(b); return got < 0 ? (int)got : NTFS_EIO; }
	*out = b;
	*out_len = n;
	return 0;
}

/* $ATTRIBUTE_LIST of a record, or none. */
static int list_load(ntfs_vol_t *v, const uint8_t *r, uint8_t **out, uint32_t *out_len)
{
	uint32_t pos, end;
	attr_t a;
	*out = NULL;
	*out_len = 0;
	int rc = rec_area(r, v->rec_size, &pos, &end);
	if (rc) return rc;
	while ((rc = attr_next(r, &pos, end, &a)) > 0)
		if (a.type == AT_ATTRIBUTE_LIST) return attr_value(v, &a, MAX_LIST_BYTES, out, out_len);
	return rc;
}

/* Walk list entries: *pos to the next one. Fields in le are checked to stay inside. */
typedef struct {
	uint32_t type;
	uint32_t name_len;
	const uint8_t *name;
	uint64_t lowest, mref;
	uint16_t id;
} list_ent_t;

static int list_next(const uint8_t *l, uint32_t len, uint32_t *pos, list_ent_t *e)
{
	uint32_t o = *pos;
	if (o >= len) return 0;
	if (len - o < 26) return NTFS_EIO;
	uint32_t elen = le16(l + o + 4);
	if (elen < 26 || (elen & 7) || elen > len - o) return NTFS_EIO;
	e->type = le32(l + o);
	e->name_len = l[o + 6];
	uint32_t no = l[o + 7];
	if (e->name_len && no + 2 * e->name_len > elen) return NTFS_EIO;
	e->name = l + o + no;
	e->lowest = le64(l + o + 8);
	e->mref = le64(l + o + 16);
	e->id = le16(l + o + 24);
	*pos = o + elen;
	return 1;
}

static int list_name_eq(const list_ent_t *e, const uint8_t *name, uint32_t nlen)
{
	if (e->name_len != nlen) return 0;
	return !nlen || memcmp(e->name, name, 2 * (size_t)nlen) == 0;
}

/* Gather all extents of (type, name) of record no into s. Uses rec_a and rec_b. */
static int stream_open(ntfs_vol_t *v, uint64_t no, uint32_t type, const uint8_t *name,
                       uint32_t nlen, int hdr_only, ntfs_stream_t *s)
{
	int r = rec_read(v, no, v->rec_a);
	if (r) return r;

	uint8_t *list = NULL;
	uint32_t llen = 0;
	r = list_load(v, v->rec_a, &list, &llen);
	if (r) return r;

	if (!list) {
		uint32_t pos, end;
		attr_t a;
		r = rec_area(v->rec_a, v->rec_size, &pos, &end);
		if (r) return r;
		while ((r = attr_next(v->rec_a, &pos, end, &a)) > 0) {
			if (a.type != type || !name_eq(&a, name, nlen)) continue;
			r = stream_take(v, s, &a, hdr_only);
			if (r) return r;
			if (hdr_only) return 0;
		}
		if (r < 0) return r;
		return s->started ? 0 : NTFS_ENOENT;
	}

	uint32_t lpos = 0;
	list_ent_t e;
	r = NTFS_ENOENT;
	int lr;
	while ((lr = list_next(list, llen, &lpos, &e)) > 0) {
		if (e.type != type || !list_name_eq(&e, name, nlen)) continue;
		const uint8_t *rec = v->rec_a;
		uint64_t eno = MREF_NO(e.mref);
		if (eno != no) {
			r = rec_read(v, eno, v->rec_b);
			if (r) break;
			if (MREF_NO(le64(v->rec_b + 32)) != no) { r = NTFS_EIO; break; }
			rec = v->rec_b;
		}
		uint32_t pos, end;
		attr_t a;
		int ar = rec_area(rec, v->rec_size, &pos, &end);
		if (ar) { r = ar; break; }
		int found = 0;
		while ((ar = attr_next(rec, &pos, end, &a)) > 0) {
			if (a.type != type || a.id != e.id || !name_eq(&a, name, nlen)) continue;
			found = 1;
			break;
		}
		if (ar < 0) { r = ar; break; }
		if (!found) { r = NTFS_EIO; break; }
		r = stream_take(v, s, &a, hdr_only);
		if (r || hdr_only) break;
	}
	if (lr < 0) r = lr;
	NTFS_FREE(list);
	if (r) return r;
	return s->started ? 0 : NTFS_ENOENT;
}

/* Decode LZNT1 (MS-XCA section 2.5). Each chunk holds at most 4 KB of output; a
 * phrase token splits its 16 bits between displacement and length according to how
 * far the chunk has got, so a back-reference cannot reach before the chunk. */
long ntfs_lznt1(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t cap)
{
	uint32_t ip = 0, op = 0;
	while (ip + 2 <= in_len && op < cap) {
		uint32_t h = le16(in + ip);
		ip += 2;
		if (!h) break;
		uint32_t size = (h & 0xFFF) + 1;
		if (size > in_len - ip) return NTFS_EIO;
		uint32_t cend = ip + size;
		uint32_t start = op;
		uint32_t limit = cap - start > 4096 ? start + 4096 : cap;

		if (!(h & 0x8000)) {
			if (size > limit - op) return NTFS_EIO;
			memcpy(out + op, in + ip, size);
			op += size;
			ip = cend;
		} else {
			while (ip < cend && op < limit) {
				uint8_t tags = in[ip++];
				for (int bit = 0; bit < 8 && ip < cend && op < limit; bit++) {
					if (!(tags >> bit & 1)) {
						out[op++] = in[ip++];
						continue;
					}
					if (cend - ip < 2) return NTFS_EIO;
					uint32_t tok = le16(in + ip);
					ip += 2;
					uint32_t pos = op - start;
					if (!pos) return NTFS_EIO;
					uint32_t lbits = 12;
					for (uint32_t p = pos - 1; p >= 0x10; p >>= 1) lbits--;
					uint32_t disp = (tok >> lbits) + 1;
					uint32_t mlen = (tok & ((1u << lbits) - 1)) + 3;
					if (disp > pos || mlen > limit - op) return NTFS_EIO;
					for (uint32_t i = 0; i < mlen; i++, op++) out[op] = out[op - disp];
				}
			}
			ip = cend;
		}
		/* a chunk that ends short leaves the rest of its 4 KB as zeros */
		if (in_len - ip >= 2 && le16(in + ip) && op < limit) {
			memset(out + op, 0, limit - op);
			op = limit;
		}
	}
	return (long)op;
}

/* Fill the cache with compression unit idx of s. A unit stored in fewer clusters
 * than it spans is compressed and ends in a hole; a full one is stored raw; one with
 * no clusters is zeros. */
static int cu_load(ntfs_vol_t *v, ntfs_stream_t *s, uint64_t idx)
{
	uint32_t cu_cl = 1u << s->cu_shift;
	uint32_t cu = v->cluster_size << s->cu_shift;
	if (v->cu_valid && v->cu_stream == s->id && v->cu_idx == idx) return 0;
	if (cu > v->cu_cap) {
		uint8_t *a = NTFS_ALLOC(cu), *b = NTFS_ALLOC(cu);
		if (!a || !b) { NTFS_FREE(a); NTFS_FREE(b); return NTFS_ENOMEM; }
		NTFS_FREE(v->cu_in);
		NTFS_FREE(v->cu_out);
		v->cu_in = a;
		v->cu_out = b;
		v->cu_cap = cu;
	}
	v->cu_valid = 0;

	uint64_t base = idx << s->cu_shift;
	uint32_t real = 0, c = 0;
	int hole_seen = 0;
	while (c < cu_cl) {
		const ntfs_run_t *r = run_find(s, base + c);
		if (!r) {
			if (!c) return NTFS_EIO;
			break;     /* the run list may stop at the end of the last unit's data */
		}
		uint64_t in  = base + c - r->vcn;
		uint64_t n64 = r->len - in;
		uint32_t n   = n64 > cu_cl - c ? cu_cl - c : (uint32_t)n64;
		if (r->lcn == LCN_HOLE) {
			hole_seen = 1;
		} else {
			if (hole_seen) return NTFS_EIO;
			int rc = vol_read(v, (r->lcn + in) * v->cluster_size,
			                  v->cu_in + (uint64_t)real * v->cluster_size,
			                  (uint64_t)n * v->cluster_size);
			if (rc) return rc;
			real += n;
		}
		c += n;
	}
	if (!real) {
		memset(v->cu_out, 0, cu);
	} else if (real == cu_cl) {
		memcpy(v->cu_out, v->cu_in, cu);
	} else {
		long got = ntfs_lznt1(v->cu_in, real * v->cluster_size, v->cu_out, cu);
		if (got < 0) return (int)got;
		memset(v->cu_out + got, 0, cu - (uint32_t)got);
	}
	v->cu_stream = s->id;
	v->cu_idx = idx;
	v->cu_valid = 1;
	return 0;
}

static long stream_read(ntfs_vol_t *v, ntfs_stream_t *s, uint64_t off, void *buf, uint64_t len)
{
	uint8_t *out = buf;
	if (off >= s->size) return 0;
	if (len > s->size - off) len = s->size - off;
	if (len > (1u << 30)) len = 1u << 30;

	if (s->resident) {
		uint64_t have = s->res_len > off ? s->res_len - off : 0;
		if (have < len) return NTFS_EIO;
		memcpy(out, s->res + off, (size_t)len);
		return (long)len;
	}

	uint64_t done = 0;
	while (done < len) {
		uint64_t pos = off + done;
		uint64_t n = len - done;
		if (pos >= s->init) {          /* past the valid data length: zeros */
			memset(out + done, 0, (size_t)n);
			break;
		}
		if (n > s->init - pos) n = s->init - pos;

		if (s->comp) {
			uint64_t cu = (uint64_t)v->cluster_size << s->cu_shift;
			uint64_t in = pos & (cu - 1);
			if (n > cu - in) n = cu - in;
			int rc = cu_load(v, s, pos >> (s->cu_shift + __builtin_ctz(v->cluster_size)));
			if (rc) return rc;
			memcpy(out + done, v->cu_out + in, (size_t)n);
		} else {
			uint64_t vcn = pos / v->cluster_size;
			uint32_t in  = (uint32_t)(pos % v->cluster_size);
			const ntfs_run_t *r = run_find(s, vcn);
			if (!r) return NTFS_EIO;
			uint64_t left = (r->len - (vcn - r->vcn)) * v->cluster_size - in;
			if (n > left) n = left;
			if (r->lcn == LCN_HOLE) {
				memset(out + done, 0, (size_t)n);
			} else {
				int rc = vol_read(v, (r->lcn + (vcn - r->vcn)) * v->cluster_size + in,
				                  out + done, n);
				if (rc) return rc;
			}
		}
		done += n;
	}
	return (long)len;
}

/* ── MFT ───────────────────────────────────────────────────────── */

static int rec_check(ntfs_vol_t *v, uint8_t *r, uint64_t no)
{
	int rc = fixup(r, v->rec_size, "FILE");
	if (rc) return rc;
	uint32_t first = le16(r + 20);
	/* NTFS 3.1 keeps the record's own number in the header */
	if (first >= 48 && le32(r + 44) != (uint32_t)no) return NTFS_EIO;
	return 0;
}

static int rec_read(ntfs_vol_t *v, uint64_t no, uint8_t *out)
{
	uint64_t slot = no % RC_SLOTS;
	uint8_t *c = v->rc + slot * v->rec_size;
	if (v->rc_tag[slot] == no + 1) {
		memcpy(out, c, v->rec_size);
		return 0;
	}
	if (no >= v->mft.size / v->rec_size) return NTFS_ENOENT;
	long got = stream_read(v, &v->mft, no * v->rec_size, out, v->rec_size);
	if (got != (long)v->rec_size) return got < 0 ? (int)got : NTFS_EIO;
	int rc = rec_check(v, out, no);
	if (rc) return rc;
	memcpy(c, out, v->rec_size);
	v->rc_tag[slot] = no + 1;
	return 0;
}

/* $MFT describes itself: record 0 sits where the boot sector says, and when the
 * file is so fragmented that its runs spill into extent records, those are reached
 * through the part of $MFT already known. */
static int mft_load(ntfs_vol_t *v)
{
	int rc = vol_read(v, v->mft_lcn * v->cluster_size, v->rec_a, v->rec_size);
	if (rc) return rc;
	rc = rec_check(v, v->rec_a, 0);
	if (rc) return rc;
	if (!(le16(v->rec_a + 22) & RF_IN_USE)) return NTFS_EIO;

	uint32_t pos, end;
	attr_t a;
	rc = rec_area(v->rec_a, v->rec_size, &pos, &end);
	if (rc) return rc;
	int found = 0;
	while ((rc = attr_next(v->rec_a, &pos, end, &a)) > 0) {
		if (a.type != AT_DATA || a.name_len || !a.nonres || a.lowest != v->mft.end_vcn) continue;
		rc = stream_take(v, &v->mft, &a, 0);
		if (rc) return rc;
		found = 1;
	}
	if (rc < 0 || !found) return rc < 0 ? rc : NTFS_EIO;
	if (v->mft.comp || (v->mft.flags & (AF_SPARSE | AF_ENCRYPTED))) return NTFS_EIO;

	uint8_t *list;
	uint32_t llen;
	rc = list_load(v, v->rec_a, &list, &llen);
	if (rc) return rc;
	if (!list) return 0;

	uint32_t lpos = 0;
	list_ent_t e;
	int lr;
	rc = 0;
	while (!rc && (lr = list_next(list, llen, &lpos, &e)) > 0) {
		if (e.type != AT_DATA || e.name_len || !e.lowest || !MREF_NO(e.mref)) continue;
		if (e.lowest != v->mft.end_vcn) { rc = NTFS_EIO; break; }
		rc = rec_read(v, MREF_NO(e.mref), v->rec_b);
		if (rc) break;
		rc = rec_area(v->rec_b, v->rec_size, &pos, &end);
		if (rc) break;
		int ar, hit = 0;
		while ((ar = attr_next(v->rec_b, &pos, end, &a)) > 0) {
			if (a.type != AT_DATA || a.name_len || a.id != e.id || !a.nonres) continue;
			rc = stream_take(v, &v->mft, &a, 0);
			hit = 1;
			break;
		}
		if (ar < 0) rc = ar;
		else if (!hit && !rc) rc = NTFS_EIO;
	}
	if (!rc && lr < 0) rc = lr;
	NTFS_FREE(list);
	return rc;
}

/* ── names ─────────────────────────────────────────────────────── */

/* UTF-16LE to UTF-8. An unpaired surrogate, which Windows allows in a name, keeps its
 * three-byte form so the mapping stays one to one. Returns bytes, without the NUL. */
static uint32_t name_utf8(const uint8_t *u, uint32_t n, char *out, uint32_t cap)
{
	uint32_t o = 0;
	for (uint32_t i = 0; i < n; i++) {
		uint32_t c = le16(u + 2 * i);
		if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) {
			uint32_t d = le16(u + 2 * (i + 1));
			if (d >= 0xDC00 && d < 0xE000) {
				c = 0x10000 + ((c - 0xD800) << 10) + (d - 0xDC00);
				i++;
			}
		}
		if (o + 4 >= cap) break;
		if (c < 0x80) {
			out[o++] = (char)c;
		} else if (c < 0x800) {
			out[o++] = (char)(0xC0 | c >> 6);
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else if (c < 0x10000) {
			out[o++] = (char)(0xE0 | c >> 12);
			out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		} else {
			out[o++] = (char)(0xF0 | c >> 18);
			out[o++] = (char)(0x80 | (c >> 12 & 0x3F));
			out[o++] = (char)(0x80 | (c >> 6 & 0x3F));
			out[o++] = (char)(0x80 | (c & 0x3F));
		}
	}
	out[o] = '\0';
	return o;
}

static uint64_t fnv64(const void *p, uint32_t n)
{
	const uint8_t *b = p;
	uint64_t h = 0xCBF29CE484222325ULL;
	while (n--) { h ^= *b++; h *= 0x100000001B3ULL; }
	return h;
}

/* A name of 255 UTF-16 units can need 765 bytes and the VFS holds 255. The long form
 * keeps a readable head and a hash of the whole name, stable from mount to mount. */
static uint32_t name_shorten(char *name, uint32_t len, uint64_t hash)
{
	static const char hex[] = "0123456789abcdef";
	uint32_t cut = NTFS_NAME_MAX - 17;
	while (cut && ((uint8_t)name[cut] & 0xC0) == 0x80) cut--;
	(void)len;
	name[cut++] = '~';
	for (int i = 60; i >= 0; i -= 4) name[cut++] = hex[(hash >> i) & 0xF];
	name[cut] = '\0';
	return cut;
}

/* ── directories ───────────────────────────────────────────────── */

static void dir_free(ntfs_dir_t *d)
{
	if (!d) return;
	NTFS_FREE(d->ents);
	NTFS_FREE(d->names);
	NTFS_FREE(d->hash);
	NTFS_FREE(d->shown);
	NTFS_FREE(d);
}

static int names_put(ntfs_dir_t *d, const char *s, uint32_t len, uint32_t *off)
{
	if (len + 1 > d->names_cap - d->names_len) {
		uint64_t cap = d->names_cap ? (uint64_t)d->names_cap * 2 : 4096;
		while (cap - d->names_len < len + 1ULL) cap *= 2;
		if (cap > MAX_NAMES_BYTES) return NTFS_EFBIG;
		char *n = NTFS_ALLOC((size_t)cap);
		if (!n) return NTFS_ENOMEM;
		if (d->names) memcpy(n, d->names, d->names_len);
		NTFS_FREE(d->names);
		d->names = n;
		d->names_cap = (uint32_t)cap;
	}
	*off = d->names_len;
	memcpy(d->names + d->names_len, s, len);
	d->names[d->names_len + len] = '\0';
	d->names_len += len + 1;
	return 0;
}

/* One $FILE_NAME index key. A hostile name ("..", or one holding a slash) would be a
 * path traversal once it left this driver, so it never does: the directory is marked
 * damaged and the entry stays out. */
static int dir_add(ntfs_vol_t *v, ntfs_dir_t *d, uint64_t mref, const uint8_t *key, uint32_t klen)
{
	if (klen < 66) { d->damaged = 1; return 0; }
	uint32_t nchars = key[64];
	uint8_t  space  = key[65];
	if (66 + 2 * nchars > klen || !nchars) { d->damaged = 1; return 0; }
	if (space == 2) return 0;                         /* the DOS alias of a long name */
	if (space > 3) { d->damaged = 1; return 0; }

	uint32_t len = name_utf8(key + 66, nchars, v->name, sizeof(v->name));
	if ((len == 1 && v->name[0] == '.') || (len == 2 && v->name[0] == '.' && v->name[1] == '.')) {
		if (d->mft != NTFS_MFT_ROOT) d->damaged = 1;  /* the root holds a "." of its own */
		return 0;
	}
	for (uint32_t i = 0; i < len; i++)
		if (v->name[i] == '/' || !v->name[i]) { d->damaged = 1; return 0; }
	if (d->mft == NTFS_MFT_ROOT && MREF_NO(mref) < 16) return 0;   /* the metafiles */

	if (d->n == d->cap) {
		uint32_t cap = d->cap ? d->cap * 2 : 32;
		if (cap > MAX_DIR_ENTRIES) return NTFS_EFBIG;
		dir_ent_t *ne = NTFS_ALLOC(cap * sizeof(*ne));
		if (!ne) return NTFS_ENOMEM;
		if (d->ents) memcpy(ne, d->ents, d->n * sizeof(*ne));
		NTFS_FREE(d->ents);
		d->ents = ne;
		d->cap = cap;
	}
	dir_ent_t *e = &d->ents[d->n];
	e->mref = mref;
	e->longname = ~0u;
	e->hidden = 0;
	uint32_t fa = le32(key + 56);
	e->is_dir = (fa & FA_DIR_INDEX) != 0;
	e->is_reparse = (fa & FA_REPARSE) != 0;
	int rc;
	if (len > NTFS_NAME_MAX) {
		rc = names_put(d, v->name, len, &e->longname);
		if (rc) return rc;
		len = name_shorten(v->name, len, fnv64(key + 66, 2 * nchars));
	}
	rc = names_put(d, v->name, len, &e->name);
	if (rc) return rc;
	d->n++;
	return 0;
}

/* Entries of one index node. off/end are inside b; end was checked by the caller. */
static int index_walk(ntfs_vol_t *v, ntfs_dir_t *d, const uint8_t *b, uint32_t off, uint32_t end)
{
	while (off + 16 <= end) {
		uint32_t elen = le16(b + off + 8);
		uint32_t klen = le16(b + off + 10);
		uint32_t fl   = le16(b + off + 12);
		if (elen < 16 || (elen & 7) || elen > end - off) { d->damaged = 1; return 0; }
		if (fl & 2) return 0;                         /* the last entry carries no key */
		uint32_t room = elen - 16 - ((fl & 1) ? 8 : 0);
		if (elen < 16 + ((fl & 1) ? 8u : 0u) || klen > room) { d->damaged = 1; return 0; }
		int rc = dir_add(v, d, le64(b + off), b + off + 16, klen);
		if (rc) return rc;
		off += elen;
	}
	d->damaged = 1;    /* ran out without the end marker */
	return 0;
}

static uint32_t name_hash(const char *s)
{
	uint32_t h = 2166136261u;
	while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
	return h;
}

/* Index by exact name. A second entry of one name would make the listing and the
 * lookup disagree: the first stays, the rest are dropped and the directory is marked. */
static int dir_seal(ntfs_dir_t *d)
{
	uint64_t want = 16;
	while (want < (uint64_t)d->n * 2 + 2) want <<= 1;
	d->hash = zalloc((size_t)want * sizeof(uint32_t));
	if (!d->hash) return NTFS_ENOMEM;
	d->shown = NTFS_ALLOC((d->n + 1) * sizeof(uint32_t));
	if (!d->shown) return NTFS_ENOMEM;
	d->hash_mask = (uint32_t)want - 1;
	for (uint32_t i = 0; i < d->n; i++) {
		const char *nm = d->names + d->ents[i].name;
		uint32_t h = name_hash(nm) & d->hash_mask;
		int dup = 0;
		while (d->hash[h]) {
			if (strcmp(d->names + d->ents[d->hash[h] - 1].name, nm) == 0) { dup = 1; break; }
			h = (h + 1) & d->hash_mask;
		}
		if (dup) {
			d->ents[i].hidden = 1;
			d->damaged = 1;
			continue;
		}
		d->hash[h] = i + 1;
		d->shown[d->nshown++] = i;
	}
	return 0;
}

static const uint8_t I30[8] = { '$', 0, 'I', 0, '3', 0, '0', 0 };

/* Every in-use node of the index, in block order. Going by the allocation bitmap
 * rather than down the tree needs no recursion and meets no cycles; the leaves and
 * branches between them hold every key. */
static int dir_scan(ntfs_vol_t *v, ntfs_dir_t *d)
{
	ntfs_stream_t root, ia, bm;
	uint8_t *block = NULL, *bits = NULL;
	memset(&root, 0, sizeof(root));
	memset(&ia, 0, sizeof(ia));
	memset(&bm, 0, sizeof(bm));

	int rc = stream_open(v, d->mft, AT_INDEX_ROOT, I30, 4, 0, &root);
	if (rc) return rc == NTFS_ENOENT ? NTFS_ENOTDIR : rc;
	if (!root.resident || root.res_len < 32) { rc = NTFS_EIO; goto out; }

	const uint8_t *r = root.res;
	uint32_t ib_size = le32(r + 8);
	if (ib_size < 512 || ib_size > (1u << 16) || !is_pow2(ib_size)) { rc = NTFS_EIO; goto out; }
	uint32_t eoff = le32(r + 16), eend = le32(r + 20);
	uint32_t hflags = r[28];
	if (eend > root.res_len - 16 || eoff < 16 || eoff >= eend) { rc = NTFS_EIO; goto out; }
	rc = index_walk(v, d, r + 16, eoff, eend);
	if (rc || !(hflags & 1)) goto out;

	rc = stream_open(v, d->mft, AT_INDEX_ALLOC, I30, 4, 0, &ia);
	if (rc == 0) rc = stream_open(v, d->mft, AT_BITMAP, I30, 4, 0, &bm);
	if (rc) { rc = rc == NTFS_ENOENT ? NTFS_EIO : rc; goto out; }
	if (ia.resident || ia.comp || bm.size > MAX_BITMAP_BYTES) { rc = NTFS_EIO; goto out; }

	uint32_t blen = (uint32_t)bm.size;
	bits = NTFS_ALLOC(blen ? blen : 1);
	block = NTFS_ALLOC(ib_size);
	if (!bits || !block) { rc = NTFS_ENOMEM; goto out; }
	long got = stream_read(v, &bm, 0, bits, blen);
	if (got != (long)blen) { rc = got < 0 ? (int)got : NTFS_EIO; goto out; }

	uint64_t nblocks = ia.size / ib_size;
	uint32_t bad = 0;
	for (uint64_t i = 0; i < nblocks && (i >> 3) < blen && bad < MAX_BAD_BLOCKS; i++) {
		if (!(bits[i >> 3] >> (i & 7) & 1)) continue;
		got = stream_read(v, &ia, i * ib_size, block, ib_size);
		if (got != (long)ib_size || fixup(block, ib_size, "INDX") != 0) {
			d->damaged = 1;
			bad++;
			continue;
		}
		uint32_t beoff = le32(block + 24), beend = le32(block + 28);
		if (beend > ib_size - 24 || beoff < 16 || beoff >= beend) {
			d->damaged = 1;
			bad++;
			continue;
		}
		rc = index_walk(v, d, block + 24, beoff, beend);
		if (rc) goto out;
	}
	rc = 0;
out:
	NTFS_FREE(block);
	NTFS_FREE(bits);
	stream_clear(&root);
	stream_clear(&ia);
	stream_clear(&bm);
	return rc;
}

int ntfs_dir_load(ntfs_vol_t *v, uint64_t mft, ntfs_dir_t **out)
{
	uint32_t victim = 0;
	for (uint32_t i = 0; i < DIR_SLOTS; i++) {
		ntfs_dir_t *c = v->dirs[i];
		if (c && c->mft == mft) {
			c->used = ++v->clock;
			*out = c;
			return 0;
		}
		if (!c) victim = i;
		else if (v->dirs[victim] && c->used < v->dirs[victim]->used) victim = i;
	}
	ntfs_dir_t *d = zalloc(sizeof(*d));
	if (!d) return NTFS_ENOMEM;
	d->mft = mft;
	int rc = dir_scan(v, d);
	if (!rc) rc = dir_seal(d);
	if (rc) { dir_free(d); return rc; }
	dir_free(v->dirs[victim]);
	v->dirs[victim] = d;
	d->used = ++v->clock;
	*out = d;
	return 0;
}

uint32_t ntfs_dir_count(const ntfs_dir_t *d) { return d->nshown; }
int      ntfs_dir_damaged(const ntfs_dir_t *d) { return d->damaged; }

static void ent_out(const ntfs_dir_t *d, const dir_ent_t *e, ntfs_dirent_t *out)
{
	out->name = d->names + e->name;
	out->longname = e->longname != ~0u ? d->names + e->longname : NULL;
	out->mref = e->mref;
	out->is_dir = e->is_dir;
	out->is_reparse = e->is_reparse;
}

int ntfs_dir_entry(const ntfs_dir_t *d, uint32_t idx, ntfs_dirent_t *out)
{
	if (idx >= d->nshown) return NTFS_ENOENT;
	ent_out(d, &d->ents[d->shown[idx]], out);
	return 0;
}

int ntfs_dir_find(const ntfs_dir_t *d, const char *name, ntfs_dirent_t *out)
{
	uint32_t h = name_hash(name) & d->hash_mask;
	while (d->hash[h]) {
		const dir_ent_t *e = &d->ents[d->hash[h] - 1];
		if (strcmp(d->names + e->name, name) == 0) {
			ent_out(d, e, out);
			return 0;
		}
		h = (h + 1) & d->hash_mask;
	}
	return NTFS_ENOENT;
}

/* ── file information ──────────────────────────────────────────── */

/* Reparse data becomes a single path component that names nothing, so following it
 * finds nothing and no walk can leave the mount. The bytes stay readable as text. */
static void target_text(ntfs_info_t *o, const char *kind, const uint8_t *u, uint32_t nchars, int utf8)
{
	char *t = o->target;
	uint32_t n = 0, cap = NTFS_TARGET_MAX;
	const char *prefix = "ntfs-reparse:";
	while (*prefix) t[n++] = *prefix++;
	while (*kind) t[n++] = *kind++;
	t[n++] = ':';
	if (utf8) {
		uint32_t m = nchars > cap - n - 1 ? cap - n - 1 : nchars;
		memcpy(t + n, u, m);
		n += m;
		t[n] = '\0';
	} else {
		n += name_utf8(u, nchars, t + n, cap - n);
	}
	for (uint32_t i = 0; i < n; i++)
		if (t[i] == '/' || !t[i]) t[i] = '%';
	t[n] = '\0';
}

static void reparse_parse(ntfs_info_t *o, const uint8_t *b, uint32_t n)
{
	uint32_t tag = n >= 8 ? le32(b) : 0;
	o->reparse_tag = tag;
	uint32_t data = n >= 8 ? le16(b + 4) : 0;
	if (n < 8 || data > n - 8) {
		o->unreadable = NTFS_UNREAD_REPARSE;
		return;
	}
	const uint8_t *p = b + 8;
	if (tag == TAG_SYMLINK || tag == TAG_JUNCTION) {
		uint32_t hdr = tag == TAG_SYMLINK ? 12 : 8;
		const char *kind = tag == TAG_SYMLINK ? "symlink" : "junction";
		if (data < hdr) { o->unreadable = NTFS_UNREAD_REPARSE; return; }
		uint32_t subo = le16(p), subl = le16(p + 2), pro = le16(p + 4), prl = le16(p + 6);
		uint32_t room = data - hdr;
		uint32_t off = prl ? pro : subo, len = prl ? prl : subl;
		if ((len & 1) || off > room || len > room - off) {
			o->unreadable = NTFS_UNREAD_REPARSE;
			return;
		}
		target_text(o, kind, p + hdr + off, len / 2, 0);
		o->is_link = 1;
		return;
	}
	if (tag == TAG_LX_SYMLINK) {
		if (data < 4) { o->unreadable = NTFS_UNREAD_REPARSE; return; }
		target_text(o, "lxsymlink", p + 4, data - 4, 1);
		o->is_link = 1;
		return;
	}
	if (o->is_dir) return;      /* a directory's entries are still real */
	if (tag == TAG_WOF) {
		o->unreadable = NTFS_UNREAD_WOF;
	} else if (tag == TAG_DEDUP) {
		o->unreadable = NTFS_UNREAD_DEDUP;
	} else if ((tag & TAG_CLOUD_MASK) == TAG_CLOUD) {
		/* a hydrated placeholder keeps the tag and has its data */
		if (o->file_attrs & (FA_OFFLINE | FA_RECALL_ON_OPEN | FA_RECALL_ON_ACCESS))
			o->unreadable = NTFS_UNREAD_CLOUD;
	} else {
		o->unreadable = NTFS_UNREAD_REPARSE;
	}
}

static void list_count_streams(const uint8_t *list, uint32_t llen, ntfs_info_t *o)
{
	uint32_t pos = 0;
	list_ent_t e;
	while (list_next(list, llen, &pos, &e) > 0)
		if (e.type == AT_DATA && e.name_len && !e.lowest) o->named_streams++;
}

int ntfs_get_info(ntfs_vol_t *v, uint64_t mref, ntfs_info_t *o)
{
	uint64_t no = MREF_NO(mref);
	uint16_t seq = MREF_SEQ(mref);
	int rc = rec_read(v, no, v->rec_a);
	if (rc) return rc;
	const uint8_t *r = v->rec_a;
	uint32_t rflags = le16(r + 22);
	if (!(rflags & RF_IN_USE)) return NTFS_ENOENT;
	if (MREF_NO(le64(r + 32))) return NTFS_ENOENT;        /* an extent, not a file */
	uint16_t rseq = le16(r + 16);
	if (seq && rseq && seq != rseq) return NTFS_ENOENT;   /* the name outlived the file */

	memset(o, 0, sizeof(*o) - sizeof(o->target));
	o->target[0] = '\0';
	o->mft = no;
	o->seq = rseq;
	o->links = le16(r + 18);
	o->is_dir = (rflags & RF_DIR) != 0;

	uint32_t pos, end;
	attr_t a;
	rc = rec_area(r, v->rec_size, &pos, &end);
	if (rc) return rc;
	int have_si = 0, have_rp = 0;
	uint32_t base_streams = 0;
	while ((rc = attr_next(r, &pos, end, &a)) > 0) {
		if (a.type == AT_STANDARD_INFO && !a.nonres && a.val_len >= 36) {
			o->crtime = le64(a.val);
			o->mtime  = le64(a.val + 8);
			o->ctime  = le64(a.val + 16);
			o->atime  = le64(a.val + 24);
			o->file_attrs = le32(a.val + 32);
			have_si = 1;
		} else if (a.type == AT_REPARSE) {
			have_rp = 1;
		} else if (a.type == AT_DATA && a.name_len && !a.lowest) {
			base_streams++;
		}
	}
	if (rc < 0) return rc;
	if (!have_si) return NTFS_EIO;
	o->named_streams = base_streams;

	uint8_t *list;
	uint32_t llen;
	rc = list_load(v, r, &list, &llen);
	if (rc) return rc;
	if (list) {
		uint32_t lpos = 0;
		list_ent_t e;
		o->named_streams = 0;       /* the list names the ones in this record too */
		list_count_streams(list, llen, o);
		while (list_next(list, llen, &lpos, &e) > 0)
			if (e.type == AT_REPARSE) have_rp = 1;
		NTFS_FREE(list);
	}

	if (!o->is_dir) {
		ntfs_stream_t s;
		memset(&s, 0, sizeof(s));
		rc = stream_open(v, no, AT_DATA, NULL, 0, 1, &s);
		o->size = s.size;
		uint32_t fl = s.flags;
		stream_clear(&s);
		if (rc && rc != NTFS_ENOENT) return rc;
		if (fl & AF_ENCRYPTED) o->unreadable = NTFS_UNREAD_ENCRYPTED;
	}
	if ((o->file_attrs & FA_ENCRYPTED) && !o->is_dir) o->unreadable = NTFS_UNREAD_ENCRYPTED;

	if (have_rp || (o->file_attrs & FA_REPARSE)) {
		ntfs_stream_t s;
		memset(&s, 0, sizeof(s));
		rc = stream_open(v, no, AT_REPARSE, NULL, 0, 0, &s);
		if (rc == 0) {
			uint32_t n = s.size > MAX_REPARSE_BYTES ? MAX_REPARSE_BYTES : (uint32_t)s.size;
			long got = stream_read(v, &s, 0, v->scratch, n);
			if (got == (long)n) {
				uint8_t was = o->unreadable;
				reparse_parse(o, v->scratch, n);
				if (was) o->unreadable = was;
			} else {
				o->unreadable = NTFS_UNREAD_REPARSE;
			}
		} else if (rc == NTFS_ENOENT) {
			if (!o->is_dir) o->unreadable = NTFS_UNREAD_REPARSE;
		} else {
			stream_clear(&s);
			return rc;
		}
		stream_clear(&s);
	}
	return 0;
}

int ntfs_parent(ntfs_vol_t *v, uint64_t mft, uint64_t *out)
{
	int rc = rec_read(v, mft, v->rec_a);
	if (rc) return rc;
	uint32_t pos, end;
	attr_t a;
	rc = rec_area(v->rec_a, v->rec_size, &pos, &end);
	if (rc) return rc;
	while ((rc = attr_next(v->rec_a, &pos, end, &a)) > 0) {
		if (a.type != AT_FILE_NAME || a.nonres || a.val_len < 8) continue;
		*out = MREF_NO(le64(a.val));
		return 0;
	}
	return rc < 0 ? rc : NTFS_ENOENT;
}

/* ── file data ─────────────────────────────────────────────────── */

static int stream_get(ntfs_vol_t *v, uint64_t mft, ntfs_stream_t **out)
{
	uint32_t victim = 0;
	for (uint32_t i = 0; i < STREAM_SLOTS; i++) {
		ntfs_stream_t *c = v->streams[i];
		if (c && c->mft == mft) {
			c->used = ++v->clock;
			*out = c;
			return 0;
		}
		if (!c) victim = i;
		else if (v->streams[victim] && c->used < v->streams[victim]->used) victim = i;
	}
	ntfs_info_t *info = NTFS_ALLOC(sizeof(*info));
	if (!info) return NTFS_ENOMEM;
	int rc = ntfs_get_info(v, mft, info);
	uint8_t unreadable = info->unreadable;
	NTFS_FREE(info);
	if (rc) return rc;

	ntfs_stream_t *s = zalloc(sizeof(*s));
	if (!s) return NTFS_ENOMEM;
	rc = stream_open(v, mft, AT_DATA, NULL, 0, 0, s);
	if (rc) { stream_clear(s); NTFS_FREE(s); return rc; }
	s->mft = mft;
	s->unreadable = unreadable;
	s->id = ++v->next_id;
	if (v->streams[victim]) {
		stream_clear(v->streams[victim]);
		NTFS_FREE(v->streams[victim]);
	}
	v->streams[victim] = s;
	s->used = ++v->clock;
	*out = s;
	return 0;
}

long ntfs_file_read(ntfs_vol_t *v, uint64_t mft, uint64_t off, void *buf, uint32_t len)
{
	ntfs_stream_t *s;
	int rc = stream_get(v, mft, &s);
	if (rc) return rc;
	if (s->unreadable) return NTFS_EOPNOTSUPP;
	return stream_read(v, s, off, buf, len);
}

/* ── volume ────────────────────────────────────────────────────── */

int ntfs_is_bitlocker(const void *sector512)
{
	return memcmp((const uint8_t *)sector512 + 3, "-FVE-FS-", 8) == 0;
}

static int boot_parse(ntfs_vol_t *v, const uint8_t *b)
{
	if (ntfs_is_bitlocker(b)) return NTFS_EBITLOCKER;
	if (memcmp(b + 3, "NTFS    ", 8) != 0 || b[510] != 0x55 || b[511] != 0xAA)
		return NTFS_EINVAL;

	uint32_t bps = le16(b + 11);
	if (bps < 512 || bps > 4096 || !is_pow2(bps)) return NTFS_EINVAL;
	uint32_t spc = b[13];
	uint64_t cs;
	if (spc <= 0x80) {
		if (!spc || !is_pow2(spc)) return NTFS_EINVAL;
		cs = (uint64_t)bps * spc;
	} else {
		uint32_t sh = 256 - spc;           /* a negative power of two: 2 MB clusters */
		if (sh > 12) return NTFS_EINVAL;
		cs = (uint64_t)bps << sh;
	}
	if (cs > (1u << 21)) return NTFS_EINVAL;

	int8_t crec = (int8_t)b[64];
	uint64_t rec;
	if (crec > 0) rec = cs * (uint64_t)crec;
	else {
		uint32_t sh = (uint32_t)(-(int)crec);
		if (sh < 8 || sh > 16) return NTFS_EINVAL;
		rec = 1ULL << sh;
	}
	if (rec < 512 || rec > (1u << 16) || !is_pow2(rec)) return NTFS_EINVAL;

	uint64_t tsec = le64(b + 40);
	uint64_t secs512 = bps / 512;
	if (!tsec || tsec > v->dev_sectors / secs512) return NTFS_EINVAL;

	v->sector_size = bps;
	v->cluster_size = (uint32_t)cs;
	v->rec_size = (uint32_t)rec;
	v->total_clusters = tsec * bps / cs;
	v->mft_lcn = le64(b + 48);
	v->max_vcn = MAX_POS / cs;
	if (!v->total_clusters || v->mft_lcn >= v->total_clusters) return NTFS_EINVAL;
	return 0;
}

static int vol_details(ntfs_vol_t *v)
{
	ntfs_stream_t s;
	uint8_t val[16];
	memset(&s, 0, sizeof(s));
	int rc = stream_open(v, 3, AT_VOLUME_INFO, NULL, 0, 0, &s);
	if (rc) { stream_clear(&s); return rc; }
	long got = stream_read(v, &s, 0, val, 12);
	stream_clear(&s);
	if (got != 12) return got < 0 ? (int)got : NTFS_EIO;
	v->info.major = val[8];
	v->info.minor = val[9];
	v->info.dirty = le16(val + 10) & 1;
	if (v->info.major != 3) return NTFS_EINVAL;

	memset(&s, 0, sizeof(s));
	rc = stream_open(v, 3, AT_VOLUME_NAME, NULL, 0, 0, &s);
	if (rc == 0 && s.resident && s.res_len) {
		uint32_t n = s.res_len / 2;
		if (n > 80) n = 80;
		name_utf8(s.res, n, v->info.label, sizeof(v->info.label));
	}
	stream_clear(&s);
	v->info.cluster_size = v->cluster_size;
	v->info.record_size = v->rec_size;
	v->info.total_clusters = v->total_clusters;
	return 0;
}

/* hiberfil.sys starting with a live signature means Windows did not shut down: what
 * is on disk may be older than what it thinks it wrote. A warning, not a refusal. */
static void hibernate_check(ntfs_vol_t *v)
{
	ntfs_dir_t *d;
	ntfs_dirent_t e;
	uint8_t sig[4];
	if (ntfs_dir_load(v, NTFS_MFT_ROOT, &d) != 0) return;
	if (ntfs_dir_find(d, "hiberfil.sys", &e) != 0) return;
	if (ntfs_file_read(v, MREF_NO(e.mref), 0, sig, 4) != 4) return;
	for (int i = 0; i < 4; i++) sig[i] = (uint8_t)(sig[i] & ~0x20);   /* either case */
	/* after a resume the header reads "wake": that image is spent */
	if (memcmp(sig, "HIBR", 4) == 0 || memcmp(sig, "RSTR", 4) == 0)
		v->info.hibernated = 1;
}

int ntfs_open(ntfs_read_fn rd, void *dev, uint64_t dev_sectors, ntfs_vol_t **out)
{
	ntfs_vol_t *v = zalloc(sizeof(*v));
	if (!v) return NTFS_ENOMEM;
	v->rd = rd;
	v->dev = dev;
	v->dev_sectors = dev_sectors;
	if (dev_sectors >> 54) { NTFS_FREE(v); return NTFS_EINVAL; }

	int rc = v->rd(dev, 0, v->sec, 1) ? NTFS_EIO : 0;
	if (!rc) rc = boot_parse(v, v->sec);
	if (rc) { NTFS_FREE(v); return rc; }

	v->rec_a = NTFS_ALLOC(v->rec_size);
	v->rec_b = NTFS_ALLOC(v->rec_size);
	v->rc = NTFS_ALLOC((size_t)RC_SLOTS * v->rec_size);
	v->scratch = NTFS_ALLOC(MAX_REPARSE_BYTES);
	if (!v->rec_a || !v->rec_b || !v->rc || !v->scratch) { ntfs_close(v); return NTFS_ENOMEM; }

	rc = mft_load(v);
	if (!rc) rc = vol_details(v);
	if (!rc) {
		ntfs_info_t *root = NTFS_ALLOC(sizeof(*root));
		if (!root) rc = NTFS_ENOMEM;
		else {
			rc = ntfs_get_info(v, NTFS_MFT_ROOT, root);
			if (!rc && !root->is_dir) rc = NTFS_EINVAL;
			NTFS_FREE(root);
		}
	}
	if (rc) { ntfs_close(v); return rc; }
	hibernate_check(v);
	*out = v;
	return 0;
}

void ntfs_volinfo(ntfs_vol_t *v, ntfs_volinfo_t *out) { *out = v->info; }

void ntfs_close(ntfs_vol_t *v)
{
	if (!v) return;
	for (uint32_t i = 0; i < DIR_SLOTS; i++) dir_free(v->dirs[i]);
	for (uint32_t i = 0; i < STREAM_SLOTS; i++)
		if (v->streams[i]) { stream_clear(v->streams[i]); NTFS_FREE(v->streams[i]); }
	stream_clear(&v->mft);
	NTFS_FREE(v->rec_a);
	NTFS_FREE(v->rec_b);
	NTFS_FREE(v->rc);
	NTFS_FREE(v->cu_in);
	NTFS_FREE(v->cu_out);
	NTFS_FREE(v->scratch);
	NTFS_FREE(v);
}
