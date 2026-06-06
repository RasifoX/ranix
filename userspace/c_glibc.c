#include <unistd.h>

int main(void)
{
	if (write(1, "glibc-ok\n", 9) != 9)
		return 1;
	return 0;
}
