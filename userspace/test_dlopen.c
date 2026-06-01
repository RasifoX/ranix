/* Runtime shared-library load: dlopen + dlsym must map libfoo.so on demand
 * and resolve foo(). */
#define _GNU_SOURCE
#include <stdio.h>
#include <dlfcn.h>

int main(void)
{
	void *h = dlopen("/lib/libfoo.so", RTLD_NOW);
	if (!h) { printf("dlopen-fail open: %s\n", dlerror()); return 0; }
	int (*fp)(int) = (int (*)(int))dlsym(h, "foo");
	if (!fp) { printf("dlopen-fail sym\n"); dlclose(h); return 0; }
	int r = fp(20);
	dlclose(h);
	printf("%s\n", r == 41 ? "dlopen-ok" : "dlopen-fail");
	return 0;
}
