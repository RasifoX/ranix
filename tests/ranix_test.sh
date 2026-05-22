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

echo "---"
echo "Results: $PASS passed, $FAIL failed out of $((PASS+FAIL)) tests"
[ $FAIL -eq 0 ] && echo ALL_PASS || echo SOME_FAIL
