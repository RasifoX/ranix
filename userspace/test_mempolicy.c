/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
/* A one-node machine still answers the NUMA calls libnuma makes at load. */
#include <errno.h>
#include <stdio.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(void)
{
	int mode = -1;
	unsigned long mask[2] = { 0, 0 };
	if (syscall(SYS_get_mempolicy, &mode, mask, 128UL, 0UL, 0UL) != 0 || mode != 0) { printf("FAIL default\n"); return 1; }
	if (syscall(SYS_get_mempolicy, NULL, mask, 128UL, 0UL, 4UL) != 0 || mask[0] != 1) { printf("FAIL mems allowed %lx\n", mask[0]); return 1; }
	if (syscall(SYS_get_mempolicy, NULL, NULL, 0UL, 0UL, 64UL) != -1 || errno != EINVAL) { printf("FAIL flags\n"); return 1; }
	if (syscall(SYS_set_mempolicy, 0, NULL, 0UL) != 0) { printf("FAIL set\n"); return 1; }
	if (syscall(SYS_set_mempolicy, 99, NULL, 0UL) != -1 || errno != EINVAL) { printf("FAIL set bad\n"); return 1; }
	printf("OK\n");
	return 0;
}
