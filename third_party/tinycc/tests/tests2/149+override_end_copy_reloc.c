#include <stdio.h>

extern unsigned char copy_reloc_data[257];

int end(void) { return 42; }
int _end(void) { return 24; }

int main(void)
{
    int errors;

    copy_reloc_data[256] = 42;
    errors = end() != 42;
    errors += _end() != 24;
    errors += copy_reloc_data[256] != 42;
    printf("user end copy relocation errors: %d\n", errors);
    return errors;
}
