echo NT-START
D=/dev/nvme0n1
mkdir -p /mnt/b
sort /ref.txt > /tmp/ref.s
for p in /bad/*.pat; do
  c=$(basename $p .pat)
  test_ntfsfuzz $D --apply $p /tmp/undo
  echo "CASE-START $c"
  if fjord disk mount $D /mnt/b > /tmp/m 2>&1; then
    echo "MOUNTED $c"; grep warning /tmp/m
    fjord disk hash /mnt/b > /tmp/h 2> /tmp/e; rc=$?
    echo "HASHRC $rc"
    grep -v '^[0-9a-f]\{64\}  ' /tmp/h | grep -vE "^(LINK|LONGNAME|STREAMS) |wof|unknown-tag" | head -8
    grep "^[0-9a-f]\{64\}  " /tmp/h | sort > /tmp/h.s; echo "GOODCOUNT $(wc -l < /tmp/h.s)"; comm -23 /tmp/h.s /tmp/ref.s | head -12
    tail -1 /tmp/e
    fjord disk unmount /mnt/b; echo "UNMOUNT $?"
  else
    echo "REFUSED $c: $(tail -1 /tmp/m | cut -c1-90)"
  fi
  echo "CASE-END $c"
  test_ntfsfuzz $D --apply /tmp/undo /dev/null
done
dmesg | grep -ciE "panic|oops|general protection"
echo NT-END
poweroff -f
