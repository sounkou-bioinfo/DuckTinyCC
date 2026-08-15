#include <stdio.h>

int etext(void) { return 1; }
int edata(void) { return 2; }
int end(void) { return 3; }

int main(void)
{
    int errors = etext() + edata() + end() != 6;

    printf("user definitions errors: %d\n", errors);
    return errors;
}
