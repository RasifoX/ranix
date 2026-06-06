/* setuid-root helper for the no_new_privs test: prints the euid it runs with.
 * Made suid-root in the Makefile (chmod u+s). With NNP set the exec must NOT
 * grant uid 0, so a caller running as uid 1000 should see euid=1000 here. */
#include <unistd.h>
#include <stdio.h>

int main(void)
{
    printf("euid=%d\n", (int)geteuid());
    return 0;
}
