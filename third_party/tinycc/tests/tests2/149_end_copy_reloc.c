#include <stdio.h>

extern unsigned char copy_reloc_data[257];
extern char end[], _end[];

int main(void)
{
    unsigned long data_end;
    int errors = 0;

    copy_reloc_data[256] = 42;
    data_end = (unsigned long)(copy_reloc_data + sizeof copy_reloc_data);
    errors += end != _end;
    errors += (unsigned long)end < data_end;
    errors += copy_reloc_data[256] != 42;
    printf("end copy relocation errors: %d\n", errors);
    return errors;
}
