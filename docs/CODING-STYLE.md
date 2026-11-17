# Ranix coding style

Goal: anyone can read the code and understand it. The style comes from the
first Ranix code; the rules below are checked where a machine can check them
(`tools/gate.sh`), and the full test suite passes 100% before a merge.

## Language
- **Everything in the source tree is English:** identifiers, file and
  directory names, comments, log lines, error text, commit messages, test
  names and script options. A contributor anywhere must be able to read it.
- The only exception is data a test needs on purpose (for example the
  Turkish file names in `tests/ntfs/`, which exercise Unicode handling).
- User-facing text that should be localized belongs in Fjord (the OS), not
  in the kernel. Text the kernel itself shows (panic or shutdown screens) is
  English.
- `tools/gate.sh` fails on Turkish-specific letters (c and s with cedilla,
  g with breve, dotless i, o and u with umlaut) outside the allowed test data.

## Kept style
- **Names are short and plain.** `kout`, `pb_u64`, `vm_space_t`. If a name
  needs an abbreviation, use the same one throughout the file.
- **A comment says why.** What the code does is read from the code; the
  comment gives the reason. Every file starts with a comment that says in
  plain words what it does.
- **Indentation:** follow the file's existing indentation (tabs in most
  files). A function's `{` goes on its own line.

## Rules
1. **Search before you write.** Look in `kernel/lib` and `include/` before
   adding a helper.
2. **No `extern` in a `.c` file.** A declaration goes in the header its
   definition belongs to, and the defining file includes that header, so the
   compiler warns when a signature changes. Time: `kernel/time.h`; boot entry
   points: `kernel/init.h`; error codes: `kernel/errno.h`; SMP: `kernel/smp.h`.
3. **Constants have names.** Return `-EINVAL`, never `-22`. Do not redefine
   `HZ`, `RLIMIT_*`, `SIG*` or errno values with a local `#define`.
4. **A function is at most 120 lines and nests at most 5 levels.** Do not add
   a branch to a function over 120 lines: split it first, without changing
   behavior, in its own commit.
5. **A file does one job.** A file past ~1500 lines is split. No policy in
   drivers: shortcuts, sound and keyboard layouts are Fjord's job.
6. **Error paths use `goto out`.** No cleanup macros. Return an error rather
   than panic.
7. **No `#if __x86_64__` in generic code.** Add an `arch_*()` function.
8. **Every gate starts closed.** A device node is 0600 root or 0660 group,
   never world-writable. A pid, uid or path from the user goes through a
   permission check. Every new gate has a negative test.
9. **No test hooks in production.** A new `ranix.*` command-line key needs
   approval.
10. **The kernel does not say "agent" or "AI".** Use neutral names such as
    session, lease, holder, injected.
11. **A comment is not an incident log.** "Seen on machine X on date Y"
    goes in the commit message. If a comment and the code disagree, fix one.
12. **Every behavior change has a test:** `userspace/test_<name>.c`, run by
    a script under `tests/` (see `CONTRIBUTING.md`).
