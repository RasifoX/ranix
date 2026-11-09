/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <unistd.h>

int main(void)
{
	if (write(1, "glibc-ok\n", 9) != 9)
		return 1;
	return 0;
}
