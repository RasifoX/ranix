/*
 * SPDX-FileCopyrightText: 2026 RasifoX
 * SPDX-License-Identifier: EUPL-1.2
 */
#include <kernel/extable.h>


unsigned long search_extable(unsigned long pc)
{
	for (struct ex_entry *e = __start___ex_table; e < __stop___ex_table; e++)
		if (e->insn == pc)
			return e->fixup;
	return 0;
}
