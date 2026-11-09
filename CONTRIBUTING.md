# Contributing to Ranix

Ranix is licensed under the EUPL v1.2 (`LICENSE`, summary in
`LICENSING.md`). By contributing you agree that your contribution is
licensed under the same licence ("inbound = outbound"). There is no
contributor licence agreement; the Developer Certificate of Origin below
takes its place.

The licensor of Ranix is RasifoX (a personal project, developed under the
Metastaz umbrella). You keep the copyright in your own changes; the
project's notice reads "Copyright (c) 2026 RasifoX" and contributors are
recorded in the Git history through their sign-off.

## Ground rules
- Original code only. Do not copy, paste, translate or "port" code from
  GPL or other copyleft projects. Reading other systems' code to learn
  how hardware behaves is allowed; write the Ranix code yourself and name
  what you read in the commit message. Public specifications and
  datasheets are fine; cite them too.
- Code under a permissive licence (MIT, BSD, ISC, Apache-2.0) may be
  imported only unchanged in its own file or directory, with its
  copyright and licence header kept, and an entry added to `NOTICE`.
- New Ranix files start with `/* SPDX-License-Identifier: EUPL-1.2 */`
  (`# SPDX-License-Identifier: EUPL-1.2` in scripts and Makefiles).
  Never change or remove a third-party file's licence header.
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
- Every commit ends with `Signed-off-by:` (see below).

## Developer Certificate of Origin

Ranix uses the Developer Certificate of Origin 1.1
(<https://developercertificate.org/>). Adding

    Signed-off-by: Your Name <you@example.org>

to a commit (`git commit -s`) certifies that you wrote the change or have
the right to submit it under the project's licence, as the DCO text says.
Use your real name. A commit without a sign-off is not merged.

## Changes written with AI agents

AI agents may write code for Ranix, but an agent cannot sign the DCO and
cannot be responsible for a change. So:

- Credit the agent with a trailer naming the model, for example
  `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- A named human adds `Signed-off-by:` and is responsible for the change:
  that it may be contributed under the EUPL, that it is not copied from
  incompatible code, and that it was reviewed and tested as below.
- Say in the commit message what the agent was given to read (datasheets,
  other drivers), the same as for a human author.

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
