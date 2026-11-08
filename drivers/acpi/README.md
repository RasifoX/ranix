# drivers/acpi

`acpica/` is Intel's ACPICA interpreter core, imported unchanged (see
`acpica/NOTICE`). It is **not built yet**: the Makefile does not list this
directory. Next steps, each separately approved: a Ranix OS layer (acranix.h +
osl), a kernel build behind a boot flag, then thermal-zone reading. Executing
`_INI` (which writes EC RAM on Casper) waits for Rasi's approval.

Host check (Pi, userspace): the same release builds as `acpiexec`
(`generate/unix`, `NOWERROR=TRUE` for GCC 13) and loads Casper's DSDT: 5020
objects, 216 devices, 127 regions, 1469 methods, 31 `_INI` run in simulation;
one unresolved name, `\_PR.EPCS`, defined in an SSDT.
