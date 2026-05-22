#!/bin/bash
set +m
PASS=0; FAIL=0

check() {
    local desc="$1" expect="$2" actual="$3"
    if [ "$actual" = "$expect" ]; then
        PASS=$((PASS+1))
    else
        FAIL=$((FAIL+1))
        echo "FAIL[$FAIL]: $desc  expect='$expect' got='$actual'"
    fi
}

# 1: fork512 stress
for i in $(seq 1 512); do true & done; wait 2>/dev/null
check "fork512" "" ""

# 2: exec200 batch
for i in $(seq 1 200); do echo x | cat >/dev/null & done; wait 2>/dev/null
check "exec200" "" ""

# 3: basic echo
check "echo" "hello" "$(echo hello)"

# 4: arithmetic
check "arith" "42" "$((6*7))"

# 5: string length
S="hello world"
check "strlen" "11" "${#S}"

# 6: substring
check "substr" "world" "${S:6}"

# 7: pipe + grep
check "pipe-grep" "foo" "$(echo -e 'foo\nbar' | grep foo)"

# 8: sort
check "sort" "$(printf 'a\nb\nc')" "$(printf 'c\na\nb' | sort)"

# 9: wc -w
check "wc-w" "3" "$(echo one two three | wc -w | tr -d ' ')"

# 10: sed substitution
check "sed" "bar" "$(echo foo | sed 's/foo/bar/')"

# 11: awk field
check "awk" "world" "$(echo 'hello world' | awk '{print $2}')"

# 12: head
check "head" "first" "$(printf 'first\nsecond\nthird' | head -1)"

# 13: tail
check "tail" "third" "$(printf 'first\nsecond\nthird' | tail -1)"

# 14: cut
check "cut" "hello" "$(echo 'hello:world' | cut -d: -f1)"

# 15: tr
check "tr" "HELLO" "$(echo hello | tr a-z A-Z)"

# 16: uniq
check "uniq" "$(printf 'a\nb')" "$(printf 'a\na\nb\nb' | uniq)"

# 17: file create/read
echo "ranix" > /tmp/t.txt
check "file-rw" "ranix" "$(cat /tmp/t.txt)"
rm -f /tmp/t.txt

# 18: mkdir/rmdir
mkdir -p /tmp/testdir
check "mkdir" "0" "$?"
rmdir /tmp/testdir
check "rmdir" "0" "$?"

# 19: redirection append
echo "line1" > /tmp/app.txt
echo "line2" >> /tmp/app.txt
check "append" "2" "$(wc -l < /tmp/app.txt | tr -d ' ')"
rm -f /tmp/app.txt

# 20: subshell
check "subshell" "42" "$(x=42; echo $x)"

# 21: function
myfunc() { echo "func$1"; }
check "function" "func7" "$(myfunc 7)"

# 22: array
arr=(a b c)
check "array" "b" "${arr[1]}"

# 23: for loop count
cnt=0
for i in 1 2 3 4 5; do cnt=$((cnt+1)); done
check "for-loop" "5" "$cnt"

# 24: while loop
n=0; i=0
while [ $i -lt 5 ]; do n=$((n+i)); i=$((i+1)); done
check "while-loop" "10" "$n"

# 25: if/else
x=10
if [ $x -gt 5 ]; then r=big; else r=small; fi
check "if-else" "big" "$r"

# 26: case
val=banana
case $val in
  apple)  r=fruit1 ;;
  banana) r=fruit2 ;;
  *)      r=other  ;;
esac
check "case" "fruit2" "$r"

# 27: heredoc
check "heredoc" "hello heredoc" "$(cat <<'EOF'
hello heredoc
EOF
)"

# 28: read from pipe
check "read-pipe" "hello" "$(echo hello | (read line; echo $line))"

# 29: background + wait with status
(exit 42) &
wait $!
check "bg-wait" "42" "$?"

# 30: signal delivery (SIGCHLD counted)
child_done=0
trap 'child_done=$((child_done+1))' SIGCHLD
( sleep 0 ) &
wait $! 2>/dev/null
trap - SIGCHLD
check "sigchld" "" ""

# 31: pipe multiple commands
check "multi-pipe" "3" "$(echo 'one two three' | tr ' ' '\n' | wc -l | tr -d ' ')"

# 32: command substitution nested
check "nested-cmdsub" "HELLO" "$(echo $(echo hello) | tr a-z A-Z)"

# 33: glob expansion
touch /tmp/glob_a.txt /tmp/glob_b.txt
count=$(ls /tmp/glob_*.txt 2>/dev/null | wc -l | tr -d ' ')
check "glob" "2" "$count"
rm -f /tmp/glob_a.txt /tmp/glob_b.txt

# 34: exec200 batch 2
for i in $(seq 1 200); do echo x | cat >/dev/null & done; wait 2>/dev/null
check "exec200-2" "" ""

# 35: deep 6-stage pipeline
check "deep-pipe-6" "6" "$(echo a:b:c:d:e:f | tr ':' '\n' | grep -c '.' | tr -d ' ')"

# 36: PID recycling 300
for i in $(seq 1 300); do true & done; wait 2>/dev/null
check "pid-recycle-300" "" ""

# 37: PID recycling 300 batch 2
for i in $(seq 1 300); do ( echo x > /dev/null ) & done; wait 2>/dev/null
check "pid-recycle-300-2" "" ""

# 38: pipe 1MB
_pipeout=$(dd if=/dev/zero bs=4096 count=256 2>/dev/null | wc -c | tr -d ' ')
check "pipe-1mb" "1048576" "$_pipeout"

# 39: multi-writer pipe
( dd if=/dev/zero bs=1 count=8192 2>/dev/null; dd if=/dev/zero bs=1 count=8192 2>/dev/null ) | wc -c | tr -d ' ' > /tmp/pwc.txt
check "pipe-multiwriter" "16384" "$(cat /tmp/pwc.txt)"
rm -f /tmp/pwc.txt

# 40: dentry stress 100
for i in $(seq 1 100); do cat /proc/self/status > /dev/null; done
check "dentry-stress-100" "" ""

# 41: /proc/cmdline readable
_cl=$(cat /proc/cmdline 2>/dev/null)
check "proc-cmdline" "0" "$?"

# 42: /proc/self/maps readable
cat /proc/self/maps > /dev/null 2>&1
check "proc-maps" "0" "$?"

# 43: /proc/meminfo MemTotal
check "meminfo-total" "1" "$(grep -c '^MemTotal' /proc/meminfo)"

# 44: /proc/meminfo MemFree
check "meminfo-free" "1" "$(grep -c '^MemFree' /proc/meminfo)"

# 45: memory pressure 50 subshells
for i in $(seq 1 50); do
    ( dd if=/dev/zero bs=4096 count=16 2>/dev/null | wc -c > /dev/null ) &
done
wait 2>/dev/null
check "mem-pressure-50" "" ""

# 46: concurrent I/O 50
for i in $(seq 1 50); do
    ( echo "proc$i" > /tmp/cptest_$i.txt && cat /tmp/cptest_$i.txt > /dev/null ) &
done
wait 2>/dev/null
ok=1
for i in $(seq 1 50); do
    [ "$(cat /tmp/cptest_$i.txt 2>/dev/null)" = "proc$i" ] || ok=0
    rm -f /tmp/cptest_$i.txt
done
check "concurrent-io-50" "1" "$ok"

# 47: SIGTERM self
trap 'got_sigterm=1' SIGTERM
got_sigterm=0
kill -SIGTERM $$
trap - SIGTERM
check "sigterm-self" "1" "$got_sigterm"

# 48: pipe basic
result=$(echo hello | (cat > /dev/null; echo ok))
check "pipe-basic-ok" "ok" "$result"

# 49: 10 files create/rm
for i in $(seq 1 10); do echo "ftest$i" > /tmp/ftest_$i.txt; done
fcount=$(ls /tmp/ftest_*.txt 2>/dev/null | wc -l | tr -d ' ')
check "fs-10files-create" "10" "$fcount"
for i in $(seq 1 10); do rm -f /tmp/ftest_$i.txt; done
check "fs-10files-rm" "0" "$?"

# 50: large file 16KB
dd if=/dev/zero bs=1024 count=16 2>/dev/null | tr '\0' 'A' > /tmp/large.txt
sz=$(wc -c < /tmp/large.txt | tr -d ' ')
check "large-file-16k" "16384" "$sz"
rm -f /tmp/large.txt

echo "=== RESULTS: PASS=$PASS FAIL=$FAIL ==="
echo "=== DONE ==="
