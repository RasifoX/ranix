#include <kernel/cmdline.h>
#include <kernel/lib/string.h>
#include <kernel/config.h>

static char _cmdline[CMDLINE_MAX];

void cmdline_init(const char *str)
{
    if (!str) { _cmdline[0] = '\0'; return; }
    size_t n = strlen(str);
    if (n >= CMDLINE_MAX) n = CMDLINE_MAX - 1;
    memcpy(_cmdline, str, n);
    _cmdline[n] = '\0';
}

const char *cmdline_str(void) { return _cmdline; }

/* Walk space-separated tokens in cmdline, match "key" or "key=value". */
const char *cmdline_get(const char *key, char *buf, size_t bufsz)
{
    if (!key || !bufsz) return NULL;
    size_t klen = strlen(key);
    const char *p = _cmdline;

    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;

        const char *tok = p;
        while (*p && *p != ' ') p++;
        size_t tlen = (size_t)(p - tok);

        if (tlen >= klen && memcmp(tok, key, klen) == 0) {
            if (tok[klen] == '=') {
                /* key=value */
                const char *val = tok + klen + 1;
                size_t vlen = tlen - klen - 1;
                if (vlen >= bufsz) vlen = bufsz - 1;
                memcpy(buf, val, vlen);
                buf[vlen] = '\0';
                return buf;
            } else if (tok[klen] == '\0' || tok[klen] == ' ') {
                /* bare flag */
                buf[0] = '\0';
                return buf;
            }
        }
    }
    return NULL;
}

int cmdline_has(const char *flag)
{
    char tmp[2];
    return cmdline_get(flag, tmp, sizeof(tmp)) != NULL;
}
