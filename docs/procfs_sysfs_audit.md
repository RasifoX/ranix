# /proc, /sys, /dev completeness audit

Read-only audit of Ranix's pseudo-filesystem surfaces against Linux, dual-arch
(x86_64 + aarch64; both serve identical procfs/sysfs/devfs code, only `cpuinfo`
branches on arch). Linux userspace tooling (ps, top, free, ip, ss, mount, login,
glibc, language runtimes, container runtimes, udev/mdev) reads these surfaces;
missing or wrong-format nodes break those tools silently.

Sources audited:
- `kernel/fs/procfs.c` (2189 lines) — all of /proc
- `kernel/fs/sysfs.c` (655 lines) — all of /sys
- `kernel/fs/devfs.c` (572 lines) — /dev core + builtins
- `kernel/vfs.c:623-666` — /proc/mounts and /proc/PID/mountinfo formatters
- device registrations: `kernel/tty/tty_core.c:407-416`, `kernel/fs/pty.c:514-524`,
  `drivers/video/fbdev.c:101`, `drivers/input/input_core.c:159`, `kernel/block.c:102`

Severity: **HIGH** = breaks common tooling/runtimes; **MED** = breaks specific
tools or degrades output; **LOW** = rarely consumed or cosmetic.

---

## 1. /proc global

| Node | Ranix (present? format, file:line) | Linux | Gap | Who needs it | Sev |
|------|-----------------------------------|-------|-----|--------------|-----|
| version | yes, `Ranix version 6.6.0-ranix … #1 SMP` `procfs.c:67` | full uname string | faithful | uname -a, glibc | — |
| cpuinfo | yes, per-cpu, arch-branched `procfs.c:75` | full flags/MHz | stub fields (no `flags`/`features` real list; aarch64 lacks `Hardware`/`Revision`/`Serial`) | lscpu, nproc, runtimes feature-probe | MED |
| meminfo | yes `procfs.c:104` | ~50 fields | **partial**: Buffers/Cached/Inactive all 0, MemAvailable==MemFree, no Slab/Shmem/Dirty/Writeback/CommitLimit/Committed_AS/VmallocTotal | free, top, vmstat, JVM/Go/Node heap sizing | HIGH |
| stat | yes `procfs.c:143` | full | **stub**: intr/ctxt/btime all 0, no `softirq` line, procs_running hardcoded 1 | top/vmstat (cpu%), uptime (btime) | HIGH |
| uptime | yes `procfs.c:130` | idle field real | idle==uptime (no real idle) | uptime, top | LOW |
| loadavg | yes `procfs.c:163` | real EWMA | hardcoded `0.00 0.00 0.00` | uptime, top, schedulers | MED |
| cmdline | yes (kernel cmdline) `procfs.c:279` | kernel boot args | faithful | — | — |
| filesystems | yes, nodev-tagged `procfs.c:311` | full | faithful | mount autodetect | — |
| mounts | yes `vfs.c:623` `source mp fs rw 0 0` | per-mount | **source field == fstype** (no device path); opts always `rw` | mount, df, findmnt | MED |
| swaps | yes, header only `procfs.c:409` | swap areas | faithful (no swap) | swapon -s, free | LOW |
| vmstat | yes, 5 fields `procfs.c:450` | ~150 counters | **stub**: only nr_free_pages real; pgfault/pgmajfault 0 | vmstat, sar | MED |
| partitions | yes `procfs.c:418` | major minor blocks name | major hardcoded 8, no partition rows | lsblk, fdisk, df | MED |
| diskstats | yes `procfs.c:435` | 11 I/O counters | **all counters 0**, major hardcoded 8 | iostat, df | LOW |
| modules | yes, **empty** `procfs.c:406` | lsmod list | faithful (monolith) | lsmod, modprobe | — |
| self | yes → /proc/N `procfs.c:1991` | symlink | faithful | nearly everything | — |
| **interrupts** | **MISSING** | per-IRQ counts | absent | irqbalance, `top` (some), debugging | LOW |
| **devices** | **MISSING** | char/block major map | absent | MAKEDEV, some mknod helpers | MED |
| **cgroups** | **MISSING** | v1 controller table | absent | systemd, container runtimes (probe v1) | MED |
| **misc/crypto/kallsyms/buddyinfo/zoneinfo/locks/sysvipc/** | MISSING | various | absent | profilers, ipcs (sysvipc), perf | LOW |
| **thread-self** | **MISSING** | symlink → /proc/T/task/T | absent | thread-aware tooling, some glibc paths | LOW |

---

## 2. /proc/&lt;pid&gt;/

PID dir lookup table: `pid_files[]` `procfs.c:488` + explicit branches in
`procfs_piddir_lookup` `procfs.c:1446`.

| Node | Ranix (file:line) | Linux | Gap | Who needs it | Sev |
|------|-------------------|-------|-----|--------------|-----|
| status | yes `procfs.c:182` | ~55 fields | **partial**: VmRSS hardcoded 64 kB, VmSize≈brk only; no VmPeak/VmData/VmStk/VmExe/VmLck/VmPin/VmHWM, no `FDSize`, no `NSpid`, no `Seccomp`, no `Cpus_allowed` | ps, top, htop, status parsers | HIGH |
| stat | yes `procfs.c:231` | 52 fields | **partial**: starttime(22)=0, vsize=brk, rss hardcoded 64, fields 7-13 and 25-52 zero (no tty_nr, no real flags, no rss_lim, no processor) | ps, top, htop (these are the core ps fields) | HIGH |
| statm | yes `procfs.c:266` | 7 fields | resident hardcoded 64, shared/lib 0 | pmap, top | MED |
| cmdline | yes, **uses p->name not argv** `procfs.c:289` | NUL-sep argv | **wrong**: shows comm, single token, no real args | ps (full cmdline), pgrep -f, htop | HIGH |
| comm | yes `procfs.c:300` | short name | faithful | ps, pgrep | — |
| maps | yes `procfs.c:326` | per-VMA | **partial**: dev `00:00`, inode 0, no path/`[heap]`/`[stack]`/`[vdso]` labels | pmap, gdb, ASAN, JIT runtimes | MED |
| mountinfo | yes `vfs.c:643` | full | source field == fstype, fabricated ids | libmount, findmnt | MED |
| fd/ | yes, dir+symlinks `procfs.c:750` | fd symlinks | faithful (resolves to file path) | lsof, ls -l /proc/PID/fd | — |
| exe / cwd / root | yes, symlinks `procfs.c:832` | symlinks | root always `/`; cwd/exe real | readlink, lsof, getcwd fallback | LOW |
| mem | yes, R/W via vm switch `procfs.c:628` | cross-proc mem | faithful | gdb, debuggers | — |
| ns/ | yes (uts,ipc,time,mnt,pid,user,net,cgroup) `procfs.c:881` | ns symlinks | faithful incl. setns pinning | nsenter, container runtimes | — |
| uid_map / gid_map | yes, R/W `procfs.c:1351` | userns maps | faithful | unshare, container runtimes | — |
| cgroup | yes `0::<path>` v2 `procfs.c:1406` | v2 line | faithful (v2 only) | systemd, runtimes | — |
| timens_offsets | yes, R/W `procfs.c:1249` | offsets | faithful | container time ns | — |
| **environ** | **MISSING** | NUL-sep env | absent | ps e, debuggers, `cat /proc/PID/environ` | MED |
| **smaps / smaps_rollup** | **MISSING** | per-VMA detail | absent | pmap -x, memory profilers, OOM analysis | MED |
| **task/** | **MISSING** | per-thread dirs | absent | ps -T/-L, top -H, htop threads, gdb thread list | HIGH |
| **limits** | **MISSING** | rlimit table | absent | systemd, prlimit display, container runtimes | MED |
| **oom_score / oom_score_adj / oom_adj** | **MISSING** | OOM tuning | absent | systemd, container runtimes, oom-killer tuning | MED |
| **fdinfo/** | **MISSING** | per-fd pos/flags | absent | lsof -o, CRIU, epoll/inotify introspection | LOW |
| **io** | **MISSING** | r/w byte counters | absent | iotop, pidstat -d | LOW |
| **stack / wchan / sched / schedstat** | MISSING | scheduler info | absent | latencytop, debugging | LOW |
| **auxv** | **MISSING** | ELF auxv | absent | some loaders/debuggers, `LD_SHOW_AUXV` consumers | MED |
| **statm fields, status threads** | counted via proc_thread_count | — | thread count real, but no task/ to enumerate | — | — |
| **autogroup / cpuset / personality / setgroups** | MISSING | various | absent | systemd-specific | LOW |

---

## 3. /proc/sys (sysctl)

Tables: `sys_kernel_files[]` `procfs.c:1765`, `sys_net_core_files[]` `procfs.c:1777`,
`sys_fs_files[]` `procfs.c:1786`. Only kernel/, net/core/, fs/ subtrees exist.

| Knob | Ranix (file:line) | Linux value | Gap | Who needs it | Sev |
|------|-------------------|-------------|-----|--------------|-----|
| kernel.hostname | yes, R/W `procfs.c:544,1767` | hostname | faithful | hostname, glibc | — |
| kernel.domainname | yes `procfs.c:1768` | domain | faithful | — | — |
| kernel.ostype | yes (returns hostname!) `procfs.c:1766` | "Linux" | **wrong**: aliased to hostname, should be "Ranix" | uname consumers reading sysctl | LOW |
| kernel.pid_max | yes `32768` `procfs.c:548,1770` | 4194304 | value low but present | container runtimes, ulimit calc | MED |
| kernel.ngroups_max | yes `65536` `procfs.c:547` | 65536 | faithful | glibc getgrouplist | — |
| kernel.overcommit_memory | yes `0` `procfs.c:546` | — | present (read-only) | Redis (warns), JVM | MED |
| kernel.randomize_va_space | yes `0` `procfs.c:556,1772` | 2 | reports 0 (ASLR off-looking) | hardening checks | LOW |
| kernel.core_pattern | yes, R/W `procfs.c:557,1773` | pattern | faithful | coredump handlers, systemd-coredump | — |
| **kernel.osrelease** | **MISSING in /proc/sys** (only in /sys/kernel) | "6.6.0" | absent under /proc/sys/kernel | glibc/`uname` sysctl path, version checks | MED |
| **kernel.threads-max** | MISSING | thread cap | absent | runtimes, ulimit | LOW |
| **kernel.shmmax / shmall / sem / msgmax** | MISSING | SysV IPC limits | absent | PostgreSQL, Oracle, Redis | MED |
| **kernel.sched_* / sched_rt_runtime_us** | MISSING | sched tuning | absent | RT apps, audio | LOW |
| **kernel.yama.ptrace_scope** | MISSING | ptrace policy | absent | debuggers, gdb permission check | LOW |
| **kernel.cap_last_cap** | MISSING | last cap num | absent | libcap, container runtimes | MED |
| **kernel.unprivileged_userns_clone** | MISSING | userns gate | absent | Chrome, container runtimes | LOW |
| net.core.somaxconn | yes `128` `procfs.c:550,1778` | 4096 | present, value low | nginx, redis (warn) | MED |
| net.core.{r,w}mem_default/max | yes `procfs.c:551-554` | — | faithful constants | socket tuners | — |
| **net.ipv4.ip_forward** | MISSING | routing toggle | absent | routers, container NAT | MED |
| **net.ipv4.ip_local_port_range** | MISSING | ephemeral range | absent | servers, load testers | MED |
| **net.ipv4.tcp_* (tw_reuse, fin_timeout, keepalive_*)** | MISSING | TCP tuning | absent | high-conn servers | LOW |
| **net.ipv6.conf.*.disable_ipv6** | MISSING | v6 toggle | absent | dual-stack apps | LOW |
| fs.file-max | yes `65536` `procfs.c:549,1787` | — | faithful | servers | — |
| fs.file-nr | yes `4096 0 65536` `procfs.c:555,1788` | live counts | **stub** (not live) | lsof, monitoring | LOW |
| **fs.nr_open** | **MISSING** | per-proc fd ceiling | absent | servers raising ulimit -n, container runtimes | HIGH |
| **fs.inotify.max_user_watches / max_user_instances** | MISSING | inotify limits | absent | VS Code, watchman, webpack, file watchers | HIGH |
| **fs.pipe-max-size / fs.aio-max-nr** | MISSING | — | absent | databases, io_uring/aio apps | LOW |
| **vm.* (entire subtree)** | **MISSING** (no vm/ dir) | overcommit_ratio, max_map_count, swappiness, dirty_ratio… | absent | **vm.max_map_count** → Elasticsearch/JVM/games; vm.overcommit_ratio → Redis | HIGH |
| **vm.overcommit_memory** | exists under kernel/ (wrong dir) | should be vm/ | **wrong location**: Linux has it at vm.overcommit_memory not kernel.* | Redis reads vm.overcommit_memory | MED |

---

## 4. /proc/net

Table `net_files[]` `procfs.c:1698`. Functions `procfs.c:1566-1696`.

| Node | Ranix (file:line) | Linux | Gap | Who needs it | Sev |
|------|-------------------|-------|-----|--------------|-----|
| net/dev | yes `procfs.c:1566` | per-iface counters | **all counters 0** (header correct) | ifconfig, ip -s, sar -n | MED |
| net/route | yes `procfs.c:1639` | IPv4 routes | faithful format (default + subnet from netdev0) | route, netstat -r, ip route (fallback) | — |
| net/tcp | yes `procfs.c:1581` | listening+established | faithful hex format; inode synthesized | ss, netstat -t | — |
| net/udp | yes `procfs.c:1615` | sockets | faithful format | ss -u, netstat -u | — |
| net/if_inet6 | yes `procfs.c:1681` | v6 addrs | faithful (lo + per-dev ll/global) | ip -6 addr (fallback), netstat | — |
| **net/tcp6** | **MISSING** | v6 TCP sockets | absent | ss/netstat for IPv6 TCP | MED |
| **net/udp6** | **MISSING** | v6 UDP sockets | absent | ss/netstat for IPv6 UDP | MED |
| **net/unix** | **MISSING** | AF_UNIX sockets | absent | ss -x, netstat -x, lsof, D-Bus debugging | HIGH |
| **net/arp** | **MISSING** | ARP table | absent | arp, `ip neigh` (fallback) | MED |
| **net/snmp / netstat** | MISSING | protocol stats | absent | netstat -s, nstat, sar | LOW |
| **net/dev_mcast / igmp / packet / raw** | MISSING | various | absent | specialized tools | LOW |
| **net/sockstat** | **MISSING** | socket totals | absent | ss -s, monitoring | LOW |
| **net/wireless / net/route6 / net/ipv6_route** | MISSING | — | absent | ip -6 route, wireless tools | LOW |

---

## 5. /sys

Root subtree `sysfs_root_lookup` `sysfs.c:214` exposes: kernel/, class/, block/,
devices/, fs/, bus/. No module/, no firmware/, no hwmon/power/thermal.

| Node | Ranix (file:line) | Linux | Gap | Who needs it | Sev |
|------|-------------------|-------|-----|--------------|-----|
| /sys/kernel/{ostype,osrelease,hostname} | yes `sysfs.c:236` | + many | faithful subset | uname fallback | — |
| /sys/kernel/mm/transparent_hugepage/enabled | yes `sysfs.c:314` | toggle | read-only (no defrag) | THP-aware DBs (Mongo, Redis warn) | LOW |
| /sys/kernel/mm/hugepages/hugepages-2048kB/{nr,free}_hugepages | yes, nr R/W `sysfs.c:293` | + 1G pool | no hugepages-1048576kB dir | hugepage apps, DPDK | LOW |
| /sys/class/net/&lt;dev&gt;/{address,mtu,operstate} | yes `sysfs.c:502` | + many attrs | **partial**: no `flags`, `carrier`, `speed`, `statistics/`, `ifindex`, `type` | NetworkManager, ip, ethtool, container CNI | MED |
| /sys/block/&lt;dev&gt;/{size,stat} | yes `sysfs.c:524` | + queue/, ro, removable, dev | partial; no `queue/`, no `dev` (major:minor), no partitions | lsblk, udev, mdev | MED |
| /sys/devices/system/cpu/{online,possible} | yes `sysfs.c:562` | + cpuN/, topology/ | no per-cpu dirs, no cpufreq, no topology | lscpu (full), numactl, taskset (some) | MED |
| /sys/fs/cgroup (cgroup.controllers, cgroup.procs) | yes `sysfs.c:329` | full v2 tree | **stub**: two static files, controllers static string, procs hardcoded `1`; no subtree_control, no per-cgroup dirs, no cgroup.events | **systemd, Docker, Podman, k8s** (need writable v2 hierarchy) | HIGH |
| /sys/bus/pci/devices/&lt;addr&gt;/{vendor,device,capabilities} | yes `sysfs.c:365` | + class, irq, resource, config | partial PCI attrs | lspci (partial), driver probing | LOW |
| **/sys/class/{tty,block,input,mem,...}** | MISSING (only net) | many classes | absent | udev/mdev rules, getty | MED |
| **/sys/dev/{char,block}/&lt;maj:min&gt;** | MISSING | dev-number symlinks | absent | udev, systemd device units | MED |
| **uevent files + hotplug/netlink** | **MISSING** (no uevent anywhere) | per-device uevent + NETLINK_KOBJECT_UEVENT | **absent entirely** | **udev/mdev/eudev** (no dynamic /dev population), systemd | HIGH |
| **/sys/module/** | MISSING | module params | absent | tools reading module params | LOW |
| **/sys/firmware/{dmi,acpi,efi}/** | MISSING | firmware tables | absent | dmidecode, EFI tools, cloud-init | LOW |
| **/sys/class/power_supply, /sys/class/thermal, /sys/class/hwmon** | MISSING | sensors/battery | absent | upower, sensors, laptops | LOW |
| **/sys/fs/{ext4,cgroup v1 controllers}** | MISSING | fs tunables | absent | tune2fs sysfs, cgroup v1 | LOW |

---

## 6. /dev (devfs / devtmpfs)

Builtins `devfs.c:238`; dynamic registrations across drivers. Major:minor below
are decoded from the `rdev` argument and checked against Linux assignments.

| Node | Ranix (maj:min, file:line) | Linux maj:min | Gap | Who needs it | Sev |
|------|---------------------------|---------------|-----|--------------|-----|
| null | 1:3 `devfs.c:239` | 1:3 | correct | everything | — |
| zero | 1:5 `devfs.c:240` | 1:5 | correct | everything | — |
| full | 1:7 `devfs.c:241` | 1:7 | correct | tests, glibc | — |
| random | 1:8 `devfs.c:243` | 1:8 | correct | crypto, runtimes | — |
| urandom | 1:9 `devfs.c:242` | 1:9 | correct | crypto, runtimes, ssl | — |
| kmsg | 1:11 `devfs.c:570` | 1:11 | correct (drains klog) | dmesg, journald | — |
| tty | 5:0 `tty_core.c:407` | 5:0 | correct (ctty) | shells, login | — |
| console | 5:1 `tty_core.c:408` | 5:1 | correct | init, login, kernel | — |
| ptmx | 5:2 `pty.c:515` | 5:2 | correct | openpty, sshd, terminals | — |
| tty0 | 4:0 `tty_core.c:409` | 4:0 | correct | console | — |
| tty1..tty6 | 4:1..4:6 `tty_core.c:416` | 4:1..4:6 | correct (shared discipline) | getty, login, openvt | — |
| pts/0..N | 136:0..N `pty.c:523` | 136:n | correct (devpts-style) | terminals, sshd | — |
| fb0 | 29:0 `fbdev.c:101` | 29:0 | correct | fbterm, graphics | — |
| rtc0 | 254:0 `devfs.c:571` | 254:0 (rtc class) | correct (RTC_RD_TIME only) | hwclock | — |
| input/event0 | 13:96 `input_core.c:159` | 13:64+ (eventN) | correct-ish | libinput, X, evdev | — |
| disk nodes (sd*/vd*) | 8:* S_IFBLK `block.c:102` | 8:* / 254:* (vd) | major from driver; check vd uses 8 not 254 | mount, fdisk, lsblk | LOW |
| stdin/stdout/stderr | fd-proxy chardev `devfs.c:432` | symlinks→fd | functional (proxy not symlink, but readlink ok) | shells, glibc | — |
| fd → /proc/self/fd | symlink `devfs.c:233` | symlink | correct | shells | — |
| shm → /run/shm | symlink `devfs.c:234` | usually tmpfs dir at /dev/shm | **symlink not a tmpfs mount**; target /run/shm must exist+be tmpfs | POSIX shm_open, Python mp, Chrome | HIGH |
| **pts/ptmx** | MISSING (ptmx only at /dev/ptmx) | /dev/pts/ptmx | absent | strict devpts consumers | LOW |
| **/dev/std{in,out,err}** | present (above) | — | — | — | — |
| **/dev/tty (ctty)** | present | — | — | — | — |
| **/dev/loop-control, loop0..** | **MISSING** | 10:237 / 7:N | absent | losetup, mount -o loop, snap/flatpak, CI images | MED |
| **/dev/net/tun** | **MISSING** | 10:200 | absent | OpenVPN, WireGuard, Docker bridge, QEMU netdev | MED |
| **/dev/disk/by-{id,uuid,label,path}/** | MISSING | udev symlinks | absent (no udev) | mount by UUID, fstab, GRUB | MED |
| **/dev/mapper/, /dev/dm-*** | MISSING | device-mapper | absent | LVM, LUKS, cryptsetup | LOW |
| **/dev/mem, /dev/kmem, /dev/port** | MISSING | 1:1, 1:2, 1:4 | absent | X (legacy), flashrom, hw tools | LOW |
| **/dev/fd** | present (symlink) | — | — | — | — |
| **/dev/hugepages (mount)** | MISSING | hugetlbfs mount | absent | DBs, DPDK | LOW |
| **/dev/snd, /dev/dri, /dev/input/mice** | MISSING | audio/gpu/mouse | absent | audio, GPU, input stacks | LOW |
| **/dev/vcs, /dev/vcsa** | MISSING | 7:N | absent | screen-scrapers, setterm | LOW |

---

## Top 15 missing / wrong nodes that break real Linux software

Ranked by how commonly real tooling and runtimes depend on them.

1. **/proc/&lt;pid&gt;/cmdline shows comm, not argv** (`procfs.c:289`) — HIGH. `ps`
   full-command, `pgrep -f`, htop, supervisors, and every "find my process by
   args" workflow get a single bogus token. Single most visible ps/top defect.
2. **/proc/&lt;pid&gt;/stat + status are partial** (starttime=0, rss=64,
   no VmPeak/VmHWM/tty_nr) (`procfs.c:231`, `:182`) — HIGH. ps/top/htop CPU%,
   elapsed time, and memory columns are wrong or zero. Core monitoring breaks.
3. **/proc/sys/vm/ subtree absent, esp. vm.max_map_count** — HIGH. Elasticsearch
   refuses to start, JVM/Go/native allocators and games that bump max_map_count
   fail; Redis reads vm.overcommit_memory (Ranix has it under kernel.* — wrong path).
4. **/sys uevent + hotplug netlink absent** (`sysfs.c` whole) — HIGH. udev/mdev/eudev
   cannot enumerate or populate /dev dynamically; systemd device units never fire.
   Anything expecting dynamic device management is dead.
5. **/sys/fs/cgroup is a 2-file stub** (`sysfs.c:329`) — HIGH. systemd, Docker,
   Podman, Kubernetes need a real writable cgroup v2 hierarchy (subtree_control,
   per-cgroup dirs, controllers, cgroup.events). Container runtimes won't run.
6. **/proc/&lt;pid&gt;/task/ absent** (`procfs.c:1446`) — HIGH. `ps -T`, `top -H`,
   htop thread view, and gdb thread enumeration see no threads.
7. **/proc/net/unix absent** (`procfs.c:1698`) — HIGH. `ss -x`, `netstat -x`, lsof
   socket resolution, D-Bus/socket debugging all blind to AF_UNIX.
8. **fs.inotify.max_user_watches absent** — HIGH. VS Code, webpack, watchman,
   chokidar emit fatal "ENOSPC: inotify watch limit" or refuse to start.
9. **fs.nr_open absent** — HIGH. Servers/container runtimes that raise `ulimit -n`
   read/clamp against nr_open; absence breaks high-fd-count services.
10. **/dev/shm is a symlink to /run/shm, not a tmpfs** (`devfs.c:234`) — HIGH.
    `shm_open`, Python multiprocessing, Chrome, PostgreSQL fail unless /run/shm
    exists and is tmpfs; many images assume a real /dev/shm mount.
11. **/proc/meminfo partial** (Cached/Buffers/Slab/Committed_AS = 0) (`procfs.c:104`)
    — HIGH. `free`/top show no cache; JVM/Go/Node container heap auto-sizing and
    cgroup-unaware memory math misbehave.
12. **/proc/stat counters stubbed** (intr/ctxt/btime=0, no softirq) (`procfs.c:143`)
    — HIGH. top/vmstat CPU-utilization deltas and `uptime` boot time are wrong.
13. **/dev/net/tun absent** — MED/HIGH. OpenVPN, WireGuard userspace, Docker
    bridge networking, QEMU `-netdev tap` cannot create tunnels.
14. **/proc/&lt;pid&gt;/environ absent** (`procfs.c:1446`) — MED. `ps e`, debuggers,
    and env-introspection (e.g. reading a daemon's environment) fail.
15. **/proc/&lt;pid&gt;/limits, oom_score_adj absent** — MED. systemd, container
    runtimes, and `prlimit`-style introspection cannot read/show process limits.

### Surfaces already faithful (no action needed)
- /proc/version, /proc/cmdline, /proc/filesystems, /proc/self, /proc/swaps, /proc/modules
- /proc/net/{tcp,udp,route,if_inet6} formats; tcp/udp inode synthesis is adequate
- /proc/&lt;pid&gt;/{comm, fd/, mem, ns/, uid_map, gid_map, cgroup, timens_offsets}
- /dev core nodes and their major:minor numbers (null/zero/full/random/urandom/
  kmsg/tty/console/ptmx/tty0-6/pts/fb0/rtc0) — all Linux-correct
- kernel.hostname (R/W), kernel.core_pattern (R/W), net.core.* constants
