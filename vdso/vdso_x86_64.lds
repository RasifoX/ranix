/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
LINUX_2.6 {
global:
	__vdso_clock_gettime;
	__vdso_gettimeofday;
	__vdso_clock_getres;
	__vdso_time;
	__vdso_getcpu;
local: *;
};
