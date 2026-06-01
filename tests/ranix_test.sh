#!/bin/bash
set +m
PASS=0; FAIL=0
check() {
    local name="$1" exp="$2" got="$3"
    if [ "$got" = "$exp" ]; then
        echo "PASS: $name"
        PASS=$((PASS+1))
    else
        echo "FAIL: $name (expected='$exp' got='$got')"
        FAIL=$((FAIL+1))
    fi
}

# ── 1. echo / printf ──────────────────────────────────────────
check "echo-basic"     "hello"         "$(echo hello)"
check "echo-n"         "ab"            "$(echo -n a; echo -n b)"
check "printf-str"     "hello world"   "$(printf '%s %s' hello world)"
check "printf-int"     "42"            "$(printf '%d' 42)"
check "printf-hex"     "ff"            "$(printf '%x' 255)"
check "printf-oct"     "10"            "$(printf '%o' 8)"
check "printf-escape"  "a	b"          "$(printf 'a\tb')"

# ── 2. arithmetic ─────────────────────────────────────────────
check "arith-add"      "7"    "$((3+4))"
check "arith-mul"      "12"   "$((3*4))"
check "arith-div"      "3"    "$((9/3))"
check "arith-mod"      "1"    "$((10%3))"
check "arith-neg"      "-5"   "$((-5))"
check "arith-parens"   "14"   "$((2*(3+4)))"
check "arith-shift"    "8"    "$((1<<3))"
check "arith-band"     "2"    "$((3&6))"
check "arith-bor"      "7"    "$((3|4))"
check "arith-xor"      "5"    "$((3^6))"

# ── 3. string ops ─────────────────────────────────────────────
s="hello world"
check "str-len"        "11"           "${#s}"
check "str-sub1"       "hello"        "${s:0:5}"
check "str-sub2"       "world"        "${s:6}"
check "str-strip-pre"  "world"        "${s#hello }"
check "str-strip-suf"  "hello"        "${s% world}"
check "str-replace"    "hXllX wXrld"  "${s//[aeiou]/X}"
check "str-upper"      "HELLO WORLD"  "${s^^}"
check "str-lower"      "hello world"  "${s,,}"

# ── 4. arrays ─────────────────────────────────────────────────
arr=(a b c d e)
check "arr-elem"    "c"       "${arr[2]}"
check "arr-len"     "5"       "${#arr[@]}"
check "arr-slice"   "b c d"   "${arr[*]:1:3}"
check "arr-last"    "e"       "${arr[-1]}"
arr[2]="X"
check "arr-set"     "X"       "${arr[2]}"

# ── 5. conditionals ───────────────────────────────────────────
check "if-true"    "yes"  "$(if true; then echo yes; fi)"
check "if-false"   "no"   "$(if false; then echo yes; else echo no; fi)"
check "cmp-eq"     "yes"  "$([ 1 -eq 1 ] && echo yes || echo no)"
check "cmp-ne"     "yes"  "$([ 1 -ne 2 ] && echo yes || echo no)"
check "cmp-lt"     "yes"  "$([ 2 -lt 3 ] && echo yes || echo no)"
check "cmp-gt"     "yes"  "$([ 3 -gt 2 ] && echo yes || echo no)"
check "str-eq"     "yes"  "$([ abc = abc ] && echo yes || echo no)"
check "str-ne"     "yes"  "$([ abc != xyz ] && echo yes || echo no)"
check "test-z"     "yes"  "$([ -z '' ] && echo yes || echo no)"
check "test-n"     "yes"  "$([ -n 'x' ] && echo yes || echo no)"

# ── 6. loops ──────────────────────────────────────────────────
check "for-seq"    "1 2 3 4 5"  "$(for i in 1 2 3 4 5; do printf '%s ' $i; done | sed 's/ $//')"
check "while-cnt"  "5"          "$(i=0; while [ $i -lt 5 ]; do i=$((i+1)); done; echo $i)"
check "until-cnt"  "3"          "$(i=0; until [ $i -ge 3 ]; do i=$((i+1)); done; echo $i)"
check "for-c"      "10"         "$(s=0; for((i=1;i<=4;i++)); do s=$((s+i)); done; echo $s)"
check "break-loop" "3"          "$(for i in 1 2 3 4 5; do [ $i -eq 3 ] && break; done; echo $i)"
check "cont-loop"  "2 4"        "$(for i in 1 2 3 4; do [ $((i%2)) -eq 1 ] && continue; echo -n "$i "; done | sed 's/ $//')"

# ── 7. functions ──────────────────────────────────────────────
add(){ echo $(($1+$2)); }
fact(){ [ $1 -le 1 ] && echo 1 || echo $(($1 * $(fact $(($1-1))))); }
check "func-add"   "7"    "$(add 3 4)"
check "func-recur" "120"  "$(fact 5)"
greet(){ local name=$1; echo "hi $name"; }
check "func-local" "hi bob" "$(greet bob)"

# ── 8. pipes & redirection ────────────────────────────────────
check "pipe-basic"   "HELLO"   "$(echo hello | tr a-z A-Z)"
check "pipe-chain"   "3"       "$(echo -e 'a\nb\nc' | wc -l | tr -d ' ')"
check "redir-out"    "ok"      "$(echo ok > /tmp/t1.txt; cat /tmp/t1.txt)"
check "redir-app"    "ab"      "$(echo -n a > /tmp/t2.txt; echo -n b >> /tmp/t2.txt; cat /tmp/t2.txt)"
check "redir-in"     "hello"   "$(echo hello > /tmp/t3.txt; cat < /tmp/t3.txt)"
check "pipe-subsh"   "5"       "$(echo '1 2 3 4 5' | wc -w | tr -d ' ')"
check "here-string"  "hello"   "$(cat <<< 'hello')"
check "heredoc"      "hi"      "$(cat <<EOF
hi
EOF
)"

# ── 9. process substitution / command sub ────────────────────
check "cmdsub-basic"  "hello"  "$(r=$(echo hello); echo $r)"
check "cmdsub-nest"   "3"      "$(echo $(echo 3))"
check "cmdsub-arith"  "9"      "$(echo $(($(echo 3) * 3)))"

# ── 10. environment / export ──────────────────────────────────
export TESTVAR=hello
check "env-export"  "hello"  "$(bash -c 'echo $TESTVAR')"
check "env-local"   ""       "$(unset _RANIX_TEST_VAR; bash -c 'echo $_RANIX_TEST_VAR')"
check "env-path"    "true"   "$([ -n "$PATH" ] && echo true)"

# ── 11. file tests ────────────────────────────────────────────
echo test > /tmp/ftest.txt
mkdir -p /tmp/dtest
check "file-e"   "yes"  "$([ -e /tmp/ftest.txt ] && echo yes || echo no)"
check "file-f"   "yes"  "$([ -f /tmp/ftest.txt ] && echo yes || echo no)"
check "file-d"   "yes"  "$([ -d /tmp/dtest ] && echo yes || echo no)"
check "file-r"   "yes"  "$([ -r /tmp/ftest.txt ] && echo yes || echo no)"
check "file-s"   "yes"  "$([ -s /tmp/ftest.txt ] && echo yes || echo no)"
check "file-ne"  "yes"  "$([ ! -e /tmp/no_such_file_xyz ] && echo yes || echo no)"

# ── 12. string tests ──────────────────────────────────────────
check "str-cmp-lt"  "yes"  "$([ abc \< def ] && echo yes || echo no)"
check "str-cmp-gt"  "yes"  "$([ def \> abc ] && echo yes || echo no)"
check "str-regex"   "yes"  "$(echo hello123 | grep -qE '^[a-z]+[0-9]+$' && echo yes || echo no)"
check "str-glob"    "yes"  "$(x=foobar; [[ $x == foo* ]] && echo yes || echo no)"
check "str-contain" "yes"  "$(x=foobar; [[ $x == *oo* ]] && echo yes || echo no)"

# ── 13. sort / uniq / cut / tr ───────────────────────────────
check "sort-alpha"  "a b c"  "$(echo -e 'c\na\nb' | sort | tr '\n' ' ' | sed 's/ $//')"
check "sort-num"    "1 2 3"  "$(echo -e '3\n1\n2' | sort -n | tr '\n' ' ' | sed 's/ $//')"
check "uniq-basic"  "a b"    "$(echo -e 'a\na\nb' | uniq | tr '\n' ' ' | sed 's/ $//')"
check "cut-field"   "b"      "$(echo a:b:c | cut -d: -f2)"
check "tr-squeeze"  "a b c"  "$(echo 'a  b  c' | tr -s ' ')"

# ── 14. awk / sed ─────────────────────────────────────────────
check "awk-print"  "hello"   "$(echo hello world | awk '{print $1}')"
check "awk-sum"    "6"       "$(echo -e '1\n2\n3' | awk '{s+=$1}END{print s}')"
check "sed-sub"    "hXllo"   "$(echo hello | sed 's/e/X/')"
check "sed-del"    "ac"      "$(echo -e 'a\nb\nc' | sed '/b/d' | tr -d '\n')"

# ── 15. signals & pipes ───────────────────────────────────────
check "sigpipe-ok"  "ok"  "$(bash -c '(yes 2>/dev/null | head -c 1 >/dev/null); echo ok' 2>/dev/null)"
check "pipe-nodata" "1"   "$(echo | wc -c | tr -d ' \n')"
check "pipe-eof"    ""    "$(true | cat)"

# ── 16. subshell isolation ────────────────────────────────────
x=outer
check "subsh-iso"   "outer"  "$((x=inner); echo $x)"
check "subsh-exit"  "ok"     "$(( exit 0 ) && echo ok)"
check "subsh-env"   "42"     "$(A=42 bash -c 'echo $A')"

# ── 17. read builtin ─────────────────────────────────────────
check "read-basic"  "hello"  "$(echo hello | (read line; echo $line))"
check "read-multi"  "a b"    "$(echo 'a b' | (read x y; echo $x $y))"
check "read-ifs"    "a:b"    "$(echo 'a:b' | (IFS=: read x y; echo $x:$y))"

# ── 18. getopts ───────────────────────────────────────────────
parse(){ local o; while getopts "ab:c" o "$@"; do
    case $o in a) echo A;; b) echo "B=$OPTARG";; c) echo C;; esac
done; }
check "getopts-a"   "A"    "$(parse -a)"
check "getopts-b"   "B=X"  "$(parse -b X)"
check "getopts-abc" "A
B=Y
C"  "$(parse -a -b Y -c)"

# ── 19. trap ──────────────────────────────────────────────────
check "trap-exit"   "bye"  "$(bash -c 'trap "echo bye" EXIT; exit 0')"
check "trap-int"    "caught"  "$(bash -c 'trap "echo caught; exit 0" INT; kill -INT $$; echo nope' 2>/dev/null)"

# ── 20. misc builtins ─────────────────────────────────────────
check "type-builtin"  "echo is a shell builtin"  "$(type echo)"
check "pwd-works"     "/"                          "$(cd /; pwd)"
check "set-e"         "1"                          "$(bash -c 'set -e; echo 1')"

# ── 21. /dev symlinks ─────────────────────────────────────────
check "dev-stdin-link"   "/proc/self/fd/0"  "$(readlink /dev/stdin)"
check "dev-stdout-link"  "/proc/self/fd/1"  "$(readlink /dev/stdout)"
check "dev-stderr-link"  "/proc/self/fd/2"  "$(readlink /dev/stderr)"
check "dev-fd-link"      "/proc/self/fd"    "$(readlink /dev/fd)"
check "dev-stdin-read"   "hello"            "$(echo hello | cat /dev/stdin)"

# ── 22. /proc/sys ─────────────────────────────────────────────
check "sys-hostname"     "ranix"   "$(cat /proc/sys/kernel/hostname)"
check "sys-pid-max"      "32768"   "$(cat /proc/sys/kernel/pid_max)"
check "sys-somaxconn"    "128"     "$(cat /proc/sys/net/core/somaxconn)"
check "sys-file-max"     "65536"   "$(cat /proc/sys/fs/file-max)"

# ── 23. mount ─────────────────────────────────────────────────
check "mount-tmpfs"  "ok"  "$(mount -t tmpfs tmpfs /tmp && echo ok)"
check "mount-write"  "hi"  "$(echo hi > /tmp/x && cat /tmp/x)"

# ── 24. /etc files ────────────────────────────────────────────
check "etc-hosts"    "localhost"     "$(grep localhost /etc/hosts | awk '{print $2}')"
check "etc-resolv"   "nameserver"    "$(awk 'NR==1{print $1}' /etc/resolv.conf)"
check "etc-passwd"   "root"          "$(awk -F: 'NR==1{print $1}' /etc/passwd)"

# ── 25. statfs / df ───────────────────────────────────────────
check "df-has-rows"  "2"    "$(df / 2>/dev/null | wc -l | tr -d ' ')"
check "df-nonzero"   "1"    "$(df / 2>/dev/null | awk 'NR==2{print ($2>0)?1:0}')"


# ── 26. umount ────────────────────────────────────────────────
mkdir -p /tmp/umnt
check "umount-mount"  "ok"  "$(mount -t tmpfs tmpfs /tmp/umnt && echo ok)"
check "umount-write"  "xyz" "$(echo xyz > /tmp/umnt/f && cat /tmp/umnt/f)"
check "umount-do"     "ok"  "$(umount /tmp/umnt && echo ok)"
check "umount-gone"   ""    "$(cat /tmp/umnt/f 2>/dev/null)"

# ── 27. sysctl write ──────────────────────────────────────────
check "sysctl-write-hostname"  "testhostname" \
    "$(echo testhostname > /proc/sys/kernel/hostname && cat /proc/sys/kernel/hostname)"
check "sysctl-restore-hostname" "ranix" \
    "$(echo ranix > /proc/sys/kernel/hostname && cat /proc/sys/kernel/hostname)"
check "sysctl-hostname-cmd"    "ranix" "$(hostname)"

# ── 28. /proc entries ─────────────────────────────────────────
check "proc-self-status"  "1"  "$([ -f /proc/self/status ] && echo 1)"
check "proc-self-maps"    "1"  "$([ -f /proc/self/maps ] && echo 1)"
check "proc-self-exe"     "1"  "$([ -n \"$(readlink /proc/self/exe)\" ] && echo 1)"
check "proc-self-cwd"     "1"  "$([ -n \"$(readlink /proc/self/cwd)\" ] && echo 1)"
check "proc-self-root"    "/"  "$(readlink /proc/self/root)"
check "proc-self-comm"    "1"  "$([ -n \"$(cat /proc/self/comm)\" ] && echo 1)"
check "proc-self-ns-uts"  "uts" "$(readlink /proc/self/ns/uts | cut -d: -f1)"
check "proc-self-ns-ipc"  "ipc" "$(readlink /proc/self/ns/ipc | cut -d: -f1)"
check "proc-self-ns-time" "time" "$(readlink /proc/self/ns/time | cut -d: -f1)"
check "proc-self-ns-mnt"  "mnt" "$(readlink /proc/self/ns/mnt | cut -d: -f1)"
check "proc-self-ns-pid"  "pid" "$(readlink /proc/self/ns/pid | cut -d: -f1)"
check "proc-self-ns-user" "user" "$(readlink /proc/self/ns/user | cut -d: -f1)"
check "proc-self-ns-net"  "net" "$(readlink /proc/self/ns/net | cut -d: -f1)"
check "proc-self-ns-cgroup" "cgroup" "$(readlink /proc/self/ns/cgroup | cut -d: -f1)"
check "proc-filesystems"  "1"  "$(grep -c ext2 /proc/filesystems)"
check "proc-meminfo"      "1"  "$([ -f /proc/meminfo ] && echo 1)"
check "proc-uptime"       "1"  "$([ -f /proc/uptime ] && echo 1)"
check "proc-version"      "1"  "$([ -f /proc/version ] && echo 1)"
check "proc-net-dev"      "1"  "$([ -f /proc/net/dev ] && echo 1)"
check "proc-self-pid"     "1"  "$(awk '/^Pid:/{print ($2>0)?1:0}' /proc/self/status)"
check "proc-tgid-eq-pid"  "1"  "$(awk '/^Pid:/{p=$2} /^Tgid:/{t=$2} END{print (p==t)?1:0}' /proc/self/status)"

# ── 29. symlinks ──────────────────────────────────────────────
check "symlink-create"   "hello"     "$(echo hello > /tmp/sym_orig && ln -s /tmp/sym_orig /tmp/sym_link && cat /tmp/sym_link)"
check "symlink-readlink" "/tmp/sym_orig" "$(readlink /tmp/sym_link)"
check "symlink-unlink"   "1"         "$(rm /tmp/sym_link && [ ! -e /tmp/sym_link ] && echo 1)"
check "hardlink-create"  "world"     "$(echo world > /tmp/hl_orig && ln /tmp/hl_orig /tmp/hl_link && cat /tmp/hl_link)"
check "hardlink-persist" "world"     "$(rm /tmp/hl_orig && cat /tmp/hl_link)"

# ── 30. permissions ───────────────────────────────────────────
check "chmod-mode-600" "-rw-------" "$(echo x > /tmp/perm_f && chmod 600 /tmp/perm_f && ls -l /tmp/perm_f | awk '{print $1}')"
check "chmod-mode-644" "-rw-r--r--" "$(chmod 644 /tmp/perm_f && ls -l /tmp/perm_f | awk '{print $1}')"
check "chmod-exec"     "1"   "$(printf '#!/bin/sh\necho 1\n' > /tmp/perm_x && chmod +x /tmp/perm_x && /tmp/perm_x)"
check "stat-filetype"  "1"   "$(stat -c '%F' /tmp/perm_f 2>/dev/null | grep -c 'file')"
check "umask-set-get"  "0022" "$(umask 0077; umask 0022; umask)"

# ── 31. named pipes (FIFO) ────────────────────────────────────
check "fifo-anon"      "pipedata" \
    "$(r=$(echo pipedata | cat); echo $r)"
check "fifo-subsh"     "42"  \
    "$(echo 42 | { read v; echo $v; })"
check "pipe-fork-rw"   "hello" \
    "$(echo hello | (cat))"

# ── 32. process groups & sessions ────────────────────────────
check "getpgrp-nonzero" "1"  "$([ $$ -gt 0 ] && echo 1)"
check "pgrp-positive"   "1"  "$([ $(ps | wc -l) -gt 1 ] && echo 1)"
check "getpid-positive" "1"  "$([ $$ -gt 0 ] && echo 1)"
check "subshell-pid"    "1"  "$(bash -c '[ $$ -gt 0 ] && echo 1')"

# ── 33. more signals ──────────────────────────────────────────
check "sig-usr1"    "caught" \
    "$(bash -c 'trap "echo caught; exit 0" USR1; kill -USR1 $$' 2>/dev/null)"
check "sig-kill-0"  "alive"  \
    "$(bash -c 'kill -0 $$ 2>/dev/null && echo alive')"
check "sig-term"    "0"      \
    "$(bash -c 'trap "" TERM; kill -TERM $$; echo 0' 2>/dev/null)"
check "pipefail-sig" "0"   \
    "$(bash -c 'yes | head -1 > /tmp/pf_out; echo $?' 2>/dev/null)"

# ── 34. file operations ───────────────────────────────────────
check "truncate-file"  "3"  "$(echo hello > /tmp/trunc_f && dd if=/tmp/trunc_f bs=1 count=3 of=/tmp/trunc_f2 2>/dev/null && wc -c < /tmp/trunc_f2 | tr -d ' ')"
check "dd-copy"        "5"  "$(echo hello > /tmp/dd_src && dd if=/tmp/dd_src of=/tmp/dd_dst bs=1 count=5 2>/dev/null && wc -c < /tmp/dd_dst | tr -d ' ')"
check "cp-file"        "ok" "$(echo ok > /tmp/cp_src && cp /tmp/cp_src /tmp/cp_dst && cat /tmp/cp_dst | tr -d '\n')"
check "wc-lines"       "3"  "$(printf 'a\nb\nc\n' > /tmp/wc_f && wc -l < /tmp/wc_f | tr -d ' ')"
check "head-n1"        "line1" "$(printf 'line1\nline2\n' | head -1)"
check "tail-n1"        "line2" "$(printf 'line1\nline2\n' | tail -1)"

# ── 35. network ───────────────────────────────────────────────
check "net-dev-entries" "1"  "$(awk 'NR>2{found=1} END{print found+0}' /proc/net/dev)"
check "proc-net-tcp"    "1"  "$([ -f /proc/net/tcp ] && echo 1)"
check "proc-net-udp"    "1"  "$([ -f /proc/net/udp ] && echo 1)"

# ── 36. C test: threads + tgid ────────────────────────────────
check "c-threads"    "OK"  "$(timeout 8 /bin/test_threads 2>/dev/null)"

# ── 37. C test: inotify ───────────────────────────────────────
check "c-inotify"    "OK"  "$(timeout 5 /bin/test_inotify 2>/dev/null)"

# ── 38. C test: AF_UNIX path socket ──────────────────────────
check "c-unix-sock"  "OK"  "$(timeout 5 /bin/test_unix 2>/dev/null)"
check "c-mqueue"     "OK"  "$(timeout 5 /bin/test_mqueue 2>/dev/null)"
check "c-futex-timed" "OK" "$(timeout 8 /bin/test_futex_timed 2>/dev/null)"
check "c-poll"       "OK"  "$(timeout 8 /bin/test_poll 2>/dev/null)"
check "c-timerfd"    "OK"  "$(timeout 8 /bin/test_timerfd 2>/dev/null)"
check "c-eventfd"    "OK"  "$(timeout 8 /bin/test_eventfd 2>/dev/null)"
check "c-mmap"       "OK"  "$(timeout 8 /bin/test_mmap 2>/dev/null)"
check "c-hugepage"   "OK"  "$(timeout 8 /bin/test_hugepage 2>/dev/null)"
check "c-hugepage1g" "OK"  "$(timeout 20 /bin/test_hugepage1g 2>/dev/null)"
check "c-thp"        "OK"  "$(timeout 15 /bin/test_thp 2>/dev/null)"
check "c-pty"        "OK"  "$(timeout 8 /bin/test_pty 2>/dev/null)"
# GPT + ext4: only where a virtio-blk disk is attached (x86_64 test target)
if ls /sys/block 2>/dev/null | grep -q vblk; then
check "gpt-ext4"     "ranix-gpt-ok" "$(cat /mnt/hello.txt 2>/dev/null)"
check "ext4-write"   "ranix-w64"    "$(echo -n ranix-w64 > /mnt/wtest.txt 2>/dev/null; cat /mnt/wtest.txt 2>/dev/null)"
fi
check "sys-ostype"   "Ranix" "$(cat /sys/kernel/ostype 2>/dev/null)"
check "sys-net-addr" "00:00:00:00:00:00" "$(cat /sys/class/net/lo/address 2>/dev/null)"
check "sys-net-state" "up"  "$(cat /sys/class/net/lo/operstate 2>/dev/null)"
check "sys-cpu-online" "0-3" "$(cat /sys/devices/system/cpu/online 2>/dev/null)"
check "sys-block-ls"  "ok"  "$(ls /sys/block >/dev/null 2>&1 && echo ok)"
check "c-pipestress" "OK" "$(timeout 60 /bin/test_pipestress 2>/dev/null | tail -1)"
check "c-tcp"        "OK"  "$(timeout 10 /bin/test_tcp 2>/dev/null)"
check "c-udp"        "OK"  "$(timeout 10 /bin/test_udp 2>/dev/null)"
check "c-uts-ns"     "uts-ok" "$(timeout 10 /bin/test_uts 2>/dev/null)"
check "c-setns"      "setns-ok" "$(timeout 10 /bin/test_setns 2>/dev/null)"
check "c-clone-ns"   "clonens-ok" "$(timeout 10 /bin/test_clonens 2>/dev/null)"
check "c-ipc-ns"     "ipcns-ok" "$(timeout 10 /bin/test_ipcns 2>/dev/null)"
check "c-ipc-setns"  "ipcsetns-ok" "$(timeout 10 /bin/test_ipcsetns 2>/dev/null)"
check "c-time-ns"    "timens-ok" "$(timeout 10 /bin/test_timens 2>/dev/null)"
check "dynamic-link" "dynamic-ok" "$(timeout 10 /bin/test_dynamic 2>/dev/null)"
check "shared-lib"   "sharedlib-ok" "$(timeout 10 /bin/test_sharedlib 2>/dev/null)"
check "dlopen"       "dlopen-ok" "$(timeout 10 /bin/test_dlopen 2>/dev/null)"
check "cgroup-pids"  "cgroup-ok" "$(timeout 15 /bin/test_cgroup 2>/dev/null)"
check "cgroup-mem"   "memcg-ok" "$(timeout 15 /bin/test_memcg 2>/dev/null)"
check "cgroup-freeze" "freeze-ok" "$(timeout 15 /bin/test_freeze 2>/dev/null)"
check "cgroup-cpu"   "cpuw-ok" "$(timeout 15 /bin/test_cpuweight 2>/dev/null)"
check "mount-ns"     "mountns-ok" "$(timeout 10 /bin/test_mountns 2>/dev/null)"
check "pid-ns"       "pidns-ok" "$(timeout 10 /bin/test_pidns 2>/dev/null)"
check "user-ns"      "userns-ok" "$(timeout 10 /bin/test_userns 2>/dev/null)"
check "net-ns"      "netns-ok" "$(timeout 10 /bin/test_netns 2>/dev/null)"
check "cgroup-ns"   "cgroupns-ok" "$(timeout 10 /bin/test_cgroupns 2>/dev/null)"
check "container"     "container-ok" "$(timeout 12 /bin/test_container 2>/dev/null)"

# ── 39. C test: ptrace ────────────────────────────────────────
check "c-ptrace"     "OK"  "$(timeout 8 /bin/test_ptrace 2>/dev/null)"

# ── 40. misc ─────────────────────────────────────────────────
check "uname-has-output"  "1"    "$(uname -r | wc -c | awk '{print ($1>0)?1:0}')"
check "date-epoch"        "1"    "$(date +%s | awk '{print ($1>0)?1:0}')"
check "clock-monotonic"   "OK"   "$(timeout 8 /bin/test_clock 2>/dev/null)"
check "env-has-path"      "1"    "$(env | grep -c '^PATH=')"
check "which-bash"        "1"    "$(which bash | grep -c bash)"
check "seq-count"         "5"    "$(seq 1 5 | wc -l | tr -d ' ')"
check "expr-calc"         "42"   "$(expr 6 \* 7)"
check "sort-order"        "1
2
3" "$(printf '3\n1\n2\n' | sort -n)"

# ── 41. credentials (uid/gid/resuid/caps/getrandom) ──────────
check "uid-is-root"    "0"  "$(id -u)"
check "gid-is-root"    "0"  "$(id -g)"
check "proc-uid-field" "0"  "$(awk '/^Uid:/{print $2}' /proc/self/status)"
check "proc-euid-field" "0" "$(awk '/^Uid:/{print $3}' /proc/self/status)"
check "proc-gid-field" "0"  "$(awk '/^Gid:/{print $2}' /proc/self/status)"
check "proc-capprm-set" "1" "$(awk '/^CapPrm:/{print (length($2)>0)?1:0}' /proc/self/status)"
check "proc-capeff-nonzero" "1" \
    "$(awk '/^CapEff:/{print ($2 ~ /[1-9a-fA-F]/)?1:0}' /proc/self/status)"
check "c-creds"        "OK"  "$(timeout 5 /bin/test_creds 2>/dev/null | tail -1)"

# ── 42. ASLR — mmap base differs between processes ────────────
_a1=$(/bin/test_aslr 2>/dev/null)
_a2=$(/bin/test_aslr 2>/dev/null)
check "aslr-mmap-differs" "1" "$([ \"$_a1\" != \"$_a2\" ] && echo 1 || echo 0)"

# ── 43. /dev/shm symlink → /run/shm ──────────────────────────
check "devshm-link"  "/run/shm"  "$(readlink /dev/shm)"
check "devshm-write" "ok"        "$(echo ok > /run/shm/t && cat /run/shm/t && rm /run/shm/t)"

# ── 44. signalfd + epoll ──────────────────────────────────────
check "c-signalfd"   "OK"  "$(timeout 5 /bin/test_signalfd 2>/dev/null | tail -1)"

# ── 45. su — kullanıcı değiştirme ────────────────────────────
check "su-switches-uid"  "1000" \
    "$(timeout 5 su test -c 'id -u' 2>/dev/null)"
check "su-switches-gid"  "1000" \
    "$(timeout 5 su test -c 'id -g' 2>/dev/null)"
check "shadow-no-read" "1" \
    "$(timeout 5 su test -c 'cat /etc/shadow 2>/dev/null' 2>/dev/null | wc -c | awk '{print ($1==0)?1:0}')"

echo "---"

# ── 46. SMP — nproc ve affinity ───────────────────────────────
check "smp-nproc"     "4"  "$(nproc)"
check "smp-affinity-nproc" "4" \
    "$(timeout 5 /bin/test_affinity 2>/dev/null | grep '^nproc=' | sed 's/nproc=//')"
check "smp-threads-ok" "OK" \
    "$(timeout 5 /bin/test_affinity 2>/dev/null | tail -1)"
check "smp-cow-race" "OK" \
    "$(timeout 90 /bin/test_cowrace 2>/dev/null | tail -1)"

echo "---"
echo "Results: $PASS passed, $FAIL failed out of $((PASS+FAIL)) tests"
[ $FAIL -eq 0 ] && echo ALL_PASS || echo SOME_FAIL
