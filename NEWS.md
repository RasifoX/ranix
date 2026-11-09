# Ranix release notes

## 0.9.0 (release candidate, 2026-11-09)

First release under the EUPL v1.2. Ranix is experimental: do not run it on a
machine whose data you cannot afford to lose. x86_64 only; the aarch64 port
is in the tree but does not work yet (the Raspberry Pi 4 kernel does not link).

### Licence

- `LICENSE` (official EUPL-1.2 text), `NOTICE` (third-party parts: ACPICA,
  i915 MIT parts, Spleen, LinuxKPI, FreeBSD rtwn and genet), `LICENSING.md`.
  Every Ranix source file carries an SPDX header. Contributions need a DCO
  sign-off (`CONTRIBUTING.md`).
- Code that was derived from GPL sources is gone: the Wi-Fi (RTL8723BE) and
  Raspberry Pi 4 Ethernet (GENET) drivers were rewritten clean-room from
  FreeBSD, and an unused Linux font was removed.

### Fixes and new behaviour since the last internal snapshot

- Security: `connect`/`sendto` read the socket address by its length;
  ptrace's resume signal number is bounded (a large value wrote past a kernel
  table); seccomp validates filter programs like Linux and `KILL_THREAD` ends
  one thread; `set*id` calls require `CAP_SETUID`/`CAP_SETGID`, with
  user-namespace limits; permission checks use fsuid/fsgid and the `CAP_DAC_*`
  capabilities; `rt_sigqueueinfo` cannot spoof `si_code`; TCP initial sequence
  numbers are random.
- Correctness: a regular file used as a parent directory returns `ENOTDIR`
  (it used to crash the kernel); path walks report `ENOTDIR`, `ELOOP` and
  `ENAMETOOLONG`; futexes on shared mappings work across processes; POSIX
  record locks split and merge; `writev` to a pipe is atomic up to `PIPE_BUF`;
  `recvmsg` fills every iovec and reports `MSG_TRUNC`; `MAP_SHARED` writes
  reach files on small-block ext2; a lost wakeup in POSIX timers is fixed;
  `/tmp` is mode 01777; `waitid`, `faccessat2`, `RLIMIT_MEMLOCK`, epoll error
  codes, `/proc/<pid>/maps` and `status` now match Linux.
- Most of the very long functions (syscall dispatch, signal delivery, mmap,
  boot, networking, scheduler, page faults and others) were split into helpers
  without behaviour change.
- Tests: the QEMU/KVM suite has 605 checks; all pass. One check (`apt-live`) is
  skipped because it depends on an outside Debian mirror.

### Known limitations

- **Raspberry Pi 4 Ethernet (`bcmgenet`) is not verified on hardware.** It
  compiles; QEMU has no model of the chip and no Pi 4 boot was done.
- **Wi-Fi data path not measured.** The new RTL8723BE driver associates (WPA2),
  gets a DHCP address and keeps running on the test laptop, but the ping tests
  went out over Ethernet because the cable was plugged in: Ranix has no
  routing table. Egress is the first network device with an address, and
  `ip link set ... down`, `ip route del` and `SO_BINDTODEVICE` are not
  supported. A routing table is planned for the next release.
- GPU: running 3D work on the live desktop has powered the test laptop off
  more than once; the cause is still being investigated. dma-buf sharing in
  the Fjord compositor stays off by default.
- No hardware watchdog on the test laptop; a hang only resets through
  `ranix.deadline`.
