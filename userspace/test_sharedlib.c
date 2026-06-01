/* Load-time linked against libfoo.so: ld.so must map the extra DSO and bind
 * the call to foo() before main runs. */
#include <stdio.h>
extern int foo(int);

int main(void)
{
	printf("%s\n", foo(20) == 41 ? "sharedlib-ok" : "sharedlib-fail");
	return 0;
}
