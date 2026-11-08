# Ranix LinuxKPI

Linux kernel interfaces over Ranix primitives, so MIT-licensed Linux GPU code
(i915 gt/ and gem/) builds with few changes. Written fresh for Ranix
(BSD-2-Clause), in the spirit of FreeBSD's linuxkpi; no Linux code.

Only what the ported files actually use is here, added as they need it:
linux/types.h, bits.h, bitops.h, bitfield.h, build_bug.h.
Builds see these headers only for drivers/gpu/drm/i915 (Makefile pattern rule).
