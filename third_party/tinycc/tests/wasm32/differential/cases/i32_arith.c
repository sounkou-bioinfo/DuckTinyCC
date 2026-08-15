#include <stdint.h>

int diff_i32_add(int a, int b)
{
    return a + b;
}

int diff_i32_mix(int a, int b, int c)
{
    int x = a + b;
    int y = x * c;
    return y - a;
}

int diff_i32_cmp(int a, int b)
{
    return (a < b) + 2 * (a == b) + 4 * (a > b);
}
