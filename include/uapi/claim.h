#ifndef UAPI_CLAIM_H
#define UAPI_CLAIM_H

/* Ranix claims (kernel/claim.c): who is using which part of the machine, for
 * what, until when. Agents and people share one laptop; a claim says "Agent C:
 * GPU test, until 18:30" where both can see it, and the kernel and fjord-sys
 * honour the ones that guard destructive acts (reboot).
 * stop_pid must be a process the taker may signal (else EPERM).
 *
 * /dev/claim (0660 root:claim; group claim may take, read and wait):
 *   read     JSON snapshot {"gen":N,"now":EPOCH,"claims":[...]}; lseek(0) to re-read
 *   poll     readable when the table changed since this fd's last read
 *   ioctl    CLAIM_IOC_* below
 *
 * Resources are free-form names ([a-z0-9._:-], up to 15 bytes). Well-known:
 *   display input gpu reboot network wifi disk audio usb camera
 * Modes: exclusive (default) conflicts with everything; CLAIM_F_SHARED claims
 * coexist with each other.
 * Lifetime: by default a claim belongs to the open file: it ends on close (or
 * when its holder dies). CLAIM_F_DETACH keeps it until release or expiry.
 * Every claim may carry an expiry (secs); detached claims must.
 */

#include <uapi/types.h>

#define CLAIM_RES_MAX     16
#define CLAIM_HOLDER_MAX  32
#define CLAIM_PURPOSE_MAX 80

#define CLAIM_F_SHARED  0x1
#define CLAIM_F_DETACH  0x2
#define CLAIM_F_WAIT    0x4     /* TAKE: sleep until free (killable) */

struct claim_req {
	char     res[CLAIM_RES_MAX];
	char     holder[CLAIM_HOLDER_MAX];     /* "Agent C"; empty = the caller's comm */
	char     purpose[CLAIM_PURPOSE_MAX];
	uint32_t secs;                         /* 0 = no expiry (attached claims only) */
	uint32_t flags;
	int32_t  stop_pid;                     /* process group leader a "stop" ends; 0 = the caller */
	uint32_t pad;
	uint64_t id;                           /* out: TAKE; in: RELEASE, EXTEND, STOP */
	/* out on EBUSY: the claim in the way */
	char     busy_holder[CLAIM_HOLDER_MAX];
	char     busy_purpose[CLAIM_PURPOSE_MAX];
	int64_t  busy_until;                   /* epoch seconds, 0 = none */
	uint64_t busy_id;
};

#define CLAIM_IOC_TAKE     0xC1A0   /* -EBUSY (busy_* filled), -EINVAL, -ENOSPC, -EPERM/-ESRCH (stop_pid) */
#define CLAIM_IOC_RELEASE  0xC1A1   /* id; owner uid or root */
#define CLAIM_IOC_EXTEND   0xC1A2   /* id, secs from now */
#define CLAIM_IOC_STOP     0xC1A3   /* id; ends the claim and stops its holder: SIGTERM to
                                     * stop_pid's group, SIGKILL 5 s later. Owner uid or root */
#define CLAIM_IOC_CHECK    0xC1A4   /* res, holder: 0 if holder may act on res now, else
                                     * -EBUSY with busy_* naming who holds it */

#endif
