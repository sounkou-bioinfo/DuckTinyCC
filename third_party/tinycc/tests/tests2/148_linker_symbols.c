#include <stdio.h>

int check_linker_symbols(void);

int main(void)
{
    int errors = check_linker_symbols();

    printf("linker symbols errors: %d\n", errors);
    return errors;
}
