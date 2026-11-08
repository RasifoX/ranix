# Ranix DRM (stage 0)

`/dev/dri/card0` (226:0) and `/dev/dri/renderD128` (226:128) with the Linux DRM uAPI,
so libdrm, modetest, Mesa's GBM (kms_swrast) and KMS compositors can talk to Ranix.
Clean-room: the uAPI structs and numbers match Linux `include/uapi/drm` (they are the
ABI); the code is Ranix's own.

| file | what |
|---|---|
| `drm_uapi.h` | ioctl numbers, structs, constants (checked against Linux headers with static asserts) |
| `drm_core.c` | device nodes, dispatcher, per-open state, GEM/dumb, mmap, PRIME (dma-buf fds), syncobj, sync_file |
| `drm_kms.c`  | one CRTC/encoder/connector, primary + cursor plane, scanout (copy or flip), vblank |
| `drm_sysfs.c`| `/sys/dev/char/226:*`, `/sys/class/drm/*`, the PCI device directory libdrm reads |

Hooks outside this directory: `drm_init()` after `fbdev_init()` in `kernel/main.c`,
`/sys/dev` and `/sys/class/drm` in `kernel/fs/sysfs.c`, the sources in `Makefile`.

## ioctls
VERSION, GET_UNIQUE, SET_VERSION, GET_MAGIC, AUTH_MAGIC, SET/DROP_MASTER, GET_CAP,
SET_CLIENT_CAP (UNIVERSAL_PLANES, STEREO_3D, ASPECT_RATIO; ATOMIC is refused so clients
use legacy KMS), GEM_CLOSE, GEM_FLINK, GEM_OPEN, PRIME_HANDLE_TO_FD, PRIME_FD_TO_HANDLE,
WAIT_VBLANK (blocking and event), CRTC_GET_SEQUENCE, CRTC_QUEUE_SEQUENCE,
MODE_GETRESOURCES, GETCRTC, SETCRTC, CURSOR, CURSOR2, GET/SETGAMMA (size 0), GETENCODER,
GETCONNECTOR, GETPROPERTY, SETPROPERTY, OBJ_GETPROPERTIES, OBJ_SETPROPERTY, GETPROPBLOB,
CREATE/DESTROYPROPBLOB, GETFB, GETFB2, ADDFB, ADDFB2 (LINEAR modifier), RMFB, CLOSEFB,
PAGE_FLIP (+EVENT), DIRTYFB, CREATE_DUMB, MAP_DUMB, DESTROY_DUMB, GETPLANERESOURCES,
GETPLANE, SETPLANE, SYNCOBJ_CREATE/DESTROY/HANDLE_TO_FD/FD_TO_HANDLE/WAIT/RESET/SIGNAL,
SYNCOBJ_TIMELINE_WAIT/QUERY/TRANSFER/TIMELINE_SIGNAL. MODE_ATOMIC answers EOPNOTSUPP.
dma-buf fds: mmap, lseek(SEEK_END) = size, DMA_BUF_IOCTL_SYNC, EXPORT/IMPORT_SYNC_FILE.
Properties: connector DPMS, non-desktop; plane type. Formats XRGB/ARGB/XBGR/ABGR8888, RGB565.

## Display
No modeset: the CRTC keeps the mode the firmware (GOP) set; SETCRTC accepts only that size.

`drm_kms.c` is generic: it knows no hardware. A display back end (`drm_display_t` in
`drm_priv.h`; today only `i915/intel_display.c`) supplies the vblank counter, the
direct flip and the per-tick hook; without one KMS copies and keeps a 60 Hz clock.

- **copy** (every machine with a firmware framebuffer): the client buffer is copied into
  the firmware framebuffer through a write-combining mapping. A page flip waits until
  the beam enters vertical blank (PIPEDSL >= vdisplay) and copies top-down; the copy
  (~1.5 ms for 1366x768) outruns the beam, so the frame does not tear. The flip event
  carries the sequence and time of that vblank. The cursor is drawn in software.
  Elsewhere (no Intel registers) flips complete on a 60 Hz software clock.
- **flip** (Intel Gen9, on by default; `ranix.drmcopy` on the command line or
  `echo 0 > /sys/class/drm/ranix_flip` turns it off): the buffer's pages are written into a GGTT window at the top of the GGTT
  (PTE = phys | present, 256 KiB-aligned, then `GFX_FLSH_CNTL`), CPU caches are flushed
  (clflush), and `PLANE_STRIDE`/`PLANE_SURF` of the plane the firmware enabled are written;
  the hardware latches at the next vblank and the flip completes when `PLANE_SURFLIVE`
  reads the new address (checked from the 1 ms tick). A visible cursor, another size,
  format or a stride the plane cannot take falls back to copy. When the client goes away
  the last image is copied to the firmware buffer and the plane is pointed back at it.
  Correctness was checked with the pipe CRC (`PIPE_CRC_CTL`/`PIPE_CRC_RES_1`): the same
  image flipped and copied gives the same CRC.
- Vblank counter: `PIPE_FRMCOUNT`; timestamps from CLOCK_MONOTONIC, corrected by the
  scanline. Registers per the Kaby Lake PRM Vol 2c.

`/sys/class/drm/ranix_flip` reads `0|1` and a line of state (plane registers, pending
flip, GGTT PTEs, pipe CRC). Writing `t` (root, no client on screen) runs a self-test that
shows the firmware image through the GGTT window from the firmware's pages and from a RAM
copy, then black three ways, and logs the pipe CRCs (they must match); it leaves the
firmware buffer black.

While a client's buffer is on screen the kernel text console is switched to graphics mode
(as KDSETMODE does) and restored when the CRTC goes off.

## Measured on Casper (i5-7200U, HD 620, 1366x768 eDP, 2026-11-08)
- copy: 300 flips at 16.67 ms (60.0 fps), worst 17.3 ms; PAGE_FLIP blocks until the vblank
  it lands in (~16.6 ms, by design). The full-frame copy itself is ~1.5 ms (WC mapping;
  through the kernel's UC console mapping it was ~50 ms, 18 fps).
- flip: 300 flips at 16.67 ms (60.0 fps), worst 17.3 ms; PAGE_FLIP returns in 0.5 ms
  (CLFLUSHOPT of 4 MiB; with CLFLUSH it was 6 ms).
- modetest -v: 60.0 Hz in both modes. Pipe CRC equal for the same image copied and
  flipped (flat colours, per-page stripes, console text through the GGTT window).
- QEMU (no Intel, software vblank): the suite test passes, flips paced at 60 Hz.

## Tests
`userspace/test_drm.c` (in the suite as `c-drm`; prints OK when there is no card):
caps, resources, planes and properties, dumb buffers, SETCRTC, 30 page flips with events
(pacing checked), WAIT_VBLANK, cursor, DIRTYFB, PRIME export/mmap/import (card and
render node), flink, syncobj and sync_file, RMFB. `test_drm -v N` animates for N
seconds and prints the frame rate; `test_drm -h N` holds a still image.

## Stage 1: i915 on the render engine (`i915/`, in progress)
Linux 6.1.176 i915 parts (MIT, listed in `i915/NOTICE`) over the LinuxKPI shim in
`../linuxkpi`, plus Ranix's own engine/GEM code.

- `ranix_gt.c`: forcewake, PPAT, HWSP, execlists on RCS. Requests are written into
  per-context rings with i915's gen8 emitters, queued, and sent to ELSP one at a time
  on an idle engine (CSB ACTIVE_TO_IDLE). Contexts start restore-inhibited, are freed
  only after the engine parks on a kernel context. A request older than 2 s wedges
  the GT: engine reset (RESET_CTL + GDRST), everything completes with -EIO, the GT
  stays off for the boot; the display never depends on it.
- `ranix_vm.c`: one 48-bit 4-level PPGTT per open file.
- `ranix_gem.c`: i915 uAPI — GETPARAM, QUERY (engines, memory regions, topology),
  GEM_CREATE(_EXT), PREAD/PWRITE, MMAP_OFFSET, SET_DOMAIN, BUSY, WAIT, SET/GET_CACHING,
  SET/GET_TILING (linear only), MADVISE, GET_APERTURE, CONTEXT_CREATE_EXT (engine map),
  CONTEXT_DESTROY/GETPARAM/SETPARAM, REG_READ (RCS timestamp), GET_RESET_STATS,
  EXECBUFFER2 (softpin, relocations, HANDLE_LUT, BATCH_FIRST, fence array, in/out
  fences — fences it hands out are signaled because it waits for the request first).
  Missing: other engines, VM_CREATE, userptr, real async fences, golden context /
  render state and workaround lists, RC6, tiling.
- Tests: `userspace/test_i915.c` (suite `c-i915`). On Casper: IGT 1.27.1
  gem_exec_basic passes (rcs0-smem), gem_basic 3/3.
- `/sys/class/drm/ranix_gt` (root, 0600): read for engine state and the RPS line;
  write `audit` (read-only check of every GGTT entry), `rps on`, `rps off`,
  `rps max <MHz>`. `/sys/class/drm/ranix_power` (root, 0400): power, temperature,
  GT frequency (ranix_power.c).
- GPU address safety: unused GGTT/PPGTT entries point at a zeroed scratch page,
  and every mapped page must be RAM the page allocator manages.
