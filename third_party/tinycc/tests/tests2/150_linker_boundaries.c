#include <stdio.h>

extern void linker_test_text(void);
extern unsigned char linker_test_data[23];
extern unsigned char linker_test_bss[257];
extern char etext[], _etext[];
extern char edata[], _edata[];
extern char end[], _end[];

int main(void)
{
    unsigned long text_addr = (unsigned long)linker_test_text;
    unsigned long data_end = (unsigned long)(linker_test_data
                                              + sizeof linker_test_data);
    unsigned long bss_end = (unsigned long)(linker_test_bss
                                             + sizeof linker_test_bss);
    int errors = 0;

    linker_test_data[22] = 42;
    linker_test_bss[256] = 42;
    errors += etext != _etext;
    errors += edata != _edata;
    errors += end != _end;
    errors += (unsigned long)etext <= text_addr;
    errors += (unsigned long)edata < data_end;
    errors += (unsigned long)end < bss_end;
    errors += linker_test_data[22] != 42;
    errors += linker_test_bss[256] != 42;
    printf("linker boundary errors: %d\n", errors);
    return errors;
}
