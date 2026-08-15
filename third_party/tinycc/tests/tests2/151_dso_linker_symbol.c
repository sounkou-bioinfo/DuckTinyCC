#include <stdio.h>

extern char _end[];
extern char *get_dso_end(void);

int main(void)
{
    int errors = get_dso_end() != _end;

    printf("DSO linker symbol errors: %d\n", errors);
    return errors;
}
