# Security policy (DRAFT)

Status: draft. Ranix is experimental and has had no external audit.
Do not use it where a compromise would hurt.

## Reporting
Report suspected vulnerabilities privately to the maintainers. A contact
address will be published here before the first public release; until
then do not open a public issue for a security bug.

Please include: the kernel commit, the architecture, a minimal reproducer
(a `userspace/test_*.c` style program is ideal) and what you observed.

## What we do
- Acknowledge within 7 days, give a first assessment within 14.
- Fix in private, add a regression test, then publish the fix with a
  short advisory crediting the reporter (unless they prefer not).
- Default disclosure deadline: 90 days from the report, earlier if the
  bug is being exploited.

## In scope
Kernel memory safety, privilege escalation, escapes from namespaces,
chroot or agent sessions (asess), permission bypass in the VFS, IPC and
sockets, and kernel information leaks to unprivileged users.

## Out of scope
Denial of service by root, bugs that need physical access to unlock
hardware debug features, and third-party userspace programs.
