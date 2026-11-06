# Contributing to Ranix (DRAFT)

Status: draft, not yet adopted. The license is not decided (see README);
until it is, contributions cannot be accepted from outside the project.

## Ground rules
- Original code only. Do not copy, paste, translate or "port" code from
  GPL or other copyleft projects, and do not read such sources while
  writing a Ranix patch. Public specifications and datasheets are fine;
  cite them in the commit message.
- Fix the root cause. A patch that hides a symptom (a retry, a longer
  timeout, a skipped test) needs a written reason in the commit message.
- Every behavior change comes with a test: `userspace/test_<name>.c`
  plus a `check "c-<name>" ...` line in `tests/ranix_test.sh`. The test
  must fail on the kernel without the fix; say how you checked that.
- The full suite must not regress. Run `make ARCH=x86_64 test` and quote
  the `Results:` line in the pull request.
- System text (log lines, errors, UI strings, comments) is in English.
- Keep a change to one subject. Refactors go in their own commit.

## Kernel review checklist
- User memory only through copy_from_user/copy_to_user (or the fop
  helpers); lengths bounded before the copy; no double fetch.
- Lock order documented where two locks nest; no sleep with a spinlock
  held; every error path unlocks and drops what it took.
- A file used across a sleep is pinned with fget()/fput(), not fd_get().
- Size and offset arithmetic checked for overflow (__builtin_*_overflow).
- Permission checks match Linux semantics unless the difference is
  documented and tested.

## Commits
- Subject: `area: what changed` (e.g. `ipc: wake every semaphore sleeper`).
- Body: the bug, the root cause, the fix, the test, and the suite result.
- If an AI agent wrote any part of the patch, say so in a trailer:
  `Agent: <tool/model>`; the human who approved it signs off.

## Rules for internal AI contributor agents
1. Work only in your own working directory or branch. Never write to
   `main`, never push, never tag a release. A human merges.
2. Every patch has a test as above, and the full suite result.
3. Every patch is reviewed by a different agent (or a human) that did not
   write it, starting from the diff, not from the author's summary.
   Findings are fixed or answered before a human is asked to merge.
4. No claim without evidence: a report quotes the command and the output
   line that shows it. "Not run" is written as such.
5. Nothing leaves the machine without a human's explicit approval: no
   e-mail, issue, post, upload or package publish. No spending money.
6. Secrets (keys, tokens, passwords) are never printed, logged or
   written to disk; pass them on stdin.
7. Real hardware, reboots and boot loader changes need the exclusive
   lock and a backup first; shared builds and QEMU use the shared lock.
8. Bounded loops only: every wait has a timeout. Do not match processes
   by command line (`pgrep -f`, `pkill -f`); use PIDs you started.
9. If a change cannot be finished safely, stop at a clean point, revert
   the half-done part and report what is left open.
