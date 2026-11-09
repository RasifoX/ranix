#include <kernel/extable.h>


unsigned long search_extable(unsigned long pc)
{
	for (struct ex_entry *e = __start___ex_table; e < __stop___ex_table; e++)
		if (e->insn == pc)
			return e->fixup;
	return 0;
}
