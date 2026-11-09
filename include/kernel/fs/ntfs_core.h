/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#ifndef NTFS_CORE_H
#define NTFS_CORE_H

/*
 * NTFS on-disk parser, read-only by construction: it is handed one read callback
 * and has no way to reach a device otherwise. No VFS types, so the same source
 * builds in the host fuzz harness (tests/ntfs/) with NTFS_HOST defined.
 */

#ifdef NTFS_HOST
#include <stddef.h>
#include <stdint.h>
#else
#include <uapi/types.h>
#endif

#define NTFS_MFT_ROOT       5u
#define NTFS_NAME_MAX       255u   /* bytes a VFS name may hold */
#define NTFS_LONGNAME_MAX   1024u  /* 255 UTF-16 units as UTF-8, surrogates included */
#define NTFS_TARGET_MAX     1024u

/* Results are 0 or a negative errno; these are the ones callers tell apart. */
#define NTFS_ENOENT         (-2)
#define NTFS_EIO            (-5)
#define NTFS_ENOMEM         (-12)
#define NTFS_ENOTDIR        (-20)
#define NTFS_EINVAL         (-22)
#define NTFS_EFBIG          (-27)
#define NTFS_EOPNOTSUPP     (-95)
#define NTFS_EBITLOCKER     (-1000)  /* mount: BitLocker volume, nothing readable */

/* Why a file's data cannot be given out. Reading such a file fails: handing back
 * the unnamed stream would give zeros or ciphertext with no sign of it. */
#define NTFS_UNREAD_NONE       0
#define NTFS_UNREAD_ENCRYPTED  1   /* EFS */
#define NTFS_UNREAD_WOF        2   /* system-compressed, data is in another stream */
#define NTFS_UNREAD_CLOUD      3   /* placeholder, data is not on this disk */
#define NTFS_UNREAD_DEDUP      4   /* data deduplication chunk store */
#define NTFS_UNREAD_REPARSE    5   /* a reparse tag this driver does not know */

typedef int (*ntfs_read_fn)(void *dev, uint64_t lba512, void *buf, uint32_t sectors);

typedef struct {
	uint64_t mft;
	uint16_t seq;
	uint16_t links;
	uint8_t  is_dir;
	uint8_t  is_link;        /* symlink or junction, target holds the safe text */
	uint8_t  unreadable;     /* NTFS_UNREAD_* */
	uint32_t file_attrs;     /* FILE_ATTRIBUTE_* */
	uint32_t reparse_tag;    /* 0 if none */
	uint32_t named_streams;  /* alternate data streams, not exposed as files */
	uint64_t size;
	uint64_t crtime, mtime, ctime, atime;   /* FILETIME */
	char     target[NTFS_TARGET_MAX];
} ntfs_info_t;

typedef struct {
	uint8_t  major, minor;
	uint8_t  dirty;          /* $Volume says so: chkdsk or a log replay was due */
	uint8_t  hibernated;     /* hiberfil.sys holds a live image: disk may be stale */
	uint32_t cluster_size;
	uint32_t record_size;
	uint64_t total_clusters;
	char     label[256];
} ntfs_volinfo_t;

typedef struct ntfs_vol ntfs_vol_t;
typedef struct ntfs_dir ntfs_dir_t;

typedef struct {
	const char *name;        /* NUL-terminated, at most NTFS_NAME_MAX bytes */
	const char *longname;    /* the real name when name was shortened, else NULL */
	uint64_t    mref;        /* record number and sequence, as stored */
	uint8_t     is_dir;
	uint8_t     is_reparse;
} ntfs_dirent_t;

/* lba512 and sectors count 512-byte units whatever the device sector is. */
int  ntfs_open(ntfs_read_fn rd, void *dev, uint64_t dev_sectors, ntfs_vol_t **out);
void ntfs_close(ntfs_vol_t *v);
void ntfs_volinfo(ntfs_vol_t *v, ntfs_volinfo_t *out);
int  ntfs_is_bitlocker(const void *sector512);

/* out is big: callers keep it off the stack. */
int  ntfs_get_info(ntfs_vol_t *v, uint64_t mref, ntfs_info_t *out);

/* A directory table stays valid until the next ntfs_dir_load on this volume. */
int  ntfs_dir_load(ntfs_vol_t *v, uint64_t mft, ntfs_dir_t **out);
uint32_t ntfs_dir_count(const ntfs_dir_t *d);
int  ntfs_dir_damaged(const ntfs_dir_t *d);
int  ntfs_dir_entry(const ntfs_dir_t *d, uint32_t idx, ntfs_dirent_t *out);
int  ntfs_dir_find(const ntfs_dir_t *d, const char *name, ntfs_dirent_t *out);

/* The directory holding record mft, from its $FILE_NAME. */
int  ntfs_parent(ntfs_vol_t *v, uint64_t mft, uint64_t *out);

/* Bytes read, 0 at end of file, or a negative errno. */
long ntfs_file_read(ntfs_vol_t *v, uint64_t mft, uint64_t off, void *buf, uint32_t len);

/* The LZNT1 decoder alone, for tests. Returns bytes produced or a negative errno. */
long ntfs_lznt1(const uint8_t *in, uint32_t in_len, uint8_t *out, uint32_t cap);

#endif
