#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <sys/syscall.h>

/* getresuid/getresgid */
static int getresuid(uid_t *r, uid_t *e, uid_t *s)
{
#ifdef __x86_64__
    return (int)syscall(118, r, e, s);
#else
    return (int)syscall(148, r, e, s);
#endif
}
static int getresgid(gid_t *r, gid_t *e, gid_t *s)
{
#ifdef __x86_64__
    return (int)syscall(120, r, e, s);
#else
    return (int)syscall(150, r, e, s);
#endif
}

/* capget v3 */
typedef struct { unsigned version; int pid; } cap_hdr;
typedef struct { unsigned effective, permitted, inheritable; } cap_data;
static int capget(cap_hdr *h, cap_data *d)
{
    return (int)syscall(
#ifdef __x86_64__
        125,
#else
        90,
#endif
        h, d);
}

/* getrandom */
static long getrandom_sc(void *buf, size_t len, unsigned flags)
{
#ifdef __x86_64__
    return syscall(318, buf, len, flags);
#else
    return syscall(278, buf, len, flags);
#endif
}

static int fail = 0;
static void check(const char *name, int ok)
{
    if (ok) printf("  OK: %s\n", name);
    else    { printf("  FAIL: %s\n", name); fail = 1; }
}

int main(void)
{
    /* ── basic uid/gid ─────────────────────────────────────── */
    check("getuid==0",   getuid()  == 0);
    check("geteuid==0",  geteuid() == 0);
    check("getgid==0",   getgid()  == 0);
    check("getegid==0",  getegid() == 0);

    /* ── resuid/resgid triplet ─────────────────────────────── */
    uid_t ru, eu, su;
    gid_t rg, eg, sg;
    check("getresuid-ok",   getresuid(&ru, &eu, &su) == 0);
    check("resuid-r==0",    ru == 0);
    check("resuid-e==0",    eu == 0);
    check("resuid-s==0",    su == 0);
    check("getresgid-ok",   getresgid(&rg, &eg, &sg) == 0);
    check("resgid-r==0",    rg == 0);
    check("resgid-e==0",    eg == 0);
    check("resgid-s==0",    sg == 0);

    /* ── capabilities (root → full set) ───────────────────── */
    cap_hdr hdr = { 0x20080522, 0 };
    cap_data dat[2] = {{0}};
    int cret = capget(&hdr, dat);
    check("capget-ok",      cret == 0);
    /* root: all caps set in both halves */
    check("capeff-low-full",  dat[0].effective   == 0xffffffff);
    check("capprm-low-full",  dat[0].permitted   == 0xffffffff);
    check("capinh-low-full",  dat[0].inheritable == 0xffffffff);
    check("capeff-hi-full",   dat[1].effective   == 0xffffffff);

    /* ── getrandom quality ─────────────────────────────────── */
    unsigned char a[16], b[16];
    long ra = getrandom_sc(a, sizeof(a), 0);
    long rb = getrandom_sc(b, sizeof(b), 0);
    check("getrandom-len-a",  ra == 16);
    check("getrandom-len-b",  rb == 16);
    /* chance of collision: 2^-128 */
    check("getrandom-differs", memcmp(a, b, 16) != 0);
    /* not all zeros */
    int nonzero_a = 0, nonzero_b = 0;
    for (int i = 0; i < 16; i++) {
        if (a[i]) nonzero_a = 1;
        if (b[i]) nonzero_b = 1;
    }
    check("getrandom-nonzero-a", nonzero_a);
    check("getrandom-nonzero-b", nonzero_b);

    printf(fail ? "FAIL\n" : "OK\n");
    return fail;
}
