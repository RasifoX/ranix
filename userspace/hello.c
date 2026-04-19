#include <unistd.h>

int main(void)
{
    write(1, "hello from musl\n", 16);
    return 0;
}
