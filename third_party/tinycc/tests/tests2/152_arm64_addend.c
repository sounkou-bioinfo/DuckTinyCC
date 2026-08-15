#include <stdint.h>
#include <stdio.h>

static unsigned short data[844];

static uint64_t address_for_index(int index)
{
    return (uint64_t)(uintptr_t)data - 845186ULL
           + (uint64_t)index * 2;
}

int main(void)
{
    printf("%d\n", address_for_index(422593)
                    == (uint64_t)(uintptr_t)data);
    return 0;
}
