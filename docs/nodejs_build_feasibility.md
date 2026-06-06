# Node.js Build Feasibility (cross-compile for Ranix)

**Date:** 2026-06-06
**Goal:** Map exactly what it takes to *cross-build* Node.js (V8 + libuv) for Ranix
(dual-arch x86_64 + aarch64, musl userspace) and where the build will fight us, so the
eventual port (task #43) is de-risked. **Planning, not building. READ-ONLY.**

**Scope:** This doc is about the **build/toolchain**, not the kernel. Kernel readiness
is covered in `docs/nodejs_readiness.md` (one hard kernel blocker — the 2 GB user-VA —
was just widened to 256 GB so V8's cage fits). The two analyses are orthogonal: the
kernel can run Node and the toolchain can build Node are separate gates.

**Method:** Grounded against the existing Ranix cross-build rules in the root `Makefile`
(`BASH_*`, `CURL_*`, `MBEDTLS_*`, `DROPBEAR_*`, and especially the CPython
host-bootstrap `PYTHON_HOST → PYTHON_BIN` pattern at `Makefile:443-490`), the installed
musl cross-toolchains under `~/opt/*-linux-musl-cross`, and the upstream Node `BUILDING.md`
+ Alpine `aports` recipe + Node cross-compile issue tracker.

---

## Bottom line up front

There is **no hard build blocker.** The single thing that would have blocked us — "no
C++ compiler / no libstdc++ in the musl cross-toolchain" — **is not true**: the installed
musl-cross toolchains ship a full **g++ 11.2.1** with **static + shared libstdc++ 6.0.29**
and the C++ headers. GCC 11.2.1 is C++17/C++20-capable, which is enough for an LTS Node.

The catch is **version selection**: Node's `main`/v24 now demands **GCC ≥ 13.2 and
C++20** (`BUILDING.md`), which our **GCC 11.2.1 cannot meet**. The fix is to target an
**older LTS** whose floor is GCC 8.3 / C++17 — **Node 18 LTS** is the safe pick (Node 20
also builds on GCC 10–11 but is closer to the edge). With Node 18 the toolchain is
sufficient. The real cost is **size and time** (multi-GB V8 build, ~1+ hour), plus the
**host-tool bootstrap** (V8 `torque`/`mksnapshot`/`bytecode_builtins_list` must be built
for the *host* and run there, then the rest cross-compiled), exactly analogous to the
CPython `--with-build-python` host-interpreter step we already do.

**Verdict: GO, but it is a multi-day epic, not a 1-agent job.** See the verdict section.

---

## 1. Toolchain reality

**What Ranix uses today.** Every userspace artifact is built with `$(MUSL_CC)` =
`~/opt/<arch>-linux-musl-cross/bin/<arch>-linux-musl-gcc` (`Makefile:4-12`). The active
sysroot is `$(MUSL_SYSROOT)` (`Makefile:9,12`). The kernel itself uses
`clang --target=<arch>-none-elf` (`Makefile:21-23`), but **all userspace** (bash, curl,
dropbear, mbedTLS, CPython, the `test_*` programs) goes through the musl gcc.

**Is anything C++ built today?** **No.** A full scan of the `Makefile` finds zero
`g++`/`CXX`/`.cpp`/`.cc`/`-std=c++`/`libstdc++` references. Node would be the **first
C++ artifact** in the tree — that is the novelty, not a missing capability.

**Does the cross-toolchain have g++/libstdc++?** **Yes, both arches:**

| Item | aarch64 | x86_64 |
|---|---|---|
| C++ driver | `aarch64-linux-musl-g++` (+`c++`) | `x86_64-linux-musl-g++` (+`c++`) |
| GCC version | **11.2.1** (20211120) | **11.2.1** (20211120) |
| libstdc++ static | `…/lib/libstdc++.a` | `…/lib/libstdc++.a` |
| libstdc++ shared | `…/lib/libstdc++.so.6.0.29` | `…/lib/libstdc++.so.6.0.29` |
| C++ headers | `…/include/c++/11.2.1/` (`<iostream>` etc.) | same |

So the cross side is covered. **GCC 11.2.1 implements C++17 fully and most of C++20** —
adequate for Node ≤ 20. It is **below** Node 22+/main's GCC-12/13.2 floor.

**Host toolchain.** The build box has **GCC (host) + clang 18.1.3 + ninja 1.x** already
(`/usr/bin/ninja`, `/usr/bin/clang`). Node bundles **GN under `tools/`** (no system `gn`
needed — none is installed, and that's fine). Host `CC_host`/`CXX_host` should be the
*native* (glibc) host gcc/g++, **not** the musl cross-gcc, because the host tools must run
on this build machine. Node's own `configure`/`tools` need a **supported Python 3** —
already present (we build CPython 3.12 and have host python).

**Toolchain takeaway:** no missing piece. The one constraint is **GCC 11.2.1 caps us at
Node ≤ 20**. Choose the Node version to fit the compiler, not vice-versa.

---

## 2. V8's build system: GN + ninja, and the host-tool bootstrap

Node does **not** use autotools or plain make for the engine. `./configure` is a thin
Python wrapper that, on modern Node, drives **GN → ninja** to build the bundled **V8**
(older Node used GYP → make; either way the principle below holds).

**Why cross-compiling V8 is special — generated code.** V8 *generates C++ at build time*
using tools that must execute **on the build host**, then compiles the generated + engine
sources for the **target**. The host-only tools are:

- **`torque`** — compiles V8's `.tq` files into C++/CSA. (Documented host-only;
  cross-builds historically failed with "Exec format error" when `torque` was wrongly
  built for the target and run on the host — issue nodejs/node#27838.)
- **`bytecode_builtins_list`** — generates the interpreter builtins table.
- **`mksnapshot`** — serializes the V8 heap **snapshot** (the embedded startup blob).
  On cross builds the action is redirected to a **`mksnapshot_host`** built for the build
  CPU (nodejs/node#32582/#32717). If host snapshot generation is problematic, V8 can be
  built **`--without-snapshot`** (slower startup, but unblocks the cross build).
- **`gen-regexp-special-case`** and a few other small codegen helpers — host-built.

This is the **exact analogue of the CPython bootstrap we already do**: `PYTHON_HOST` is
built first natively and passed as `--with-build-python=` so the cross build can run a
same-version interpreter to freeze modules (`Makefile:445-471`). For Node, the build
itself produces the host tools (Node's build files special-case `host_toolchain` vs
`target_toolchain` in GN); we just have to give it a working **`CC_host`/`CXX_host`** so
that side compiles for this machine.

**Cross-compile invocation shape** (env + flags; the canonical pattern from the Node
cross-compile docs/issues):

```
CC="$(MUSL_CC)"                  CXX="$(MUSL_CXX)" \
CC_host="cc"                     CXX_host="c++"    \   # native host compiler
AR="$(MUSL_AR)" RANLIB=…-ranlib  LD=…              \
./configure \
    --cross-compiling \
    --dest-cpu=arm64   \          # arm64 for aarch64, x86_64 for x86_64
    --dest-os=linux    \
    --prefix=/usr      \
    <minimizing flags — see §3/§5>
make -j$(nproc)                   # or: ninja -C out/Release
```

where (added to the Makefile alongside `MUSL_CC`):
`MUSL_CXX = $(MUSL_CC:gcc=g++)` and `MUSL_AR = $(MUSL_CC:gcc=ar)` (the `:gcc=ar` idiom is
already used at `Makefile:245`).

`--dest-cpu` values: **`x86_64`** for x86_64, **`arm64`** for aarch64. `--cross-compiling`
is the load-bearing flag that tells the build to split host vs target toolchains.

**GN/ninja are present** (ninja installed; GN bundled). No new build-system dependency.

---

## 3. musl vs glibc — patches and flags

Node-on-musl is **known-possible**: Alpine ships Node (the `aports` recipe builds
`pkgver=24.16.0` against musl). That recipe is the reference for the musl delta. Note its
patch set is mostly **arch-niche** (`v8-ppc64le-compat`, `v8-riscv-*`, `v8-ppc_vsx`) and
**openssl/icu plumbing** (`ncrypto-include-openssl-rand.h`,
`remove-unused-openssl-config`, `v8-no-static-zlib`, `node_sea`, `node_snapshotable`) —
i.e. **the core x86_64/aarch64 musl build needs essentially no V8 source surgery**; the
patches we care about are few. The classic musl frictions and how to neutralize them:

| glibc assumption | Where it bites | Mitigation |
|---|---|---|
| `execinfo.h` / `backtrace()` | V8/Node crash reporter, `--stack-trace` | Already conditional on musl; build defines `__GLIBC__`-guarded paths off. Usually no patch needed on modern Node; a 1-line guard if it surfaces. |
| `gnu_get_libc_version()` | `process.report` / `os` libc string | musl lacks it; Node already `#ifdef __GLIBC__`-guards. No-op on musl. |
| `__register_atfork` / pthread internals | static linking pthread | musl folds pthread into libc; `-lpthread` is a harmless no-op (Ranix already relies on this, `Makefile:382-385`). |
| `posix_spawn` large-arg / `MAP_STACK` | `child_process` | musl has `posix_spawn`; fine. |
| **default thread stack size** | musl's default pthread stack is **128 KB** vs glibc 8 MB → V8 background/parser threads can overflow | Build/run with explicit larger stacks. Node sets V8 stack limits; the *main-thread* 1 MB Ranix stack is the bigger risk (see `nodejs_readiness.md`). |
| `dlopen` for N-API addons | npm native modules | Works (Ranix userspace ld.so present, `nodejs_readiness.md` §6). **Core Node needs no addons.** |
| ICU (`--with-intl`) | huge | **Use `--without-intl`** (or `--with-intl=small-icu`) — see below. |

**Recommended minimizing/musl flags for the *first* Ranix target** (much leaner than
Alpine's full-featured recipe — we want proof-of-life, not the kitchen sink):

```
--without-intl              # drop ICU entirely (saves the largest single chunk)
--without-npm               # no npm/corepack in the image
--without-corepack
--without-inspector         # no DevTools/debugger protocol
--with-snapshot   (default) # keep embedded snapshot if mksnapshot_host works,
                            #   else --without-snapshot to unblock the cross build
--openssl-no-asm            # avoid arch-specific openssl asm friction (or bundle as-is)
--v8-disable-temporal-support   # (Node ≥ 22 only; smaller)
# Statically link the C++ runtime so we don't have to ship libstdc++:
#   pass -static-libstdc++ -static-libgcc via LDFLAGS / common.gypi additions
```

Deliberately **NOT** using Alpine's `--shared-openssl/zlib/cares/nghttp2/icu/...`: those
require those libs to pre-exist in our sysroot (they don't). **Bundle everything** (the
Node default) for the first build; revisit sharing later if size demands it.

**ICU recommendation:** `--without-intl` for the first target. `Intl`/`String.normalize`
are unavailable, but `node -e`, HTTP, fs, streams all work. Upgrade to `small-icu`
(English data, ~tens of MB) once the base build is proven. Full ICU (~30+ MB data) is not
worth it for an embedded image.

---

## 4. Size / time / RAM, and which Node version

- **Disk:** a V8/Node build tree is **multi-GB** (source ~ hundreds of MB; `out/` with
  objects + the host-tool toolchain easily **4–8 GB** per arch). Our `.cache/<pkg>-$(ARCH)`
  convention (`Makefile:229-244`) would hold `node-host/` + `node-$(ARCH)/` trees — budget
  **~10 GB free** comfortably.
- **RAM:** upstream states **≥ 8 GB RAM for `-j4`**. Linking V8 is the spike. On a small
  box, cap parallelism (`-j2`) or add swap; OOM during the final link is the most common
  failure.
- **Time:** **~30 min to well over an hour** per arch on a typical multicore box (cold),
  dominated by V8. Two arches ≈ double. This dwarfs the CPython build.
- **First target = minimal, single-arch.** Build **x86_64 first** (matches the build
  host CPU, removes cross-toolchain variables from the host-tool side and lets the host
  tools double as target tools for the proof-of-life), `--without-intl --without-npm
  --without-inspector`, static libstdc++. Only then do aarch64 and add ICU.
- **Node version pick:** **Node 18 LTS** (`v18.x`, latest 18 patch). Rationale: floor is
  **GCC 8.3 / C++17** (Node `BUILDING.md` history; nodejs/build#2815), well within our
  **GCC 11.2.1**; mature musl support; smaller/older V8 than v22+. **Node 20** is an
  acceptable stretch (still builds on GCC 10–11) if a newer JS feature set is wanted.
  **Avoid Node 22/24/main** — they require GCC 12/13.2 and C++20 that our 11.2.1 can't
  satisfy without upgrading the cross-toolchain.

---

## 5. Runtime link & what ships in the initramfs

- **Static vs dynamic:** prefer a **mostly-static** binary. Node core can link its bundled
  deps statically; the open question is libc/libstdc++:
  - **libstdc++/libgcc:** link **static** (`-static-libstdc++ -static-libgcc`) so we don't
    ship `libstdc++.so.6`. (Both `.a` files exist in the sysroot.)
  - **libc (musl):** Ranix already supports dynamic musl — `ld-musl-$(ARCH).so.1` is shipped
    to `/lib` (`Makefile:18-19,302-303`) and the in-kernel dynamic loader works
    (`test_dynamic`/`test_dlopen` pass). A **dynamically-libc** Node is fine and is what
    Alpine does. A **fully static** Node is also fine and simplest to ship (matches how
    bash-static/busybox/the `test_*` bins are built, `Makefile:384-385`). **Recommend
    fully static for the first proof** (no `.so` to ship, no loader variance).
- **If dynamic:** ship `/lib/ld-musl-$(ARCH).so.1` (already done) and, only if not
  static-linked, `libstdc++.so.6` + `libgcc_s.so.1` from the sysroot.
- **Ship mechanics:** mirror the CPython block (`Makefile:334-340`): copy the cross-built
  `node` into `initramfs/bin/node`, `chmod +x`, add to the `initramfs.cpio` prerequisite
  list (`Makefile:295`). With `--without-intl` there is **no ICU data file** to ship; with
  `small-icu` later, ship the `icudtl.dat` like we ship the python stdlib zip.
- **N-API addons** need userspace `dlopen` (present) — but **core Node and the `claude`
  CLI are pure JS**, so addons are out of scope for the first target.

---

## 6. Acceptance test — the `c-node` target shape

Define a staged acceptance, smallest first, matching how `test_*` programs are
auto-discovered and run in `tests/ranix_test.sh`:

1. **Smoke (must pass):** `node -e 'console.log(1+1)'` prints `2` and exits 0.
   Proves V8 init (cage reservation in the now-256 GB VA), snapshot load, JIT, libc.
2. **Eval + fs:** `node -e 'console.log(require("fs").readFileSync("/etc/passwd","utf8").length)'`
   — proves libuv fs + the syscall surface.
3. **Timers/event loop:** `node -e 'setTimeout(()=>console.log("tick"),50)'` — proves the
   libuv epoll/timerfd loop turns (10 ms granularity noted in readiness §6).
4. **Tiny HTTP server (the real proof):** a one-liner `http.createServer` on localhost +
   a `busybox wget`/`curl` GET against it. Exercises the libuv epoll backend end-to-end.
   *Caveat (from `nodejs_readiness.md` §3):* the **64-fd `EPOLL_MAX_WATCH`**, missing
   **EINPROGRESS** on non-blocking connect, and missing **EPIPE/MSG_NOSIGNAL** mean a
   *loaded* server misbehaves; a single sequential request should work and is the bar for
   "Node runs on Ranix." Server-scale conformance is a separate kernel task.

**Target shape:** add a `c-node` (or fold into `tests/ranix_test.sh`) check that runs
steps 1–4 inside QEMU after boot, mirroring how python/curl are smoke-tested. The Makefile
gets a `node:` phony (like `python:`/`curl:`) producing `$(NODE_BIN)` =
`$(CACHE_DIR)/node-$(ARCH)/out/Release/node`.

---

## Hard build blockers

**None that stop the port.** Specifically, the feared blocker does **not** apply:

- ~~"No libstdc++/g++ in the cross-toolchain."~~ **FALSE** — g++ 11.2.1 + static & shared
  libstdc++ 6.0.29 + C++ headers are present for **both** arches.

The two real constraints (manageable, not blockers):

1. **Compiler version caps the Node version.** GCC **11.2.1** cannot build Node **22/24/main**
   (they need GCC 12/13.2 + C++20). → **Target Node 18 LTS** (or at most 20). Only becomes
   a hard blocker if someone *insists* on current Node without upgrading the cross-toolchain.
2. **Build cost.** Multi-GB tree, ≥ 8 GB RAM for the V8 link, ~1 h+ per arch, plus the
   host-tool bootstrap (`torque`/`mksnapshot`/`bytecode_builtins_list` host-built; use
   `mksnapshot_host` or `--without-snapshot` for cross). Engineering effort, not a wall.

---

## Recommended exact configure/flags + version

**Version:** **Node.js 18 LTS** (latest `v18.x` patch). Fallback stretch: Node 20.

**First (proof-of-life, x86_64, minimal):**
```
CC="$(MUSL_CC_X86_64)" CXX="$(MUSL_CC_X86_64:gcc=g++)" \
CC_host=cc CXX_host=c++ \
AR="$(MUSL_CC_X86_64:gcc=ar)" \
LDFLAGS="-static -static-libstdc++ -static-libgcc" \
./configure --cross-compiling --dest-cpu=x86_64 --dest-os=linux \
            --without-intl --without-npm --without-corepack \
            --without-inspector --fully-static --enable-static \
            --prefix=/usr
make -j$(nproc)        # binary at out/Release/node
```
(For aarch64: `CC=$(MUSL_CC_AARCH64)`, `CXX=…g++`, `--dest-cpu=arm64`. Keep
`CC_host/CXX_host` as the native host compiler in both cases.)

Drop `--fully-static`/`-static` and ship `ld-musl-$(ARCH).so.1` (+ libstdc++/libgcc if not
static) if a fully-static link proves troublesome. Add `--with-intl=small-icu` only after
the base build passes the smoke test.

---

## Staged build plan

1. **Toolchain spike (½ day):** confirm `<arch>-linux-musl-g++` compiles a trivial C++17
   program static against `libstdc++.a` for both arches. (Toolchain is present; this just
   de-risks linking.)
2. **Host tools (host build):** let Node's build produce `torque`, `bytecode_builtins_list`,
   `mksnapshot_host`, `gen-regexp-special-case` for the build CPU via `CC_host/CXX_host`.
   Analogous to `PYTHON_HOST` (`Makefile:445-457`).
3. **Cross V8 + Node (x86_64, minimal):** the configure invocation above; `--without-intl
   --without-npm --without-inspector`, static libstdc++. If snapshot generation fights,
   `--without-snapshot`. This is the long pole (V8).
4. **Ship:** copy `node` → `initramfs/bin/node`, add to the cpio prereq
   (`Makefile:295,334-340`); fully-static needs no extra `.so`.
5. **`c-node` acceptance:** smoke (`node -e 'console.log(1+1)'`) → fs → timers → tiny HTTP
   server, run in QEMU like the python/curl smoke tests.
6. **Second arch + ICU:** repeat for aarch64; add `--with-intl=small-icu`; both arches green.

Each step gated on the prior; steps 2–3 are where days are spent.

---

## Go / Risk verdict

**GO — but a multi-day epic, not a 1-agent job.**

The toolchain is genuinely ready (g++ 11.2.1 + libstdc++ on both arches), the kernel VA
blocker is fixed, the host-tool bootstrap is a known, already-practiced pattern (CPython),
and Node-on-musl is proven (Alpine). Nothing is structurally missing. **But** the V8 build
is the heaviest single thing this tree would ever compile — multi-GB, 8 GB-RAM link,
~1 h+ per arch, host/target toolchain split, snapshot cross-build quirks, and a forced
**Node-18 version pin** because GCC 11.2.1 can't do current Node's C++20/GCC-13.2 floor.
Expect **iteration on the V8 cross-build** (snapshot, static-libstdc++ link, musl guards),
which is precisely the kind of long-tail that turns "should work" into several focused
days. Single-arch minimal proof-of-life first; dual-arch + ICU is a follow-on.

**Risk hot-spots:** (a) V8 `mksnapshot` host/target split — keep `--without-snapshot` in
the back pocket; (b) final-link OOM — cap `-j`, add swap; (c) static-libstdc++ link errors
— fall back to dynamic + ship `libstdc++.so.6`; (d) resisting the urge to chase current
Node — pin 18 unless the cross-toolchain is upgraded to GCC 13+.
